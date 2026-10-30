/**
 * @file mqtt_bridge.cpp
 * @brief MqttBridge implementation on libmosquitto (and a stub when built without it).
 */
#include "mqtt_bridge.hpp"

#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <sstream>

#include "../common/log.hpp"
#include "messages.hpp"

#ifdef SD_HAVE_MQTT
#include <mosquitto.h>
#endif

namespace sd::pipeline {

MqttOptions parse_mqtt_url(const std::string& url) {
    MqttOptions o;
    std::string s = url;
    if (s.rfind("mqtt://", 0) == 0) s = s.substr(7);
    if (s.rfind("tcp://", 0) == 0) s = s.substr(6);
    if (!s.empty() && s.back() == '/') s.pop_back();
    const auto colon = s.rfind(':');
    if (colon != std::string::npos) {
        o.host = s.substr(0, colon);
        o.port = std::atoi(s.substr(colon + 1).c_str());
    } else if (!s.empty()) {
        o.host = s;
    }
    if (o.host.empty()) o.host = "localhost";
    if (o.port <= 0) o.port = 1883;
    return o;
}

std::string slugify(const std::string& label) {
    std::string out;
    for (unsigned char c : label) {
        if (std::isalnum(c)) out += static_cast<char>(std::tolower(c));
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "sensor" : out;
}

namespace {

/// @brief Escapes quotes and backslashes for a JSON string (control characters dropped).
[[maybe_unused]] std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        if (static_cast<unsigned char>(c) < 0x20) continue;
        o += c;
    }
    return o;
}

/// @brief Formats a number with 3 decimals for JSON payloads.
[[maybe_unused]] std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    return buf;
}

/// @brief Host name of this machine (used in the default topic prefix).
[[maybe_unused]] std::string hostname() {
    char buf[256] = {};
    if (::gethostname(buf, sizeof(buf) - 1) != 0 || !buf[0]) return "localhost";
    return buf;
}

}  // namespace

#ifdef SD_HAVE_MQTT

/// @brief libmosquitto client and connection state.
struct MqttBridge::Impl {
    mosquitto* m = nullptr;               ///< libmosquitto client handle.
    std::atomic<bool> connected{false};   ///< Set by the connect / disconnect callbacks.
    std::atomic<bool> need_meta{true};    ///< Re-publish sensor metadata (after (re)connect or sensor changes).
    /// @brief Per-sensor topic data.
    struct Meta {
        ///@{
        std::string data_topic, unit;  ///< Topic for the sensor's samples, and its unit.
        ///@}
    };
    std::map<int64_t, Meta> sensors;  ///< bridge thread only
    std::string status_topic;         ///< `<prefix>/status` (online / offline, last will).
};

bool MqttBridge::available() { return true; }

MqttBridge::MqttBridge(bus::Bus& bus, db::Database& db, MqttOptions opts)
    : bus_(bus), db_(db), opts_(std::move(opts)), impl_(std::make_unique<Impl>()) {
    if (opts_.prefix.empty()) opts_.prefix = "sensordash/" + slugify(hostname());
    if (opts_.client_id.empty()) opts_.client_id = "sensordash-" + std::to_string(::getpid());
    impl_->status_topic = opts_.prefix + "/status";
    sub_ = bus_.subscribe("mqtt-bridge",
                          {msg::topic::kSamplesAll, msg::topic::kAlarmStateAll, msg::topic::kSensorsChanged}, 1 << 14);
}

MqttBridge::~MqttBridge() { stop(); }

bool MqttBridge::start() {
    mosquitto_lib_init();
    impl_->m = mosquitto_new(opts_.client_id.c_str(), true, impl_.get());
    if (!impl_->m) {
        log::error("mqtt: cannot create client");
        return false;
    }
    auto* m = impl_->m;
    if (!opts_.username.empty())
        mosquitto_username_pw_set(m, opts_.username.c_str(), opts_.password.empty() ? nullptr : opts_.password.c_str());
    const std::string offline = "offline";
    mosquitto_will_set(m, impl_->status_topic.c_str(), static_cast<int>(offline.size()), offline.data(), 1, true);
    mosquitto_reconnect_delay_set(m, 1, 30, true);
    mosquitto_connect_callback_set(m, [](mosquitto*, void* ud, int rc) {
        auto* impl = static_cast<Impl*>(ud);
        if (rc == 0) {
            impl->connected = true;
            impl->need_meta = true;  // republish retained metadata after every (re)connect
            log::info("mqtt: connected");
        } else {
            log::warn("mqtt: connection refused (", mosquitto_connack_string(rc), ")");
        }
    });
    mosquitto_disconnect_callback_set(m, [](mosquitto*, void* ud, int rc) {
        auto* impl = static_cast<Impl*>(ud);
        if (impl->connected) log::warn("mqtt: disconnected (rc=", rc, "), reconnecting");
        impl->connected = false;
    });
    const int rc = mosquitto_connect_async(m, opts_.host.c_str(), opts_.port, opts_.keepalive_s);
    if (rc != MOSQ_ERR_SUCCESS)
        log::warn("mqtt: broker ", opts_.host, ":", opts_.port, " not reachable yet (", mosquitto_strerror(rc),
                  "); will keep retrying");
    if (mosquitto_loop_start(m) != MOSQ_ERR_SUCCESS) {
        log::error("mqtt: cannot start network thread");
        return false;
    }
    log::info("mqtt: bridging to ", opts_.host, ":", opts_.port, " under '", opts_.prefix, "/'");
    stop_ = false;
    thread_ = std::thread([this] { run(); });
    return true;
}

void MqttBridge::stop() {
    if (!impl_ || !impl_->m) return;
    stop_ = true;
    sub_->close();
    if (thread_.joinable()) thread_.join();
    bool clean = false;
    if (impl_->connected) {
        const std::string off = "offline";
        mosquitto_publish(impl_->m, nullptr, impl_->status_topic.c_str(), static_cast<int>(off.size()), off.data(), 1,
                          true);
        clean = mosquitto_disconnect(impl_->m) == MOSQ_ERR_SUCCESS;
    }
    // After a clean disconnect the network thread exits by itself; cancel it
    // only if the broker was unreachable (it may be sleeping in a retry).
    mosquitto_loop_stop(impl_->m, !clean);
    mosquitto_destroy(impl_->m);
    impl_->m = nullptr;
    mosquitto_lib_cleanup();
    bus_.unsubscribe(sub_);
}

void MqttBridge::load_sensors() {
    impl_->sensors.clear();
    for (const auto& s : db_.list_sensors()) {
        const std::string slug = slugify(s.label);
        impl_->sensors[s.id] = {opts_.prefix + "/" + s.category + "/" + slug, s.unit};
        if (impl_->connected) {
            std::ostringstream meta;
            meta << "{\"id\":" << s.id << ",\"key\":\"" << json_escape(s.key) << "\",\"label\":\""
                 << json_escape(s.label) << "\",\"category\":\"" << json_escape(s.category) << "\",\"kind\":\""
                 << json_escape(s.kind) << "\",\"unit\":\"" << json_escape(s.unit) << "\"}";
            const std::string t = opts_.prefix + "/sensors/" + slug + "/meta", p = meta.str();
            mosquitto_publish(impl_->m, nullptr, t.c_str(), static_cast<int>(p.size()), p.data(), 1, true);
        }
    }
}

void MqttBridge::run() {
    try {
        load_sensors();
    } catch (const std::exception& e) {
        log::warn("mqtt: cannot load sensors: ", e.what());
    }
    while (!stop_) {
        auto msgs = sub_->drain(std::chrono::milliseconds(500));
        try {
            if (impl_->connected && impl_->need_meta.exchange(false)) {
                const std::string on = "online";
                mosquitto_publish(impl_->m, nullptr, impl_->status_topic.c_str(), static_cast<int>(on.size()),
                                  on.data(), 1, true);
                load_sensors();
            }
            for (const auto& m : msgs) {
                if (bus::payload_as<msg::SensorsChanged>(*m)) {
                    load_sensors();
                    continue;
                }
                if (!impl_->connected) continue;  // QoS 0 live data: skip while offline
                if (const auto* b = bus::payload_as<msg::SampleBatch>(*m)) {
                    for (const auto& s : b->samples) {
                        auto it = impl_->sensors.find(s.sensor_id);
                        if (it == impl_->sensors.end()) continue;
                        const std::string p = "{\"ts\":" + std::to_string(b->ts) + ",\"value\":" + num(s.value) +
                                              ",\"unit\":\"" + it->second.unit + "\"}";
                        if (mosquitto_publish(impl_->m, nullptr, it->second.data_topic.c_str(),
                                              static_cast<int>(p.size()), p.data(), 0, false) == MOSQ_ERR_SUCCESS)
                            ++published_;
                        else
                            ++failed_;
                    }
                } else if (const auto* t = bus::payload_as<msg::AlarmTransition>(*m)) {
                    const auto& a = t->alarm;
                    std::ostringstream os;
                    os << "{\"id\":" << a.id << ",\"sensor_id\":" << a.sensor_id << ",\"sensor\":\""
                       << json_escape(t->sensor_label) << "\",\"condition\":\"" << a.func << "(value, last "
                       << a.window_min << " min) " << a.op << " " << num(a.threshold) << "\",\"state\":\"" << a.state
                       << "\",\"from\":\"" << t->from_state << "\",\"value\":"
                       << (a.last_value ? num(*a.last_value) : std::string("null")) << ",\"ts\":" << t->ts << "}";
                    const std::string topic = opts_.prefix + "/alarms/" + std::to_string(a.id), p = os.str();
                    if (mosquitto_publish(impl_->m, nullptr, topic.c_str(), static_cast<int>(p.size()), p.data(), 1,
                                          true) == MOSQ_ERR_SUCCESS)
                        ++published_;
                    else
                        ++failed_;
                }
            }
        } catch (const std::exception& e) {
            log::warn("mqtt bridge: ", e.what());
        }
    }
}

MqttBridge::Stats MqttBridge::stats() const {
    Stats s;
    s.connected = impl_ && impl_->connected;
    s.published = published_;
    s.failed = failed_;
    s.dropped = sub_ ? sub_->dropped() : 0;
    s.broker = opts_.host + ":" + std::to_string(opts_.port);
    s.prefix = opts_.prefix;
    return s;
}

#else  // !SD_HAVE_MQTT

/// Stub used when the build has no libmosquitto: available() is false and start() fails.
struct MqttBridge::Impl {};
bool MqttBridge::available() { return false; }
MqttBridge::MqttBridge(bus::Bus& bus, db::Database& db, MqttOptions opts)
    : bus_(bus), db_(db), opts_(std::move(opts)) {}
MqttBridge::~MqttBridge() = default;
bool MqttBridge::start() {
    log::error("mqtt: this build has no MQTT support (install libmosquitto-dev and rebuild)");
    return false;
}
void MqttBridge::stop() {}
void MqttBridge::run() {}
void MqttBridge::load_sensors() {}
MqttBridge::Stats MqttBridge::stats() const { return {}; }

#endif

}  // namespace sd::pipeline

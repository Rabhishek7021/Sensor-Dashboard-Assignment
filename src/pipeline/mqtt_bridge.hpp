#pragma once
/**
 * @file mqtt_bridge.hpp
 * @brief MqttBridge: forwards the internal bus to an external MQTT broker.
 *
 * Works with any broker (Mosquitto, EMQX, HiveMQ, ...), so tools such as Home Assistant,
 * Node-RED, Telegraf/Grafana or `mosquitto_sub` can consume live data.
 *
 * Built only when libmosquitto is available (SD_HAVE_MQTT); otherwise
 * available() returns false and the binaries reject --mqtt with a message.
 *
 * Topics (prefix defaults to `sensordash/<hostname>`):
 * @verbatim
 *   <prefix>/status                    "online" / "offline" (retained, last will)
 *   <prefix>/sensors/<slug>/meta       {"id","key","label","category","kind","unit"} (retained)
 *   <prefix>/<category>/<slug>         {"ts":...,"value":...,"unit":"celsius"}   every sample, QoS 0
 *   <prefix>/alarms/<id>               {"id","sensor","condition","state","from","value","ts"} (retained, QoS 1)
 * @endverbatim
 * `<slug>` is the sensor label in lower case with non-alphanumerics as '_',
 * e.g. "core_0", "fan_1", "bat0_charge".
 *
 * Runs on its own thread (a bus subscriber); libmosquitto adds its network
 * thread and reconnects automatically if the broker restarts.
 */

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "../bus/bus.hpp"
#include "../common/db.hpp"

namespace sd::pipeline {

/// @brief Broker connection and topic settings.
struct MqttOptions {
    std::string host = "localhost";  ///< Broker host.
    int port = 1883;  ///< Broker port.
    std::string prefix;  ///< empty = `sensordash/<hostname>`
    std::string client_id;  ///< MQTT client id (empty = generated).
    ///@{
    std::string username, password;  ///< Optional credentials.
    ///@}
    int keepalive_s = 30;  ///< Keep-alive interval in seconds.
};

/// Parses "host", "host:port" or "mqtt://host:port".
MqttOptions parse_mqtt_url(const std::string& url);
/// @brief Topic-safe form of a label: lower case, non-alphanumerics as '_' ("Core 0" -> "core_0").
std::string slugify(const std::string& label);

/// @brief Bus subscriber that publishes samples, sensor metadata and alarm transitions to MQTT.
class MqttBridge {
public:
    /// @brief True if this build includes libmosquitto (SD_HAVE_MQTT).
    static bool available();

    /// @brief Creates the bridge; call start() to connect and run.
    MqttBridge(bus::Bus& bus, db::Database& db, MqttOptions opts);
    /// @brief Publishes "offline", disconnects and stops the thread.
    ~MqttBridge();
    /// Starts the bridge thread; connection happens in the background.
    bool start();
    /// @brief Stops the bridge thread and disconnects.
    void stop();

    /// @brief Counters for /api/stats.
    struct Stats {
        bool connected = false;  ///< True while connected to the broker.
        ///@{
        uint64_t published = 0, failed = 0, dropped = 0;  ///< Messages published, failed publishes, bus messages dropped.
        ///@}
        ///@{
        std::string broker, prefix;  ///< "host:port" and the topic prefix in use.
        ///@}
    };
    /// @brief Current counters.
    Stats stats() const;

private:
    /// libmosquitto handle and connection state (defined in the .cpp).
    struct Impl;
    /// @brief Thread body: drain the bus subscription and publish to the broker.
    void run();
    /// @brief Loads sensor metadata from the database and publishes the retained meta topics.
    void load_sensors();

    bus::Bus& bus_;  ///< Bus to mirror.
    db::Database& db_;  ///< Sensor metadata source.
    MqttOptions opts_;  ///< Configuration.
    std::shared_ptr<bus::Subscription> sub_;  ///< Subscription to samples, alarm transitions and sensor changes.
    std::unique_ptr<Impl> impl_;  ///< libmosquitto state.
    std::thread thread_;  ///< Bridge thread.
    std::atomic<bool> stop_{false};  ///< Set by stop().
    ///@{
    std::atomic<uint64_t> published_{0}, failed_{0};  ///< Counters behind Stats.
    ///@}
};

}  // namespace sd::pipeline

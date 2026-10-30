/**
 * @file main.cpp
 * @brief sensor-server: the whole system in one multi-threaded process.
 *
 * @verbatim
   Sampler threads (1 per provider, every 500 ms) ─┐
 *      or DbFollower (if a sensor-poller samples)   │ publish "samples/<source>"
 *                                                   ▼
 *                                     in-process pub/sub bus (MQTT-style topics)
 *      ┌──────────────┬──────────────┬──────────────┼───────────────┬──────────────┐
 *      ▼              ▼              ▼              ▼               ▼
 *   DbWriter      AlarmEngine    LiveCache     SSE streams     MqttBridge
 *   (batched      (in-memory     (latest       (one per open   (optional,
 *    SQLite)       windows)       values)       dashboard)      libmosquitto)
   @endverbatim
 *
 * plus the HTTP worker pool for the REST API and static dashboard files.
 * Every box is its own thread with its own queue; none of them waits on
 * another.
 */

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include "../bus/bus.hpp"
#include "../common/alarm_logic.hpp"
#include "../common/db.hpp"
#include "../common/log.hpp"
#include "../common/time_util.hpp"
#include "../pipeline/messages.hpp"
#include "../pipeline/options.hpp"
#include "../pipeline/source.hpp"
#include "../sensors/registry.hpp"
#include "alarm_engine.hpp"
#include "live_cache.hpp"

/// @brief JSON type used by the HTTP API.
using json = nlohmann::json;

namespace {

/// @brief Set by SIGINT / SIGTERM to stop cleanly.
std::atomic<bool> g_stop{false};
/// @brief Signal handler: requests shutdown.
void on_signal(int) { g_stop = true; }

/// @brief Parsed command line of sensor-server.
struct Config {
    std::string db_path = "data/sensors.db";  ///< SQLite file (--db, env SENSOR_DB).
    std::string host = "0.0.0.0";  ///< Bind address (--host).
    int port = 8080;  ///< HTTP port (--port, env PORT).
    std::string static_dir = "frontend/dist";  ///< Built dashboard directory (--static).
    std::string source = "auto";  ///< auto | local | db
    double alarm_interval_s = 1.0;  ///< Alarm evaluation period in seconds (--alarm-interval).
    int alarm_min_samples = 3;  ///< Samples needed before an alarm can fire (--alarm-min-samples).
    int sse_max = 32;  ///< Maximum concurrent live streams (--max-live-clients).
    sd::pipeline::PipelineConfig pipe;  ///< Shared sampling / storage / MQTT settings.
};

constexpr int kMaxWindowMinutes = 7 * 24 * 60;  ///< Longest history window accepted by /api/readings (7 days).
constexpr int kDefaultMaxPoints = 600;  ///< Default points per series for /api/readings.

/// @brief Prints the command-line help.
void usage() {
    std::cout << "Usage: sensor-server [options]\n"
                 "  --db PATH                 SQLite file (default data/sensors.db)\n"
                 "  --host ADDR               bind address (default 0.0.0.0)\n"
                 "  --port N                  port (default 8080)\n"
                 "  --static DIR              built frontend directory (default frontend/dist)\n"
                 "  --source auto|local|db    auto (default): sample here unless a sensor-poller already\n"
                 "                            samples into --db, then follow the database;\n"
                 "                            local: always sample here; db: never sample, only follow\n"
                 "  --alarm-interval SECONDS  alarm evaluation period (default 1)\n"
                 "  --alarm-min-samples N     samples needed in a window before an alarm can fire (default 3)\n"
                 "  --max-live-clients N      concurrent live dashboard streams (default 32)\n"
              << sd::pipeline::pipeline_usage() << "Environment: SENSOR_DB, PORT\n";
}

/// @brief Parses the command line and environment; exits on --help or invalid input.
Config parse(int argc, char** argv) {
    Config c;
    if (const char* v = std::getenv("SENSOR_DB")) c.db_path = v;
    if (const char* v = std::getenv("PORT")) c.port = std::atoi(v);
    sd::pipeline::apply_pipeline_env(c.pipe);
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::cerr << "missing value for " << a << "\n"; std::exit(2); }
            return argv[++i];
        };
        if (sd::pipeline::parse_pipeline_flag(a, next, c.pipe)) continue;
        if (a == "--db") c.db_path = next();
        else if (a == "--host") c.host = next();
        else if (a == "--port") c.port = std::atoi(next().c_str());
        else if (a == "--static") c.static_dir = next();
        else if (a == "--source") c.source = next();
        else if (a == "--alarm-interval") c.alarm_interval_s = std::atof(next().c_str());
        else if (a == "--alarm-min-samples") c.alarm_min_samples = std::atoi(next().c_str());
        else if (a == "--max-live-clients") c.sse_max = std::atoi(next().c_str());
        else if (a == "-h" || a == "--help") { usage(); std::exit(0); }
        else { std::cerr << "unknown option " << a << "\n"; usage(); std::exit(2); }
    }
    std::string err = sd::pipeline::validate(c.pipe);
    if (c.source != "auto" && c.source != "local" && c.source != "db") err = "--source must be auto, local or db";
    if (c.alarm_interval_s < 0.1) err = "--alarm-interval must be >= 0.1";
    if (!err.empty()) {
        std::cerr << err << "\n";
        std::exit(2);
    }
    return c;
}

// ---------- helpers ----------

/// @brief Error carrying an HTTP status; turned into {"error": ...} by the exception handler.
struct HttpError : std::runtime_error {
    int status;  ///< HTTP status code to send.
    /// @brief Creates an error with status `s` and message `m`.
    HttpError(int s, const std::string& m) : std::runtime_error(m), status(s) {}
};

/// @brief Sends `body` as application/json with the given status.
void send_json(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

/// @brief Reads an integer query parameter, `def` if absent.
/// @throws HttpError 400 if it is not an integer in [lo, hi].
long long parse_int_param(const httplib::Request& req, const std::string& name, long long def, long long lo,
                          long long hi) {
    if (!req.has_param(name)) return def;
    const std::string s = req.get_param_value(name);
    try {
        size_t pos = 0;
        long long v = std::stoll(s, &pos);
        if (pos != s.size()) throw std::invalid_argument("trailing");
        if (v < lo || v > hi) throw HttpError(400, name + " must be between " + std::to_string(lo) + " and " +
                                                       std::to_string(hi));
        return v;
    } catch (const HttpError&) {
        throw;
    } catch (...) {
        throw HttpError(400, name + " must be an integer");
    }
}

/// @brief Parses "1,2,3" into a set of ids (invalid items rejected with HttpError 400).
std::set<int64_t> parse_id_list(const std::string& s) {
    std::set<int64_t> ids;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.empty()) continue;
        try { ids.insert(std::stoll(item)); } catch (...) { throw HttpError(400, "invalid sensor id: " + item); }
    }
    return ids;
}

/// @brief JSON number rounded to 3 decimals, or null.
json opt(const std::optional<double>& v) { return v ? json(std::round(*v * 1000) / 1000) : json(nullptr); }
/// @brief JSON integer, or null.
json opt(const std::optional<int64_t>& v) { return v ? json(*v) : json(nullptr); }

/// @brief JSON form of an alarm, including the sensor label and a readable condition such as
/// "avg(value, last 10 min) > 75" or "min(value, last 5 min) < 20".
json alarm_json(const sd::db::Alarm& a, const std::map<int64_t, std::string>& labels) {
    auto it = labels.find(a.sensor_id);
    return {{"id", a.id},
            {"sensor_id", a.sensor_id},
            {"sensor_label", it != labels.end() ? it->second : ""},
            {"func", a.func},
            {"window_min", a.window_min},
            {"threshold", a.threshold},
            {"op", a.op},
            {"enabled", a.enabled},
            {"state", a.state},
            {"last_value", opt(a.last_value)},
            {"last_eval_ts", opt(a.last_eval_ts)},
            {"last_change_ts", opt(a.last_change_ts)},
            {"created_at", a.created_at},
            {"condition", a.func + "(value, last " + std::to_string(a.window_min) + " min) " + a.op + " " +
                              [&] { std::ostringstream o; o << a.threshold; return o.str(); }()}};
}

/// @brief Sensor id -> label for all sensors.
std::map<int64_t, std::string> sensor_labels(sd::db::Database& db) {
    std::map<int64_t, std::string> m;
    for (const auto& s : db.list_sensors()) m[s.id] = s.label;
    return m;
}

/// @brief Rejects a threshold outside the range of the sensor's unit
///        (sd::alarm::threshold_range: -50 to 100 °C, 0 to 5000 RPM, 0 to 100 %).
/// @throws HttpError 400 naming the sensor and the accepted range.
void check_threshold(const sd::db::Sensor& s, double threshold) {
    if (sd::alarm::is_valid_threshold(s.unit, threshold)) return;
    const auto r = sd::alarm::threshold_range(s.unit);
    std::ostringstream msg;
    msg << "threshold for " << s.label << " must be from " << r->min << " to " << r->max << " " << r->suffix;
    throw HttpError(400, msg.str());
}

/// @brief Parses the request body as JSON.
/// @throws HttpError 400 for invalid JSON, including numbers too large for a double (e.g. 1e400).
json parse_body(const httplib::Request& req) {
    try {
        return json::parse(req.body);
    } catch (const json::exception& e) {
        throw HttpError(400, std::string("invalid JSON body: ") + e.what());
    }
}

/// Validates fields shared by create and patch. Throws HttpError on bad input.
void validate_alarm_fields(const json& b, bool require_all) {
    auto need = [&](const char* k) {
        if (require_all && !b.contains(k)) throw HttpError(400, std::string("missing field: ") + k);
    };
    need("func");
    need("window_min");
    need("threshold");
    if (b.contains("func")) {
        if (!b["func"].is_string() || !sd::alarm::is_valid_func(b["func"].get<std::string>()))
            throw HttpError(400, "func must be one of avg, max, min, stddev");
    }
    if (b.contains("window_min")) {
        if (!b["window_min"].is_number_integer() || !sd::alarm::is_valid_window(b["window_min"].get<int64_t>()))
            throw HttpError(400, "window_min must be a whole number of minutes from " +
                                     std::to_string(sd::alarm::kMinWindowMin) + " to " +
                                     std::to_string(sd::alarm::kMaxWindowMin));
    }
    // The range of a threshold depends on the sensor's unit; see check_threshold().
    if (b.contains("threshold") && (!b["threshold"].is_number() || !std::isfinite(b["threshold"].get<double>())))
        throw HttpError(400, "threshold must be a number");
    // Optional everywhere: a new alarm without "op" fires above the threshold.
    if (b.contains("op") && (!b["op"].is_string() || !sd::alarm::is_valid_op(b["op"].get<std::string>())))
        throw HttpError(400, "op must be \">\" (fires above threshold) or \"<\" (fires below threshold)");
    if (b.contains("enabled") && !b["enabled"].is_boolean()) throw HttpError(400, "enabled must be a boolean");
}

/// @brief The numeric id captured from the URL path.
/// @throws HttpError 400 if it is not a number.
int64_t path_id(const httplib::Request& req) {
    try { return std::stoll(req.matches[1]); } catch (...) { throw HttpError(400, "invalid id"); }
}


/// Rounds for compact JSON (sensor values never need more than 3 decimals).
double r3(double v) { return std::round(v * 1000) / 1000; }

/// Formats bus messages as Server-Sent Events. Samples of the same tick from
/// different providers are merged into one entry, and only the newest alarm
/// snapshot is sent, so a browser gets at most one small event per kind per
/// wake-up regardless of how many providers there are.
std::string encode_sse(const std::vector<sd::bus::MessagePtr>& msgs, const std::map<int64_t, std::string>& labels) {
    using namespace sd;
    std::map<int64_t, std::vector<msg::Sample>> ticks;
    const msg::AlarmSnapshot* snap = nullptr;
    std::string out;
    bool sensors_changed = false;
    for (const auto& m : msgs) {
        if (const auto* b = bus::payload_as<msg::SampleBatch>(*m)) {
            auto& t = ticks[b->ts];
            t.insert(t.end(), b->samples.begin(), b->samples.end());
        } else if (const auto* s = bus::payload_as<msg::AlarmSnapshot>(*m)) {
            snap = s;
        } else if (const auto* t = bus::payload_as<msg::AlarmTransition>(*m)) {
            json j = {{"id", t->alarm.id},           {"sensor_id", t->alarm.sensor_id},
                      {"sensor_label", t->sensor_label}, {"state", t->alarm.state},
                      {"from", t->from_state},       {"value", opt(t->alarm.last_value)},
                      {"ts", t->ts}};
            out += "event: alarm\ndata: " + j.dump() + "\n\n";
        } else if (bus::payload_as<msg::SensorsChanged>(*m)) {
            sensors_changed = true;
        }
    }
    if (!ticks.empty()) {
        std::string d = "[";
        char buf[64];
        bool first_tick = true;
        for (const auto& [ts, samples] : ticks) {
            d += first_tick ? "" : ",";
            first_tick = false;
            d += "{\"ts\":" + std::to_string(ts) + ",\"v\":{";
            for (size_t i = 0; i < samples.size(); ++i) {
                std::snprintf(buf, sizeof(buf), "%s\"%lld\":%.3f", i ? "," : "",
                              static_cast<long long>(samples[i].sensor_id), samples[i].value);
                d += buf;
            }
            d += "}}";
        }
        out += "event: samples\ndata: " + d + "]\n\n";
    }
    if (snap) {
        json arr = json::array();
        for (const auto& a : snap->alarms)
            arr.push_back({{"id", a.id}, {"state", a.state}, {"last_value", opt(a.last_value)},
                           {"last_eval_ts", opt(a.last_eval_ts)}, {"last_change_ts", opt(a.last_change_ts)},
                           {"samples", a.samples}});
        out += "event: alarms\ndata: " + arr.dump() + "\n\n";
    }
    if (sensors_changed) out += "event: sensors\ndata: {}\n\n";
    (void)labels;
    return out;
}

/// Serves the built dashboard from memory: files are read once (and again
/// only if they change on disk), so responses can be gzip-compressed, and
/// content-hashed assets get a one-year cache lifetime.
class StaticFiles {
public:
    /// @brief Serves files below `root`.
    explicit StaticFiles(std::filesystem::path root) : root_(std::move(root)) {}

    /// @brief Writes the file for `url_path` into `res`; unknown non-asset paths get index.html.
    /// @return false if nothing can be served (404).
    bool serve(const std::string& url_path, httplib::Response& res) {
        std::string rel = url_path.empty() || url_path == "/" ? "index.html" : url_path.substr(1);
        if (rel.find("..") != std::string::npos) return false;
        auto path = root_ / rel;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            if (rel.rfind("assets/", 0) == 0 || rel.rfind("api/", 0) == 0) return false;
            path = root_ / "index.html";  // client-side routes fall back to the app
            rel = "index.html";
        }
        const auto mtime = std::filesystem::last_write_time(path, ec);
        if (ec) return false;
        std::lock_guard lock(mu_);
        auto& e = cache_[rel];
        if (e.mtime != mtime || e.body.empty()) {
            std::ifstream in(path, std::ios::binary);
            e.body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            e.mtime = mtime;
        }
        res.set_content(e.body, mime(rel));
        res.set_header("Cache-Control", rel.rfind("assets/", 0) == 0 ? "public, max-age=31536000, immutable"
                                                                    : "no-cache");
        return true;
    }

private:
    /// @brief Content type from the file extension.
    static const char* mime(const std::string& p) {
        auto ends = [&](const char* ext) {
            const std::string e(ext);
            return p.size() >= e.size() && p.compare(p.size() - e.size(), e.size(), e) == 0;
        };
        if (ends(".html")) return "text/html";
        if (ends(".js")) return "text/javascript";
        if (ends(".css")) return "text/css";
        if (ends(".svg")) return "image/svg+xml";
        if (ends(".json")) return "application/json";
        if (ends(".png")) return "image/png";
        if (ends(".ico")) return "image/x-icon";
        if (ends(".woff2")) return "font/woff2";
        return "application/octet-stream";
    }
    /// @brief One cached file.
    struct Entry {
        std::filesystem::file_time_type mtime{};  ///< Modification time when the file was read.
        std::string body;  ///< File contents.
    };
    std::filesystem::path root_;  ///< Directory served.
    std::mutex mu_;  ///< Guards cache_.
    std::map<std::string, Entry> cache_;  ///< Files read so far, by relative path.
};

/// Open live streams, so shutdown can end them before stopping the server.
struct SseHub {
    std::mutex mu;  ///< Guards subs.
    std::set<std::shared_ptr<sd::bus::Subscription>> subs;  ///< Bus subscription of each open stream.
    std::atomic<int> clients{0};  ///< Streams currently open.
    std::atomic<uint64_t> total{0};  ///< Streams opened since start.
    /// @brief Registers an open stream.
    void add(const std::shared_ptr<sd::bus::Subscription>& s) {
        std::lock_guard l(mu);
        subs.insert(s);
        ++clients;
        ++total;
    }
    /// @brief Unregisters a closed stream.
    void remove(const std::shared_ptr<sd::bus::Subscription>& s) {
        std::lock_guard l(mu);
        if (subs.erase(s)) --clients;
    }
    /// @brief Closes every open stream (shutdown).
    void close_all() {
        std::lock_guard l(mu);
        for (auto& s : subs) s->close();
    }
};

}  // namespace

/// @brief Entry point of sensor-server.
///
/// Opens the database (one connection per role), chooses the sample source
/// (local Sampler, or DbFollower when a sensor-poller holds the sampler lock),
/// starts DbWriter, AlarmEngine, LiveCache and the optional MqttBridge, then
/// serves the HTTP API and dashboard until SIGINT / SIGTERM.
///
/// Routes:
/// - GET /api/health, /api/sensors, /api/readings, /api/stream (SSE), /api/stats
/// - GET / POST /api/alarms, PATCH / DELETE /api/alarms/{id}, GET /api/alarm-events
/// - GET everything else: the dashboard from --static
///
/// @return 0 on clean exit, non-zero if start-up fails.
int main(int argc, char** argv) {
    using namespace sd;
    Config cfg = parse(argc, argv);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    try {
        std::filesystem::path dbp(cfg.db_path);
        if (dbp.has_parent_path()) std::filesystem::create_directories(dbp.parent_path());

        // One SQLite connection per role: WAL lets the API read while the
        // writer writes, and no role waits on another's mutex.
        db::Database api_db(cfg.db_path);
        api_db.migrate();
        db::Database writer_db(cfg.db_path);
        db::Database alarm_db(cfg.db_path);
        bus::Bus bus;

        // ---- sensors / sampling source ----
        const auto& registry = sensors::builtin_providers();
        std::unique_ptr<sensors::SensorManager> manager;
        try {
            manager = std::make_unique<sensors::SensorManager>(
                registry, sensors::parse_provider_list(cfg.pipe.providers, registry), cfg.pipe.ctx);
        } catch (const sensors::UnknownProvider& e) {
            std::cerr << e.what() << "\n";
            return 2;
        }

        // Subscribers first, so they see the very first samples.
        server::AlarmEngine engine(alarm_db, bus,
                                   {std::chrono::milliseconds(static_cast<long>(cfg.alarm_interval_s * 1000)),
                                    cfg.alarm_min_samples});
        server::LiveCache live(bus);
        std::unique_ptr<pipeline::MqttBridge> mqtt;
        if (!cfg.pipe.mqtt_url.empty())
            mqtt = std::make_unique<pipeline::MqttBridge>(bus, api_db, pipeline::mqtt_options(cfg.pipe));

        std::unique_ptr<pipeline::SamplerLock> lock;
        std::unique_ptr<pipeline::DbWriter> writer;
        std::unique_ptr<pipeline::Sampler> sampler;
        std::unique_ptr<pipeline::DbFollower> follower;
        std::string mode;

        auto start_local = [&]() -> bool {
            writer = std::make_unique<pipeline::DbWriter>(writer_db, bus, cfg.pipe.writer);
            sampler = std::make_unique<pipeline::Sampler>(*manager, writer_db, bus, cfg.pipe.sampler);
            writer->start();
            mode = "local";
            if (sampler->start() == 0) {
                log::warn("no sensors found for providers {", cfg.pipe.providers,
                          "}; serving stored data only (check --providers / --hwmon-root, or load the driver, "
                          "e.g. `sudo modprobe coretemp`)");
                return false;
            }
            log::info("sampling ", sampler->summary(), " every ", cfg.pipe.sampler.interval.count(), " ms");
            return true;
        };
        auto start_follow = [&] {
            const auto pid = pipeline::SamplerLock::holder(cfg.db_path);
            log::info("another process", pid ? " (pid " + std::to_string(*pid) + ")" : std::string(),
                      " samples into ", cfg.db_path, "; following the database for live data");
            follower = std::make_unique<pipeline::DbFollower>(api_db, bus, cfg.pipe.sampler.interval);
            follower->start();
            mode = "db";
        };

        engine.start();
        live.start();
        if (mqtt && !mqtt->start()) return 1;

        if (cfg.source != "db") lock = pipeline::SamplerLock::try_acquire(cfg.db_path);
        if (lock) {
            start_local();
        } else if (cfg.source == "local") {
            log::error("--source local: another process is already sampling into ", cfg.db_path);
            return 1;
        } else {
            start_follow();
        }

        // ---- HTTP ----
        httplib::Server svr;
        SseHub hub;
        const int pool = 8 + cfg.sse_max;  // every live stream holds one worker
        svr.new_task_queue = [pool] { return new httplib::ThreadPool(static_cast<size_t>(pool)); };

        // Errors: HttpError -> its status, anything else -> 500; always {"error": ...}.
        svr.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const HttpError& e) {
                send_json(res, {{"error", e.what()}}, e.status);
            } catch (const std::exception& e) {
                log::error("unhandled error: ", e.what());
                send_json(res, {{"error", "internal error"}}, 500);
            }
        });
        // Debug-level log of every API request (not shown at the default Info level).
        svr.set_logger([](const httplib::Request& req, const httplib::Response& res) {
            if (req.path.rfind("/api/", 0) == 0) log::debug(req.method, " ", req.path, " -> ", res.status);
        });
        // Permissive CORS so the Vite dev server (port 5173) can call the API.
        svr.set_default_headers({{"Access-Control-Allow-Origin", "*"},
                                 {"Access-Control-Allow-Methods", "GET, POST, PATCH, DELETE, OPTIONS"},
                                 {"Access-Control-Allow-Headers", "Content-Type"}});
        svr.Options(R"(/api/.*)", [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

        const int64_t interval_ms = cfg.pipe.sampler.interval.count();

        // ---- health ----
        svr.Get("/api/health", [&](const httplib::Request&, httplib::Response& res) {
            const int64_t now = time::now_ms();
            const int64_t last = live.last_ts();
            const bool fresh = last > 0 && now - last <= std::max<int64_t>(3 * interval_ms, 5000);
            send_json(res, {{"status", fresh ? "ok" : "stale"},
                            {"server_time", now},
                            {"last_sample_ts", last > 0 ? json(last) : json(nullptr)},
                            {"readings_last_minute", live.samples_last_minute(now)},
                            {"source", mode},
                            {"interval_ms", interval_ms}});
        });

        // ---- sensors: metadata from the DB, latest value from memory ----
        svr.Get("/api/sensors", [&](const httplib::Request&, httplib::Response& res) {
            std::map<int64_t, int> firing;
            const auto snap = engine.snapshot();
            for (const auto& def : api_db.list_alarms())
                if (auto it = snap.find(def.id); it != snap.end() && it->second.state == "firing") firing[def.sensor_id]++;
            const auto latest = live.latest();
            json arr = json::array();
            for (const auto& s : api_db.list_sensors()) {
                json lat = nullptr;
                if (auto it = latest.find(s.id); it != latest.end())
                    lat = {{"ts", it->second.ts}, {"value", r3(it->second.value)}};
                else if (s.latest_ts)
                    lat = {{"ts", *s.latest_ts}, {"value", r3(*s.latest_value)}};
                arr.push_back({{"id", s.id},
                               {"key", s.key},
                               {"chip", s.chip},
                               {"label", s.label},
                               {"kind", s.kind},
                               {"category", s.category},
                               {"unit", s.unit},
                               {"latest", lat},
                               {"alarms_firing", firing[s.id]}});
            }
            send_json(res, arr);
        });

        // ---- readings: bucketed history for a window ----
        // Bucket width = max(sampling interval, window / max_points); 10 s and
        // wider buckets read the rollup table, so 24 h costs about as much as
        // 15 min. Points are [ts, avg, min, max, count].
        svr.Get("/api/readings", [&](const httplib::Request& req, httplib::Response& res) {
            // `to` is the newest reading already stored, so a client can append
            // live samples with ts > to without gaps or double counting.
            int64_t now = time::now_ms();
            if (writer && writer->flushed_until() > 0) now = std::min(now, writer->flushed_until());
            const auto minutes = parse_int_param(req, "minutes", 15, 1, kMaxWindowMinutes);
            const auto max_points = parse_int_param(req, "max_points", kDefaultMaxPoints, 10, 10000);
            std::set<int64_t> wanted;
            if (req.has_param("sensor_ids")) wanted = parse_id_list(req.get_param_value("sensor_ids"));

            const int64_t window = minutes * 60 * 1000;
            int64_t bucket = std::max<int64_t>(interval_ms, (window + max_points - 1) / max_points);
            if (bucket >= db::kRollupMs) bucket = (bucket + db::kRollupMs - 1) / db::kRollupMs * db::kRollupMs;
            const int64_t from = (now - window) / bucket * bucket;

            json series = json::array();
            for (const auto& s : api_db.list_sensors()) {
                if (!wanted.empty() && !wanted.count(s.id)) continue;
                json pts = json::array();
                for (const auto& b : api_db.buckets(s.id, from, now, bucket)) {
                    if (b.n <= 0) continue;
                    pts.push_back({b.ts, r3(b.sum / static_cast<double>(b.n)), r3(b.min), r3(b.max), b.n});
                }
                series.push_back({{"sensor_id", s.id},
                                  {"key", s.key},
                                  {"label", s.label},
                                  {"kind", s.kind},
                                  {"category", s.category},
                                  {"unit", s.unit},
                                  {"points", std::move(pts)}});
            }
            send_json(res, {{"from", from},
                            {"to", now},
                            {"minutes", minutes},
                            {"bucket_ms", bucket},
                            {"interval_ms", interval_ms},
                            {"point_format", {"ts", "avg", "min", "max", "count"}},
                            {"series", std::move(series)}});
        });

        // ---- live stream (Server-Sent Events) ----
        // Each open dashboard gets its own bus subscription and receives every
        // tick (all providers merged), alarm transitions and alarm snapshots
        // as they happen. A slow browser only drops its own oldest messages.
        svr.Get("/api/stream", [&](const httplib::Request&, httplib::Response& res) {
            if (hub.clients >= cfg.sse_max) throw HttpError(503, "too many live clients");
            auto sub = bus.subscribe("sse-client",
                                     {msg::topic::kSamplesAll, msg::topic::kAlarmStateAll,
                                      msg::topic::kAlarmSnapshot, msg::topic::kSensorsChanged},
                                     512);
            hub.add(sub);
            struct State {
                bool hello = false;
                std::chrono::steady_clock::time_point last_write = std::chrono::steady_clock::now();
                std::set<std::string> sources;  // providers seen so far (cpu_temp, fan, ...)
            };
            auto st = std::make_shared<State>();
            res.set_header("Cache-Control", "no-cache");
            res.set_header("X-Accel-Buffering", "no");
            res.set_chunked_content_provider(
                "text/event-stream",
                [&, sub, st](size_t, httplib::DataSink& sink) {
                    std::string out;
                    if (!st->hello) {
                        st->hello = true;
                        json h = {{"server_time", time::now_ms()}, {"interval_ms", interval_ms}, {"source", mode}};
                        out = "retry: 2000\nevent: hello\ndata: " + h.dump() + "\n\n";
                    } else {
                        auto msgs = sub->drain(std::chrono::milliseconds(1000));
                        for (const auto& m : msgs)
                            if (bus::payload_as<msg::SensorsChanged>(*m)) st->sources.clear();  // providers may have changed
                        // Each provider publishes its own batch for a tick. Send the
                        // tick as soon as every provider seen before has reported;
                        // wait at most 30 ms for stragglers, so one tick is one event.
                        auto tick_complete = [&] {
                            int64_t newest = 0;
                            for (const auto& m : msgs)
                                if (const auto* b = bus::payload_as<msg::SampleBatch>(*m)) newest = std::max(newest, b->ts);
                            if (newest == 0) return true;
                            std::set<std::string> got;
                            for (const auto& m : msgs)
                                if (const auto* b = bus::payload_as<msg::SampleBatch>(*m))
                                    if (b->ts == newest) got.insert(b->source);
                            const bool complete = std::includes(got.begin(), got.end(), st->sources.begin(), st->sources.end());
                            st->sources.insert(got.begin(), got.end());
                            return complete;
                        };
                        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
                        while (!msgs.empty() && !tick_complete() && std::chrono::steady_clock::now() < deadline) {
                            auto more = sub->drain_until(deadline);
                            msgs.insert(msgs.end(), more.begin(), more.end());
                        }
                        if (sub->closed() || g_stop) {
                            sink.done();
                            return true;
                        }
                        out = encode_sse(msgs, {});
                        if (out.empty() && std::chrono::steady_clock::now() - st->last_write > std::chrono::seconds(10))
                            out = ": ping\n\n";  // keeps proxies open and detects closed browsers
                    }
                    if (!out.empty()) {
                        if (!sink.write(out.data(), out.size())) return false;
                        st->last_write = std::chrono::steady_clock::now();
                    }
                    return true;
                },
                [&, sub](bool) {
                    hub.remove(sub);
                    bus.unsubscribe(sub);
                });
        });

        // ---- pipeline statistics ----
        svr.Get("/api/stats", [&](const httplib::Request&, httplib::Response& res) {
            json j;
            j["source"] = mode;
            j["interval_ms"] = interval_ms;
            j["bus"]["published"] = bus.published();
            json subs = json::array();
            for (const auto& s : bus.stats())
                subs.push_back({{"name", s.name}, {"queued", s.depth}, {"capacity", s.capacity},
                                {"received", s.received}, {"dropped", s.dropped}});
            j["bus"]["subscribers"] = subs;
            if (sampler) {
                const auto s = sampler->stats();
                j["sampler"] = {{"sensors", s.sensors}, {"threads", s.threads}, {"ticks", s.ticks},
                                {"samples", s.samples}, {"read_failures", s.read_failures},
                                {"skipped_ticks", s.skipped_ticks}, {"last_tick_us", r3(s.last_tick_us)},
                                {"max_tick_us", r3(s.max_tick_us)}};
            }
            if (writer) {
                const auto w = writer->stats();
                j["db_writer"] = {{"rows", w.rows}, {"flushes", w.flushes}, {"pending", w.pending},
                                  {"rollup_buckets", w.rollup_buckets}, {"pruned", w.pruned}, {"errors", w.errors},
                                  {"last_flush_ms", r3(w.last_flush_ms)}, {"max_flush_ms", r3(w.max_flush_ms)},
                                  {"dropped", w.dropped}};
            }
            if (follower) j["db_follower"] = {{"published", follower->published()}};
            const auto a = engine.stats();
            j["alarm_engine"] = {{"alarms", a.alarms}, {"evaluations", a.evaluations},
                                 {"transitions", a.transitions}, {"samples", a.samples},
                                 {"sensors_tracked", a.sensors_tracked}, {"last_eval_us", r3(a.last_eval_us)},
                                 {"max_eval_us", r3(a.max_eval_us)}, {"dropped", a.dropped}};
            j["live_clients"] = hub.clients.load();
            j["live_clients_total"] = hub.total.load();
            if (mqtt) {
                const auto m = mqtt->stats();
                j["mqtt"] = {{"connected", m.connected}, {"broker", m.broker}, {"prefix", m.prefix},
                             {"published", m.published}, {"failed", m.failed}, {"dropped", m.dropped}};
            }
            send_json(res, j);
        });

        // ---- alarms CRUD (each alarm belongs to exactly one sensor) ----
        // Definitions live in the DB; live state comes from the alarm engine.
        auto alarm_out = [&](const db::Alarm& a, const std::map<int64_t, std::string>& labels,
                             const std::map<int64_t, msg::AlarmLive>& snap) {
            json j = alarm_json(a, labels);
            if (auto it = snap.find(a.id); it != snap.end()) {
                j["state"] = it->second.state;
                j["last_value"] = opt(it->second.last_value);
                j["last_eval_ts"] = opt(it->second.last_eval_ts);
                j["last_change_ts"] = opt(it->second.last_change_ts);
                j["samples"] = it->second.samples;
            }
            return j;
        };

        // List alarms (optionally ?sensor_id=) with live state.
        svr.Get("/api/alarms", [&](const httplib::Request& req, httplib::Response& res) {
            std::optional<int64_t> sensor;
            if (req.has_param("sensor_id")) sensor = parse_int_param(req, "sensor_id", 0, 1, INT64_MAX);
            const auto labels = sensor_labels(api_db);
            const auto snap = engine.snapshot();
            json arr = json::array();
            for (const auto& a : api_db.list_alarms())
                if (!sensor || a.sensor_id == *sensor) arr.push_back(alarm_out(a, labels, snap));
            send_json(res, arr);
        });

        // Body: {"sensor_id": 3 | "sensor_ids": [1,2,...] | "sensor_ids": "all",
        //        "func": "avg", "window_min": 10, "threshold": 75, "op": ">"}
        // "op" is optional: ">" (default, fires above the threshold) or "<" (fires below it).
        // Creating for several sensors yields one independent alarm per sensor.
        svr.Post("/api/alarms", [&](const httplib::Request& req, httplib::Response& res) {
            json b = parse_body(req);
            if (!b.is_object()) throw HttpError(400, "body must be a JSON object");
            validate_alarm_fields(b, true);

            std::vector<int64_t> targets;
            if (b.contains("sensor_ids") && b["sensor_ids"] == "all") {
                for (const auto& s : api_db.list_sensors()) targets.push_back(s.id);
            } else if (b.contains("sensor_ids") && b["sensor_ids"].is_array()) {
                for (const auto& v : b["sensor_ids"]) {
                    if (!v.is_number_integer()) throw HttpError(400, "sensor_ids must contain integers");
                    targets.push_back(v.get<int64_t>());
                }
            } else if (b.contains("sensor_id") && b["sensor_id"].is_number_integer()) {
                targets.push_back(b["sensor_id"].get<int64_t>());
            } else {
                throw HttpError(400, "provide sensor_id (integer) or sensor_ids (array or \"all\")");
            }
            if (targets.empty()) throw HttpError(400, "no target sensors");
            for (auto id : targets)
                if (!api_db.sensor_exists(id)) throw HttpError(404, "sensor " + std::to_string(id) + " not found");
            {
                // The threshold must suit every target's unit; nothing is created otherwise.
                const double y = b["threshold"].get<double>();
                const std::set<int64_t> wanted(targets.begin(), targets.end());
                for (const auto& s : api_db.list_sensors())
                    if (wanted.count(s.id)) check_threshold(s, y);
            }

            const int64_t now = time::now_ms();
            std::vector<int64_t> created;
            for (auto id : targets)
                created.push_back(api_db.create_alarm(id, b["func"], b["window_min"].get<int>(),
                                                      b["threshold"].get<double>(), now, b.value("op", std::string(">"))));
            engine.reload();  // evaluates the new alarms at once

            const auto labels = sensor_labels(api_db);
            const auto snap = engine.snapshot();
            json arr = json::array();
            for (auto id : created)
                if (auto a = api_db.get_alarm(id)) arr.push_back(alarm_out(*a, labels, snap));
            send_json(res, {{"created", arr}}, 201);
        });

        // Partial update: any of func, window_min, threshold, op, enabled.
        svr.Patch(R"(/api/alarms/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
            const int64_t id = path_id(req);
            json b = parse_body(req);
            if (!b.is_object()) throw HttpError(400, "body must be a JSON object");
            validate_alarm_fields(b, false);
            if (b.contains("threshold")) {
                const auto cur = api_db.get_alarm(id);
                if (!cur) throw HttpError(404, "alarm not found");
                for (const auto& s : api_db.list_sensors())
                    if (s.id == cur->sensor_id) check_threshold(s, b["threshold"].get<double>());
            }
            db::AlarmPatch p;
            if (b.contains("func")) p.func = b["func"].get<std::string>();
            if (b.contains("window_min")) p.window_min = b["window_min"].get<int>();
            if (b.contains("threshold")) p.threshold = b["threshold"].get<double>();
            if (b.contains("op")) p.op = b["op"].get<std::string>();
            if (b.contains("enabled")) p.enabled = b["enabled"].get<bool>();
            if (!api_db.update_alarm(id, p)) throw HttpError(404, "alarm not found");
            engine.reload();
            send_json(res, alarm_out(*api_db.get_alarm(id), sensor_labels(api_db), engine.snapshot()));
        });

        // Delete an alarm; its events cascade.
        svr.Delete(R"(/api/alarms/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
            if (!api_db.delete_alarm(path_id(req))) throw HttpError(404, "alarm not found");
            engine.reload();
            res.status = 204;
        });

        // Fired / cleared history, newest first (?limit=50&alarm_id=).
        svr.Get("/api/alarm-events", [&](const httplib::Request& req, httplib::Response& res) {
            const auto limit = parse_int_param(req, "limit", 50, 1, 1000);
            std::optional<int64_t> alarm_id;
            if (req.has_param("alarm_id")) alarm_id = parse_int_param(req, "alarm_id", 0, 1, INT64_MAX);
            auto labels = sensor_labels(api_db);
            json arr = json::array();
            for (const auto& e : api_db.list_events(static_cast<int>(limit), alarm_id)) {
                arr.push_back({{"id", e.id},
                               {"alarm_id", e.alarm_id},
                               {"sensor_id", e.sensor_id},
                               {"sensor_label", labels[e.sensor_id]},
                               {"ts", e.ts},
                               {"type", e.type},
                               {"value", opt(e.value)},
                               {"threshold", e.threshold},
                               {"op", e.op}});
            }
            send_json(res, arr);
        });

        // ---- frontend ----
        StaticFiles files(cfg.static_dir);
        if (std::filesystem::exists(cfg.static_dir)) {
            svr.Get(R"(/(?!api/).*)", [&](const httplib::Request& req, httplib::Response& res) {
                if (!files.serve(req.path, res)) send_json(res, {{"error", "not found"}}, 404);
            });
            log::info("serving dashboard from ", cfg.static_dir);
        } else {
            log::warn("static dir ", cfg.static_dir, " not found; API only (build the frontend first)");
        }

        if (!svr.bind_to_port(cfg.host, cfg.port)) {
            log::error("could not bind ", cfg.host, ":", cfg.port);
            g_stop = true;
        }
        std::thread http;
        if (!g_stop) {
            http = std::thread([&] { svr.listen_after_bind(); });
            log::info("listening on http://", cfg.host, ":", cfg.port, " (db: ", cfg.db_path, ", source: ", mode,
                      ")");
        }

        // Main thread: wait for a signal; in follow mode, take over sampling
        // as soon as the other sampling process exits.
        while (!g_stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (g_stop) break;  // never take over while shutting down
            if (mode == "db" && cfg.source == "auto") {
                static int tick = 0;
                if (++tick % 10 == 0) {
                    if (auto l = pipeline::SamplerLock::try_acquire(cfg.db_path)) {
                        follower->stop();
                        follower.reset();
                        lock = std::move(l);
                        log::info("the other sampling process stopped; sampling here from now on");
                        start_local();
                        bus.publish(msg::topic::kSensorsChanged, msg::SensorsChanged{0});
                    }
                }
            }
        }

        log::info("shutting down");
        hub.close_all();  // ends live streams so the HTTP workers can finish
        svr.stop();
        if (http.joinable()) http.join();
        if (sampler) sampler->stop();
        if (follower) follower->stop();
        if (writer) writer->stop();  // flushes buffered readings
        engine.stop();
        live.stop();
        if (mqtt) mqtt->stop();
        log::info("shut down");
    } catch (const std::exception& e) {
        log::error("fatal: ", e.what());
        return 1;
    }
    return 0;
}

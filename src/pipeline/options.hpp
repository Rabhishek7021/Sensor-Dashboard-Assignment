#pragma once
/**
 * @file options.hpp
 * @brief Command-line options shared by sensor-server and sensor-poller.
 *
 * Covers what to sample, how often, how long to keep data, and the optional MQTT bridge.
 */

#include <cstdlib>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "../sensors/provider.hpp"
#include "db_writer.hpp"
#include "mqtt_bridge.hpp"
#include "sampler.hpp"

namespace sd::pipeline {

/// @brief Settings shared by sensor-server and sensor-poller (sampling, storage, MQTT).
struct PipelineConfig {
    std::string providers = "cpu_temp";  ///< Comma-separated provider names, or "all".
    sensors::ProviderContext ctx;  ///< sysfs roots and CPU chip names.
    SamplerOptions sampler;     ///< interval 500 ms, rediscover 60 s
    WriterOptions writer;       ///< flush 1 s, raw 24 h, rollups 7 days
    std::string mqtt_url;       ///< empty = no MQTT bridge
    ///@{
    std::string mqtt_prefix, mqtt_user, mqtt_password;  ///< MQTT topic prefix and credentials.
    ///@}
};

/// @brief Splits "a,b,c" into its non-empty items.
inline std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) if (!item.empty()) out.push_back(item);
    return out;
}

/// @brief Help text for the shared flags.
inline const char* pipeline_usage() {
    return "Sampling:\n"
           "  --providers a,b|all       sensor families (default cpu_temp; see sensor-poller --list-providers)\n"
           "  --interval-ms N           sampling period in milliseconds (default 500, min 50)\n"
           "  --interval SECONDS        same, in seconds (e.g. 0.5)\n"
           "  --rediscover SECONDS      re-run discovery this often, 0 = never (default 60)\n"
           "  --hwmon-root PATH         default /sys/class/hwmon\n"
           "  --power-supply-root PATH  default /sys/class/power_supply\n"
           "  --thermal-root PATH       default /sys/class/thermal\n"
           "  --chips a,b,c             CPU hwmon drivers (default coretemp,k10temp,zenpower)\n"
           "Storage:\n"
           "  --flush-ms N              write buffered readings every N ms (default 1000)\n"
           "  --raw-retention-hours N   keep raw readings N hours, 0 = forever (default 24)\n"
           "  --retention-days N        keep 10 s rollups N days, 0 = forever (default 7)\n"
           "MQTT bridge (optional, needs a build with libmosquitto):\n"
           "  --mqtt HOST[:PORT]        publish readings and alarm changes to this broker\n"
           "  --mqtt-prefix TOPIC       topic prefix (default sensordash/<hostname>)\n"
           "  --mqtt-user U / --mqtt-password P\n"
           "Environment: SENSOR_PROVIDERS, POLL_INTERVAL_MS, HWMON_ROOT, POWER_SUPPLY_ROOT, THERMAL_ROOT, MQTT_URL\n";
}

/// @brief Applies the environment variables (SENSOR_PROVIDERS, POLL_INTERVAL_MS, ...) to `c`.
inline void apply_pipeline_env(PipelineConfig& c) {
    if (const char* v = std::getenv("SENSOR_PROVIDERS")) c.providers = v;
    if (const char* v = std::getenv("POLL_INTERVAL_MS")) c.sampler.interval = std::chrono::milliseconds(std::atol(v));
    if (const char* v = std::getenv("POLL_INTERVAL_S"))
        c.sampler.interval = std::chrono::milliseconds(static_cast<long>(std::atof(v) * 1000));
    if (const char* v = std::getenv("HWMON_ROOT")) c.ctx.hwmon_root = v;
    if (const char* v = std::getenv("POWER_SUPPLY_ROOT")) c.ctx.power_supply_root = v;
    if (const char* v = std::getenv("THERMAL_ROOT")) c.ctx.thermal_root = v;
    if (const char* v = std::getenv("MQTT_URL")) c.mqtt_url = v;
}

/// Handles one shared flag. `next` returns the flag's value. Returns false if
/// the flag is not a pipeline flag.
inline bool parse_pipeline_flag(const std::string& a, const std::function<std::string()>& next, PipelineConfig& c) {
    using std::chrono::milliseconds;
    if (a == "--providers") c.providers = next();
    else if (a == "--interval-ms") c.sampler.interval = milliseconds(std::atol(next().c_str()));
    else if (a == "--interval") c.sampler.interval = milliseconds(static_cast<long>(std::atof(next().c_str()) * 1000));
    else if (a == "--rediscover") c.sampler.rediscover = milliseconds(static_cast<long>(std::atof(next().c_str()) * 1000));
    else if (a == "--hwmon-root") c.ctx.hwmon_root = next();
    else if (a == "--power-supply-root") c.ctx.power_supply_root = next();
    else if (a == "--thermal-root") c.ctx.thermal_root = next();
    else if (a == "--chips") c.ctx.cpu_chips = split_csv(next());
    else if (a == "--flush-ms") c.writer.flush_interval = milliseconds(std::atol(next().c_str()));
    else if (a == "--raw-retention-hours") c.writer.raw_retention = std::chrono::hours(std::atol(next().c_str()));
    else if (a == "--retention-days") c.writer.rollup_retention = std::chrono::hours(24 * std::atol(next().c_str()));
    else if (a == "--mqtt") c.mqtt_url = next();
    else if (a == "--mqtt-prefix") c.mqtt_prefix = next();
    else if (a == "--mqtt-user") c.mqtt_user = next();
    else if (a == "--mqtt-password") c.mqtt_password = next();
    else return false;
    return true;
}

/// Returns an error message, or empty if the configuration is valid.
inline std::string validate(const PipelineConfig& c) {
    if (c.sampler.interval.count() < 50) return "--interval-ms must be >= 50";
    if (c.writer.flush_interval.count() < 50) return "--flush-ms must be >= 50";
    if (!c.mqtt_url.empty() && !MqttBridge::available())
        return "--mqtt given, but this build has no MQTT support (install libmosquitto-dev and rebuild)";
    return {};
}

/// @brief Builds the MQTT bridge options from the parsed configuration.
inline MqttOptions mqtt_options(const PipelineConfig& c) {
    auto o = parse_mqtt_url(c.mqtt_url);
    o.prefix = c.mqtt_prefix;
    o.username = c.mqtt_user;
    o.password = c.mqtt_password;
    return o;
}

}  // namespace sd::pipeline

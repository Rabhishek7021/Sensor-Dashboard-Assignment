#pragma once
/**
 * @file sensor.hpp
 * @brief Core abstraction of the sensor layer.
 *
 * A Sensor is one measurable value on the machine (one CPU core's
 * temperature, one fan's speed, one battery's charge). Everything above this
 * layer (poller, database, API, dashboard) works only with `SensorInfo` and
 * `Sensor::read()`, so it never needs to know where a value comes from.
 *
 * Sensors are created by SensorProviders (see provider.hpp).
 */

#include <optional>
#include <string>

/// @brief Sensor layer: sensors, providers, registry and manager (library @c sd_sensors).
namespace sd::sensors {

/// Well-known categories and units. Plain strings (not enums) so a new
/// provider can introduce its own without touching shared code; these
/// constants just keep the built-in ones consistent.
namespace category {
inline constexpr const char* kTemperature = "temperature";  ///< Temperatures (CPU cores, dies, packages).
inline constexpr const char* kFan = "fan";                  ///< Fan speeds.
inline constexpr const char* kBattery = "battery";          ///< Battery charge levels.
}  // namespace category

/// Well-known measurement units used by the built-in providers.
namespace unit {
inline constexpr const char* kCelsius = "celsius";  ///< Degrees Celsius.
inline constexpr const char* kRpm = "rpm";          ///< Revolutions per minute.
inline constexpr const char* kPercent = "percent";  ///< Percent (0-100).
}  // namespace unit

/// Static description of a sensor. Persisted in the `sensors` table.
struct SensorInfo {
    std::string key;       ///< stable unique identity, e.g. "coretemp/Core 4" (survives reboots)
    std::string source;    ///< device / driver it comes from, e.g. "coretemp", "BAT0"
    std::string label;     ///< human label, e.g. "Core 4", "Fan 1", "BAT0 charge"
    std::string category;  ///< groups sensors in the UI: temperature | fan | battery | ...
    std::string kind;      ///< finer type inside a category: core | die | cpu | fan | charge ...
    std::string unit;      ///< celsius | rpm | percent | ...
    int sort_order = 0;    ///< order within the provider (the manager adds a provider offset)
};

/// Abstract sensor. Implementations must be cheap to read and must never
/// throw from read(): an unreadable value is reported as std::nullopt.
class Sensor {
public:
    /// @brief Creates a sensor with its static description.
    explicit Sensor(SensorInfo info) : info_(std::move(info)) {}
    virtual ~Sensor() = default;
    Sensor(const Sensor&) = delete;
    Sensor& operator=(const Sensor&) = delete;

    /// @brief Static description of this sensor.
    const SensorInfo& info() const { return info_; }
    /// @brief Mutable description (the manager adjusts sort order after discovery).
    SensorInfo& mutable_info() { return info_; }

    /// Name of the provider that found this sensor (set by SensorManager);
    /// the sampler gives each provider its own thread.
    const std::string& provider() const { return provider_; }
    /// @brief Records which provider found this sensor.
    void set_provider(std::string p) { provider_ = std::move(p); }

    /// Current value in `info().unit`, or nullopt if it cannot be read now.
    virtual std::optional<double> read() const = 0;

    /// Where the value is read from, for diagnostics (e.g. a sysfs path).
    virtual std::string origin() const { return {}; }

private:
    SensorInfo info_;       ///< Static description.
    std::string provider_;  ///< Name of the provider that found it.
};

/// The common case on Linux: a sysfs file holding one integer, multiplied by
/// a scale factor (hwmon temperatures are millidegrees -> scale 0.001).
class SysfsValueSensor : public Sensor {
public:
    /// @brief Creates a sensor reading `path` and multiplying the integer by `scale`.
    SysfsValueSensor(SensorInfo info, std::string path, double scale = 1.0)
        : Sensor(std::move(info)), path_(std::move(path)), scale_(scale) {}
    std::optional<double> read() const override;
    std::string origin() const override { return path_; }

private:
    std::string path_;  ///< sysfs file holding the raw integer.
    double scale_;      ///< Factor applied to the raw value.
};

}  // namespace sd::sensors

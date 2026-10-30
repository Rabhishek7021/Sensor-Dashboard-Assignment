#pragma once
/**
 * @file provider.hpp
 * @brief SensorProvider: the extension point of the sensor layer (Strategy pattern).
 *
 * Each provider knows how to find one family of sensors on a Linux machine
 * (CPU core temperatures, fans, batteries, ...) and returns them as Sensor
 * objects. Providers run discovery at start-up, so nothing about the machine
 * (number of cores, fans, batteries) is ever hard-coded.
 *
 * To add a new sensor family:
 *   1. Implement SensorProvider in src/sensors/providers/\<name\>.{hpp,cpp}.
 *   2. Register it with one line in src/sensors/registry.cpp.
 *   3. Add the .cpp to CMakeLists.txt.
 * Nothing in the poller, database, API or dashboard has to change.
 */

#include <memory>
#include <string>
#include <vector>

#include "sensor.hpp"

namespace sd::sensors {

/// Where providers look. Overridable so tests and the fake-sensor script can
/// point them at a simulated sysfs tree.
struct ProviderContext {
    std::string hwmon_root = "/sys/class/hwmon";                ///< hwmon class directory
    std::string power_supply_root = "/sys/class/power_supply";  ///< power_supply class directory (batteries)
    std::string thermal_root = "/sys/class/thermal";            ///< thermal zones (CPU fallback)
    /// hwmon drivers that report CPU temperatures (Intel, AMD, AMD alt driver).
    std::vector<std::string> cpu_chips = {"coretemp", "k10temp", "zenpower"};
};

/// @brief Interface of a sensor family discoverer (Strategy pattern); see the file description.
class SensorProvider {
public:
    virtual ~SensorProvider() = default;

    /// Short id used on the command line (--providers) and in logs.
    virtual std::string name() const = 0;

    /// Scans the machine and returns every sensor of this family found.
    /// Must not throw for a missing/unreadable device: an empty result simply
    /// means "this machine has none".
    virtual std::vector<std::unique_ptr<Sensor>> discover() = 0;
};

}  // namespace sd::sensors

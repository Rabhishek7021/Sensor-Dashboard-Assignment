#pragma once
/**
 * @file hwmon.hpp
 * @brief Generic enumeration of the Linux hwmon subsystem (/sys/class/hwmon).
 *
 * hwmon exposes every hardware monitor chip as hwmonN/ with a `name` file and
 * numbered channels such as temp3_input / temp3_label or fan1_input. This
 * helper lists devices and channels; the providers that use it (cpu_temp,
 * fan, ...) only decide which channels they care about.
 */

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

/// @brief Enumeration of Linux hwmon devices and channels.
namespace sd::hwmon {

/// @brief One hwmon device (a hardware monitor chip), e.g. /sys/class/hwmon/hwmon3.
struct Device {
    std::filesystem::path dir;  ///< e.g. /sys/class/hwmon/hwmon3
    std::string chip;           ///< driver name from `name`, e.g. "coretemp"
    std::string device_id;      ///< stable id of the underlying device, e.g. "coretemp.0"
    int rank = 0;               ///< 0,1,... among devices with the same chip (e.g. CPU sockets)
    bool multi = false;         ///< true if more than one device has this chip name

    /// Stable key prefix. The hwmonN index is never used because the kernel
    /// may renumber it at boot; the device id is added only when needed to
    /// keep keys unique (multi-socket machines).
    std::string key_prefix() const { return multi ? chip + "." + device_id : chip; }
};

/// @brief One numbered channel of a device, e.g. temp3_input with its temp3_label.
struct Channel {
    int index = 0;                ///< N in tempN_input
    std::string label;            ///< tempN_label, or "\<type\>\<N\>" if absent
    bool has_label = false;       ///< true if a tempN_label file exists
    std::filesystem::path input;  ///< absolute path of \<type\>N_input
};

/// All hwmon devices under `root` (sorted, deterministic). If `accept` is
/// given, only devices whose chip name it accepts are returned.
std::vector<Device> scan(const std::string& root,
                         const std::function<bool(const std::string& chip)>& accept = {});

/// Channels of one type ("temp", "fan", "in", "power", ...) on a device,
/// sorted by channel index.
std::vector<Channel> channels(const Device& dev, const std::string& type);

/// Chip names present under `root` (diagnostics when discovery finds nothing).
std::vector<std::string> chip_names(const std::string& root);

}  // namespace sd::hwmon

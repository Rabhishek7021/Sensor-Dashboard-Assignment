/**
 * @file hwmon.cpp
 * @brief Implementation of hwmon device and channel enumeration.
 */
#include "hwmon.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <regex>
#include <system_error>

#include "sysfs.hpp"

namespace fs = std::filesystem;

namespace sd::hwmon {

namespace {

/// @brief Stable id of the device behind an hwmonN directory (its `device` link target),
/// or the hwmonN name for virtual devices without a parent.
std::string device_id_of(const fs::path& hwmon_dir) {
    std::error_code ec;
    fs::path dev = fs::canonical(hwmon_dir / "device", ec);
    if (!ec) return dev.filename().string();
    return hwmon_dir.filename().string();  // virtual devices without a parent
}

}  // namespace

/// Devices are sorted by chip name and device id so discovery order is deterministic.
std::vector<Device> scan(const std::string& root, const std::function<bool(const std::string&)>& accept) {
    std::vector<Device> out;
    std::map<std::string, std::vector<std::string>> ids_per_chip;
    for (const auto& dir : sysfs::list_dir(root)) {
        auto chip = sysfs::read_line(dir / "name");
        if (!chip || chip->empty()) continue;
        if (accept && !accept(*chip)) continue;
        Device d;
        d.dir = dir;
        d.chip = *chip;
        d.device_id = device_id_of(dir);
        auto& ids = ids_per_chip[d.chip];
        auto it = std::find(ids.begin(), ids.end(), d.device_id);
        if (it == ids.end()) {
            ids.push_back(d.device_id);
            it = ids.end() - 1;
        }
        d.rank = static_cast<int>(it - ids.begin());
        out.push_back(std::move(d));
    }
    for (auto& d : out) d.multi = ids_per_chip[d.chip].size() > 1;
    return out;
}

std::vector<Channel> channels(const Device& dev, const std::string& type) {
    const std::regex input_re("^" + type + R"((\d+)_input$)");
    std::vector<Channel> out;
    for (const auto& f : sysfs::list_dir(dev.dir)) {
        const std::string fname = f.filename().string();
        std::smatch m;
        if (!std::regex_match(fname, m, input_re)) continue;
        Channel c;
        c.index = std::atoi(m[1].str().c_str());
        c.input = f;
        auto lbl = sysfs::read_line(dev.dir / (type + std::to_string(c.index) + "_label"));
        c.has_label = lbl && !lbl->empty();
        c.label = c.has_label ? *lbl : type + std::to_string(c.index);
        out.push_back(std::move(c));
    }
    std::sort(out.begin(), out.end(), [](const Channel& a, const Channel& b) { return a.index < b.index; });
    return out;
}

std::vector<std::string> chip_names(const std::string& root) {
    std::vector<std::string> names;
    for (const auto& dir : sysfs::list_dir(root))
        if (auto n = sysfs::read_line(dir / "name")) names.push_back(*n);
    return names;
}

}  // namespace sd::hwmon

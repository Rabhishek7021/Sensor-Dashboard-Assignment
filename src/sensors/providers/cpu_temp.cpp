/**
 * @file cpu_temp.cpp
 * @brief CPU temperature discovery with per-core / per-die / whole-CPU fallback tiers.
 */
#include "cpu_temp.hpp"

#include <algorithm>
#include <cctype>

#include "../../common/log.hpp"
#include "../hwmon.hpp"
#include "../sysfs.hpp"

namespace sd::sensors {

namespace {

/// @brief Detail level of a CPU temperature channel; lower is more detailed.
enum class Tier { Core = 0, Die = 1, Cpu = 2, Ignore = 3 };

/// @brief Tier of an hwmon label ("Core N", "Tccd N", "Package id N"/"Tctl"/"Tdie", else Ignore).
Tier classify(const std::string& label) {
    if (label.rfind("Core ", 0) == 0) return Tier::Core;
    if (label.rfind("Tccd", 0) == 0) return Tier::Die;
    if (label.rfind("Package id", 0) == 0 || label == "Tctl" || label == "Tdie") return Tier::Cpu;
    return Tier::Ignore;
}

/// @brief Sensor kind stored for a tier: "core", "die" or "cpu".
const char* kind_of(Tier t) {
    switch (t) {
        case Tier::Core: return "core";
        case Tier::Die: return "die";
        default: return "cpu";
    }
}

/// @brief ASCII lower-case copy of a string.
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

}  // namespace

/// Uses the most detailed tier any CPU chip exposes; falls back to thermal zones if none.
std::vector<std::unique_ptr<Sensor>> CpuTempProvider::discover() {
    const auto& chips = ctx_.cpu_chips;
    auto devices = hwmon::scan(ctx_.hwmon_root, [&](const std::string& chip) {
        return std::find(chips.begin(), chips.end(), chip) != chips.end();
    });

    std::vector<std::unique_ptr<Sensor>> tiers[3];
    for (const auto& dev : devices) {
        for (const auto& ch : hwmon::channels(dev, "temp")) {
            const Tier tier = classify(ch.label);
            if (tier == Tier::Ignore) continue;
            SensorInfo info;
            info.key = dev.key_prefix() + "/" + ch.label;
            info.source = dev.chip;
            info.label = (dev.multi ? "CPU" + std::to_string(dev.rank) + " " : "") + ch.label;
            info.category = category::kTemperature;
            info.kind = kind_of(tier);
            info.unit = unit::kCelsius;
            // Socket first, then the number in the label ("Core 12" -> 12).
            info.sort_order = dev.rank * 100000 + sysfs::trailing_number(ch.label, ch.index);
            tiers[static_cast<int>(tier)].push_back(
                std::make_unique<SysfsValueSensor>(std::move(info), ch.input.string(), 0.001));
        }
    }

    // Order by socket, then core/die number: Core 0, Core 4, Core 8, ...
    for (auto& t : tiers)
        std::stable_sort(t.begin(), t.end(),
                         [](const auto& a, const auto& b) { return a->info().sort_order < b->info().sort_order; });

    if (!tiers[0].empty()) return std::move(tiers[0]);
    if (!tiers[1].empty()) {
        log::info("cpu_temp: this CPU exposes no per-core sensors; using ", tiers[1].size(), " per-die sensor(s)");
        return std::move(tiers[1]);
    }
    if (!tiers[2].empty()) {
        log::info("cpu_temp: this CPU exposes no per-core sensors; using whole-CPU temperature");
        return std::move(tiers[2]);
    }
    auto zones = discover_thermal_zones();
    if (!zones.empty())
        log::info("cpu_temp: no CPU hwmon driver found; using ", zones.size(), " CPU thermal zone(s)");
    return zones;
}

std::vector<std::unique_ptr<Sensor>> CpuTempProvider::discover_thermal_zones() const {
    std::vector<std::unique_ptr<Sensor>> out;
    for (const auto& zone : sysfs::list_dir(ctx_.thermal_root)) {
        const std::string zname = zone.filename().string();
        if (zname.rfind("thermal_zone", 0) != 0) continue;
        auto type = sysfs::read_line(zone / "type");
        if (!type) continue;
        const std::string t = lower(*type);
        if (t.find("cpu") == std::string::npos && t.find("soc") == std::string::npos &&
            t != "x86_pkg_temp")
            continue;
        const int idx = sysfs::trailing_number(zname, 0);
        SensorInfo info;
        info.key = "thermal/" + *type + "#" + std::to_string(idx);
        info.source = "thermal";
        info.label = *type;
        info.category = category::kTemperature;
        info.kind = "cpu";
        info.unit = unit::kCelsius;
        info.sort_order = idx;
        out.push_back(std::make_unique<SysfsValueSensor>(std::move(info), (zone / "temp").string(), 0.001));
    }
    return out;
}

}  // namespace sd::sensors

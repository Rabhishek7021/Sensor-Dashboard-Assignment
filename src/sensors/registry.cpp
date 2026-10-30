/**
 * @file registry.cpp
 * @brief ProviderRegistry implementation and the list of built-in providers.
 */
#include "registry.hpp"

#include <algorithm>

#include "providers/battery.hpp"
#include "providers/cpu_temp.hpp"
#include "providers/fan.hpp"

namespace sd::sensors {

void ProviderRegistry::add(std::string name, std::string description, Factory factory) {
    if (contains(name)) throw std::logic_error("provider registered twice: " + name);
    entries_.push_back({std::move(name), std::move(description), std::move(factory)});
}

bool ProviderRegistry::contains(const std::string& name) const {
    return std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.name == name; });
}

int ProviderRegistry::rank(const std::string& name) const {
    for (size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].name == name) return static_cast<int>(i);
    throw UnknownProvider("unknown sensor provider '" + name + "'");
}

std::unique_ptr<SensorProvider> ProviderRegistry::create(const std::string& name,
                                                         const ProviderContext& ctx) const {
    return entries_[rank(name)].factory(ctx);
}

const ProviderRegistry& builtin_providers() {
    static const ProviderRegistry registry = [] {
        ProviderRegistry r;
        // One line per sensor family. Add new providers here.
        r.add("cpu_temp", "CPU temperature per core (hwmon coretemp/k10temp/zenpower, thermal zones fallback)",
              [](const ProviderContext& c) { return std::make_unique<CpuTempProvider>(c); });
        r.add("fan", "Fan speeds in RPM (hwmon fan*_input)",
              [](const ProviderContext& c) { return std::make_unique<FanProvider>(c); });
        r.add("battery", "Battery charge in percent (/sys/class/power_supply)",
              [](const ProviderContext& c) { return std::make_unique<BatteryProvider>(c); });
        return r;
    }();
    return registry;
}

}  // namespace sd::sensors

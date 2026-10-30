/**
 * @file manager.cpp
 * @brief SensorManager implementation: runs every provider and merges their sensors into one ordered list.
 */
#include "manager.hpp"

#include <algorithm>
#include <set>
#include <sstream>

#include "../common/log.hpp"

namespace sd::sensors {

namespace {
/// Sort orders from different providers are kept apart by this offset so
/// families never interleave (all CPU cores, then all fans, then batteries).
constexpr int kProviderStride = 10'000'000;
}  // namespace

SensorManager::SensorManager(const ProviderRegistry& registry, const std::vector<std::string>& enabled,
                             const ProviderContext& ctx) {
    std::set<std::string> seen;
    for (const auto& name : enabled) {
        if (!seen.insert(name).second) continue;  // ignore duplicates
        ranks_.push_back(registry.rank(name));    // throws UnknownProvider
        names_.push_back(name);
        providers_.push_back(registry.create(name, ctx));
    }
}

Discovery SensorManager::discover() {
    Discovery out;
    std::set<std::string> keys;
    for (size_t i = 0; i < providers_.size(); ++i) {
        std::vector<std::unique_ptr<Sensor>> found;
        try {
            found = providers_[i]->discover();
        } catch (const std::exception& e) {
            // One broken provider must not take the others down.
            log::error("provider '", names_[i], "' failed during discovery: ", e.what());
        }
        size_t added = 0;
        for (auto& s : found) {
            if (!keys.insert(s->info().key).second) {
                log::warn("duplicate sensor key '", s->info().key, "' from provider '", names_[i], "'; skipped");
                continue;
            }
            auto& info = s->mutable_info();
            info.sort_order = ranks_[i] * kProviderStride + std::min(std::max(info.sort_order, 0), kProviderStride - 1);
            s->set_provider(names_[i]);
            out.sensors.push_back(std::move(s));
            ++added;
        }
        out.per_provider.emplace_back(names_[i], added);
    }
    std::stable_sort(out.sensors.begin(), out.sensors.end(),
                     [](const auto& a, const auto& b) { return a->info().sort_order < b->info().sort_order; });
    return out;
}

std::vector<std::string> parse_provider_list(const std::string& csv, const ProviderRegistry& registry) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item.erase(0, item.find_first_not_of(" \t"));
        item.erase(item.find_last_not_of(" \t") + 1);
        if (item.empty()) continue;
        if (item == "all") {
            for (const auto& e : registry.entries()) out.push_back(e.name);
        } else {
            out.push_back(item);
        }
    }
    return out;
}

}  // namespace sd::sensors

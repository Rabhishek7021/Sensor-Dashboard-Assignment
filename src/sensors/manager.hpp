#pragma once
/**
 * @file manager.hpp
 * @brief SensorManager: single entry point (Facade) over the enabled providers.
 *
 * The poller tells it which providers to use; it builds them through the
 * registry, runs discovery on all of them, and returns one ordered list of
 * sensors with globally unique keys. Callers never deal with individual
 * providers.
 */

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "provider.hpp"
#include "registry.hpp"

namespace sd::sensors {

/// @brief Result of SensorManager::discover().
struct Discovery {
    std::vector<std::unique_ptr<Sensor>> sensors;              ///< ordered for display
    std::vector<std::pair<std::string, size_t>> per_provider;  ///< provider name -> sensors found
};

/// @brief Facade over the enabled providers; see the file description.
class SensorManager {
public:
    /// Throws UnknownProvider if a name in `enabled` is not registered.
    SensorManager(const ProviderRegistry& registry, const std::vector<std::string>& enabled,
                  const ProviderContext& ctx);

    /// Runs discovery on every enabled provider. Safe to call again later to
    /// pick up hardware that appeared or disappeared (e.g. a battery).
    Discovery discover();

    /// @brief Names of the enabled providers, in registry order.
    const std::vector<std::string>& enabled() const { return names_; }

private:
    std::vector<std::string> names_;                         ///< Enabled provider names.
    std::vector<int> ranks_;                                 ///< Registry rank of each provider (sort offset).
    std::vector<std::unique_ptr<SensorProvider>> providers_; ///< Provider instances.
};

/// Parses "cpu_temp,fan" or "all" into provider names (validated later by
/// the SensorManager constructor).
std::vector<std::string> parse_provider_list(const std::string& csv, const ProviderRegistry& registry);

}  // namespace sd::sensors

#pragma once
/**
 * @file registry.hpp
 * @brief ProviderRegistry: maps provider names to factories (Factory + Registry pattern).
 *
 * The poller asks the registry for the providers enabled on the command line instead of
 * constructing concrete classes itself, so adding a provider never touches the poller.
 */

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "provider.hpp"

namespace sd::sensors {

/// @brief Thrown when a provider name is not registered.
struct UnknownProvider : std::invalid_argument {
    using std::invalid_argument::invalid_argument;
};

/// @brief Maps provider names to factories; see the file description.
class ProviderRegistry {
public:
    /// @brief Creates a provider for a given sysfs context.
    using Factory = std::function<std::unique_ptr<SensorProvider>(const ProviderContext&)>;

    /// @brief One registered provider.
    struct Entry {
        std::string name;         ///< Name used with --providers, e.g. "fan".
        std::string description;  ///< One-line description for --list-providers.
        Factory factory;          ///< Creates the provider.
    };

    /// Registers a provider. Registration order is also the display order of
    /// the sensor families (e.g. CPU temperatures before fans).
    void add(std::string name, std::string description, Factory factory);

    /// @brief True if a provider with this name is registered.
    bool contains(const std::string& name) const;
    /// @brief Builds the named provider.
    /// @throws UnknownProvider if the name is not registered.
    std::unique_ptr<SensorProvider> create(const std::string& name, const ProviderContext& ctx) const;
    /// Position of a provider in registration order (throws if unknown).
    int rank(const std::string& name) const;
    /// @brief All registered providers, in registration order.
    const std::vector<Entry>& entries() const { return entries_; }

private:
    std::vector<Entry> entries_;  ///< Registered providers, in order.
};

/// Registry with every provider that ships with this project.
const ProviderRegistry& builtin_providers();

}  // namespace sd::sensors

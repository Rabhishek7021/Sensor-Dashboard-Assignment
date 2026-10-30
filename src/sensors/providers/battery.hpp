#pragma once
/**
 * @file battery.hpp
 * @brief Battery provider: charge level (percent) of every system battery found in
 *        /sys/class/power_supply.
 *
 * Peripheral batteries (wireless mice, keyboards; scope == "Device") are ignored. Desktops simply
 * yield no sensors.
 */

#include "../provider.hpp"

namespace sd::sensors {

/// @brief Discovers battery charge sensors; see the file description.
class BatteryProvider : public SensorProvider {
public:
    /// @brief Creates the provider with the sysfs roots to scan.
    explicit BatteryProvider(ProviderContext ctx) : ctx_(std::move(ctx)) {}
    std::string name() const override { return "battery"; }
    std::vector<std::unique_ptr<Sensor>> discover() override;

private:
    /// Where to look (sysfs roots, CPU chip names).
    ProviderContext ctx_;
};

}  // namespace sd::sensors

#pragma once
/**
 * @file fan.hpp
 * @brief Fan speed provider: every hwmon fan<N>_input on the machine, in RPM.
 *
 * Works with any hwmon driver that reports fans (thinkpad, dell_smm, asus-ec, nct6775, it87, ...).
 * Laptops whose firmware hides fan speed simply yield no sensors.
 */

#include "../provider.hpp"

namespace sd::sensors {

/// @brief Discovers fan speed sensors; see the file description.
class FanProvider : public SensorProvider {
public:
    /// @brief Creates the provider with the sysfs roots to scan.
    explicit FanProvider(ProviderContext ctx) : ctx_(std::move(ctx)) {}
    std::string name() const override { return "fan"; }
    std::vector<std::unique_ptr<Sensor>> discover() override;

private:
    /// Where to look (sysfs roots, CPU chip names).
    ProviderContext ctx_;
};

}  // namespace sd::sensors

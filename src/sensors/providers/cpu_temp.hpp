#pragma once
/**
 * @file cpu_temp.hpp
 * @brief CPU temperature provider.
 *
 * Prefers one sensor per physical core. CPUs differ in what they expose, so
 * discovery falls back in tiers and uses the most detailed tier available:
 *
 *   1. per core     hwmon "Core N" labels       (Intel coretemp)
 *   2. per die      hwmon "Tccd N" labels       (AMD Ryzen/EPYC: k10temp, zenpower)
 *   3. whole CPU    hwmon "Package id N"/"Tctl"/"Tdie", then thermal zones
 *                   whose type mentions cpu/soc (ARM boards, some VMs)
 *
 * Core labels are kept as the kernel reports them: the number in "Core N" is
 * the physical core id, so it can be non-contiguous (0, 4, 8, ..., 24-27 on
 * hybrid Intel CPUs). Cores are ordered by that number.
 */

#include "../provider.hpp"

namespace sd::sensors {

/// @brief Discovers CPU temperature sensors; see the file description.
class CpuTempProvider : public SensorProvider {
public:
    /// @brief Creates the provider with the sysfs roots to scan.
    explicit CpuTempProvider(ProviderContext ctx) : ctx_(std::move(ctx)) {}
    std::string name() const override { return "cpu_temp"; }
    std::vector<std::unique_ptr<Sensor>> discover() override;

private:
    /// @brief Last-resort fallback: thermal zones whose type mentions cpu or soc (ARM boards, some VMs).
    std::vector<std::unique_ptr<Sensor>> discover_thermal_zones() const;
    /// Where to look (sysfs roots, CPU chip names).
    ProviderContext ctx_;
};

}  // namespace sd::sensors

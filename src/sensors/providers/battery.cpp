/**
 * @file battery.cpp
 * @brief Battery charge discovery from /sys/class/power_supply.
 */
#include "battery.hpp"

#include <filesystem>

#include "../sysfs.hpp"

namespace fs = std::filesystem;

namespace sd::sensors {

namespace {

/// Some batteries report no `capacity` file, only now/full counters
/// (energy_* in µWh or charge_* in µAh). Percent = now / full * 100.
class RatioPercentSensor : public Sensor {
public:
    /// @brief Creates a sensor computing now / full * 100.
    RatioPercentSensor(SensorInfo info, fs::path now, fs::path full)
        : Sensor(std::move(info)), now_(std::move(now)), full_(std::move(full)) {}
    std::optional<double> read() const override {
        auto n = sysfs::read_int(now_);
        auto f = sysfs::read_int(full_);
        if (!n || !f || *f <= 0) return std::nullopt;
        return static_cast<double>(*n) * 100.0 / static_cast<double>(*f);
    }
    std::string origin() const override { return now_.string() + " / " + full_.string(); }

private:
    ///@{
    fs::path now_, full_;  ///< energy_now/charge_now and energy_full/charge_full files.
    ///@}
};

}  // namespace

std::vector<std::unique_ptr<Sensor>> BatteryProvider::discover() {
    std::vector<std::unique_ptr<Sensor>> out;
    int order = 0;
    for (const auto& dir : sysfs::list_dir(ctx_.power_supply_root)) {
        if (sysfs::read_line(dir / "type").value_or("") != "Battery") continue;
        if (sysfs::read_line(dir / "scope").value_or("") == "Device") continue;  // peripheral

        const std::string name = dir.filename().string();  // e.g. BAT0
        SensorInfo info;
        info.key = "battery:" + name + "/charge";
        info.source = name;
        info.label = name + " charge";
        info.category = category::kBattery;
        info.kind = "charge";
        info.unit = unit::kPercent;
        info.sort_order = order++;

        if (fs::exists(dir / "capacity")) {
            out.push_back(std::make_unique<SysfsValueSensor>(std::move(info), (dir / "capacity").string(), 1.0));
        } else if (fs::exists(dir / "energy_now") && fs::exists(dir / "energy_full")) {
            out.push_back(std::make_unique<RatioPercentSensor>(std::move(info), dir / "energy_now", dir / "energy_full"));
        } else if (fs::exists(dir / "charge_now") && fs::exists(dir / "charge_full")) {
            out.push_back(std::make_unique<RatioPercentSensor>(std::move(info), dir / "charge_now", dir / "charge_full"));
        }
    }
    return out;
}

}  // namespace sd::sensors

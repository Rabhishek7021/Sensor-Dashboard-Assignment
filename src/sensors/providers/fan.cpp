/**
 * @file fan.cpp
 * @brief Fan speed discovery from hwmon fan\<N\>_input channels.
 */
#include "fan.hpp"

#include "../hwmon.hpp"

namespace sd::sensors {

std::vector<std::unique_ptr<Sensor>> FanProvider::discover() {
    struct Found {
        const hwmon::Device* dev;
        hwmon::Channel ch;
        int order;
    };
    const auto devices = hwmon::scan(ctx_.hwmon_root);
    std::vector<Found> found;
    int dev_order = 0;
    size_t devices_with_fans = 0;
    for (const auto& dev : devices) {
        auto chans = hwmon::channels(dev, "fan");
        if (chans.empty()) continue;
        ++devices_with_fans;
        for (auto& ch : chans) {
            const int order = dev_order * 1000 + ch.index;
            found.push_back({&dev, std::move(ch), order});
        }
        ++dev_order;
    }

    std::vector<std::unique_ptr<Sensor>> out;
    for (auto& f : found) {
        SensorInfo info;
        info.key = "fan:" + f.dev->key_prefix() + "/fan" + std::to_string(f.ch.index);
        info.source = f.dev->chip;
        info.label = f.ch.has_label ? f.ch.label : "Fan " + std::to_string(f.ch.index);
        // Name the chip only when fans come from several chips.
        if (devices_with_fans > 1) info.label += " (" + f.dev->chip + ")";
        info.category = category::kFan;
        info.kind = "fan";
        info.unit = unit::kRpm;
        info.sort_order = f.order;
        out.push_back(std::make_unique<SysfsValueSensor>(std::move(info), f.ch.input.string(), 1.0));
    }
    return out;
}

}  // namespace sd::sensors

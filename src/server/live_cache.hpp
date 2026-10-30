#pragma once
/**
 * @file live_cache.hpp
 * @brief LiveCache: latest value of every sensor and the recent sample rate, kept in memory by a
 *        bus subscriber thread.
 *
 * /api/health and /api/sensors answer from here instead of querying the readings table.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>

#include "../bus/bus.hpp"
#include "../pipeline/messages.hpp"

namespace sd::server {

/// @brief Latest value of every sensor and a samples-per-minute counter, fed by the bus.
class LiveCache {
public:
    /// @brief Newest reading of one sensor.
    struct Latest {
        int64_t ts;  ///< Sample time (epoch ms).
        double value;  ///< Value in the sensor's unit.
    };

    /// @brief Subscribes to "samples/#"; call start() to run.
    explicit LiveCache(bus::Bus& bus) : bus_(bus) {
        sub_ = bus_.subscribe("live-cache", {msg::topic::kSamplesAll}, 4096);
    }
    /// @brief Stops the thread.
    ~LiveCache() { stop(); }

    /// @brief Starts the thread that folds samples into the cache.
    void start() {
        thread_ = std::thread([this] {
            while (!sub_->closed()) {
                for (const auto& m : sub_->drain(std::chrono::milliseconds(500))) {
                    const auto* b = bus::payload_as<msg::SampleBatch>(*m);
                    if (!b) continue;
                    std::lock_guard lock(mu_);
                    for (const auto& s : b->samples) {
                        auto& l = latest_[s.sensor_id];
                        if (b->ts >= l.ts) l = {b->ts, s.value};
                    }
                    last_ts_ = std::max(last_ts_, b->ts);
                    auto& slot = per_second_[static_cast<size_t>((b->ts / 1000) % 60)];
                    if (slot.first != b->ts / 1000) slot = {b->ts / 1000, 0};
                    slot.second += static_cast<int64_t>(b->samples.size());
                }
            }
        });
    }
    /// @brief Stops the thread and unsubscribes.
    void stop() {
        if (!thread_.joinable()) return;
        sub_->close();
        thread_.join();
        bus_.unsubscribe(sub_);
    }

    /// @brief Copy of the newest value per sensor id.
    std::map<int64_t, Latest> latest() const {
        std::lock_guard lock(mu_);
        return latest_;
    }
    /// @brief Newest sample time of any sensor (0 before the first sample).
    int64_t last_ts() const {
        std::lock_guard lock(mu_);
        return last_ts_;
    }
    /// @brief Samples received in the 60 seconds before `now_ms` (for /api/health).
    int64_t samples_last_minute(int64_t now_ms) const {
        std::lock_guard lock(mu_);
        int64_t n = 0;
        for (const auto& [sec, count] : per_second_)
            if (sec > now_ms / 1000 - 60) n += count;
        return n;
    }

private:
    bus::Bus& bus_;  ///< Bus the samples come from.
    std::shared_ptr<bus::Subscription> sub_;  ///< Subscription to "samples/#".
    std::thread thread_;  ///< Cache thread.
    mutable std::mutex mu_;  ///< Guards the fields below.
    std::map<int64_t, Latest> latest_;  ///< Newest value per sensor id.
    int64_t last_ts_ = 0;  ///< Newest sample time of any sensor.
    ///@{
    std::array<std::pair<int64_t, int64_t>, 60> per_second_{};  ///< Ring of (second, sample count) for the last minute.
    ///@}
};

}  // namespace sd::server

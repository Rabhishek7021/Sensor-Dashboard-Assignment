#pragma once
/**
 * @file sampler.hpp
 * @brief Sampler: reads every sensor on a fixed interval and publishes the values.
 *
 * Threads:
 *   * one sampling thread per provider (cpu_temp, fan, battery, ...). A slow
 *     device, such as a battery behind an ACPI embedded controller, can
 *     never delay the CPU core readings.
 *   * one supervisor thread that re-runs discovery periodically and swaps
 *     the sensor set when hardware appears or disappears.
 *
 * All sampling threads tick on the same clock grid (timestamps are exact
 * multiples of the interval), so readings taken at the same moment carry
 * the same timestamp in every provider, the database and the dashboard.
 * Ticks are scheduled against absolute deadlines, so the interval does not
 * drift; a tick that overruns is skipped rather than queued.
 *
 * Output: one msg::SampleBatch per provider per tick on topic
 * "samples/<provider>". The sampler never touches the readings table: storage
 * is just another bus subscriber (DbWriter).
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../bus/bus.hpp"
#include "../common/db.hpp"
#include "../sensors/manager.hpp"

namespace sd::pipeline {

/// @brief Configuration of the Sampler.
struct SamplerOptions {
    std::chrono::milliseconds interval{500};  ///< Sampling period.
    std::chrono::milliseconds rediscover{60000};  ///< 0 = never
};

/// @brief Sampling threads plus a rediscovery supervisor; see the file description.
class Sampler {
public:
    /// @brief Creates the sampler; call start() to discover sensors and launch the threads.
    Sampler(sensors::SensorManager& manager, db::Database& registry, bus::Bus& bus, SamplerOptions opts);
    /// @brief Stops all threads.
    ~Sampler();

    /// Discovers sensors, registers them in the database and starts the
    /// threads. Returns the number of sensors (0 = nothing to sample; no
    /// threads are started).
    size_t start();
    /// @brief Stops the sampling and supervisor threads and joins them.
    void stop();

    /// "13 sensor(s): cpu_temp=10 fan=2 battery=1 [Core 0, ...]"
    std::string summary() const;

    /// @brief Counters for /api/stats.
    struct Stats {
        uint64_t ticks = 0;          ///< provider ticks completed
        uint64_t samples = 0;        ///< values published
        uint64_t read_failures = 0;  ///< reads that returned no value
        uint64_t skipped_ticks = 0;  ///< ticks missed because a read overran
        double last_tick_us = 0;     ///< read time of the latest tick (all providers)
        double max_tick_us = 0;  ///< slowest tick so far
        size_t sensors = 0;  ///< sensors being sampled
        size_t threads = 0;  ///< sampling threads running
    };
    /// @brief Current counters.
    Stats stats() const;

private:
    struct Group;  ///< the sensors of one provider and their thread
    /// @brief Registers a discovery result in the database and makes it the current sensor set.
    void activate(sensors::Discovery d);
    /// @brief Starts one sampling thread per provider group.
    void start_groups();
    /// @brief Stops and joins the sampling threads.
    void stop_groups();
    /// @brief Sampling loop of one provider: read on aligned ticks, publish a SampleBatch.
    void run_group(Group& g);
    /// @brief Supervisor loop: rediscover periodically and swap the sensor set if it changed.
    void supervise();

    sensors::SensorManager& manager_;  ///< Runs discovery.
    db::Database& registry_;  ///< Where discovered sensors are registered (ids).
    bus::Bus& bus_;  ///< Where samples are published.
    SamplerOptions opts_;  ///< Configuration.

    mutable std::mutex mu_;  ///< guards discovery_, groups_, summary_
    sensors::Discovery discovery_;  ///< Current sensor set.
    std::vector<std::unique_ptr<Group>> groups_;  ///< One group (and thread) per provider.
    std::string summary_;  ///< Text returned by summary().

    std::atomic<bool> stop_{false};  ///< Set by stop().
    std::atomic<bool> groups_stop_{false};  ///< Stops the current sampling threads (rediscovery swap).
    std::mutex wait_mu_;  ///< Used with wait_cv_.
    std::condition_variable wait_cv_;  ///< Wakes the supervisor early on stop().
    std::thread supervisor_;  ///< Rediscovery thread.

    ///@{
    std::atomic<uint64_t> ticks_{0}, samples_{0}, failures_{0}, skipped_{0};  ///< Counters behind Stats.
    ///@}
    ///@{
    std::atomic<double> last_tick_us_{0}, max_tick_us_{0};  ///< Tick timings behind Stats.
    ///@}
};

}  // namespace sd::pipeline

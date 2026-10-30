#pragma once
/**
 * @file source.hpp
 * @brief Where live samples come from.
 *
 * Exactly one process may sample sensors into a given database, otherwise
 * every reading would be stored twice. SamplerLock enforces this with an
 * exclusive flock() on `<db>.sampler.lock` (released automatically if the
 * process dies, so a crash never leaves a stale lock).
 *
 * When another process (a standalone sensor-poller) holds the lock, the
 * server does not sample. Instead DbFollower reads the rows that process
 * writes and republishes them on the bus as "samples/db", so the alarm
 * engine, live dashboard stream and MQTT bridge work the same in both setups.
 */

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "../bus/bus.hpp"
#include "../common/db.hpp"

namespace sd::pipeline {

/// @brief Exclusive "one sampler per database" lock (flock on "\<db\>.sampler.lock").
class SamplerLock {
public:
    /// Returns the lock if no other process holds it, nullptr otherwise.
    static std::unique_ptr<SamplerLock> try_acquire(const std::string& db_path);
    /// PID written by the current holder, if any.
    static std::optional<long> holder(const std::string& db_path);
    /// @brief Releases the lock.
    ~SamplerLock();
    SamplerLock(const SamplerLock&) = delete;
    SamplerLock& operator=(const SamplerLock&) = delete;

private:
    /// @brief Wraps an already locked file descriptor (see try_acquire()).
    explicit SamplerLock(int fd) : fd_(fd) {}
    int fd_;  ///< Open, flock()ed lock file.
};

/// @brief Republishes rows written by another sampling process as "samples/db" batches.
class DbFollower {
public:
    /// @brief Creates the follower; `poll` is the read interval.
    DbFollower(db::Database& db, bus::Bus& bus, std::chrono::milliseconds poll = std::chrono::milliseconds(500));
    /// @brief Stops the thread.
    ~DbFollower();
    /// @brief Starts the follower thread (history older than about 2 s is not replayed).
    void start();
    /// @brief Stops and joins the follower thread.
    void stop();
    /// @brief Number of readings republished.
    uint64_t published() const { return published_; }

private:
    /// @brief Thread body: poll new rows per sensor and publish them, grouped by timestamp.
    void run();
    db::Database& db_;  ///< Database written by the other process.
    bus::Bus& bus_;  ///< Where rows are republished.
    std::chrono::milliseconds poll_;  ///< How often new rows are read.
    std::thread thread_;  ///< Follower thread.
    std::atomic<bool> stop_{false};  ///< Set by stop().
    std::atomic<uint64_t> published_{0};  ///< Rows republished so far.
};

}  // namespace sd::pipeline

#pragma once
/**
 * @file db_writer.hpp
 * @brief DbWriter: persistent storage as a bus subscriber, on its own thread.
 *
 * Subscribes to "samples/#" and
 *   * buffers readings in memory and writes them in one transaction every
 *     `flush_interval` (default 1 s) or as soon as `max_rows` are pending.
 *     At 500 ms sampling this replaces 2 commits per second per provider
 *     with 1 commit per second in total, and the sampling threads never
 *     wait on the disk;
 *   * maintains the 10-second rollup table (every 10 s, only closed buckets);
 *   * prunes raw readings older than `raw_retention` and rollups older than
 *     `rollup_retention` (hourly; rollups are made before raw data is pruned).
 *
 * stop() drains the queue and flushes, so a clean shutdown loses nothing.
 */

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "../bus/bus.hpp"
#include "../common/db.hpp"

/// @brief Threads around the bus: sampling, storage, DB following, MQTT (library @c sd_pipeline).
namespace sd::pipeline {

/// @brief Configuration of the DbWriter.
struct WriterOptions {
    std::chrono::milliseconds flush_interval{1000};  ///< Commit buffered readings this often.
    size_t max_rows = 20000;  ///< Flush early when this many readings are pending.
    std::chrono::hours raw_retention{24};  ///< Raw readings are kept this long (0 = forever).
    std::chrono::hours rollup_retention{24 * 7};  ///< 0 = keep forever
    std::chrono::milliseconds rollup_interval{10000};  ///< Roll up closed 10 s buckets this often.
    std::chrono::milliseconds maintenance_interval{3600 * 1000};  ///< Run retention pruning this often.
};

/// @brief Bus subscriber that stores samples, maintains rollups and applies retention.
class DbWriter {
public:
    /// @brief Creates the writer; call start() to launch its thread.
    DbWriter(db::Database& db, bus::Bus& bus, WriterOptions opts = {});
    /// @brief Stops the thread (flushing pending readings).
    ~DbWriter();
    /// @brief Subscribes to "samples/#" and starts the writer thread.
    void start();
    /// @brief Drains the queue, flushes, writes final rollups and joins the thread.
    void stop();

    /// @brief Counters for /api/stats.
    struct Stats {
        ///@{
        uint64_t rows = 0, flushes = 0, rollup_buckets = 0, pruned = 0, errors = 0;  ///< Rows written, commits, rollup buckets written, rows pruned, failed operations.
        ///@}
        ///@{
        double last_flush_ms = 0, max_flush_ms = 0;  ///< Duration of the latest / slowest commit.
        ///@}
        size_t pending = 0;  ///< Readings buffered but not yet committed.
        uint64_t dropped = 0;  ///< queue overflow (should stay 0)
    };
    /// @brief Current counters.
    Stats stats() const;
    /// Newest timestamp written to the database (0 before the first flush).
    int64_t flushed_until() const { return flushed_until_; }

private:
    /// @brief Thread body: drain, buffer, flush, roll up, maintain.
    void run();
    /// @brief Writes pending_ in one transaction.
    void flush();
    /// @brief Rolls up closed 10 s buckets (all buckets on the final pass).
    void rollup(bool final_pass);
    /// @brief Prunes raw readings and rollups past their retention.
    void maintain();

    db::Database& db_;  ///< Database written to.
    bus::Bus& bus_;  ///< Bus the samples come from.
    WriterOptions opts_;  ///< Configuration.
    std::shared_ptr<bus::Subscription> sub_;  ///< Subscription to "samples/#".
    std::thread thread_;  ///< Writer thread.
    std::atomic<bool> stop_{false};  ///< Set by stop().

    std::vector<db::Reading> pending_;  ///< Readings waiting for the next flush.
    int64_t max_flushed_ts_ = 0;  ///< Newest timestamp committed so far.
    int64_t rolled_until_ = 0;  ///< exclusive end of rolled-up data
    int64_t last_touch_ = 0;  ///< Last time sensors.last_seen was refreshed.

    ///@{
    std::atomic<uint64_t> rows_{0}, flushes_{0}, buckets_{0}, pruned_{0}, errors_{0};  ///< Counters behind Stats.
    ///@}
    ///@{
    std::atomic<double> last_flush_ms_{0}, max_flush_ms_{0};  ///< Flush timings behind Stats.
    ///@}
    std::atomic<size_t> pending_count_{0};  ///< Size of pending_, readable from other threads.
    std::atomic<int64_t> flushed_until_{0};  ///< Value returned by flushed_until().
};

}  // namespace sd::pipeline

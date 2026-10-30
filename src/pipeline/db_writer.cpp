/**
 * @file db_writer.cpp
 * @brief DbWriter implementation: batched commits, rollups and retention on the writer thread.
 */
#include "db_writer.hpp"

#include <algorithm>

#include "../common/log.hpp"
#include "../common/time_util.hpp"
#include "messages.hpp"

namespace sd::pipeline {

using Clock = std::chrono::steady_clock;  ///< Monotonic clock for flush and maintenance timers.

DbWriter::DbWriter(db::Database& db, bus::Bus& bus, WriterOptions opts) : db_(db), bus_(bus), opts_(opts) {
    // Large queue: the writer may pause for seconds (disk busy, big prune)
    // without losing samples; at 13 sensors this is hours of headroom.
    sub_ = bus_.subscribe("db-writer", {msg::topic::kSamplesAll}, 1 << 16);
}

DbWriter::~DbWriter() { stop(); }

void DbWriter::start() {
    stop_ = false;
    thread_ = std::thread([this] { run(); });
}

void DbWriter::stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    sub_->close();
    thread_.join();
    bus_.unsubscribe(sub_);
}

/// Commits every flush_interval or when max_rows are pending; rollups every rollup_interval,
/// pruning every maintenance_interval.
void DbWriter::run() {
    try {
        rolled_until_ = db_.rollup_watermark().value_or(0);
        if (rolled_until_ == 0) rolled_until_ = db_.oldest_raw_ts().value_or(time::now_ms()) / db::kRollupMs * db::kRollupMs;
    } catch (const std::exception& e) {
        log::error("db writer: cannot read rollup state: ", e.what());
        rolled_until_ = time::now_ms() / db::kRollupMs * db::kRollupMs;
    }
    // Catch up on rollups for data written by earlier runs, then prune.
    rollup(false);
    maintain();

    auto next_flush = Clock::now() + opts_.flush_interval;
    auto next_rollup = Clock::now() + opts_.rollup_interval;
    auto next_maint = Clock::now() + opts_.maintenance_interval;

    while (true) {
        const auto deadline = std::min({next_flush, next_rollup, next_maint});
        auto msgs = sub_->drain_until(deadline);
        for (const auto& m : msgs) {
            if (const auto* b = bus::payload_as<msg::SampleBatch>(*m)) {
                for (const auto& s : b->samples) pending_.push_back({s.sensor_id, b->ts, s.value});
            }
        }
        pending_count_ = pending_.size();
        const bool stopping = stop_ || sub_->closed();
        const auto now = Clock::now();
        if (pending_.size() >= opts_.max_rows || now >= next_flush || stopping) {
            flush();
            next_flush = now + opts_.flush_interval;
        }
        if (now >= next_rollup || stopping) {
            rollup(stopping);
            next_rollup = now + opts_.rollup_interval;
        }
        if (now >= next_maint) {
            maintain();
            next_maint = now + opts_.maintenance_interval;
        }
        if (stopping) {
            // Final drain: anything published between close() and here.
            for (const auto& m : sub_->drain(std::chrono::milliseconds(0)))
                if (const auto* b = bus::payload_as<msg::SampleBatch>(*m))
                    for (const auto& s : b->samples) pending_.push_back({s.sensor_id, b->ts, s.value});
            flush();
            break;
        }
    }
}

void DbWriter::flush() {
    if (pending_.empty()) return;
    const auto t0 = Clock::now();
    try {
        db_.insert_readings(pending_);
        int64_t max_ts = 0;
        for (const auto& r : pending_) max_ts = std::max(max_ts, r.ts);
        max_flushed_ts_ = std::max(max_flushed_ts_, max_ts);
        flushed_until_ = max_flushed_ts_;
        rows_ += pending_.size();
        ++flushes_;
        pending_.clear();
        pending_count_ = 0;
        // last_seen only needs minute precision; avoid rewriting the sensors
        // table on every flush.
        if (max_flushed_ts_ - last_touch_ >= 60000) {
            db_.touch_sensors(max_flushed_ts_);
            last_touch_ = max_flushed_ts_;
        }
    } catch (const std::exception& e) {
        // Keep the rows and retry on the next flush (e.g. database locked).
        ++errors_;
        log::error("db writer: flush of ", pending_.size(), " rows failed: ", e.what());
        if (pending_.size() > opts_.max_rows * 10) {
            log::error("db writer: dropping ", pending_.size(), " buffered rows");
            pending_.clear();
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    last_flush_ms_ = ms;
    if (ms > max_flush_ms_) max_flush_ms_ = ms;
}

void DbWriter::rollup(bool final_pass) {
    // Only buckets that are closed and fully flushed. Samples of one tick
    // from different providers can arrive a moment apart, hence the margin.
    const int64_t limit = final_pass ? max_flushed_ts_ + 1 : max_flushed_ts_ - 2000;
    int64_t target = limit / db::kRollupMs * db::kRollupMs;
    if (max_flushed_ts_ == 0) {
        // Nothing written by this run yet: roll up what earlier runs left
        // (every bucket that ended more than 10 s ago).
        target = std::max(target, time::now_ms() / db::kRollupMs * db::kRollupMs - db::kRollupMs);
    }
    try {
        // Chunks of one hour keep each write transaction short.
        while (rolled_until_ < target) {
            const int64_t to = std::min(target, rolled_until_ + 3600 * 1000);
            buckets_ += db_.rollup(rolled_until_, to);
            rolled_until_ = to;
        }
    } catch (const std::exception& e) {
        ++errors_;
        log::error("db writer: rollup failed: ", e.what());
    }
}

void DbWriter::maintain() {
    const int64_t now = time::now_ms();
    try {
        // Never prune raw data that has not been rolled up yet.
        const int64_t raw_cut = std::min<int64_t>(
            now - std::chrono::duration_cast<std::chrono::milliseconds>(opts_.raw_retention).count(), rolled_until_);
        int64_t n = 0;
        if (opts_.raw_retention.count() > 0) n += db_.prune_before(raw_cut);
        if (opts_.rollup_retention.count() > 0)
            n += db_.prune_rollups_before(
                now - std::chrono::duration_cast<std::chrono::milliseconds>(opts_.rollup_retention).count());
        pruned_ += n;
        if (n > 0) log::info("retention: pruned ", n, " old rows");
    } catch (const std::exception& e) {
        ++errors_;
        log::warn("retention prune failed: ", e.what());
    }
}

DbWriter::Stats DbWriter::stats() const {
    Stats s;
    s.rows = rows_;
    s.flushes = flushes_;
    s.rollup_buckets = buckets_;
    s.pruned = pruned_;
    s.errors = errors_;
    s.last_flush_ms = last_flush_ms_;
    s.max_flush_ms = max_flush_ms_;
    s.pending = pending_count_;
    s.dropped = sub_->dropped();
    return s;
}

}  // namespace sd::pipeline

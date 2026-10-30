#pragma once
/**
 * @file alarm_window.hpp
 * @brief Sliding-window statistics for the alarm engine, kept in memory.
 *
 * SecondWindow stores one aggregate (count, sum, sum of squares, min, max)
 * per second for the last `horizon_s` seconds in a ring buffer. Adding a
 * sample is O(1); the aggregate over any window up to the horizon is one
 * pass over at most `window` seconds (600 slots for a 10-minute alarm), so
 * evaluating an alarm every second costs microseconds and never touches the
 * database. Memory: 48 bytes x 3600 s = ~170 KB per sensor.
 *
 * Window semantics: samples with timestamps in (now - window, now], at
 * one-second granularity.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "db.hpp"

/// @brief Alarm evaluation: pure logic and in-memory sliding windows.
namespace sd::alarm {

/// @brief Running count / sum / sum of squares / min / max that can merge partial aggregates.
struct Accumulator {
    int64_t n = 0;  ///< Number of samples.
    ///@{
    double sum = 0, sumsq = 0;  ///< Sum and sum of squares of the samples.
    ///@}
    double min = std::numeric_limits<double>::infinity();   ///< Smallest sample.
    double max = -std::numeric_limits<double>::infinity();  ///< Largest sample.

    /// @brief Merges a partial aggregate of `cn` samples (ignored if cn <= 0).
    void add(int64_t cn, double csum, double csumsq, double cmin, double cmax) {
        if (cn <= 0) return;
        n += cn;
        sum += csum;
        sumsq += csumsq;
        min = std::min(min, cmin);
        max = std::max(max, cmax);
    }
    /// @brief Converts to count / avg / min / max / population stddev.
    db::Aggregate finish() const {
        db::Aggregate a;
        a.count = n;
        if (n > 0) {
            a.avg = sum / static_cast<double>(n);
            a.min = min;
            a.max = max;
            const double var = sumsq / static_cast<double>(n) - a.avg * a.avg;  // population variance
            a.stddev = std::sqrt(var > 0 ? var : 0.0);
        }
        return a;
    }
};

/// @brief Ring buffer of per-second aggregates for one sensor; see the file description.
class SecondWindow {
public:
    /// @brief Creates a window covering the last `horizon_s` seconds (at least 1).
    explicit SecondWindow(int horizon_s = 3600) : ring_(static_cast<size_t>(std::max(horizon_s, 1))) {}

    /// @brief Number of seconds kept.
    int horizon_s() const { return static_cast<int>(ring_.size()); }

    /// @brief Adds one sample taken at `ts_ms`.
    void add(int64_t ts_ms, double v) { add_bucket(ts_ms / 1000, 1, v, v * v, v, v); }

    /// @brief Adds a pre-aggregated second (warm-up from the database). A slot holding an
    /// older second is reset first.
    void add_bucket(int64_t sec, int64_t n, double sum, double sumsq, double min, double max) {
        Slot& s = ring_[static_cast<size_t>(sec % static_cast<int64_t>(ring_.size()))];
        if (s.sec != sec) {
            s = Slot{};
            s.sec = sec;
        }
        s.acc.add(n, sum, sumsq, min, max);
    }

    /// Aggregate of samples in (now_ms - window_ms, now_ms], at most horizon.
    db::Aggregate aggregate(int64_t now_ms, int64_t window_ms) const {
        const int64_t hi = now_ms / 1000;
        int64_t lo = (now_ms - window_ms) / 1000 + 1;
        lo = std::max(lo, hi - static_cast<int64_t>(ring_.size()) + 1);
        Accumulator acc;
        for (int64_t sec = lo; sec <= hi; ++sec) {
            const Slot& s = ring_[static_cast<size_t>(sec % static_cast<int64_t>(ring_.size()))];
            if (s.sec == sec) acc.add(s.acc.n, s.acc.sum, s.acc.sumsq, s.acc.min, s.acc.max);
        }
        return acc.finish();
    }

private:
    /// @brief Aggregate of one second.
    struct Slot {
        int64_t sec = std::numeric_limits<int64_t>::min();  ///< Epoch second this slot holds (min = empty).
        Accumulator acc;                                     ///< Samples of that second.
    };
    std::vector<Slot> ring_;  ///< One slot per second, indexed by sec % size.
};

}  // namespace sd::alarm

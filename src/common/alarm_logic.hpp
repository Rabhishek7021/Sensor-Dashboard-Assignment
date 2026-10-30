#pragma once
/**
 * @file alarm_logic.hpp
 * @brief Pure alarm-evaluation logic, kept free of I/O so it can be unit tested.
 *
 * Specification: an alarm fires when
 *     aggregate(sensor_value, last X minutes) > Y     (op ">", the default)
 * or  aggregate(sensor_value, last X minutes) < Y     (op "<", e.g. battery charge below 20 %)
 * where aggregate is one of avg | max | min | stddev.
 */

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

#include "db.hpp"

namespace sd::alarm {

/// @brief True for a supported comparison: ">" (fires above the threshold) or "<" (fires below it).
inline bool is_valid_op(const std::string& op) { return op == ">" || op == "<"; }

/// @brief True if `value` breaches `threshold` under `op` (strict comparison; unknown ops never fire).
inline bool breached(const std::string& op, double value, double threshold) {
    if (op == "<") return value < threshold;
    if (op == ">") return value > threshold;
    return false;
}

/// @name Input limits for new or edited alarms (API and dashboard enforce the same values)
///@{

inline constexpr int kMinWindowMin = 1;    ///< Shortest alarm window X, in minutes.
inline constexpr int kMaxWindowMin = 100;  ///< Longest alarm window X, in minutes.

/// @brief True if `minutes` is an accepted alarm window (kMinWindowMin to kMaxWindowMin).
inline bool is_valid_window(int64_t minutes) { return minutes >= kMinWindowMin && minutes <= kMaxWindowMin; }

/// @brief Inclusive range of accepted thresholds for one unit.
struct ThresholdRange {
    double min;          ///< Lowest accepted threshold.
    double max;          ///< Highest accepted threshold.
    const char* suffix;  ///< Unit as shown in error messages, e.g. "°C".
};

/// @brief Accepted threshold range for a sensor unit: celsius -50 to 100 °C, rpm 0 to 5000 RPM,
///        percent 0 to 100 %; nullopt for other units (any finite number).
inline std::optional<ThresholdRange> threshold_range(const std::string& unit) {
    if (unit == "celsius") return ThresholdRange{-50, 100, "°C"};
    if (unit == "rpm") return ThresholdRange{0, 5000, "RPM"};
    if (unit == "percent") return ThresholdRange{0, 100, "%"};
    return std::nullopt;
}

/// @brief True if `threshold` is finite and inside threshold_range(unit) (bounds included).
inline bool is_valid_threshold(const std::string& unit, double threshold) {
    if (!std::isfinite(threshold)) return false;
    const auto r = threshold_range(unit);
    return !r || (threshold >= r->min && threshold <= r->max);
}
///@}

/// @brief True for a supported aggregate function: avg, max, min or stddev.
inline bool is_valid_func(const std::string& f) {
    return f == "avg" || f == "max" || f == "min" || f == "stddev";
}

/// @brief Outcome of one alarm evaluation.
struct Result {
    std::string state;            ///< ok | firing | no_data
    std::optional<double> value;  ///< the computed aggregate
};

/// @brief Selects the value of `func` from an aggregate; nullopt if empty or unknown.
inline std::optional<double> pick(const std::string& func, const db::Aggregate& a) {
    if (a.count == 0) return std::nullopt;
    if (func == "avg") return a.avg;
    if (func == "max") return a.max;
    if (func == "min") return a.min;
    if (func == "stddev") return a.stddev;
    return std::nullopt;
}

/// Evaluates `func(window) op threshold`, where `op` is ">" (default) or "<".
/// `min_samples` guards against firing on a near-empty window (e.g. right
/// after the poller starts). stddev additionally needs at least 2 samples.
inline Result evaluate(const std::string& func, const db::Aggregate& agg, double threshold,
                       int min_samples = 1, const std::string& op = ">") {
    const int64_t needed = func == "stddev" ? std::max(min_samples, 2) : min_samples;
    if (agg.count < needed) return {"no_data", pick(func, agg)};
    auto v = pick(func, agg);
    if (!v) return {"no_data", std::nullopt};
    return {breached(op, *v, threshold) ? "firing" : "ok", v};
}

}  // namespace sd::alarm

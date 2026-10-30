#pragma once
/**
 * @file messages.hpp
 * @brief Topics and payload types exchanged on the bus.
 *
 * @verbatim
 *   samples/<source>          SampleBatch       one tick of one provider (or "db" when following a DB)
 *   sensors/changed           SensorsChanged    the sensor set was (re)registered
 *   alarms/<id>/state         AlarmTransition   an alarm changed state (ok -> firing, ...)
 *   alarms/snapshot           AlarmSnapshot     live state of every alarm, after each evaluation
 *   control/alarms/reload     ReloadRequest     alarm definitions changed (API -> alarm engine)
 * @endverbatim
 */

#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../common/db.hpp"

/// @brief Bus topics and message payloads.
namespace sd::msg {

/// @brief Topic names and builders.
namespace topic {
/// @brief "samples/\<source\>": one tick of one provider.
inline std::string samples(const std::string& source) { return "samples/" + source; }
inline constexpr const char* kSamplesAll = "samples/#";  ///< Filter for samples of every source.
inline constexpr const char* kSensorsChanged = "sensors/changed";  ///< Sensor set changed.
/// @brief "alarms/\<id\>/state": state transition of one alarm.
inline std::string alarm_state(int64_t id) { return "alarms/" + std::to_string(id) + "/state"; }
inline constexpr const char* kAlarmStateAll = "alarms/+/state";  ///< Filter for every alarm transition.
inline constexpr const char* kAlarmSnapshot = "alarms/snapshot";  ///< Snapshot of all alarms.
inline constexpr const char* kAlarmReload = "control/alarms/reload";  ///< Request to reload alarm definitions.
}  // namespace topic

/// @brief One sensor value within a SampleBatch.
struct Sample {
    int64_t sensor_id;  ///< Database id of the sensor.
    double value;  ///< Value in the sensor's unit.
};

/// All samples one provider read at one tick. Every provider ticks on the
/// same aligned clock, so batches from different providers share `ts`.
struct SampleBatch {
    int64_t ts = 0;  ///< epoch ms UTC, aligned to the sampling interval
    std::string source;  ///< Provider name, or "db" when following a database.
    std::vector<Sample> samples;  ///< Values read at this tick.
};

/// @brief Payload of "sensors/changed": the sensor set was (re)registered.
struct SensorsChanged {
    size_t count = 0;  ///< Number of sensors now registered.
};

/// @brief Payload of "alarms/\<id\>/state": an alarm changed state.
struct AlarmTransition {
    db::Alarm alarm;            ///< definition and new state
    std::string from_state;  ///< Previous state.
    std::string sensor_label;  ///< Label of the alarm's sensor.
    int64_t ts = 0;  ///< Time of the transition (epoch ms).
};

/// @brief Live state of one alarm (part of AlarmSnapshot).
struct AlarmLive {
    int64_t id = 0;  ///< Alarm id.
    std::string state;  ///< ok | firing | no_data | disabled
    std::optional<double> last_value;  ///< Aggregate value at the last evaluation.
    ///@{
    std::optional<int64_t> last_eval_ts, last_change_ts;  ///< Last evaluation / last state change (epoch ms).
    ///@}
    int64_t samples = 0;  ///< samples in the window at the last evaluation
};

/// @brief Payload of "alarms/snapshot": every alarm after one evaluation round.
struct AlarmSnapshot {
    int64_t ts = 0;  ///< Evaluation time (epoch ms).
    std::vector<AlarmLive> alarms;  ///< State of every alarm.
};

/// Asks the alarm engine to reload definitions and evaluate at once; the
/// promise is fulfilled when done, so the API can return the fresh state.
struct ReloadRequest {
    std::shared_ptr<std::promise<void>> done;  ///< Fulfilled after the reload and evaluation (may be null).
};

}  // namespace sd::msg

#pragma once
/**
 * @file alarm_engine.hpp
 * @brief Alarm engine: a bus subscriber with its own thread.
 *
 * @verbatim
   in:  samples/#              -> per-sensor in-memory windows (SecondWindow)
        control/alarms/reload  -> reload definitions from the DB, evaluate now
        sensors/changed        -> refresh sensor labels
   out: alarms/<id>/state      -> on every state transition (ok <-> firing, ...)
        alarms/snapshot        -> live state of all alarms after each evaluation
   @endverbatim
 *
 * Every `eval_interval` (default 1 s) each enabled alarm is evaluated:
 *   func(values of its own sensor over the last X minutes) > Y   (or < Y for "below" alarms)
 * Windows up to `memory_horizon` (60 min) come from memory. Longer windows
 * read the 10-second rollups from the database (every `long_eval_interval`,
 * default 10 s). On start the memory windows are filled from the database,
 * so a restart does not reset alarms to no_data.
 *
 * Persistence: transitions (with their fired/cleared event) are written at
 * once; the latest aggregate values every `persist_interval` in one batch.
 * The API reads live state from snapshot(), so it is always current.
 */

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "../bus/bus.hpp"
#include "../common/alarm_window.hpp"
#include "../common/db.hpp"
#include "../pipeline/messages.hpp"

/// @brief Server-side components: alarm engine and live value cache.
namespace sd::server {

/// @brief Configuration of the AlarmEngine.
struct AlarmOptions {
    std::chrono::milliseconds eval_interval{1000};  ///< How often every alarm is evaluated.
    int min_samples = 3;  ///< Samples needed in a window before an alarm can fire.
    std::chrono::milliseconds persist_interval{10000};  ///< How often latest values are written to the DB.
    std::chrono::milliseconds long_eval_interval{10000};  ///< Evaluation period of windows longer than the memory horizon.
    int memory_horizon_s = 3600;  ///< Longest window kept in memory (seconds).
};

/// @brief Evaluates every alarm against in-memory windows and publishes state changes; see the file description.
class AlarmEngine {
public:
    /// @brief Creates the engine; call start() to warm up and run.
    AlarmEngine(db::Database& db, bus::Bus& bus, AlarmOptions opts = {});
    /// @brief Stops the engine thread.
    ~AlarmEngine();
    /// @brief Warms windows from the DB, loads alarms and starts the engine thread.
    void start();
    /// @brief Stops and joins the engine thread (latest values are persisted).
    void stop();

    /// Reloads alarm definitions and evaluates them now. Blocks until done
    /// (or timeout); returns false on timeout.
    bool reload(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));

    /// Live state of every alarm, keyed by id (thread-safe).
    std::map<int64_t, msg::AlarmLive> snapshot() const;

    /// @brief Counters for /api/stats.
    struct Stats {
        ///@{
        uint64_t evaluations = 0, transitions = 0, samples = 0, dropped = 0;  ///< Evaluations, state transitions, samples ingested, bus messages dropped.
        ///@}
        ///@{
        double last_eval_us = 0, max_eval_us = 0;  ///< Duration of the latest / slowest evaluation round (µs).
        ///@}
        ///@{
        size_t alarms = 0, sensors_tracked = 0;  ///< Alarms loaded / sensors with an in-memory window.
        ///@}
    };
    /// @brief Current counters.
    Stats stats() const;

private:
    /// @brief An alarm plus its in-memory evaluation state.
    struct Runtime {
        db::Alarm def;  ///< Definition and current state.
        std::optional<double> value;  ///< Aggregate at the last evaluation.
        ///@{
        std::optional<int64_t> eval_ts, change_ts;  ///< Last evaluation / last state change (epoch ms).
        ///@}
        int64_t samples = 0;  ///< Samples in the window at the last evaluation.
        int64_t next_long_eval = 0;  ///< Next evaluation time for long windows (from the DB).
    };

    /// @brief Thread body: ingest bus messages, evaluate every eval_interval, persist periodically.
    void run();
    /// @brief Fills the memory windows from the database so a restart keeps alarm state.
    void warm_up();
    /// @brief Adds a batch of samples to the per-sensor windows.
    void ingest(const msg::SampleBatch& b);
    /// @brief Reloads alarm definitions from the database.
    void load_alarms();
    /// @brief Reloads sensor labels from the database.
    void load_labels();
    /// @brief Evaluates the alarms (all of them if `force`), recording and publishing transitions.
    void evaluate(int64_t now, bool force);
    /// @brief Aggregate over a window longer than the memory horizon (rollups + newest raw data).
    db::Aggregate long_window(int64_t sensor_id, int64_t now, int64_t window_ms);
    /// @brief Writes the latest aggregate of every alarm in one transaction.
    void persist_values();

    db::Database& db_;  ///< Alarm definitions, events and long-window data.
    bus::Bus& bus_;  ///< Input samples and output transitions / snapshots.
    AlarmOptions opts_;  ///< Configuration.
    std::shared_ptr<bus::Subscription> sub_;  ///< Subscription to samples, reload requests and sensor changes.
    std::thread thread_;  ///< Engine thread.
    std::atomic<bool> stop_{false};  ///< Set by stop().

    /// Engine-thread state.
    std::unordered_map<int64_t, alarm::SecondWindow> windows_;
    std::unordered_map<int64_t, int64_t> warm_until_;  ///< Per sensor: newest second loaded from the DB at warm-up.
    std::vector<Runtime> alarms_;  ///< Loaded alarms.
    std::map<int64_t, std::string> labels_;  ///< Sensor id -> label (for transition messages).
    bool reload_pending_ = false;  ///< Definitions must be reloaded before the next evaluation.

    mutable std::mutex snap_mu_;  ///< Guards snapshot_.
    std::map<int64_t, msg::AlarmLive> snapshot_;  ///< Live state returned by snapshot().

    ///@{
    std::atomic<uint64_t> evals_{0}, transitions_{0}, samples_{0};  ///< Counters behind Stats.
    ///@}
    ///@{
    std::atomic<double> last_eval_us_{0}, max_eval_us_{0};  ///< Evaluation timings behind Stats.
    ///@}
    ///@{
    std::atomic<size_t> alarm_count_{0}, tracked_{0};  ///< Sizes behind Stats.
    ///@}
};

}  // namespace sd::server

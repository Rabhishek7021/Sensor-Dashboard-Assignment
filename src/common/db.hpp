#pragma once
/**
 * @file db.hpp
 * @brief SQLite persistence layer shared by the poller and the API server.
 *
 * Each process opens its own connection. WAL journal mode lets the poller
 * write while the server reads. Within one process, all access is
 * serialised by a mutex (the HTTP server is multi-threaded).
 *
 * Tables: `sensors`, `readings` (raw samples), `readings_10s` (10 s rollups),
 * `alarms` and `alarm_events`.
 */

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../sensors/sensor.hpp"

struct sqlite3;

/// @brief SQLite storage (library @c sd_common).
namespace sd::db {

/// @brief Error raised by any failed SQLite operation.
struct DbError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// @brief A row of the `sensors` table, plus its latest reading.
struct Sensor {
    int64_t id = 0;  ///< Database id (stable across restarts).
    ///@{
    std::string key, chip, label, unit, kind, category;  ///< Sensor description, see sensors::SensorInfo (`chip` = SensorInfo::source).
    ///@}
    int sort_order = 0;  ///< Display order (CPU cores first).
    ///@{
    int64_t first_seen = 0, last_seen = 0;  ///< First / last time the sensor was discovered (epoch ms).
    ///@}
    std::optional<int64_t> latest_ts;      ///< Timestamp of the newest stored reading, if any.
    std::optional<double> latest_value;    ///< Value of the newest stored reading, if any.
};

/// One raw reading.
struct Reading {
    int64_t sensor_id;  ///< Sensor the value belongs to.
    int64_t ts;  ///< epoch ms UTC
    double value;  ///< Value in the sensor's unit.
};

/// Aggregated readings of one time bucket (dashboard series, alarm warm-up).
struct Bucket {
    int64_t ts;  ///< bucket start
    int64_t n;   ///< Number of readings in the bucket.
    ///@{
    double sum, sumsq, min, max;  ///< Sum, sum of squares, minimum and maximum of the readings.
    ///@}
};

/// Width of the stored rollup buckets.
inline constexpr int64_t kRollupMs = 10000;

/// @brief Statistics of one sensor over a time range (alarm evaluation).
struct Aggregate {
    int64_t count = 0;  ///< Number of readings.
    ///@{
    double avg = 0, min = 0, max = 0, stddev = 0;  ///< stddev = population stddev
    ///@}
};

/// @brief A row of the `alarms` table: `func(sensor over last window_min minutes) op threshold`.
struct Alarm {
    int64_t id = 0;          ///< Alarm id.
    int64_t sensor_id = 0;   ///< The one sensor this alarm watches.
    std::string func;        ///< avg | max | min | stddev
    int window_min = 0;      ///< X
    double threshold = 0;    ///< Y
    std::string op = ">";    ///< Comparison: ">" fires above Y, "<" fires below Y
    bool enabled = true;     ///< Disabled alarms are not evaluated.
    std::string state;       ///< ok | firing | no_data | disabled
    std::optional<double> last_value;       ///< Aggregate value at the last evaluation.
    std::optional<int64_t> last_eval_ts;    ///< Time of the last evaluation (epoch ms).
    std::optional<int64_t> last_change_ts;  ///< Time of the last state change (epoch ms).
    int64_t created_at = 0;                 ///< Creation time (epoch ms).
};

/// @brief Partial update of an alarm (PATCH /api/alarms/{id}); unset fields are unchanged.
struct AlarmPatch {
    std::optional<std::string> func;   ///< New function.
    std::optional<int> window_min;     ///< New window in minutes.
    std::optional<double> threshold;   ///< New threshold.
    std::optional<std::string> op;     ///< New comparison (">" or "<").
    std::optional<bool> enabled;       ///< Enable / disable.
};

/// @brief A row of `alarm_events`: one transition into or out of "firing".
struct AlarmEvent {
    ///@{
    int64_t id = 0, alarm_id = 0, sensor_id = 0, ts = 0;  ///< Event id, alarm, sensor and time (epoch ms).
    ///@}
    std::string type;  ///< fired | cleared
    std::optional<double> value;  ///< Aggregate value at the transition.
    double threshold = 0;         ///< Alarm threshold at the transition.
    std::string op = ">";         ///< Alarm comparison at the transition (">" or "<").
};

/**
 * @brief One SQLite connection with all queries used by the system.
 *
 * Thread-safe: every public method locks an internal recursive mutex.
 * Errors are reported as DbError.
 */
class Database {
public:
    /// @brief Opens (or creates) the database file and configures WAL mode.
    /// @throws DbError if the file cannot be opened.
    explicit Database(const std::string& path);
    /// @brief Closes the connection.
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    /// Schema (idempotent).
    void migrate();

    /// @name Sensors and readings (poller / DB writer)
    ///@{

    /// Registers (or refreshes) a sensor by its stable key and returns its id.
    int64_t upsert_sensor(const sensors::SensorInfo& info, int64_t now);
    /// Bulk insert in one transaction (the DB writer's hot path).
    void insert_readings(const std::vector<Reading>& rows);
    /// Convenience: one tick of values, also refreshes last_seen.
    void insert_readings(int64_t ts, const std::vector<std::pair<int64_t, double>>& values);
    /// @brief Sets last_seen of every sensor to `ts`.
    void touch_sensors(int64_t ts);
    /// @brief Ids of all registered sensors.
    std::vector<int64_t> sensor_ids();
    ///@}

    /// @name Rollups and retention (DB writer)
    ///@{

    /// Rolls raw readings in [from, to) into 10 s buckets (bounds rounded down
    /// to bucket edges); returns the number of buckets written.
    int64_t rollup(int64_t from, int64_t to);
    std::optional<int64_t> rollup_watermark();         ///< end of rolled-up data, any sensor
    std::optional<int64_t> rollup_end(int64_t sensor_id);  ///< end of rolled-up data for one sensor
    std::optional<int64_t> oldest_raw_ts();            ///< timestamp of the oldest raw reading
    int64_t prune_before(int64_t ts);                  ///< raw readings
    int64_t prune_rollups_before(int64_t ts);          ///< rollup buckets; returns rows deleted
    ///@}

    /// @name Queries (server)
    ///@{

    /// @brief All sensors with their latest reading, in display order.
    std::vector<Sensor> list_sensors();
    /// Buckets of `bucket_ms` for one sensor in [from, to]. Widths below 10 s
    /// read raw readings; wider ones read the rollups plus the newest raw
    /// seconds that are not rolled up yet.
    std::vector<Bucket> buckets(int64_t sensor_id, int64_t from, int64_t to, int64_t bucket_ms);
    /// @brief Buckets computed from raw readings in [from, to].
    std::vector<Bucket> raw_buckets(int64_t sensor_id, int64_t from, int64_t to, int64_t bucket_ms);
    /// @brief Buckets computed from the 10 s rollups in [from, to_excl).
    std::vector<Bucket> rollup_buckets(int64_t sensor_id, int64_t from, int64_t to_excl, int64_t bucket_ms);
    /// Raw readings newer than `ts`, oldest first (DB follower).
    std::vector<Reading> readings_after(int64_t sensor_id, int64_t ts, int limit);
    /// @brief Number of raw readings (all sensors) in [from, to] (for /api/health).
    int64_t count_readings(int64_t from, int64_t to);
    /// @brief count / avg / min / max / stddev of one sensor in [from, to].
    Aggregate aggregate(int64_t sensor_id, int64_t from, int64_t to);
    ///@}

    /// @name Alarms
    ///@{

    /// @brief All alarms, by id.
    std::vector<Alarm> list_alarms();
    /// @brief One alarm, or nullopt if it does not exist.
    std::optional<Alarm> get_alarm(int64_t id);
    /// @brief Creates an alarm `func(window_min) op threshold` and returns its id.
    int64_t create_alarm(int64_t sensor_id, const std::string& func, int window_min, double threshold,
                         int64_t now, const std::string& op = ">");
    /// @brief Applies a partial update; false if the alarm does not exist.
    bool update_alarm(int64_t id, const AlarmPatch& p);
    /// @brief Deletes an alarm (its events cascade); false if it did not exist.
    bool delete_alarm(int64_t id);
    /// @brief True if a sensor with this id exists.
    bool sensor_exists(int64_t id);
    /// Persists the result of one evaluation; records an event when the
    /// state transitions into or out of "firing". Returns false (and writes
    /// nothing) if the alarm was edited, disabled or deleted since `a` was read.
    bool record_evaluation(const Alarm& a, const std::string& new_state, std::optional<double> value,
                           int64_t ts);
    /// @brief Newest alarm events first, optionally for one alarm only.
    std::vector<AlarmEvent> list_events(int limit, std::optional<int64_t> alarm_id);
    /// Persists the latest aggregate of many alarms in one transaction
    /// (state changes go through record_evaluation).
    struct AlarmValue {
        int64_t id;                   ///< Alarm id.
        std::optional<double> value;  ///< Latest aggregate value.
        int64_t eval_ts;              ///< Evaluation time (epoch ms).
    };
    /// @brief Writes many AlarmValue rows in one transaction.
    void update_alarm_values(const std::vector<AlarmValue>& values);
    ///@}

private:
    /// @brief Executes SQL without results; throws DbError on failure.
    void exec(const char* sql);
    /// @brief Like exec(), but retries while the database is busy (start-up races).
    void exec_retry(const char* sql, int attempts = 40);
    /// @brief Deletes rows of `table` with `ts_col` < ts; returns rows deleted.
    int64_t prune_table_before(const char* table, const char* ts_col, int64_t ts);
    /// @brief True if `table` has a column named `column`.
    bool has_column(const char* table, const char* column);
    /// @brief Schema creation / upgrade; caller holds the write lock (see migrate()).
    void migrate_locked();
    sqlite3* db_ = nullptr;     ///< SQLite connection handle.
    std::recursive_mutex mu_;   ///< Serialises all access to db_.
};

}  // namespace sd::db

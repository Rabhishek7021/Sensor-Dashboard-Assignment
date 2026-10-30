/**
 * @file db.cpp
 * @brief SQLite implementation of sd::db::Database: schema and migrations, batched inserts,
 *        rollups, retention, bucketed queries and alarm storage.
 */
#include "db.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace sd::db {

namespace {

/// RAII wrapper around a prepared statement.
class Stmt {
public:
    /// @brief Prepares `sql`; throws DbError on failure.
    Stmt(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &st_, nullptr) != SQLITE_OK)
            throw DbError(std::string("prepare failed: ") + sqlite3_errmsg(db) + " | " + sql);
    }
    ~Stmt() { sqlite3_finalize(st_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

        /// @brief Binds a 64-bit integer to parameter `i` (1-based).
    Stmt& bind(int i, int64_t v) { check(sqlite3_bind_int64(st_, i, v)); return *this; }
    /// @brief Binds an int to parameter `i`.
    Stmt& bind(int i, int v) { check(sqlite3_bind_int64(st_, i, v)); return *this; }
    /// @brief Binds a double to parameter `i`.
    Stmt& bind(int i, double v) { check(sqlite3_bind_double(st_, i, v)); return *this; }
    /// @brief Binds a text value to parameter `i` (copied by SQLite).
    Stmt& bind(int i, const std::string& v) {
        check(sqlite3_bind_text(st_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT));
        return *this;
    }
    /// @brief Binds NULL to parameter `i`.
    Stmt& bind_null(int i) { check(sqlite3_bind_null(st_, i)); return *this; }
    template <typename T>
    /// @brief Binds the value, or NULL if empty.
    Stmt& bind(int i, const std::optional<T>& v) { return v ? bind(i, *v) : bind_null(i); }

    /// Returns true if a row is available.
    bool step() {
        int rc = sqlite3_step(st_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw DbError(std::string("step failed: ") + sqlite3_errmsg(db_));
    }
    /// @brief Resets the statement and clears bindings for reuse.
    void reset() { sqlite3_reset(st_); sqlite3_clear_bindings(st_); }

        /// @brief Column `c` (0-based) of the current row as a 64-bit integer.
    int64_t i64(int c) const { return sqlite3_column_int64(st_, c); }
    /// @brief Column `c` as a double.
    double dbl(int c) const { return sqlite3_column_double(st_, c); }
    /// @brief True if column `c` is NULL.
    bool is_null(int c) const { return sqlite3_column_type(st_, c) == SQLITE_NULL; }
    /// @brief Column `c` as text ("" for NULL).
    std::string text(int c) const {
        auto p = sqlite3_column_text(st_, c);
        return p ? reinterpret_cast<const char*>(p) : "";
    }
    /// @brief Column `c` as an integer, or nullopt for NULL.
    std::optional<int64_t> opt_i64(int c) const { return is_null(c) ? std::nullopt : std::optional(i64(c)); }
    /// @brief Column `c` as a double, or nullopt for NULL.
    std::optional<double> opt_dbl(int c) const { return is_null(c) ? std::nullopt : std::optional(dbl(c)); }

private:
    /// @brief Throws DbError unless rc is SQLITE_OK.
    void check(int rc) {
        if (rc != SQLITE_OK) throw DbError(std::string("bind failed: ") + sqlite3_errmsg(db_));
    }
    sqlite3* db_;                 ///< Owning connection (for error messages).
    sqlite3_stmt* st_ = nullptr;  ///< Prepared statement.
};

/// @brief Column list read by read_alarm(), in order.
constexpr const char* kAlarmCols =
    "id, sensor_id, func, window_min, threshold, enabled, state, last_value, last_eval_ts, "
    "last_change_ts, created_at, op";

/// @brief Builds an Alarm from a row selected with kAlarmCols.
Alarm read_alarm(const Stmt& s) {
    Alarm a;
    a.id = s.i64(0);
    a.sensor_id = s.i64(1);
    a.func = s.text(2);
    a.window_min = static_cast<int>(s.i64(3));
    a.threshold = s.dbl(4);
    a.enabled = s.i64(5) != 0;
    a.state = s.text(6);
    a.last_value = s.opt_dbl(7);
    a.last_eval_ts = s.opt_i64(8);
    a.last_change_ts = s.opt_i64(9);
    a.created_at = s.i64(10);
    a.op = s.text(11);
    return a;
}

}  // namespace

Database::Database(const std::string& path) {
    int rc = sqlite3_open_v2(path.c_str(), &db_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
        sqlite3_close(db_);
        throw DbError("cannot open database '" + path + "': " + msg);
    }
    sqlite3_busy_timeout(db_, 5000);
    // Switching a new database file to WAL can briefly fail while another
    // process opens the same file; retry instead of exiting.
    exec_retry("PRAGMA journal_mode=WAL;");
    exec("PRAGMA synchronous=NORMAL;");   // WAL + NORMAL: durable on checkpoint, no fsync per commit
    exec("PRAGMA foreign_keys=ON;");
    exec("PRAGMA temp_store=MEMORY;");    // GROUP BY scratch space in RAM
    exec("PRAGMA cache_size=-16000;");    // 16 MB page cache per connection
}

Database::~Database() { sqlite3_close(db_); }

void Database::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw DbError("exec failed: " + msg);
    }
}

/// Runs `sql`, retrying while the database is busy or locked by another
/// process (start-up only; normal statements rely on the busy timeout).
void Database::exec_retry(const char* sql, int attempts) {
    for (int i = 1;; ++i) {
        char* err = nullptr;
        const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
        if (rc == SQLITE_OK) return;
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        const int code = rc & 0xff;
        if ((code != SQLITE_BUSY && code != SQLITE_LOCKED) || i >= attempts)
            throw DbError("exec failed: " + msg);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

void Database::migrate() {
    std::lock_guard lock(mu_);
    // The poller and the server both migrate at start-up, possibly at the
    // same moment. One write-locked transaction makes the check-then-alter
    // steps below atomic across processes (SQLite DDL is transactional).
    exec_retry("BEGIN IMMEDIATE;");
    try {
        migrate_locked();
        exec("COMMIT;");
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

void Database::migrate_locked() {
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS sensors (
    id          INTEGER PRIMARY KEY,
    key         TEXT NOT NULL UNIQUE,      -- stable identity, e.g. 'coretemp/Core 4'
    chip        TEXT NOT NULL,             -- source device/driver, e.g. 'coretemp', 'BAT0'
    label       TEXT NOT NULL,             -- 'Core 4', 'Fan 1', 'BAT0 charge'
    unit        TEXT NOT NULL,             -- celsius | rpm | percent | ...
    kind        TEXT NOT NULL,             -- core | die | cpu | fan | charge | ...
    category    TEXT NOT NULL DEFAULT 'temperature',  -- temperature | fan | battery | ...
    sort_order  INTEGER NOT NULL,
    first_seen  INTEGER NOT NULL,          -- epoch ms UTC
    last_seen   INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS readings (
    sensor_id   INTEGER NOT NULL REFERENCES sensors(id),
    ts          INTEGER NOT NULL,          -- epoch ms UTC
    value       REAL    NOT NULL,
    PRIMARY KEY (sensor_id, ts)
) WITHOUT ROWID;
-- Every query reads readings per sensor through the primary key, so the old
-- secondary index on ts only cost space and write time (~40% of the file).
DROP INDEX IF EXISTS idx_readings_ts;

-- 10-second rollups (count, sum, sum of squares, min, max) maintained by the
-- DB writer. Long dashboard windows and long alarm windows read these instead
-- of millions of raw rows, and they outlive the raw retention period.
CREATE TABLE IF NOT EXISTS readings_10s (
    sensor_id   INTEGER NOT NULL REFERENCES sensors(id),
    bucket      INTEGER NOT NULL,          -- epoch ms UTC, multiple of 10000
    n           INTEGER NOT NULL,
    sum         REAL    NOT NULL,
    sumsq       REAL    NOT NULL,
    min         REAL    NOT NULL,
    max         REAL    NOT NULL,
    PRIMARY KEY (sensor_id, bucket)
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS alarms (
    id              INTEGER PRIMARY KEY,
    sensor_id       INTEGER NOT NULL REFERENCES sensors(id),
    func            TEXT    NOT NULL CHECK (func IN ('avg','max','min','stddev')),
    window_min      INTEGER NOT NULL CHECK (window_min > 0),
    threshold       REAL    NOT NULL,
    op              TEXT    NOT NULL DEFAULT '>' CHECK (op IN ('>','<')),
    enabled         INTEGER NOT NULL DEFAULT 1,
    state           TEXT    NOT NULL DEFAULT 'no_data',
    last_value      REAL,
    last_eval_ts    INTEGER,
    last_change_ts  INTEGER,
    created_at      INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_alarms_sensor ON alarms(sensor_id);

CREATE TABLE IF NOT EXISTS alarm_events (
    id          INTEGER PRIMARY KEY,
    alarm_id    INTEGER NOT NULL REFERENCES alarms(id) ON DELETE CASCADE,
    sensor_id   INTEGER NOT NULL,
    ts          INTEGER NOT NULL,
    type        TEXT    NOT NULL CHECK (type IN ('fired','cleared')),
    value       REAL,
    threshold   REAL    NOT NULL,
    op          TEXT    NOT NULL DEFAULT '>'
);
CREATE INDEX IF NOT EXISTS idx_events_alarm_ts ON alarm_events(alarm_id, ts);
)SQL");

    // Databases created before sensor categories existed: add the column.
    // Every sensor stored by those versions was a CPU temperature.
    if (!has_column("sensors", "category"))
        exec("ALTER TABLE sensors ADD COLUMN category TEXT NOT NULL DEFAULT 'temperature';");

    // Alarms without a comparison operator: add it. Every alarm stored without
    // one is a "greater than" alarm, which is the column default.
    if (!has_column("alarms", "op"))
        exec("ALTER TABLE alarms ADD COLUMN op TEXT NOT NULL DEFAULT '>' CHECK (op IN ('>','<'));");
    if (!has_column("alarm_events", "op"))
        exec("ALTER TABLE alarm_events ADD COLUMN op TEXT NOT NULL DEFAULT '>';");

    // Package sensors are no longer tracked. Remove any left over from older
    // versions together with their readings and alarms (alarm_events go with
    // the alarms via ON DELETE CASCADE). No-op once the database is clean.
    exec(R"SQL(
DELETE FROM readings WHERE sensor_id IN (SELECT id FROM sensors WHERE kind = 'package');
DELETE FROM readings_10s WHERE sensor_id IN (SELECT id FROM sensors WHERE kind = 'package');
DELETE FROM alarms   WHERE sensor_id IN (SELECT id FROM sensors WHERE kind = 'package');
DELETE FROM sensors  WHERE kind = 'package';
)SQL");
}

bool Database::has_column(const char* table, const char* column) {
    Stmt s(db_, (std::string("PRAGMA table_info(") + table + ")").c_str());
    while (s.step())
        if (s.text(1) == column) return true;
    return false;
}

/// Written as INSERT OR IGNORE + UPDATE + SELECT rather than UPSERT/RETURNING
/// so it works with any SQLite 3 shipped by older distributions.
int64_t Database::upsert_sensor(const sensors::SensorInfo& info, int64_t now) {
    std::lock_guard lock(mu_);
    exec("BEGIN IMMEDIATE;");
    try {
        Stmt ins(db_, "INSERT OR IGNORE INTO sensors "
                      "(key, chip, label, unit, kind, category, sort_order, first_seen, last_seen) "
                      "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?8)");
        ins.bind(1, info.key).bind(2, info.source).bind(3, info.label).bind(4, info.unit).bind(5, info.kind)
            .bind(6, info.category).bind(7, info.sort_order).bind(8, now);
        ins.step();
        Stmt upd(db_, "UPDATE sensors SET chip = ?2, label = ?3, unit = ?4, kind = ?5, category = ?6, "
                      "sort_order = ?7, last_seen = ?8 WHERE key = ?1");
        upd.bind(1, info.key).bind(2, info.source).bind(3, info.label).bind(4, info.unit).bind(5, info.kind)
            .bind(6, info.category).bind(7, info.sort_order).bind(8, now);
        upd.step();
        Stmt sel(db_, "SELECT id FROM sensors WHERE key = ?1");
        sel.bind(1, info.key);
        if (!sel.step()) throw DbError("upsert_sensor: row not found after insert");
        const int64_t id = sel.i64(0);
        exec("COMMIT;");
        return id;
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

void Database::insert_readings(const std::vector<Reading>& rows) {
    if (rows.empty()) return;
    std::lock_guard lock(mu_);
    exec("BEGIN IMMEDIATE;");
    try {
        // One prepared statement and one transaction for the whole batch:
        // thousands of rows cost about as much as a single commit.
        Stmt s(db_, "INSERT OR REPLACE INTO readings (sensor_id, ts, value) VALUES (?1, ?2, ?3)");
        for (const auto& r : rows) {
            s.bind(1, r.sensor_id).bind(2, r.ts).bind(3, r.value);
            s.step();
            s.reset();
        }
        exec("COMMIT;");
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

void Database::insert_readings(int64_t ts, const std::vector<std::pair<int64_t, double>>& values) {
    std::vector<Reading> rows;
    rows.reserve(values.size());
    for (const auto& [id, v] : values) rows.push_back({id, ts, v});
    insert_readings(rows);
    touch_sensors(ts);
}

void Database::touch_sensors(int64_t ts) {
    std::lock_guard lock(mu_);
    Stmt u(db_, "UPDATE sensors SET last_seen = ?1 WHERE last_seen < ?1");
    u.bind(1, ts);
    u.step();
}

std::vector<int64_t> Database::sensor_ids() {
    std::lock_guard lock(mu_);
    Stmt s(db_, "SELECT id FROM sensors ORDER BY id");
    std::vector<int64_t> out;
    while (s.step()) out.push_back(s.i64(0));
    return out;
}

std::optional<int64_t> Database::oldest_raw_ts() {
    std::lock_guard lock(mu_);
    // MIN per sensor walks the primary key (no full scan, no ts index needed).
    Stmt s(db_, "SELECT MIN(m) FROM (SELECT (SELECT MIN(ts) FROM readings WHERE sensor_id = s.id) AS m "
                "FROM sensors s)");
    if (!s.step() || s.is_null(0)) return std::nullopt;
    return s.i64(0);
}

std::optional<int64_t> Database::rollup_watermark() {
    std::lock_guard lock(mu_);
    Stmt s(db_, "SELECT MAX(m) FROM (SELECT (SELECT MAX(bucket) FROM readings_10s WHERE sensor_id = s.id) AS m "
                "FROM sensors s)");
    if (!s.step() || s.is_null(0)) return std::nullopt;
    return s.i64(0) + kRollupMs;
}

std::optional<int64_t> Database::rollup_end(int64_t sensor_id) {
    std::lock_guard lock(mu_);
    Stmt s(db_, "SELECT MAX(bucket) FROM readings_10s WHERE sensor_id = ?1");
    s.bind(1, sensor_id);
    if (!s.step() || s.is_null(0)) return std::nullopt;
    return s.i64(0) + kRollupMs;
}

int64_t Database::rollup(int64_t from, int64_t to) {
    from = from / kRollupMs * kRollupMs;
    to = to / kRollupMs * kRollupMs;
    if (to <= from) return 0;
    const auto ids = sensor_ids();
    std::lock_guard lock(mu_);
    exec("BEGIN IMMEDIATE;");
    try {
        Stmt s(db_, "INSERT OR REPLACE INTO readings_10s (sensor_id, bucket, n, sum, sumsq, min, max) "
                    "SELECT sensor_id, (ts / 10000) * 10000, COUNT(*), SUM(value), SUM(value * value), "
                    "MIN(value), MAX(value) FROM readings "
                    "WHERE sensor_id = ?1 AND ts >= ?2 AND ts < ?3 GROUP BY 2");
        int64_t n = 0;
        for (auto id : ids) {
            s.bind(1, id).bind(2, from).bind(3, to);
            s.step();
            n += sqlite3_changes(db_);
            s.reset();
        }
        exec("COMMIT;");
        return n;
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

int64_t Database::prune_table_before(const char* table, const char* ts_col, int64_t ts) {
    const auto ids = sensor_ids();
    std::lock_guard lock(mu_);
    const std::string sql = std::string("DELETE FROM ") + table + " WHERE sensor_id = ?1 AND " + ts_col + " < ?2";
    int64_t n = 0;
    exec("BEGIN IMMEDIATE;");
    try {
        Stmt s(db_, sql.c_str());
        for (auto id : ids) {
            s.bind(1, id).bind(2, ts);
            s.step();
            n += sqlite3_changes(db_);
            s.reset();
        }
        exec("COMMIT;");
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
    return n;
}

int64_t Database::prune_before(int64_t ts) { return prune_table_before("readings", "ts", ts); }
int64_t Database::prune_rollups_before(int64_t ts) { return prune_table_before("readings_10s", "bucket", ts); }

std::vector<Bucket> Database::raw_buckets(int64_t sensor_id, int64_t from, int64_t to, int64_t bucket_ms) {
    std::lock_guard lock(mu_);
    Stmt s(db_, "SELECT (ts / ?4) * ?4 AS b, COUNT(*), SUM(value), SUM(value * value), MIN(value), MAX(value) "
                "FROM readings WHERE sensor_id = ?1 AND ts >= ?2 AND ts <= ?3 GROUP BY b ORDER BY b");
    s.bind(1, sensor_id).bind(2, from).bind(3, to).bind(4, std::max<int64_t>(bucket_ms, 1));
    std::vector<Bucket> out;
    while (s.step()) out.push_back({s.i64(0), s.i64(1), s.dbl(2), s.dbl(3), s.dbl(4), s.dbl(5)});
    return out;
}

std::vector<Bucket> Database::rollup_buckets(int64_t sensor_id, int64_t from, int64_t to, int64_t bucket_ms) {
    std::lock_guard lock(mu_);
    Stmt s(db_, "SELECT (bucket / ?4) * ?4 AS b, SUM(n), SUM(sum), SUM(sumsq), MIN(min), MAX(max) "
                "FROM readings_10s WHERE sensor_id = ?1 AND bucket >= ?2 AND bucket < ?3 GROUP BY b ORDER BY b");
    s.bind(1, sensor_id).bind(2, from).bind(3, to).bind(4, std::max<int64_t>(bucket_ms, kRollupMs));
    std::vector<Bucket> out;
    while (s.step()) out.push_back({s.i64(0), s.i64(1), s.dbl(2), s.dbl(3), s.dbl(4), s.dbl(5)});
    return out;
}

std::vector<Bucket> Database::buckets(int64_t sensor_id, int64_t from, int64_t to, int64_t bucket_ms) {
    if (bucket_ms < kRollupMs) return raw_buckets(sensor_id, from, to, bucket_ms);
    // Rollups up to their end, raw readings for the most recent seconds that
    // are not rolled up yet; buckets on the seam are merged.
    bucket_ms = (bucket_ms + kRollupMs - 1) / kRollupMs * kRollupMs;
    const int64_t seam = rollup_end(sensor_id).value_or(from);
    auto out = rollup_buckets(sensor_id, from / kRollupMs * kRollupMs, std::min(seam, to + 1), bucket_ms);
    if (seam <= to) {
        for (const auto& b : raw_buckets(sensor_id, std::max(seam, from), to, bucket_ms)) {
            if (!out.empty() && out.back().ts == b.ts) {
                auto& m = out.back();
                m.n += b.n;
                m.sum += b.sum;
                m.sumsq += b.sumsq;
                m.min = std::min(m.min, b.min);
                m.max = std::max(m.max, b.max);
            } else {
                out.push_back(b);
            }
        }
    }
    return out;
}

std::vector<Reading> Database::readings_after(int64_t sensor_id, int64_t ts, int limit) {
    std::lock_guard lock(mu_);
    Stmt s(db_, "SELECT ts, value FROM readings WHERE sensor_id = ?1 AND ts > ?2 ORDER BY ts LIMIT ?3");
    s.bind(1, sensor_id).bind(2, ts).bind(3, limit);
    std::vector<Reading> out;
    while (s.step()) out.push_back({sensor_id, s.i64(0), s.dbl(1)});
    return out;
}

std::vector<Sensor> Database::list_sensors() {
    std::lock_guard lock(mu_);
    Stmt s(db_, R"SQL(
SELECT s.id, s.key, s.chip, s.label, s.unit, s.kind, s.sort_order, s.first_seen, s.last_seen,
       r.ts, r.value, s.category
FROM sensors s
LEFT JOIN readings r
  ON r.sensor_id = s.id
 AND r.ts = (SELECT MAX(ts) FROM readings WHERE sensor_id = s.id)
ORDER BY s.sort_order, s.id)SQL");
    std::vector<Sensor> out;
    while (s.step()) {
        Sensor x;
        x.id = s.i64(0);
        x.key = s.text(1);
        x.chip = s.text(2);
        x.label = s.text(3);
        x.unit = s.text(4);
        x.kind = s.text(5);
        x.sort_order = static_cast<int>(s.i64(6));
        x.first_seen = s.i64(7);
        x.last_seen = s.i64(8);
        x.latest_ts = s.opt_i64(9);
        x.latest_value = s.opt_dbl(10);
        x.category = s.text(11);
        out.push_back(std::move(x));
    }
    return out;
}

int64_t Database::count_readings(int64_t from, int64_t to) {
    std::lock_guard lock(mu_);
    Stmt s(db_, "SELECT SUM(c) FROM (SELECT (SELECT COUNT(*) FROM readings r WHERE r.sensor_id = s.id "
                "AND r.ts BETWEEN ?1 AND ?2) AS c FROM sensors s)");
    s.bind(1, from).bind(2, to);
    s.step();
    return s.is_null(0) ? 0 : s.i64(0);
}

Aggregate Database::aggregate(int64_t sensor_id, int64_t from, int64_t to) {
    std::lock_guard lock(mu_);
    // Population variance computed in one pass as E[x^2] - E[x]^2. For
    // temperature-scale values (tens of degrees, ~1e3 samples) the
    // cancellation error is negligible; clamp tiny negatives to zero.
    Stmt s(db_, "SELECT COUNT(*), AVG(value), MIN(value), MAX(value), AVG(value * value) "
                "FROM readings WHERE sensor_id = ?1 AND ts > ?2 AND ts <= ?3");
    s.bind(1, sensor_id).bind(2, from).bind(3, to);
    s.step();
    Aggregate a;
    a.count = s.i64(0);
    if (a.count > 0) {
        a.avg = s.dbl(1);
        a.min = s.dbl(2);
        a.max = s.dbl(3);
        double var = s.dbl(4) - a.avg * a.avg;
        a.stddev = std::sqrt(var > 0 ? var : 0.0);
    }
    return a;
}

std::vector<Alarm> Database::list_alarms() {
    std::lock_guard lock(mu_);
    Stmt s(db_, (std::string("SELECT ") + kAlarmCols + " FROM alarms ORDER BY sensor_id, id").c_str());
    std::vector<Alarm> out;
    while (s.step()) out.push_back(read_alarm(s));
    return out;
}

std::optional<Alarm> Database::get_alarm(int64_t id) {
    std::lock_guard lock(mu_);
    Stmt s(db_, (std::string("SELECT ") + kAlarmCols + " FROM alarms WHERE id = ?1").c_str());
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    return read_alarm(s);
}

bool Database::sensor_exists(int64_t id) {
    std::lock_guard lock(mu_);
    Stmt s(db_, "SELECT 1 FROM sensors WHERE id = ?1");
    s.bind(1, id);
    return s.step();
}

int64_t Database::create_alarm(int64_t sensor_id, const std::string& func, int window_min,
                               double threshold, int64_t now, const std::string& op) {
    std::lock_guard lock(mu_);
    Stmt s(db_, "INSERT INTO alarms (sensor_id, func, window_min, threshold, op, enabled, state, created_at) "
                "VALUES (?1, ?2, ?3, ?4, ?6, 1, 'no_data', ?5)");
    s.bind(1, sensor_id).bind(2, func).bind(3, window_min).bind(4, threshold).bind(5, now).bind(6, op);
    s.step();
    return sqlite3_last_insert_rowid(db_);
}

bool Database::update_alarm(int64_t id, const AlarmPatch& p) {
    std::lock_guard lock(mu_);
    // COALESCE keeps the existing value for fields not present in the patch.
    // Changing the definition resets state so the next evaluation starts clean.
    Stmt s(db_, R"SQL(
UPDATE alarms SET
    func       = COALESCE(?2, func),
    window_min = COALESCE(?3, window_min),
    threshold  = COALESCE(?4, threshold),
    op         = COALESCE(?6, op),
    enabled    = COALESCE(?5, enabled),
    state      = CASE WHEN COALESCE(?5, enabled) = 0 THEN 'disabled'
                      WHEN state = 'disabled' THEN 'no_data'
                      ELSE state END
WHERE id = ?1)SQL");
    s.bind(1, id).bind(2, p.func).bind(3, p.window_min).bind(4, p.threshold);
    if (p.enabled) s.bind(5, *p.enabled ? 1 : 0); else s.bind_null(5);
    s.bind(6, p.op);
    s.step();
    return sqlite3_changes(db_) > 0;
}

bool Database::delete_alarm(int64_t id) {
    std::lock_guard lock(mu_);
    Stmt s(db_, "DELETE FROM alarms WHERE id = ?1");
    s.bind(1, id);
    s.step();
    return sqlite3_changes(db_) > 0;
}

bool Database::record_evaluation(const Alarm& a, const std::string& new_state, std::optional<double> value,
                                 int64_t ts) {
    std::lock_guard lock(mu_);
    exec("BEGIN IMMEDIATE;");
    try {
        const bool changed = new_state != a.state;
        // Compare-and-set on (state, enabled): if the alarm was edited,
        // disabled or deleted through the API since `a` was read, this
        // evaluation is stale and is dropped; the next cycle re-evaluates.
        Stmt s(db_, "UPDATE alarms SET state = ?2, last_value = ?3, last_eval_ts = ?4, "
                    "last_change_ts = CASE WHEN ?5 THEN ?4 ELSE last_change_ts END "
                    "WHERE id = ?1 AND state = ?6 AND enabled = 1 AND func = ?7 AND window_min = ?8 "
                    "AND threshold = ?9 AND op = ?10");
        s.bind(1, a.id).bind(2, new_state).bind(3, value).bind(4, ts).bind(5, changed ? 1 : 0)
            .bind(6, a.state).bind(7, a.func).bind(8, a.window_min).bind(9, a.threshold).bind(10, a.op);
        s.step();
        if (sqlite3_changes(db_) == 0) {
            exec("COMMIT;");
            return false;
        }

        std::optional<std::string> ev;
        if (new_state == "firing" && a.state != "firing") ev = "fired";
        else if (a.state == "firing" && new_state != "firing") ev = "cleared";
        if (ev) {
            Stmt e(db_, "INSERT INTO alarm_events (alarm_id, sensor_id, ts, type, value, threshold, op) "
                        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)");
            e.bind(1, a.id).bind(2, a.sensor_id).bind(3, ts).bind(4, *ev).bind(5, value).bind(6, a.threshold)
                .bind(7, a.op);
            e.step();
        }
        exec("COMMIT;");
        return true;
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

std::vector<AlarmEvent> Database::list_events(int limit, std::optional<int64_t> alarm_id) {
    std::lock_guard lock(mu_);
    Stmt s(db_, "SELECT id, alarm_id, sensor_id, ts, type, value, threshold, op FROM alarm_events "
                "WHERE (?1 IS NULL OR alarm_id = ?1) ORDER BY ts DESC, id DESC LIMIT ?2");
    s.bind(1, alarm_id).bind(2, limit);
    std::vector<AlarmEvent> out;
    while (s.step()) {
        AlarmEvent e;
        e.id = s.i64(0);
        e.alarm_id = s.i64(1);
        e.sensor_id = s.i64(2);
        e.ts = s.i64(3);
        e.type = s.text(4);
        e.value = s.opt_dbl(5);
        e.threshold = s.dbl(6);
        e.op = s.text(7);
        out.push_back(std::move(e));
    }
    return out;
}

void Database::update_alarm_values(const std::vector<AlarmValue>& values) {
    if (values.empty()) return;
    std::lock_guard lock(mu_);
    exec("BEGIN IMMEDIATE;");
    try {
        Stmt s(db_, "UPDATE alarms SET last_value = ?2, last_eval_ts = ?3 WHERE id = ?1 AND enabled = 1");
        for (const auto& v : values) {
            s.bind(1, v.id).bind(2, v.value).bind(3, v.eval_ts);
            s.step();
            s.reset();
        }
        exec("COMMIT;");
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

}  // namespace sd::db

/**
 * @file alarm_engine.cpp
 * @brief AlarmEngine implementation: warm-up, sample ingestion, evaluation, transitions and persistence.
 */
#include "alarm_engine.hpp"

#include <future>
#include <iomanip>
#include <sstream>

#include "../common/alarm_logic.hpp"
#include "../common/log.hpp"
#include "../common/time_util.hpp"

namespace sd::server {

using SteadyClock = std::chrono::steady_clock;  ///< Monotonic clock for evaluation timing.

AlarmEngine::AlarmEngine(db::Database& db, bus::Bus& bus, AlarmOptions opts) : db_(db), bus_(bus), opts_(opts) {
    // Subscribe before warm-up so no sample published in between is missed;
    // samples already in the DB are skipped by timestamp (warm_until_).
    sub_ = bus_.subscribe("alarm-engine",
                          {msg::topic::kSamplesAll, msg::topic::kAlarmReload, msg::topic::kSensorsChanged}, 1 << 16);
}

AlarmEngine::~AlarmEngine() { stop(); }

void AlarmEngine::start() {
    stop_ = false;
    thread_ = std::thread([this] { run(); });
}

void AlarmEngine::stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    sub_->close();
    thread_.join();
    bus_.unsubscribe(sub_);
}

bool AlarmEngine::reload(std::chrono::milliseconds timeout) {
    auto p = std::make_shared<std::promise<void>>();
    auto f = p->get_future();
    bus_.publish(msg::topic::kAlarmReload, msg::ReloadRequest{p});
    return f.wait_for(timeout) == std::future_status::ready;
}

std::map<int64_t, msg::AlarmLive> AlarmEngine::snapshot() const {
    std::lock_guard lock(snap_mu_);
    return snapshot_;
}

AlarmEngine::Stats AlarmEngine::stats() const {
    Stats s;
    s.evaluations = evals_;
    s.transitions = transitions_;
    s.samples = samples_;
    s.dropped = sub_->dropped();
    s.last_eval_us = last_eval_us_;
    s.max_eval_us = max_eval_us_;
    s.alarms = alarm_count_;
    s.sensors_tracked = tracked_;
    return s;
}

void AlarmEngine::run() {
    std::vector<std::shared_ptr<std::promise<void>>> waiting;
    auto release = [&] {
        for (auto& p : waiting) p->set_value();
        waiting.clear();
    };
    try {
        warm_up();
        load_labels();
        load_alarms();
        evaluate(time::now_ms(), true);
    } catch (const std::exception& e) {
        log::error("alarm engine: start-up failed: ", e.what());
    }
    log::info("alarm engine started: ", alarms_.size(), " alarm(s), evaluating every ", opts_.eval_interval.count(),
              " ms");

    auto next_eval = SteadyClock::now() + opts_.eval_interval;
    auto next_persist = SteadyClock::now() + opts_.persist_interval;
    while (!stop_) {
        auto msgs = sub_->drain_until(std::min(next_eval, next_persist));
        bool reload = false, labels = false;
        for (const auto& m : msgs) {
            if (const auto* b = bus::payload_as<msg::SampleBatch>(*m)) {
                ingest(*b);
            } else if (const auto* r = bus::payload_as<msg::ReloadRequest>(*m)) {
                reload = true;
                if (r->done) waiting.push_back(r->done);
            } else if (bus::payload_as<msg::SensorsChanged>(*m)) {
                labels = true;
            }
        }
        try {
            if (labels) load_labels();
            if (reload || reload_pending_) load_alarms();
            const auto now = SteadyClock::now();
            if (reload || now >= next_eval) {
                evaluate(time::now_ms(), reload);
                next_eval = now + opts_.eval_interval;
            }
            if (now >= next_persist) {
                persist_values();
                next_persist = now + opts_.persist_interval;
            }
        } catch (const std::exception& e) {
            log::error("alarm evaluation failed: ", e.what());
        }
        release();
    }
    try {
        persist_values();
    } catch (...) {
    }
    release();
}

/// Fills the in-memory windows with the last hour from the database.
void AlarmEngine::warm_up() {
    const int64_t now = time::now_ms();
    const int64_t from = now - int64_t(opts_.memory_horizon_s) * 1000;
    for (auto id : db_.sensor_ids()) {
        auto secs = db_.raw_buckets(id, from, now, 1000);
        if (secs.empty()) continue;
        // A second that ended less than 2 s ago may still be filling up:
        // leave it (and anything later) to the bus, which this engine
        // subscribed to before warm-up.
        if (secs.back().ts >= now - 2000) secs.pop_back();
        auto& w = windows_.try_emplace(id, opts_.memory_horizon_s).first->second;
        int64_t until = from;
        for (const auto& b : secs) {
            w.add_bucket(b.ts / 1000, b.n, b.sum, b.sumsq, b.min, b.max);
            until = b.ts + 999;
        }
        warm_until_[id] = until;  // bus samples at or before this were loaded from the DB
    }
    tracked_ = windows_.size();
}

void AlarmEngine::ingest(const msg::SampleBatch& b) {
    for (const auto& s : b.samples) {
        auto wu = warm_until_.find(s.sensor_id);
        if (wu != warm_until_.end() && b.ts <= wu->second) continue;  // already loaded from the DB
        auto it = windows_.find(s.sensor_id);
        if (it == windows_.end()) {
            it = windows_.try_emplace(s.sensor_id, opts_.memory_horizon_s).first;
            tracked_ = windows_.size();
        }
        it->second.add(b.ts, s.value);
    }
    samples_ += b.samples.size();
}

void AlarmEngine::load_labels() {
    labels_.clear();
    for (const auto& s : db_.list_sensors()) labels_[s.id] = s.label;
}

void AlarmEngine::load_alarms() {
    reload_pending_ = false;
    load_labels();  // new alarms may reference sensors registered since the last load
    std::map<int64_t, Runtime> old;
    for (auto& r : alarms_) old[r.def.id] = std::move(r);
    alarms_.clear();
    for (auto& def : db_.list_alarms()) {
        Runtime rt;
        auto it = old.find(def.id);
        const bool same = it != old.end() && it->second.def.func == def.func &&
                          it->second.def.window_min == def.window_min && it->second.def.threshold == def.threshold &&
                          it->second.def.op == def.op && it->second.def.enabled == def.enabled;
        if (same) {
            rt = std::move(it->second);
        } else {
            rt.value = def.last_value;
            rt.eval_ts = def.last_eval_ts;
        }
        rt.change_ts = def.last_change_ts;
        rt.def = std::move(def);  // DB state is authoritative (the API may have reset it)
        alarms_.push_back(std::move(rt));
    }
    alarm_count_ = alarms_.size();
}

db::Aggregate AlarmEngine::long_window(int64_t sensor_id, int64_t now, int64_t window_ms) {
    // Rollups for most of the window plus raw rows for the newest seconds
    // (Database::buckets merges both); hour-wide buckets keep it to a few rows.
    alarm::Accumulator acc;
    for (const auto& b : db_.buckets(sensor_id, now - window_ms + 1, now, 3600 * 1000))
        acc.add(b.n, b.sum, b.sumsq, b.min, b.max);
    return acc.finish();
}

void AlarmEngine::evaluate(int64_t now, bool force) {
    const auto t0 = SteadyClock::now();
    const int64_t horizon_ms = int64_t(opts_.memory_horizon_s) * 1000;
    msg::AlarmSnapshot snap;
    snap.ts = now;
    for (auto& rt : alarms_) {
        auto& a = rt.def;
        if (a.enabled) {
            const int64_t window_ms = int64_t(a.window_min) * 60 * 1000;
            bool evaluate_now = true;
            db::Aggregate agg;
            if (window_ms <= horizon_ms) {
                auto w = windows_.find(a.sensor_id);
                if (w != windows_.end()) agg = w->second.aggregate(now, window_ms);
            } else if (force || now >= rt.next_long_eval) {
                agg = long_window(a.sensor_id, now, window_ms);
                rt.next_long_eval = now + opts_.long_eval_interval.count();
            } else {
                evaluate_now = false;  // long windows change slowly; keep the last result
            }
            if (evaluate_now) {
                const auto res = alarm::evaluate(a.func, agg, a.threshold, opts_.min_samples, a.op);
                rt.value = res.value;
                rt.eval_ts = now;
                rt.samples = agg.count;
                if (res.state != a.state) {
                    // Compare-and-set in the DB: if the alarm was edited meanwhile,
                    // drop this result and reload definitions on the next pass.
                    if (db_.record_evaluation(a, res.state, res.value, now)) {
                        const std::string from = a.state;
                        a.state = res.state;
                        rt.change_ts = now;
                        a.last_change_ts = now;
                        a.last_value = res.value;
                        ++transitions_;
                        if (res.state == "firing" || from == "firing") {
                            std::ostringstream os;
                            os << std::fixed << std::setprecision(2) << "alarm #" << a.id << " ("
                               << labels_[a.sensor_id] << ": " << a.func << " over " << a.window_min << " min " << a.op << " "
                               << a.threshold << ") " << (res.state == "firing" ? "FIRED" : "CLEARED")
                               << ", value=" << res.value.value_or(0.0) << " n=" << agg.count;
                            if (res.state == "firing") log::warn(os.str()); else log::info(os.str());
                        }
                        bus_.publish(msg::topic::alarm_state(a.id),
                                     msg::AlarmTransition{a, from, labels_[a.sensor_id], now});
                    } else {
                        reload_pending_ = true;
                    }
                }
            }
        }
        snap.alarms.push_back({a.id, a.enabled ? a.state : "disabled", rt.value, rt.eval_ts, rt.change_ts, rt.samples});
    }
    {
        std::lock_guard lock(snap_mu_);
        snapshot_.clear();
        for (const auto& l : snap.alarms) snapshot_[l.id] = l;
    }
    bus_.publish(msg::topic::kAlarmSnapshot, std::move(snap));
    ++evals_;
    const double us = std::chrono::duration<double, std::micro>(SteadyClock::now() - t0).count();
    last_eval_us_ = us;
    if (us > max_eval_us_) max_eval_us_ = us;
}

void AlarmEngine::persist_values() {
    std::vector<db::Database::AlarmValue> v;
    for (const auto& rt : alarms_)
        if (rt.def.enabled && rt.eval_ts) v.push_back({rt.def.id, rt.value, *rt.eval_ts});
    db_.update_alarm_values(v);
}

}  // namespace sd::server

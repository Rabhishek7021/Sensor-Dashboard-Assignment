/**
 * @file sampler.cpp
 * @brief Sampler implementation: discovery and registration, one aligned sampling thread per
 *        provider, and the rediscovery supervisor.
 */
#include "sampler.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

#include "../common/log.hpp"
#include "../common/time_util.hpp"
#include "messages.hpp"

namespace sd::pipeline {

/// @brief The sensors of one provider and the thread that samples them.
struct Sampler::Group {
    std::string provider;  ///< Provider name, e.g. "cpu_temp".
    std::vector<sensors::Sensor*> sensors;  ///< Sensors read by this thread (owned by the Discovery).
    std::vector<int64_t> ids;  ///< Database id of each sensor, same order.
    std::vector<int> failures;  ///< Consecutive read failures per sensor (for log throttling).
    std::thread thread;  ///< Sampling thread.
};

Sampler::Sampler(sensors::SensorManager& manager, db::Database& registry, bus::Bus& bus, SamplerOptions opts)
    : manager_(manager), registry_(registry), bus_(bus), opts_(opts) {}

Sampler::~Sampler() { stop(); }

namespace {

/// @brief Text for Sampler::summary(): "N sensor(s): provider=count ... [labels]".
std::string make_summary(const sensors::Discovery& d) {
    std::ostringstream os;
    os << d.sensors.size() << " sensor(s):";
    for (const auto& [name, n] : d.per_provider) os << ' ' << name << '=' << n;
    os << " [";
    for (size_t i = 0; i < d.sensors.size(); ++i) os << (i ? ", " : "") << d.sensors[i]->info().label;
    return os.str() + "]";
}

/// @brief Stable keys of a discovery result (to detect a changed sensor set).
std::set<std::string> keys_of(const sensors::Discovery& d) {
    std::set<std::string> k;
    for (const auto& s : d.sensors) k.insert(s->info().key);
    return k;
}

/// @brief Raises `a` to `v` if `v` is larger (lock-free).
void atomic_max(std::atomic<double>& a, double v) {
    double cur = a.load();
    while (v > cur && !a.compare_exchange_weak(cur, v)) {
    }
}

}  // namespace

/// Runs discovery once, registers the sensors and starts the sampling and supervisor threads.
size_t Sampler::start() {
    auto d = manager_.discover();
    if (d.sensors.empty()) return 0;
    stop_ = false;
    {
        std::lock_guard lock(mu_);
        activate(std::move(d));
        start_groups();
        // Subscribers (alarm engine, MQTT bridge, dashboards) refresh their
        // sensor names and ids from the registry.
        bus_.publish(msg::topic::kSensorsChanged, msg::SensorsChanged{discovery_.sensors.size()});
    }
    if (opts_.rediscover.count() > 0) supervisor_ = std::thread([this] { supervise(); });
    std::lock_guard lock(mu_);
    return discovery_.sensors.size();
}

void Sampler::stop() {
    stop_ = true;
    {
        std::lock_guard l(wait_mu_);
    }
    wait_cv_.notify_all();
    if (supervisor_.joinable()) supervisor_.join();
    std::lock_guard lock(mu_);
    stop_groups();
}

/// Registers the sensors (stable ids by key) and groups them by provider.
/// Caller holds mu_ and has stopped the previous groups.
void Sampler::activate(sensors::Discovery d) {
    const int64_t now = time::now_ms();
    std::map<std::string, std::unique_ptr<Group>> by_provider;
    std::vector<std::string> order;
    for (const auto& s : d.sensors) {
        auto& g = by_provider[s->provider()];
        if (!g) {
            g = std::make_unique<Group>();
            g->provider = s->provider();
            order.push_back(g->provider);
        }
        g->sensors.push_back(s.get());
        g->ids.push_back(registry_.upsert_sensor(s->info(), now));
        g->failures.push_back(0);
    }
    groups_.clear();
    for (const auto& name : order) groups_.push_back(std::move(by_provider[name]));
    summary_ = make_summary(d);
    discovery_ = std::move(d);
}

void Sampler::start_groups() {
    groups_stop_ = false;
    for (auto& g : groups_) {
        Group* gp = g.get();
        g->thread = std::thread([this, gp] { run_group(*gp); });
    }
}

void Sampler::stop_groups() {
    groups_stop_ = true;
    {
        std::lock_guard l(wait_mu_);
    }
    wait_cv_.notify_all();
    for (auto& g : groups_)
        if (g->thread.joinable()) g->thread.join();
}

/// Ticks are aligned to multiples of the interval; an overrun tick is skipped, not queued.
void Sampler::run_group(Group& g) {
    const int64_t interval = opts_.interval.count();
    const std::string topic = msg::topic::samples(g.provider);
    int64_t last_tick = 0;
    while (!groups_stop_) {
        // Next tick on the shared grid (epoch multiple of the interval). The
        // max() guards against an early wake-up producing the same tick twice.
        const int64_t now = time::now_ms();
        const int64_t tick = std::max((now / interval + 1) * interval, last_tick + interval);
        last_tick = tick;
        {
            std::unique_lock lock(wait_mu_);
            wait_cv_.wait_until(lock, std::chrono::steady_clock::now() + std::chrono::milliseconds(tick - now),
                                [&] { return groups_stop_.load(); });
        }
        if (groups_stop_) break;

        const auto t0 = std::chrono::steady_clock::now();
        msg::SampleBatch batch;
        batch.ts = tick;
        batch.source = g.provider;
        batch.samples.reserve(g.sensors.size());
        for (size_t i = 0; i < g.sensors.size(); ++i) {
            auto v = g.sensors[i]->read();
            if (!v) {
                ++failures_;
                // Log on the first failure, then about once a minute, not every tick.
                if (g.failures[i]++ % std::max<int64_t>(1, 60000 / interval) == 0)
                    log::warn("read failed for ", g.sensors[i]->info().key, " (", g.sensors[i]->origin(), ")");
                continue;
            }
            if (g.failures[i] > 0) {
                log::info("read recovered for ", g.sensors[i]->info().key);
                g.failures[i] = 0;
            }
            batch.samples.push_back({g.ids[i], *v});
        }
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        last_tick_us_ = us;
        atomic_max(max_tick_us_, us);
        ++ticks_;
        samples_ += batch.samples.size();
        if (time::now_ms() >= tick + interval) ++skipped_;  // overran into the next tick
        if (!batch.samples.empty()) bus_.publish(topic, std::move(batch));
    }
}

void Sampler::supervise() {
    while (!stop_) {
        {
            std::unique_lock lock(wait_mu_);
            wait_cv_.wait_for(lock, opts_.rediscover, [&] { return stop_.load(); });
        }
        if (stop_) break;
        try {
            auto fresh = manager_.discover();
            std::lock_guard lock(mu_);
            if (fresh.sensors.empty() || keys_of(fresh) == keys_of(discovery_)) continue;
            log::info("sensor set changed, now ", make_summary(fresh));
            stop_groups();
            activate(std::move(fresh));
            start_groups();
            bus_.publish(msg::topic::kSensorsChanged, msg::SensorsChanged{discovery_.sensors.size()});
        } catch (const std::exception& e) {
            log::warn("rediscovery failed: ", e.what());
        }
    }
}

std::string Sampler::summary() const {
    std::lock_guard lock(mu_);
    return summary_;
}

Sampler::Stats Sampler::stats() const {
    Stats s;
    s.ticks = ticks_;
    s.samples = samples_;
    s.read_failures = failures_;
    s.skipped_ticks = skipped_;
    s.last_tick_us = last_tick_us_;
    s.max_tick_us = max_tick_us_;
    std::lock_guard lock(mu_);
    s.sensors = discovery_.sensors.size();
    s.threads = groups_.size() + (opts_.rediscover.count() > 0 ? 1 : 0);
    return s;
}

}  // namespace sd::pipeline

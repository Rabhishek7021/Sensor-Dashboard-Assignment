/**
 * @file test_main.cpp
 * @brief Dependency-free unit tests (17 groups, run by CTest).
 *
 * Covers the sensor providers against a fake sysfs tree, the SQLite store and
 * schema check, alarm logic and alarm state transitions, the bus, the sliding
 * window, the DB writer, the sampler, and the DB follower / sampler lock.
 */

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <functional>
#include <map>
#include <thread>
#include <unistd.h>
#include <memory>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "common/alarm_logic.hpp"
#include "common/db.hpp"
#include "common/time_util.hpp"
#include "sensors/manager.hpp"
#include "sensors/providers/cpu_temp.hpp"
#include "sensors/registry.hpp"
#include "server/alarm_engine.hpp"
#include "bus/bus.hpp"
#include "common/alarm_window.hpp"
#include "pipeline/db_writer.hpp"
#include "pipeline/messages.hpp"
#include "pipeline/mqtt_bridge.hpp"
#include "pipeline/sampler.hpp"
#include "pipeline/source.hpp"

namespace fs = std::filesystem;
using namespace sd;

///@{
static int g_failed = 0, g_passed = 0;  ///< Failed / passed CHECKs across all groups.
///@}
/// @brief Records a pass if `cond` is true, otherwise prints the failing expression with file:line.
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (cond) ++g_passed;                                                             \
        else { ++g_failed; std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond "\n"; } \
    } while (0)
/// @brief CHECK that |a - b| < eps.
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) < (eps))

/// @brief Writes `s` plus a newline to `p`, creating parent directories (builds fake sysfs files).
static void put(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << s << "\n";
}

/// @brief Fresh, empty temporary directory for one test.
static fs::path tmpdir(const std::string& name) {
    fs::path p = fs::temp_directory_path() / ("sd-test-" + name + "-" + std::to_string(time::now_ms()));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

/// Runs one provider through the registry against a fake sysfs tree.
static std::vector<std::unique_ptr<sensors::Sensor>> run(const std::string& provider,
                                                         const sensors::ProviderContext& ctx) {
    return sensors::builtin_providers().create(provider, ctx)->discover();
}

/// @brief ProviderContext pointing hwmon / power_supply / thermal at a fake tree under `root`.
static sensors::ProviderContext ctx_for(const fs::path& root) {
    sensors::ProviderContext c;
    c.hwmon_root = (root / "hwmon").string();
    c.power_supply_root = (root / "power_supply").string();
    c.thermal_root = (root / "thermal").string();
    return c;
}

/// Mirrors the target laptop: coretemp with a package sensor (must be
/// skipped) and non-contiguous core numbers, plus unrelated chips that must be
/// ignored. Cores must come out ordered Core 0, Core 4, Core 8, ...
static void test_cpu_temp_single_socket() {
    auto root = tmpdir("hw1");
    auto hw = root / "hwmon";
    put(hw / "hwmon0/name", "acpitz");
    put(hw / "hwmon0/temp1_input", "54000");
    put(hw / "hwmon1/name", "nvme");
    put(hw / "hwmon1/temp1_input", "38900");
    put(hw / "hwmon5/name", "coretemp");
    put(hw / "hwmon5/temp1_label", "Package id 0");
    put(hw / "hwmon5/temp1_input", "53000");
    const int cores[] = {0, 4, 8, 12, 16, 20, 24, 25, 26, 27};
    int idx = 2;
    for (int c : {20, 0, 27, 4, 8, 12, 16, 24, 25, 26}) {  // shuffled on purpose
        put(hw / "hwmon5" / ("temp" + std::to_string(idx) + "_label"), "Core " + std::to_string(c));
        put(hw / "hwmon5" / ("temp" + std::to_string(idx) + "_input"), std::to_string(40000 + c * 100));
        ++idx;
    }

    auto s = run("cpu_temp", ctx_for(root));
    CHECK(s.size() == 10);  // package excluded, cores only
    for (size_t i = 0; i < 10 && i < s.size(); ++i) {
        const auto& in = s[i]->info();
        CHECK(in.label == "Core " + std::to_string(cores[i]));  // numeric order: 0,4,8,...
        CHECK(in.key == "coretemp/Core " + std::to_string(cores[i]));  // same keys as earlier versions
        CHECK(in.kind == "core" && in.category == "temperature" && in.unit == "celsius");
    }
    auto v = s[0]->read();
    CHECK(v.has_value());
    if (v) CHECK_NEAR(*v, 40.0, 1e-9);  // Core 0 = 40000 millidegrees

    // Unreadable value is reported as nullopt, not a crash.
    put(hw / "hwmon5/temp3_input", "garbage");
    CHECK(!s[0]->read().has_value());
    fs::remove(hw / "hwmon5/temp3_input");
    CHECK(!s[0]->read().has_value());

    // No matching chip -> empty result (and no thermal zones -> still empty).
    auto c = ctx_for(root);
    c.cpu_chips = {"k10temp"};
    CHECK(sensors::CpuTempProvider(c).discover().empty());
    // Missing root -> empty, no throw.
    CHECK(run("cpu_temp", ctx_for(root / "missing")).empty());
    fs::remove_all(root);
}

/// Dual-socket server: one coretemp device per socket, core labels repeat.
static void test_cpu_temp_multi_socket() {
    auto root = tmpdir("hw2");
    auto devs = root / "devices";
    for (int s = 0; s < 2; ++s) {
        fs::create_directories(devs / ("coretemp." + std::to_string(s)));
        auto h = root / "hwmon" / ("hwmon" + std::to_string(s));
        put(h / "name", "coretemp");
        fs::create_directory_symlink(devs / ("coretemp." + std::to_string(s)), h / "device");
        put(h / "temp1_label", "Package id " + std::to_string(s));
        put(h / "temp1_input", "50000");
        put(h / "temp2_label", "Core 0");
        put(h / "temp2_input", "45000");
        put(h / "temp3_label", "Core 1");
        put(h / "temp3_input", "46000");
    }
    auto s = run("cpu_temp", ctx_for(root));
    CHECK(s.size() == 4);  // two cores per socket, packages excluded
    std::set<std::string> keys;
    for (auto& x : s) keys.insert(x->info().key);
    CHECK(keys.size() == 4);  // unique despite repeated "Core 0"
    CHECK(keys.count("coretemp.coretemp.1/Core 0") == 1);
    if (s.size() == 4) {
        CHECK(s[0]->info().label == "CPU0 Core 0");
        CHECK(s[1]->info().label == "CPU0 Core 1");
        CHECK(s[2]->info().label == "CPU1 Core 0");
    }
    fs::remove_all(root);
}

/// AMD exposes no per-core sensors: fall back to per-die (Tccd), and to the
/// whole-CPU Tctl when there are no dies either. ARM boards without a CPU
/// hwmon driver fall back to thermal zones.
static void test_cpu_temp_fallbacks() {
    auto root = tmpdir("amd");
    auto k = root / "hwmon/hwmon2";
    put(k / "name", "k10temp");
    put(k / "temp1_label", "Tctl");
    put(k / "temp1_input", "61000");
    put(k / "temp3_label", "Tccd1");
    put(k / "temp3_input", "58000");
    put(k / "temp4_label", "Tccd2");
    put(k / "temp4_input", "59000");
    auto s = run("cpu_temp", ctx_for(root));
    CHECK(s.size() == 2 && s[0]->info().label == "Tccd1" && s[0]->info().kind == "die");

    fs::remove(k / "temp3_input");
    fs::remove(k / "temp4_input");
    s = run("cpu_temp", ctx_for(root));
    CHECK(s.size() == 1 && s[0]->info().label == "Tctl" && s[0]->info().kind == "cpu");

    fs::remove_all(root / "hwmon");
    put(root / "thermal/thermal_zone0/type", "cpu-thermal");
    put(root / "thermal/thermal_zone0/temp", "47250");
    put(root / "thermal/thermal_zone1/type", "battery");  // not a CPU zone
    put(root / "thermal/thermal_zone1/temp", "30000");
    put(root / "thermal/cooling_device0/type", "Processor");
    s = run("cpu_temp", ctx_for(root));
    CHECK(s.size() == 1);
    if (s.size() == 1) {
        CHECK(s[0]->info().label == "cpu-thermal");
        CHECK_NEAR(s[0]->read().value_or(0), 47.25, 1e-9);
    }
    fs::remove_all(root);
}

/// Fans: RPM values, a stopped fan (0 RPM) and fans on several chips.
static void test_fan_provider() {
    auto root = tmpdir("fan");
    auto hw = root / "hwmon";
    put(hw / "hwmon0/name", "coretemp");  // no fans: ignored
    put(hw / "hwmon0/temp2_input", "40000");
    put(hw / "hwmon4/name", "thinkpad");
    put(hw / "hwmon4/fan1_input", "2450");
    put(hw / "hwmon4/fan2_input", "0");  // stopped fan is a valid reading
    put(hw / "hwmon4/fan2_label", "GPU Fan");
    auto s = run("fan", ctx_for(root));
    CHECK(s.size() == 2);
    if (s.size() == 2) {
        CHECK(s[0]->info().label == "Fan 1" && s[0]->info().key == "fan:thinkpad/fan1");
        CHECK(s[0]->info().unit == "rpm" && s[0]->info().category == "fan");
        CHECK_NEAR(s[0]->read().value_or(-1), 2450.0, 1e-9);
        CHECK(s[1]->info().label == "GPU Fan");
        CHECK_NEAR(s[1]->read().value_or(-1), 0.0, 1e-9);
    }
    // Fans on a second chip: labels name the chip to stay unambiguous.
    put(hw / "hwmon6/name", "nct6775");
    put(hw / "hwmon6/fan1_input", "900");
    s = run("fan", ctx_for(root));
    CHECK(s.size() == 3 && s[2]->info().label == "Fan 1 (nct6775)");
    fs::remove_all(root);
}

/// Batteries: `capacity` file, energy_now / energy_full fallback, peripheral batteries
/// ignored, and machines without a battery.
static void test_battery_provider() {
    auto root = tmpdir("bat");
    auto ps = root / "power_supply";
    put(ps / "AC/type", "Mains");
    put(ps / "BAT0/type", "Battery");
    put(ps / "BAT0/scope", "System");
    put(ps / "BAT0/capacity", "87");
    put(ps / "BAT1/type", "Battery");  // no capacity file: computed from energy
    put(ps / "BAT1/energy_now", "30000000");
    put(ps / "BAT1/energy_full", "40000000");
    put(ps / "hidpp_battery_0/type", "Battery");  // wireless mouse
    put(ps / "hidpp_battery_0/scope", "Device");
    put(ps / "hidpp_battery_0/capacity", "50");
    auto s = run("battery", ctx_for(root));
    CHECK(s.size() == 2);
    if (s.size() == 2) {
        CHECK(s[0]->info().label == "BAT0 charge" && s[0]->info().unit == "percent");
        CHECK_NEAR(s[0]->read().value_or(-1), 87.0, 1e-9);
        CHECK(s[1]->info().key == "battery:BAT1/charge");
        CHECK_NEAR(s[1]->read().value_or(-1), 75.0, 1e-9);
    }
    // Desktop without batteries.
    CHECK(run("battery", ctx_for(root / "none")).empty());
    fs::remove_all(root);
}

/// Providers are combined by the manager: families stay grouped in
/// registration order and unknown names are rejected.
static void test_registry_and_manager() {
    const auto& reg = sensors::builtin_providers();
    CHECK(reg.contains("cpu_temp") && reg.contains("fan") && reg.contains("battery"));
    bool threw = false;
    try { reg.create("gpu", {}); } catch (const sensors::UnknownProvider&) { threw = true; }
    CHECK(threw);
    CHECK(sensors::parse_provider_list("all", reg).size() == reg.entries().size());
    CHECK((sensors::parse_provider_list(" fan, cpu_temp ,", reg) == std::vector<std::string>{"fan", "cpu_temp"}));

    auto root = tmpdir("mgr");
    put(root / "hwmon/hwmon1/name", "coretemp");
    put(root / "hwmon/hwmon1/temp2_label", "Core 4");
    put(root / "hwmon/hwmon1/temp2_input", "41000");
    put(root / "hwmon/hwmon1/temp3_label", "Core 0");
    put(root / "hwmon/hwmon1/temp3_input", "40000");
    put(root / "hwmon/hwmon2/name", "thinkpad");
    put(root / "hwmon/hwmon2/fan1_input", "2000");
    put(root / "power_supply/BAT0/type", "Battery");
    put(root / "power_supply/BAT0/capacity", "55");

    // Listed fan-first, but displayed in registry order: CPU, fan, battery.
    sensors::SensorManager m(reg, {"battery", "fan", "cpu_temp"}, ctx_for(root));
    auto d = m.discover();
    std::vector<std::string> labels;
    for (auto& s : d.sensors) labels.push_back(s->info().label);
    CHECK((labels == std::vector<std::string>{"Core 0", "Core 4", "Fan 1", "BAT0 charge"}));
    CHECK(d.per_provider.size() == 3);

    threw = false;
    try { sensors::SensorManager bad(reg, {"cpu_temp", "gpu"}, ctx_for(root)); }
    catch (const sensors::UnknownProvider&) { threw = true; }
    CHECK(threw);
    fs::remove_all(root);
}

/// A provider defined outside the built-ins plugs into the same machinery
/// (this is all a new sensor family needs).
static void test_custom_provider() {
    struct ConstSensor : sensors::Sensor {
        using Sensor::Sensor;
        std::optional<double> read() const override { return 12.5; }
    };
    struct VoltageProvider : sensors::SensorProvider {
        std::string name() const override { return "voltage"; }
        std::vector<std::unique_ptr<sensors::Sensor>> discover() override {
            std::vector<std::unique_ptr<sensors::Sensor>> v;
            v.push_back(std::make_unique<ConstSensor>(
                sensors::SensorInfo{"volt:in0", "test", "Vcore", "voltage", "rail", "volt", 0}));
            return v;
        }
    };
    sensors::ProviderRegistry reg;
    reg.add("voltage", "test rails",
            [](const sensors::ProviderContext&) { return std::make_unique<VoltageProvider>(); });
    sensors::SensorManager m(reg, {"voltage"}, {});
    auto d = m.discover();
    CHECK(d.sensors.size() == 1 && d.sensors[0]->info().category == "voltage");
    CHECK_NEAR(d.sensors[0]->read().value_or(0), 12.5, 1e-9);
}

/// Store: stable sensor ids on re-registration, aggregates (stddev checked by hand),
/// bucketed queries and retention.
static void test_store_and_aggregates() {
    auto dir = tmpdir("db");
    db::Database store((dir / "t.db").string());
    store.migrate();
    store.migrate();  // idempotent

    sensors::SensorInfo a{"coretemp/Core 0", "coretemp", "Core 0", "temperature", "core", "celsius", 1000};
    sensors::SensorInfo b{"coretemp/Core 4", "coretemp", "Core 4", "temperature", "core", "celsius", 1004};
    auto ida = store.upsert_sensor(a, 1);
    auto idb = store.upsert_sensor(b, 1);
    CHECK(store.upsert_sensor(a, 2) == ida);  // stable id on re-register
    CHECK(ida != idb);

    const int64_t t0 = 1'000'000;
    const double vals[] = {40, 42, 44, 46, 48};
    for (int i = 0; i < 5; ++i) store.insert_readings(t0 + i * 5000, {{ida, vals[i]}, {idb, 60.0}});

    auto agg = store.aggregate(ida, t0 - 1, t0 + 20000);
    CHECK(agg.count == 5);
    CHECK_NEAR(agg.avg, 44.0, 1e-9);
    CHECK_NEAR(agg.min, 40.0, 1e-9);
    CHECK_NEAR(agg.max, 48.0, 1e-9);
    CHECK_NEAR(agg.stddev, std::sqrt(8.0), 1e-9);  // population stddev of 40..48 step 2
    CHECK(store.aggregate(idb, t0 - 1, t0 + 20000).count == 5);
    CHECK(store.aggregate(ida, t0 + 100000, t0 + 200000).count == 0);

    auto raw = store.raw_buckets(ida, t0, t0 + 20000, 1);
    CHECK(raw.size() == 5 && store.raw_buckets(idb, t0, t0 + 20000, 1).size() == 5);
    auto bucketed = store.raw_buckets(ida, t0, t0 + 20000, 10000);
    CHECK(bucketed.size() == 3);  // buckets at 1.00M, 1.01M, 1.02M
    CHECK(bucketed.size() == 3 && bucketed[0].n == 2 && bucketed[0].min == 40 && bucketed[0].max == 42);

    auto sensors = store.list_sensors();
    CHECK(sensors.size() == 2);
    CHECK(sensors[0].latest_value && std::fabs(*sensors[0].latest_value - 48.0) < 1e-9);

    CHECK(store.prune_before(t0 + 10000) == 4);  // 2 ticks x 2 sensors
    fs::remove_all(dir);
}

/// Alarm logic: every function, the strict ">" boundary and min_samples.
static void test_alarm_logic() {
    db::Aggregate a{5, 44.0, 40.0, 48.0, 2.83};
    CHECK(alarm::evaluate("avg", a, 43.0).state == "firing");
    CHECK(alarm::evaluate("avg", a, 44.0).state == "ok");  // strictly greater
    CHECK(alarm::evaluate("max", a, 47.9).state == "firing");
    CHECK(alarm::evaluate("min", a, 40.0).state == "ok");
    CHECK(alarm::evaluate("stddev", a, 2.5).state == "firing");
    CHECK(alarm::evaluate("avg", db::Aggregate{}, 0).state == "no_data");
    CHECK(alarm::evaluate("avg", a, 0, 10).state == "no_data");  // below min_samples
    db::Aggregate one{1, 50, 50, 50, 0};
    CHECK(alarm::evaluate("stddev", one, -1).state == "no_data");  // stddev needs >= 2
    CHECK(alarm::is_valid_func("stddev") && !alarm::is_valid_func("median"));
    // "<" alarms fire below the threshold, also strictly.
    CHECK(alarm::evaluate("min", a, 41.0, 1, "<").state == "firing");
    CHECK(alarm::evaluate("min", a, 40.0, 1, "<").state == "ok");      // strictly smaller
    CHECK(alarm::evaluate("avg", a, 43.0, 1, "<").state == "ok");
    CHECK(alarm::evaluate("avg", a, 43.0, 1, ">").state == "firing");  // explicit ">" = default
    CHECK(alarm::evaluate("avg", db::Aggregate{}, 100, 1, "<").state == "no_data");  // empty never fires
    CHECK(alarm::is_valid_op(">") && alarm::is_valid_op("<") && !alarm::is_valid_op(">=") && !alarm::is_valid_op(""));
    CHECK(alarm::breached("<", 1, 2) && !alarm::breached("<", 2, 2) && !alarm::breached("=", 1, 2));
}

/// Below-threshold alarms ("<"), the case of a battery running low: the engine
/// fires them when the aggregate drops under the threshold, stores the
/// comparison with the alarm and its events, and treats a changed comparison
/// like any other edit (stale evaluations are rejected).
static void test_alarm_below_threshold() {
    auto dir = tmpdir("below");
    const auto path = (dir / "t.db").string();
    db::Database store(path);
    store.migrate();
    auto bat = store.upsert_sensor({"battery:BAT0/charge", "BAT0", "BAT0 charge", "battery", "charge", "percent", 0}, 1);
    const int64_t now = time::now_ms();
    for (int i = 0; i < 6; ++i) store.insert_readings(now - 5000 - (6 - i) * 5000, {{bat, 15.0}});

    auto low = store.create_alarm(bat, "min", 10, 20.0, now, "<");    // 15 % < 20 %: fires
    auto crit = store.create_alarm(bat, "min", 10, 10.0, now, "<");   // 15 % < 10 %: ok
    auto high = store.create_alarm(bat, "max", 10, 20.0, now);        // default ">": 15 > 20 is false
    CHECK(store.get_alarm(low)->op == "<" && store.get_alarm(high)->op == ">");

    bus::Bus bus;
    db::Database engine_db(path);
    server::AlarmEngine eng(engine_db, bus, {std::chrono::milliseconds(60000), 3});
    eng.start();
    CHECK(eng.reload());
    CHECK(store.get_alarm(low)->state == "firing");
    CHECK(store.get_alarm(crit)->state == "ok");
    CHECK(store.get_alarm(high)->state == "ok");
    auto ev = store.list_events(10, low);
    CHECK(ev.size() == 1 && ev[0].type == "fired" && ev[0].op == "<" && ev[0].threshold == 20.0);

    // Switching the comparison is an edit: a result computed for the old
    // definition is rejected, and the engine re-evaluates with the new one.
    const auto before = *store.get_alarm(crit);
    db::AlarmPatch p;
    p.op = ">";
    CHECK(store.update_alarm(crit, p));
    CHECK(!store.record_evaluation(before, "firing", 15.0, now));  // stale: op changed
    CHECK(eng.reload());
    CHECK(store.get_alarm(crit)->op == ">" && store.get_alarm(crit)->state == "firing");  // 15 > 10

    // The battery charges again: the average over the window rises above 20 %
    // (6 old readings at 15 %, 8 new at 90 %) and the "<" alarm clears.
    for (int i = 0; i < 8; ++i)
        bus.publish("samples/test", msg::SampleBatch{now - 3500 + i * 500, "test", {{bat, 90.0}}});
    db::AlarmPatch w;
    w.func = "avg";
    CHECK(store.update_alarm(low, w));
    CHECK(eng.reload());
    CHECK(store.get_alarm(low)->state == "ok");
    eng.stop();
    fs::remove_all(dir);
}

/// Two cores with independent alarms: only the hot core fires, and
/// fired/cleared events are recorded on transitions only. History comes from
/// the DB (warm-up), live samples from the bus.
static void test_alarm_engine_per_core() {
    auto dir = tmpdir("eng");
    const auto path = (dir / "t.db").string();
    db::Database store(path);
    store.migrate();
    sensors::SensorInfo c0{"coretemp/Core 0", "coretemp", "Core 0", "temperature", "core", "celsius", 1000};
    sensors::SensorInfo c4{"coretemp/Core 4", "coretemp", "Core 4", "temperature", "core", "celsius", 1004};
    sensors::SensorInfo c8{"coretemp/Core 8", "coretemp", "Core 8", "temperature", "core", "celsius", 1008};
    auto id0 = store.upsert_sensor(c0, 1);
    auto id4 = store.upsert_sensor(c4, 1);
    auto id8 = store.upsert_sensor(c8, 1);  // never has data

    const int64_t now = time::now_ms();
    for (int i = 0; i < 6; ++i) store.insert_readings(now - 5000 - (6 - i) * 5000, {{id0, 45.0}, {id4, 80.0}});

    auto a0 = store.create_alarm(id0, "avg", 10, 75.0, now);
    auto a4 = store.create_alarm(id4, "avg", 10, 75.0, now);
    auto a8 = store.create_alarm(id8, "max", 10, 75.0, now);
    auto along = store.create_alarm(id4, "avg", 120, 75.0, now);  // > 60 min: evaluated from the DB

    bus::Bus bus;
    auto transitions = bus.subscribe("test", {msg::topic::kAlarmStateAll});
    db::Database engine_db(path);
    server::AlarmEngine eng(engine_db, bus, {std::chrono::milliseconds(60000), 3});  // only explicit reloads
    eng.start();
    CHECK(eng.reload());
    CHECK(store.get_alarm(a0)->state == "ok");
    CHECK(store.get_alarm(a4)->state == "firing");
    CHECK(store.get_alarm(a8)->state == "no_data");
    CHECK(store.get_alarm(along)->state == "firing");
    auto snap = eng.snapshot();
    CHECK(snap[a4].samples == 6);  // all six DB rows were loaded at start-up
    CHECK(snap[a4].last_value && std::fabs(*snap[a4].last_value - 80.0) < 1e-9);
    auto ev = store.list_events(10, std::nullopt);
    CHECK(ev.size() == 2);  // a4 and the long-window alarm fired

    // Live samples on the bus push Core 0 over its threshold.
    for (int i = 0; i < 8; ++i)
        bus.publish("samples/test", msg::SampleBatch{now - 3500 + i * 500, "test", {{id0, 200.0}}});
    CHECK(eng.reload());
    CHECK(store.get_alarm(a0)->state == "firing");
    CHECK(eng.snapshot()[a0].samples == 14);
    bool saw_a0 = false;
    for (const auto& m : transitions->drain(std::chrono::milliseconds(100)))
        if (const auto* t = bus::payload_as<msg::AlarmTransition>(*m))
            if (t->alarm.id == a0 && t->alarm.state == "firing" && t->from_state == "ok" && t->sensor_label == "Core 0")
                saw_a0 = true;
    CHECK(saw_a0);

    // Re-evaluating without a change records no new event.
    CHECK(eng.reload());
    CHECK(store.list_events(10, a0).size() == 1);

    // Raising the threshold clears it and records a "cleared" event.
    db::AlarmPatch p;
    p.threshold = 90.0;
    CHECK(store.update_alarm(a4, p));
    CHECK(eng.reload());
    CHECK(store.get_alarm(a4)->state == "ok");
    ev = store.list_events(10, a4);
    CHECK(ev.size() == 2 && ev[0].type == "cleared");

    // Disabled alarms are not evaluated.
    db::AlarmPatch off;
    off.enabled = false;
    store.update_alarm(a0, off);
    CHECK(eng.reload());
    CHECK(store.get_alarm(a0)->state == "disabled" && eng.snapshot()[a0].state == "disabled");

    // Stale evaluation (alarm edited after it was read) is discarded.
    auto stale = *store.get_alarm(a4);
    db::AlarmPatch t;
    t.threshold = 10.0;
    store.update_alarm(a4, t);
    CHECK(!store.record_evaluation(stale, "firing", 80.0, now));
    CHECK(store.get_alarm(a4)->threshold == 10.0 && store.get_alarm(a4)->state == "ok");

    eng.stop();
    CHECK(store.delete_alarm(a4));
    CHECK(store.list_events(10, a4).empty());  // cascade
    CHECK(!store.get_alarm(a4).has_value());
    fs::remove_all(dir);
}

/// MQTT-style topic matching, delivery, overflow and multi-threaded publishing.
static void test_bus() {
    CHECK(bus::topic_matches("samples/#", "samples/cpu_temp"));
    CHECK(bus::topic_matches("samples/#", "samples"));  // '#' also matches the parent level
    CHECK(bus::topic_matches("alarms/+/state", "alarms/7/state"));
    CHECK(!bus::topic_matches("alarms/+/state", "alarms/7/8/state"));
    CHECK(!bus::topic_matches("samples/cpu", "samples/cpu/x"));
    CHECK(!bus::topic_matches("samples/cpu/x", "samples/cpu"));
    CHECK(bus::topic_matches("#", "anything/at/all"));
    CHECK(bus::valid_filter("+/+/#") && !bus::valid_filter("a/#/b") && !bus::valid_filter("a+/b") &&
          !bus::valid_filter(""));
    bool threw = false;
    bus::Bus b;
    try { b.subscribe("bad", {"a/#/b"}); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);

    auto s1 = b.subscribe("one", {"samples/#"}, 3);
    auto s2 = b.subscribe("two", {"alarms/+/state"});
    CHECK(b.publish("samples/fan", 1) == 1);
    CHECK(b.publish("alarms/1/state", std::string("x")) == 1);
    CHECK(b.publish("other", 2) == 0);
    auto m = s1->drain(std::chrono::milliseconds(0));
    CHECK(m.size() == 1 && *bus::payload_as<int>(*m[0]) == 1 && !bus::payload_as<std::string>(*m[0]));
    CHECK(s2->drain(std::chrono::milliseconds(0)).size() == 1);

    // A full queue drops its oldest messages and counts them.
    for (int i = 0; i < 5; ++i) b.publish("samples/x", i);
    m = s1->drain(std::chrono::milliseconds(0));
    CHECK(m.size() == 3 && *bus::payload_as<int>(*m[0]) == 2 && s1->dropped() == 2);

    // drain() waits at most its timeout; close() wakes a waiter.
    auto t0 = std::chrono::steady_clock::now();
    CHECK(s1->drain(std::chrono::milliseconds(50)).empty());
    CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(45));
    std::thread closer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        s1->close();
    });
    t0 = std::chrono::steady_clock::now();
    s1->drain(std::chrono::seconds(5));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
    closer.join();

    // Four publisher threads, one consumer thread: nothing lost or duplicated.
    auto big = b.subscribe("big", {"load/+"}, 1 << 20);
    std::atomic<long long> sum{0};
    std::atomic<int> got{0};
    std::thread consumer([&] {
        while (got < 40000)
            for (const auto& x : big->drain(std::chrono::milliseconds(100))) {
                sum += *bus::payload_as<int>(*x);
                ++got;
            }
    });
    std::vector<std::thread> pubs;
    for (int t = 0; t < 4; ++t)
        pubs.emplace_back([&, t] {
            for (int i = 0; i < 10000; ++i) b.publish("load/" + std::to_string(t), 1);
        });
    for (auto& p : pubs) p.join();
    consumer.join();
    CHECK(got == 40000 && sum == 40000 && big->dropped() == 0);
    b.unsubscribe(big);
    CHECK(b.publish("load/0", 1) == 0);
}

/// The in-memory window must agree with a brute-force computation.
static void test_second_window() {
    alarm::SecondWindow w(3600);
    std::vector<std::pair<int64_t, double>> all;
    const int64_t base = 1'700'000'000'000;
    unsigned seed = 12345;
    for (int i = 0; i < 4 * 7200; ++i) {  // 4 h at 500 ms
        seed = seed * 1103515245u + 12345u;
        const double v = 40 + (seed >> 16) % 4000 / 100.0;
        const int64_t ts = base + i * 500;
        w.add(ts, v);
        all.push_back({ts, v});
    }
    const int64_t now = base + (4 * 7200 - 1) * 500;
    for (int minutes : {1, 10, 60}) {
        const int64_t window = minutes * 60000LL;
        alarm::Accumulator ref;
        for (const auto& [ts, v] : all)
            if (ts / 1000 > (now - window) / 1000 && ts / 1000 <= now / 1000) ref.add(1, v, v * v, v, v);
        const auto a = w.aggregate(now, window);
        const auto r = ref.finish();
        CHECK(a.count == r.count && a.count == minutes * 120);
        CHECK_NEAR(a.avg, r.avg, 1e-9);
        CHECK_NEAR(a.min, r.min, 1e-12);
        CHECK_NEAR(a.max, r.max, 1e-12);
        CHECK_NEAR(a.stddev, r.stddev, 1e-6);
    }
    // Windows beyond the horizon are clamped to it; old slots never leak in.
    CHECK(w.aggregate(now, 3 * 3600 * 1000LL).count == 3600 * 2);
    CHECK(w.aggregate(now + 7200 * 1000LL, 60000).count == 0);
}

/// The writer batches samples into few transactions, rolls up closed 10 s
/// buckets exactly, and loses nothing on stop().
static void test_db_writer() {
    auto dir = tmpdir("writer");
    const auto path = (dir / "t.db").string();
    db::Database store(path);
    store.migrate();
    auto ida = store.upsert_sensor({"a", "x", "A", "temperature", "core", "celsius", 0}, 1);
    auto idb = store.upsert_sensor({"b", "x", "B", "fan", "fan", "rpm", 1}, 1);

    bus::Bus bus;
    db::Database wdb(path);
    pipeline::WriterOptions o;
    o.flush_interval = std::chrono::milliseconds(200);
    o.rollup_interval = std::chrono::milliseconds(100);
    o.raw_retention = std::chrono::hours(0);  // no pruning in this test
    o.rollup_retention = std::chrono::hours(0);
    pipeline::DbWriter w(wdb, bus, o);
    w.start();
    const int64_t base = (time::now_ms() / 10000 + 1) * 10000;  // bucket-aligned, after the writer started
    for (int i = 0; i < 120; ++i)  // 60 s of ticks at 500 ms, two providers
        for (auto [id, src] : {std::pair{ida, "p1"}, std::pair{idb, "p2"}})
            bus.publish(msg::topic::samples(src), msg::SampleBatch{base + i * 500, src, {{id, double(i)}}});
    w.stop();

    const auto st = w.stats();
    CHECK(st.rows == 240 && st.pending == 0 && st.dropped == 0 && st.errors == 0);
    CHECK(st.flushes >= 1 && st.flushes <= 5);  // batched, not one commit per tick
    CHECK(store.count_readings(base, base + 60000) == 240);
    // Closed buckets were rolled up and match the raw data exactly.
    auto raw = store.raw_buckets(ida, base, base + 49999, 10000);
    auto roll = store.rollup_buckets(ida, base, base + 50000, 10000);
    CHECK(raw.size() == 5 && roll.size() == 5);
    for (size_t i = 0; i < std::min(raw.size(), roll.size()); ++i) {
        CHECK(roll[i].ts == raw[i].ts && roll[i].n == 20 && roll[i].n == raw[i].n);
        CHECK_NEAR(roll[i].sum, raw[i].sum, 1e-9);
        CHECK(roll[i].min == raw[i].min && roll[i].max == raw[i].max);
    }
    // buckets() stitches rollups and the not-yet-rolled raw tail together.
    auto stitched = store.buckets(ida, base, base + 60000, 30000);
    int64_t n = 0;
    double sum = 0;
    for (const auto& b : stitched) {
        n += b.n;
        sum += b.sum;
    }
    // 60 s over a 30 s grid spans 2 or 3 buckets depending on alignment;
    // what matters is that no reading is lost or counted twice at the seam.
    CHECK(n == 120 && (stitched.size() == 2 || stitched.size() == 3));
    for (size_t i = 1; i < stitched.size(); ++i) CHECK(stitched[i].ts > stitched[i - 1].ts);
    CHECK_NEAR(sum, 119.0 * 120 / 2, 1e-9);
    // Retention removes rollups and raw rows per sensor.
    CHECK(store.prune_rollups_before(base + 20000) == 4);  // 2 buckets x 2 sensors
    CHECK(store.prune_before(base + 10000) == 40);
    fs::remove_all(dir);
}

/// Sampler: one thread per provider, aligned ticks, registration, rediscovery.
static void test_sampler() {
    auto root = tmpdir("sampler");
    put(root / "hwmon/hwmon1/name", "coretemp");
    for (int c : {0, 4, 8}) {
        put(root / "hwmon/hwmon1" / ("temp" + std::to_string(c + 2) + "_label"), "Core " + std::to_string(c));
        put(root / "hwmon/hwmon1" / ("temp" + std::to_string(c + 2) + "_input"), "45000");
    }
    put(root / "hwmon/hwmon2/name", "thinkpad");
    put(root / "hwmon/hwmon2/fan1_input", "2000");
    sensors::ProviderContext ctx;
    ctx.hwmon_root = (root / "hwmon").string();
    ctx.power_supply_root = (root / "power_supply").string();
    ctx.thermal_root = (root / "thermal").string();
    sensors::SensorManager mgr(sensors::builtin_providers(), {"cpu_temp", "fan", "battery"}, ctx);

    db::Database store((root / "t.db").string());
    store.migrate();
    bus::Bus bus;
    auto sub = bus.subscribe("test", {msg::topic::kSamplesAll, msg::topic::kSensorsChanged}, 1 << 14);
    pipeline::Sampler sampler(mgr, store, bus, {std::chrono::milliseconds(100), std::chrono::milliseconds(300)});
    CHECK(sampler.start() == 4);
    CHECK(store.list_sensors().size() == 4);
    CHECK(sampler.stats().threads == 3);  // cpu_temp + fan + supervisor (battery has no sensors)

    std::map<int64_t, std::set<std::string>> by_tick;
    std::set<int64_t> ids;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    bool changed = false;
    bool battery_added = false;
    while (std::chrono::steady_clock::now() < until + std::chrono::milliseconds(800)) {
        if (!battery_added && std::chrono::steady_clock::now() > until) {
            put(root / "power_supply/BAT0/type", "Battery");
            put(root / "power_supply/BAT0/capacity", "77");
            battery_added = true;
        }
        for (const auto& m : sub->drain(std::chrono::milliseconds(50))) {
            if (const auto* b = bus::payload_as<msg::SampleBatch>(*m)) {
                CHECK(b->ts % 100 == 0);  // aligned to the interval grid
                by_tick[b->ts].insert(b->source);
                for (const auto& s : b->samples) ids.insert(s.sensor_id);
            } else if (bus::payload_as<msg::SensorsChanged>(*m)) {
                changed = true;
            }
        }
    }
    sampler.stop();
    // Both providers sampled, on the same ticks (one batch each per tick).
    size_t shared = 0;
    for (const auto& [ts, srcs] : by_tick) shared += srcs.count("cpu_temp") && srcs.count("fan");
    CHECK(by_tick.size() >= 6 && shared + 2 >= by_tick.size() - 4);
    CHECK(changed);                                // rediscovery found the battery
    CHECK(ids.size() == 5);                        // 3 cores + fan + battery
    CHECK(sampler.stats().read_failures == 0 && sampler.stats().samples > 20);
    fs::remove_all(root);
}

/// Following a database written by another process, and the sampling lock.
static void test_follower_and_lock() {
    auto dir = tmpdir("follow");
    const auto path = (dir / "t.db").string();
    {
        auto l1 = pipeline::SamplerLock::try_acquire(path);
        CHECK(l1 != nullptr);
        CHECK(pipeline::SamplerLock::try_acquire(path) == nullptr);  // held
        CHECK(pipeline::SamplerLock::holder(path).value_or(0) == ::getpid());
    }
    CHECK(pipeline::SamplerLock::try_acquire(path) != nullptr);  // released with the first

    db::Database store(path);
    store.migrate();
    auto id = store.upsert_sensor({"a", "x", "A", "temperature", "core", "celsius", 0}, 1);
    bus::Bus bus;
    auto sub = bus.subscribe("test", {"samples/db"});
    db::Database fdb(path);
    pipeline::DbFollower f(fdb, bus, std::chrono::milliseconds(50));
    f.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const int64_t now = time::now_ms();
    store.insert_readings(now, {{id, 41.5}});
    store.insert_readings(now + 500, {{id, 42.5}});
    std::vector<msg::SampleBatch> got;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (got.size() < 2 && std::chrono::steady_clock::now() < until)
        for (const auto& m : sub->drain(std::chrono::milliseconds(100)))
            if (const auto* b = bus::payload_as<msg::SampleBatch>(*m)) got.push_back(*b);
    f.stop();
    CHECK(got.size() == 2);
    if (got.size() == 2) {
        CHECK(got[0].ts == now && got[0].samples.size() == 1 && got[0].samples[0].value == 41.5);
        CHECK(got[1].ts == now + 500 && got[1].samples[0].value == 42.5);
    }
    CHECK(pipeline::slugify("BAT0 charge") == "bat0_charge" && pipeline::slugify("Fan 1 (nct6775)") == "fan_1_nct6775");
    auto mo = pipeline::parse_mqtt_url("mqtt://broker.local:1884");
    CHECK(mo.host == "broker.local" && mo.port == 1884 && pipeline::parse_mqtt_url("localhost").port == 1883);
    fs::remove_all(dir);
}

/// A database that holds a package sensor with readings and alarms:
/// migrate() must remove all of them and keep the cores.
static void test_migrate_drops_package() {
    auto dir = tmpdir("mig");
    const auto path = (dir / "t.db").string();
    int64_t core_id = 0;
    {
        db::Database store(path);
        store.migrate();
        sensors::SensorInfo c0{"coretemp/Core 0", "coretemp", "Core 0", "temperature", "core", "celsius", 0};
        core_id = store.upsert_sensor(c0, 1);
    }
    sqlite3* raw = nullptr;
    CHECK(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    const char* legacy =
        "PRAGMA foreign_keys=ON;"
        "INSERT INTO sensors (id, key, chip, label, unit, kind, sort_order, first_seen, last_seen) "
        "VALUES (99, 'coretemp/Package id 0', 'coretemp', 'Package id 0', 'celsius', 'package', 0, 1, 1);"
        "INSERT INTO readings (sensor_id, ts, value) VALUES (99, 1000, 55.0);"
        "INSERT INTO alarms (id, sensor_id, func, window_min, threshold, created_at) "
        "VALUES (7, 99, 'avg', 5, 70, 1);"
        "INSERT INTO alarm_events (alarm_id, sensor_id, ts, type, value, threshold) "
        "VALUES (7, 99, 1000, 'fired', 72, 70);";
    CHECK(sqlite3_exec(raw, legacy, nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);

    db::Database store(path);
    store.migrate();
    auto sensors = store.list_sensors();
    CHECK(sensors.size() == 1 && sensors[0].id == core_id && sensors[0].kind == "core");
    CHECK(!store.sensor_exists(99));
    CHECK(!store.get_alarm(7).has_value());
    CHECK(store.list_events(10, std::nullopt).empty());
    CHECK(store.count_readings(0, 10'000) == 0);
    store.migrate();  // still idempotent
    fs::remove_all(dir);
}

/// A database without the `category` column: migrate() adds it (existing
/// rows are CPU temperatures) and new categories round-trip.
static void test_migrate_adds_category() {
    auto dir = tmpdir("cat");
    const auto path = (dir / "t.db").string();
    sqlite3* raw = nullptr;
    CHECK(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    CHECK(sqlite3_exec(raw,
                       "CREATE TABLE sensors (id INTEGER PRIMARY KEY, key TEXT NOT NULL UNIQUE, chip TEXT NOT NULL, "
                       "label TEXT NOT NULL, unit TEXT NOT NULL, kind TEXT NOT NULL, sort_order INTEGER NOT NULL, "
                       "first_seen INTEGER NOT NULL, last_seen INTEGER NOT NULL);"
                       "INSERT INTO sensors VALUES (5, 'coretemp/Core 4', 'coretemp', 'Core 4', 'celsius', 'core', "
                       "4, 1, 1);",
                       nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);

    db::Database store(path);
    store.migrate();
    store.migrate();
    auto list = store.list_sensors();
    CHECK(list.size() == 1 && list[0].id == 5 && list[0].category == "temperature");
    // Re-registering by key keeps the id (history preserved).
    CHECK(store.upsert_sensor({"coretemp/Core 4", "coretemp", "Core 4", "temperature", "core", "celsius", 4}, 2) == 5);
    auto fan = store.upsert_sensor({"fan:thinkpad/fan1", "thinkpad", "Fan 1", "fan", "fan", "rpm", 10000000}, 2);
    list = store.list_sensors();
    CHECK(list.size() == 2 && list[1].id == fan && list[1].category == "fan" && list[1].unit == "rpm");
    fs::remove_all(dir);
}

/// A database whose alarm tables have no `op` column (alarms that can only
/// fire above the threshold): migrate() adds it with ">" for existing alarms
/// and events, keeps their ids, and new "<" alarms round-trip.
static void test_migrate_adds_op() {
    auto dir = tmpdir("op");
    const auto path = (dir / "t.db").string();
    sqlite3* raw = nullptr;
    CHECK(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    CHECK(sqlite3_exec(raw,
                       "CREATE TABLE sensors (id INTEGER PRIMARY KEY, key TEXT NOT NULL UNIQUE, chip TEXT NOT NULL, "
                       "label TEXT NOT NULL, unit TEXT NOT NULL, kind TEXT NOT NULL, category TEXT NOT NULL "
                       "DEFAULT 'temperature', sort_order INTEGER NOT NULL, first_seen INTEGER NOT NULL, "
                       "last_seen INTEGER NOT NULL);"
                       "CREATE TABLE alarms (id INTEGER PRIMARY KEY, sensor_id INTEGER NOT NULL, func TEXT NOT NULL, "
                       "window_min INTEGER NOT NULL, threshold REAL NOT NULL, enabled INTEGER NOT NULL DEFAULT 1, "
                       "state TEXT NOT NULL DEFAULT 'no_data', last_value REAL, last_eval_ts INTEGER, "
                       "last_change_ts INTEGER, created_at INTEGER NOT NULL);"
                       "CREATE TABLE alarm_events (id INTEGER PRIMARY KEY, alarm_id INTEGER NOT NULL REFERENCES "
                       "alarms(id) ON DELETE CASCADE, sensor_id INTEGER NOT NULL, ts INTEGER NOT NULL, "
                       "type TEXT NOT NULL, value REAL, threshold REAL NOT NULL);"
                       "INSERT INTO sensors VALUES (1, 'coretemp/Core 0', 'coretemp', 'Core 0', 'celsius', 'core', "
                       "'temperature', 0, 1, 1);"
                       "INSERT INTO alarms (id, sensor_id, func, window_min, threshold, created_at) "
                       "VALUES (7, 1, 'avg', 10, 75, 1);"
                       "INSERT INTO alarm_events VALUES (3, 7, 1, 5, 'fired', 80, 75);",
                       nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);

    db::Database store(path);
    store.migrate();
    store.migrate();  // idempotent
    auto a = store.get_alarm(7);
    CHECK(a && a->op == ">" && a->threshold == 75.0);
    auto ev = store.list_events(10, 7);
    CHECK(ev.size() == 1 && ev[0].id == 3 && ev[0].op == ">");
    auto b = store.create_alarm(1, "min", 5, 30.0, 2, "<");
    CHECK(store.get_alarm(b)->op == "<");
    CHECK(store.record_evaluation(*store.get_alarm(b), "firing", 25.0, 3));
    ev = store.list_events(10, b);
    CHECK(ev.size() == 1 && ev[0].op == "<");
    fs::remove_all(dir);
}

/// @brief Test runner.
///
/// Usage:
/// @verbatim
///   sd-tests                run every group
///   sd-tests --list         list group names
///   sd-tests fan battery    run groups whose name contains any of the words
/// @endverbatim
/// @return 0 if every check passed, 1 if any failed, 2 if no group matched.
int main(int argc, char** argv) {
    std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"cpu_temp_single_socket", test_cpu_temp_single_socket},
        {"cpu_temp_multi_socket", test_cpu_temp_multi_socket},
        {"cpu_temp_fallbacks", test_cpu_temp_fallbacks},
        {"fan_provider", test_fan_provider},
        {"battery_provider", test_battery_provider},
        {"registry_and_manager", test_registry_and_manager},
        {"custom_provider", test_custom_provider},
        {"store_and_aggregates", test_store_and_aggregates},
        {"migrate_drops_package", test_migrate_drops_package},
        {"migrate_adds_category", test_migrate_adds_category},
        {"migrate_adds_op", test_migrate_adds_op},
        {"alarm_logic", test_alarm_logic},
        {"alarm_below_threshold", test_alarm_below_threshold},
        {"alarm_engine_per_core", test_alarm_engine_per_core},
        {"bus", test_bus},
        {"second_window", test_second_window},
        {"db_writer", test_db_writer},
        {"sampler", test_sampler},
        {"follower_and_lock", test_follower_and_lock},
    };
    std::vector<std::string> filters(argv + 1, argv + argc);
    if (filters.size() == 1 && filters[0] == "--list") {
        for (auto& t : tests) std::cout << t.first << "\n";
        return 0;
    }
    int ran = 0;
    for (auto& [name, fn] : tests) {
        if (!filters.empty() && std::none_of(filters.begin(), filters.end(), [&](const std::string& f) {
                return std::string(name).find(f) != std::string::npos;
            }))
            continue;
        ++ran;
        int before = g_failed;
        try { fn(); } catch (const std::exception& e) { ++g_failed; std::cerr << "  EXCEPTION " << e.what() << "\n"; }
        std::cout << (g_failed == before ? "[PASS] " : "[FAIL] ") << name << "\n";
    }
    if (ran == 0) {
        std::cerr << "no test group matches; use --list\n";
        return 2;
    }
    std::cout << ran << " group(s), " << g_passed << " checks passed, " << g_failed << " failed\n";
    return g_failed ? 1 : 0;
}

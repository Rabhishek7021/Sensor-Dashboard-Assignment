/**
 * @file main.cpp
 * @brief sensor-poller: headless sampling into SQLite (no HTTP server).
 *
 * The dashboard server samples by itself, so this binary is only needed to
 * record without the dashboard, or to keep sampling in a separate process.
 * It runs the same pipeline as the server:
 *
 * @verbatim
   Sampler threads (one per provider, every 500 ms) --bus--> DbWriter thread
                                                         \-> MqttBridge thread (optional)
   @endverbatim
 *
 * Only one process may sample into a database; if the server is already
 * sampling, the poller exits. If the poller is started first, a server
 * started later follows the database instead of sampling.
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#include "../bus/bus.hpp"
#include "../common/db.hpp"
#include "../common/log.hpp"
#include "../pipeline/messages.hpp"
#include "../pipeline/options.hpp"
#include "../pipeline/source.hpp"
#include "../sensors/hwmon.hpp"
#include "../sensors/registry.hpp"

namespace {

/// @brief Set by SIGINT / SIGTERM to stop cleanly.
std::atomic<bool> g_stop{false};
/// @brief Signal handler: requests shutdown.
void on_signal(int) { g_stop = true; }

/// @brief Parsed command line of sensor-poller.
struct Config {
    std::string db_path = "data/sensors.db";  ///< SQLite file (--db, env SENSOR_DB).
    sd::pipeline::PipelineConfig pipe;  ///< Shared sampling / storage / MQTT settings.
    ///@{
    bool list_only = false, list_providers = false, verbose = false;  ///< --list, --list-providers, --verbose.
    ///@}
};

/// @brief Prints the command-line help.
void usage() {
    std::cout << "Usage: sensor-poller [options]\n"
                 "  --db PATH                 SQLite file (default data/sensors.db)\n"
                 "  --list                    print discovered sensors and current values, then exit\n"
                 "  --list-providers          print available providers, then exit\n"
                 "  --verbose                 log every sample\n"
              << sd::pipeline::pipeline_usage() << "Environment: SENSOR_DB\n";
}

/// @brief Parses the command line and environment; exits on --help or invalid input.
Config parse(int argc, char** argv) {
    Config c;
    if (const char* v = std::getenv("SENSOR_DB")) c.db_path = v;
    sd::pipeline::apply_pipeline_env(c.pipe);
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::cerr << "missing value for " << a << "\n"; std::exit(2); }
            return argv[++i];
        };
        if (sd::pipeline::parse_pipeline_flag(a, next, c.pipe)) continue;
        if (a == "--db") c.db_path = next();
        else if (a == "--list") c.list_only = true;
        else if (a == "--list-providers") c.list_providers = true;
        else if (a == "--verbose") c.verbose = true;
        else if (a == "-h" || a == "--help") { usage(); std::exit(0); }
        else { std::cerr << "unknown option " << a << "\n"; usage(); std::exit(2); }
    }
    if (auto err = sd::pipeline::validate(c.pipe); !err.empty()) {
        std::cerr << err << "\n";
        std::exit(2);
    }
    return c;
}

}  // namespace

/// @brief Entry point: --list / --list-providers, or sample until SIGINT / SIGTERM.
/// @return 0 on clean exit, non-zero on error (no sensors, database or lock failure).
int main(int argc, char** argv) {
    using namespace sd;
    Config cfg = parse(argc, argv);
    if (cfg.verbose) log::threshold() = log::Level::Debug;
    const auto& registry = sensors::builtin_providers();

    if (cfg.list_providers) {
        for (const auto& e : registry.entries())
            std::cout << std::left << std::setw(10) << e.name << e.description << "\n";
        return 0;
    }

    std::unique_ptr<sensors::SensorManager> manager;
    try {
        manager = std::make_unique<sensors::SensorManager>(
            registry, sensors::parse_provider_list(cfg.pipe.providers, registry), cfg.pipe.ctx);
    } catch (const sensors::UnknownProvider& e) {
        std::cerr << e.what() << " (use --list-providers)\n";
        return 2;
    }
    if (manager->enabled().empty()) {
        std::cerr << "no providers enabled\n";
        return 2;
    }

    auto no_sensors = [&] {
        std::ostringstream chips;
        for (auto& n : hwmon::chip_names(cfg.pipe.ctx.hwmon_root)) chips << n << ' ';
        log::error("no sensors found for providers {", cfg.pipe.providers, "}. hwmon chips present: [", chips.str(),
                   "]. For CPU temperatures load the driver (e.g. `sudo modprobe coretemp` or `k10temp`) "
                   "or pass --chips.");
    };

    if (cfg.list_only) {
        auto d = manager->discover();
        if (d.sensors.empty()) {
            no_sensors();
            return 1;
        }
        std::cout << std::left << std::setw(30) << "KEY" << std::setw(13) << "CATEGORY" << std::setw(8) << "KIND"
                  << "VALUE\n";
        for (const auto& s : d.sensors) {
            const auto& i = s->info();
            auto v = s->read();
            std::ostringstream val;
            if (v) val << std::fixed << std::setprecision(1) << *v << ' ' << i.unit;
            else val << "unreadable";
            std::cout << std::setw(30) << i.key << std::setw(13) << i.category << std::setw(8) << i.kind
                      << val.str() << "\n";
        }
        return 0;
    }

    auto lock = pipeline::SamplerLock::try_acquire(cfg.db_path);
    if (!lock) {
        const auto pid = pipeline::SamplerLock::holder(cfg.db_path);
        log::error("another process", pid ? " (pid " + std::to_string(*pid) + ")" : std::string(),
                   " is already sampling into ", cfg.db_path,
                   ". The dashboard server samples by itself; stop it or use another --db.");
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    try {
        db::Database store(cfg.db_path);
        store.migrate();
        bus::Bus bus;

        pipeline::DbWriter writer(store, bus, cfg.pipe.writer);
        std::unique_ptr<pipeline::MqttBridge> mqtt;
        if (!cfg.pipe.mqtt_url.empty()) mqtt = std::make_unique<pipeline::MqttBridge>(bus, store, pipeline::mqtt_options(cfg.pipe));
        std::shared_ptr<bus::Subscription> debug_sub;
        if (cfg.verbose) debug_sub = bus.subscribe("verbose-log", {msg::topic::kSamplesAll}, 1024);

        pipeline::Sampler sampler(*manager, store, bus, cfg.pipe.sampler);
        writer.start();
        if (mqtt && !mqtt->start()) return 1;
        if (sampler.start() == 0) {
            no_sensors();
            writer.stop();
            return 1;
        }
        log::info("discovered ", sampler.summary(), "; sampling every ", cfg.pipe.sampler.interval.count(),
                  " ms into ", cfg.db_path);

        auto last_report = std::chrono::steady_clock::now();
        while (!g_stop) {
            if (debug_sub) {
                for (const auto& m : debug_sub->drain(std::chrono::milliseconds(200))) {
                    if (const auto* b = bus::payload_as<msg::SampleBatch>(*m)) {
                        std::ostringstream os;
                        for (size_t i = 0; i < b->samples.size(); ++i)
                            os << (i ? ", " : "") << std::fixed << std::setprecision(1) << b->samples[i].value;
                        log::debug(b->source, " @", b->ts, ": ", os.str());
                    }
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            if (std::chrono::steady_clock::now() - last_report > std::chrono::minutes(10)) {
                last_report = std::chrono::steady_clock::now();
                const auto s = sampler.stats();
                const auto w = writer.stats();
                log::info("stats: ", s.samples, " samples, ", s.read_failures, " read failures, ", w.rows,
                          " rows stored in ", w.flushes, " flushes (last ", w.last_flush_ms, " ms)");
            }
        }
        log::info("shutting down");
        sampler.stop();
        writer.stop();  // flushes everything still buffered
        if (mqtt) mqtt->stop();
    } catch (const std::exception& e) {
        log::error("fatal: ", e.what());
        return 1;
    }
    return 0;
}

# Multi-Sensor Dashboard

An end-to-end system that discovers the **hardware sensors of the Linux machine it runs on** and monitors them live. Three sensor families are built in:

| Family               | What is measured                                                                                                                 | Source                                                               | Unit |
| -------------------- | -------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------- | ---- |
| **CPU temperatures** | One sensor per physical core (the package sensor is deliberately ignored); per die or whole CPU on CPUs without per-core sensors | hwmon `coretemp` / `k10temp` / `zenpower`, thermal zones as fallback | °C   |
| **Fans**             | Every fan the firmware exposes                                                                                                   | hwmon `fan<N>_input` (any driver)                                    | RPM  |
| **Batteries**        | Charge level of every system battery                                                                                             | `/sys/class/power_supply`                                            | %    |

All sensors are sampled **every 500 ms**, stored in SQLite, served through a C++ HTTP API and a live event stream, and visualised in a React dashboard with one tab per sensor family. Every sensor, whatever its family, can carry its **own independent statistical alarms** (`avg | max | min | stddev` over the last *X* minutes, `> Y` or `< Y`): for example a hot core (`avg > 75 °C`), a racing fan (`max > 4500 RPM`) or a battery running low (`min < 20 %`).

| Part                       | Status                  | Implementation                                                                                                                                                   |
| -------------------------- | ----------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1. Read hardware sensors   | Done                    | C++17, Linux sysfs: CPU core temperatures (auto-discovers N cores), fan speeds and battery charge, as pluggable providers; one sampling thread per sensor family |
| 2. Persistent store        | Done                    | SQLite (WAL), batched writer thread, 10 s rollups, retention                                                                                                     |
| 3. Backend API + dashboard | Done                    | C++ `cpp-httplib` + `nlohmann/json`, Server-Sent Events; React 18 + Recharts 3 (Vite 5 build)                                                                    |
| 4. Alarm engine (bonus)    | Done ("extended" scope) | Per-sensor alarms on in-memory sliding windows, CRUD API, state + firing history, UI                                                                             |

Internally every part is connected through a **multi-threaded publish/subscribe bus with MQTT-style topics**. Sampling, storage, alarms, live dashboards and an optional bridge to a real **MQTT broker** each run on their own thread with their own queue, so none of them can slow down another (see [Architecture](#3-architecture) and [Performance](#4-performance)).

The sensor layer is a set of pluggable **providers**, one per family: `cpu_temp`, `fan` and `battery`. `scripts/run_all.sh` enables all of them by default; the `sensor-server` and `sensor-poller` binaries default to `cpu_temp` and take `--providers` (for example `--providers all` or `--providers cpu_temp,fan`). Nothing above the sensor layer is specific to a family: the database, API, alarms, MQTT bridge and dashboard work with generic sensors (key, label, category, unit), so a new family (GPU temperature, voltages, disk temperatures, ...) is one class plus one registration line (see [Adding a new sensor type](#adding-a-new-sensor-type)).

## 1. Quick start

**Prerequisites** are set up by a script (tested on Ubuntu 24.04 with GCC 13, CMake 3.28, Node 22):

```bash
scripts/setup_env.sh              # check the requirements, install anything missing
scripts/setup_env.sh --all        # also the optional MQTT bridge and documentation tools
```

It prints every required package with the installed version, and installs what is missing or too old through the system package manager (apt, dnf, pacman or zypper; it uses `sudo` when not run as root). On a machine that already has everything it only prints the table:

```
==> build environment (package manager: apt-get)
    Package                    Minimum    Installed    Status
    C++17 compiler (gcc)       >= 8       13.3.0       ok
    make                       >= 3.81    4.3          ok
    CMake                      >= 3.16    3.28.3       ok
    SQLite headers             >= 3.8.2   3.45.1       ok
    zlib headers               >= 1.2     1.3          ok
    Node.js                    >= 18      22.23.4      ok
    npm                        >= 8       10.9.9       ok
    libmosquitto (MQTT)        >= 1.4     -            not installed (optional: --with-mqtt)
    Doxygen                    >= 1.9     1.9.8        ok (optional)
    Graphviz (dot)             >= 2.0     2.43.0       ok (optional)
==> all required packages are installed
```

`scripts/run_all.sh` runs this check **on every build**, so a fresh machine needs no manual installation step. Options: `--check` (report only, exit 1 if something is missing), `--with-mqtt`, `--with-docs`, `--all`.

Requirements: a C++17 compiler (GCC ≥ 8 or Clang ≥ 7), make, CMake ≥ 3.16, SQLite headers ≥ 3.8.2, zlib headers, Node.js ≥ 18 and npm. Optional: libmosquitto (MQTT bridge), Doxygen ≥ 1.9 and Graphviz (code documentation). If the distribution's Node.js is older than 18, the script says so and shows how to install a current one (NodeSource or nvm). The build uses only standard Linux sysfs, so it runs on any distribution and any architecture (x86-64, ARM). The dashboard is built from `frontend/src` with Node.js ≥ 18 and npm; `scripts/run_all.sh` does this automatically on the first run.

**One command** (checks the environment, builds everything, starts the server, which also samples):

```bash
scripts/run_all.sh            # real sensors: CPU cores, fans and batteries
SENSOR_PROVIDERS=cpu_temp scripts/run_all.sh   # CPU core temperatures only
# open http://localhost:8080
```

No root privileges are needed: the hwmon (`temp*_input`, `fan*_input`) and power_supply files are world-readable.

Which families appear depends on the machine: a desktop has no battery, and many laptops hide fan speed from Linux. Families with no sensors are simply left out of the dashboard.

No hardware sensors on this machine (VM, container, CI)? Run against a simulated 10-core laptop with two fans and a battery:

```bash
scripts/run_all.sh --fake
```

Optional MQTT bridge: `MQTT_URL=localhost:1883 scripts/run_all.sh` makes the environment check install libmosquitto as well (or run `scripts/setup_env.sh --with-mqtt` once; manual flag: `--mqtt host:port`). The build prints `MQTT bridge: enabled` or `disabled`. Without the library everything else works the same.

`run_all.sh` also reads `SETUP_OPTS` (extra options for the environment check, e.g. `SETUP_OPTS=--with-docs`) and `SKIP_SETUP=1` (skip the check, e.g. offline or without sudo).

### Building and running each component manually

```bash
# C++: server, poller, unit tests
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j        # ends by printing the sensors detected on this machine
ctest --test-dir build --output-on-failure

# Frontend
cd frontend && npm ci && npm run build && cd ..

# See what this machine exposes
./build/sensor-poller --list-providers
./build/sensor-poller --list --providers all

# Everything in one process: sampling, storage, alarms, API, live stream, dashboard
./build/sensor-server --db data/sensors.db --port 8080 --static frontend/dist
./build/sensor-server --providers all --mqtt localhost:1883   # + fans, battery, MQTT bridge

# Optional: record without the dashboard (headless). A server started later on
# the same --db follows this poller instead of sampling a second time.
./build/sensor-poller --db data/sensors.db
```

Frontend dev mode with hot reload: `cd frontend && npm run dev` (port 5173; `/api` is proxied to `:8080`).

### Code documentation (Doxygen)

Every source file (C++ in `src/` and `tests/`, JavaScript in `frontend/src/`) carries Doxygen comments, and a `Doxyfile` at the project root turns them into a browsable HTML site:

```bash
scripts/setup_env.sh --with-docs      # once: installs Doxygen and Graphviz if missing
doxygen Doxyfile                      # from the project root, a few seconds
xdg-open docs/html/index.html
```

The site has this README as its main page, `Project-Overview.md` and `TESTING.md` as related pages, and a reference page for every namespace, class, file, function and frontend module. `docs/` is generated (ignored by Git, removed by `scripts/clean.sh`); warnings go to `docs/doxygen-warnings.log`. Without Graphviz, set `HAVE_DOT = NO` in the `Doxyfile`.

### Configuration

Sampling, storage and MQTT flags are the same for `sensor-server` and `sensor-poller`.

| Binary | Flag                                                      | Default                     | Notes                                                                                              |
| ------ | --------------------------------------------------------- | --------------------------- | -------------------------------------------------------------------------------------------------- |
| both   | `--interval-ms N` (or `--interval S`)                     | `500`                       | Sampling period (env `POLL_INTERVAL_MS`); minimum 50                                               |
| both   | `--db PATH`                                               | `data/sensors.db`           | env `SENSOR_DB`                                                                                    |
| both   | `--providers a,b`                                         | `cpu_temp`                  | Sensor families: `cpu_temp`, `fan`, `battery` or `all` (env `SENSOR_PROVIDERS`)                    |
| both   | `--hwmon-root` / `--power-supply-root` / `--thermal-root` | `/sys/class/...`            | Point at a simulated tree (env `HWMON_ROOT`, ...)                                                  |
| both   | `--chips a,b`                                             | `coretemp,k10temp,zenpower` | hwmon drivers treated as CPU sensors                                                               |
| both   | `--rediscover S`                                          | `60`                        | Re-run discovery this often (hardware that appears later); `0` = never                             |
| both   | `--flush-ms N`                                            | `1000`                      | The DB writer commits buffered readings this often                                                 |
| both   | `--raw-retention-hours N`                                 | `24`                        | Raw 500 ms readings kept this long (`0` = forever)                                                 |
| both   | `--retention-days N`                                      | `7`                         | 10 s rollups kept this long (`0` = forever)                                                        |
| both   | `--mqtt HOST[:PORT]`                                      | off                         | Publish to an MQTT broker (env `MQTT_URL`); also `--mqtt-prefix`, `--mqtt-user`, `--mqtt-password` |
| server | `--port N` / `--host`                                     | `8080` / `0.0.0.0`          | env `PORT`                                                                                         |
| server | `--static DIR`                                            | `frontend/dist`             | Built dashboard                                                                                    |
| server | `--source auto\|local\|db`                                | `auto`                      | Sample here, or follow a DB another process samples into (see [Process model](#process-model))     |
| server | `--alarm-interval S`                                      | `1`                         | Alarm evaluation period                                                                            |
| server | `--alarm-min-samples N`                                   | `3`                         | Samples needed in a window before an alarm can fire                                                |
| server | `--max-live-clients N`                                    | `32`                        | Concurrent live dashboard streams                                                                  |
| poller | `--list` / `--list-providers` / `--verbose`               |                             | Print sensors / providers / log every sample                                                       |
| build  | `-DSD_REPORT_SENSORS=OFF`                                 | `ON`                        | Skip the post-build sensor report                                                                  |
| build  | `-DSD_WITH_MQTT=ON\|OFF`                                  | `AUTO`                      | Require or disable the MQTT bridge (default: enabled if libmosquitto is found)                     |

## 2. Sensors

All built-in sensors are read from Linux **sysfs**, where every value is a small text file:

| Provider (`--providers`) | Dashboard tab    | Sensors found                                                                                                                                                                               | Key (stable across reboots) | Label                                                       |
| ------------------------ | ---------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------- | ----------------------------------------------------------- |
| `cpu_temp`               | CPU temperatures | Every `Core N` channel of the CPU's hwmon driver (fallbacks below)                                                                                                                          | `coretemp/Core 4`           | `Core 4`                                                    |
| `fan`                    | Fans             | Every `fan<N>_input` channel on any hwmon chip (thinkpad, dell_smm, asus-ec, nct6775, it87, ...); a stopped fan reads 0 RPM                                                                 | `fan:thinkpad/fan1`         | `Fan 1` (chip name added when fans come from several chips) |
| `battery`                | Battery          | Every `power_supply` entry of type `Battery`, except peripheral batteries (mice, keyboards: scope `Device`); charge from `capacity`, or `energy_now/energy_full` / `charge_now/charge_full` | `battery:BAT0/charge`       | `BAT0 charge`                                               |

`./build/sensor-poller --list-providers` lists the providers and `./build/sensor-poller --list --providers all` shows what this machine exposes. Each family is sampled on its own thread, so a slow device (a battery behind an ACPI embedded controller, for example) never delays the CPU core readings.

**Why read sysfs directly instead of `libsensors`:** hwmon and power_supply values are plain text files (`temp<N>_input` in millidegrees Celsius, `fan<N>_input` in RPM, `capacity` in percent). Reading them is what `libsensors` does internally. Skipping the library removes a runtime dependency, avoids its config-file layer and keeps discovery logic explicit and testable. `libiio` targets IIO devices (accelerometers, ADCs); these sensors live in hwmon and power_supply, not IIO.

### CPU core temperatures (the primary family)

**Sensor:** CPU temperature from the Linux **hwmon** subsystem, specifically the `coretemp` driver (Intel). `k10temp`/`zenpower` (AMD) are also supported.

**Why it is the primary family:**

- It is a real hardware sensor (digital thermal sensors on the die), not a simulated value.
- It responds within seconds to load (e.g. `stress -c 8`), so the chart is visibly live.
- It maps directly onto the brief's example alarm (`avg(temp, 10 min) > 75 °C`).
- It is exposed per core, which makes per-core monitoring and per-core alarms meaningful.

**Development machine** (HP laptop, Intel), as reported by `sensors`:

```
coretemp-isa-0000
Package id 0:  +54.0°C      Core 0: +40.0°C   Core 4: +42.0°C   Core 8: +42.0°C
Core 12: +40.0°C            Core 16: +44.0°C  Core 20: +43.0°C  Core 24: +45.0°C
Core 25: +45.0°C            Core 26: +45.0°C  Core 27: +46.0°C
```

That is **10 cores** (the `Package id 0` line is a whole-CPU reading and is not tracked). Core numbers are **not contiguous** (0, 4, 8 … 24-27): the number in each `Core N` label is the kernel's physical core id (`/sys/devices/system/cpu/cpu*/topology/core_id`), not a 0..9 index, which is typical of hybrid P/E-core CPUs. The dashboard keeps these labels exactly as the kernel reports them, so `Core 4` in the UI is the same core as `Core 4` in `sensors`, and orders them numerically: Core 0, Core 4, Core 8 … Core 27. Nothing in this project assumes a core count or numbering.

#### How multi-core discovery works (any core count, any Linux)

The core count is **never compiled in or configured**. Each time sampling starts (and every `--rediscover` seconds after), the `cpu_temp` provider:

1. Scans every `/sys/class/hwmon/hwmon*` directory and keeps those whose `name` is a CPU driver (`coretemp` for Intel, `k10temp`/`zenpower` for AMD). Other chips (`acpitz`, `nvme`, fan controllers) are left to other providers.
2. Reads every `temp<N>_input` channel and its `temp<N>_label`.
3. Uses the **most detailed level the CPU exposes**:
   - **per core** (`Core N`): Intel. This is the normal case, and the package sensor is skipped.
   - **per die** (`Tccd N`): AMD Ryzen/EPYC, which have no per-core sensors.
   - **whole CPU** (`Tctl`/`Tdie`/`Package id`, then `/sys/class/thermal` zones of type `cpu*`/`soc*`): older AMD, ARM boards such as the Raspberry Pi, and some VMs.
4. Sorts by socket, then by the number in the label (Core 0, Core 4, Core 8 …).
5. Gives each sensor a **stable key** such as `coretemp/Core 4`, so database ids and history survive restarts. The `hwmonN` index is deliberately *not* used, because the kernel may renumber it at boot.
6. On multi-socket machines (one `coretemp` device per socket, with repeated core labels), qualifies keys with the device id (`coretemp.coretemp.1/Core 0`) and prefixes labels with `CPU1`.

So the same binary reports 4 cores on a quad-core desktop, 10 on the dev laptop and 2 × 64 on a dual-socket server. At the end of every build, CMake runs the freshly built poller once with all providers and prints what it found: the CPU core count (`-- Sensors detected on this machine (10 CPU core temperature sensor(s))`) followed by every sensor, fans and batteries included. This is a check for the build machine only: the real discovery always happens at run time on the machine the binary runs on, so binaries can be copied to other hosts.

If no enabled provider finds anything, the poller exits with an actionable error that lists the hwmon chips that *are* present and suggests `modprobe coretemp` or `--chips`.

### Fans and batteries

Fans and batteries are discovered the same way, at start and on every rediscovery, so the same binary works on a desktop with five fans and no battery or on a laptop with one battery and no visible fans. A battery that appears later (or a fan driver loaded later) is picked up without a restart. Fan speeds are integers in RPM; battery charge is 0-100 %, computed from now/full counters when the firmware has no `capacity` file.

Like CPU cores, every fan and battery is an ordinary sensor everywhere above the sensor layer: it is stored, has its own history, gets its own chart, can carry its own alarms (for example `max(Fan 1, 5 min) > 4500` RPM) and is published to MQTT.

## 3. Architecture

```
   /sys/class/hwmon · /sys/class/power_supply · /sys/class/thermal
                              │
       CpuTempProvider · FanProvider · BatteryProvider · …      SensorProvider (Strategy),
                              │                                 built by ProviderRegistry (Factory),
                       SensorManager (Facade)                   combined by SensorManager
                              │
   ┌──────────────────────────▼────────────────────────────┐
   │ Sampler: 1 thread per provider, every 500 ms,          │  one batch per provider per tick,
   │ all on the same clock grid (shared timestamps)         │  topic "samples/<provider>"
   └──────────────────────────┬────────────────────────────┘
                              ▼ publish
   ════════════ in-process pub/sub bus (MQTT-style topics, one queue per subscriber) ════════════
        │                 │                 │                    │                   │
        ▼ samples/#       ▼ samples/#       ▼ samples/#          ▼ samples/#,       ▼ samples/#,
   ┌──────────┐     ┌────────────┐    ┌───────────┐        alarms/…, sensors/…   alarms/+/state
   │ DbWriter │     │ AlarmEngine│    │ LiveCache │     ┌──────────────────┐  ┌──────────────┐
   │ thread   │     │ thread     │    │ thread    │     │ SSE stream per   │  │ MqttBridge   │
   │ batches, │     │ in-memory  │    │ latest    │     │ open dashboard   │  │ (optional)   │
   │ rollups, │     │ windows,   │    │ values,   │     │ (HTTP worker)    │  │ → Mosquitto  │
   │ retention│     │ publishes  │    │ health    │     └────────┬─────────┘  └──────────────┘
   └────┬─────┘     │ alarms/…   │    └───────────┘              │ text/event-stream
        ▼           └────────────┘                               ▼
   SQLite (WAL)  ◀── REST API (HTTP worker pool) ───────── React dashboard
   readings · readings_10s · sensors · alarms · alarm_events
```

```
src/bus/                 library sd_bus: Bus, Subscription, MQTT topic matching (+, #)
src/sensors/             library sd_sensors: hardware discovery only (no DB, no HTTP)
  sensor.hpp             SensorInfo + abstract Sensor; SysfsValueSensor (file × scale)
  provider.hpp           SensorProvider interface + ProviderContext (sysfs roots)
  registry.{hpp,cpp}     ProviderRegistry; builtin_providers() = the one place providers are listed
  manager.{hpp,cpp}      SensorManager facade
  hwmon.* · sysfs.*      generic hwmon enumeration, exception-free file helpers
  providers/             cpu_temp.* · fan.* · battery.*
src/pipeline/            library sd_pipeline: the threads around the bus
  messages.hpp           topics and payload types
  sampler.*              per-provider sampling threads + rediscovery supervisor
  db_writer.*            batched storage, rollups, retention
  source.*               SamplerLock (one sampler per DB) and DbFollower
  mqtt_bridge.*          optional libmosquitto bridge
  options.hpp            command-line options shared by server and poller
src/common/              library sd_common: db.* (SQLite), alarm_logic.hpp, alarm_window.hpp
src/server/              main.cpp (HTTP, SSE, static files), alarm_engine.*, live_cache.hpp
src/poller/main.cpp      headless sampler (same pipeline, no HTTP)
tests/test_main.cpp      19 test groups, see Testing
frontend/src/            App.jsx · live.js (stream + buckets) · TempChart.jsx · AlarmPanel.jsx · units.js · api.js
scripts/                 setup_env.sh (environment check / install) · run_all.sh · fake_hwmon.sh · clean.sh · doxygen-md-filter.sh
Doxyfile                 Doxygen configuration (code documentation → docs/html)
third_party/             httplib.h (MIT) · nlohmann/json.hpp (MIT); vendored so the build needs no network
```

**Design patterns.**

- **Publish/subscribe (Observer through a message bus):** producers publish to topics and never know who consumes them. Adding a consumer (say a CSV exporter or a webhook notifier) is one more subscriber thread, with no change to sampling, storage or alarms. Topics follow MQTT conventions, so the optional MQTT bridge maps them one-to-one onto a real broker.
- **Strategy:** each `SensorProvider` encapsulates how one sensor family is found.
- **Factory + Registry:** `ProviderRegistry` maps a name (`fan`) to a factory; the binaries build providers from `--providers` without knowing any concrete class.
- **Facade:** `SensorManager` hides the providers behind one `discover()`. One failing provider is logged and skipped.
- **Polymorphic Sensor:** most Linux sensors are "an integer in a sysfs file × scale" (`SysfsValueSensor`). Anything else subclasses `Sensor` (the battery provider does, for `energy_now / energy_full`).
- **Layering:** `sd_sensors` (hardware), `sd_bus` (messaging), `sd_common` (storage) and `sd_pipeline` (threads) are separate libraries. The database and dashboard deal in generic `(key, label, category, kind, unit)` rows, so neither knows what a "core" or a "fan" is.

#### Threads and topics

| Thread                              | Subscribes to                                                       | Publishes                              | Notes                                                          |
| ----------------------------------- | ------------------------------------------------------------------- | -------------------------------------- | -------------------------------------------------------------- |
| Sampler, one per provider           |                                                                     | `samples/<provider>`                   | Aligned 500 ms ticks; reads are plain `open/read/close`        |
| Sampler supervisor                  |                                                                     | `sensors/changed`                      | Rediscovery every 60 s                                         |
| DbWriter                            | `samples/#`                                                         |                                        | One transaction per second; rollups every 10 s; pruning hourly |
| AlarmEngine                         | `samples/#`, `control/alarms/reload`, `sensors/changed`             | `alarms/<id>/state`, `alarms/snapshot` | Evaluates every second                                         |
| LiveCache                           | `samples/#`                                                         |                                        | Latest value per sensor, for `/api/health` and `/api/sensors`  |
| SSE stream (one per open dashboard) | `samples/#`, `alarms/+/state`, `alarms/snapshot`, `sensors/changed` |                                        | Runs on an HTTP worker; merges providers' ticks into one event |
| MqttBridge (optional)               | `samples/#`, `alarms/+/state`, `sensors/changed`                    | (to broker)                            | libmosquitto adds its own network thread                       |
| HTTP worker pool                    |                                                                     | `control/alarms/reload`                | 8 + `--max-live-clients` workers                               |
| DbFollower (follow mode only)       |                                                                     | `samples/db`                           | Replaces the sampler when another process samples              |

Each subscriber has a bounded queue (64 Ki messages for storage and alarms, 512 for a browser). A full queue drops its *oldest* messages and counts them in `/api/stats`, so a stalled browser can never block sampling or storage.

### Adding a new sensor type

Example: GPU temperature from the `amdgpu` hwmon driver.

1. Create `src/sensors/providers/gpu_temp.{hpp,cpp}`:
   
   ```cpp
   class GpuTempProvider : public SensorProvider {
   public:
       explicit GpuTempProvider(ProviderContext ctx) : ctx_(std::move(ctx)) {}
       std::string name() const override { return "gpu_temp"; }
       std::vector<std::unique_ptr<Sensor>> discover() override {
           std::vector<std::unique_ptr<Sensor>> out;
           for (const auto& dev : hwmon::scan(ctx_.hwmon_root, [](auto& c) { return c == "amdgpu"; }))
               for (const auto& ch : hwmon::channels(dev, "temp"))
                   out.push_back(std::make_unique<SysfsValueSensor>(
                       SensorInfo{"gpu:" + dev.key_prefix() + "/" + ch.label, dev.chip, "GPU " + ch.label,
                                  "gpu", "edge", unit::kCelsius, ch.index},
                       ch.input.string(), 0.001));   // millidegrees -> °C
           return out;
       }
   private:
       ProviderContext ctx_;
   };
   ```
2. Register it in `builtin_providers()` (`src/sensors/registry.cpp`):
   
   ```cpp
   r.add("gpu_temp", "GPU temperatures (amdgpu)",
         [](const ProviderContext& c) { return std::make_unique<GpuTempProvider>(c); });
   ```
3. Add `src/sensors/providers/gpu_temp.cpp` to `sd_sensors` in `CMakeLists.txt`, and add a test that builds a fake sysfs tree (see `test_fan_provider`).
4. Run with `--providers cpu_temp,gpu_temp`.

Nothing else changes. The new sensors are sampled on their own thread, stored, streamed, published to MQTT, get their own dashboard tab and can carry alarms. Optionally, add a title and value format for the new category or unit in `frontend/src/units.js`; unknown ones are displayed generically.

### Process model

By default `sensor-server` does everything in one process. Exactly one process may sample into a database, otherwise each reading would be stored twice. This is enforced with an exclusive `flock()` on `<db>.sampler.lock`, which the kernel releases if the process dies:

- **server alone (default):** the server takes the lock and samples itself.
- **poller + server:** if a `sensor-poller` already holds the lock, the server *follows* the database instead: a DbFollower thread reads the poller's new rows every 500 ms and publishes them on the bus, so alarms, live dashboards and MQTT work the same. Live data then lags by up to about 1.5 s (the poller's flush interval plus the follow interval). When the poller exits, the server takes over sampling within 2 s.
- **a second sampler** (poller or `--source local` server) on the same database is refused with a clear message.

### MQTT bridge

With `--mqtt host[:port]` (and a build with libmosquitto) the bus is mirrored to a broker under `sensordash/<hostname>/` (change with `--mqtt-prefix`):

| Topic                          | Payload                                                                                                                         | QoS / retained |
| ------------------------------ | ------------------------------------------------------------------------------------------------------------------------------- | -------------- |
| `<prefix>/status`              | `online` / `offline` (also the last will, sent by the broker if the process dies)                                               | 1, retained    |
| `<prefix>/sensors/<slug>/meta` | `{"id","key","label","category","kind","unit"}`                                                                                 | 1, retained    |
| `<prefix>/<category>/<slug>`   | `{"ts":1793347475500,"value":75.002,"unit":"celsius"}` every sample                                                             | 0              |
| `<prefix>/alarms/<id>`         | `{"id","sensor","condition","state","from","value","ts"}` on every transition (condition such as `min(value, last 5 min) < 20`) | 1, retained    |

`<slug>` is the label in lower case, for example `core_0`, `fan_1`, `bat0_charge`. Try it with `mosquitto_sub -v -t 'sensordash/#'`. The bridge reconnects by itself if the broker restarts.

## 4. Performance

Measured on a simulated 13-sensor machine (10 CPU cores, 2 fans, 1 battery) with **24 hours of 500 ms history** (2.2 million readings), 13 alarms (`avg` over 10 min) and one open dashboard.

| Metric                                            | Result                                                                                                                                                                                                                                                                                                                             |
| ------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Reading → dashboard latency                       | **1.5 ms median**, 2.3 ms p95                                                                                                                                                                                                                                                                                                      |
| Dashboard network traffic                         | one 80 KB history load (gzip), then ≈ 120 KB/min of live events                                                                                                                                                                                                                                                                    |
| Requests per dashboard after load                 | **0** (one open stream)                                                                                                                                                                                                                                                                                                            |
| `/api/readings` 15 min / 6 h / 24 h (median of 5) | 35 ms / 40 ms / 0.15 s                                                                                                                                                                                                                                                                                                             |
| Server CPU, 60 s, 1 dashboard                     | **0.21 s**                                                                                                                                                                                                                                                                                                                         |
| Server CPU, 115 s, 10 dashboards + MQTT           | 0.80 s (0.7 % of one core), memory flat at 12 MB                                                                                                                                                                                                                                                                                   |
| Alarm evaluation, 13 alarms                       | ≈ 0.2 ms for all 13 every second, from memory                                                                                                                                                                                                                                                                                      |
| DB size per day of 500 ms data                    | **61.8 MB** (no time index; includes 10 s rollups)                                                                                                                                                                                                                                                                                 |
| Dashboard bundle                                  | 166 KB (gzip) of code in four files: dashboard code 7 KB, stylesheet 3 KB, React 46 KB, charts (Recharts 3) 111 KB, plus 92 KB of bundled fonts (IBM Plex, five WOFF2 files, no internet needed) and the 19 KB logo; each cached for a year (hashed file names), so a dashboard change only re-downloads the small dashboard files |

How this is achieved:

- **Sampling** never waits on anything: each provider's thread reads its sysfs files (about 0.1 ms per tick for 10 cores) and publishes. Ticks are on absolute deadlines, so 500 ms stays 500 ms.
- **Storage** batches about 26 readings per second into one transaction, through a single prepared statement. There is no separate time index: every query reads per sensor through the primary key. History older than 24 h lives on as 10 s rollups (count, sum, sum of squares, min, max), so long windows read about 1/20 of the rows and alarms over long windows stay exact.
- **Alarms** keep one aggregate per second per sensor for the last hour in a ring buffer. Evaluating a 10-minute window sums 600 slots instead of querying the database. Windows longer than 60 minutes read the rollups every 10 s.
- **Dashboard** receives pushed events instead of re-downloading the whole window. It folds samples into the same buckets the server used, redraws charts at most once per second and current values twice per second, and memoised charts skip renders whose data has not changed.
- **HTTP**: gzip for JSON and static files (the live stream is never compressed), the dashboard is served from memory, and each thread role has its own SQLite connection, so API reads never wait for the writer (WAL).

`GET /api/stats` shows the live numbers for every stage (queue depths, dropped messages, tick and flush durations, alarm evaluation time, MQTT counters).

## 5. Data model

```sql
sensors (
  id INTEGER PRIMARY KEY,
  key TEXT UNIQUE,            -- 'coretemp/Core 4', 'fan:thinkpad/fan1' (stable across reboots)
  chip TEXT, label TEXT,      -- source device and label: 'coretemp', 'Core 4'
  unit TEXT,                  -- celsius | rpm | percent | ...
  kind TEXT,                  -- core | die | cpu | fan | charge | ...
  category TEXT,              -- temperature | fan | battery | ...  (dashboard tab)
  sort_order INTEGER, first_seen INTEGER, last_seen INTEGER)

readings (                    -- raw samples, kept --raw-retention-hours (24 h)
  sensor_id INTEGER, ts INTEGER /* epoch ms UTC */, value REAL,
  PRIMARY KEY (sensor_id, ts)) WITHOUT ROWID

readings_10s (                -- rollups, kept --retention-days (7 days)
  sensor_id INTEGER, bucket INTEGER /* multiple of 10000 */,
  n INTEGER, sum REAL, sumsq REAL, min REAL, max REAL,
  PRIMARY KEY (sensor_id, bucket)) WITHOUT ROWID

alarms (
  id INTEGER PRIMARY KEY, sensor_id INTEGER,              -- one alarm ↔ one sensor
  func TEXT CHECK (func IN ('avg','max','min','stddev')),
  window_min INTEGER CHECK (window_min > 0),             -- X
  threshold REAL,                                        -- Y
  op TEXT CHECK (op IN ('>','<')),                       -- fires above (>) or below (<) Y
  enabled INTEGER, state TEXT,                           -- ok | firing | no_data | disabled
  last_value REAL, last_eval_ts INTEGER, last_change_ts INTEGER, created_at INTEGER)

alarm_events (
  id INTEGER PRIMARY KEY, alarm_id INTEGER REFERENCES alarms ON DELETE CASCADE,
  sensor_id INTEGER, ts INTEGER, type TEXT /* fired | cleared */, value REAL, threshold REAL,
  op TEXT)                                               -- the alarm's comparison at that moment
```

- **Narrow (long) readings table** rather than one column per sensor. The number of cores, fans and batteries varies by machine, so a wide table would need a different schema per machine. Adding a core, a fan or a whole new sensor family means adding rows, never a migration.
- **Schema check on start-up** (`Database::migrate`) runs inside one write-locked transaction (the poller and server may start at the same moment). It brings any existing database file to the schema above: missing columns (`category`) and tables (`readings_10s`) are added, a `ts`-only index is dropped, and any package-level CPU sensor is removed with its readings and alarms. Core history is kept, and raw data is rolled up before any of it is pruned.
- Only long-standing SQLite features are used (no `RETURNING`/`UPSERT`), so the system builds against the SQLite of older distributions too.
- `(sensor_id, ts)` as a `WITHOUT ROWID` primary key clusters each sensor's history, so every query (one sensor, time range) is a single range scan; queries over all sensors run one scan per sensor. No secondary index is needed.
- All sensors in a tick share one timestamp (aligned to the interval), so series line up exactly across providers and in the dashboard.
- Volume at 500 ms: 13 sensors ≈ 2.2 M rows/day ≈ 62 MB/day raw. With the defaults (24 h raw, 7 days of rollups) the file levels off at about 100 MB; rollups alone cost about 3.5 MB per sensor per week.

## 6. API

All timestamps are Unix epoch **milliseconds, UTC**. Every sensor carries its `unit` (`celsius`, `rpm`, `percent`) and `category`. Errors use the shape `{"error": "message"}` with a `4xx`/`5xx` status.

### `GET /api/health`

```json
{"status": "ok", "last_sample_ts": 1793347568000, "readings_last_minute": 1560,
 "server_time": 1793347568577, "source": "local", "interval_ms": 500}
```

`status` is `stale` when no sample arrived for 3 intervals (at least 5 s). `source` is `local` (this process samples) or `db` (following another process).

### `GET /api/sensors`

Lists every discovered sensor, CPU cores first (Core 0, Core 4, Core 8 …), then fans and batteries if enabled, with its latest value and the number of its alarms currently firing.

```json
[{"id": 2, "key": "coretemp/Core 0", "chip": "coretemp", "label": "Core 0", "kind": "core",
  "category": "temperature", "unit": "celsius", "latest": {"ts": 1793339070000, "value": 40.0},
  "alarms_firing": 0},
 {"id": 12, "key": "fan:thinkpad/fan1", "label": "Fan 1", "kind": "fan", "category": "fan", "unit": "rpm", ...}]
```

### `GET /api/readings?minutes=15[&sensor_ids=1,2][&max_points=600]`

History for the last `minutes` (1–10080), for all sensors or a subset, in equal time buckets of `bucket_ms = max(interval, window / max_points)` (`max_points` 10–10000, default 600). Buckets of 10 s or more come from the rollup table. Each point is `[ts, avg, min, max, count]`. `to` is the newest reading already stored, so a client can append live samples with `ts > to` without gaps or double counting.

```json
{"from": 1793338170000, "to": 1793339070500, "minutes": 15, "bucket_ms": 1500, "interval_ms": 500,
 "point_format": ["ts", "avg", "min", "max", "count"],
 "series": [{"sensor_id": 2, "key": "coretemp/Core 0", "label": "Core 0", "kind": "core",
             "category": "temperature", "unit": "celsius",
             "points": [[1793338171500, 40.25, 40.0, 40.5, 3], ...]}]}
```

### `GET /api/stream` (Server-Sent Events)

One long-lived response per dashboard (`text/event-stream`, reconnects automatically in browsers):

| Event     | Data                                                                      | When                              |
| --------- | ------------------------------------------------------------------------- | --------------------------------- |
| `hello`   | `{"server_time","interval_ms","source"}`                                  | On connect                        |
| `samples` | `[{"ts":1793346818500,"v":{"1":36.15,"2":38.46,...}}]`                    | Every tick (all providers merged) |
| `alarms`  | `[{"id","state","last_value","last_eval_ts","last_change_ts","samples"}]` | After every alarm evaluation      |
| `alarm`   | `{"id","sensor_id","sensor_label","state","from","value","ts"}`           | On a state transition             |
| `sensors` | `{}`                                                                      | The sensor set changed            |

A comment line (`: ping`) is sent after 10 s without events. Try it: `curl -N localhost:8080/api/stream`.

### `GET /api/stats`

Pipeline metrics: bus subscribers (queued, received, dropped), sampler (ticks, read failures, skipped ticks, tick time), DB writer (rows, flushes, flush time, rollups, pruned), alarm engine (evaluations, evaluation time), live clients and MQTT.

### Alarms

| Method   | Path                | Body / query                                        | Result                              |
| -------- | ------------------- | --------------------------------------------------- | ----------------------------------- |
| `GET`    | `/api/alarms`       | `?sensor_id=` optional                              | List with current state             |
| `POST`   | `/api/alarms`       | see below                                           | `201 {"created": [...]}`            |
| `PATCH`  | `/api/alarms/{id}`  | any of `func`, `window_min`, `threshold`, `enabled` | Updated alarm                       |
| `DELETE` | `/api/alarms/{id}`  |                                                     | `204` (events cascade)              |
| `GET`    | `/api/alarm-events` | `?limit=50&alarm_id=`                               | Fired/cleared history, newest first |

Create one alarm for one sensor (the threshold is in that sensor's unit):

```bash
curl -X POST localhost:8080/api/alarms -H 'Content-Type: application/json' \
  -d '{"sensor_id": 3, "func": "avg", "window_min": 10, "threshold": 75}'     # a CPU core, °C
curl -X POST localhost:8080/api/alarms -H 'Content-Type: application/json' \
  -d '{"sensor_id": 12, "func": "max", "window_min": 5, "threshold": 4500}'  # a fan, RPM
curl -X POST localhost:8080/api/alarms -H 'Content-Type: application/json' \
  -d '{"sensor_id": 13, "func": "min", "window_min": 5, "threshold": 20, "op": "<"}'  # a battery below 20 %
```

`op` is optional: `">"` (the default) fires when the aggregate is above the threshold, `"<"` when it is below. It can be changed later with `PATCH` like any other field; any other value is rejected with `400`.
Create the same rule for several sensors at once. This yields **one independent alarm per sensor**, each with its own state and history:

```bash
-d '{"sensor_ids": [2,3,4], "func": "max", "window_min": 5, "threshold": 90}'
-d '{"sensor_ids": "all",   "func": "stddev", "window_min": 10, "threshold": 4}'
```

Alarm object:

```json
{"id": 11, "sensor_id": 11, "sensor_label": "Core 27", "func": "avg", "window_min": 10,
 "threshold": 75.0, "op": ">", "enabled": true, "state": "firing", "last_value": 77.3,
 "last_eval_ts": 1793339070000, "last_change_ts": 1793338990000, "created_at": 1793338900000,
 "condition": "avg(value, last 10 min) > 75"}
```

## 7. Alarm engine

- **Semantics:** an alarm fires when `func(value of its sensor over (now − X min, now]) op Y`, where `op` is `>` (the default, as specified in the brief) or `<`. Both comparisons are strict: an aggregate exactly equal to `Y` does not fire. `stddev` is the population standard deviation.
- **Above or below:** `>` suits values that are dangerous when high (temperatures, fan speed); `<` suits values that are dangerous when low, above all battery charge (`min(BAT0 charge, 5 min) < 20`). The dashboard pre-selects `below 20 %` when a battery is chosen and `above` for temperatures and fans.
- **Per sensor, any family:** every alarm is bound to exactly one sensor (a CPU core, a fan or a battery) and evaluated only against that sensor's readings, in its unit. A hot Core 27 fires its own alarm while Core 0's alarm stays `ok`; a fan alarm fires on its own RPM. The UI's "Every core" / "Every fan" options are a convenience that creates one independent alarm per sensor of the family.
- **States:** `ok`, `firing`, `no_data` (fewer than `--alarm-min-samples` readings in the window, which stops an alarm from firing on one sample right after start-up; `stddev` needs at least 2), and `disabled`.
- **Evaluation:** the alarm engine thread evaluates every `--alarm-interval` seconds (default 1). Windows up to 60 minutes come from in-memory per-second aggregates fed by the bus (about 15 µs per alarm per evaluation for a 10-minute window). Longer windows read the 10 s rollups plus the newest raw seconds, every 10 s. On start the memory windows are filled from the database, so a restart does not reset alarms to `no_data`. Each alarm is also evaluated immediately when it is created or edited (the API waits for that evaluation), so the UI shows a state at once.
- **Push, not poll:** transitions are published as `alarms/<id>/state` (live dashboards, MQTT) and every evaluation as `alarms/snapshot`. Transitions are written to the database at once; the latest aggregate values every 10 s in one batch.
- **History:** only **transitions** are recorded (`fired` when entering `firing`, `cleared` when leaving it), together with the aggregate value and the threshold at that moment. Repeated evaluations in the same state produce no events.
- **Concurrency:** an evaluation writes back with compare-and-set on `(state, enabled, func, window, threshold)`. If a user edits or disables an alarm while it is being evaluated, the stale result is discarded instead of overwriting the edit.
- **Staleness:** if sampling stops, the window empties and alarms move to `no_data`. Silence is never reported as `ok`.
- **Notification:** transitions are logged by the server (`WARN … FIRED`), pushed to open dashboards, and published (retained) to MQTT when the bridge is on. No email or webhook is sent (see limitations); a notifier would be one more bus subscriber.

## 8. Dashboard

- **Category tabs:** one tab per sensor family present (CPU temperatures, Fans, Battery, or any new category). The selected tab drives the whole page: charts, sensor table and the Alarms section. A tab with firing alarms shows an orange `N firing` badge, so a firing alarm stays visible while another tab is open. With only CPU temperatures enabled, no tabs are shown. Values, axes and thresholds use each sensor's unit (°C, RPM, %).
- **Live:** history is loaded once per window change; after that everything arrives over `/api/stream`: new samples every 500 ms, alarm states every second, and notifications when alarms or sensors change. No polling. A pulsing orange **Live** marker shows the stream state (grey `Reconnecting…` while the stream is down); after a reconnect the gap is filled from the API.
- **Window selector:** 5 min, 15 min, 1 h, 6 h, 24 h, always about 600 points per series (for example 500 ms per point for 5 min, 150 s for 24 h). Charts redraw at most once per second and current values twice per second.
- **Combined view:** all sensors of the selected family on one chart, each with a stable colour.
- **Per-sensor view:** small multiples, one chart per sensor (per core, per fan, per battery), each with its own alarm thresholds drawn as reference lines. A sensor with a firing alarm is outlined in orange on a dark orange-tinted background.
- **Sensor table:** current value in the sensor's unit, window min/avg/max, alarm badges, and visibility toggles (show all / hide all).
- **Header:** the Armory logo (`frontend/src/assets/logo.webp`, a web-sized copy of `frontend/assets/Armory-Logo-Metal.png`), one tag per sensor family found (for example `10 cores`, `2 fans`, `1 battery`), the live marker and the time of the latest sample. All times on the page use the 24-hour clock.
- **Alert strip:** shown under the header only while alarms fire, on every tab: the number of firing alarms and up to four of them (sensor and condition, for example `Core 27 avg(last 10 min) > 60`), then `and N more`.
- **Look:** dark dot-textured page, dark panels with corner brackets (charts, tables and the alarm form on the same dark surface, chart lines in light colours), square controls, IBM Plex Mono for headings, tabs and readings and IBM Plex Sans for text. Orange is used only for what needs attention (firing alarms, the live marker, the primary button); chart lines use twelve non-orange colours so an orange line always means a threshold that is firing. Styles live in `frontend/src/styles.css`.
- **Alarm panel (per family):** titled after the selected tab (`Alarms: CPU temperatures`, `Alarms: Fans`, `Alarms: Battery`) and limited to that family. The **Sensor** dropdown lists only that family's sensors plus "Every core / fan / battery (one alarm each)"; the form starts with that family's suggested comparison and threshold (above 75 °C, above 4500 RPM, below 20 %) and the threshold label shows its unit. The alarm table (live state and aggregate, enable/disable, delete) and the fired/cleared event log (with each condition, `> 75`, `< 20`) show only that family's alarms; a note says how many alarms belong to other families.
- **Browser tab title:** `System Sensors`, the same as the page heading.
- API errors are shown in a dismissible banner, and the UI keeps the last good data.

## 9. Testing

Step-by-step rebuild instructions and a manual check for every feature, with expected results, are in **[TESTING.md](TESTING.md)**.

```bash
ctest --test-dir build --output-on-failure     # 19 test groups, 274 checks
./build/sd-tests --list                        # run one group: ./build/sd-tests fan
```

- **CPU discovery:** a fake sysfs tree shaped like the dev laptop (package skipped, non-contiguous cores in numeric order, unrelated chips ignored, unreadable values), a dual-socket tree with repeated labels, and the AMD per-die, whole-CPU and ARM thermal-zone fallbacks.
- **Fans and batteries:** stopped fans, fans on several chips, `capacity` vs `energy_now/energy_full`, peripheral batteries ignored, machines with none.
- **Framework:** registry (unknown names rejected, `all`), manager ordering across providers, and a custom provider defined in the test itself to prove the extension point.
- **Schema check:** a database missing the `category` column gets it, alarm tables without the `op` column get it (existing alarms and events become `>`), package sensors are removed, ids are kept, and two processes starting at once do not conflict.
- **Store:** id stability on re-registration, aggregates (including stddev against a hand-computed value), bucketed series, retention.
- **Alarms:** each function and both comparisons with their boundaries (strict `>` and `<`), a battery alarm firing below its threshold and clearing when the charge rises, a changed comparison rejecting stale results, `min_samples`, per-core independence, warm-up from the DB, live samples from the bus, transition messages, long windows from the DB, transition-only events, disable, stale-evaluation rejection, cascade delete.
- **Bus:** MQTT filter matching and validation, delivery, overflow (drop oldest), timeouts, and 4 publisher threads × 10 000 messages with nothing lost.
- **Sliding window:** 4 h of random data against a brute-force computation (count, avg, min, max, stddev).
- **DB writer:** batching (240 readings in at most 5 commits), rollups equal to the raw aggregates, rollup/raw stitching without double counting, retention, nothing lost on stop.
- **Sampler:** one thread per provider, aligned shared ticks, registration, rediscovery of a battery that appears while running.
- **Follower and lock:** a second sampler is refused, the lock is released with its holder, rows written by another process are republished in order.

Also verified: the whole suite and a server under concurrent load (8 live streams, alarm create/edit/delete, history queries, rediscovery, MQTT) under **ThreadSanitizer** (no data races) and **AddressSanitizer + UBSan** (no memory errors); a 2-minute soak with 10 dashboards (flat memory and thread count, no dropped messages); MQTT against Mosquitto including broker restart and last will; poller → server follow mode and takeover; the start-up schema check on a real database; and the dashboard in headless Chromium (no console errors, no requests after load).



## 10. Assumptions, trade-offs and limitations

**Assumptions**

- "All available sensors" means every channel the enabled providers find: for CPU temperatures every per-core channel reported by the CPU's hwmon driver, for fans every hwmon fan channel, for batteries every system battery. The package sensor is not tracked; if a database contains one, it is removed with its readings and alarms on start-up (`Database::migrate`).
- The sensor set is discovered **when sampling starts and again every `--rediscover` seconds** (default 60). A battery that appears or a driver loaded later is picked up without a restart. Historical sensors stay in the DB.
- On CPUs without per-core sensors (most AMD, ARM), "cores" falls back to the most detailed CPU temperature available (per die, then whole CPU), and the log says which level is used.
- `scripts/run_all.sh` enables every provider (`all`); the binaries default to `cpu_temp` and take `--providers`. Many laptops hide fan speed from Linux and desktops have no battery; those providers then simply find nothing and their tabs do not appear.
- Linux only. The brief allows any OS, and macOS/Windows readers would be additional `SensorProvider`s behind the same interface.

**Trade-offs**

- **In-process bus instead of a mandatory MQTT broker:** messages between threads cost one queue push and no serialisation, and the system runs on any Linux without extra services. MQTT is an optional bridge for other tools. Splitting sampling and serving into separate machines would use the broker as the transport (a `--source mqtt` subscriber), which this design leaves room for.
- **One process by default:** simpler to run, and live data needs no inter-process hop. The standalone poller remains for headless recording, and the server follows it automatically.
- **SQLite over a time-series database:** no infrastructure; per-sensor range scans plus stored 10 s rollups keep queries in milliseconds at this volume.
- **Server-Sent Events over WebSockets:** traffic is one-way (server to browser), SSE is plain HTTP, reconnects by itself, and needs no extra library. Commands still use the REST API.
- **Alarm windows in memory up to 60 minutes:** about 170 KB per sensor. Longer windows use rollups at 10 s resolution, evaluated every 10 s.
- **stddev** is computed from sums of squares (`sqrt(E[x²] − E[x]²)`), which is exact enough for sensor values and lets rollups and memory windows combine buckets. Welford's algorithm would be the choice for large-magnitude data.
- **Drop-oldest queues:** under extreme overload a consumer loses its oldest messages (counted in `/api/stats`) rather than blocking sampling. With the default capacities this needs a consumer stalled for over an hour.
- **No hysteresis or "for N consecutive evaluations" debounce.** The windowed aggregate already smooths noise, and each alarm has a single threshold (above or below).

**Known limitations / with more time**

- Webhook/email notification on `fired` (the event table is already the outbox).
- Alarm hysteresis (`clear_below`) and per-alarm evaluation intervals.
- Authentication on the API (it currently binds `0.0.0.0` with permissive CORS; use `--host 127.0.0.1` on shared networks).
- A systemd unit and Docker packaging.
- `--source mqtt`: a server that subscribes to pollers on other machines through the broker.
- Per-provider sampling intervals (for example batteries every 5 s, cores every 500 ms).
- More built-in families (GPU and disk temperatures, voltages, power): each is one provider (see [Adding a new sensor type](#adding-a-new-sensor-type)).

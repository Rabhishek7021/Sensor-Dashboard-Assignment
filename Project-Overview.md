# Multi-Sensor Dashboard — Project Documentation

A complete guide to the Multi-Sensor Dashboard: what it does, how it is built, how to install its dependencies, set up the repository, build it, test it, run it and generate its code documentation.

> **Verified:** every command in sections 4–9 was run on a copy of the project in its current state (sources only: no `build/`, `frontend/dist/` or database) on Ubuntu 24.04 with GCC 13, CMake 3.28, SQLite 3.45, Node 22 and Doxygen 1.9.8. `scripts/setup_env.sh` was run on a machine with everything installed (table only), with zlib removed and MQTT requested (both installed, then reported `ok`), and with an outdated Node.js (clear error). Result: clean build with no warnings, **20/20 test entries passed (274 checks)**, `scripts/run_all.sh --fake` built the dashboard and ran the full system with 13 simulated sensors (including above- and below-threshold alarms through the API and the dashboard), and `doxygen Doxyfile` generated the documentation with no warnings about the code.

## Contents

1. [Overview](#1-overview)
2. [Project structure](#2-project-structure)
3. [Architecture](#3-architecture)
4. [Required installations](#4-required-installations)
5. [Project / repository setup](#5-project--repository-setup)
6. [Cleaning the project](#6-cleaning-the-project)
7. [Build commands and code documentation](#7-build-commands-and-code-documentation)
8. [Test commands](#8-test-commands)
9. [Running the whole project](#9-running-the-whole-project)
10. [Configuration reference](#10-configuration-reference)
11. [REST API and live stream](#11-rest-api-and-live-stream)
12. [Data model](#12-data-model)
13. [Alarm engine](#13-alarm-engine)
14. [Dashboard (frontend)](#14-dashboard-frontend)
15. [MQTT bridge (optional)](#15-mqtt-bridge-optional)
16. [Adding a new sensor type](#16-adding-a-new-sensor-type)
17. [Troubleshooting](#17-troubleshooting)
18. [Assumptions and limitations](#18-assumptions-and-limitations)
19. [Command cheat sheet](#19-command-cheat-sheet)

## 1. Overview

The system automatically discovers the **per-core CPU temperature sensors** on the Linux machine it runs on. Fans (RPM) and battery charge (%) are optional. It:

- **samples** every sensor every **500 ms** (one thread per sensor family),
- **stores** readings in **SQLite** (WAL mode, batched writes, 10-second rollups, automatic retention),
- **serves** a C++ REST API and a **live Server-Sent Events stream**,
- shows everything in a **React dashboard** (live charts, per-core views, sensor table),
- evaluates **per-sensor statistical alarms** of the form `avg | max | min | stddev` over the last *X* minutes `> Y` or `< Y` (for example a hot core, a racing fan, or a battery below 20 %),
- can optionally publish everything to an **MQTT broker**.

| Component          | Technology                                                                                                                                      |
| ------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| Sensor reading     | C++17, Linux sysfs (`/sys/class/hwmon`, `/sys/class/power_supply`, `/sys/class/thermal`)                                                        |
| Storage            | SQLite ≥ 3.8.2 (WAL), batched writer thread                                                                                                     |
| Backend            | C++17, `cpp-httplib` (HTTP/SSE), `nlohmann/json`, both vendored in `third_party/`                                                               |
| Internal messaging | In-process publish/subscribe bus with MQTT-style topics                                                                                         |
| Frontend           | React 18, Recharts 3, Vite 5 (bundle split into `index`, `react` and `charts` files); IBM Plex fonts bundled from `@fontsource` (works offline) |
| Build              | CMake ≥ 3.16; npm for the frontend                                                                                                              |
| Optional           | libmosquitto (MQTT bridge), zlib (gzip HTTP compression)                                                                                        |
| Documentation      | Doxygen comments in every source file; `Doxyfile` → HTML in `docs/html/` (Graphviz diagrams)                                                    |

Two executables are produced:

| Binary          | Role                                                                                              |
| --------------- | ------------------------------------------------------------------------------------------------- |
| `sensor-server` | Main program: sampling + storage + alarms + REST API + live stream + serves the dashboard         |
| `sensor-poller` | Headless sampler: records to the database with no HTTP; also used for `--list` (sensor discovery) |
| `sd-tests`      | Unit test runner (20 test groups)                                                                 |

## 2. Project structure

```
Multi-Sensor-Dasboard/
├── CMakeLists.txt            C++ build definition (libraries, executables, tests)
├── cmake/ReportSensors.cmake Post-build report of sensors found on the build machine
├── README.md                 Design write-up (architecture, performance, trade-offs)
├── TESTING.md                Step-by-step manual checks for every feature
├── Project-Overview.md       This file
├── Doxyfile                  Doxygen configuration (code documentation → docs/html)
├── .gitignore
├── data/                     Runtime SQLite database (created at run time)
├── scripts/
│   ├── setup_env.sh          Check / install the required packages (run by run_all.sh)
│   ├── run_all.sh            Check the environment, build everything, start the full system
│   ├── fake_hwmon.sh         Simulated sensors (10-core CPU, 2 fans, battery)
│   ├── clean.sh              Remove all generated files
│   └── doxygen-md-filter.sh  Doxygen helper for the Markdown pages
├── src/
│   ├── bus/                  sd_bus: pub/sub bus, MQTT topic matching (+, #)
│   ├── sensors/              sd_sensors: hardware discovery (no DB, no HTTP)
│   │   ├── sensor.hpp, provider.hpp, registry.*, manager.*, hwmon.*, sysfs.*
│   │   └── providers/        cpu_temp.* · fan.* · battery.*
│   ├── common/               sd_common: SQLite store (db.*), alarm logic, sliding windows
│   ├── pipeline/             sd_pipeline: sampler, DB writer, DB follower, MQTT bridge, CLI options
│   ├── poller/main.cpp       sensor-poller executable
│   └── server/               sensor-server: main.cpp (HTTP/SSE/static), alarm_engine.*, live_cache.hpp
├── tests/test_main.cpp       All unit tests (20 groups, 285 checks)
├── third_party/              httplib.h, nlohmann/json.hpp (MIT, vendored: no network needed to build)
└── frontend/
    ├── package.json, package-lock.json, vite.config.js, index.html
    ├── assets/               Armory-Logo-Metal.png (original logo artwork)
    ├── src/                  App.jsx, live.js, TempChart.jsx, AlarmPanel.jsx, units.js, api.js, styles.css,
    │                         assets/logo.webp (web-sized logo shown in the header)
    └── dist/                 Built dashboard, served by sensor-server (generated by npm run build)
```

**Generated (not source, safe to delete):** `build/`, `docs/`, `frontend/dist/`, `frontend/node_modules/`, `data/sensors.db*` and `data/*.lock`. None of them is part of the project folder until you build or run it: `cmake` creates `build/`, `npm run build` (or the first `scripts/run_all.sh`) creates `frontend/dist/`, the server creates the database, and `doxygen Doxyfile` creates `docs/`.

## 3. Architecture

```
  /sys/class/hwmon · /sys/class/power_supply · /sys/class/thermal
                              │
      CpuTempProvider · FanProvider · BatteryProvider        (Strategy, built by ProviderRegistry)
                              │
                     SensorManager (Facade)
                              │
          Sampler: 1 thread per provider, every 500 ms, shared timestamps
                              │ publish  samples/<provider>
  ═══════════════ in-process pub/sub bus (one bounded queue per subscriber) ═══════════════
       │               │                │                 │                    │
   DbWriter        AlarmEngine      LiveCache        SSE stream per       MqttBridge
   (batches,       (in-memory       (latest          open dashboard       (optional,
   rollups,        windows,         values,                               → broker)
   retention)      alarms/…)        health)
       │                                                  │
   SQLite (WAL) ◀──── REST API (HTTP worker pool) ────── React dashboard
```

**Key design points**

- **Publish/subscribe:** producers never know who consumes their data. Adding a consumer (exporter, notifier) means adding one subscriber thread.
- **Bounded queues, drop-oldest:** a stalled browser can never block sampling or storage. Drops are counted in `/api/stats`.
- **Layered libraries:** `sd_sensors` (hardware) → `sd_bus` (messaging) → `sd_common` (storage) → `sd_pipeline` (threads) → executables.
- **One sampler per database:** enforced with an exclusive `flock()` on `<db>.sampler.lock`. If a `sensor-poller` already samples, `sensor-server` *follows* the database instead of sampling a second time, and takes over within about 2 s if the poller exits.
- **Discovery at run time:** the core count is never hard-coded. Supported layouts are Intel per-core, AMD per-die (`Tccd`), whole-CPU (`Tctl`/`Package`) and ARM thermal zones. Discovery is re-run every 60 s.

| Thread                        | Subscribes to                                                       | Publishes                              |
| ----------------------------- | ------------------------------------------------------------------- | -------------------------------------- |
| Sampler (one per provider)    | —                                                                   | `samples/<provider>`                   |
| Sampler supervisor            | —                                                                   | `sensors/changed`                      |
| DbWriter                      | `samples/#`                                                         | —                                      |
| AlarmEngine                   | `samples/#`, `control/alarms/reload`, `sensors/changed`             | `alarms/<id>/state`, `alarms/snapshot` |
| LiveCache                     | `samples/#`                                                         | —                                      |
| SSE stream (per dashboard)    | `samples/#`, `alarms/+/state`, `alarms/snapshot`, `sensors/changed` | —                                      |
| MqttBridge (optional)         | `samples/#`, `alarms/+/state`, `sensors/changed`                    | → broker                               |
| DbFollower (follow mode only) | —                                                                   | `samples/db`                           |

## 4. Required installations

### 4.0 Automatic setup (recommended)

`scripts/setup_env.sh` checks every requirement, prints the installed versions, and installs whatever is missing through the system package manager. `scripts/run_all.sh` runs it automatically before every build.

```bash
chmod +x scripts/*.sh             # once, after copying or cloning the project
scripts/setup_env.sh              # check, install missing required packages
scripts/setup_env.sh --with-mqtt  # also libmosquitto, the mosquitto broker and clients
scripts/setup_env.sh --with-docs  # also Doxygen and Graphviz
scripts/setup_env.sh --all        # required + MQTT + docs
scripts/setup_env.sh --check      # report only; exit 1 if something required is missing
```

When everything is installed it only prints the table (in under a second):

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

When something is missing it is marked `MISSING` (or `TOO OLD`), installed, and the table is printed again:

```
    zlib headers               >= 1.2     -            MISSING
==> installing: zlib1g-dev
…
==> build environment after installing
    zlib headers               >= 1.2     1.3          ok
==> all required packages are installed
```

| What it checks                        | How the version is read                                 | Package installed (apt / dnf / pacman / zypper)                                            |
| ------------------------------------- | ------------------------------------------------------- | ------------------------------------------------------------------------------------------ |
| C++17 compiler (GCC ≥ 8 or Clang ≥ 7) | `$CXX` or `c++` `-dumpfullversion`                      | `g++` / `gcc-c++` / `gcc` / `gcc-c++`                                                      |
| make ≥ 3.81                           | `make --version`                                        | `make`                                                                                     |
| CMake ≥ 3.16                          | `cmake --version`                                       | `cmake`                                                                                    |
| SQLite headers ≥ 3.8.2                | `SQLITE_VERSION` from `sqlite3.h`, through the compiler | `libsqlite3-dev` / `sqlite-devel` / `sqlite` / `sqlite3-devel`                             |
| zlib headers                          | `ZLIB_VERSION` from `zlib.h`                            | `zlib1g-dev` / `zlib-devel` / `zlib` / `zlib-devel`                                        |
| Node.js ≥ 18, npm ≥ 8                 | `node --version`, `npm --version`                       | `nodejs`, `npm`                                                                            |
| Optional: libmosquitto ≥ 1.4          | `LIBMOSQUITTO_*` from `mosquitto.h`                     | `libmosquitto-dev mosquitto mosquitto-clients` / `mosquitto-devel mosquitto` / `mosquitto` |
| Optional: Doxygen ≥ 1.9, Graphviz     | `doxygen --version`, `dot -V`                           | `doxygen`, `graphviz`                                                                      |

- Installing needs root: the script uses `sudo` when it is not run as root (you may be asked for your password).
- Headers are checked through the compiler, exactly as the build sees them, so a missing `-dev` package is detected even when the runtime library is present.
- Exit status: `0` all required packages present, `1` something required is still missing or too old (the message says what to do), `2` unknown option.
- If the distribution's Node.js is older than 18, the package manager cannot fix it; the script prints how to install a current LTS (NodeSource or nvm) and exits with `1`.
- The tools used only by the checks in TESTING.md (`sqlite3`, `python3`, `curl`, `stress-ng`) are not part of it; install them as in 4.2 if you run those checks.

The sections below list the same packages for installing by hand.

### 4.1 Minimum versions

| Tool                 | Minimum                                 | Tested       |
| -------------------- | --------------------------------------- | ------------ |
| C++ compiler         | GCC ≥ 8 or Clang ≥ 7 (C++17)            | GCC 13       |
| CMake                | ≥ 3.16                                  | 3.28         |
| SQLite (dev headers) | ≥ 3.8.2                                 | 3.45         |
| Node.js + npm        | Node ≥ 18 (builds the dashboard)        | Node 22      |
| Doxygen (+ Graphviz) | 1.9 (optional: code documentation)      | 1.9.8        |
| OS                   | Linux (any distribution, x86-64 or ARM) | Ubuntu 24.04 |

No root privileges are needed to *run* the project: hwmon sensor files are world-readable.

### 4.2 Ubuntu / Debian

```bash
sudo apt update

# Required: compiler, CMake, SQLite headers, zlib (gzip for HTTP)
sudo apt install -y build-essential cmake libsqlite3-dev zlib1g-dev

# Frontend: builds the dashboard (frontend/dist) from frontend/src
sudo apt install -y nodejs npm

# Tools used by the checks in TESTING.md
sudo apt install -y sqlite3 python3 curl

# Optional: MQTT bridge
sudo apt install -y libmosquitto-dev mosquitto mosquitto-clients

# Optional: see your machine's sensors / generate CPU load
sudo apt install -y lm-sensors stress

# Optional: generate the code documentation (section 7.5)
sudo apt install -y doxygen graphviz

# Or let the setup script do all of the above that the project needs (section 4.0):
# scripts/setup_env.sh --all
```

> If your distribution's `nodejs` is older than 18, install a current LTS from NodeSource or with `nvm`:
> `curl -fsSL https://deb.nodesource.com/setup_22.x | sudo -E bash - && sudo apt install -y nodejs`

### 4.3 Fedora / RHEL

```bash
sudo dnf install -y gcc-c++ cmake sqlite-devel zlib-devel nodejs npm
sudo dnf install -y mosquitto-devel mosquitto          # optional MQTT
```

### 4.4 Arch Linux

```bash
sudo pacman -S --needed base-devel cmake sqlite zlib nodejs npm
sudo pacman -S --needed mosquitto                      # optional MQTT
```

### 4.5 Check the installation

```bash
g++ --version          # 8 or newer
cmake --version        # 3.16 or newer
sqlite3 --version      # 3.8.2 or newer (CLI is optional; the headers are what the build needs)
node --version         # v18 or newer
npm --version
doxygen --version      # 1.9 or newer (optional, code documentation)
```

### 4.6 Sensor driver check (real hardware)

```bash
ls /sys/class/hwmon/*/name | xargs cat     # expect coretemp (Intel) or k10temp / zenpower (AMD)
sensors                                    # from lm-sensors, optional
# If no CPU driver is listed on Intel:
sudo modprobe coretemp
```

In a VM or container there are usually no hardware sensors. Use the simulated sensors (`--fake`, see section 9.2) instead.

## 5. Project / repository setup

### 5.1 Get the code

From a Git remote:

```bash
git clone <repository-url> Multi-Sensor-Dasboard
cd Multi-Sensor-Dasboard
```

Or from the existing folder:

```bash
cd ~/Desktop/Armory-Home-Assignment/Multi-Sensor-Dasboard
```

### 5.2 Make the scripts executable

```bash
chmod +x scripts/*.sh
```

(Alternatively, prefix every script with `bash`, for example `bash scripts/run_all.sh`.)

### 5.3 Put the project under Git (if it is not already)

A `.gitignore` is included. It excludes everything generated: `build/`, `docs/`, `frontend/dist/`, `frontend/node_modules/`, the database files and lock files.

```bash
scripts/clean.sh                 # start from a clean tree (section 6)
git init
git add .
git commit -m "Multi-sensor dashboard: initial commit"
# optional:
git branch -M main
git remote add origin <repository-url>
git push -u origin main
```

### 5.4 External dependencies

- **C++ libraries:** `cpp-httplib` and `nlohmann/json` are vendored in `third_party/`, so the C++ build needs **no network access**.
- **Frontend packages** are pinned in `frontend/package-lock.json` and installed with `npm ci`.

## 6. Cleaning the project

`scripts/clean.sh` removes everything that building or running the project generates:

```bash
# Stop the server first (Ctrl+C), then:
scripts/clean.sh          # removes build/, docs/, data/sensors.db*, lock file, frontend/node_modules, /tmp test trees
scripts/clean.sh --all    # also removes the built dashboard frontend/dist (rebuilt by npm run build or run_all.sh)
```

Manual equivalent:

```bash
pkill -f 'build/sensor-' || true
rm -rf build docs frontend/node_modules frontend/.vite
rm -f data/sensors.db data/sensors.db-wal data/sensors.db-shm data/sensors.db.sampler.lock
rm -rf /tmp/fake-sys /tmp/sd-test
# rm -rf frontend/dist     # what --all adds (the dashboard is rebuilt by npm run build)
```

> Deleting `data/sensors.db` erases all recorded history and all alarms. Keep it if you want to preserve them.

## 7. Build commands and code documentation

Run all commands from the project root.

### 7.1 Full clean build (backend + frontend)

```bash
scripts/clean.sh

# C++ backend: sensor-server, sensor-poller, sd-tests
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

# Frontend dashboard → frontend/dist
cd frontend
npm ci
npm run build
cd ..
```

**Expected output**

- Configure step: `Found SQLite3 … (found version "3.x")`, `MQTT bridge: enabled` (or `disabled` without libmosquitto), and `HTTP compression: gzip enabled`.
- Build step ends with a sensor report. On real hardware it looks like `-- Sensors detected on this machine (10 CPU core temperature sensor(s)): …`; in a VM or container it says `No sensors detected on this build machine`, which is fine.
- `build/` now contains `sensor-server`, `sensor-poller` and `sd-tests`.
- `npm ci` and `npm run build` print no warnings. Vite lists the stylesheet (`index-….css`, about 9 kB), the IBM Plex font files and three JavaScript files in `frontend/dist/assets/`: `index-….js` (dashboard code, about 18 kB), `react-….js` (about 142 kB) and `charts-….js` (Recharts 3 and its dependencies, about 382 kB), each below Vite's 500 kB limit, and ends with `✓ built in …`.

### 7.2 Incremental rebuild (after editing code)

```bash
cmake --build build -j"$(nproc)"                      # C++ only, recompiles changed files
(cd frontend && npm run build)                        # frontend only
```

`scripts/run_all.sh` also rebuilds incrementally every time it starts (section 9.1).

### 7.3 Build options

| Option                 | Values                               | Default   | Effect                                                           |
| ---------------------- | ------------------------------------ | --------- | ---------------------------------------------------------------- |
| `-DCMAKE_BUILD_TYPE=`  | `Release`, `Debug`, `RelWithDebInfo` | `Release` | Optimisation / debug symbols                                     |
| `-DSD_WITH_MQTT=`      | `AUTO`, `ON`, `OFF`                  | `AUTO`    | MQTT bridge: auto-detect libmosquitto, require it, or disable it |
| `-DSD_REPORT_SENSORS=` | `ON`, `OFF`                          | `ON`      | Print the detected sensors after building                        |

Examples:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DSD_WITH_MQTT=OFF
cmake -S . -B build -DSD_WITH_MQTT=ON            # fails if libmosquitto-dev is missing
```

Sanitizer build (debugging):

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan -j"$(nproc)"
ctest --test-dir build-asan --output-on-failure
```

### 7.4 Code documentation comments

Every source file carries Doxygen comments:

- **C++ (`src/`, `tests/`):** a file header comment (Doxygen *file* and *brief* commands) in each file; `///` comments on namespaces, classes, structs, functions and type aliases; `///<` comments on members, constants and enum values. Implementation-only helpers in `.cpp` files (anonymous namespaces) are documented too.
- **Frontend (`frontend/src/`):** a file header comment in each file and `/** … */` comments on every function, hook, component and constant, with parameters and props listed under the description.

Comments inside function bodies stay plain `//` (they explain steps, not interfaces).

### 7.5 Generate the documentation (Doxygen)

```bash
scripts/setup_env.sh --with-docs          # once: installs Doxygen + Graphviz if missing
doxygen Doxyfile                          # from the project root, takes a few seconds
xdg-open docs/html/index.html             # open in the browser
```

- The output goes to `docs/html/` (ignored by Git, removed by `scripts/clean.sh`). Warnings are written to `docs/doxygen-warnings.log`.
- The main page is `README.md`; `Project-Overview.md` and `TESTING.md` appear under **Related Pages**.
- Input: `src/`, `tests/`, `frontend/src/` and the three Markdown files. `third_party/` and build output are excluded.
- Without Graphviz, set `HAVE_DOT = NO` in the `Doxyfile` (no diagrams, everything else works).
- To list anything left undocumented after editing code, run with `EXTRACT_ALL` off:
  
  ```bash
  (cat Doxyfile; echo "EXTRACT_ALL=NO") | doxygen - && cat docs/doxygen-warnings.log
  ```

**Expected result**

- Doxygen finishes in a few seconds; `docs/html/` holds about 350 pages.
- `docs/doxygen-warnings.log` contains at most one line, about a heading level in `README.md` (harmless). There are no warnings about the code, also with `EXTRACT_ALL=NO`.

**What the generated site contains**

| Menu          | Content                                                                                                                                   |
| ------------- | ----------------------------------------------------------------------------------------------------------------------------------------- |
| Main Page     | `README.md`                                                                                                                               |
| Related Pages | `Project-Overview.md`, `TESTING.md`                                                                                                       |
| Namespaces    | `sd::bus`, `sd::sensors`, `sd::hwmon`, `sd::sysfs`, `sd::db`, `sd::alarm`, `sd::msg`, `sd::pipeline`, `sd::server`, `sd::log`, `sd::time` |
| Classes       | Every class and struct with its members, collaboration and inheritance diagrams                                                           |
| Files         | Every file in `src/`, `tests/` and `frontend/src/` with its description, functions, constants and source listing                          |

**How the Doxyfile is set up**

| Setting                                                                                    | Why                                                                                                                                                                     |
| ------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `INPUT` = `README.md`, `Project-Overview.md`, `TESTING.md`, `src`, `tests`, `frontend/src` | Code plus the project guides; `third_party/`, `build/`, `frontend/dist/` and `node_modules/` are excluded                                                               |
| `EXTENSION_MAPPING = js=C++ jsx=C++`                                                       | Doxygen's own JavaScript parser cannot read modern `import`/`export`/JSX code; the C++ parser lists every frontend function and constant with its comment               |
| `FILTER_PATTERNS` → `scripts/doxygen-md-filter.sh` (run with `bash`)                       | Turns GitHub-style links to headings in the same file (targets such as `#3-architecture`) into plain text for the generated pages only; the `.md` files are not changed |
| `JAVADOC_AUTOBRIEF`, `DISTRIBUTE_GROUP_DOC`                                                | First sentence is the summary; one comment documents every variable of `a, b;` declarations                                                                             |
| `EXTRACT_ALL`, `EXTRACT_PRIVATE`, `EXTRACT_ANON_NSPACES`                                   | Private members and file-local helpers appear too                                                                                                                       |
| `HAVE_DOT = YES`, `DOT_IMAGE_FORMAT = svg`                                                 | Class, collaboration and include diagrams (needs Graphviz)                                                                                                              |
| `OUTPUT_DIRECTORY = docs`, `WARN_LOGFILE = docs/doxygen-warnings.log`                      | Everything generated stays in `docs/`                                                                                                                                   |

Regenerate after any code change with `doxygen Doxyfile` (it rewrites `docs/html/`).

## 8. Test commands

### 8.1 Automated unit tests

```bash
ctest --test-dir build --output-on-failure
```

**Expected:** `100% tests passed, 0 tests failed out of 21`. That is one entry that runs everything, plus one entry for each of the 20 groups (285 checks).

Run the test binary directly:

```bash
./build/sd-tests                     # all groups
./build/sd-tests --list              # list group names
./build/sd-tests cpu_temp            # every group whose name contains "cpu_temp"
./build/sd-tests db_writer sampler   # several groups
ctest --test-dir build -R alarm      # select through CTest by regex
```

| Test group               | What it verifies                                                                                                     |
| ------------------------ | -------------------------------------------------------------------------------------------------------------------- |
| `cpu_temp_single_socket` | Laptop layout: package sensor skipped, cores ordered 0, 4, 8 … 27, unrelated chips ignored                           |
| `cpu_temp_multi_socket`  | Two CPUs with repeated `Core 0` labels get unique keys and `CPU0`/`CPU1` labels                                      |
| `cpu_temp_fallbacks`     | AMD per-die (`Tccd`), whole-CPU (`Tctl`) and ARM thermal-zone fallbacks                                              |
| `fan_provider`           | RPM reading, stopped fan, fans on several chips                                                                      |
| `battery_provider`       | `capacity` and `energy_now/energy_full`, peripheral batteries ignored, no battery                                    |
| `registry_and_manager`   | Unknown providers rejected, `all`, ordering CPU → fan → battery                                                      |
| `custom_provider`        | A provider defined outside the project plugs in unchanged                                                            |
| `store_and_aggregates`   | Stable ids, avg/min/max/stddev, bucketed queries, retention                                                          |
| `migrate_drops_package`  | A package sensor in the database is removed with its readings, alarms and events                                     |
| `migrate_adds_category`  | A database without the `category` column gets it; ids kept                                                           |
| `migrate_adds_op`        | Alarm tables without the `op` column get it; existing alarms and events become `>`                                   |
| `alarm_logic`            | Each function, strict `>` and `<` boundaries, operator validation, minimum samples                                   |
| `alarm_input_limits`     | Window 1–100 min; threshold −50–100 °C, 0–5000 RPM, 0–100 % with bounds included; other units take any finite number |
| `alarm_below_threshold`  | A battery `min < 20`-style alarm fires and clears; events store `<`; changing `op` rejects stale results             |
| `alarm_engine_per_core`  | Per-core independence, warm-up from DB, transitions, disable, cascade delete                                         |
| `bus`                    | MQTT filter matching, delivery, drop-oldest overflow, 4 × 10 000 messages, nothing lost                              |
| `second_window`          | Sliding window vs brute force over 4 h of random data                                                                |
| `db_writer`              | Batching, rollups equal raw aggregates, retention, nothing lost on stop                                              |
| `sampler`                | One thread per provider, aligned ticks, rediscovery at run time                                                      |
| `follower_and_lock`      | Second sampler refused, lock released, follower republishes rows in order                                            |

### 8.2 Smoke test of the running system

With the system running (section 9) on port 8080:

```bash
curl -s localhost:8080/api/health                 # "status":"ok", "source":"local"
curl -s localhost:8080/api/sensors                # list of sensors with latest values
curl -s "localhost:8080/api/readings?minutes=5"   # bucketed history
curl -N localhost:8080/api/stream                 # live events (Ctrl+C to stop)
curl -s localhost:8080/api/stats                  # pipeline metrics
curl -s -o /dev/null -w "%{http_code}\n" localhost:8080/    # 200 = dashboard served
```

Alarm round-trip:

```bash
# create (sensor_id from /api/sensors)
curl -s -X POST localhost:8080/api/alarms -H 'Content-Type: application/json' \
     -d '{"sensor_id": 2, "func": "avg", "window_min": 1, "threshold": 30}'
curl -s localhost:8080/api/alarms                 # state becomes "firing" when avg > 30
curl -s localhost:8080/api/alarm-events           # fired / cleared history
curl -s -X DELETE localhost:8080/api/alarms/1     # 204
```

### 8.3 Manual feature checks

`TESTING.md` contains step-by-step checks with expected results for every feature: discovery, fallbacks, storage, live stream, rediscovery, API validation, alarms, poller + server follow/takeover, database migration, MQTT, dashboard UI, performance and real-hardware load. Those checks use port **8090** and a scratch directory `/tmp/sd-test`, so they never touch your real database.

### 8.4 Real-hardware load test

```bash
scripts/run_all.sh                    # terminal 1
stress -c "$(nproc)" -t 120           # terminal 2: all core temperatures climb within seconds
```

## 9. Running the whole project

### 9.1 One command (recommended)

```bash
scripts/run_all.sh
```

Then open **http://localhost:8080**. Stop with **Ctrl+C** (buffered readings are written to the database before the server exits).

What `run_all.sh` does:

0. Runs `scripts/setup_env.sh`: prints the required packages with their versions and installs any that are missing (section 4.0). It stops here if a requirement cannot be installed.
1. Configures and builds the C++ code (incrementally).
2. Builds the dashboard with npm (`npm ci && npm run build`) when `frontend/dist` does not exist yet or `frontend/src`, `index.html`, `package.json`, `package-lock.json` or `vite.config.js` is newer than it (so a dependency upgrade is installed automatically); on a fresh copy this happens on the first run. Without npm, a missing dashboard cannot be built: the server then runs API-only and `/` returns 404.
3. Prints the sensors found (`sensor-poller --list`).
4. Starts `sensor-server`, which samples, stores, evaluates alarms, serves the API, the live stream and the dashboard.

By default it samples **all** sensor families (CPU core temperatures, fans, batteries). Use `SENSOR_PROVIDERS=cpu_temp scripts/run_all.sh` for CPU core temperatures only.

### 9.2 With simulated sensors (VM, container, or no sensors)

```bash
scripts/run_all.sh --fake
```

This starts `scripts/fake_hwmon.sh`, which simulates a 10-core laptop (Core 27 runs hot so alarms can be seen firing), 2 fans and 1 battery, and runs the server against it with all providers enabled. Expected log:

```
==> sensors found (providers: all):
==> dashboard: http://localhost:8080   (Ctrl+C to stop)
[INFO] sampling 13 sensor(s): cpu_temp=10 fan=2 battery=1 [...] every 500 ms
```

### 9.3 Environment variables for `run_all.sh`

| Variable           | Default           | Meaning                                                                                                    |
| ------------------ | ----------------- | ---------------------------------------------------------------------------------------------------------- |
| `PORT`             | `8080`            | HTTP port                                                                                                  |
| `SENSOR_DB`        | `data/sensors.db` | Database file                                                                                              |
| `POLL_INTERVAL_MS` | `500`             | Sampling period                                                                                            |
| `SENSOR_PROVIDERS` | `all`             | `cpu_temp`, `fan`, `battery`, comma list, or `all`                                                         |
| `MQTT_URL`         | unset             | For example `localhost:1883` to publish to a broker; the environment check then also installs libmosquitto |
| `SETUP_OPTS`       | unset             | Extra options for `scripts/setup_env.sh`, e.g. `--with-docs` or `--all`                                    |
| `SKIP_SETUP`       | unset             | `1` skips the environment check (offline machines, no sudo)                                                |

Examples:

```bash
SENSOR_PROVIDERS=cpu_temp scripts/run_all.sh                  # CPU core temperatures only
PORT=9000 POLL_INTERVAL_MS=1000 scripts/run_all.sh            # other port, 1 s sampling
MQTT_URL=localhost:1883 scripts/run_all.sh                    # also publish to an MQTT broker
```

### 9.4 Running each component manually

```bash
# What does this machine expose?
./build/sensor-poller --list-providers
./build/sensor-poller --list --providers all

# Full system in one process
./build/sensor-server --db data/sensors.db --port 8080 --static frontend/dist
./build/sensor-server --providers all --mqtt localhost:1883     # + fans, battery, MQTT

# Against simulated hardware
bash scripts/fake_hwmon.sh /tmp/fake-sys &                     # keep running
./build/sensor-server --providers all \
    --hwmon-root /tmp/fake-sys/hwmon \
    --power-supply-root /tmp/fake-sys/power_supply \
    --thermal-root /tmp/fake-sys/thermal
```

### 9.5 Headless recording + dashboard (two processes)

```bash
# Terminal 1: record only (no HTTP)
./build/sensor-poller --db data/sensors.db

# Terminal 2: dashboard; detects the poller and follows the database instead of sampling
./build/sensor-server --db data/sensors.db --port 8080
curl -s localhost:8080/api/health          # "source":"db"
```

If the poller stops, the server takes over sampling within about 2 s.

### 9.6 Frontend development mode (hot reload)

```bash
# Terminal 1: backend
./build/sensor-server --port 8080

# Terminal 2: Vite dev server on http://localhost:5173 (/api is proxied to :8080)
cd frontend && npm run dev
# use another backend: API_URL=http://localhost:9000 npm run dev
```

### 9.7 Bind only to localhost (shared networks)

The API has no authentication and binds `0.0.0.0` by default:

```bash
./build/sensor-server --host 127.0.0.1 --port 8080
```



## 10. Configuration reference

All flags are shown by `./build/sensor-server --help` and `./build/sensor-poller --help`. Sampling, storage and MQTT flags are shared by both binaries.

| Binary | Flag                                                      | Default                     | Notes                                                                             |
| ------ | --------------------------------------------------------- | --------------------------- | --------------------------------------------------------------------------------- |
| both   | `--db PATH`                                               | `data/sensors.db`           | env `SENSOR_DB`                                                                   |
| both   | `--providers a,b\|all`                                    | `cpu_temp`                  | `cpu_temp`, `fan`, `battery` (env `SENSOR_PROVIDERS`)                             |
| both   | `--interval-ms N` / `--interval S`                        | `500`                       | Sampling period, minimum 50 ms (env `POLL_INTERVAL_MS`)                           |
| both   | `--rediscover S`                                          | `60`                        | Re-run discovery; `0` = never                                                     |
| both   | `--hwmon-root` / `--power-supply-root` / `--thermal-root` | `/sys/class/...`            | Point at a simulated tree (env `HWMON_ROOT`, `POWER_SUPPLY_ROOT`, `THERMAL_ROOT`) |
| both   | `--chips a,b`                                             | `coretemp,k10temp,zenpower` | hwmon drivers treated as CPU sensors                                              |
| both   | `--flush-ms N`                                            | `1000`                      | DB writer commit period                                                           |
| both   | `--raw-retention-hours N`                                 | `24`                        | Raw readings kept; `0` = forever                                                  |
| both   | `--retention-days N`                                      | `7`                         | 10 s rollups kept; `0` = forever                                                  |
| both   | `--mqtt HOST[:PORT]`                                      | off                         | env `MQTT_URL`; also `--mqtt-prefix`, `--mqtt-user`, `--mqtt-password`            |
| server | `--host ADDR` / `--port N`                                | `0.0.0.0` / `8080`          | env `PORT`                                                                        |
| server | `--static DIR`                                            | `frontend/dist`             | Built dashboard directory                                                         |
| server | `--source auto\|local\|db`                                | `auto`                      | Sample here, or follow a DB another process samples into                          |
| server | `--alarm-interval S`                                      | `1`                         | Alarm evaluation period                                                           |
| server | `--alarm-min-samples N`                                   | `3`                         | Samples needed before an alarm may fire                                           |
| server | `--max-live-clients N`                                    | `32`                        | Concurrent live dashboard streams                                                 |
| poller | `--list` / `--list-providers` / `--verbose`               |                             | Print sensors / providers / every sample                                          |

## 11. REST API and live stream

All timestamps are Unix epoch **milliseconds, UTC**. Errors use `{"error": "message"}` with a 4xx/5xx status.

| Method | Path                                                         | Purpose                                                                                                                                                                                                                                          |
| ------ | ------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| GET    | `/api/health`                                                | `status` (`ok` / `stale`), last sample time, readings in the last minute, `source` (`local` / `db`)                                                                                                                                              |
| GET    | `/api/sensors`                                               | All sensors with `unit`, `category`, latest value, number of firing alarms                                                                                                                                                                       |
| GET    | `/api/readings?minutes=15[&sensor_ids=1,2][&max_points=600]` | Bucketed history; each point `[ts, avg, min, max, count]`                                                                                                                                                                                        |
| GET    | `/api/stream`                                                | Server-Sent Events: `hello`, `samples`, `alarms`, `alarm`, `sensors`                                                                                                                                                                             |
| GET    | `/api/stats`                                                 | Pipeline metrics (queues, drops, tick/flush times, alarm eval time, MQTT)                                                                                                                                                                        |
| GET    | `/api/alarms[?sensor_id=]`                                   | Alarms with current state                                                                                                                                                                                                                        |
| POST   | `/api/alarms`                                                | Create: `{"sensor_id" \| "sensor_ids":[..] \| "all", "func", "window_min", "threshold", "op"}` → `201` (`op` optional: `">"` default, or `"<"`). `window_min` 1–100; `threshold` −50–100 °C, 0–5000 RPM, 0–100 % by sensor unit; otherwise `400` |
| PATCH  | `/api/alarms/{id}`                                           | Change `func`, `window_min`, `threshold`, `op`, `enabled` (same limits as create)                                                                                                                                                                |
| DELETE | `/api/alarms/{id}`                                           | `204`; events cascade                                                                                                                                                                                                                            |
| GET    | `/api/alarm-events?limit=50[&alarm_id=]`                     | Fired/cleared history, newest first                                                                                                                                                                                                              |

Create one alarm per sensor in one call (`"all"` means every sensor of every family; pass a list of ids to target only the cores, or only the fans):

```bash
curl -X POST localhost:8080/api/alarms -H 'Content-Type: application/json' \
     -d '{"sensor_ids": [1,2,3,4,5,6,7,8,9,10], "func": "max", "window_min": 5, "threshold": 90}'
```

Battery running low (fires **below** the threshold):

```bash
curl -X POST localhost:8080/api/alarms -H 'Content-Type: application/json' \
     -d '{"sensor_id": 13, "func": "min", "window_min": 5, "threshold": 20, "op": "<"}'
```

## 12. Data model

SQLite, WAL mode. Default file `data/sensors.db`.

| Table          | Contents                                                                                                                | Retention                      |
| -------------- | ----------------------------------------------------------------------------------------------------------------------- | ------------------------------ |
| `sensors`      | `id`, stable `key` (e.g. `coretemp/Core 4`), `chip`, `label`, `unit`, `kind`, `category`, `sort_order`, first/last seen | permanent                      |
| `readings`     | `(sensor_id, ts) → value`, `WITHOUT ROWID`                                                                              | `--raw-retention-hours` (24 h) |
| `readings_10s` | 10 s rollups: `n`, `sum`, `sumsq`, `min`, `max`                                                                         | `--retention-days` (7 days)    |
| `alarms`       | `sensor_id`, `func`, `window_min`, `threshold`, `op` (`>` or `<`), `enabled`, `state`, last value/eval/change           | permanent                      |
| `alarm_events` | `fired` / `cleared` transitions with value, threshold and `op`                                                          | cascade on alarm delete        |

- Narrow (long) table: any number of cores, fans or new sensor types without schema changes.
- Schema migrations run automatically at start-up inside one transaction.
- Size: about 62 MB per day of raw 500 ms data for 13 sensors; with the default retention the file levels off at about 100 MB.

Inspect it:

```bash
sqlite3 data/sensors.db ".tables"
sqlite3 data/sensors.db "SELECT id,label,unit FROM sensors;"
sqlite3 data/sensors.db "SELECT COUNT(*) FROM readings;"
```

## 13. Alarm engine

- **Rule:** fires when `func(values of its sensor over the last X minutes) op Y`. `func` is `avg`, `max`, `min` or `stddev` (population); `op` is `>` (default, fires above Y) or `<` (fires below Y, e.g. `min(BAT0 charge, 5 min) < 20`). Both comparisons are strict.
- **Per sensor:** each alarm is bound to exactly one sensor. "Every core" in the UI creates N independent alarms.
- **States:** `ok`, `firing`, `no_data` (fewer than `--alarm-min-samples` readings in the window; stddev needs at least 2), `disabled`.
- **Evaluation:** every second, from in-memory per-second aggregates for windows ≤ 60 min. Longer windows use the 10 s rollups every 10 s. Windows are warmed from the database at start-up.
- **History:** only transitions are stored (`fired`, `cleared`).
- **Notification:** server log (`WARN … FIRED`), live push to dashboards, retained MQTT message if the bridge is on.

## 14. Dashboard (frontend)

Served by `sensor-server` at `http://localhost:8080` from `frontend/dist`, which is built from `frontend/src` by `npm run build` (or automatically by `scripts/run_all.sh`).

- One tab per sensor category (CPU temperatures, Fans, Battery). The selected tab drives the charts, the sensor table and the Alarms section; a tab with firing alarms shows an orange `N firing` badge. Tabs are hidden when only CPU temperatures are enabled.
- Header with the Armory logo, one tag per sensor family (`10 cores`, `2 fans`, `1 battery`), the stream state (pulsing orange **Live**, grey `Reconnecting…`) and the time of the latest sample (24-hour clock, as everywhere on the page).
- Alert strip under the header, only while alarms fire: the count and up to four firing alarms with their conditions.
- Live updates over `/api/stream` (no polling).
- Time window: 5 min, 15 min, 1 h, 6 h, 24 h (about 600 points per series).
- Combined chart, plus small multiples per core/fan with alarm threshold lines (orange outline when firing).
- Sensor table with current / min / avg / max and visibility toggles.
- Alarm panel, limited to the selected tab's family (`Alarms: Fans`, ...): the Sensor dropdown offers only that family's sensors plus "Every … (one alarm each)", firing **above** or **below** the threshold with the family's suggestion pre-filled (above 75 °C, above 4500 RPM, below 20 %); inputs are validated as typed (window 1–100 min; threshold −50–100 °C, 0–5000 RPM or 0–100 %), invalid entries are outlined in orange with a message and **Add alarm** stays disabled; the alarm list (enable/disable, delete, live state) and the event log show only that family, with a note counting alarms of other families.
- Browser tab title: `System Sensors`, the same as the page heading.

Source files: `App.jsx` (layout), `live.js` (stream + bucketing), `TempChart.jsx`, `AlarmPanel.jsx`, `units.js` (formats per unit/category), `api.js` (REST calls).

## 15. MQTT bridge (optional)

Requires `libmosquitto-dev` at build time. The configure step must print `MQTT bridge: enabled`.

```bash
sudo systemctl start mosquitto            # or: mosquitto -v &
./build/sensor-server --providers all --mqtt localhost:1883
mosquitto_sub -v -t 'sensordash/#'        # in another terminal
```

| Topic                                   | Payload                                    |
| --------------------------------------- | ------------------------------------------ |
| `sensordash/<host>/status`              | `online` / `offline` (retained, last will) |
| `sensordash/<host>/sensors/<slug>/meta` | sensor metadata (retained)                 |
| `sensordash/<host>/<category>/<slug>`   | `{"ts","value","unit"}` every sample       |
| `sensordash/<host>/alarms/<id>`         | alarm transition (retained)                |

Change the prefix with `--mqtt-prefix`. The bridge reconnects automatically if the broker restarts.

## 16. Adding a new sensor type

1. Create `src/sensors/providers/<name>.{hpp,cpp}` with a class derived from `SensorProvider` that returns sensors from `discover()`. Use `SysfsValueSensor` for "integer in a sysfs file × scale".
2. Register it in `builtin_providers()` in `src/sensors/registry.cpp`.
3. Add the `.cpp` to the `sd_sensors` library in `CMakeLists.txt`, and add a test that builds a fake sysfs tree.
4. Rebuild and run with `--providers cpu_temp,<name>`.

Storage, API, alarms, live stream, MQTT and the dashboard pick up the new sensors with no other changes. Optionally add a title and format in `frontend/src/units.js`. README.md section "Adding a new sensor type" has a complete GPU-temperature example.

## 17. Troubleshooting

| Symptom                                                                          | Cause / fix                                                                                                                                                                                |
| -------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `Could NOT find SQLite3`                                                         | `sudo apt install libsqlite3-dev`                                                                                                                                                          |
| `==> environment setup failed` from run_all.sh                                   | The table above it shows what is `MISSING` / `TOO OLD`; install it (or fix sudo / network) and run again. `SKIP_SETUP=1` skips the check                                                   |
| Environment check asks for a password                                            | It runs the package manager through `sudo` to install missing packages; nothing is asked when everything is installed                                                                      |
| `Node.js … TOO OLD` after installing                                             | The distribution's Node.js is older than 18: install a current LTS (NodeSource or nvm), as the script prints                                                                               |
| `CMake 3.16 or higher is required`                                               | Upgrade CMake (`pip install cmake` or Kitware APT repo)                                                                                                                                    |
| CMake errors about a stale cache / different generator                           | Delete the build tree: `rm -rf build` and configure again                                                                                                                                  |
| `Permission denied` running a script                                             | `chmod +x scripts/*.sh` or run with `bash scripts/…`                                                                                                                                       |
| `no CPU temperature sensors found`                                               | VM/container: use `scripts/run_all.sh --fake`. Intel: `sudo modprobe coretemp`. Other driver: `--chips <name>`                                                                             |
| Server says another process is sampling the DB                                   | A `sensor-poller`/`sensor-server` is still running on the same `--db`: `pkill -f 'build/sensor-'`. The lock is released automatically when the process dies                                |
| `Address already in use`                                                         | Another process uses the port: `PORT=9000 scripts/run_all.sh`, or find it with `ss -ltnp \| grep 8080`                                                                                     |
| Blank page / 404 at `/`, log says `static dir frontend/dist not found; API only` | The dashboard is not built: `cd frontend && npm ci && npm run build` (or run `scripts/run_all.sh`, which builds it)                                                                        |
| `doxygen: command not found`                                                     | `scripts/setup_env.sh --with-docs` (or `sudo apt install doxygen graphviz`)                                                                                                                |
| Doxygen: `dot` not found / no diagrams                                           | Install `graphviz`, or set `HAVE_DOT = NO` in the `Doxyfile`                                                                                                                               |
| `npm ci` fails                                                                   | Node too old (need ≥ 18), or `package-lock.json` missing                                                                                                                                   |
| Vite "Some chunks are larger than 500 kB" warning                                | Does not occur with the current `vite.config.js` (vendor code is split into `react` and `charts` files). If a new large dependency brings it back, give it its own entry in `manualChunks` |
| `npm warn deprecated recharts@2…`                                                | The dashboard uses Recharts 3; an old `node_modules` is left over: `cd frontend && rm -rf node_modules && npm ci` (or `scripts/clean.sh`)                                                  |
| `MQTT bridge: disabled`                                                          | Install `libmosquitto-dev` and reconfigure (`rm -rf build` first)                                                                                                                          |
| `/api/health` shows `"status":"stale"`                                           | No samples for ≥ 5 s: sampling stopped or the poller being followed died                                                                                                                   |
| Fans show nothing on a laptop                                                    | Many laptops hide fan speed from Linux; the fan provider then finds nothing (expected)                                                                                                     |

## 18. Assumptions and limitations

- Linux only (sysfs). macOS/Windows would need additional providers.
- "All cores" means every per-core channel from the CPU hwmon driver; the package sensor is not tracked. AMD/ARM fall back to per-die or whole-CPU readings.
- `scripts/run_all.sh` enables all providers by default; the binaries default to `cpu_temp` and take `--providers`.
- No API authentication; use `--host 127.0.0.1` on shared networks.
- No email/webhook notifications, alarm hysteresis, systemd unit or Docker packaging yet.
- One sampling interval for all providers.

## 19. Command cheat sheet

```bash
# ---------- one-time setup ----------
cd ~/Desktop/Armory-Home-Assignment/Multi-Sensor-Dasboard
chmod +x scripts/*.sh
scripts/setup_env.sh             # check / install requirements (run_all.sh also does this)
scripts/setup_env.sh --all       # optional: + MQTT bridge + Doxygen/Graphviz

# ---------- clean ----------
scripts/clean.sh                 # add --all to also remove the built dashboard (frontend/dist)

# ---------- build ----------
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
(cd frontend && npm ci && npm run build)

# ---------- test ----------
ctest --test-dir build --output-on-failure
./build/sd-tests --list
./build/sd-tests alarm

# ---------- run ----------
scripts/run_all.sh               # real sensors      → http://localhost:8080
scripts/run_all.sh --fake        # simulated sensors → http://localhost:8080
SENSOR_PROVIDERS=cpu_temp PORT=9000 scripts/run_all.sh

# ---------- check ----------
curl -s localhost:8080/api/health
curl -s localhost:8080/api/sensors
curl -N localhost:8080/api/stream

# ---------- code documentation ----------
scripts/setup_env.sh --with-docs         # once
doxygen Doxyfile                         # → docs/html/index.html
```

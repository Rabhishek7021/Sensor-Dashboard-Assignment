# Rebuild and test guide

Step-by-step instructions to rebuild the project, run it as a whole, and test every feature on its own. Each check lists the expected result. All commands were run and verified on a clean copy of the project.

- [0. Prerequisites](#0-prerequisites)
- [1. Clean rebuild](#1-clean-rebuild)
- [2. Automated tests](#2-automated-tests)
- [3. Whole project, end to end](#3-whole-project-end-to-end)
- [4. Each feature on its own](#4-each-feature-on-its-own)
  - [4.0 Setup](#40-shared-setup) 
  - [4.1 Discovery](#41-sensor-discovery-on-this-machine) 
  - [4.2 Per provider](#42-discovery-per-provider-simulated-hardware) 
  - [4.3 CPU fallbacks](#43-cpu-fallbacks-amd-arm-no-sensors) 
  - [4.4 Sampling threads and storage](#44-sampling-threads-and-storage) 
  - [4.5 Live stream](#45-live-stream-server-sent-events) 
  - [4.6 Rediscovery](#46-rediscovery-hardware-appearing-at-run-time) 
  - [4.7 REST API](#47-rest-api) 
  - [4.8 Input validation](#48-api-input-validation) 
  - [4.9 Alarm engine](#49-alarm-engine) 
  - [4.10 Poller + server, stale data](#410-poller--server-stale-data-and-takeover) 
  - [4.11 Schema check](#411-database-schema-check-on-start-up) 
  - [4.12 Simultaneous start](#412-processes-starting-at-the-same-time) · [4.13 MQTT](#413-mqtt-bridge) 
  - [4.14 Build-time report](#414-build-time-sensor-report) 
  - [4.15 Dashboard](#415-dashboard-ui) 
  - [4.16 Performance and stability](#416-performance-and-stability) 
  - [4.17 Real load](#417-real-hardware-load-test)
- [5. Code documentation (Doxygen)](#5-code-documentation-doxygen)
- [6. Clean up](#6-clean-up)

## 0. Prerequisites

The project's own requirements are checked and installed by `scripts/setup_env.sh` (also run by `scripts/run_all.sh` before every build). For the checks in this guide, install everything including the optional parts, plus the tools the checks use:

```bash
scripts/setup_env.sh --all                     # compiler, CMake, SQLite, zlib, Node.js/npm, MQTT, Doxygen
sudo apt install sqlite3 python3 curl          # tools used by the checks below
```

**Expected:** a table of every package with `Minimum`, `Installed` and `Status`, all `ok`, ending with `==> all required packages are installed`. Missing packages are marked `MISSING`, installed (you may be asked for your sudo password), and the table is printed again.

Check-only mode does not install anything and fails if something required is missing:

```bash
scripts/setup_env.sh --check; echo "exit $?"   # exit 0 when everything is installed
```

Equivalent manual installation (Ubuntu/Debian):

```bash
sudo apt install build-essential cmake libsqlite3-dev sqlite3 python3 curl
sudo apt install nodejs npm                                  # builds the dashboard (frontend/dist)
sudo apt install doxygen graphviz                            # code documentation (section 5)
sudo apt install libmosquitto-dev mosquitto mosquitto-clients   # optional: MQTT bridge (4.13)
```

`sqlite3` (the command-line tool), `python3`, `mosquitto` and `mosquitto-clients` are only used by the checks below. `libmosquitto-dev` enables the optional MQTT bridge at build time.

## 1. Clean rebuild

```bash
cd ~/Desktop/Armory-Home-Assignment/Multi-Sensor-Dasboard

rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

# Dashboard: builds frontend/dist from frontend/src
cd frontend && npm ci && npm run build && cd ..
```

**Expected**

- The configure step prints `MQTT bridge: enabled` (or `disabled` without libmosquitto) and `HTTP compression: gzip enabled`.

- No warnings or errors.

- `build/` contains `sensor-server`, `sensor-poller` and `sd-tests`.

- `npm ci` and `npm run build` print **no warnings** (no deprecated packages, no chunk-size warning). `frontend/dist/assets/` contains the stylesheet (~9 kB), the IBM Plex font files (`ibm-plex-mono-…`, `ibm-plex-sans-…`) and three JavaScript files, all below 500 kB: `index-….js` (dashboard, ~18 kB), `react-….js` (~142 kB) and `charts-….js` (Recharts 3, ~382 kB). The build ends with `✓ built in …`.

- The build ends with a report of what this machine has, for example on the dev laptop:
  
  ```
  -- Sensors detected on this machine (10 CPU core temperature sensor(s)):
  KEY                           CATEGORY     KIND    VALUE
  coretemp/Core 0               temperature  core    40.0 celsius
  ...
  ```
  
  In a VM or container it prints `No sensors detected on this build machine` instead. That is fine (see [4.14](#414-build-time-sensor-report)).

## 2. Automated tests

```bash
ctest --test-dir build --output-on-failure        # everything
```

**Expected:** `100% tests passed, 0 tests failed out of 20` (all groups together, plus each of the 19 groups on its own; 274 checks).

Run one feature's tests on their own:

```bash
./build/sd-tests --list                 # list test groups
./build/sd-tests cpu_temp               # all CPU discovery tests
./build/sd-tests bus                    # publish/subscribe bus
./build/sd-tests db_writer sampler      # pipeline threads
./build/sd-tests alarm                  # alarm logic + alarm engine
ctest --test-dir build -R migrate       # same, through CTest
```

| Group                    | What it proves                                                                                                                                                               |
| ------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `cpu_temp_single_socket` | Laptop layout: package skipped, 10 cores in order Core 0, 4, 8 … 27, unrelated chips ignored, unreadable values handled                                                      |
| `cpu_temp_multi_socket`  | Two CPUs with repeated `Core 0` labels get unique keys and `CPU0`/`CPU1` labels                                                                                              |
| `cpu_temp_fallbacks`     | AMD per-die (`Tccd`), whole-CPU (`Tctl`), and ARM thermal-zone fallbacks                                                                                                     |
| `fan_provider`           | RPM reading, stopped fan (0 RPM), fans on several chips                                                                                                                      |
| `battery_provider`       | `capacity` file, `energy_now/energy_full` fallback, peripheral batteries ignored, no battery                                                                                 |
| `registry_and_manager`   | Unknown provider rejected, `all`, providers ordered CPU → fan → battery                                                                                                      |
| `custom_provider`        | A provider written outside the project plugs in without changes (the extension point)                                                                                        |
| `store_and_aggregates`   | Stable ids, avg/min/max/stddev, bucketed queries, retention                                                                                                                  |
| `migrate_drops_package`  | A package sensor in the database is removed with its readings, alarms and events                                                                                             |
| `migrate_adds_category`  | A database without the `category` column gets it; ids are kept                                                                                                               |
| `migrate_adds_op`        | Alarm tables without the `op` column get it; existing alarms and events become `>`, ids are kept, new `<` alarms round-trip                                                  |
| `alarm_logic`            | Every function, strict `>` and strict `<`, operator validation, minimum samples                                                                                              |
| `alarm_below_threshold`  | A battery alarm `min < 20` fires at 15 % and clears when the charge rises; events record `<`; changing `op` rejects a stale evaluation and re-evaluates                      |
| `alarm_engine_per_core`  | Warm-up from the DB, live samples from the bus, per-core independence, transition messages, long windows from rollups, fired/cleared events, disable, stale-write protection |
| `bus`                    | MQTT topic filters (`+`, `#`), delivery, drop-oldest overflow, timeouts, 4 threads × 10 000 messages with nothing lost                                                       |
| `second_window`          | In-memory alarm window equals a brute-force computation over 4 h of data                                                                                                     |
| `db_writer`              | Batching (240 readings in ≤ 5 commits), rollups equal raw aggregates, rollup/raw stitching, retention, nothing lost on stop                                                  |
| `sampler`                | One thread per provider, shared aligned ticks, registration, a battery appearing while running                                                                               |
| `follower_and_lock`      | Only one sampler per database, lock released with its holder, rows from another process republished in order                                                                 |

**Optional: thread and memory checkers.** Builds the tests with ThreadSanitizer (data races) and AddressSanitizer + UBSan (memory errors, undefined behaviour):

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DSD_REPORT_SENSORS=OFF \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan -j"$(nproc)" && ./build-tsan/sd-tests 2>&1 | grep -c "WARNING: ThreadSanitizer"

cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DSD_REPORT_SENSORS=OFF \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j"$(nproc)" --target sd-tests && ./build-asan/sd-tests 2>&1 | grep -c -E "ERROR: AddressSanitizer|runtime error"
```

**Expected:** both print `0`, and the tests pass.

## 3. Whole project, end to end

**Real sensors of this machine** (CPU cores, fans and batteries; the default is `SENSOR_PROVIDERS=all`):

```bash
scripts/run_all.sh
```

**CPU core temperatures only:**

```bash
SENSOR_PROVIDERS=cpu_temp scripts/run_all.sh
```

**Real sensors, also publishing to a local MQTT broker:**

```bash
MQTT_URL=localhost:1883 scripts/run_all.sh
```

**Simulated laptop** (10 cores with Core 27 running hot, 2 fans, 1 battery; works anywhere):

```bash
scripts/run_all.sh --fake
```

Then open **http://localhost:8080**.

**Expected**

- The terminal first shows `==> build environment (package manager: …)` with the package table and `==> all required packages are installed` (missing packages are installed at this point).

- It then shows `==> building C++ components`, `==> building frontend` on the first run, `==> sensors found` with a table, then `sampling N sensor(s): cpu_temp=… fan=… battery=… every 500 ms` (13 with `--fake`: 10 cores, 2 fans, 1 battery; on real hardware the counts depend on the machine) and `==> dashboard: http://localhost:8080`.

- `curl -s localhost:8080/api/health` returns `"status":"ok"` and `"source":"local"`.

- The dashboard shows a pulsing orange **Live** marker and values change twice per second; the checklist in [4.15](#415-dashboard-ui) passes.

Stop with **Ctrl+C** (buffered readings are written before exit). If `run_all.sh` says "Permission denied", run `chmod +x scripts/*.sh` or use `bash scripts/run_all.sh`.

**Environment check behaviour:**

```bash
SKIP_SETUP=1 scripts/run_all.sh --fake          # no package table: the check is skipped
SETUP_OPTS=--with-docs scripts/run_all.sh --fake   # the check also covers Doxygen and Graphviz
```

**Expected:** with `SKIP_SETUP=1` the output starts directly with `==> building C++ components`; with `SETUP_OPTS=--with-docs` the Doxygen and Graphviz rows show `ok` instead of `ok (optional)` (and are installed if missing). If a required package cannot be installed (for example a Node.js older than 18 from the distribution), `run_all.sh` stops with `==> environment setup failed: fix the packages listed above, or run with SKIP_SETUP=1`.

## 4. Each feature on its own

The checks below use a throw-away database and port **8090**, so they never touch `data/sensors.db` or a dashboard running on 8080. Several terminals are used. They are called **A** (fake hardware), **B** (poller, only in 4.10), **C** (server) and **D** (your commands).

### 4.0 Shared setup

Paste this into **every** terminal you open:

```bash
cd ~/Desktop/Armory-Home-Assignment/Multi-Sensor-Dasboard
export T=/tmp/sd-test
export FAKE="--hwmon-root $T/sys/hwmon --power-supply-root $T/sys/power_supply --thermal-root $T/sys/thermal"
export API=http://localhost:8090/api
id_of() { curl -s $API/sensors | python3 -c "import json,sys; print(next(s['id'] for s in json.load(sys.stdin) if s['label'] == sys.argv[1]))" "$1"; }
show() { python3 -c "import json,sys; d=json.load(sys.stdin); d=d.get('created',d) if isinstance(d,dict) else d; d=d if isinstance(d,list) else [d]; [print(a['id'], a['sensor_label'], a['condition'], '->', a['state'], a['last_value']) for a in d]"; }
```

In terminal **A**, start the simulated hardware and leave it running:

```bash
rm -rf $T && mkdir -p $T
bash scripts/fake_hwmon.sh $T/sys
```

### 4.1 Sensor discovery on this machine

```bash
./build/sensor-poller --list-providers
./build/sensor-poller --list                    # CPU cores (default)
./build/sensor-poller --list --providers all    # + fans + batteries
```

**Expected**

- Three providers are listed: `cpu_temp`, `fan`, `battery`.
- On the dev laptop, `--list` shows exactly 10 rows, `coretemp/Core 0`, `Core 4`, `Core 8`, `Core 12`, `Core 16`, `Core 20`, `Core 24`, `Core 25`, `Core 26`, `Core 27`, in that order, with **no** `Package id 0`.

Cross-check against the kernel:

```bash
grep -H . /sys/class/hwmon/hwmon*/name                 # which chip is coretemp
grep -h . /sys/class/hwmon/hwmon*/temp*_label          # all labels, incl. "Package id 0"
sensors                                                # if lm-sensors is installed
```

The core labels must match the `Core N` lines exactly. The fan list only shows fans that your laptop's firmware exposes; many HP laptops expose none, and an empty fan list is then correct.

### 4.2 Discovery per provider (simulated hardware)

```bash
./build/sensor-poller $FAKE --list --providers all
./build/sensor-poller $FAKE --list --providers fan
./build/sensor-poller $FAKE --list --providers battery
```

**Expected:** 13 sensors with `all`. Fans and battery look like this:

```
KEY                           CATEGORY     KIND    VALUE
fan:thinkpad/fan1             fan          fan     2738.0 rpm
fan:thinkpad/fan2             fan          fan     1951.0 rpm
battery:BAT0/charge           battery      charge  95.0 percent
```

### 4.3 CPU fallbacks (AMD, ARM, no sensors)

**AMD (no per-core sensors):** build a tiny fake AMD chip.

```bash
A=$T/amd/hwmon/hwmon0; mkdir -p $A
echo k10temp > $A/name
echo Tctl  > $A/temp1_label; echo 61000 > $A/temp1_input
echo Tccd1 > $A/temp3_label; echo 58000 > $A/temp3_input
echo Tccd2 > $A/temp4_label; echo 59000 > $A/temp4_input
./build/sensor-poller --hwmon-root $T/amd/hwmon --thermal-root $T/none --list
```

**Expected:** `using 2 per-die sensor(s)` with `k10temp/Tccd1` and `k10temp/Tccd2` (kind `die`).

```bash
rm $A/temp3_* $A/temp4_*
./build/sensor-poller --hwmon-root $T/amd/hwmon --thermal-root $T/none --list
```

**Expected:** `using whole-CPU temperature` with `k10temp/Tctl` (kind `cpu`).

**ARM board (no CPU hwmon driver):**

```bash
Z=$T/arm/thermal/thermal_zone0; mkdir -p $Z
echo cpu-thermal > $Z/type; echo 47250 > $Z/temp
./build/sensor-poller --hwmon-root $T/none --thermal-root $T/arm/thermal --list
```

**Expected:** `using 1 CPU thermal zone(s)` with `thermal/cpu-thermal#0` at 47.2 °C.

**No sensors at all, and a wrong provider name:**

```bash
./build/sensor-poller --hwmon-root $T/none --thermal-root $T/none --list; echo "exit=$?"
./build/sensor-poller --providers gpu --list; echo "exit=$?"
```

**Expected:** an error that lists the hwmon chips present and suggests `modprobe`, with `exit=1`. Then `unknown sensor provider 'gpu'` with `exit=2`.

### 4.4 Sampling threads and storage

Terminal **C**: the server samples, stores, evaluates alarms and serves the API, all in one process.

```bash
./build/sensor-server --db $T/test.db --port 8090 --static frontend/dist $FAKE --providers all
```

**Expected:** `sampling 13 sensor(s): cpu_temp=10 fan=2 battery=1 [...] every 500 ms`, then `listening on http://0.0.0.0:8090 (…, source: local)`.

Terminal **D**, after a few seconds:

```bash
curl -s $API/stats | python3 -c "import json,sys; d=json.load(sys.stdin); print('sampler  ', d['sampler']); print('db_writer', d['db_writer']); print('bus      ', [(s['name'], s['received'], s['dropped']) for s in d['bus']['subscribers']])"
sqlite3 -header -column $T/test.db "SELECT id, label, category, kind, unit FROM sensors ORDER BY sort_order;"
sqlite3 -header -column $T/test.db "SELECT ts, ts % 500 AS off_grid, COUNT(*) AS sensors_in_tick FROM readings WHERE sensor_id IN (1,11,13) GROUP BY ts ORDER BY ts DESC LIMIT 4 OFFSET 4;"
sqlite3 -header -column $T/test.db "SELECT s.label, COUNT(*) AS n, ROUND(MIN(r.value),1) AS min, ROUND(MAX(r.value),1) AS max
                                    FROM readings r JOIN sensors s ON s.id = r.sensor_id GROUP BY s.id ORDER BY s.sort_order;"
```

**Expected**

- `sampler`: `threads` = 4 (one per provider plus the rediscovery supervisor), `read_failures` = 0, `skipped_ticks` = 0, `last_tick_us` well under 1000.
- `db_writer`: `flushes` about one per second (not one per tick), `errors` = 0, `dropped` = 0.
- `bus`: subscribers `alarm-engine`, `live-cache`, `db-writer`, all with `dropped` 0.
- 13 sensors with the right category and unit. Ticks are exactly 500 ms apart (`off_grid` = 0), and the CPU, fan and battery providers share each timestamp (`sensors_in_tick` = 3 for the three sensors queried).
- Core 27 is around 66–79 °C, the other cores around 34–50 °C, the fans in the thousands of RPM.

After 10–20 s the 10-second rollups appear:

```bash
sqlite3 -header -column $T/test.db "SELECT bucket, n, ROUND(sum/n,2) AS avg, min, max FROM readings_10s WHERE sensor_id = 1 ORDER BY bucket DESC LIMIT 3;"
```

**Expected:** one row per 10 s with `n` = 20 (20 samples at 500 ms; the first bucket after start can have fewer).

Leave the server running for 4.5–4.9.

### 4.5 Live stream (Server-Sent Events)

```bash
timeout 3 curl -sN $API/stream
```

**Expected:** `event: hello` with `"interval_ms":500`, then `event: samples` twice per second, each carrying one tick for all 13 sensors, for example `[{"ts":1793346818500,"v":{"1":36.150,…,"13":61.000}}]`, and `event: alarms` once per second.

Measure the delay from sampling to a client:

```bash
python3 - <<'EOF'
import json, time, urllib.request
r = urllib.request.urlopen("http://localhost:8090/api/stream"); lat = []; ev = None; end = time.time() + 10
while time.time() < end:
    line = r.readline().decode().rstrip("\n")
    if line.startswith("event: "): ev = line[7:]
    elif line.startswith("data: ") and ev == "samples":
        lat += [time.time() * 1000 - t["ts"] for t in json.loads(line[6:])]
lat.sort(); print(f"{len(lat)} ticks, median {lat[len(lat)//2]:.1f} ms, max {lat[-1]:.1f} ms")
EOF
```

**Expected:** about 20 ticks in 10 s (one event per tick), median a few ms, max under about 10 ms on an idle machine.

### 4.6 Rediscovery (hardware appearing at run time)

The standalone poller runs the same sampler; here it watches an empty battery directory.

```bash
mkdir -p $T/re/power_supply
./build/sensor-poller --db $T/re.db --hwmon-root $T/sys/hwmon --power-supply-root $T/re/power_supply \
    --providers cpu_temp,battery --rediscover 5 &
sleep 2
mkdir -p $T/re/power_supply/BAT1
echo Battery > $T/re/power_supply/BAT1/type
echo 64 > $T/re/power_supply/BAT1/capacity
sleep 7; kill %1
```

**Expected:** the first log line says `battery=0`. Within 5 seconds of adding the battery: `sensor set changed, now 11 sensor(s): cpu_temp=10 battery=1 [... BAT1 charge]`, and the poller exits cleanly on `kill`.

### 4.7 REST API

```bash
curl -s $API/health; echo
curl -s $API/sensors | python3 -m json.tool | head -15
for m in 5 15 1440; do curl -s "$API/readings?minutes=$m" | python3 -c "import json,sys; d=json.load(sys.stdin); s=d['series'][0]; print('minutes', d['minutes'], '| bucket_ms', d['bucket_ms'], '|', len(d['series']), 'series |', s['label'], 'first point', s['points'][0] if s['points'] else None)"; done
curl -s "$API/readings?minutes=5&sensor_ids=$(id_of 'Core 0'),$(id_of 'Fan 1')" | python3 -c "import json,sys; print([s['label'] for s in json.load(sys.stdin)['series']])"
curl -s -H 'Accept-Encoding: gzip' -o /dev/null -w 'readings 15 min, gzip: %{size_download} bytes\n' "$API/readings?minutes=15"
```

**Expected**

- Health: `"status":"ok"`, `"source":"local"`, `"interval_ms":500`, `readings_last_minute` > 0.
- Each sensor has `category`, `unit` and a `latest` value.
- Readings: 13 series. `bucket_ms` is 500 for 5 min, 1500 for 15 min and 150000 for 24 h (always about 600 points). Each point is `[ts, avg, min, max, count]`.
- The filtered request returns `['Core 0', 'Fan 1']`.
- The gzip size is a fraction of the plain response.

### 4.8 API input validation

```bash
curl -s -w '  -> HTTP %{http_code}\n' "$API/readings?minutes=0"
curl -s -w '  -> HTTP %{http_code}\n' -X POST $API/alarms -H 'Content-Type: application/json' -d '{"sensor_id":1,"func":"median","window_min":5,"threshold":70}'
curl -s -w '  -> HTTP %{http_code}\n' -X POST $API/alarms -H 'Content-Type: application/json' -d '{"sensor_id":1,"func":"avg","window_min":0,"threshold":70}'
curl -s -w '  -> HTTP %{http_code}\n' -X POST $API/alarms -H 'Content-Type: application/json' -d '{"sensor_id":1,"func":"avg","window_min":5,"threshold":70,"op":">="}'
curl -s -w '  -> HTTP %{http_code}\n' -X POST $API/alarms -H 'Content-Type: application/json' -d '{"sensor_id":999,"func":"avg","window_min":5,"threshold":70}'
curl -s -w '  -> HTTP %{http_code}\n' -X POST $API/alarms -H 'Content-Type: application/json' -d 'not json'
curl -s -w '  -> HTTP %{http_code}\n' -X PATCH $API/alarms/999 -H 'Content-Type: application/json' -d '{"enabled":false}'
curl -s -w '  -> HTTP %{http_code}\n' -X DELETE $API/alarms/999
```

**Expected:** `400`, `400`, `400`, `400 op must be ">" … or "<" …`, `404 sensor 999 not found`, `400 invalid JSON body`, `404 alarm not found`, `404 alarm not found`. Each response has a JSON `{"error": "..."}` body.

### 4.9 Alarm engine

```bash
HOT=$(id_of 'Core 27'); COOL=$(id_of 'Core 0'); FAN=$(id_of 'Fan 1'); BAT=$(id_of 'BAT0 charge')
show() { python3 -c "import json,sys; d=json.load(sys.stdin); d=d.get('created',d) if isinstance(d,dict) else d; d=d if isinstance(d,list) else [d]; [print(a['id'], a['sensor_label'], a['condition'], '->', a['state'], a['last_value']) for a in d]"; }

# Same rule on a hot and a cool core: one fires, the other does not (per-core alarms)
curl -s -X POST $API/alarms -H 'Content-Type: application/json' \
  -d "{\"sensor_ids\":[$HOT,$COOL],\"func\":\"avg\",\"window_min\":1,\"threshold\":60}" | show
# Fan alarm in RPM
curl -s -X POST $API/alarms -H 'Content-Type: application/json' \
  -d "{\"sensor_id\":$FAN,\"func\":\"max\",\"window_min\":1,\"threshold\":2000}" | show
# stddev alarm
curl -s -X POST $API/alarms -H 'Content-Type: application/json' \
  -d "{\"sensor_id\":$HOT,\"func\":\"stddev\",\"window_min\":1,\"threshold\":50}" | show
# Below-threshold battery alarms ("op":"<"): the simulated battery is at 95 % or less
curl -s -X POST $API/alarms -H 'Content-Type: application/json' \
  -d "{\"sensor_id\":$BAT,\"func\":\"min\",\"window_min\":1,\"threshold\":99,\"op\":\"<\"}" | show
curl -s -X POST $API/alarms -H 'Content-Type: application/json' \
  -d "{\"sensor_id\":$BAT,\"func\":\"min\",\"window_min\":1,\"threshold\":10,\"op\":\"<\"}" | show
```

**Expected**

```
1 Core 27 avg(value, last 1 min) > 60 -> firing 76.7…
2 Core 0 avg(value, last 1 min) > 60 -> ok 44.6…
3 Fan 1 max(value, last 1 min) > 2000 -> firing 3309.0
4 Core 27 stddev(value, last 1 min) > 50 -> ok 2.1…
5 BAT0 charge min(value, last 1 min) < 99 -> firing 9x.0
6 BAT0 charge min(value, last 1 min) < 10 -> ok 9x.0
```

Terminal C logs `alarm #1 (Core 27: …) FIRED`, `alarm #3 (Fan 1: …) FIRED` and `alarm #5 (BAT0 charge: min over 1 min < 99.00) FIRED`. Every alarm is evaluated each second from memory; open dashboards get the change pushed at once.

Edit, disable, read the history, delete:

```bash
curl -s -X PATCH $API/alarms/1 -H 'Content-Type: application/json' -d '{"threshold":95}' | show    # -> ok (cleared)
curl -s -X PATCH $API/alarms/2 -H 'Content-Type: application/json' -d '{"enabled":false}' | show  # -> disabled
curl -s -X PATCH $API/alarms/6 -H 'Content-Type: application/json' -d '{"op":">"}' | show         # 9x > 10 -> firing
curl -s "$API/alarm-events?limit=10" | python3 -c "import json,sys; [print(e['alarm_id'], e['sensor_label'], e['type'], round(e['value'] or 0,1), e['op'], e['threshold']) for e in json.load(sys.stdin)]"
curl -s -o /dev/null -w 'delete -> HTTP %{http_code}\n' -X DELETE $API/alarms/3
curl -s $API/alarms | show
```

**Expected**

- Alarm 1 goes to `ok`, alarm 2 to `disabled`, and alarm 6 (condition now `min(value, last 1 min) > 10`) to `firing`.

- Events, newest first: `6 BAT0 charge fired … > 10`, `1 Core 27 cleared … > 95`, then the `fired` events of alarms 5 (`< 99`), 3 and 1. Each event shows the comparison of its alarm. Only state changes are recorded, never repeats.

- The delete returns `204`, and alarm 3 is gone from the list (its events are deleted with it).

### 4.10 Poller + server, stale data and takeover

Stop the server in terminal **C** (Ctrl+C). Start a standalone poller in terminal **B**:

```bash
./build/sensor-poller --db $T/test.db $FAKE --providers all
```

Then start the server again in terminal **C**, told to only follow the database:

```bash
./build/sensor-server --db $T/test.db --port 8090 --static frontend/dist $FAKE --providers all --source db
```

**Expected:** the server logs `another process (pid …) samples into …; following the database for live data`, and `curl -s $API/health` shows `"source":"db"`. Live data still flows (`timeout 3 curl -sN $API/stream`), up to about 1.5 s behind.

A second sampler on the same database is refused:

```bash
./build/sensor-poller --db $T/test.db $FAKE; echo "exit=$?"
```

**Expected:** `another process (pid …) is already sampling into …` and `exit=1`.

**Stale data:** stop the poller in terminal **B** (Ctrl+C), then in **D**:

```bash
until curl -s $API/alarms | grep -q no_data; do sleep 2; done
curl -s $API/alarms | show; curl -s $API/health; echo
```

**Expected:** about 1 minute later (the alarm window), enabled alarms switch to `no_data`, never to `ok`, and health reports `"status":"stale"`.

**Takeover:** stop the server (C). Start the poller again in **B**, then the server in **C** without `--source db` (the default is `auto`):

```bash
./build/sensor-server --db $T/test.db --port 8090 --static frontend/dist $FAKE --providers all
```

**Expected:** the server follows the running poller (`"source":"db"`). Now stop the poller (Ctrl+C in B): within 2 s the server logs `the other sampling process stopped; sampling here from now on`, and health shows `"source":"local"` again. Leave this server running.

### 4.11 Database schema check on start-up

Create a database that differs from the current schema (no `category` column, a `ts` index, a package sensor):

```bash
rm -f $T/upgrade.db*
sqlite3 $T/upgrade.db <<'SQL'
CREATE TABLE sensors (id INTEGER PRIMARY KEY, key TEXT NOT NULL UNIQUE, chip TEXT NOT NULL,
  label TEXT NOT NULL, unit TEXT NOT NULL, kind TEXT NOT NULL, sort_order INTEGER NOT NULL,
  first_seen INTEGER NOT NULL, last_seen INTEGER NOT NULL);
CREATE TABLE readings (sensor_id INTEGER NOT NULL REFERENCES sensors(id), ts INTEGER NOT NULL,
  value REAL NOT NULL, PRIMARY KEY (sensor_id, ts)) WITHOUT ROWID;
CREATE INDEX idx_readings_ts ON readings(ts);
INSERT INTO sensors VALUES
  (1, 'coretemp/Package id 0', 'coretemp', 'Package id 0', 'celsius', 'package', 0, 1, 1),
  (2, 'coretemp/Core 0',       'coretemp', 'Core 0',       'celsius', 'core', 1000, 1, 1);
INSERT INTO readings VALUES
  (1, strftime('%s','now')*1000 - 60000, 55.0),
  (2, strftime('%s','now')*1000 - 60000, 41.0),
  (2, strftime('%s','now')*1000 - 30000, 42.0);
SQL
timeout 2 ./build/sensor-server --db $T/upgrade.db --port 8091 --source db > /dev/null 2>&1
sqlite3 -header -column $T/upgrade.db "SELECT id, label, kind, category FROM sensors;"
sqlite3 -header -column $T/upgrade.db "SELECT sensor_id, COUNT(*) AS readings FROM readings GROUP BY sensor_id;"
sqlite3 $T/upgrade.db "SELECT name FROM sqlite_master WHERE name IN ('idx_readings_ts', 'readings_10s');"
```

(`--source db` keeps the server from sampling, so only the migration runs.)

**Expected:** only `2 | Core 0 | core | temperature` remains, sensor 2 still has its 2 readings, the package sensor and its readings are gone, and the last query prints only `readings_10s` (the `idx_readings_ts` index was dropped, the rollup table created).

To check your real database the same way, back it up first, then run the project once:

```bash
cp data/sensors.db data/sensors.db.bak
sqlite3 -header -column data/sensors.db "SELECT id, label, kind, category FROM sensors ORDER BY sort_order;"
```

### 4.12 Processes starting at the same time

Both processes update the database format on start-up, and both try to become the sampler. This starts a server and a poller together 10 times on a copy of the database from 4.11:

```bash
fails=0
for i in $(seq 10); do
  rm -f $T/race.db*; cp $T/upgrade.db $T/race.db
  ./build/sensor-server --db $T/race.db --port $((9100+i)) $FAKE > $T/race-server.log 2>&1 & srv=$!
  ./build/sensor-poller --db $T/race.db $FAKE > $T/race-poller.log 2>&1 & pol=$!
  sleep 4; kill $srv $pol 2>/dev/null; wait $srv $pol 2>/dev/null
  samplers=$(grep -l -E "sampling 10 sensor|discovered 10 sensor" $T/race-*.log | wc -l)
  if grep -q fatal $T/race-*.log || [ "$samplers" -ne 1 ]; then fails=$((fails+1)); fi
done; echo "failed starts: $fails/10"
```

**Expected:** `failed starts: 0/10`: no errors, and exactly one of the two processes samples each time (the other follows or exits).

**Database busy for longer than SQLite's 5 s wait:** start-up retries instead of exiting.

```bash
rm -f $T/lock.db*
sqlite3 $T/lock.db "PRAGMA journal_mode=WAL; CREATE TABLE t(a);" > /dev/null
sqlite3 $T/lock.db "BEGIN IMMEDIATE; INSERT INTO t VALUES(1);" ".shell sleep 7" "COMMIT;" &   # hold a write lock for 7 s
sleep 0.5
timeout 15 ./build/sensor-poller --db $T/lock.db $FAKE > $T/lock.log 2>&1
grep -E "fatal|discovered" $T/lock.log
```

**Expected:** a `discovered 10 sensor(s)` line after about 7 seconds, and no `fatal: database is locked`.

### 4.13 MQTT bridge

Needs a build with `MQTT bridge: enabled` and the `mosquitto` packages. In terminal **D**, start a local test broker:

```bash
mosquitto -p 1884 &
```

Stop the server in terminal **C** and start it with the bridge:

```bash
./build/sensor-server --db $T/test.db --port 8090 --static frontend/dist $FAKE --providers all \
    --mqtt localhost:1884 --mqtt-prefix sensordash/test
```

Terminal **D**:

```bash
mosquitto_sub -p 1884 -W 3 -v -t 'sensordash/#' | head -20
mosquitto_sub -p 1884 -W 3 -t 'sensordash/test/temperature/#' | wc -l
curl -s -X POST $API/alarms -H 'Content-Type: application/json' -d "{\"sensor_id\":$(id_of 'Core 27'),\"func\":\"avg\",\"window_min\":1,\"threshold\":60}" > /dev/null
mosquitto_sub -p 1884 -W 1 -v -t 'sensordash/test/alarms/#'
curl -s $API/stats | python3 -c "import json,sys; print(json.load(sys.stdin)['mqtt'])"
```

**Expected**

- `sensordash/test/status online`, retained `sensordash/test/sensors/<slug>/meta` messages, and samples such as `sensordash/test/temperature/core_27 {"ts":…,"value":75.002,"unit":"celsius"}` and `sensordash/test/fan/fan_1 {…"unit":"rpm"}`.
- About 60 temperature messages in 3 s (10 cores × 2 per second).
- A retained `sensordash/test/alarms/<id>` message with `"state":"firing"`, delivered even to a subscriber that connects later.
- `connected: True`, `failed: 0`.
- `mosquitto_sub` prints `Timed out` when its `-W` time limit ends; that is normal.

Restart the broker in terminal **D** (`kill %1; sleep 3; mosquitto -p 1884 &`): the server logs `mqtt: disconnected …, reconnecting` then `mqtt: connected`, and samples flow again. If the server is killed with `kill -9`, `mosquitto_sub -p 1884 -W 1 -v -t sensordash/test/status` shows `offline` (the last will), after the broker's keep-alive timeout (about 45 s).

Afterwards stop the broker (`kill %1` in D) and restart the server in C without `--mqtt` for the next steps.

### 4.14 Build-time sensor report

The report runs automatically after `sensor-poller` is linked. Run it by hand, against the real machine and then the simulated one:

```bash
cmake -DPOLLER=build/sensor-poller -P cmake/ReportSensors.cmake
HWMON_ROOT=$T/sys/hwmon POWER_SUPPLY_ROOT=$T/sys/power_supply \
  cmake -DPOLLER=build/sensor-poller -P cmake/ReportSensors.cmake
```

**Expected:** on the laptop, `Sensors detected on this machine (10 CPU core temperature sensor(s))`. With the simulated tree you see the same count plus the fans and battery.

Turn the report off (for example in CI): `cmake -S . -B build -DSD_REPORT_SENSORS=OFF`.

The report is information only. Sampling always discovers sensors again when it starts, so a binary built on one machine reports the correct cores on another.

### 4.15 Dashboard UI

With the server running against the simulated hardware, open **http://localhost:8090** and check:

| Check                                                                         | Expected                                                                                                                                                                                                                                                                                              |
| ----------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Header                                                                        | Tags `10 cores`, `2 fans`, `1 battery`, a pulsing orange **Live** marker and the time of the latest sample (24-hour clock), ticking                                                                                                                                                                   |
| Look                                                                          | Armory logo above `SYSTEM SENSORS`; dark dot-textured page, dark panels with corner brackets (no light-coloured sections), headings and tabs in uppercase IBM Plex Mono (fonts load with no internet connection); the selected tab has the panel colour and a light top edge, the others are outlined |
| Network (browser DevTools)                                                    | After the page loads: `readings`, `sensors`, `alarms`, `alarm-events` once, then only the open `/api/stream`; no repeated requests                                                                                                                                                                    |
| Tabs                                                                          | `CPU temperatures (10)`, `Fans (2)`, `Battery (1)`. With only `cpu_temp` enabled there are no tabs.                                                                                                                                                                                                   |
| CPU tab                                                                       | Legend and table in order Core 0, 4, 8, 12, 16, 20, 24, 25, 26, 27; no package; values in °C; Core 27 clearly hotter; current values update twice per second                                                                                                                                          |
| Fans tab                                                                      | Values and axis in RPM; title `Fans`; table column `Fan`                                                                                                                                                                                                                                              |
| Battery tab                                                                   | Value in %, slowly decreasing                                                                                                                                                                                                                                                                         |
| Window buttons                                                                | 5 min … 24 h change the time axis; the toolbar shows the point width (`500 ms per point` for 5 min, `150 s per point` for 24 h)                                                                                                                                                                       |
| View                                                                          | `Combined` (one chart) and `Per core` / `Per fan` (one small chart per sensor with alarm lines)                                                                                                                                                                                                       |
| Show all / Hide all, checkboxes                                               | Lines appear and disappear on the chart                                                                                                                                                                                                                                                               |
| Alarm form                                                                    | Section title `Alarms: CPU temperatures`; the Sensor dropdown lists only "Every core (one alarm each)" and the 10 cores (no fans, no battery); threshold label in °C; **Fires when** `above (>)`, threshold `75`                                                                                      |
| Select the **Fans** tab, then **Battery**                                     | The Alarms section follows the tab: `Alarms: Fans` with only "Every fan", Fan 1, Fan 2 and `above (>)` / `4500`; `Alarms: Battery` with only "Every battery", BAT0 charge and `below (<)` / `20`, hint `Fires when avg(value, last 10 min) < 20`                                                      |
| Alarms on several families (e.g. one per tab)                                 | Each tab lists only its own alarms and events; the note reads `N more alarm(s) on other sensor families; select their tab above to see them`; every tab with a firing alarm shows an orange `N firing` badge                                                                                          |
| On the Fans tab, **Add alarm** with "Every fan"                               | One alarm per fan is created (Fan 1, Fan 2) and nothing for cores or the battery                                                                                                                                                                                                                      |
| Create `min < 99` for the battery, open the Battery tab, **Per battery** view | The alarm fires; the battery chart is outlined in orange with a dashed line labelled `min<99`; the event log shows `< 99` in the Threshold column                                                                                                                                                     |
| Browser tab                                                                   | The tab title is `System Sensors`                                                                                                                                                                                                                                                                     |
| Create `avg > 60` for "Every core"                                            | 10 alarms created; only Core 27 shows `firing` within a second and its row gets a dark orange tint and an orange bar; an orange strip under the header reads `1 alarm firing` … `Core 27 avg(last 10 min) > 60`, on every tab                                                                         |
| Alarm table                                                                   | "Current aggregate" updates every second without reloading                                                                                                                                                                                                                                            |
| Disable / Delete buttons                                                      | State becomes `disabled` / row disappears                                                                                                                                                                                                                                                             |
| Second browser tab                                                            | An alarm created in one tab appears in the other within a second                                                                                                                                                                                                                                      |
| Stop the server                                                               | The marker turns grey (`Reconnecting…`); the last data stays visible. Start it again: the marker turns orange (`Live`) and the gap is filled                                                                                                                                                          |

Dashboard development with hot reload: run the server on port 8080, then `cd frontend && npm run dev` and open http://localhost:5173 (API calls and the stream are forwarded to 8080).

### 4.16 Performance and stability

With the server from 4.4 running, open several live streams and watch the process for a minute:

```bash
PID=$(pgrep -x sensor-server)
for i in $(seq 10); do (timeout 70 curl -sN $API/stream > /dev/null &); done
cpu() { awk '{print $14+$15}' /proc/$PID/stat; }
c0=$(cpu); grep VmRSS /proc/$PID/status; ls /proc/$PID/task | wc -l
sleep 60
c1=$(cpu); grep VmRSS /proc/$PID/status; ls /proc/$PID/task | wc -l
echo "CPU seconds in 60 s: $(echo "($c1-$c0)/$(getconf CLK_TCK)" | bc -l | cut -c1-5)"
curl -s $API/stats | python3 -c "import json,sys; d=json.load(sys.stdin); print('live clients', d['live_clients']); print('dropped', {s['name']: s['dropped'] for s in d['bus']['subscribers']}); print('max tick us', d['sampler']['max_tick_us'], '| max flush ms', d['db_writer']['max_flush_ms'], '| alarm eval us', d['alarm_engine']['last_eval_us'])"
```

**Expected:** memory (`VmRSS`, about 12 MB) and the thread count do not grow, CPU stays under about 1 % of one core (under 0.6 CPU seconds per minute), `live clients 10`, every `dropped` is 0, ticks take well under a millisecond on average and flushes a few milliseconds at most.

### 4.17 Real hardware load test

On the laptop, with `scripts/run_all.sh` running and the dashboard open:

```bash
sudo apt install stress-ng
stress-ng --cpu 0 --timeout 120s       # load every core for 2 minutes
```

Before starting the load, create an alarm `max > 80` for "Every core" with a 1-minute window (pick a threshold just above your idle temperatures).

**Expected:** all core lines climb within seconds, alarms on the hot cores switch to `firing` and log events, and they clear about 1 minute after the load stops.

## 5. Code documentation (Doxygen)

Every source file carries Doxygen comments; the `Doxyfile` at the project root generates HTML documentation from them.

```bash
doxygen --version          # 1.9 or newer (sudo apt install doxygen graphviz)
doxygen Doxyfile           # from the project root
cat docs/doxygen-warnings.log
xdg-open docs/html/index.html
```

**Expected**

- `doxygen Doxyfile` finishes in a few seconds and creates `docs/html/index.html` (about 350 HTML pages).
- `docs/doxygen-warnings.log` holds at most one line, `README.md: … Unexpected subsubsection command found inside section!`, which only concerns a heading level in README and is harmless. There are no warnings about the code.
- The main page shows README.md. **Related Pages** lists `Project-Overview.md` and `TESTING.md`. **Namespaces** lists `sd::bus`, `sd::sensors`, `sd::db`, `sd::alarm`, `sd::pipeline`, `sd::server` and more; **Files** lists every file of `src/`, `tests/` and `frontend/src/`, each with a description.
- Class pages (for example `sd::bus::Bus`, `sd::db::Database`) describe every member and show collaboration diagrams (with Graphviz installed).

**Check that nothing in the code is undocumented** (doxygen then warns about every undocumented item):

```bash
(cat Doxyfile; echo "EXTRACT_ALL=NO") | doxygen -
cat docs/doxygen-warnings.log
```

**Expected:** the same single README line, no warnings for `src/`, `tests/` or `frontend/src/`.



## 6. Clean up

Stop the processes in terminals A, B and C with Ctrl+C (and any test broker with `kill %1`), then:

```bash
rm -rf /tmp/sd-test
scripts/clean.sh           # optional: also remove build/, docs/, data/sensors.db*, frontend/node_modules
```

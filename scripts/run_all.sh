#!/usr/bin/env bash
# Checks the build environment (scripts/setup_env.sh: prints the installed
# packages with their versions and installs anything missing), builds
# everything (incrementally) and starts the dashboard server, which
# samples the sensors itself (one thread per sensor family, every 500 ms),
# stores them, evaluates alarms and streams live data to the browser.
# Ctrl+C stops it cleanly (buffered readings are written first).
#
#   scripts/run_all.sh            # real sensors of this machine
#   scripts/run_all.sh --fake     # simulated sensors (VMs/containers without hardware sensors)
#
# Env: PORT (default 8080), SENSOR_DB (default data/sensors.db),
#      POLL_INTERVAL_MS (default 500),
#      SENSOR_PROVIDERS (default all; e.g. "cpu_temp" or "cpu_temp,fan"),
#      MQTT_URL (optional, e.g. localhost:1883 to publish to an MQTT broker; also makes
#                the environment check install the MQTT packages),
#      SETUP_OPTS (extra options for scripts/setup_env.sh, e.g. "--with-docs"),
#      SKIP_SETUP=1 (skip the environment check, e.g. offline or without sudo)
set -euo pipefail
cd "$(dirname "$0")/.."

PORT="${PORT:-8080}"
DB="${SENSOR_DB:-data/sensors.db}"
INTERVAL_MS="${POLL_INTERVAL_MS:-500}"
PROVIDERS="${SENSOR_PROVIDERS:-all}"
ROOTS=()
EXTRA=()
pids=()

cleanup() { kill "${pids[@]}" 2>/dev/null || true; wait 2>/dev/null || true; }
trap cleanup EXIT INT TERM

# Environment check on every run: lists the required packages with their
# versions and installs any that are missing (needs sudo for that).
if [[ "${SKIP_SETUP:-0}" != 1 ]]; then
  SETUP_ARGS=()
  if [[ -n "${MQTT_URL:-}" ]]; then SETUP_ARGS+=(--with-mqtt); fi
  # shellcheck disable=SC2206  # SETUP_OPTS is a space-separated option list
  if [[ -n "${SETUP_OPTS:-}" ]]; then SETUP_ARGS+=(${SETUP_OPTS}); fi
  if ! bash scripts/setup_env.sh ${SETUP_ARGS[@]+"${SETUP_ARGS[@]}"}; then
    echo "==> environment setup failed: fix the packages listed above, or run with SKIP_SETUP=1" >&2
    exit 1
  fi
fi

if [[ "${1:-}" == "--fake" ]]; then
  FAKE=/tmp/fake-sys
  bash scripts/fake_hwmon.sh "$FAKE" >/dev/null &
  pids+=($!)
  ROOTS=(--hwmon-root "$FAKE/hwmon" --power-supply-root "$FAKE/power_supply" --thermal-root "$FAKE/thermal")
  PROVIDERS="${SENSOR_PROVIDERS:-all}"
  sleep 1.5
fi
if [[ -n "${MQTT_URL:-}" ]]; then EXTRA+=(--mqtt "$MQTT_URL"); fi

# Always run the (incremental) build so source changes are never left out.
echo "==> building C++ components"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc 2>/dev/null || echo 2)"

# Rebuild when the dashboard sources, its dependencies (package*.json) or the
# build configuration changed since the last build.
if [[ ! -f frontend/dist/index.html || -n "$(find frontend/src frontend/index.html frontend/package.json frontend/package-lock.json frontend/vite.config.js -newer frontend/dist/index.html 2>/dev/null)" ]]; then
  if command -v npm >/dev/null; then
    echo "==> building frontend"
    (cd frontend && npm ci --no-audit --no-fund && npm run build)
  else
    echo "==> npm not found; serving the prebuilt frontend/dist"
  fi
fi

mkdir -p "$(dirname "$DB")"
echo "==> sensors found (providers: $PROVIDERS):"
build/sensor-poller --providers "$PROVIDERS" ${ROOTS[@]+"${ROOTS[@]}"} --list

build/sensor-server --db "$DB" --port "$PORT" --static frontend/dist --providers "$PROVIDERS" \
  --interval-ms "$INTERVAL_MS" ${ROOTS[@]+"${ROOTS[@]}"} ${EXTRA[@]+"${EXTRA[@]}"} &
pids+=($!)

echo "==> dashboard: http://localhost:$PORT   (Ctrl+C to stop)"
wait -n "${pids[@]}"

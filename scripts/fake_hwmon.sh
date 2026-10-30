#!/usr/bin/env bash
# Simulated sysfs tree for development on machines without real sensors
# (VMs, containers, CI). It mirrors a 10-core Intel laptop:
#   hwmon/        coretemp with Package id 0 (ignored by the poller) and
#                 Cores 0,4,8,12,16,20,24,25,26,27 (core 27 runs hot so
#                 per-core alarms can be seen), plus a thinkpad fan chip
#   power_supply/ one battery (BAT0) slowly discharging, and AC
# Values keep changing (slow sine wave plus noise).
#
# Usage:  scripts/fake_hwmon.sh [ROOT]        (default ROOT=/tmp/fake-sys)
# Then:   ./build/sensor-poller --providers all --hwmon-root ROOT/hwmon \
#             --power-supply-root ROOT/power_supply --thermal-root ROOT/thermal
set -euo pipefail

ROOT="${1:-/tmp/fake-sys}"
HW="$ROOT/hwmon"
PS="$ROOT/power_supply"
CORES=(0 4 8 12 16 20 24 25 26 27)

mkdir -p "$HW/hwmon0" "$HW/hwmon3" "$HW/hwmon5" "$PS/BAT0" "$PS/AC" "$ROOT/thermal"
echo acpitz > "$HW/hwmon0/name"          # not a CPU driver: ignored by cpu_temp
echo 54000  > "$HW/hwmon0/temp1_input"
echo coretemp > "$HW/hwmon3/name"
echo "Package id 0" > "$HW/hwmon3/temp1_label"
i=2
for c in "${CORES[@]}"; do
  echo "Core $c" > "$HW/hwmon3/temp${i}_label"
  i=$((i + 1))
done
echo thinkpad > "$HW/hwmon5/name"
echo Mains > "$PS/AC/type"
echo Battery > "$PS/BAT0/type"
echo System > "$PS/BAT0/scope"

# Atomic write so the poller never reads a half-written file.
put() { echo "$2" > "$1.tmp" && mv "$1.tmp" "$1"; }

echo "fake sysfs at $ROOT (Ctrl+C to stop)"
t=0
while true; do
  wave=$(awk -v t="$t" 'BEGIN { printf "%d", 6000 * sin(t / 12.0) }')
  i=2
  max=0
  for c in "${CORES[@]}"; do
    base=$((40000 + (c % 8) * 600))
    [[ $c -eq 27 ]] && base=72000
    v=$((base + wave + RANDOM % 1500))
    put "$HW/hwmon3/temp${i}_input" "$v"
    (( v > max )) && max=$v
    i=$((i + 1))
  done
  put "$HW/hwmon3/temp1_input" $((max + 2000))
  put "$HW/hwmon5/fan1_input" $((2600 + wave / 10 + RANDOM % 120))
  put "$HW/hwmon5/fan2_input" $((1900 + wave / 12 + RANDOM % 90))
  put "$PS/BAT0/capacity" $((95 - (t / 30) % 80))
  t=$((t + 1))
  sleep 1
done

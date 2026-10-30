#!/usr/bin/env bash
# Environment setup for the Multi-Sensor Dashboard.
#
# Checks every tool and library the project needs, prints what is installed
# (with versions), and installs whatever is missing through the system package
# manager (apt, dnf, pacman or zypper). scripts/run_all.sh runs it before every
# build; it can also be run on its own.
#
#   scripts/setup_env.sh              # check, install missing requirements
#   scripts/setup_env.sh --check      # check only; exit 1 if something is missing
#   scripts/setup_env.sh --with-mqtt  # also the MQTT bridge (libmosquitto, broker, clients)
#   scripts/setup_env.sh --with-docs  # also Doxygen and Graphviz (code documentation)
#   scripts/setup_env.sh --all        # required + MQTT + docs
#
# Required:  C++17 compiler (GCC >= 8 or Clang >= 7), make, CMake >= 3.16,
#            SQLite headers >= 3.8.2, zlib headers, Node.js >= 18, npm
# Optional:  libmosquitto (MQTT bridge), Doxygen >= 1.9 + Graphviz (docs)
#
# Installing needs root: the script uses sudo when not run as root.
# Exit status: 0 = all required packages present, 1 = something required is
# still missing (or --check found something missing), 2 = bad arguments.
set -uo pipefail

CHECK_ONLY=0
WANT_MQTT=0
WANT_DOCS=0
for arg in "$@"; do
  case "$arg" in
    --check) CHECK_ONLY=1 ;;
    --with-mqtt) WANT_MQTT=1 ;;
    --with-docs) WANT_DOCS=1 ;;
    --all) WANT_MQTT=1; WANT_DOCS=1 ;;
    -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "setup_env: unknown option $arg (see --help)" >&2; exit 2 ;;
  esac
done

# ---------------------------------------------------------------------------
# Package manager and package names
# ---------------------------------------------------------------------------
PM=""
for p in apt-get dnf pacman zypper; do
  if command -v "$p" >/dev/null 2>&1; then PM="$p"; break; fi
done

# pkg_names <component> -> distribution package(s) providing it
pkg_names() {
  case "$PM:$1" in
    apt-get:compiler) echo "g++" ;;          apt-get:make) echo "make" ;;
    apt-get:cmake) echo "cmake" ;;           apt-get:sqlite) echo "libsqlite3-dev" ;;
    apt-get:zlib) echo "zlib1g-dev" ;;       apt-get:node) echo "nodejs" ;;
    apt-get:npm) echo "npm" ;;               apt-get:mqtt) echo "libmosquitto-dev mosquitto mosquitto-clients" ;;
    apt-get:doxygen) echo "doxygen" ;;       apt-get:dot) echo "graphviz" ;;
    dnf:compiler) echo "gcc-c++" ;;          dnf:make) echo "make" ;;
    dnf:cmake) echo "cmake" ;;               dnf:sqlite) echo "sqlite-devel" ;;
    dnf:zlib) echo "zlib-devel" ;;           dnf:node) echo "nodejs" ;;
    dnf:npm) echo "npm" ;;                   dnf:mqtt) echo "mosquitto-devel mosquitto" ;;
    dnf:doxygen) echo "doxygen" ;;           dnf:dot) echo "graphviz" ;;
    pacman:compiler) echo "gcc" ;;           pacman:make) echo "make" ;;
    pacman:cmake) echo "cmake" ;;            pacman:sqlite) echo "sqlite" ;;
    pacman:zlib) echo "zlib" ;;              pacman:node) echo "nodejs" ;;
    pacman:npm) echo "npm" ;;                pacman:mqtt) echo "mosquitto" ;;
    pacman:doxygen) echo "doxygen" ;;        pacman:dot) echo "graphviz" ;;
    zypper:compiler) echo "gcc-c++" ;;       zypper:make) echo "make" ;;
    zypper:cmake) echo "cmake" ;;            zypper:sqlite) echo "sqlite3-devel" ;;
    zypper:zlib) echo "zlib-devel" ;;        zypper:node) echo "nodejs" ;;
    zypper:npm) echo "npm" ;;                zypper:mqtt) echo "mosquitto-devel mosquitto" ;;
    zypper:doxygen) echo "doxygen" ;;        zypper:dot) echo "graphviz" ;;
  esac
}

# ---------------------------------------------------------------------------
# Version detection (fast: no package-manager queries, so it can run on
# every build). Each prints the installed version, or nothing if missing.
# ---------------------------------------------------------------------------
CXX_BIN="${CXX:-c++}"
command -v "$CXX_BIN" >/dev/null 2>&1 || CXX_BIN=g++

compiler_kind() { "$CXX_BIN" --version 2>/dev/null | head -1 | grep -qi clang && echo clang || echo gcc; }
ver_compiler() {
  command -v "$CXX_BIN" >/dev/null 2>&1 || return 0
  "$CXX_BIN" -dumpfullversion -dumpversion 2>/dev/null | head -1
}
ver_make()  { command -v make  >/dev/null 2>&1 && make --version 2>/dev/null | head -1 | grep -oE '[0-9]+(\.[0-9]+)+' | head -1; }
ver_cmake() { command -v cmake >/dev/null 2>&1 && cmake --version 2>/dev/null | head -1 | grep -oE '[0-9]+(\.[0-9]+)+' | head -1; }
ver_node()  { command -v node  >/dev/null 2>&1 && node --version 2>/dev/null | sed 's/^v//'; }
ver_npm()   { command -v npm   >/dev/null 2>&1 && npm --version 2>/dev/null | head -1; }
ver_doxygen() { command -v doxygen >/dev/null 2>&1 && doxygen --version 2>/dev/null | grep -oE '^[0-9]+(\.[0-9]+)+'; }
ver_dot()   { command -v dot   >/dev/null 2>&1 && dot -V 2>&1 | grep -oE '[0-9]+(\.[0-9]+)+' | head -1; }

# Header versions are read through the preprocessor, which is exactly what the
# build sees (and also proves the development package is installed).
header_macro() {  # header_macro <header> <expression>
  command -v "$CXX_BIN" >/dev/null 2>&1 || return 0
  printf '#include <%s>\n%s\n' "$1" "$2" | "$CXX_BIN" -E -P -x c++ - 2>/dev/null | tail -1 | tr -d '" '
}
ver_sqlite() { header_macro sqlite3.h SQLITE_VERSION | grep -E '^[0-9]'; }
ver_zlib()   { header_macro zlib.h ZLIB_VERSION | grep -E '^[0-9]'; }
ver_mqtt()   { header_macro mosquitto.h 'LIBMOSQUITTO_MAJOR.LIBMOSQUITTO_MINOR.LIBMOSQUITTO_REVISION' | grep -E '^[0-9]'; }

# ver_ge <installed> <minimum>: true if installed >= minimum
ver_ge() { [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -1)" = "$2" ]; }

# ---------------------------------------------------------------------------
# Requirements: id | label | minimum | required (1) or optional group
# ---------------------------------------------------------------------------
min_compiler() { [ "$(compiler_kind)" = clang ] && echo 7 || echo 8; }
REQS=(
  "compiler|C++17 compiler|COMPILER|1"
  "make|make|3.81|1"
  "cmake|CMake|3.16|1"
  "sqlite|SQLite headers|3.8.2|1"
  "zlib|zlib headers|1.2|1"
  "node|Node.js|18|1"
  "npm|npm|8|1"
  "mqtt|libmosquitto (MQTT)|1.4|mqtt"
  "doxygen|Doxygen|1.9|docs"
  "dot|Graphviz (dot)|2.0|docs"
)

label_of() {
  if [ "$1" = compiler ]; then
    command -v "$CXX_BIN" >/dev/null 2>&1 && echo "C++17 compiler ($(compiler_kind))" || echo "C++17 compiler"
  else
    for r in "${REQS[@]}"; do IFS='|' read -r id label _ _ <<<"$r"; [ "$id" = "$1" ] && echo "$label"; done
  fi
}

MISSING=()          # components that are wanted but missing or too old
TOO_OLD=()          # subset: installed but below the minimum
report() {          # report <title>: prints the table and fills MISSING / TOO_OLD
  MISSING=(); TOO_OLD=()
  echo "==> $1"
  printf '    %-26s %-10s %-12s %s\n' "Package" "Minimum" "Installed" "Status"
  for r in "${REQS[@]}"; do
    IFS='|' read -r id _ min group <<<"$r"
    [ "$min" = COMPILER ] && min=$(min_compiler)
    local wanted=0
    [ "$group" = 1 ] && wanted=1
    [ "$group" = mqtt ] && [ "$WANT_MQTT" = 1 ] && wanted=1
    [ "$group" = docs ] && [ "$WANT_DOCS" = 1 ] && wanted=1
    local v status
    v=$("ver_$id")
    if [ -z "$v" ]; then
      if [ "$wanted" = 1 ]; then status="MISSING"; MISSING+=("$id")
      else
        case "$group" in
          mqtt) status="not installed (optional: --with-mqtt)" ;;
          docs) status="not installed (optional: --with-docs)" ;;
        esac
      fi
      v="-"
    elif ver_ge "$v" "$min"; then
      status="ok"
      [ "$wanted" = 0 ] && status="ok (optional)"
    else
      if [ "$wanted" = 1 ]; then status="TOO OLD"; MISSING+=("$id"); TOO_OLD+=("$id")
      else status="too old (optional)"; fi
    fi
    printf '    %-26s %-10s %-12s %s\n' "$(label_of "$id")" ">= $min" "$v" "$status"
  done
}

# ---------------------------------------------------------------------------
# Check, install what is missing, check again
# ---------------------------------------------------------------------------
report "build environment (package manager: ${PM:-none found})"
if [ ${#MISSING[@]} -eq 0 ]; then
  echo "==> all required packages are installed"
  exit 0
fi

if [ "$CHECK_ONLY" = 1 ]; then
  echo "==> missing or too old: ${MISSING[*]} (run scripts/setup_env.sh to install)" >&2
  exit 1
fi
if [ -z "$PM" ]; then
  echo "==> no supported package manager (apt, dnf, pacman, zypper); install manually: ${MISSING[*]}" >&2
  exit 1
fi

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
  if command -v sudo >/dev/null 2>&1; then SUDO="sudo"
  else echo "==> not root and sudo is not available; install manually: ${MISSING[*]}" >&2; exit 1; fi
fi

PKGS=()
for id in "${MISSING[@]}"; do
  for p in $(pkg_names "$id"); do
    [[ " ${PKGS[*]-} " == *" $p "* ]] || PKGS+=("$p")
  done
done
echo "==> installing: ${PKGS[*]}"
case "$PM" in
  apt-get) $SUDO env DEBIAN_FRONTEND=noninteractive apt-get update -qq &&
           $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "${PKGS[@]}" ;;
  dnf)     $SUDO dnf install -y "${PKGS[@]}" ;;
  pacman)  $SUDO pacman -S --needed --noconfirm "${PKGS[@]}" ;;
  zypper)  $SUDO zypper --non-interactive install "${PKGS[@]}" ;;
esac || echo "==> the package manager reported an error (see above)" >&2

report "build environment after installing"
if [ ${#MISSING[@]} -eq 0 ]; then
  echo "==> all required packages are installed"
  exit 0
fi
echo "==> still missing or too old: ${MISSING[*]}" >&2
for id in "${TOO_OLD[@]}"; do
  case "$id" in
    node|npm) echo "    Node.js from this distribution is older than 18. Install a current LTS, e.g.:" >&2
              echo "      curl -fsSL https://deb.nodesource.com/setup_22.x | sudo -E bash - && sudo apt-get install -y nodejs   (Debian/Ubuntu)" >&2
              echo "      or use nvm: https://github.com/nvm-sh/nvm" >&2 ;;
    cmake)    echo "    CMake is older than 3.16: pip install cmake, or use Kitware's APT repository." >&2 ;;
    compiler) echo "    The C++ compiler is too old for C++17: install g++ 8+ or clang 7+ and set CXX." >&2 ;;
  esac
done
exit 1

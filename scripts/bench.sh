#!/usr/bin/env bash
# Build a portable release, run bench_book, and save the results together with
# the machine, OS, compiler, flags and git commit that produced them.
#
#   ./scripts/bench.sh                 # default: 1,000,000 operations per type
#   ./scripts/bench.sh --ops 5000000   # extra arguments go to bench_book
#
# Writes bench/results/<date>-<os>-<machine>.json and .md. Works on macOS and
# Linux (bash 3.2 compatible). For stable numbers: plug in the power adapter,
# close other apps, and run it two or three times.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

cmake --preset release -DRTES_NATIVE=OFF >/dev/null
cmake --build --preset release -j --target bench_book >/dev/null

OS="$(uname -s)"
DATE="$(date -u +%Y-%m-%dT%H%M%SZ)"
GIT_SHA="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
GIT_DIRTY="$( [ -n "$(git status --porcelain -- src include tools CMakeLists.txt 2>/dev/null)" ] && echo true || echo false)"
CACHE=build/release/CMakeCache.txt
cache_get() { grep "^$1:" "$CACHE" | head -1 | cut -d= -f2-; }
COMPILER_PATH="$(cache_get CMAKE_CXX_COMPILER)"
COMPILER="$("$COMPILER_PATH" --version 2>/dev/null | head -1)"
BUILD_TYPE="$(cache_get CMAKE_BUILD_TYPE)"
FLAGS="$(cache_get CMAKE_CXX_FLAGS_RELEASE) $(cache_get CMAKE_CXX_FLAGS)"
NATIVE="$(cache_get RTES_NATIVE)"

if [ "$OS" = "Darwin" ]; then
  MACHINE="$(sysctl -n hw.model)"
  CPU="$(sysctl -n machdep.cpu.brand_string)"
  CORES="$(sysctl -n hw.ncpu) logical ($(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo ?) performance cores)"
  MEM="$(( $(sysctl -n hw.memsize) / 1073741824 )) GB"
  OSVER="macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
  POWER="$(pmset -g batt 2>/dev/null | head -1 | sed "s/Now drawing from //; s/'//g")"
  GOVERNOR="n/a (macOS)"
else
  MACHINE="$(cat /sys/devices/virtual/dmi/id/product_name 2>/dev/null || echo linux)"
  CPU="$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
  CORES="$(nproc) logical"
  MEM="$(( $(grep MemTotal /proc/meminfo | awk '{print $2}') / 1048576 )) GB"
  OSVER="$( (. /etc/os-release && echo "$PRETTY_NAME") 2>/dev/null || echo Linux), kernel $(uname -r)"
  POWER="n/a"
  GOVERNOR="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
fi
PINNING="none (threads are not pinned; bench_book is single-threaded)"

SLUG="$(printf '%s-%s' "$OS" "$MACHINE" | tr 'A-Z' 'a-z' | tr -c 'a-z0-9.-' '-' | sed 's/-*$//')"
OUT_DIR=bench/results
mkdir -p "$OUT_DIR"
BASE="$OUT_DIR/$DATE-$SLUG"
RAW="$(mktemp)"

echo "Running bench_book on $CPU ($OSVER)..."
./build/release/bench_book --json "$RAW" "$@" | tee "$BASE.txt"

json_str() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }
{
  printf '{\n'
  printf '  "date_utc": "%s",\n' "$DATE"
  printf '  "git_commit": "%s",\n  "git_dirty": %s,\n' "$GIT_SHA" "$GIT_DIRTY"
  printf '  "machine": {\n'
  printf '    "model": "%s",\n    "cpu": "%s",\n    "cores": "%s",\n    "memory": "%s",\n' \
         "$(json_str "$MACHINE")" "$(json_str "$CPU")" "$(json_str "$CORES")" "$(json_str "$MEM")"
  printf '    "os": "%s",\n    "power": "%s",\n    "cpu_governor": "%s",\n    "thread_pinning": "%s"\n  },\n' \
         "$(json_str "$OSVER")" "$(json_str "$POWER")" "$(json_str "$GOVERNOR")" "$(json_str "$PINNING")"
  printf '  "build": {\n    "compiler": "%s",\n    "build_type": "%s",\n    "flags": "%s",\n    "rtes_native": "%s"\n  },\n' \
         "$(json_str "$COMPILER")" "$(json_str "$BUILD_TYPE")" "$(json_str "$FLAGS")" "$(json_str "$NATIVE")"
  printf '  "command": "./scripts/bench.sh %s",\n' "$(json_str "$*")"
  printf '  "bench_book": '
  sed 's/^/  /' "$RAW" | sed '1s/^  //'
  printf '}\n'
} > "$BASE.json"
rm -f "$RAW"

{
  echo "# bench_book results: $DATE"
  echo
  echo "Order book operations only: single thread, in process. No network, gateway, risk or"
  echo "inter-thread queues: this is not wire-to-wire latency."
  echo
  echo "| | |"
  echo "|---|---|"
  echo "| Machine | $MACHINE |"
  echo "| CPU | $CPU, $CORES |"
  echo "| Memory | $MEM |"
  echo "| OS | $OSVER |"
  echo "| Power | $POWER |"
  echo "| CPU governor | $GOVERNOR |"
  echo "| Thread pinning | $PINNING |"
  echo "| Compiler | $COMPILER |"
  echo "| Build | $BUILD_TYPE, flags \`$FLAGS\`, RTES_NATIVE=$NATIVE |"
  echo "| Commit | \`$GIT_SHA\` (uncommitted source changes: $GIT_DIRTY) |"
  echo "| Command | \`./scripts/bench.sh $*\` |"
  echo
  echo '```'
  cat "$BASE.txt"
  echo '```'
} > "$BASE.md"
rm -f "$BASE.txt"

echo
echo "Saved $BASE.json and $BASE.md"
if [ "$GIT_DIRTY" = "true" ]; then
  echo "WARNING: source files have uncommitted changes; commit them before publishing these numbers."
fi

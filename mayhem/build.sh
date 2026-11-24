#!/usr/bin/env bash
#
# mayhem/build.sh — build the flipperzero-wifi-marauder fuzz targets and functional self-test.
#
# Upstream is a Flipper Zero external app (.fap) built with ufbt for ARM against the Flipper SDK —
# not something a host fuzzer can run. Instead the app's own C sources (script parser + cJSON,
# script executor/worker, UART RX demux, console-output RX callbacks, sequential file, evil-portal
# reader) are compiled UNCHANGED for the host against mayhem/shim/, a small stand-in for the SDK
# headers (furi, furi_hal serial, storage, dialogs, gui no-ops). No network, no external deps.
# Outputs go to mayhem/out/ (gitignored by mayhem/.gitignore); re-running rebuilds in place.
set -euo pipefail

[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
: "${SRC:=$(cd "$(dirname "$0")/.." && pwd)}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"
OUT="$SRC/mayhem/out"
mkdir -p "$OUT/fuzz" "$OUT/test"

APP_SRCS=(
  script/cJSON.c
  script/wifi_marauder_script.c
  script/wifi_marauder_script_executor.c
  script/wifi_marauder_script_worker.c
  wifi_marauder_uart.c
  scenes/wifi_marauder_scene_console_output.c
  file/sequential_file.c
  wifi_marauder_ep.c
)
INC=(-I"$SRC/mayhem/shim" -I"$SRC")
# The app is written for the Flipper SDK's GCC; keep its implicit-declaration-free C but don't let
# clang's newer default warnings-as-errors stop the build.
CWARN=(-std=gnu11 -Wno-error -Wno-int-conversion -Wno-incompatible-pointer-types)

compile_set() { # <objdir> <cflags...> : compile APP_SRCS + the shim into <objdir>
  local objdir="$1"; shift
  mkdir -p "$objdir"
  local s
  for s in "${APP_SRCS[@]}"; do
    local extra=()
    # Vendored cJSON (Flipper-modified: its own string_to_double instead of strtod) overflows int in
    # `exponent = exponent * 10 + digit` (cJSON.c:318, string_to_double) on any number literal with
    # a 10+ digit exponent, e.g. `1e99999999999`. Unrelaxed, that one frame was 23 of 47 crashing
    # inputs in a 150 s local run, so it floods out the app's own bugs. It wraps harmlessly on the
    # device's Cortex-M4. Relax only signed-integer-overflow, only for cJSON.c. ASan and every other
    # UBSan check, including float-cast-overflow, stay on and halting.
    [ "$s" = script/cJSON.c ] && extra=(-fno-sanitize=signed-integer-overflow)
    "$CC" "$@" "${extra[@]}" "${CWARN[@]}" "${INC[@]}" -include "$SRC/mayhem/shim/alloc_track.h" -c "$SRC/$s" -o "$objdir/$(echo "$s" | tr / _).o" &
  done
  "$CC" "$@" "${CWARN[@]}" "${INC[@]}" -c "$SRC/mayhem/shim/furi_shim.c" -o "$objdir/furi_shim.o" &
  wait
}

# 1) Fuzz build: the app sources instrumented for coverage + ASan/UBSan (halting), DWARF 3.
# shellcheck disable=SC2086
compile_set "$OUT/fuzz/obj" $SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link -O1
# shellcheck disable=SC2086
"$CXX" $SANITIZER_FLAGS $DEBUG_FLAGS -c "$SRC/mayhem/lsan_off.cc" -o "$OUT/fuzz/lsan_off.o"
# shellcheck disable=SC2086
"$CC" $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$OUT/fuzz/standalone_main.o"

for h in fuzz_script fuzz_uart; do
  # shellcheck disable=SC2086
  "$CC" $SANITIZER_FLAGS $DEBUG_FLAGS "${CWARN[@]}" "${INC[@]}" -c "$SRC/mayhem/$h.c" -o "$OUT/fuzz/$h.o"
  # shellcheck disable=SC2086
  "$CXX" $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE "$OUT/fuzz/$h.o" "$OUT"/fuzz/obj/*.o \
    "$OUT/fuzz/lsan_off.o" -lm -o "$OUT/$h"
  # shellcheck disable=SC2086
  "$CXX" $SANITIZER_FLAGS $DEBUG_FLAGS "$OUT/fuzz/standalone_main.o" "$OUT/fuzz/$h.o" "$OUT"/fuzz/obj/*.o \
    "$OUT/fuzz/lsan_off.o" -lm -o "$OUT/$h-standalone"
done

# 2) Functional self-test (the oracle mayhem/test.sh runs): same app sources, NORMAL flags, no
#    sanitizers, built independently of the fuzz build.
# shellcheck disable=SC2086
compile_set "$OUT/test/obj" -O1 -g $COVERAGE_FLAGS
# shellcheck disable=SC2086
"$CC" -O1 -g $COVERAGE_FLAGS "${CWARN[@]}" "${INC[@]}" "$SRC/mayhem/tests/selftest.c" "$OUT"/test/obj/*.o \
  -lm -o "$OUT/marauder_selftest"

echo "built: $(ls "$OUT"/fuzz_* "$OUT"/marauder_selftest | tr '\n' ' ')"

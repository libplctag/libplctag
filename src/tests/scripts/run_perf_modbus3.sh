#!/usr/bin/env bash
#
# run_perf_modbus.sh - Drive the perf_benchmark program against a local Modbus
# simulator across the full parameter matrix and produce a CSV results file.
#
# Usage:
#   run_perf_modbus.sh [options]
#
#   --executable-dir=DIR  directory containing perf_benchmark and modbus_server3 binaries
#   --results-file=FILE   output CSV path (default: perf_modbus_results_<timestamp>.csv)
#   --output-dir=DIR      directory for results file (default: current directory)
#   --duration=SECS       seconds per test case (default: 10)
#
# Tags drive the matrix:
#   - threads iterate through standard steps, clamped at the tag count
#   - one connection group is always used
#   - each thread owns an exclusive slice of tags (no sharing, no locking)
#
# Example:
#   ./run_perf_modbus.sh --executable-dir=../../build/bin_dist
#   ./run_perf_modbus.sh --executable-dir=../../build/bin_dist --duration=5
#

set -euo pipefail

#--- Configuration ---

STEPS=(1 50 100 500 1000)
TAG_COUNTS=(1 50 100 500 1000)
MODES=(sync async)

MODBUS_SERVER_PORT=1502
MODBUS_SERVER_PID=""

#--- Parse arguments ---

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
EXECUTABLE_DIR=""
RESULTS_FILE=""
OUTPUT_DIR=""
DURATION="10"

for arg in "$@"; do
    case "$arg" in
        --duration=*)       DURATION="${arg#--duration=}" ;;
        --output-dir=*)     OUTPUT_DIR="${arg#--output-dir=}" ;;
        --results-file=*)   RESULTS_FILE="${arg#--results-file=}" ;;
        --executable-dir=*) EXECUTABLE_DIR="${arg#--executable-dir=}" ;;
        --*)                echo "ERROR: Unknown flag: $arg" >&2; exit 1 ;;
        *)                  echo "ERROR: Unknown argument: $arg" >&2; exit 1 ;;
    esac
done


if [ -z "$EXECUTABLE_DIR" ]; then
    echo "ERROR: --executable-dir is required." >&2
    exit 1
fi

# Expand a leading ~ since tilde expansion doesn't happen inside quoted strings.
EXECUTABLE_DIR="${EXECUTABLE_DIR/#\~/$HOME}"


if ! [[ "$DURATION" =~ ^[0-9]+$ ]] || [ "$DURATION" -le 0 ]; then
    echo "ERROR: --duration must be a positive integer, got: '$DURATION'" >&2
    exit 1
fi

# Determine results file path.
if [ -z "$RESULTS_FILE" ]; then
    if [ -n "$OUTPUT_DIR" ]; then
        mkdir -p "$OUTPUT_DIR"
        RESULTS_FILE="${OUTPUT_DIR}/perf_modbus_results_${TIMESTAMP}.csv"
    else
        RESULTS_FILE="perf_modbus_results_${TIMESTAMP}.csv"
    fi
fi

PERF_BENCHMARK="${EXECUTABLE_DIR}/perf_benchmark"
MODBUS_SERVER="${EXECUTABLE_DIR}/modbus_server3"

# Verify required binaries exist.
for bin in "$PERF_BENCHMARK" "$MODBUS_SERVER"; do
    if [ ! -x "$bin" ]; then
        echo "ERROR: $bin not found or not executable." >&2
        exit 1
    fi
done

#--- Helper functions ---

# Cross-platform process killing function (copied from run_simulator_tests.sh)
kill_process() {
    local process_name=$1
    if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "win32" ]]; then
        taskkill //F //IM "${process_name}.exe" > /dev/null 2>&1 || true
    else
        pkill -TERM "$process_name" > /dev/null 2>&1 || true
    fi
}

kill_modbus_server() {
    # Kill any running modbus_server3 instances (including ones not started by this script).
    kill_process modbus_server3

    if [ -n "$MODBUS_SERVER_PID" ]; then
        wait "$MODBUS_SERVER_PID" 2>/dev/null || true
        MODBUS_SERVER_PID=""
    fi
}

cleanup() {
    kill_modbus_server
}
trap cleanup EXIT INT TERM

start_modbus_server() {
    kill_modbus_server

    echo "Starting modbus_server3 on port $MODBUS_SERVER_PORT..." >&2
    "$MODBUS_SERVER" \
        --listen=127.0.0.1:"$MODBUS_SERVER_PORT" \
        &
    MODBUS_SERVER_PID=$!

    # Give it time to bind the port.
    sleep 2

    if ! kill -0 "$MODBUS_SERVER_PID" 2>/dev/null; then
        echo "ERROR: modbus_server3 failed to start." >&2
        exit 1
    fi
    echo "modbus_server3 running (PID $MODBUS_SERVER_PID)." >&2
}

#--- Capture metadata ---

GIT_HASH=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
GIT_DESCRIBE=$(git describe --tags --always --dirty 2>/dev/null || echo "unknown")
PLATFORM=$(uname -s -m)
HOSTNAME_SHORT=$(hostname -s 2>/dev/null || hostname)

BASE_TAG_PATH="protocol=modbus-tcp&gateway=127.0.0.1:${MODBUS_SERVER_PORT}&path=1&name=hr0"

echo "# perf_benchmark (Modbus) results" >&2
echo "#   version:  $GIT_DESCRIBE ($GIT_HASH)" >&2
echo "#   platform: $PLATFORM" >&2
echo "#   host:     $HOSTNAME_SHORT" >&2
echo "#   date:     $(date -Iseconds)" >&2
echo "#   duration: ${DURATION}s per test" >&2
echo "#   tag:      $BASE_TAG_PATH" >&2

#--- Write CSV header ---

{
    echo "# version=$GIT_DESCRIBE commit=$GIT_HASH platform=\"$PLATFORM\" host=$HOSTNAME_SHORT date=$(date -Iseconds) duration_per_test=${DURATION}s tag=\"$BASE_TAG_PATH\""
    echo "mode,connection_groups,threads,tags,tags_per_thread,duration_ms,total_reads,reads_per_sec,cpu_load_pct,fairness_cv,fairness_min_max_ratio"
} > "$RESULTS_FILE"

#--- Start simulator ---

start_modbus_server

#--- Run the matrix ---

total_combos=0
skipped=0
failed=0
completed=0

# Count valid combinations first.
for tags in "${TAG_COUNTS[@]}"; do
    for threads in "${STEPS[@]}"; do
        [ "$threads" -gt "$tags" ] && continue
        for mode in "${MODES[@]}"; do
            total_combos=$((total_combos + 1))
        done
    done
done

echo "" >&2
echo "Running $total_combos test configurations (${DURATION}s each)..." >&2
echo "" >&2

run_index=0

for tags in "${TAG_COUNTS[@]}"; do
    for threads in "${STEPS[@]}"; do
        [ "$threads" -gt "$tags" ] && { skipped=$((skipped + 1)); continue; }
        for mode in "${MODES[@]}"; do
            run_index=$((run_index + 1))
            echo -n "[$run_index/$total_combos] mode=$mode tags=$tags threads=$threads ..." >&2

            # Use if/else so set -e doesn't exit on a failed test run.
            if csv_line=$("$PERF_BENCHMARK" \
                --mode="$mode" \
                --groups=1 \
                --threads="$threads" \
                --tags="$tags" \
                --duration="$DURATION" \
                --tag="$BASE_TAG_PATH" \
                2>/dev/null) && [ -n "$csv_line" ]; then
                echo "$csv_line" >> "$RESULTS_FILE"
                rps=$(echo "$csv_line" | cut -d',' -f8)
                echo "  -> reads/sec=$rps" >&2
                completed=$((completed + 1))
            else
                echo "  -> FAILED" >&2
                failed=$((failed + 1))

                # Restart modbus_server3 in case it crashed.
                if [ -n "$MODBUS_SERVER_PID" ] && ! kill -0 "$MODBUS_SERVER_PID" 2>/dev/null; then
                    echo "  -> modbus_server3 died, restarting..." >&2
                    start_modbus_server
                fi
            fi
        done
    done
done

#--- Summary ---

echo "" >&2
echo "========================================" >&2
echo "Benchmark complete." >&2
echo "  Completed: $completed" >&2
echo "  Failed:    $failed" >&2
echo "  Skipped:   $skipped (threads exceeded tag count)" >&2
echo "  Results:   $RESULTS_FILE" >&2
echo "========================================" >&2

exit $failed

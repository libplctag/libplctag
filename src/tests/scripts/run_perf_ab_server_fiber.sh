#!/usr/bin/env bash
#
# run_perf_logix.sh - Drive the perf_benchmark program against a ControlLogix
# target across the full parameter matrix and produce a CSV results file.
#
# Usage:
#   run_perf_logix.sh [options]
#
#   --executable-dir=DIR  directory containing perf_benchmark and ab_server_fiber binaries
#   --results-file=FILE   output CSV path (default: perf_logix_results_<timestamp>.csv)
#   --output-dir=DIR      directory for results file (default: current directory)
#   --duration=SECS       seconds per test case (default: 10)
#   --tag=BASE_TAG_PATH   run against real hardware instead of the local simulator
#
# Tags drive the matrix:
#   - threads iterate through standard steps, clamped at the tag count
#   - one connection group is always used
#   - each thread owns an exclusive slice of tags (no sharing, no locking)
#
# Example (simulator):
#   ./run_perf_logix.sh --executable-dir=../../build/bin_dist
# Example (hardware):
#   ./run_perf_logix.sh --executable-dir=../../build/bin_dist \
#       --tag="protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&name=TestBigArray[0]"
#

set -euo pipefail

#--- Configuration ---

STEPS=(1 50 100 500 1000)
TAG_COUNTS=(1 50 100 500 1000)
MODES=(sync async)

AB_SERVER_PORT=44818
AB_SERVER_PID=""

#--- Parse arguments ---

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
EXECUTABLE_DIR=""
RESULTS_FILE=""
OUTPUT_DIR=""
DURATION="10"
BASE_TAG_PATH=""

for arg in "$@"; do
    case "$arg" in
        --duration=*)       DURATION="${arg#--duration=}" ;;
        --output-dir=*)     OUTPUT_DIR="${arg#--output-dir=}" ;;
        --results-file=*)   RESULTS_FILE="${arg#--results-file=}" ;;
        --executable-dir=*) EXECUTABLE_DIR="${arg#--executable-dir=}" ;;
        --tag=*)            BASE_TAG_PATH="${arg#--tag=}" ;;
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
        RESULTS_FILE="${OUTPUT_DIR}/perf_logix_results_${TIMESTAMP}.csv"
    else
        RESULTS_FILE="perf_logix_results_${TIMESTAMP}.csv"
    fi
fi

PERF_BENCHMARK="${EXECUTABLE_DIR}/perf_benchmark"
AB_SERVER="${EXECUTABLE_DIR}/ab_server_fiber"

# Verify perf_benchmark exists.
if [ ! -x "$PERF_BENCHMARK" ]; then
    echo "ERROR: $PERF_BENCHMARK not found or not executable." >&2
    exit 1
fi

# Only require ab_server_fiber when running in simulator mode (no --tag supplied).
if [ -z "$BASE_TAG_PATH" ] && [ ! -x "$AB_SERVER" ]; then
    echo "ERROR: $AB_SERVER not found or not executable (required when --tag is not supplied)." >&2
    exit 1
fi

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

kill_ab_server() {
    # Kill any running ab_server_fiber instances (including ones not started by this script).
    kill_process ab_server_fiber

    if [ -n "$AB_SERVER_PID" ]; then
        wait "$AB_SERVER_PID" 2>/dev/null || true
        AB_SERVER_PID=""
    fi
}

cleanup() {
    kill_ab_server
}
trap cleanup EXIT INT TERM

start_ab_server() {
    kill_ab_server

    echo "Starting ab_server_fiber on port $AB_SERVER_PORT..." >&2
    "$AB_SERVER" \
        --plc=ControlLogix \
        --path=1,0 \
        --port="$AB_SERVER_PORT" \
        --tag=TestBigArray:DINT[2000] \
        &
    AB_SERVER_PID=$!

    # Give it time to bind the port.
    sleep 2

    if ! kill -0 "$AB_SERVER_PID" 2>/dev/null; then
        echo "ERROR: ab_server_fiber failed to start." >&2
        exit 1
    fi
    echo "ab_server_fiber running (PID $AB_SERVER_PID)." >&2
}

#--- Capture metadata ---

GIT_HASH=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
GIT_DESCRIBE=$(git describe --tags --always --dirty 2>/dev/null || echo "unknown")
PLATFORM=$(uname -s -m)
HOSTNAME_SHORT=$(hostname -s 2>/dev/null || hostname)

echo "# perf_benchmark results" >&2
echo "#   version:  $GIT_DESCRIBE ($GIT_HASH)" >&2
echo "#   platform: $PLATFORM" >&2
echo "#   host:     $HOSTNAME_SHORT" >&2
echo "#   date:     $(date -Iseconds)" >&2
echo "#   duration: ${DURATION}s per test" >&2
if [ -n "$BASE_TAG_PATH" ]; then
    echo "#   tag:      $BASE_TAG_PATH" >&2
else
    echo "#   tag:      (simulator)" >&2
fi

#--- Write CSV header ---

{
    if [ -n "$BASE_TAG_PATH" ]; then
        echo "# version=$GIT_DESCRIBE commit=$GIT_HASH platform=\"$PLATFORM\" host=$HOSTNAME_SHORT date=$(date -Iseconds) duration_per_test=${DURATION}s tag=\"$BASE_TAG_PATH\""
    else
        echo "# version=$GIT_DESCRIBE commit=$GIT_HASH platform=\"$PLATFORM\" host=$HOSTNAME_SHORT date=$(date -Iseconds) duration_per_test=${DURATION}s tag=simulator"
    fi
    echo "mode,connection_groups,threads,tags,tags_per_thread,duration_ms,total_reads,reads_per_sec,cpu_load_pct,fairness_cv,fairness_min_max_ratio"
} > "$RESULTS_FILE"

#--- Start simulator if no explicit tag path was provided ---

if [ -z "$BASE_TAG_PATH" ]; then
    start_ab_server
    BASE_TAG_PATH="protocol=ab-eip&gateway=127.0.0.1:${AB_SERVER_PORT}&path=1,0&plc=ControlLogix&name=TestBigArray[0]"
fi

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

                # Restart ab_server_fiber in case it crashed (simulator mode only).
                if [ -n "$AB_SERVER_PID" ] && ! kill -0 "$AB_SERVER_PID" 2>/dev/null; then
                    echo "  -> ab_server_fiber died, restarting..." >&2
                    start_ab_server
                    BASE_TAG_PATH="protocol=ab-eip&gateway=127.0.0.1:${AB_SERVER_PORT}&path=1,0&plc=ControlLogix&name=TestBigArray[0]"
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

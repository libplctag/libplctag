#!/usr/bin/env bash
#
# run_perf_benchmark.sh - Drive the perf_benchmark program across the full
# parameter matrix and produce a CSV results file.
#
# Usage:
#   run_perf_benchmark.sh <BUILD_DIR> [RESULTS_FILE] [DURATION_PER_TEST]
#
#   BUILD_DIR          - directory containing perf_benchmark and ab_server binaries
#   RESULTS_FILE       - output CSV path (default: perf_results_<timestamp>.csv)
#   DURATION_PER_TEST  - seconds per test case (default: 10)
#
# The script starts ab_server automatically and cleans it up on exit.
#
# Example:
#   ./run_perf_benchmark.sh ../../build/bin_dist
#   ./run_perf_benchmark.sh ../../build/bin_dist results.csv 5
#

set -euo pipefail

#--- Configuration ---

CONNECTION_GROUPS=(1 33 66 100)
THREAD_COUNTS=(1 50 100 500 1000)
TAG_COUNTS=(1 50 100 500 1000)
MODES=(sync async)

AB_SERVER_PORT=44818
AB_SERVER_PID=""

#--- Parse arguments ---

if [ $# -lt 1 ]; then
    echo "Usage: $0 <BUILD_DIR> [RESULTS_FILE] [DURATION_PER_TEST]" >&2
    exit 1
fi

BUILD_DIR="$1"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULTS_FILE="${2:-perf_results_${TIMESTAMP}.csv}"
DURATION="${3:-10}"

PERF_BENCHMARK="${BUILD_DIR}/perf_benchmark"
AB_SERVER="${BUILD_DIR}/ab_server"

# Verify binaries exist.
for bin in "$PERF_BENCHMARK" "$AB_SERVER"; do
    if [ ! -x "$bin" ]; then
        echo "ERROR: $bin not found or not executable." >&2
        exit 1
    fi
done

#--- Helper functions ---

kill_ab_server() {
    if [ -n "$AB_SERVER_PID" ] && kill -0 "$AB_SERVER_PID" 2>/dev/null; then
        echo "Stopping ab_server (PID $AB_SERVER_PID)..." >&2
        kill "$AB_SERVER_PID" 2>/dev/null || true
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

    echo "Starting ab_server on port $AB_SERVER_PORT..." >&2
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
        echo "ERROR: ab_server failed to start." >&2
        exit 1
    fi
    echo "ab_server running (PID $AB_SERVER_PID)." >&2
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

#--- Write CSV header ---

{
    echo "# version=$GIT_DESCRIBE commit=$GIT_HASH platform=\"$PLATFORM\" host=$HOSTNAME_SHORT date=$(date -Iseconds) duration_per_test=${DURATION}s"
    echo "mode,connection_groups,threads,tags,tags_per_thread,duration_ms,total_reads,reads_per_sec,cpu_load_pct,fairness_cv,fairness_min_max_ratio"
} > "$RESULTS_FILE"

#--- Start simulator ---

start_ab_server

#--- Run the matrix ---

total_combos=0
skipped=0
failed=0
completed=0

# Count valid combinations first.
for mode in "${MODES[@]}"; do
    for groups in "${CONNECTION_GROUPS[@]}"; do
        for threads in "${THREAD_COUNTS[@]}"; do
            for tags in "${TAG_COUNTS[@]}"; do
                if [ "$threads" -lt "$groups" ]; then
                    continue
                fi
                total_combos=$((total_combos + 1))
            done
        done
    done
done

echo "" >&2
echo "Running $total_combos test configurations (${DURATION}s each)..." >&2
echo "" >&2

run_index=0

for mode in "${MODES[@]}"; do
    for groups in "${CONNECTION_GROUPS[@]}"; do
        for threads in "${THREAD_COUNTS[@]}"; do
            for tags in "${TAG_COUNTS[@]}"; do
                # Skip invalid: fewer threads than connection groups.
                if [ "$threads" -lt "$groups" ]; then
                    skipped=$((skipped + 1))
                    continue
                fi

                run_index=$((run_index + 1))
                echo "[$run_index/$total_combos] mode=$mode groups=$groups threads=$threads tags=$tags ..." >&2

                csv_line=$("$PERF_BENCHMARK" \
                    --mode="$mode" \
                    --groups="$groups" \
                    --threads="$threads" \
                    --tags="$tags" \
                    --duration="$DURATION" \
                    --port="$AB_SERVER_PORT" \
                    2>/dev/null)

                rc=$?
                if [ $rc -eq 0 ] && [ -n "$csv_line" ]; then
                    echo "$csv_line" >> "$RESULTS_FILE"
                    # Extract reads/sec for progress display.
                    rps=$(echo "$csv_line" | cut -d',' -f8)
                    echo "  -> reads/sec=$rps" >&2
                    completed=$((completed + 1))
                else
                    echo "  -> FAILED (exit code $rc)" >&2
                    failed=$((failed + 1))

                    # Restart ab_server in case it crashed.
                    if ! kill -0 "$AB_SERVER_PID" 2>/dev/null; then
                        echo "  -> ab_server died, restarting..." >&2
                        start_ab_server
                    fi
                fi
            done
        done
    done
done

#--- Summary ---

echo "" >&2
echo "========================================" >&2
echo "Benchmark complete." >&2
echo "  Completed: $completed" >&2
echo "  Failed:    $failed" >&2
echo "  Skipped:   $skipped (threads < groups)" >&2
echo "  Results:   $RESULTS_FILE" >&2
echo "========================================" >&2

exit $failed

#!/usr/bin/env bash
#
# run_simulator_tests_parallel.sh — Run simulator tests in parallel using
# separate server instances on different ports.
#
# Usage:
#   run_simulator_tests_parallel.sh [options]
#
#   --executable-dir=DIR   directory containing test binaries (required)
#   --output-dir=DIR       directory for per-test logs and status files (required)
#   --display-status       live redrawn status display (interactive terminals only)
#

# ──────────────────────────────────────────────────────────────────────────────
# Argument parsing
# ──────────────────────────────────────────────────────────────────────────────

EXECUTABLE_DIR=""
OUTPUT_DIR=""
DISPLAY_STATUS=0

for arg in "$@"; do
    case "$arg" in
        --executable-dir=*)  EXECUTABLE_DIR="${arg#--executable-dir=}" ;;
        --output-dir=*)      OUTPUT_DIR="${arg#--output-dir=}" ;;
        --display-status)    DISPLAY_STATUS=1 ;;
        --*)                 echo "ERROR: Unknown flag: $arg" >&2; exit 1 ;;
        *)                   echo "ERROR: Unknown argument: $arg" >&2; exit 1 ;;
    esac
done

if [[ -z "$EXECUTABLE_DIR" ]]; then
    echo "ERROR: --executable-dir is required." >&2
    exit 1
fi

if [[ -z "$OUTPUT_DIR" ]]; then
    echo "ERROR: --output-dir is required." >&2
    exit 1
fi

# ──────────────────────────────────────────────────────────────────────────────
# Platform detection and path normalisation
# ──────────────────────────────────────────────────────────────────────────────

is_windows_shell() {
    case "$OSTYPE" in
        msys*|cygwin*|win32*) return 0 ;;
        *) return 1 ;;
    esac
}

if is_windows_shell; then
    EXECUTABLE_DIR=$(echo "$EXECUTABLE_DIR" | tr '\\' '/')
    EXECUTABLE_DIR=$(echo "$EXECUTABLE_DIR" | sed 's|^\([A-Za-z]\):|/\L\1|')
    OUTPUT_DIR=$(echo "$OUTPUT_DIR" | tr '\\' '/')
    OUTPUT_DIR=$(echo "$OUTPUT_DIR" | sed 's|^\([A-Za-z]\):|/\L\1|')
fi

# Expand leading ~
EXECUTABLE_DIR="${EXECUTABLE_DIR/#\~/$HOME}"
OUTPUT_DIR="${OUTPUT_DIR/#\~/$HOME}"

echo "EXECUTABLE_DIR : $EXECUTABLE_DIR"
echo "OUTPUT_DIR     : $OUTPUT_DIR"
echo "OSTYPE         : $OSTYPE"

# ──────────────────────────────────────────────────────────────────────────────
# Setup
# ──────────────────────────────────────────────────────────────────────────────

if [[ ! -d "$EXECUTABLE_DIR" ]]; then
    echo "ERROR: $EXECUTABLE_DIR is not a valid directory." >&2
    exit 1
fi

mkdir -p "$OUTPUT_DIR"

WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

# Raise the open-file limit before spawning parallel groups so all subshells
# inherit the higher limit regardless of execution order.
if ! is_windows_shell; then
    ulimit -n 1024
fi

VALGRIND=""

# Port assignments for parallel servers.
# PORT_LOGIX_FAST must stay 44818: test_auto_sync, test_indexed_tags,
# test_shutdown_cip, test_callback, and test_callback_ex all hardcode
# gateway=127.0.0.1 with no port override.
PORT_LOGIX_FAST=44818
PORT_MICRO800=44820
PORT_OMRON=44821
PORT_MICROLOGIX=44822
PORT_PLC5=44823

# ──────────────────────────────────────────────────────────────────────────────
# Executable preflight check
# ──────────────────────────────────────────────────────────────────────────────

EXECUTABLES="ab_server modbus_server list_tags_logix string_non_standard_udt string_standard tag_rw2 test_create_from_tag test_connection_tag test_fairness test_auto_sync test_callback test_callback_ex test_callback_ex_logix test_callback_ex_modbus test_idle_disconnect test_modbus_multiple test_raw_cip test_reconnect_after_outage_async test_reconnect_after_outage_sync test_shutdown_cip test_shutdown_modbus test_shutdown_restart test_special test_string test_tag_attributes test_tag_type_attribute thread_stress"

for EXECUTABLE in $EXECUTABLES; do
    if [[ ! -e "$EXECUTABLE_DIR/$EXECUTABLE" ]]; then
        echo "ERROR: $EXECUTABLE_DIR/$EXECUTABLE not found!" >&2
        exit 1
    fi
done

# ──────────────────────────────────────────────────────────────────────────────
# Helper functions
# ──────────────────────────────────────────────────────────────────────────────

# Name-based kill — only safe when no other instance of the named process runs.
kill_process() {
    local process_name=$1
    if is_windows_shell; then
        taskkill //F //IM "${process_name}.exe" > /dev/null 2>&1
        sleep 2
    else
        pkill -TERM "$process_name" > /dev/null 2>&1
    fi
}

# PID-based kill — used in Phase 1 so each group kills only its own server.
kill_server_pid() {
    local pid=$1
    if is_windows_shell; then
        taskkill //F //PID "$pid" > /dev/null 2>&1
        sleep 2
    else
        kill -TERM "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
    fi
}

dump_binary_diagnostics() {
    local binary_path=$1
    if [[ -e "$binary_path" ]]; then
        echo "Binary details for $binary_path:"
        ls -l "$binary_path" || true
    else
        echo "Binary not found: $binary_path"
        return
    fi
    if command -v objdump > /dev/null 2>&1; then
        echo "Dynamic dependencies (objdump):"
        objdump -p "$binary_path" 2>/dev/null | grep "DLL Name" || true
    fi
}

# run_test GROUP_OUT GROUP_RES NUM DESC CMD [ARGS...]
#
# Each test gets its own directory under OUTPUT_DIR:
#   $OUTPUT_DIR/test_NNN/test.log    — captured stdout+stderr
#   $OUTPUT_DIR/test_NNN/status      — RUNNING → OK | FAILED
#
# Appends "  Test N: desc... OK|FAILURE\n" to GROUP_OUT.
# Appends P or F to GROUP_RES.
run_test() {
    local group_out=$1 group_res=$2 num=$3 desc=$4
    shift 4

    local test_out_dir="$OUTPUT_DIR/test_$(printf '%03d' "$num")"
    mkdir -p "$test_out_dir"

    echo "RUNNING" > "$test_out_dir/status"
    printf "  Test %d: %s... " "$num" "$desc" >> "$group_out"

    if "$@" > "$test_out_dir/test.log" 2>&1; then
        echo "OK"      >> "$group_out"
        echo "OK"      >  "$test_out_dir/status"
        echo P         >> "$group_res"
    else
        echo "FAILURE" >> "$group_out"
        echo "FAILED"  >  "$test_out_dir/status"
        echo F         >> "$group_res"
    fi
}

# run_test_expect_fail: the command is expected to fail; success is a FAILURE.
run_test_expect_fail() {
    local group_out=$1 group_res=$2 num=$3 desc=$4
    shift 4

    local test_out_dir="$OUTPUT_DIR/test_$(printf '%03d' "$num")"
    mkdir -p "$test_out_dir"

    echo "RUNNING" > "$test_out_dir/status"
    printf "  Test %d: %s... " "$num" "$desc" >> "$group_out"

    if "$@" > "$test_out_dir/test.log" 2>&1; then
        echo "FAILURE" >> "$group_out"
        echo "FAILED"  >  "$test_out_dir/status"
        echo F         >> "$group_res"
    else
        echo "OK"      >> "$group_out"
        echo "OK"      >  "$test_out_dir/status"
        echo P         >> "$group_res"
    fi
}

# count_results RES_FILE — prints "successes failures"
count_results() {
    local res=$1
    local s=0 f=0
    if [[ -f "$res" ]]; then
        while IFS= read -r line; do
            [[ "$line" == P ]] && s=$((s+1)) || f=$((f+1))
        done < "$res"
    fi
    echo "$s $f"
}

# ──────────────────────────────────────────────────────────────────────────────
# Live status display (only active when --display-status is passed)
# ──────────────────────────────────────────────────────────────────────────────
#
# Displays a redrawn 3-line block:
#   Line 1: summary counts
#   Line 2: one-char-per-test bar  (space=pending  *=running  ✓=ok  ✗=failed)
#   Line 3: list of currently-running test numbers
#
# The display loop runs as a background job during Phase 1 and is stopped
# before Phase 2 begins (Phase 2 prints directly to stdout).

ALL_TEST_NUMS=(1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 \
               20 21 22 23 24 25 26 27 28 29 30 31 32 33 \
               34 35 36 37 38 39 40 41 42 43)

DISPLAY_PID=""

draw_status() {
    local bar="" running_list="" passed=0 failed=0 running_count=0 pending=0
    local cols="${COLUMNS:-80}"

    for n in "${ALL_TEST_NUMS[@]}"; do
        local status_file="$OUTPUT_DIR/test_$(printf '%03d' "$n")/status"
        local st="PENDING"
        if [[ -f "$status_file" ]]; then
            read -r st < "$status_file" 2>/dev/null || st="PENDING"
        fi

        case "$st" in
            PENDING) bar+=" ";                              pending=$((pending+1)) ;;
            RUNNING) bar+="*";                             running_count=$((running_count+1)); running_list+=" $n" ;;
            OK)      bar+=$'\e[32m\xe2\x9c\x93\e[0m';     passed=$((passed+1)) ;;   # green ✓
            FAILED)  bar+=$'\e[31m\xe2\x9c\x97\e[0m';     failed=$((failed+1)) ;;   # red ✗
        esac
    done

    printf "Passed: %-3d  Failed: %-3d  Running: %-3d  Pending: %-3d\n" \
        "$passed" "$failed" "$running_count" "$pending"
    printf "[%s]\n" "$bar"
    # Pad running list line to terminal width so leftover chars are overwritten.
    local running_str="Running tests:${running_list:- (none)}"
    printf "%-${cols}s\n" "$running_str"
}

display_loop() {
    local initialized=0
    while true; do
        [[ $initialized -eq 1 ]] && printf '\e[3A\r'
        draw_status
        initialized=1
        sleep 0.1
    done
}

start_display() {
    [[ $DISPLAY_STATUS -eq 0 ]] && return
    ( display_loop ) &
    DISPLAY_PID=$!
}

stop_display() {
    [[ $DISPLAY_STATUS -eq 0 ]] && return
    [[ -z "$DISPLAY_PID" ]] && return
    kill "$DISPLAY_PID" 2>/dev/null
    wait "$DISPLAY_PID" 2>/dev/null
    DISPLAY_PID=""
    # Overwrite the running display block with the final state.
    printf '\e[3A\r'
    draw_status
    printf '\n'
}

# ──────────────────────────────────────────────────────────────────────────────
# Phase 1 group functions
# Each runs in a background subshell.
# Output lines  → $WORK_DIR/out_<letter>  (printed after all groups finish)
# Per-test P/F  → $WORK_DIR/res_<letter>  (tallied at the end)
# Per-test dirs → $OUTPUT_DIR/test_NNN/   (log + status files)
# ──────────────────────────────────────────────────────────────────────────────

group_logix_fast() {
    local out="$WORK_DIR/out_logix_fast"
    local res="$WORK_DIR/res_logix_fast"
    local port=$PORT_LOGIX_FAST
    local gw="127.0.0.1:$port"

    echo "Starting AB emulator for fast ControlLogix tests (port $port)." >> "$out"
    { $EXECUTABLE_DIR/ab_server --debug --plc=ControlLogix --path=1,0 --port=$port \
        "--tag=TestBigArray:DINT[2000]" "--tag=Test_Array_1:DINT[1000]" \
        "--tag=Test_Array_2x3:DINT[2,3]" "--tag=Test_Array_2x3x4:DINT[2,3,4]" \
        > "$OUTPUT_DIR/logix_fast_emulator.log" 2>&1 & } 2>/dev/null
    local EMULATOR_PID=$!
    if [ $EMULATOR_PID -le 0 ]; then
        echo "Unable to start AB/ControlLogix fast emulator!" >> "$out"
        for ((i=0; i<14; i++)); do echo F >> "$res"; done
        return 1
    fi
    sleep 3

    run_test "$out" "$res" 1 "basic unconnected tag read/write" \
        $VALGRIND$EXECUTABLE_DIR/tag_rw2 --type=sint32 \
        "--tag=protocol=ab-eip&gateway=$gw&path=1,0&plc=ControlLogix&elem_count=10&name=TestBigArray&use_connected_msg=0" \
        --debug=4 --write=1,2,3,4,5,6,7,8,9

    run_test "$out" "$res" 2 "basic unconnected large tag read/write" \
        $VALGRIND$EXECUTABLE_DIR/tag_rw2 --type=sint32 \
        "--tag=protocol=ab-eip&gateway=$gw&path=1,0&plc=ControlLogix&elem_count=1000&name=TestBigArray&use_connected_msg=0" \
        --debug=4 --write=1,2,3,4,5,6,7,8,9

    run_test "$out" "$res" 3 "basic connected tag read/write" \
        $VALGRIND$EXECUTABLE_DIR/tag_rw2 --type=sint32 \
        "--tag=protocol=ab-eip&gateway=$gw&path=1,0&plc=ControlLogix&elem_count=10&name=TestBigArray" \
        --debug=4 --write=1,2,3,4,5,6,7,8,9

    run_test "$out" "$res" 4 "basic connected large tag read/write" \
        $VALGRIND$EXECUTABLE_DIR/tag_rw2 --type=sint32 \
        "--tag=protocol=ab-eip&gateway=$gw&path=1,0&plc=ControlLogix&elem_count=1000&name=TestBigArray" \
        --debug=4 --write=1,2,3,4,5,6,7,8,9

    run_test "$out" "$res" 5 "stress RC memory code" \
        $VALGRIND$EXECUTABLE_DIR/stress_rc_mem

    run_test "$out" "$res" 6 "CIP thread stress" \
        $EXECUTABLE_DIR/thread_stress 20 \
        "protocol=ab-eip&gateway=$gw&path=1,0&plc=ControlLogix&name=TestBigArray"

    # Tests 7, 8, 11 hardcode gateway=127.0.0.1 — they require PORT_LOGIX_FAST=44818.
    run_test "$out" "$res" 7 "auto sync" \
        $VALGRIND$EXECUTABLE_DIR/test_auto_sync

    run_test "$out" "$res" 8 "indexed tags" \
        $VALGRIND$EXECUTABLE_DIR/test_indexed_tags

    run_test "$out" "$res" 9 "AB/ControlLogix tag scheduling fairness" \
        $VALGRIND$EXECUTABLE_DIR/test_fairness \
        "--tag=protocol=ab-eip&gateway=$gw&path=1,0&plc=ControlLogix&name=TestBigArray[0]&auto_sync_read_ms=200" \
        --num-tags=200 --test-duration-secs=10

    run_test "$out" "$res" 10 "idle disconnect and reconnect with runtime timeout change (AB ControlLogix)" \
        $VALGRIND$EXECUTABLE_DIR/test_idle_disconnect \
        "--tag=protocol=ab-eip&gateway=$gw&path=1,0&plc=ControlLogix&name=TestBigArray"

    run_test "$out" "$res" 11 "hard library shutdown" \
        $VALGRIND$EXECUTABLE_DIR/test_shutdown_cip

    run_test "$out" "$res" 12 "library shutdown and restart" \
        $VALGRIND$EXECUTABLE_DIR/test_shutdown_restart \
        "--tag=protocol=ab-eip&gateway=$gw&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray"

    run_test "$out" "$res" 13 "connection tag connection state transitions (ControlLogix)" \
        $VALGRIND$EXECUTABLE_DIR/test_connection_tag \
        "--tag=protocol=ab-eip&gateway=$gw&path=1,0&plc=ControlLogix&name=@connection"

    run_test "$out" "$res" 14 "create-from-tag API (12 comprehensive permutation tests with AB/EIP)" \
        $VALGRIND$EXECUTABLE_DIR/test_create_from_tag \
        "--src-tag=protocol=ab-eip&gateway=$gw&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray[0]" \
        "--clone-attrib=name=TestBigArray[1]&elem_count=1" \
        "--device-tag=protocol=ab-eip&gateway=$gw&path=1,0&plc=ControlLogix&name=@connection" \
        --timeout=10000

    echo "Killing fast ControlLogix emulator." >> "$out"
    kill_server_pid $EMULATOR_PID
}


group_micro800() {
    local out="$WORK_DIR/out_micro800"
    local res="$WORK_DIR/res_micro800"
    local port=$PORT_MICRO800
    local gw="127.0.0.1:$port"

    echo "Starting AB emulator for Micro800 tests (port $port)." >> "$out"
    { $EXECUTABLE_DIR/ab_server --debug --plc=Micro800 --port=$port \
        "--tag=TestDINTArray:DINT[10]" \
        > "$OUTPUT_DIR/micro800_emulator.log" 2>&1 & } 2>/dev/null
    local EMULATOR_PID=$!
    if [ $EMULATOR_PID -le 0 ]; then
        echo "Unable to start Micro800 emulator!" >> "$out"
        echo F >> "$res"
        return 1
    fi
    sleep 1

    run_test "$out" "$res" 20 "basic Micro800 read/write" \
        $VALGRIND$EXECUTABLE_DIR/tag_rw2 --type=sint32 \
        "--tag=protocol=ab-eip&gateway=$gw&plc=micro800&name=TestDINTArray" \
        --write=42 --debug=4

    echo "Killing Micro800 emulator." >> "$out"
    kill_server_pid $EMULATOR_PID
}


group_omron() {
    local out="$WORK_DIR/out_omron"
    local res="$WORK_DIR/res_omron"
    local port=$PORT_OMRON
    local gw="127.0.0.1:$port"

    echo "Starting AB emulator for Omron tests (port $port)." >> "$out"
    { $EXECUTABLE_DIR/ab_server --debug --plc=Omron --port=$port \
        "--tag=TestDINTArray:DINT[10]" \
        > "$OUTPUT_DIR/omron_emulator.log" 2>&1 & } 2>/dev/null
    local EMULATOR_PID=$!
    if [ $EMULATOR_PID -le 0 ]; then
        echo "Unable to start Omron emulator!" >> "$out"
        for ((i=0; i<2; i++)); do echo F >> "$res"; done
        return 1
    fi
    sleep 1

    run_test "$out" "$res" 21 "basic Omron read/write" \
        $VALGRIND$EXECUTABLE_DIR/tag_rw2 --type=sint32 \
        "--tag=protocol=ab-eip&gateway=$gw&path=18,127.0.0.1&plc=omron-njnx&name=TestDINTArray" \
        --write=42 --debug=4

    run_test "$out" "$res" 22 "idle disconnect and reconnect with runtime timeout change (Omron)" \
        $VALGRIND$EXECUTABLE_DIR/test_idle_disconnect \
        "--tag=protocol=ab-eip&gateway=$gw&path=18,127.0.0.1&plc=omron-njnx&name=TestDINTArray"

    echo "Killing Omron emulator." >> "$out"
    kill_server_pid $EMULATOR_PID
}


group_micrologix() {
    local out="$WORK_DIR/out_micrologix"
    local res="$WORK_DIR/res_micrologix"
    local port=$PORT_MICROLOGIX
    local gw="127.0.0.1:$port"

    echo "Starting AB emulator for Micrologix tests (port $port)." >> "$out"
    { $EXECUTABLE_DIR/ab_server --debug --plc=Micrologix --port=$port \
        '--tag=B3[10]' '--tag=N7[10]' '--tag=L19[10]' \
        > "$OUTPUT_DIR/micrologix_emulator.log" 2>&1 & } 2>/dev/null
    local EMULATOR_PID=$!
    if [ $EMULATOR_PID -le 0 ]; then
        echo "Unable to start Micrologix emulator!" >> "$out"
        for ((i=0; i<7; i++)); do echo F >> "$res"; done
        return 1
    fi
    sleep 1

    run_test "$out" "$res" 23 "B data file Micrologix tag read/write" \
        $VALGRIND$EXECUTABLE_DIR/tag_rw2 --type=uint16 \
        "--tag=protocol=ab-eip&gateway=$gw&plc=micrologix&name=B3:0" \
        --write=0 --debug=4

    run_test "$out" "$res" 24 "B bit data file Micrologix tag read/write" \
        $VALGRIND$EXECUTABLE_DIR/tag_rw2 --type=bit \
        "--tag=protocol=ab-eip&gateway=$gw&plc=micrologix&name=B3:0/6" \
        --write=1 --debug=4

    run_test "$out" "$res" 25 "N data file Micrologix tag read/write" \
        $VALGRIND$EXECUTABLE_DIR/tag_rw2 --type=sint16 \
        "--tag=protocol=ab-eip&gateway=$gw&plc=micrologix&name=N7:0" \
        --write=42 --debug=4

    run_test "$out" "$res" 26 "N bit data file Micrologix tag read/write" \
        $VALGRIND$EXECUTABLE_DIR/tag_rw2 --type=bit \
        "--tag=protocol=ab-eip&gateway=$gw&plc=micrologix&name=N7:0/10" \
        --write=1 --debug=4

    run_test "$out" "$res" 27 "L data file Micrologix tag read/write" \
        $VALGRIND$EXECUTABLE_DIR/tag_rw2 --type=sint32 \
        "--tag=protocol=ab-eip&gateway=$gw&plc=micrologix&name=L19:0" \
        --write=0,1,2,3 --debug=4

    run_test "$out" "$res" 28 "L bit data file Micrologix tag read" \
        $VALGRIND$EXECUTABLE_DIR/tag_rw2 --type=bit \
        "--tag=protocol=ab-eip&gateway=$gw&plc=micrologix&name=L19:0/23" \
        --debug=4

    run_test_expect_fail "$out" "$res" 29 "L bit data file Micrologix tag write (should fail)" \
        $VALGRIND$EXECUTABLE_DIR/tag_rw2 --type=bit \
        "--tag=protocol=ab-eip&gateway=$gw&plc=micrologix&name=L19:0/23" \
        --write=1 --debug=4

    echo "Killing Micrologix emulator." >> "$out"
    kill_server_pid $EMULATOR_PID
}


group_plc5() {
    local out="$WORK_DIR/out_plc5"
    local res="$WORK_DIR/res_plc5"
    local port=$PORT_PLC5
    local gw="127.0.0.1:$port"

    echo "Starting AB emulator for PLC5 tests (port $port)." >> "$out"
    { $EXECUTABLE_DIR/ab_server --debug --plc=PLC/5 --port=$port \
        '--tag=B3[10]' '--tag=N7[10]' \
        > "$OUTPUT_DIR/plc5_emulator.log" 2>&1 & } 2>/dev/null
    local EMULATOR_PID=$!
    if [ $EMULATOR_PID -le 0 ]; then
        echo "Unable to start PLC5 emulator!" >> "$out"
        for ((i=0; i<4; i++)); do echo F >> "$res"; done
        return 1
    fi
    sleep 1

    run_test "$out" "$res" 30 "B data file PLC5 tag read/write" \
        $VALGRIND$EXECUTABLE_DIR/tag_rw2 --type=uint16 \
        "--tag=protocol=ab-eip&gateway=$gw&plc=plc5&elem_count=1&name=B3:0" \
        --debug=4 --write=0

    run_test "$out" "$res" 31 "B bit data file PLC5 tag read/write" \
        $VALGRIND$EXECUTABLE_DIR/tag_rw2 --type=bit \
        "--tag=protocol=ab-eip&gateway=$gw&plc=plc5&elem_count=1&name=B3:0/10" \
        --debug=4 --write=1

    run_test "$out" "$res" 32 "N data file PLC5 tag read/write" \
        $VALGRIND$EXECUTABLE_DIR/tag_rw2 --type=sint16 \
        "--tag=protocol=ab-eip&gateway=$gw&plc=plc5&elem_count=1&name=N7:0" \
        --debug=4 --write=0

    run_test "$out" "$res" 33 "N bit data file PLC5 tag read/write" \
        $VALGRIND$EXECUTABLE_DIR/tag_rw2 --type=bit \
        "--tag=protocol=ab-eip&gateway=$gw&plc=plc5&elem_count=1&name=N7:0/10" \
        --debug=4 --write=1

    echo "Killing PLC5 emulator." >> "$out"
    kill_server_pid $EMULATOR_PID
}


group_modbus() {
    local out="$WORK_DIR/out_modbus"
    local res="$WORK_DIR/res_modbus"

    echo "Starting Modbus server (ports 1502 and 2502)." >> "$out"
    $EXECUTABLE_DIR/modbus_server --listen=127.0.0.1:1502 --listen=127.0.0.1:2502 \
        --debug=DETAIL > "$OUTPUT_DIR/modbus_server.log" 2>&1 &
    local MODBUS_PID=$!
    if [ $MODBUS_PID -le 0 ]; then
        echo "Unable to start Modbus server!" >> "$out"
        for ((i=0; i<10; i++)); do echo F >> "$res"; done
        return 1
    fi
    sleep 3

    if ! is_windows_shell && ! kill -0 "$MODBUS_PID" > /dev/null 2>&1; then
        echo "Modbus server process exited during startup!" >> "$out"
        wait "$MODBUS_PID" 2>/dev/null
        echo "Modbus server exit code: $?" >> "$out"
        dump_binary_diagnostics "$EXECUTABLE_DIR/modbus_server" >> "$out" 2>&1
        if [[ -f "$OUTPUT_DIR/modbus_server.log" ]]; then
            echo "--- modbus_server.log (tail) ---" >> "$out"
            tail -n 200 "$OUTPUT_DIR/modbus_server.log" >> "$out"
            echo "--- end modbus_server.log ---" >> "$out"
        fi
        for ((i=0; i<10; i++)); do echo F >> "$res"; done
        return 1
    fi

    run_test "$out" "$res" 34 "connection tag connection state transitions (Modbus)" \
        $VALGRIND$EXECUTABLE_DIR/test_connection_tag \
        "--tag=protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&name=@connection"

    run_test "$out" "$res" 35 "test idle disconnect with Modbus" \
        $VALGRIND$EXECUTABLE_DIR/test_idle_disconnect \
        "--tag=protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&elem_count=2&name=hr10"

    run_test "$out" "$res" 36 "thread stress Modbus" \
        $VALGRIND$EXECUTABLE_DIR/thread_stress 10 \
        'protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&elem_count=2&name=hr10'

    run_test "$out" "$res" 37 "connection stress (multiple connections) Modbus" \
        $VALGRIND$EXECUTABLE_DIR/test_connection_stress --num-threads=200 \
        "--tag=protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&elem_count=2&name=hr10"

    run_test "$out" "$res" 38 "callback events Modbus" \
        $VALGRIND$EXECUTABLE_DIR/test_callback_ex_modbus

    run_test "$out" "$res" 39 "create-from-tag API (12 comprehensive permutation tests with Modbus)" \
        $VALGRIND$EXECUTABLE_DIR/test_create_from_tag \
        "--src-tag=protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&elem_count=2&name=hr10" \
        "--clone-attrib=name=hr20&elem_count=2" \
        "--device-tag=protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&name=@connection" \
        --timeout=10000

    run_test "$out" "$res" 40 "hard library shutdown (Modbus)" \
        $VALGRIND$EXECUTABLE_DIR/test_shutdown_modbus

    run_test "$out" "$res" 41 "Modbus tag scheduling fairness" \
        $VALGRIND$EXECUTABLE_DIR/test_fairness \
        "--tag=protocol=modbus-tcp&gateway=127.0.0.1:1502&path=1&name=hr10&auto_sync_read_ms=200" \
        --num-tags=200 --test-duration-secs=10

    run_test "$out" "$res" 42 "for Modbus reconnect bug" \
        $VALGRIND$EXECUTABLE_DIR/test_modbus_multiple

    # Test 43 inspects test 42's log rather than running an external binary.
    local tst42_log="$OUTPUT_DIR/test_$(printf '%03d' 42)/test.log"
    local tst43_dir="$OUTPUT_DIR/test_$(printf '%03d' 43)"
    mkdir -p "$tst43_dir"
    printf "  Test 43: check for exactly 2 PLC creation entries in Modbus reconnect test log... " >> "$out"
    echo "RUNNING" > "$tst43_dir/status"
    local plc_count
    plc_count=$(grep -c "Creating new PLC connection\." "$tst42_log" 2>/dev/null || echo 0)
    if [ "${plc_count}" = "2" ]; then
        echo "OK (found ${plc_count} PLC creation entries)" >> "$out"
        echo "OK"     > "$tst43_dir/status"
        echo P        >> "$res"
    else
        echo "FAILURE (expected 2, found ${plc_count})" >> "$out"
        echo "FAILED" > "$tst43_dir/status"
        echo F        >> "$res"
    fi

    sleep 2
    echo "Killing Modbus server." >> "$out"
    kill_server_pid $MODBUS_PID
    sleep 2
}

# ══════════════════════════════════════════════════════════════════════════════
# PHASE 1 — all groups in parallel
# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "Phase 1 (parallel):"
echo "  tests  1-14  fast ControlLogix  port $PORT_LOGIX_FAST"
echo "  test   20    Micro800           port $PORT_MICRO800"
echo "  tests 21-22  Omron              port $PORT_OMRON"
echo "  tests 23-29  Micrologix         port $PORT_MICROLOGIX"
echo "  tests 30-33  PLC5               port $PORT_PLC5"
echo "  tests 34-43  Modbus             ports 1502, 2502"
echo ""
echo "Phase 2 (sequential, after Phase 1):"
echo "  tests 15-16  reconnect after outage  (kills ab_server by name)"
echo "  tests 17-19  slow ControlLogix       port $PORT_LOGIX_FAST"
echo ""

start_display

PHASE1_PIDS=()
( group_logix_fast ) & PHASE1_PIDS+=($!)
( group_micro800   ) & PHASE1_PIDS+=($!)
( group_omron      ) & PHASE1_PIDS+=($!)
( group_micrologix ) & PHASE1_PIDS+=($!)
( group_plc5       ) & PHASE1_PIDS+=($!)
( group_modbus     ) & PHASE1_PIDS+=($!)

# Wait for the six test groups only — not the display_loop, which must keep
# running until stop_display kills it after this wait returns.
wait "${PHASE1_PIDS[@]}"

stop_display

echo ""
echo "=== Phase 1 output ==="
for name in logix_fast micro800 omron micrologix plc5 modbus; do
    local_out="$WORK_DIR/out_$name"
    [[ -f "$local_out" ]] && cat "$local_out"
done

# ══════════════════════════════════════════════════════════════════════════════
# PHASE 2 — reconnect tests then slow ControlLogix, sequential on port 44818.
#
# The reconnect test binaries call taskkill/pkill by process name, so no
# ab_server instance may be running on any port while they execute.
# The slow ControlLogix emulator starts only after both reconnect tests finish.
# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Phase 2: stand-alone reconnect tests ==="

PHASE2_RES="$WORK_DIR/res_phase2"

# Inline helper for Phase 2: same logic as run_test but output goes to stdout.
run_phase2_test() {
    local num=$1 desc=$2
    shift 2

    local test_out_dir="$OUTPUT_DIR/test_$(printf '%03d' "$num")"
    mkdir -p "$test_out_dir"

    echo "RUNNING" > "$test_out_dir/status"
    printf "  Test %d: %s... " "$num" "$desc"

    if "$@" > "$test_out_dir/test.log" 2>&1; then
        echo "OK"
        echo "OK"     > "$test_out_dir/status"
        echo P        >> "$PHASE2_RES"
    else
        echo "FAILURE"
        echo "FAILED" > "$test_out_dir/status"
        echo F        >> "$PHASE2_RES"
    fi
}

run_phase2_test 15 "Test async reconnect after PLC outage" \
    $VALGRIND$EXECUTABLE_DIR/test_reconnect_after_outage_async "${EXECUTABLE_DIR}/ab_server"

run_phase2_test 16 "Test sync reconnect after PLC outage" \
    $VALGRIND$EXECUTABLE_DIR/test_reconnect_after_outage_sync "${EXECUTABLE_DIR}/ab_server"

echo ""
echo "Starting AB emulator for functional/slow ControlLogix tests (port $PORT_LOGIX_FAST)."
{ $VALGRIND$EXECUTABLE_DIR/ab_server --plc=ControlLogix --path=1,0 \
    "--tag=TestBigArray:DINT[2000]" "--tag=Test_Array_1:DINT[1000]" \
    "--tag=Test_Array_2x3:DINT[2,3]" "--tag=Test_Array_2x3x4:DINT[2,3,4]" \
    --delay=100 > "$OUTPUT_DIR/logix_slow_emulator.log" 2>&1 & } 2>/dev/null
SLOW_PID=$!
if [ $SLOW_PID -le 0 ]; then
    echo "Unable to start slow AB/ControlLogix emulator!"
    exit 1
fi
sleep 1

# test_callback and test_callback_ex hardcode gateway=127.0.0.1 (port 44818).
run_phase2_test 17 "emulator test callbacks" \
    $VALGRIND$EXECUTABLE_DIR/test_callback

run_phase2_test 18 "emulator test extended callbacks sync" \
    $VALGRIND$EXECUTABLE_DIR/test_callback_ex

run_phase2_test 19 "emulator test extended callbacks async" \
    $VALGRIND$EXECUTABLE_DIR/test_callback_ex_logix \
    "--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&cpu=LGX&elem_count=10&name=TestBigArray"

echo "Killing slow ControlLogix emulator."
kill_server_pid $SLOW_PID

# ══════════════════════════════════════════════════════════════════════════════
# Tally all results
# ══════════════════════════════════════════════════════════════════════════════

TOTAL_S=0
TOTAL_F=0

for res_file in "$WORK_DIR"/res_*; do
    [[ -f "$res_file" ]] || continue
    read s f < <(count_results "$res_file")
    TOTAL_S=$((TOTAL_S + s))
    TOTAL_F=$((TOTAL_F + f))
done

TOTAL=$((TOTAL_S + TOTAL_F))

if [[ $DISPLAY_STATUS -eq 1 ]]; then
    echo ""
    draw_status
fi

echo ""
echo "Results:"
echo " - $TOTAL tests."
echo " - $TOTAL_S successes."
echo " - $TOTAL_F failures."

exit $TOTAL_F

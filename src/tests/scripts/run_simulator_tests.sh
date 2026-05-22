#!/bin/bash

TEST_DIR=$1
LOG_DIR=${2:-.}  # Default to current directory if not specified

is_windows_shell() {
    case "$OSTYPE" in
        msys*|cygwin*|win32*)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

# Debug: show what we received
echo "Received TEST_DIR: $TEST_DIR"
echo "Received LOG_DIR: $LOG_DIR"
echo "OSTYPE: $OSTYPE"

# Convert Windows paths to Unix paths if running on Windows (Git Bash/Cygwin)
if is_windows_shell; then
    # Convert D:\path\to\dir to /d/path/to/dir (lowercase drive letter)
    # First replace backslashes with forward slashes using tr
    TEST_DIR=$(echo "$TEST_DIR" | tr '\\' '/')
    # Then replace C: style drive letters with /c/ (lowercase)
    TEST_DIR=$(echo "$TEST_DIR" | sed 's|^\([A-Za-z]\):|/\L\1|')
    echo "After conversion TEST_DIR: $TEST_DIR"
    
    # Convert LOG_DIR if it's a Windows path
    if [[ "$LOG_DIR" != "." ]]; then
        LOG_DIR=$(echo "$LOG_DIR" | tr '\\' '/')
        LOG_DIR=$(echo "$LOG_DIR" | sed 's|^\([A-Za-z]\):|/\L\1|')
        echo "After conversion LOG_DIR: $LOG_DIR"
    fi
fi

# thanks to Stack Overflow
SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"

# Create the log directory if it doesn't exist
mkdir -p "$LOG_DIR"
echo "Logs will be saved to: $LOG_DIR"

TEST=0
SUCCESSES=0
FAILURES=0

# for deeper testing
# VALGRIND="valgrind --tool=memcheck --track-origins=yes --leak-check=full --show-leak-kinds=all --error-exitcode=1  "
VALGRIND=""

# Cross-platform process killing function
kill_process() {
    local process_name=$1
    if is_windows_shell; then
        # Windows (Git Bash/Cygwin)
        taskkill //F //IM "${process_name}.exe" > /dev/null 2>&1
        # Windows releases TCP sockets asynchronously after process exit;
        # wait for the port to become available before the caller starts the next server.
        sleep 2
    else
        # Linux/macOS/Alpine - pkill has consistent syntax across platforms
        pkill -TERM "$process_name" > /dev/null 2>&1
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

if [[ ! -d $TEST_DIR ]]; then
    # echo "Using $TEST_DIR for test executables."
# else
    echo "$TEST_DIR is not a valid path for test executables!"
    exit 1
fi

# test for the executables.
EXECUTABLES="ab_server modbus_server list_tags_logix string_non_standard_udt string_standard tag_rw2 test_connection_stress test_create_from_tag test_connection_tag test_connection_tag_late_join test_fairness test_auto_sync test_callback test_callback_ex test_callback_ex_logix test_callback_ex_modbus test_idle_disconnect test_modbus_multiple test_raw_cip test_reconnect_after_outage_async test_reconnect_after_outage_sync test_shutdown_cip test_shutdown_modbus test_shutdown_restart test_special test_string test_tag_attributes test_tag_type_attribute thread_stress"
# echo -n "  Checking for executables..."
for EXECUTABLE in $EXECUTABLES
do
    # echo -n " $EXECUTABLE "
    if [[ ! -e "$TEST_DIR/$EXECUTABLE" ]]; then
        # echo ""
        echo "$TEST_DIR/$EXECUTABLE not found!"
        exit 1
    fi
done
# echo "...Done."


# echo "  Killing any older AB emulator process."
kill_process ab_server



echo "Starting AB emulator for fast ControlLogix tests."
{ $TEST_DIR/ab_server --debug --plc=ControlLogix --path=1,0 "--tag=TestBigArray:DINT[2000]" "--tag=Test_Array_1:DINT[1000]" "--tag=Test_Array_2x3:DINT[2,3]" "--tag=Test_Array_2x3x4:DINT[2,3,4]" > "$LOG_DIR/logix_fast_emulator.log" 2>&1 & } 2>/dev/null
EMULATOR_PID=$!
if [ $EMULATOR_PID -le 0 ]; then
    echo "Unable to start AB/ControlLogix emulator!"
    exit 1
fi

sleep 3

let TEST++
echo -n "  Test $TEST: basic unconnected tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint32 '--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=10&name=TestBigArray&use_connected_msg=0' --debug=4 --write=1,2,3,4,5,6,7,8,9 > "$LOG_DIR/${TEST}_big_tag_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: basic unconnected large tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint32 '--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=1000&name=TestBigArray&use_connected_msg=0' --debug=4 --write=1,2,3,4,5,6,7,8,9 > "$LOG_DIR/${TEST}_big_tag_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: basic connected tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint32 '--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=10&name=TestBigArray' --debug=4 --write=1,2,3,4,5,6,7,8,9 > "$LOG_DIR/${TEST}_big_tag_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: basic connected large tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint32 '--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=1000&name=TestBigArray' --debug=4 --write=1,2,3,4,5,6,7,8,9 > "$LOG_DIR/${TEST}_big_tag_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: stress RC memory code ... "
$VALGRIND$TEST_DIR/stress_rc_mem > "$LOG_DIR/${TEST}_stress_rc_mem_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: CIP thread stress... "
$TEST_DIR/thread_stress 20 "protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=TestBigArray" > "$LOG_DIR/${TEST}_thread_stress_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "  Test $TEST: auto sync... "
$VALGRIND$TEST_DIR/test_auto_sync > "$LOG_DIR/${TEST}_auto_sync_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "  Test $TEST: indexed tags ... "
$VALGRIND$TEST_DIR/test_indexed_tags > "$LOG_DIR/${TEST}_test_indexed_tags.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "  Test $TEST: AB/ControlLogix tag scheduling fairness... "
$VALGRIND$TEST_DIR/test_fairness "--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=TestBigArray[0]&auto_sync_read_ms=200" --num-tags=200 --test-duration-secs=10 > "$LOG_DIR/${TEST}_ab_fairness_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "  Test $TEST: idle disconnect and reconnect with runtime timeout change (AB ControlLogix)... "
$VALGRIND$TEST_DIR/test_idle_disconnect "--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=TestBigArray" > "$LOG_DIR/${TEST}_idle_disconnect_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "  Test $TEST: hard library shutdown... "
$VALGRIND$TEST_DIR/test_shutdown_cip > "$LOG_DIR/${TEST}_shutdown.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "  Test $TEST: library shutdown and restart... "
$VALGRIND$TEST_DIR/test_shutdown_restart "--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray" > "$LOG_DIR/${TEST}_shutdown_restart.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "  Test $TEST: connection tag connection state transitions (ControlLogix)... "
$VALGRIND$TEST_DIR/test_connection_tag "--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=@connection" > "$LOG_DIR/${TEST}_connection_tag_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: multiple simultaneous @connection tags on same session (ControlLogix)... "
$VALGRIND$TEST_DIR/test_connection_tag \
    "--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=@connection" \
    --num-tags=3 \
    "--data-tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray[0]" > "$LOG_DIR/${TEST}_connection_tag_multi_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: @connection tag late join — session already UP before tag created (ControlLogix)... "
$VALGRIND$TEST_DIR/test_connection_tag_late_join \
    "--data-tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray[0]" \
    "--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=@connection" \
    --timeout=5000 > "$LOG_DIR/${TEST}_connection_tag_late_join_logix_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: @connection tag 2-cycle reconnect (5 s idle timeout, ControlLogix)... "
$VALGRIND$TEST_DIR/test_connection_tag \
    "--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=@connection" \
    --cycles=2 \
    "--data-tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray[0]" \
    --idle-timeout-ms=5000 > "$LOG_DIR/${TEST}_connection_tag_cycle_logix_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: create-from-tag API (17 permutation tests with AB/EIP)... "
$VALGRIND$TEST_DIR/test_create_from_tag \
    "--src-tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray[0]" \
    "--clone-attrib=name=TestBigArray[1]&elem_count=1" \
    "--connection-tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=@connection" \
    --timeout=10000 > "$LOG_DIR/${TEST}_create_from_tag_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


# echo "  Killing AB emulator."
kill_process ab_server


echo "Starting AB emulator for slot-16 path encoding test."
{ $TEST_DIR/ab_server --debug --plc=ControlLogix --path=1,16 "--tag=TestBigArray:DINT[2000]" > "$LOG_DIR/logix_slot16_emulator.log" 2>&1 & } 2>/dev/null
EMULATOR_PID=$!
if [ $EMULATOR_PID -le 0 ]; then
    echo "Unable to start AB/ControlLogix emulator (slot 16)!"
    exit 1
fi

sleep 1

let TEST++
echo -n "  Test $TEST: unconnected tag read/write through chassis slot 16 (path=1,16)... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint32 '--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,16&plc=ControlLogix&elem_count=10&name=TestBigArray&use_connected_msg=0' --debug=4 --write=1,2,3,4,5,6,7,8,9 > "$LOG_DIR/${TEST}_slot16_unconnected_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: connected tag read/write through chassis slot 16 (path=1,16)... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint32 '--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,16&plc=ControlLogix&elem_count=10&name=TestBigArray' --debug=4 --write=1,2,3,4,5,6,7,8,9 > "$LOG_DIR/${TEST}_slot16_connected_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

kill_process ab_server


echo "Starting stand-alone tests."

# ERR_WAIT test: no AB server is running at this point in the script, so the
# connection attempt to 127.0.0.1:44818 fails immediately (ECONNREFUSED),
# which is exactly the condition needed to trigger the ERR_WAIT state.
let TEST++
echo -n "  Test $TEST: @connection tag ERR_WAIT on unreachable host (no server running)... "
$VALGRIND$TEST_DIR/test_connection_tag \
    "--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=@connection" \
    --expect-err > "$LOG_DIR/${TEST}_connection_tag_err_wait_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: Test async reconnect after PLC outage... "
$VALGRIND$TEST_DIR/test_reconnect_after_outage_async "${TEST_DIR}/ab_server" > "$LOG_DIR/${TEST}_reconnect_after_outage_async.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: Test sync reconnect after PLC outage... "
$VALGRIND$TEST_DIR/test_reconnect_after_outage_sync "${TEST_DIR}/ab_server" > "$LOG_DIR/${TEST}_reconnect_after_outage_sync.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


echo "Starting AB emulator for functional/slow ControlLogix tests."
{ $VALGRIND$TEST_DIR/ab_server --plc=ControlLogix --path=1,0 "--tag=TestBigArray:DINT[2000]" "--tag=Test_Array_1:DINT[1000]" "--tag=Test_Array_2x3:DINT[2,3]" "--tag=Test_Array_2x3x4:DINT[2,3,4]" --delay=100  > "$LOG_DIR/logix_slow_emulator.log" 2>&1 & } 2>/dev/null
EMULATOR_PID=$!
if [ $EMULATOR_PID -le 0 ]; then
    echo "Unable to start AB/ControlLogix emulator!"
    exit 1
fi

sleep 1


let TEST++
echo -n "  Test $TEST: emulator test callbacks... "
$VALGRIND$TEST_DIR/test_callback > "$LOG_DIR/${TEST}_callback_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "  Test $TEST: emulator test extended callbacks sync... "
$VALGRIND$TEST_DIR/test_callback_ex > "$LOG_DIR/${TEST}_extended_callback_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "  Test $TEST: emulator test extended callbacks async... "
$VALGRIND$TEST_DIR/test_callback_ex_logix "--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&cpu=LGX&elem_count=10&name=TestBigArray" > "$LOG_DIR/${TEST}_extended_callback_async_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


echo "  Killing AB emulator."
kill_process ab_server


echo "Starting AB emulator for Micro800 tests."
{ $TEST_DIR/ab_server --debug --plc=Micro800 --tag=TestDINTArray:DINT[10] > "$LOG_DIR/micro800_emulator.log" 2>&1 & } 2>/dev/null
EMULATOR_PID=$!
if [ $EMULATOR_PID -le 0 ]; then
    # echo "FAILURE"
    echo "Unable to start Micro800 emulator!"
    exit 1
# else
    # echo "OK"
fi

sleep 1


let TEST++
echo -n "  Test $TEST: basic Micro800 read/write... "
$VALGRIND$TEST_DIR/./tag_rw2 --type=sint32  '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micro800&name=TestDINTArray' --write=42 --debug=4 > "$LOG_DIR/${TEST}_micro800_tag_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


echo "  Killing Micrologix emulator."
kill_process ab_server


echo "Starting AB emulator for Omron tests."
{ $TEST_DIR/ab_server --debug --plc=Omron --tag=TestDINTArray:DINT[10] > "$LOG_DIR/omron_emulator.log" 2>&1 & } 2>/dev/null
EMULATOR_PID=$!
if [ $EMULATOR_PID -le 0 ]; then
    # echo "FAILURE"
    echo "Unable to start AB/Omron emulator!"
    exit 1
# else
    # echo "OK"
fi

sleep 1


let TEST++
echo -n "  Test $TEST: basic Omron read/write... "
$VALGRIND$TEST_DIR/./tag_rw2 --type=sint32  '--tag=protocol=ab-eip&gateway=127.0.0.1&path=18,127.0.0.1&plc=omron-njnx&name=TestDINTArray' --write=42 --debug=4 > "$LOG_DIR/${TEST}_omron_tag_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: Omron thread stress... "
$VALGRIND$TEST_DIR/thread_stress 10 'protocol=ab-eip&gateway=127.0.0.1&path=18,127.0.0.1&plc=omron-njnx&name=TestDINTArray' > "$LOG_DIR/${TEST}_omron_thread_stress_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: idle disconnect and reconnect with runtime timeout change (Omron)... "
$VALGRIND$TEST_DIR/test_idle_disconnect "--tag=protocol=ab-eip&gateway=127.0.0.1&path=18,127.0.0.1&plc=omron-njnx&name=TestDINTArray" > "$LOG_DIR/${TEST}_idle_disconnect_omron_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: @connection tag connection state transitions (Omron)... "
$VALGRIND$TEST_DIR/test_connection_tag "--tag=protocol=ab-eip&gateway=127.0.0.1&path=18,127.0.0.1&plc=omron-njnx&name=@connection" > "$LOG_DIR/${TEST}_connection_tag_omron_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: multiple simultaneous @connection tags on same session (Omron)... "
$VALGRIND$TEST_DIR/test_connection_tag \
    "--tag=protocol=ab-eip&gateway=127.0.0.1&path=18,127.0.0.1&plc=omron-njnx&name=@connection" \
    --num-tags=3 \
    "--data-tag=protocol=ab-eip&gateway=127.0.0.1&path=18,127.0.0.1&plc=omron-njnx&elem_count=1&name=TestDINTArray[0]" > "$LOG_DIR/${TEST}_connection_tag_omron_multi_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: @connection tag 2-cycle reconnect (5 s idle timeout, Omron)... "
$VALGRIND$TEST_DIR/test_connection_tag \
    "--tag=protocol=ab-eip&gateway=127.0.0.1&path=18,127.0.0.1&plc=omron-njnx&name=@connection" \
    --cycles=2 \
    "--data-tag=protocol=ab-eip&gateway=127.0.0.1&path=18,127.0.0.1&plc=omron-njnx&elem_count=1&name=TestDINTArray[0]" \
    --idle-timeout-ms=5000 > "$LOG_DIR/${TEST}_connection_tag_omron_cycle_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: @connection tag late join (Omron)... "
$VALGRIND$TEST_DIR/test_connection_tag_late_join \
    "--data-tag=protocol=ab-eip&gateway=127.0.0.1&path=18,127.0.0.1&plc=omron-njnx&elem_count=1&name=TestDINTArray[0]" \
    "--tag=protocol=ab-eip&gateway=127.0.0.1&path=18,127.0.0.1&plc=omron-njnx&name=@connection" \
    --timeout=5000 > "$LOG_DIR/${TEST}_connection_tag_late_join_omron_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: connection stress (multiple connections) Omron... "
$VALGRIND$TEST_DIR/test_connection_stress --num-threads=200 --tag='protocol=ab-eip&gateway=127.0.0.1&path=18,127.0.0.1&plc=omron-njnx&name=TestDINTArray' > "$LOG_DIR/${TEST}_omron_connection_stress_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: Omron tag scheduling fairness... "
$VALGRIND$TEST_DIR/test_fairness "--tag=protocol=ab-eip&gateway=127.0.0.1&path=18,127.0.0.1&plc=omron-njnx&name=TestDINTArray[0]&auto_sync_read_ms=200" --num-tags=200 --test-duration-secs=10 > "$LOG_DIR/${TEST}_omron_fairness_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: emulator test extended callbacks async (Omron)... "
$VALGRIND$TEST_DIR/test_callback_ex_logix "--tag=protocol=ab-eip&gateway=127.0.0.1&path=18,127.0.0.1&plc=omron-njnx&elem_count=10&name=TestDINTArray" > "$LOG_DIR/${TEST}_omron_callback_ex_logix_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: create-from-tag API (17 permutation tests with Omron)... "
$VALGRIND$TEST_DIR/test_create_from_tag \
    "--src-tag=protocol=ab-eip&gateway=127.0.0.1&path=18,127.0.0.1&plc=omron-njnx&elem_count=1&name=TestDINTArray[0]" \
    "--clone-attrib=name=TestDINTArray[1]&elem_count=1" \
    "--connection-tag=protocol=ab-eip&gateway=127.0.0.1&path=18,127.0.0.1&plc=omron-njnx&name=@connection" \
    --timeout=10000 > "$LOG_DIR/${TEST}_create_from_tag_omron_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

echo "  Killing Omron emulator."
kill_process ab_server


echo "Starting AB emulator for Micrologix tests."
{ $TEST_DIR/ab_server --debug --plc=Micrologix '--tag=B3[10]' '--tag=N7[10]' '--tag=L19[10]' > "$LOG_DIR/micrologix_emulator.log" 2>&1 & } 2>/dev/null
EMULATOR_PID=$!
if [ $EMULATOR_PID -le 0 ]; then
    # echo "FAILURE"
    echo "Unable to start AB/Micrologix emulator!"
    exit 1
# else
    # echo "OK"
fi

sleep 1

let TEST++
echo -n "  Test $TEST: B data file Micrologix tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=uint16 '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=B3:0' --write=0 --debug=4 > "$LOG_DIR/${TEST}_micrologix.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: B bit data file Micrologix tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=B3:0/6' --write=1 --debug=4 > "$LOG_DIR/${TEST}_micrologix.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: N data file Micrologix tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint16 '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=N7:0' --write=42 --debug=4 > "$LOG_DIR/${TEST}_micrologix.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: N bit data file Micrologix tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=N7:0/10' --write=1 --debug=4 > "$LOG_DIR/${TEST}_micrologix.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: L data file Micrologix tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint32 '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=L19:0' --write=0,1,2,3 --debug=4 > "$LOG_DIR/${TEST}_micrologix.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: L bit data file Micrologix tag read... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=L19:0/23' --debug=4 > "$LOG_DIR/${TEST}_micrologix.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi
let TEST++
echo -n "  Test $TEST: L bit data file Micrologix tag write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=L19:0/23' --write=1 --debug=4 > "$LOG_DIR/${TEST}_micrologix.log" 2>&1
if [ $? == 0 ]; then    # This should _NOT_ succeed
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


echo "  Killing Micrologix emulator."
kill_process ab_server



echo "Starting AB emulator for PLC5 tests."
{ $TEST_DIR/ab_server --debug --plc=PLC/5 '--tag=B3[10]' '--tag=N7[10]' > "$LOG_DIR/plc5_emulator.log" 2>&1 & } 2>/dev/null
EMULATOR_PID=$!
if [ $EMULATOR_PID -le 0 ]; then
    # echo "FAILURE"
    echo "Unable to start AB/PLC5 emulator!"
    exit 1
# else
    # echo "OK"
fi

sleep 1


let TEST++
echo -n "  Test $TEST: B data file PLC5 tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=uint16 '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=plc5&elem_count=1&name=B3:0' --debug=4 --write=0 > "$LOG_DIR/${TEST}_plc5.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: B bit data file PLC5 tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=plc5&elem_count=1&name=B3:0/10' --debug=4 --write=1 > "$LOG_DIR/${TEST}_plc5.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: N data file PLC5 tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint16 '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=plc5&elem_count=1&name=N7:0' --debug=4 --write=0 > "$LOG_DIR/${TEST}_plc5.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: N bit data file PLC5 tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=plc5&elem_count=1&name=N7:0/10' --debug=4 --write=1 > "$LOG_DIR/${TEST}_plc5.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

echo "  Killing emulators."
kill_process ab_server

kill_process modbus_server

# wait for them to exit
sleep 2

echo "Starting Modbus server $SCRIPT_DIR/modbus_server."
$TEST_DIR/modbus_server --listen=127.0.0.1:1502 --listen=127.0.0.1:2502 --debug=DETAIL > "$LOG_DIR/modbus_server.log" 2>&1 &
MODBUS_PID=$!
if [ $MODBUS_PID -le 0 ]; then
    # echo "FAILURE"
    echo "Unable to start Modbus emulator!"
    exit 1
else
    # sleep to let the server start up all the way
    sleep 3
    if ! kill -0 "$MODBUS_PID" > /dev/null 2>&1; then
        echo "Modbus server process exited during startup!"
        wait "$MODBUS_PID" 2>/dev/null
        echo "Modbus server exit code: $?"
        dump_binary_diagnostics "$TEST_DIR/modbus_server"
        if [[ -f "$LOG_DIR/modbus_server.log" ]]; then
            echo "--- modbus_server.log (tail) ---"
            tail -n 200 "$LOG_DIR/modbus_server.log"
            echo "--- end modbus_server.log ---"
        fi
        exit 1
    fi
    # echo "Modbus server started"
fi

let TEST++
echo -n "  Test $TEST: connection tag connection state transitions (Modbus)... "
$VALGRIND$TEST_DIR/test_connection_tag "--tag=protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&name=@connection" > "$LOG_DIR/${TEST}_connection_tag_modbus_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: multiple simultaneous @connection tags on same session (Modbus)... "
$VALGRIND$TEST_DIR/test_connection_tag \
    "--tag=protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&name=@connection" \
    --num-tags=3 \
    "--data-tag=protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&elem_count=2&name=hr10" > "$LOG_DIR/${TEST}_connection_tag_modbus_multi_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: @connection tag 2-cycle reconnect (5 s idle timeout, Modbus)... "
$VALGRIND$TEST_DIR/test_connection_tag \
    "--tag=protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&name=@connection" \
    --cycles=2 \
    "--data-tag=protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&elem_count=2&name=hr10" \
    --idle-timeout-ms=5000 > "$LOG_DIR/${TEST}_connection_tag_modbus_cycle_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: test idle disconnect with Modbus... "
$VALGRIND$TEST_DIR/test_idle_disconnect "--tag=protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&elem_count=2&name=hr10" > "$LOG_DIR/${TEST}_modbus_idle_disconnect_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: thread stress Modbus... "
$VALGRIND$TEST_DIR/thread_stress 10 'protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&elem_count=2&name=hr10' > "$LOG_DIR/${TEST}_modbus_stress_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


# if the OS is Darwin, set the ulimits higher for the connection stress test
if [[ "$OSTYPE" == "darwin"* ]]; then
    echo "        Setting ulimit for open files to 1024 for connection stress test."
    ulimit -n 1024
fi

let TEST++
echo -n "  Test $TEST: connection stress (multiple connections) Modbus... "
$VALGRIND$TEST_DIR/test_connection_stress --num-threads=200 --tag='protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&elem_count=2&name=hr10' > "$LOG_DIR/${TEST}_modbus_connection_stress_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "  Test $TEST: callback events Modbus... "
$VALGRIND$TEST_DIR/test_callback_ex_modbus > "$LOG_DIR/${TEST}_test_callback_ex_modbus.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "  Test $TEST: @connection tag late join (Modbus)... "
$VALGRIND$TEST_DIR/test_connection_tag_late_join \
    "--data-tag=protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&elem_count=2&name=hr10" \
    "--tag=protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&name=@connection" \
    --timeout=5000 > "$LOG_DIR/${TEST}_connection_tag_late_join_modbus_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: create-from-tag API (17 permutation tests with Modbus)... "
$VALGRIND$TEST_DIR/test_create_from_tag \
    "--src-tag=protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&elem_count=2&name=hr10" \
    "--clone-attrib=name=hr20&elem_count=2" \
    "--connection-tag=protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&name=@connection" \
    --timeout=10000 > "$LOG_DIR/${TEST}_create_from_tag_modbus_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "  Test $TEST: hard library shutdown... "
$VALGRIND$TEST_DIR/test_shutdown_modbus > "$LOG_DIR/${TEST}_shutdown.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "  Test $TEST: Modbus tag scheduling fairness... "
$VALGRIND$TEST_DIR/test_fairness "--tag=protocol=modbus-tcp&gateway=127.0.0.1:1502&path=1&name=hr10&auto_sync_read_ms=200" --num-tags=200 --test-duration-secs=10 > "$LOG_DIR/${TEST}_modbus_fairness_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "  Test $TEST: for Modbus reconnect bug... "
TST_LOG="$LOG_DIR/${TEST}_modbus_reconnect_bug_test.log"
$VALGRIND$TEST_DIR/test_modbus_multiple > "${TST_LOG}" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


# Check that exactly 2 PLC objects were created during test 29.
# This validates proper PLC object reuse and no spurious creation/destruction.
let TEST++
echo -n "  Test $TEST: check for exactly 2 PLC creation entries in Modbus reconnect test log... "
PLC_COUNT=$(grep -c "Creating new PLC connection\." ${TST_LOG})
if [ "${PLC_COUNT}" = "2" ] ; then
    echo "OK (found ${PLC_COUNT} PLC creation entries in log file ${TST_LOG})"
    let SUCCESSES++
else
    echo "FAILURE (expected 2 PLC creation entries, found ${PLC_COUNT} in log file ${TST_LOG})"
    let FAILURES++
fi

# Let the server dump stats at least one more time
sleep 2

echo "Killing Modbus emulator."

kill_process modbus_server

# wait for them to exit
sleep 2

# show results
echo ""
echo "Results:"
echo " - $TEST tests."
echo " - $SUCCESSES successes."
echo " - $FAILURES failures."

exit $FAILURES

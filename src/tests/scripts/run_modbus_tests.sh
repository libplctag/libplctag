#!/bin/bash

TEST_DIR=$1
LOG_DIR=${2:-.}  # Default to current directory if not specified

# Debug: show what we received
echo "Received TEST_DIR: $TEST_DIR"
echo "Received LOG_DIR: $LOG_DIR"
echo "OSTYPE: $OSTYPE"

# Convert Windows paths to Unix paths if running on Windows (Git Bash)
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "win32" ]]; then
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
    # Find process by name using ps and send SIGTERM for graceful shutdown
    local pids=$(ps -W | grep -i "${process_name}" | grep -v grep | awk '{print $1}' || true)
    if [[ -n "$pids" ]]; then
        # Send SIGTERM to allow graceful shutdown and statistics printing
        for pid in $pids; do
            kill -TERM $pid > /dev/null 2>&1
        done
        # Give process time to shut down gracefully and print statistics
        sleep 3
        # Force kill any remaining processes
        for pid in $pids; do
            kill -KILL $pid > /dev/null 2>&1
        done
    fi
}

if [[ ! -d $TEST_DIR ]]; then
    # echo "Using $TEST_DIR for test executables."
# else
    echo "$TEST_DIR is not a valid path for test executables!"
    exit 1
fi

# test for the executables.
EXECUTABLES="ab_server list_tags_logix modbus_server string_non_standard_udt string_standard tag_rw2 test_connection_stress test_fairness test_auto_sync test_callback test_callback_ex test_callback_ex_logix test_callback_ex_modbus test_modbus_multiple test_raw_cip test_reconnect_after_outage_async test_reconnect_after_outage_sync test_shutdown_cip test_shutdown_modbus test_shutdown_restart test_special test_string test_tag_attributes test_tag_type_attribute thread_stress"
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


echo "Killing any existing Modbus emulator processes."

kill_process modbus_server

# wait for them to exit
sleep 2


echo "Phase 1: Modbus server $SCRIPT_DIR/modbus_server."

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
    # echo "Modbus server started"
fi

let TEST++
echo -n "  Test $TEST: test short reconnect with Modbus... "
$VALGRIND$TEST_DIR/test_reconnect 3 > "$LOG_DIR/${TEST}_modbus_reconnect_short_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: test long reconnect with Modbus... "
$VALGRIND$TEST_DIR/test_reconnect 15 > "$LOG_DIR/${TEST}_modbus_reconnect_long_test.log" 2>&1
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

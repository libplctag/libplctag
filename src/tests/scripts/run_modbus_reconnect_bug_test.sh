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
    if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "win32" ]]; then
        # Windows (Git Bash)
        taskkill //F //IM "${process_name}.exe" > /dev/null 2>&1
    else
        # Linux/macOS/Alpine - pkill has consistent syntax across platforms
        pkill -TERM "$process_name" > /dev/null 2>&1
    fi
}

if [[ ! -d $TEST_DIR ]]; then
    # echo "Using $TEST_DIR for test executables."
# else
    echo "$TEST_DIR is not a valid path for test executables!"
    exit 1
fi

# test for the executables.
EXECUTABLES="ab_server list_tags_logix string_non_standard_udt string_standard tag_rw2 test_fairness test_auto_sync test_callback test_callback_ex test_callback_ex_logix test_callback_ex_modbus test_modbus_multiple test_raw_cip test_reconnect_after_outage_async test_reconnect_after_outage_sync test_shutdown_cip test_shutdown_modbus test_special test_string test_tag_attributes test_tag_type_attribute thread_stress"
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



kill_process modbus_server_coro

# wait for it to exit
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
    # echo "Modbus server started"
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

# echo "  Killing Modbus emulator."
kill_process modbus_server_coro

# wait for it to exit
sleep 2



echo ""
echo "Results:"
echo " - $TEST tests."
echo " - $SUCCESSES successes."
echo " - $FAILURES failures."

exit $FAILURES

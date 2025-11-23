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
        # Linux/macOS
        killall -TERM "$process_name" > /dev/null 2>&1
    fi
}

if [[ ! -d $TEST_DIR ]]; then
    # echo "Using $TEST_DIR for test executables."
# else
    echo "$TEST_DIR is not a valid path for test executables!"
    exit 1
fi

# test for the executables.
EXECUTABLES="ab_server modbus_server test_fairness"
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

echo ""
echo "=========================================="
echo "AB/ControlLogix Fairness Test"
echo "=========================================="
echo ""

# Make sure no ab_server instances are running before starting tests
echo "Terminating any existing AB emulator instances."
kill_process ab_server

echo "Starting AB emulator for ControlLogix tests."
{ $TEST_DIR/ab_server --debug --plc=ControlLogix --path=1,0 "--tag=TestBigArray:DINT[2000]" "--tag=Test_Array_1:DINT[1000]" "--tag=Test_Array_2x3:DINT[2,3]" "--tag=Test_Array_2x3x4:DINT[2,3,4]" > "$LOG_DIR/ab_server.log" 2>&1 & } 2>/dev/null
EMULATOR_PID=$!
if [ $EMULATOR_PID -le 0 ]; then
    echo "Unable to start AB/ControlLogix emulator!"
    exit 1
fi

sleep 3

let TEST++
echo -n "  Test $TEST: AB fairness test with 200 tags for 10 seconds ... "
$VALGRIND$TEST_DIR/test_fairness "--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=TestBigArray[0]&auto_sync_read_ms=200" --num-tags=200 --test-duration-secs=10 > "$LOG_DIR/${TEST}_ab_fairness.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

# Make sure no ab_server instances are running before starting Modbus tests
echo "Stopping AB emulator."
kill_process ab_server

# wait for it to exit
sleep 2

echo ""
echo "=========================================="
echo "Modbus Fairness Test"
echo "=========================================="
echo ""

# Make sure no modbus_server instances are running before starting tests
echo "Terminating any existing Modbus server instances."
kill_process modbus_server

echo "Starting Modbus server."
$TEST_DIR/modbus_server --listen=127.0.0.1:1502 --listen=127.0.0.1:2502 --debug=DETAIL > "$LOG_DIR/modbus_server.log" 2>&1 &
MODBUS_PID=$!
if [ $MODBUS_PID -le 0 ]; then
    echo "Unable to start Modbus server!"
    exit 1
fi

sleep 3

let TEST++
echo -n "  Test $TEST: Modbus fairness test with 200 tags for 30 seconds ... "
$VALGRIND$TEST_DIR/test_fairness "--tag=protocol=modbus-tcp&gateway=127.0.0.1:1502&path=1&name=hr10&auto_sync_read_ms=200" --num-tags=100 --test-duration-secs=10 > "$LOG_DIR/${TEST}_modbus_fairness_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

# Make sure no modbus_server instances are running after tests
echo "Stopping Modbus server."
kill_process modbus_server

# wait for it to exit
sleep 2

echo ""
echo "=========================================="
echo "Results:"
echo "=========================================="
echo " - $TEST tests."
echo " - $SUCCESSES successes."
echo " - $FAILURES failures."

exit $FAILURES

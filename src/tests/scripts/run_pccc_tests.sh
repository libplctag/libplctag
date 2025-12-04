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


echo "  Killing AB emulator."
kill_process ab_server

sleep 2

echo "   AB emulator processes killed."


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
$VALGRIND$TEST_DIR/tag_rw2 --type=sint32 '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=L10:0' --write=0,1,2,3 --debug=4 > "$LOG_DIR/${TEST}_micrologix.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: L bit data file Micrologix tag read... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=L10:0/23' --debug=4 > "$LOG_DIR/${TEST}_micrologix.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi
let TEST++
echo -n "  Test $TEST: L bit data file Micrologix tag write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=L10:0/23' --write=1 --debug=4 > "$LOG_DIR/${TEST}_micrologix.log" 2>&1
if [ $? == 0 ]; then    # This should _NOT_ succeed
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


echo "  Killing Micrologix emulator."
kill_process ab_server

sleep 2

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

echo "  Killing emulator."
kill_process ab_server

# wait for emulator to exit
sleep 2


echo ""
echo "Results:"
echo " - $TEST tests."
echo " - $SUCCESSES successes."
echo " - $FAILURES failures."

exit $FAILURES

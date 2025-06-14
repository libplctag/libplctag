#!/bin/bash

TEST_DIR=$1

# thanks to Stack Overflow
SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"

TEST=0
SUCCESSES=0
FAILURES=0

# for deeper testing
# VALGRIND="valgrind --tool=memcheck --track-origins=yes --leak-check=full --show-leak-kinds=all --error-exitcode=1  "
VALGRIND=""

if [[ ! -d $TEST_DIR ]]; then
    # echo "Using $TEST_DIR for test executables."
# else
    echo "$TEST_DIR is not a valid path for test executables!"
    exit 1
fi

# test for the executables.
EXECUTABLES="ab_server list_tags_logix string_non_standard_udt string_standard tag_rw2 test_auto_sync test_callback test_callback_ex test_callback_ex_logix test_callback_ex_modbus test_raw_cip test_reconnect_after_outage test_shutdown test_special test_string test_tag_attributes test_tag_type_attribute thread_stress"
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


echo "Starting AB emulator for fast ControlLogix tests."
$TEST_DIR/ab_server --debug --plc=ControlLogix --path=1,0 "--tag=TestBigArray:DINT[2000]" "--tag=Test_Array_1:DINT[1000]" "--tag=Test_Array_2x3:DINT[2,3]" "--tag=Test_Array_2x3x4:DINT[2,3,4]" > logix_fast_emulator.log 2>&1 &
EMULATOR_PID=$!
if [ $? != 0 ]; then
    echo "Unable to start AB/ControlLogix emulator!"
    exit 1
fi

sleep 1

let TEST++
echo -n "Test $TEST: basic large tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint32 '--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=1000&name=TestBigArray' --debug=4 --write=1,2,3,4,5,6,7,8,9 > "${TEST}_big_tag_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: stress RC memory code ... "
$VALGRIND$TEST_DIR/stress_rc_mem > "${TEST}_stress_rc_mem_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: CIP thread stress... "
$TEST_DIR/thread_stress 20 "protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=TestBigArray" > "${TEST}_thread_stress_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "Test $TEST: auto sync... "
$VALGRIND$TEST_DIR/test_auto_sync > "${TEST}_auto_sync_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "Test $TEST: indexed tags ... "
$VALGRIND$TEST_DIR/test_indexed_tags > "${TEST}_test_indexed_tags.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "Test $TEST: hard library shutdown... "
$VALGRIND$TEST_DIR/test_shutdown > "${TEST}_shutdown.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


# echo "  Killing AB emulator."
killall -TERM ab_server > /dev/null 2>&1

let TEST++
echo -n "Test $TEST: Test reconnect after PLC outage... "
$VALGRIND$TEST_DIR/test_reconnect_after_outage "${TEST_DIR}/ab_server" > "${TEST}_reconnect_after_outage.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


echo "Starting AB emulator for functional/slow ControlLogix tests."
$VALGRIND$TEST_DIR/ab_server --plc=ControlLogix --path=1,0 "--tag=TestBigArray:DINT[2000]" "--tag=Test_Array_1:DINT[1000]" "--tag=Test_Array_2x3:DINT[2,3]" "--tag=Test_Array_2x3x4:DINT[2,3,4]" --delay=20  > logix_slow_emulator.log 2>&1 &
EMULATOR_PID=$!
if [ $? != 0 ]; then
    echo "Unable to start AB/ControlLogix emulator!"
    exit 1
fi

sleep 1


let TEST++
echo -n "Test $TEST: emulator test callbacks... "
$VALGRIND$TEST_DIR/test_callback > "${TEST}_callback_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "Test $TEST: emulator test extended callbacks sync... "
$VALGRIND$TEST_DIR/test_callback_ex > "${TEST}_extended_callback_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "Test $TEST: emulator test extended callbacks async... "
$VALGRIND$TEST_DIR/test_callback_ex_logix > "${TEST}_extended_callback_async_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


echo "  Killing AB emulator."
killall -TERM ab_server > /dev/null 2>&1


echo "Starting AB emulator for Micro800 tests."
$TEST_DIR/ab_server --debug --plc=Micro800 --tag=TestDINTArray:DINT[10] > micro800_emulator.log 2>&1 &
EMULATOR_PID=$!
if [ $? != 0 ]; then
    # echo "FAILURE"
    echo "Unable to start Micro800 emulator!"
    exit 1
# else
    # echo "OK"
fi

sleep 1


let TEST++
echo -n "Test $TEST: basic Micro800 read/write... "
$VALGRIND$TEST_DIR/./tag_rw2 --type=sint32  '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micro800&name=TestDINTArray' --write=42 --debug=4 > "${TEST}_micro800_tag_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


echo "  Killing Micro800 emulator."
killall -TERM ab_server > /dev/null 2>&1


echo "Starting AB emulator for Omron tests."
$TEST_DIR/ab_server --debug --plc=Omron --tag=TestDINTArray:DINT[10] > omron_emulator.log 2>&1 &
EMULATOR_PID=$!
if [ $? != 0 ]; then
    # echo "FAILURE"
    echo "Unable to start AB/Omron emulator!"
    exit 1
# else
    # echo "OK"
fi

sleep 1


let TEST++
echo -n "Test $TEST: basic Omron read/write... "
$VALGRIND$TEST_DIR/./tag_rw2 --type=sint32  '--tag=protocol=ab-eip&gateway=127.0.0.1&path=18,127.0.0.1&plc=omron-njnx&name=TestDINTArray' --write=42 --debug=4 > "${TEST}_omron_tag_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

echo "Killing Omron emulator."
killall -TERM ab_server > /dev/null 2>&1


echo ""
echo "$TEST tests."
echo "$SUCCESSES successes."
echo "$FAILURES failures."

if [ $FAILURES == 0 ]; then
    exit 0
else
    exit 1
fi

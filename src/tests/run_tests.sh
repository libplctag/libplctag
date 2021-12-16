#!/bin/bash

TEST_DIR=$1

# thanks to Stack Overflow
SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"

TEST=0
SUCCESSES=0
FAILURES=0

if [[ ! -d $TEST_DIR ]]; then
    # echo "Using $TEST_DIR for test executables."
# else
    echo "$TEST_DIR is not a valid path for test executables!"
    exit 1
fi

# test for the executables.
EXECUTABLES="ab_server tag_rw2 list_tags thread_stress test_callback"
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

let TEST++
echo -n "Test $TEST: basic large tag read/write... "
$TEST_DIR/tag_rw2 --type=sint32 '--tag=protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_count=1000&name=TestBigArray' --debug=4 --write=1,2,3,4,5,6,7,8,9 > big_tag_test.log 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: basic Micrologix tag read/write... "
$TEST_DIR/tag_rw2 --type=uint8 '--tag=protocol=ab-eip&gateway=10.206.1.36&plc=micrologix&elem_count=1&elem_size=2&name=N7:0/14' --write=0 --debug=4 > micrologix.log 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: basic PLC-5 tag read/write... "
$TEST_DIR/tag_rw2 --type=uint8 '--tag=protocol=ab-eip&gateway=10.206.1.38&plc=plc5&elem_count=1&elem_size=2&name=B3:0/10' --debug=4 --write=0 > plc5.log 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "Test $TEST: basic DH+ bridging... "
$TEST_DIR/tag_rw2 --type=uint8 '--tag=protocol=ab_eip&gateway=10.206.1.40&path=1,2,A:27:1&cpu=plc5&elem_count=1&elem_size=2&name=B3:0/10' --debug=4 --write=0  > dhp_bridge.log 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "Test $TEST: basic CIP bridging... "
$TEST_DIR//tag_rw2 --type=sint32 '--tag=protocol=ab_eip&gateway=10.206.1.39&path=1,6,18,10.206.1.40,1,4&plc=lgx&name=TestBigArray[0]' --debug=4 --write=5 > cip_bridge.log 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "Test $TEST: tag listing... "
$TEST_DIR/list_tags "10.206.1.40" "1,4" > list_tags_test.log 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "Test $TEST: CIP thread stress... "
$TEST_DIR/thread_stress 20 "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&name=TestBigArray" > thread_stress_test.log 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


# echo "  Doing tests that need the local emulator."

# echo -n "  Starting AB emulator for ControlLogix tests... "
$TEST_DIR/ab_server --plc=ControlLogix --path=1,0 --tag=TestBigArray:DINT[2000] --delay=5  > ab_emulator.log 2>&1 &
EMULATOR_PID=$!
if [ $? != 0 ]; then
    # echo "FAILURE"
    echo "Unable to start AB/ControlLogix emulator!"
    exit 1
# else
    # echo "OK"
fi


let TEST++
echo -n "Test $TEST: emulator test callbacks... "
$TEST_DIR/test_callback > callback_test.log 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


# echo "  Killing AB emulator."
killall -TERM ab_server > /dev/null 2>&1


# echo -n "  Starting AB emulator for Omron tests... "
$TEST_DIR/ab_server --debug --plc=Omron --tag=TestDINTArray:DINT[10] > omron_emulator.log 2>&1 &
EMULATOR_PID=$!
if [ $? != 0 ]; then
    # echo "FAILURE"
    echo "Unable to start AB/Omron emulator!"
    exit 1
# else
    # echo "OK"
fi


let TEST++
echo -n "Test $TEST: basic Omron read/write... "
$TEST_DIR/./tag_rw2 --type=sint32  '--tag=protocol=ab-eip&gateway=127.0.0.1&path=18,127.0.0.1&plc=omron-njnx&name=TestDINTArray' --write=42 --debug=4 > omron_tag_test.log 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


# echo "  Killing Omron emulator."
killall -TERM ab_server > /dev/null 2>&1


# echo -n "  Starting Modbus emulator... "
$SCRIPT_DIR/modbus_server.py > modbus_emulator.log 2>&1 &
MODBUS_PID=$!
if [ $? != 0 ]; then
    # echo "FAILURE"
    echo "Unable to start Modbus emulator!"
    exit 1
else
    # sleep to let the emulator start up all the way
    sleep 2
    # echo "OK"
fi

let TEST++
echo -n "Test $TEST: thread stress Modbus... "
$TEST_DIR/thread_stress 10 'protocol=modbus-tcp&gateway=127.0.0.1:5020&path=0&elem_count=2&name=hr10' > modbus_test.log 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

# echo "  Killing Modbus emulator."
kill -TERM $MODBUS_PID > /dev/null 2>&1

echo ""
echo "$TEST tests."
echo "$SUCCESSES successes."
echo "$FAILURES failures."

if [ $FAILURES == 0 ]; then
    exit 0
else
    exit 1
fi

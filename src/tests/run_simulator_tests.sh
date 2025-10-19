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
EXECUTABLES="ab_server list_tags_logix string_non_standard_udt string_standard tag_rw2 test_auto_sync test_callback test_callback_ex test_callback_ex_logix test_callback_ex_modbus test_modbus_multiple test_raw_cip test_reconnect_after_outage test_shutdown test_special test_string test_tag_attributes test_tag_type_attribute thread_stress"
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
echo -n "  Test $TEST: basic large tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint32 '--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=1000&name=TestBigArray' --debug=4 --write=1,2,3,4,5,6,7,8,9 > "${TEST}_big_tag_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: stress RC memory code ... "
$VALGRIND$TEST_DIR/stress_rc_mem > "${TEST}_stress_rc_mem_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: CIP thread stress... "
$TEST_DIR/thread_stress 20 "protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=TestBigArray" > "${TEST}_thread_stress_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "  Test $TEST: auto sync... "
$VALGRIND$TEST_DIR/test_auto_sync > "${TEST}_auto_sync_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "  Test $TEST: indexed tags ... "
$VALGRIND$TEST_DIR/test_indexed_tags > "${TEST}_test_indexed_tags.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "  Test $TEST: hard library shutdown... "
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

echo "Starting stand-alone tests."

let TEST++
echo -n "  Test $TEST: Test reconnect after PLC outage... "
$VALGRIND$TEST_DIR/test_reconnect_after_outage "${TEST_DIR}/ab_server" > "${TEST}_reconnect_after_outage.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


echo "Starting AB emulator for functional/slow ControlLogix tests."
$VALGRIND$TEST_DIR/ab_server --plc=ControlLogix --path=1,0 "--tag=TestBigArray:DINT[2000]" "--tag=Test_Array_1:DINT[1000]" "--tag=Test_Array_2x3:DINT[2,3]" "--tag=Test_Array_2x3x4:DINT[2,3,4]" --delay=50  > logix_slow_emulator.log 2>&1 &
EMULATOR_PID=$!
if [ $? != 0 ]; then
    echo "Unable to start AB/ControlLogix emulator!"
    exit 1
fi

sleep 1


let TEST++
echo -n "  Test $TEST: emulator test callbacks... "
$VALGRIND$TEST_DIR/test_callback > "${TEST}_callback_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "  Test $TEST: emulator test extended callbacks sync... "
$VALGRIND$TEST_DIR/test_callback_ex > "${TEST}_extended_callback_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "  Test $TEST: emulator test extended callbacks async... "
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
echo -n "  Test $TEST: basic Micro800 read/write... "
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
echo -n "  Test $TEST: basic Omron read/write... "
$VALGRIND$TEST_DIR/./tag_rw2 --type=sint32  '--tag=protocol=ab-eip&gateway=127.0.0.1&path=18,127.0.0.1&plc=omron-njnx&name=TestDINTArray' --write=42 --debug=4 > "${TEST}_omron_tag_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

echo "  Killing Omron emulator."
killall -TERM ab_server > /dev/null 2>&1


echo "Starting AB emulator for Micrologix tests."
$TEST_DIR/ab_server --debug --plc=Micrologix '--tag=B3[10]' '--tag=N7[10]' '--tag=L19[10]' > micrologix_emulator.log 2>&1 &
EMULATOR_PID=$!
if [ $? != 0 ]; then
    # echo "FAILURE"
    echo "Unable to start AB/Micrologix emulator!"
    exit 1
# else
    # echo "OK"
fi

sleep 1

let TEST++
echo -n "  Test $TEST: B data file Micrologix tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=uint16 '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=B3:0' --write=0 --debug=4 > "${TEST}_micrologix.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: B bit data file Micrologix tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=B3:0/6' --write=1 --debug=4 > "${TEST}_micrologix.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: N data file Micrologix tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint16 '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=N7:0' --write=42 --debug=4 > "${TEST}_micrologix.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: N bit data file Micrologix tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=N7:0/10' --write=1 --debug=4 > "${TEST}_micrologix.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: L data file Micrologix tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint32 '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=L10:0' --write=0,1,2,3 --debug=4 > "${TEST}_micrologix.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: L bit data file Micrologix tag read... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=L10:0/23' --debug=4 > "${TEST}_micrologix.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi
let TEST++
echo -n "  Test $TEST: L bit data file Micrologix tag write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=micrologix&name=L10:0/23' --write=1 --debug=4 > "${TEST}_micrologix.log" 2>&1
if [ $? == 0 ]; then    # This should _NOT_ succeed
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


echo "  Killing Micrologix emulator."
killall -TERM ab_server > /dev/null 2>&1



echo "Starting AB emulator for PLC5 tests."
$TEST_DIR/ab_server --debug --plc=PLC/5 '--tag=B3[10]' '--tag=N7[10]' > plc5_emulator.log 2>&1 &
EMULATOR_PID=$!
if [ $? != 0 ]; then
    # echo "FAILURE"
    echo "Unable to start AB/PLC5 emulator!"
    exit 1
# else
    # echo "OK"
fi

sleep 1


let TEST++
echo -n "  Test $TEST: B data file PLC5 tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=uint16 '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=plc5&elem_count=1&name=B3:0' --debug=4 --write=0 > "${TEST}_plc5.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: B bit data file PLC5 tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=plc5&elem_count=1&name=B3:0/10' --debug=4 --write=1 > "${TEST}_plc5.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: N data file PLC5 tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint16 '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=plc5&elem_count=1&name=N7:0' --debug=4 --write=0 > "${TEST}_plc5.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: N bit data file PLC5 tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=127.0.0.1&plc=plc5&elem_count=1&name=N7:0/10' --debug=4 --write=1 > "${TEST}_plc5.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

echo "  Killing emulators."
killall -TERM ab_server > /dev/null 2>&1

killall -TERM modbus_server > /dev/null 2>&1

# wait for them to exit
sleep 2

# echo -n "  Starting Modbus server $SCRIPT_DIR/modbus_server... "
$TEST_DIR/modbus_server --listen 127.0.0.1:1502 --listen 127.0.0.1:2502 > modbus_server.log 2>&1 &
MODBUS_PID=$!
if [ $? != 0 ]; then
    # echo "FAILURE"
    echo "Unable to start Modbus emulator!"
    exit 1
else
    # sleep to let the server start up all the way
    sleep 2
    # echo "Modbus server started"
fi

let TEST++
echo -n "Test $TEST: test short reconnect with Modbus... "
$VALGRIND$TEST_DIR/test_reconnect 3 > "${TEST}_modbus_reconnect_short_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: test long reconnect with Modbus... "
$VALGRIND$TEST_DIR/test_reconnect 10 > "${TEST}_modbus_reconnect_long_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "Test $TEST: thread stress Modbus... "
$VALGRIND$TEST_DIR/thread_stress 10 'protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&elem_count=2&name=hr10' > "${TEST}_modbus_stress_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "Test $TEST: callback events Modbus... "
$VALGRIND$TEST_DIR/test_callback_ex_modbus > "${TEST}_test_callback_ex_modbus.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: for Modbus reconnect bug... "
$VALGRIND$TEST_DIR/test_modbus_multiple > "${TEST}_test_modbus_multiple.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

# make sure that there is no thread(5) in the log file.
let TEST++
echo -n "Test $TEST: check for thread(5) in Modbus multiple test log... "
if grep -q "thread(5)" "${TEST}_test_modbus_multiple.log" ; then
    echo "FAILURE (found thread(5) in log file)"
    let FAILURES++
else
    echo "OK (no thread(5) found in log file)"
    let SUCCESSES++
fi

# echo "  Killing Modbus emulator."
killall -TERM modbus_server > /dev/null 2>&1

# Make sure no ab_server instances are running before running auto_sync_reconnect test
killall -TERM ab_server > /dev/null 2>&1

# wait for them to exit
sleep 2



echo ""
echo "Results:"
echo " - $TEST tests."
echo " - $SUCCESSES successes."
echo " - $FAILURES failures."

exit $FAILURES

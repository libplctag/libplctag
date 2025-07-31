#!/bin/bash

TEST_DIR=$1

# thanks to Stack Overflow
SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"

TEST=0
SUCCESSES=0
FAILURES=0

# limit the number of open files to 1024 to that Valgrind does not complain
ulimit -n 1024

# for deeper testing
# VALGRIND="valgrind --leak-check=full --error-exitcode=1 --track-origins=yes --suppressions=./valgrind.supp "
VALGRIND=""

if [[ ! -d $TEST_DIR ]]; then
    # echo "Using $TEST_DIR for test executables."
# else
    echo "$TEST_DIR is not a valid path for test executables!"
    exit 1
fi

# test for the executables.
EXECUTABLES="ab_server list_tags_logix modbus_server string_non_standard_udt string_standard tag_rw2 test_auto_sync test_reconnect_after_outage test_callback test_callback_ex test_callback_ex_logix test_callback_ex_modbus test_raw_cip test_reconnect test_shutdown test_special test_string test_tag_attributes test_tag_type_attribute thread_stress"
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
echo -n "Test $TEST: special tags... "
$VALGRIND$TEST_DIR/test_special > "${TEST}_special_tag_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: tag attributes... "
$VALGRIND$TEST_DIR/test_tag_attributes > "${TEST}_tag_attribute_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: tag type byte array attributes... "
$VALGRIND$TEST_DIR/test_tag_type_attribute > "${TEST}_tag_type_attribute_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: test tag_rw2 to get tag metadata ... "
$VALGRIND$TEST_DIR/tag_rw2 --type=metadata '--tag=protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=logix&name=TestBOOLArray' > "${TEST}_tag_rw2_metadata.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: basic large tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint32 '--tag=protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_count=1000&name=TestBigArray' --debug=4 --write=1,2,3,4,5,6,7,8,9 > "${TEST}_big_tag_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: test standard strings... "
$VALGRIND$TEST_DIR/string_standard > "${TEST}_standard_string_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: Get INT bit ... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&name=TestINTArray[0].13' --debug=4 > "${TEST}_get_INT_bit_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: Set INT bit ... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&name=TestINTArray[0].13' --debug=4 --write=1 > "${TEST}_set_INT_bit_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: Get LINT bit ... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&name=TestLINTArray[0].43' --debug=4 > "${TEST}_get_INT_bit_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: Set LINT bit ... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&name=TestLINTArray[0].43' --debug=4 --write=1 > "${TEST}_set_INT_bit_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: basic large tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint32 '--tag=protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_count=1000&name=TestBigArray' --debug=4 --write=1,2,3,4,5,6,7,8,9 > "${TEST}_big_tag_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi




let TEST++
echo -n "Test $TEST: test non-standard UDT strings... "
$VALGRIND$TEST_DIR/string_non_standard_udt > "${TEST}_non_standard_udt_string_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: test non-standard string size... "
$VALGRIND$TEST_DIR/test_string > "${TEST}_string_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: basic Micrologix tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=uint8 '--tag=protocol=ab-eip&gateway=10.206.1.36&plc=micrologix&elem_count=1&elem_size=2&name=N7:0/14' --write=0 --debug=4 > "${TEST}_micrologix.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: basic PLC5 tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=uint8 '--tag=protocol=ab-eip&gateway=10.206.1.38&plc=plc5&elem_count=1&elem_size=2&name=B3:0/10' --debug=4 --write=0 > "${TEST}_plc5.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "Test $TEST: basic DH+ bridging... "
$VALGRIND$TEST_DIR/tag_rw2 --type=uint8 '--tag=protocol=ab_eip&gateway=10.206.1.40&path=1,2,A:27:1&cpu=plc5&elem_count=1&elem_size=2&name=B3:0/10' --debug=4 --write=0  > "${TEST}_dhp_bridge.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "Test $TEST: basic CIP bridging... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint32 '--tag=protocol=ab_eip&gateway=10.206.1.37&path=1,4,18,10.206.1.39,1,0&plc=lgx&name=TestBigArray[0]' --debug=4 --write=5 > "${TEST}_cip_bridge.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "Test $TEST: raw cip tag... "
$VALGRIND$TEST_DIR/test_raw_cip > "${TEST}_raw_cip_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "Test $TEST: tag listing... "
$VALGRIND$TEST_DIR/list_tags_logix "10.206.1.40" "1,4" > "${TEST}_list_tags_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi



# echo -n "  Starting AB emulator for fast ControlLogix tests... "
$TEST_DIR/ab_server --debug --plc=ControlLogix --path=1,0 "--tag=TestBigArray:DINT[2000]" "--tag=Test_Array_1:DINT[1000]" "--tag=Test_Array_2x3:DINT[2,3]" "--tag=Test_Array_2x3x4:DINT[2,3,4]" > logix_fast_emulator.log 2>&1 &
EMULATOR_PID=$!
if [ $? != 0 ]; then
    echo "Unable to start AB/ControlLogix emulator!"
    exit 1
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
$TEST_DIR/thread_stress 20 "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&name=TestBigArray" > "${TEST}_thread_stress_test.log" 2>&1
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


# auto sync reconnect test will be run at the end because it manages its own ab_server


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


# echo -n "  Starting AB emulator for functional/slow ControlLogix tests... "
$VALGRIND$TEST_DIR/ab_server --plc=ControlLogix --path=1,0 "--tag=TestBigArray:DINT[2000]" "--tag=Test_Array_1:DINT[1000]" "--tag=Test_Array_2x3:DINT[2,3]" "--tag=Test_Array_2x3x4:DINT[2,3,4]" --delay=20  > logix_slow_emulator.log 2>&1 &
EMULATOR_PID=$!
if [ $? != 0 ]; then
    echo "Unable to start AB/ControlLogix emulator!"
    exit 1
fi


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


# echo "  Killing AB emulator."
killall -TERM ab_server > /dev/null 2>&1


# echo -n "  Starting AB emulator for Micro800 tests... "
$TEST_DIR/ab_server --debug --plc=Micro800 --tag=TestDINTArray:DINT[10] > micro800_emulator.log 2>&1 &
EMULATOR_PID=$!
if [ $? != 0 ]; then
    # echo "FAILURE"
    echo "Unable to start Micro800 emulator!"
    exit 1
# else
    # echo "OK"
fi


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


# echo "  Killing Micro800 emulator."
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
$VALGRIND$TEST_DIR/./tag_rw2 --type=sint32  '--tag=protocol=ab-eip&gateway=127.0.0.1&path=18,127.0.0.1&plc=omron-njnx&name=TestDINTArray' --write=42 --debug=4 > "${TEST}_omron_tag_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

# echo "  Killing Omron emulator."
killall -TERM ab_server > /dev/null 2>&1



# echo -n "  Starting Modbus server $SCRIPT_DIR/modbus_server... "
$TEST_DIR/modbus_server > modbus_emulator.log 2>&1 &
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
$VALGRIND$TEST_DIR/thread_stress 10 'protocol=modbus-tcp&gateway=127.0.0.1:5020&path=0&elem_count=2&name=hr10' > "${TEST}_modbus_stress_test.log" 2>&1
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

# echo "  Killing Modbus emulator."
killall -TERM modbus_server > /dev/null 2>&1

# Make sure no ab_server instances are running before running auto_sync_reconnect test
killall -TERM ab_server > /dev/null 2>&1
sleep 2

# Run auto_sync_reconnect test last since it manages its own ab_server
let TEST++
echo -n "Test $TEST: auto sync reconnect... "
$VALGRIND$TEST_DIR/test_reconnect_after_outage "${TEST_DIR}/ab_server" > "${TEST}_auto_sync_reconnect_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

echo ""
echo "$TEST tests."
echo "$SUCCESSES successes."
echo "$FAILURES failures."

if [ $FAILURES == 0 ]; then
    exit 0
else
    exit 1
fi

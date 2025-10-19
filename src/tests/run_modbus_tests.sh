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


echo "  Killing emulators."
killall -TERM ab_server > /dev/null 2>&1

killall -TERM modbus_server > /dev/null 2>&1

# wait for them to exit
sleep 2

# echo -n "  Starting Modbus server $SCRIPT_DIR/modbus_server... "
$TEST_DIR/modbus_server --listen 127.0.0.1:1502 --listen 127.0.0.1:2502 --debug > modbus_server.log 2>&1 &
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

# show results
echo ""
echo "Results:"
echo " - $TEST tests."
echo " - $SUCCESSES successes."
echo " - $FAILURES failures."

exit $FAILURES

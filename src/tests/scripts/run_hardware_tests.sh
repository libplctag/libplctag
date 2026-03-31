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
EXECUTABLES="ab_server list_tags_logix modbus_server string_non_standard_udt string_standard tag_rw2 test_auto_sync test_device_tag test_idle_disconnect test_modbus_multiple test_reconnect_after_outage_async test_reconnect_after_outage_sync test_callback test_callback_ex test_callback_ex_logix test_callback_ex_modbus test_raw_cip test_shutdown_cip test_shutdown_modbus test_shutdown_restart test_special test_string test_tag_attributes test_tag_type_attribute thread_stress get_identity"
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
$VALGRIND$TEST_DIR/test_special > "$LOG_DIR/${TEST}_special_tag_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: tag attributes... "
$VALGRIND$TEST_DIR/test_tag_attributes > "$LOG_DIR/${TEST}_tag_attribute_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: tag type byte array attributes... "
$VALGRIND$TEST_DIR/test_tag_type_attribute > "$LOG_DIR/${TEST}_tag_type_attribute_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: test tag_rw2 to get tag metadata ... "
$VALGRIND$TEST_DIR/tag_rw2 --type=metadata '--tag=protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=logix&name=TestBOOLArray' > "$LOG_DIR/${TEST}_tag_rw2_metadata.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: basic large tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint32 '--tag=protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_count=1000&name=TestBigArray' --debug=4 --write=1,2,3,4,5,6,7,8,9 > "$LOG_DIR/${TEST}_big_tag_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: test standard strings... "
$VALGRIND$TEST_DIR/string_standard > "$LOG_DIR/${TEST}_standard_string_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: idle disconnect and reconnect with runtime timeout change (ControlLogix)... "
$VALGRIND$TEST_DIR/test_idle_disconnect "--tag=protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&name=TestBigArray" > "$LOG_DIR/${TEST}_idle_disconnect_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: Get INT bit ... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&name=TestINTArray[0].13' --debug=4 > "$LOG_DIR/${TEST}_get_INT_bit_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: Set INT bit ... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&name=TestINTArray[0].13' --debug=4 --write=1 > "$LOG_DIR/${TEST}_set_INT_bit_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: Get LINT bit ... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&name=TestLINTArray[0].43' --debug=4 > "$LOG_DIR/${TEST}_get_INT_bit_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: Set LINT bit ... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&name=TestLINTArray[0].43' --debug=4 --write=1 > "$LOG_DIR/${TEST}_set_INT_bit_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: test non-standard UDT strings... "
$VALGRIND$TEST_DIR/string_non_standard_udt > "$LOG_DIR/${TEST}_non_standard_udt_string_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: test non-standard string size... "
$VALGRIND$TEST_DIR/test_string > "$LOG_DIR/${TEST}_string_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: B data file Micrologix tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=uint16 '--tag=protocol=ab-eip&gateway=10.206.1.36&plc=micrologix&name=B3:0' --write=0 --debug=4 > "$LOG_DIR/${TEST}_micrologix.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: B bit data file Micrologix tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=10.206.1.36&plc=micrologix&name=B3:0/6' --write=1 --debug=4 > "$LOG_DIR/${TEST}_micrologix.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: N data file Micrologix tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint16 '--tag=protocol=ab-eip&gateway=10.206.1.36&plc=micrologix&name=N7:0' --write=42 --debug=4 > "$LOG_DIR/${TEST}_micrologix.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: N bit data file Micrologix tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=10.206.1.36&plc=micrologix&name=N7:0/10' --write=1 --debug=4 > "$LOG_DIR/${TEST}_micrologix.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: L data file Micrologix tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint32 '--tag=protocol=ab-eip&gateway=10.206.1.36&plc=micrologix&elem_count=4&name=L10:0' --write=0,1,2,3 --debug=4 > "$LOG_DIR/${TEST}_micrologix.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: L bit data file Micrologix tag read... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=10.206.1.36&plc=micrologix&name=L10:0/23' --debug=4 > "$LOG_DIR/${TEST}_micrologix.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: L bit data file Micrologix tag write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=10.206.1.36&plc=micrologix&name=L10:0/23' --write=1 --debug=4 > "$LOG_DIR/${TEST}_micrologix.log" 2>&1
if [ $? == 0 ]; then   # this should NOT succeed
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: B data file PLC5 tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=uint16 '--tag=protocol=ab-eip&gateway=10.206.1.38&plc=plc5&elem_count=1&name=B3:0' --debug=4 --write=0 > "$LOG_DIR/${TEST}_plc5.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: B bit data file PLC5 tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=10.206.1.38&plc=plc5&elem_count=1&name=B3:0/10' --debug=4 --write=1 > "$LOG_DIR/${TEST}_plc5.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: N data file PLC5 tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint16 '--tag=protocol=ab-eip&gateway=10.206.1.38&plc=plc5&elem_count=1&name=N7:0' --debug=4 --write=0 > "$LOG_DIR/${TEST}_plc5.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: N bit data file PLC5 tag read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=bit '--tag=protocol=ab-eip&gateway=10.206.1.38&plc=plc5&elem_count=1&name=N7:0/10' --debug=4 --write=1 > "$LOG_DIR/${TEST}_plc5.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "Test $TEST: basic DH+ bridging... "
$VALGRIND$TEST_DIR/tag_rw2 --type=uint8 '--tag=protocol=ab_eip&gateway=10.206.1.40&path=1,2,A:27:1&cpu=plc5&elem_count=1&elem_size=2&name=N7:0' --debug=4 --write=42  > "$LOG_DIR/${TEST}_dhp_bridge.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "Test $TEST: basic DH+ bridging bit change... "
$VALGRIND$TEST_DIR/tag_rw2 --type=uint8 '--tag=protocol=ab_eip&gateway=10.206.1.40&path=1,2,A:27:1&cpu=plc5&elem_count=1&elem_size=2&name=B3:0/10' --debug=4 --write=0  > "$LOG_DIR/${TEST}_dhp_bridge_bit_change.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "Test $TEST: basic CIP bridging... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint32 '--tag=protocol=ab_eip&gateway=10.206.1.37&path=1,4,18,10.206.1.39,1,0&plc=lgx&name=TestBigArray[0]' --debug=4 --write=5 > "$LOG_DIR/${TEST}_cip_bridge.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "Test $TEST: raw cip tag... "
$VALGRIND$TEST_DIR/test_raw_cip > "$LOG_DIR/${TEST}_raw_cip_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "Test $TEST: tag listing... "
$VALGRIND$TEST_DIR/list_tags_logix "10.206.1.40" "1,4" > "$LOG_DIR/${TEST}_list_tags_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "Test $TEST: generic CIP device identity query... "
$VALGRIND$TEST_DIR/get_identity "--tag=protocol=ab_eip&gateway=10.206.1.40&plc=generic&name=@identity&debug=3" > "$LOG_DIR/${TEST}_get_identity_test.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi


let TEST++
echo -n "Test $TEST: device tag connection state transitions (ControlLogix)... "
$VALGRIND$TEST_DIR/test_device_tag "--tag=protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&name=@device" > "$LOG_DIR/${TEST}_device_tag_test.log" 2>&1
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

exit $FAILURES

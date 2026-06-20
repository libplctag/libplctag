#!/bin/bash

TEST_DIR=$1
LOG_DIR=${2:-.}

is_windows_shell() {
    case "$OSTYPE" in
        msys*|cygwin*|win32*)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

echo "Received TEST_DIR: $TEST_DIR"
echo "Received LOG_DIR: $LOG_DIR"
echo "OSTYPE: $OSTYPE"

if is_windows_shell; then
    TEST_DIR=$(echo "$TEST_DIR" | tr '\\' '/')
    TEST_DIR=$(echo "$TEST_DIR" | sed 's|^\([A-Za-z]\):|/\L\1|')
    echo "After conversion TEST_DIR: $TEST_DIR"
    if [[ "$LOG_DIR" != "." ]]; then
        LOG_DIR=$(echo "$LOG_DIR" | tr '\\' '/')
        LOG_DIR=$(echo "$LOG_DIR" | sed 's|^\([A-Za-z]\):|/\L\1|')
        echo "After conversion LOG_DIR: $LOG_DIR"
    fi
fi

mkdir -p "$LOG_DIR"
echo "Logs will be saved to: $LOG_DIR"

VALGRIND=""

kill_process() {
    local process_name=$1
    if is_windows_shell; then
        taskkill //F //IM "${process_name}.exe" > /dev/null 2>&1
        sleep 2
    else
        pkill -TERM "$process_name" > /dev/null 2>&1
    fi
}

if [[ ! -d $TEST_DIR ]]; then
    echo "$TEST_DIR is not a valid path for test executables!"
    exit 1
fi

EXECUTABLES="device_sim tag_rw2 thread_stress test_connection_tag test_connection_tag_late_join test_idle_disconnect test_shutdown_cip test_shutdown_restart get_identity scan_eip_network"
for EXECUTABLE in $EXECUTABLES; do
    if [[ ! -e "$TEST_DIR/$EXECUTABLE" ]]; then
        echo "$TEST_DIR/$EXECUTABLE not found!"
        exit 1
    fi
done

TEST=0
SUCCESSES=0
FAILURES=0

# device_sim uses port 44819 to avoid colliding with ab_server on 44818
DEVICE_SIM_PORT=44819
DEVICE_SIM_TAG="protocol=ab-eip&gateway=127.0.0.1:${DEVICE_SIM_PORT}&path=1,0&plc=ControlLogix"

kill_process device_sim

echo "Starting device_sim on port ${DEVICE_SIM_PORT}."
{ $TEST_DIR/device_sim --port=${DEVICE_SIM_PORT} "--tag=TestDINT:DINT[1]" "--tag=TestBigArray:DINT[2000]" "--tag=Test_Array_2x3:DINT[2,3]" "--tag=Test_Array_2x3x4:DINT[2,3,4]" > "$LOG_DIR/device_sim.log" 2>&1 & } 2>/dev/null
DEVICE_SIM_PID=$!
if [ $DEVICE_SIM_PID -le 0 ]; then
    echo "Unable to start device_sim!"
    exit 1
fi

sleep 1

let TEST++
echo -n "  Test $TEST: unconnected DINT read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint32 \
    "--tag=${DEVICE_SIM_TAG}&elem_count=1&name=TestDINT&use_connected_msg=0" \
    --write=7 --debug=4 > "$LOG_DIR/${TEST}_device_sim_unconnected_rw.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: connected DINT read/write... "
$VALGRIND$TEST_DIR/tag_rw2 --type=sint32 \
    "--tag=${DEVICE_SIM_TAG}&elem_count=1&name=TestDINT" \
    --write=42 --debug=4 > "$LOG_DIR/${TEST}_device_sim_connected_rw.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: CIP Identity object (GetAttributesAll)... "
$VALGRIND$TEST_DIR/get_identity \
    "--tag=protocol=ab-eip&gateway=127.0.0.1:${DEVICE_SIM_PORT}&plc=generic&name=@identity" \
    > "$LOG_DIR/${TEST}_device_sim_identity.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: CIP thread stress (connected)... "
$TEST_DIR/thread_stress 20 \
    "${DEVICE_SIM_TAG}&name=TestDINT" \
    > "$LOG_DIR/${TEST}_device_sim_thread_stress_connected.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: @connection tag state transitions... "
$VALGRIND$TEST_DIR/test_connection_tag \
    "--tag=${DEVICE_SIM_TAG}&name=@connection" \
    > "$LOG_DIR/${TEST}_device_sim_connection_tag.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: @connection tag late join... "
$VALGRIND$TEST_DIR/test_connection_tag_late_join \
    "--data-tag=${DEVICE_SIM_TAG}&elem_count=1&name=TestDINT" \
    "--tag=${DEVICE_SIM_TAG}&name=@connection" \
    --timeout=5000 \
    > "$LOG_DIR/${TEST}_device_sim_connection_tag_late_join.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: idle disconnect and reconnect... "
$VALGRIND$TEST_DIR/test_idle_disconnect \
    "--tag=${DEVICE_SIM_TAG}&name=TestDINT" \
    > "$LOG_DIR/${TEST}_device_sim_idle_disconnect.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

echo "Killing device_sim."
kill_process device_sim
wait $DEVICE_SIM_PID 2>/dev/null

# test_shutdown_cip hard-codes 127.0.0.1:44818 — run a second sim on that port.
echo "Starting device_sim on port 44818 for shutdown tests."
{ $TEST_DIR/device_sim --port=44818 "--tag=TestDINT:DINT[10]" "--tag=TestBigArray:DINT[2000]" > "$LOG_DIR/device_sim_44818.log" 2>&1 & } 2>/dev/null
DEVICE_SIM_PID2=$!
if [ $DEVICE_SIM_PID2 -le 0 ]; then
    echo "Unable to start device_sim on port 44818!"
    exit 1
fi

sleep 1

let TEST++
echo -n "  Test $TEST: EIP UDP List Identity (scan_eip_network)... "
$VALGRIND$TEST_DIR/scan_eip_network \
    --delay-max-ms=500 \
    "--target=127.0.0.1" \
    > "$LOG_DIR/${TEST}_device_sim_scan_eip.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: hard library shutdown (test_shutdown_cip)... "
$VALGRIND$TEST_DIR/test_shutdown_cip \
    > "$LOG_DIR/${TEST}_device_sim_shutdown.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

let TEST++
echo -n "  Test $TEST: library shutdown and restart... "
$VALGRIND$TEST_DIR/test_shutdown_restart \
    "--tag=protocol=ab-eip&gateway=127.0.0.1:44818&path=1,0&plc=ControlLogix&elem_count=1&name=TestDINT" \
    > "$LOG_DIR/${TEST}_device_sim_shutdown_restart.log" 2>&1
if [ $? != 0 ]; then
    echo "FAILURE"
    let FAILURES++
else
    echo "OK"
    let SUCCESSES++
fi

echo "Killing device_sim (port 44818)."
kill_process device_sim
wait $DEVICE_SIM_PID2 2>/dev/null

echo ""
echo "Results:"
echo " - $TEST tests."
echo " - $SUCCESSES successes."
echo " - $FAILURES failures."

exit $FAILURES

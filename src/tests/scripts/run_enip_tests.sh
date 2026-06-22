#!/bin/bash

# run_enip_tests.sh
#
# Exercises the new generic EtherNet/IP module (protocol=enip-tcp) against a
# REAL ControlLogix PLC.  Every test here is one that takes a tag attribute
# string as an argument, drawn from the ControlLogix coverage in
# run_hardware_tests.sh and run_simulator_tests.sh.  Tests with hard-coded
# connection details (string_standard, test_raw_cip, etc.) are intentionally
# excluded because they cannot be re-pointed at the enip-tcp protocol.
#
# Usage: run_enip_tests.sh <TEST_DIR> [LOG_DIR]

TEST_DIR=$1
LOG_DIR=${2:-.}  # Default to current directory if not specified

# The real ControlLogix under test (newer chassis, CPU in slot 4).
GW="10.206.1.40"
PATH_ROUTE="1,4"

# Base tag attribute string used by all data-tag tests.  The manufacturer is
# auto-detected by the generic enip module via GetIdentity, so no plc= is given.
BASE="protocol=enip-tcp&gateway=${GW}&path=${PATH_ROUTE}"

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

# Debug: show what we received
echo "Received TEST_DIR: $TEST_DIR"
echo "Received LOG_DIR: $LOG_DIR"
echo "OSTYPE: $OSTYPE"
echo "Target PLC: gateway=${GW} path=${PATH_ROUTE} (protocol=enip-tcp)"

# Convert Windows paths to Unix paths if running on Windows (Git Bash/Cygwin)
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

# Create the log directory if it doesn't exist
mkdir -p "$LOG_DIR"
echo "Logs will be saved to: $LOG_DIR"

TEST=0
SUCCESSES=0
FAILURES=0

# for deeper testing
# VALGRIND="valgrind --tool=memcheck --track-origins=yes --leak-check=full --show-leak-kinds=all --error-exitcode=1  "
VALGRIND=""

if [[ ! -d $TEST_DIR ]]; then
    echo "$TEST_DIR is not a valid path for test executables!"
    exit 1
fi

# test for the executables.
EXECUTABLES="tag_rw2 test_idle_disconnect test_shutdown_restart test_connection_tag test_connection_tag_late_join test_create_from_tag test_fairness thread_stress"
for EXECUTABLE in $EXECUTABLES
do
    if [[ ! -e "$TEST_DIR/$EXECUTABLE" ]]; then
        echo "$TEST_DIR/$EXECUTABLE not found!"
        exit 1
    fi
done

# Helper: run a test command, record OK/FAILURE based on exit code.
#   run_test "<description>" "<log-suffix>" <command...>
run_test() {
    local desc=$1
    local suffix=$2
    shift 2

    let TEST++
    echo -n "  Test $TEST: ${desc}... "
    "$@" > "$LOG_DIR/${TEST}_${suffix}.log" 2>&1
    if [ $? != 0 ]; then
        echo "FAILURE"
        let FAILURES++
    else
        echo "OK"
        let SUCCESSES++
    fi
}

# Helper: run a command and pass only if its output matches a regex (grep -E).
#   run_grep_test "<description>" "<log-suffix>" "<regex>" <command...>
run_grep_test() {
    local desc=$1
    local suffix=$2
    local pattern=$3
    shift 3

    let TEST++
    echo -n "  Test $TEST: ${desc}... "
    "$@" > "$LOG_DIR/${TEST}_${suffix}.log" 2>&1
    if grep -qE "$pattern" "$LOG_DIR/${TEST}_${suffix}.log"; then
        echo "OK"
        let SUCCESSES++
    else
        echo "FAILURE (pattern '${pattern}' not found)"
        let FAILURES++
    fi
}


echo ""
echo "=== Basic read/write (unconnected and connected messaging) ==="

# ----- from run_simulator_tests.sh: basic unconnected/connected read/write -----

run_test "basic unconnected tag read/write" "unconnected_small" \
    $VALGRIND$TEST_DIR/tag_rw2 --type=sint32 \
    "--tag=${BASE}&elem_count=10&name=TestBigArray&use_connected_msg=0" \
    --debug=4 --write=1,2,3,4,5,6,7,8,9

run_test "basic unconnected large tag read/write" "unconnected_large" \
    $VALGRIND$TEST_DIR/tag_rw2 --type=sint32 \
    "--tag=${BASE}&elem_count=1000&name=TestBigArray&use_connected_msg=0" \
    --debug=4 --write=1,2,3,4,5,6,7,8,9

run_test "basic connected tag read/write" "connected_small" \
    $VALGRIND$TEST_DIR/tag_rw2 --type=sint32 \
    "--tag=${BASE}&elem_count=10&name=TestBigArray" \
    --debug=4 --write=1,2,3,4,5,6,7,8,9

run_test "basic connected large tag read/write" "connected_large" \
    $VALGRIND$TEST_DIR/tag_rw2 --type=sint32 \
    "--tag=${BASE}&elem_count=1000&name=TestBigArray" \
    --debug=4 --write=1,2,3,4,5,6,7,8,9


echo ""
echo "=== Metadata and bit-level access (from run_hardware_tests.sh) ==="

# ----- from run_hardware_tests.sh: metadata fetch -----
run_test "get tag metadata" "metadata" \
    $VALGRIND$TEST_DIR/tag_rw2 --type=metadata \
    "--tag=${BASE}&name=TestBOOLArray&elem_count=2&debug=4"

# ----- from run_hardware_tests.sh: INT/LINT bit get/set -----
run_test "Get INT bit" "get_int_bit" \
    $VALGRIND$TEST_DIR/tag_rw2 --type=bit \
    "--tag=${BASE}&name=TestINTArray[0].13" --debug=4

run_test "Set INT bit" "set_int_bit" \
    $VALGRIND$TEST_DIR/tag_rw2 --type=bit \
    "--tag=${BASE}&name=TestINTArray[0].13" --debug=4 --write=1

run_test "Get LINT bit" "get_lint_bit" \
    $VALGRIND$TEST_DIR/tag_rw2 --type=bit \
    "--tag=${BASE}&name=TestLINTArray[0].43" --debug=4

run_test "Set LINT bit" "set_lint_bit" \
    $VALGRIND$TEST_DIR/tag_rw2 --type=bit \
    "--tag=${BASE}&name=TestLINTArray[0].43" --debug=4 --write=1

# ----- STRING (Logix 88-byte counted struct) read/write -----
run_test "STRING read" "string_read" \
    $VALGRIND$TEST_DIR/tag_rw2 --type=string \
    "--tag=${BASE}&name=barcode&elem_count=1" --debug=4

run_test "STRING write" "string_write" \
    $VALGRIND$TEST_DIR/tag_rw2 --type=string \
    "--tag=${BASE}&name=barcode&elem_count=1" --debug=4 --write="hello enip"

# ----- @identity (CIP Identity object, queried during bring-up) -----
run_test "@identity read" "identity" \
    $VALGRIND$TEST_DIR/tag_rw2 --type=identity \
    "--tag=${BASE}&name=@identity" --debug=4

# Confirm the engine classifies the device from its Identity reply. The test PLC
# is a 1756-L81E, whose CPU answers List Identity directly (device type PLC).
run_grep_test "@identity device classified as ControlLogix" "identity_class" \
    "device is ControlLogix-class" \
    $VALGRIND$TEST_DIR/tag_rw2 --type=identity \
    "--tag=${BASE}&name=@identity" --debug=4


echo ""
echo "=== Connection lifecycle and scheduling ==="

# ----- idle disconnect (appears in both scripts) -----
run_test "idle disconnect and reconnect with runtime timeout change" "idle_disconnect" \
    $VALGRIND$TEST_DIR/test_idle_disconnect \
    "--tag=${BASE}&name=TestBigArray"

# ----- from run_simulator_tests.sh: CIP thread stress -----
run_test "CIP thread stress" "thread_stress" \
    $TEST_DIR/thread_stress 20 "${BASE}&name=TestBigArray"

# ----- from run_simulator_tests.sh: tag scheduling fairness -----
run_test "tag scheduling fairness" "fairness" \
    $VALGRIND$TEST_DIR/test_fairness \
    "--tag=${BASE}&name=TestBigArray[0]&auto_sync_read_ms=200" \
    --num-tags=200 --test-duration-secs=10

# ----- from run_simulator_tests.sh: library shutdown and restart -----
run_test "library shutdown and restart" "shutdown_restart" \
    $VALGRIND$TEST_DIR/test_shutdown_restart \
    "--tag=${BASE}&elem_count=1&name=TestBigArray"


echo ""
echo "=== @connection tag tests ==="

# ----- from run_simulator_tests.sh: connection tag state transitions -----
run_test "connection tag connection state transitions" "connection_tag" \
    $VALGRIND$TEST_DIR/test_connection_tag \
    "--tag=${BASE}&name=@connection"

# ----- from run_simulator_tests.sh: multiple @connection tags on one session -----
run_test "multiple simultaneous @connection tags on same session" "connection_tag_multi" \
    $VALGRIND$TEST_DIR/test_connection_tag \
    "--tag=${BASE}&name=@connection" \
    --num-tags=3 \
    "--data-tag=${BASE}&elem_count=1&name=TestBigArray[0]"

# ----- from run_simulator_tests.sh: @connection tag late join -----
run_test "@connection tag late join (session already UP before tag created)" "connection_tag_late_join" \
    $VALGRIND$TEST_DIR/test_connection_tag_late_join \
    "--data-tag=${BASE}&elem_count=1&name=TestBigArray[0]" \
    "--tag=${BASE}&name=@connection" \
    --timeout=5000

# ----- from run_simulator_tests.sh: @connection tag 2-cycle reconnect -----
run_test "@connection tag 2-cycle reconnect (5 s idle timeout)" "connection_tag_cycle" \
    $VALGRIND$TEST_DIR/test_connection_tag \
    "--tag=${BASE}&name=@connection" \
    --cycles=2 \
    "--data-tag=${BASE}&elem_count=1&name=TestBigArray[0]" \
    --idle-timeout-ms=5000


echo ""
echo "=== Create-from-tag API ==="

# ----- from run_simulator_tests.sh: create-from-tag permutation tests -----
run_test "create-from-tag API (17 permutation tests)" "create_from_tag" \
    $VALGRIND$TEST_DIR/test_create_from_tag \
    "--src-tag=${BASE}&elem_count=1&name=TestBigArray[0]" \
    "--clone-attrib=name=TestBigArray[1]&elem_count=1" \
    "--connection-tag=${BASE}&name=@connection" \
    --timeout=10000


# show results
echo ""
echo "Results:"
echo " - $TEST tests."
echo " - $SUCCESSES successes."
echo " - $FAILURES failures."

exit $FAILURES

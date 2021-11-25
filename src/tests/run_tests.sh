#!/bin/bash

TEST_DIR=$1

if [[ -d $TEST_DIR ]]; then
    echo "Using $TEST_DIR for test executables."
else
    echo "$TEST_DIR is not a valid path for test executables!"
    exit 1
fi

# test for the executables.
EXECUTABLES="ab_server tag_rw2 async_stress thread_stress test_callback"
for EXECUTABLE in $EXECUTABLES
do
    echo "Testing for $EXECUTABLE."
    if [[ ! -e "$TEST_DIR/$EXECUTABLE" ]]; then
        echo "$TEST_DIR/$EXECUTABLE not found!"
        exit 1
    fi
done



#!/bin/bash

# clean up any previous server instances
killall -TERM modbus_server

# wait for processes to exit
sleep 1

# Start the Modbus server with multiple listening endpoints
../../build/bin_dist/modbus_server --listen 127.0.0.1:1502 --listen 127.0.0.1:2502 --debug > server.log 2>&1 &
SERVER_PID=$!
sleep 1

# Run the tests
../../build/bin_dist/test_modbus_multiple > test_modbus_multiple.log 2>&1
TEST_RC=$?
if [ $TEST_RC -ne 0 ]; then
    echo "Modbus multiple test failed with return code $TEST_RC"
fi

# Stop the server
killall -TERM modbus_server

# wait for server to exit
sleep 1

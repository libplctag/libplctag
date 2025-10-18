#!/bin/bash

# Start the Modbus server with multiple listening endpoints
../../build/bin_dist/modbus_server --listen 127.0.0.1:1502 --listen 127.0.0.1:2502 --debug > server_debug.log 2>&1 &
SERVER_PID=$!
sleep 1

# Run the tests
../../build/bin_dist/test_modbus_multiple > test_debug.log 2>&1
TEST_RC=$?
if [ $TEST_RC -ne 0 ]; then
    echo "Modbus multiple test failed with return code $TEST_RC"
fi

# Stop the server
kill $SERVER_PID
wait $SERVER_PID 2>/dev/null


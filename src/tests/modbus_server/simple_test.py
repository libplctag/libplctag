#!/usr/bin/env python3
"""
Simpler Modbus TCP test - just read holding registers
"""

import socket
import struct

def test_read():
    host = '127.0.0.1'
    port = 5502
    
    print(f"Connecting to {host}:{port}...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    sock.settimeout(5.0)
    print("Connected!")
    
    # Read 3 holding registers starting at address 0
    # MBAP: transaction_id=1, protocol_id=0, length=6, unit_id=1
    # PDU: function_code=0x03, start_addr=0, count=3
    transaction_id = 1
    protocol_id = 0
    length = 6  # unit_id + function_code + start_addr + count = 1 + 1 + 2 + 2
    unit_id = 1
    function_code = 0x03
    start_addr = 0
    count = 3
    
    request = struct.pack('>HHHBHH', 
                         transaction_id, protocol_id, length, unit_id,
                         function_code, start_addr, count)
    
    # Wait, function code should be separate...
    request = struct.pack('>HHHB', transaction_id, protocol_id, length, unit_id)
    request += struct.pack('>BHH', function_code, start_addr, count)
    
    print(f"Sending: {request.hex()}")
    sock.sendall(request)
    
    response = sock.recv(1024)
    print(f"Received: {response.hex()}")
    
    # Parse response
    tid, pid, rlen, uid = struct.unpack('>HHHB', response[:7])
    fc = response[7]
    
    print(f"Transaction ID: {tid}")
    print(f"Protocol ID: {pid}")
    print(f"Length: {rlen}")
    print(f"Unit ID: {uid}")
    print(f"Function Code: 0x{fc:02X}")
    
    if fc & 0x80:
        exception = response[8]
        print(f"EXCEPTION: 0x{exception:02X}")
        exceptions = {
            1: "Illegal Function",
            2: "Illegal Data Address",
            3: "Illegal Data Value",
            4: "Server Device Failure"
        }
        print(f"  {exceptions.get(exception, 'Unknown')}")
    else:
        byte_count = response[8]
        print(f"Byte count: {byte_count}")
        values = []
        for i in range(byte_count // 2):
            val = struct.unpack('>H', response[9+i*2:11+i*2])[0]
            values.append(val)
        print(f"Values: {values}")
    
    sock.close()

if __name__ == '__main__':
    test_read()

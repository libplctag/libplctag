#!/usr/bin/env python3
"""
Simple Modbus TCP test client for the modbus_server
Tests basic read/write operations
"""

import socket
import struct
import sys

def build_modbus_tcp_request(transaction_id, unit_id, function_code, data):
    """Build a Modbus TCP request"""
    pdu = struct.pack('B', function_code) + data
    length = len(pdu) + 1  # +1 for unit_id
    mbap = struct.pack('>HHHB', transaction_id, 0, length, unit_id)
    return mbap + pdu

def parse_modbus_tcp_response(data):
    """Parse a Modbus TCP response"""
    if len(data) < 8:
        return None
    
    transaction_id, protocol_id, length, unit_id = struct.unpack('>HHHB', data[:7])
    function_code = data[7]
    pdu_data = data[8:]
    
    return {
        'transaction_id': transaction_id,
        'protocol_id': protocol_id,
        'length': length,
        'unit_id': unit_id,
        'function_code': function_code,
        'data': pdu_data
    }

def test_write_read_holding_registers(sock):
    """Test writing and reading holding registers"""
    print("\\n=== Test: Write and Read Holding Registers ===")
    
    # Write 3 registers starting at address 0
    # Function code 0x10 (Write Multiple Registers)
    start_addr = 0
    values = [100, 200, 300]
    data = struct.pack('>HHB', start_addr, len(values), len(values) * 2)
    for val in values:
        data += struct.pack('>H', val)
    
    request = build_modbus_tcp_request(1, 1, 0x10, data)
    print(f"Sending write request: {request.hex()}")
    sock.sendall(request)
    
    response = sock.recv(1024)
    print(f"Received response: {response.hex()}")
    resp = parse_modbus_tcp_response(response)
    print(f"Write response: FC=0x{resp['function_code']:02X}")
    
    # Read the registers back
    # Function code 0x03 (Read Holding Registers)
    data = struct.pack('>HH', start_addr, len(values))
    request = build_modbus_tcp_request(2, 1, 0x03, data)
    print(f"\\nSending read request: {request.hex()}")
    sock.sendall(request)
    
    response = sock.recv(1024)
    print(f"Received response: {response.hex()}")
    resp = parse_modbus_tcp_response(response)
    
    if resp['function_code'] == 0x03:
        byte_count = resp['data'][0]
        read_values = []
        for i in range(byte_count // 2):
            val = struct.unpack('>H', resp['data'][1 + i*2:3 + i*2])[0]
            read_values.append(val)
        print(f"Read values: {read_values}")
        
        if read_values == values:
            print("✓ PASS: Values match!")
            return True
        else:
            print(f"✗ FAIL: Expected {values}, got {read_values}")
            return False
    else:
        print(f"✗ FAIL: Unexpected function code 0x{resp['function_code']:02X}")
        return False

def test_write_read_coils(sock):
    """Test writing and reading coils"""
    print("\\n=== Test: Write and Read Coils ===")
    
    # Write coil at address 5 to ON
    # Function code 0x05 (Write Single Coil)
    data = struct.pack('>HH', 5, 0xFF00)  # 0xFF00 = ON
    request = build_modbus_tcp_request(3, 1, 0x05, data)
    print(f"Sending write single coil: {request.hex()}")
    sock.sendall(request)
    
    response = sock.recv(1024)
    print(f"Received response: {response.hex()}")
    resp = parse_modbus_tcp_response(response)
    print(f"Write response: FC=0x{resp['function_code']:02X}")
    
    # Read coils back
    # Function code 0x01 (Read Coils)
    data = struct.pack('>HH', 0, 10)  # Read 10 coils starting at 0
    request = build_modbus_tcp_request(4, 1, 0x01, data)
    print(f"\\nSending read coils: {request.hex()}")
    sock.sendall(request)
    
    response = sock.recv(1024)
    print(f"Received response: {response.hex()}")
    resp = parse_modbus_tcp_response(response)
    
    if resp['function_code'] == 0x01:
        byte_count = resp['data'][0]
        coil_byte = resp['data'][1]
        print(f"Coil byte: 0x{coil_byte:02X}")
        
        # Check if bit 5 is set
        if (coil_byte & (1 << 5)) != 0:
            print("✓ PASS: Coil 5 is ON")
            return True
        else:
            print("✗ FAIL: Coil 5 is not ON")
            return False
    else:
        print(f"✗ FAIL: Unexpected function code 0x{resp['function_code']:02X}")
        return False

def main():
    host = '127.0.0.1'
    port = 5502
    
    print(f"Connecting to Modbus TCP server at {host}:{port}...")
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((host, port))
        sock.settimeout(5.0)
        print("Connected!")
        
        tests = [
            test_write_read_holding_registers,
            test_write_read_coils
        ]
        
        passed = 0
        for test in tests:
            if test(sock):
                passed += 1
        
        print(f"\\n{'='*50}")
        print(f"Tests passed: {passed}/{len(tests)}")
        print('='*50)
        
        sock.close()
        return 0 if passed == len(tests) else 1
        
    except Exception as e:
        print(f"Error: {e}")
        import traceback
        traceback.print_exc()
        return 1

if __name__ == '__main__':
    sys.exit(main())

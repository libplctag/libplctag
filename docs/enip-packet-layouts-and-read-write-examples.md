# ENIP Packet Layouts and Read/Write Examples

Date: 2026-05-28
Status: Reference design and implementation guide for ENIP explicit messaging packet formats and operation examples.

## 1. Scope

This document provides:
1. Byte-level field order and sizes for ENIP encapsulation and CPF headers used in this codebase.
2. Unconnected and connected CPF item layout details.
3. Worked examples for:
   1. AB symbolic tag read/write (normal and chunking form).
   2. Omron read/write with normal path and 0x80 simple data segment chunking path.
   3. PLC5 read/write (direct and DH+ bridge), including PLC5 bit write.
   4. SLC500 read/write, including SLC bit write.

Primary local anchors:
1. [src/libplctag/protocols/ab/defs.h](src/libplctag/protocols/ab/defs.h)
2. [src/libplctag/protocols/ab/eip_cip.c](src/libplctag/protocols/ab/eip_cip.c)
3. [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c)
4. [src/libplctag/protocols/ab/eip_lgx_pccc.c](src/libplctag/protocols/ab/eip_lgx_pccc.c)

Supporting Omron chunking reference used for segment shape:
1. External reference used during research: /tmp/aphytcomm/src/aphyt/omron/n_series.py

## 2. EIP Encapsulation Header (24 bytes)

Struct anchor: `eip_encap` in [src/libplctag/protocols/ab/defs.h](src/libplctag/protocols/ab/defs.h#L239)

| Offset | Size | Field | Notes |
|---|---:|---|---|
| 0 | 2 | encap_command | ENIP command code. |
| 2 | 2 | encap_length | Payload size excluding this 24-byte header. |
| 4 | 4 | encap_session_handle | Session handle from RegisterSession. |
| 8 | 4 | encap_status | Sender sets 0 in requests. |
| 12 | 8 | encap_sender_context | Correlation token copied into response. |
| 20 | 4 | encap_options | Reserved, 0. |

Total: 24 bytes.

## 3. CPF Header (8 bytes)

The CPF base for explicit messaging in this implementation is:
1. `interface_handle` (4 bytes)
2. `router_timeout` (2 bytes)
3. `cpf_item_count` (2 bytes)

Struct anchors:
1. [src/libplctag/protocols/ab/defs.h](src/libplctag/protocols/ab/defs.h#L304)
2. [src/libplctag/protocols/ab/defs.h](src/libplctag/protocols/ab/defs.h#L332)

| Offset (from CPF start) | Size | Field | Notes |
|---|---:|---|---|
| 0 | 4 | interface_handle | Always 0 for CIP over ENIP explicit messaging. |
| 4 | 2 | router_timeout | Seconds/ticks semantics depend on message type. |
| 6 | 2 | cpf_item_count | Typically 2 in these request paths. |

Total: 8 bytes.

## 4. Unconnected CPF Data Item Headers (8 bytes)

This is the fixed item header pair after the 8-byte CPF base.

Struct anchors:
1. `cpf_unconnected_addr_item` in [src/libplctag/protocols/ab/defs.h](src/libplctag/protocols/ab/defs.h#L266)
2. `cpf_unconnected_data_item` in [src/libplctag/protocols/ab/defs.h](src/libplctag/protocols/ab/defs.h#L271)

| Order | Size | Field | Typical Value |
|---:|---:|---|---|
| 1 | 2 | cpf_nai_item_type | 0x0000 (Null Address Item). |
| 2 | 2 | cpf_nai_item_length | 0x0000. |
| 3 | 2 | cpf_udi_item_type | 0x00B2 (Unconnected Data Item). |
| 4 | 2 | cpf_udi_item_length | Length in bytes of remaining unconnected payload. |

Total: 8 bytes.

## 5. Connected CPF Data Fields

Connected messaging uses a connected address item and a connected data item.

Struct anchors:
1. `cpf_connected_addr_item` in [src/libplctag/protocols/ab/defs.h](src/libplctag/protocols/ab/defs.h#L277)
2. `cpf_connected_data_item` in [src/libplctag/protocols/ab/defs.h](src/libplctag/protocols/ab/defs.h#L283)

### 5.1 Connected Address Item

| Order | Size | Field | Typical Value |
|---:|---:|---|---|
| 1 | 2 | cpf_cai_item_type | 0x00A1 (Connected Address Item). |
| 2 | 2 | cpf_cai_item_length | 4 |
| 3 | 4 | cpf_targ_conn_id | Target connection ID from Forward Open. |

Subtotal: 8 bytes.

### 5.2 Connected Data Item Header + Sequence

| Order | Size | Field | Typical Value |
|---:|---:|---|---|
| 1 | 2 | cpf_cdi_item_type | 0x00B1 (Connected Data Item). |
| 2 | 2 | cpf_cdi_item_length | Number of bytes in connected data item payload. |
| 3 | 2 | cpf_conn_seq_num | Connection sequence number. |
| 4 | N | CIP/PCCC payload bytes | Service + path + command data. |

Important payload accounting rule:
1. `cpf_cdi_item_length` includes `cpf_conn_seq_num` (2 bytes) plus the remaining connected payload bytes.
2. Therefore, if Forward Open negotiation allows a max explicit payload budget `P`, user data budget in connected mode is effectively `P - 2` because sequence is consumed inside the connected data item.

## 6. CIP Symbolic Name and Route Encoding Example

Tag example: `myTag[4].field1`

Encoded path bytes (including leading path-word count used by this stack):
1. `09 91 05 6D 79 54 61 67 00 28 04 91 06 66 69 65 6C 64 31`

Breakdown:
1. `91 05 6D 79 54 61 67 00` = symbolic segment `myTag`.
2. `28 04` = logical array index `[4]`.
3. `91 06 66 69 65 6C 64 31` = symbolic segment `field1`.

Route example: `1,3,A,192.168.1.2,1,0`
1. Alias normalization: `A -> 18`.
2. Encoded bytes: `01 03 12 0B 31 39 32 2E 31 36 38 2E 31 2E 32 00 01 00`

## 7. AB Tag Read and Write Examples

Service constants:
1. Read: `0x4C` (normal), `0x52` (fragmented/byte-offset form).
2. Write: `0x4D` (normal), `0x53` (fragmented/byte-offset form).

Constants anchor: [src/libplctag/protocols/ab/defs.h](src/libplctag/protocols/ab/defs.h#L81)

### 7.1 AB Normal Read (0x4C)

Request payload shape:
1. `service=0x4C`
2. `encoded_tag_path`
3. `element_count (uint16_le)`

Example skeleton:
1. `4C | 09 91 05 6D 79 54 61 67 00 28 04 91 06 66 69 65 6C 64 31 | 01 00`

### 7.2 AB Read with Byte Offset (0x52)

Request payload shape:
1. `service=0x52`
2. `encoded_tag_path`
3. `element_count (uint16_le)`
4. `byte_offset (uint32_le)`

Example skeleton for chunk 2 at byte offset 400:
1. `52 | <encoded_tag_path> | 64 00 | 90 01 00 00`

Current implementation note:
1. Current builder in [src/libplctag/protocols/ab/eip_cip.c](src/libplctag/protocols/ab/eip_cip.c#L378) uses `0x52` read-frag form in the active path.

### 7.3 AB Normal Write (0x4D)

Request payload shape:
1. `service=0x4D`
2. `encoded_tag_path`
3. `encoded_type_info`
4. `element_count (uint16_le)`
5. `write_data`

### 7.4 AB Write with Byte Offset (0x53)

Request payload shape:
1. `service=0x53`
2. `encoded_tag_path`
3. `encoded_type_info`
4. `element_count (uint16_le)`
5. `byte_offset (uint32_le)`
6. `write_data_chunk`

Current implementation note:
1. [src/libplctag/protocols/ab/eip_cip.c](src/libplctag/protocols/ab/eip_cip.c#L760) selects `0x53` when the write must be split across packets.

## 8. Omron Read/Write Examples (Normal and 0x80 Chunking Segment)

### 8.1 Omron Normal Read/Write

Normal CIP symbolic access uses the same base symbolic path style as AB for in-budget operations:
1. Read: `0x4C + path + element_count`
2. Write: `0x4D + path + type + element_count + data`

### 8.2 Omron Chunked Read/Write with 0x80 Simple Data Segment

Reference segment shape:
1. Segment type: `0x80`
2. Segment length in 16-bit words: `0x03`
3. Offset: `uint32_le`
4. Size: `uint16_le`

Example segment bytes (offset=0x00000190, size=0x0190):
1. `80 03 90 01 00 00 90 01`

This segment is appended after the variable request path and used for multi-message string/array/structure reads and writes.

Reference anchor for shape and loop:
1. External reference used during research: /tmp/aphytcomm/src/aphyt/omron/n_series.py (SimpleDataSegmentRequest and multi-message read/write helpers)

## 9. Chunking Examples

### 9.1 AB Chunked Read Example

Given:
1. Element type DINT (4 bytes).
2. Total bytes needed = 4096.
3. Safe chunk payload = 400 bytes.

Loop:
1. Chunk 0: service `0x52`, offset 0, read 100 elements.
2. Chunk 1: service `0x52`, offset 400, read 100 elements.
3. Continue until 4096 bytes are assembled.

### 9.2 AB Chunked Write Example

Given same budgets:
1. Chunk 0: service `0x53`, offset 0, write first 400 bytes.
2. Chunk 1: service `0x53`, offset 400, write next 400 bytes.
3. Continue until full write is acknowledged.

### 9.3 Omron Chunked Read/Write Example

Given max read chunk = 400:
1. Read chunk 0: append `80 03 00 00 00 00 90 01`.
2. Read chunk 1: append `80 03 90 01 00 00 90 01`.
3. For write, append same segment form and send data slice matching segment size.

## 10. PLC5 Read/Write Examples

Constants anchor: [src/libplctag/protocols/ab/defs.h](src/libplctag/protocols/ab/defs.h#L99)

### 10.1 PLC5 Direct Read

PCCC typed command request core:
1. `pccc_command = typed_cmd`
2. `pccc_function = 0x01` (PLC5 range read)
3. `pccc_offset (uint16_le)`
4. `pccc_transfer_size (word count)`
5. `encoded_plc5_address`
6. size byte where used by the specific request shape

### 10.2 PLC5 Direct Write

PCCC typed command request core:
1. `pccc_function = 0x00` (PLC5 range write)
2. transfer size in words
3. encoded address
4. write data bytes

## 11. PLC5 Read/Write over DH+ Bridge

DH+ request struct anchor: [src/libplctag/protocols/ab/defs.h](src/libplctag/protocols/ab/defs.h#L1034)

Added routing header fields before PCCC command:
1. `dest_link (uint16_le)`
2. `dest_node (uint16_le)`
3. `src_link (uint16_le)`
4. `src_node (uint16_le)`

Then normal PLC5 PCCC command/function/offset/transfer/data fields follow.

## 12. PLC5 Bit Write Example

Function code:
1. `0x26` (PLC5 read-modify-write)

Core payload shape:
1. Encoded address
2. Reset (AND) mask bytes
3. Set (OR) mask bytes

Behavior anchor: [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c#L1918)

## 13. SLC500 Read/Write Examples

Constants anchor: [src/libplctag/protocols/ab/defs.h](src/libplctag/protocols/ab/defs.h#L104)

### 13.1 SLC500 Read

Core fields:
1. `pccc_function = 0xA2` (SLC range read)
2. transfer size in bytes
3. encoded SLC address

### 13.2 SLC500 Write

Core fields:
1. `pccc_function = 0xAA` (SLC range write)
2. transfer size in bytes
3. encoded SLC address
4. write data bytes

## 14. SLC500 Bit Write Example

SLC bit-write path uses masked write style with 2-byte element constraints.

Core shape:
1. Transfer size byte.
2. Encoded address.
3. Change mask bytes.
4. Set bytes.

Behavior anchor: [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c#L2111)

## 15. Implementation Notes

1. Connected payload sizing must reserve 2 bytes for CPF connection sequence number inside `cpf_cdi_item_length`.
2. For chunking, keep one chunk request in flight per tag and advance offset/element index monotonically.
3. For PCCC families, prefer manual chunking by element stepping and transfer-size adjustment instead of introducing CIP fragmentation services in PCCC paths.

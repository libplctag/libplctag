# ENIP+PCCC Implementation Cookbook

Date: 2026-05-28
Status: File-by-file implementation guide

## 1. Source Map

Core files:
1. [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c)
2. [src/libplctag/protocols/ab/pccc.h](src/libplctag/protocols/ab/pccc.h)
3. [src/libplctag/protocols/ab/eip_lgx_pccc.c](src/libplctag/protocols/ab/eip_lgx_pccc.c)
4. [src/libplctag/protocols/ab/eip_plc5_pccc.c](src/libplctag/protocols/ab/eip_plc5_pccc.c)
5. [src/libplctag/protocols/ab/eip_slc_pccc.c](src/libplctag/protocols/ab/eip_slc_pccc.c)
6. [src/libplctag/protocols/ab/eip_plc5_dhp.c](src/libplctag/protocols/ab/eip_plc5_dhp.c)
7. [src/libplctag/protocols/ab/eip_slc_dhp.c](src/libplctag/protocols/ab/eip_slc_dhp.c)

Address parsing and encoding helpers:
1. parse_pccc_logical_address in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c#L313)
2. plc5_encode_address in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c#L370)
3. slc_encode_address in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c#L430)

## 2. Current Read/Write Entrypoints

Direct PCCC:
1. pccc_tag_read_start in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c#L1456)
2. pccc_check_read_status in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c#L1666)
3. pccc_tag_write_start in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c#L1727)
4. pccc_check_write_status in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c#L2310)

DH+ PCCC:
1. pccc_dhp_tag_read_start in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c#L2426)
2. pccc_dhp_check_read_status in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c#L2640)
3. pccc_dhp_tag_write_start in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c#L2692)
4. pccc_dhp_check_write_status in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c#L3272)

Logix-over-PCCC:
1. tag_read_start in [src/libplctag/protocols/ab/eip_lgx_pccc.c](src/libplctag/protocols/ab/eip_lgx_pccc.c#L162)
2. check_read_status in [src/libplctag/protocols/ab/eip_lgx_pccc.c](src/libplctag/protocols/ab/eip_lgx_pccc.c#L284)
3. tag_write_start in [src/libplctag/protocols/ab/eip_lgx_pccc.c](src/libplctag/protocols/ab/eip_lgx_pccc.c#L401)
4. check_write_status in [src/libplctag/protocols/ab/eip_lgx_pccc.c](src/libplctag/protocols/ab/eip_lgx_pccc.c#L736)

## 3. Bit Write Documentation Per PLC Type

### 3.1 PLC5 Direct Bit Write

Function:
1. plc5_tag_write_bit_start in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c#L1918)

Mechanics:
1. Uses PCCC typed command with PLC5 read-modify-write function.
2. Encoded address is emitted first.
3. Two masks are appended:
   1. Reset mask (AND behavior).
   2. Set mask (OR behavior).
4. Bit position is mapped by byte index and bit index.

### 3.2 SLC500/MicroLogix Direct Bit Write

Function:
1. slc_tag_write_bit_start in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c#L2111)

Mechanics:
1. Requires 2-byte element and 2-byte tag size checks.
2. Uses SLC range write mask function.
3. Payload layout includes:
   1. Transfer size byte.
   2. Encoded address.
   3. Change mask.
   4. Set bytes.

### 3.3 PLC5 DH+ Bit Write

Function:
1. plc5_dhp_tag_write_bit_start in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c#L2885)

Mechanics:
1. Same masked semantics as PLC5 direct bit write.
2. Adds DH+ routing header before PCCC command.
3. Uses connected ENIP CPF framing with connection sequence fields.

### 3.4 SLC500/MicroLogix DH+ Bit Write

Function:
1. slc_dhp_tag_write_bit_start in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c#L3075)

Mechanics:
1. Same masked semantics as SLC direct bit write.
2. Adds DH+ routing header.
3. Enforces same 2-byte element restrictions.

### 3.5 Logix-over-PCCC Bit Write

Observation:
1. No dedicated bit-write path is present in [src/libplctag/protocols/ab/eip_lgx_pccc.c](src/libplctag/protocols/ab/eip_lgx_pccc.c).
2. No is_bit branch or read-modify-write function selection is implemented there.

Guidance:
1. Treat Logix-over-PCCC bit edits as element writes for now.
2. If dedicated bit writes are required later, add a Logix-over-PCCC masked write path explicitly rather than reusing PLC5/SLC masks blindly.

## 4. Manual Chunking Cookbook

### 4.1 Shared Preparation

1. Derive request-side and response-side data budgets after protocol overhead.
2. For reads, size chunks from response budget first.
3. For writes, size chunks from request budget first.
4. Align chunk bytes to element size boundaries.
5. Optionally validate the non-dominant side and reduce chunk size only if needed.

### 4.2 Read Chunk Loop

1. bytes_done = 0.
2. While bytes_done < total_bytes:
   1. Compute chunk_bytes for remaining data.
   2. Compute chunk element index from base element plus bytes_done/elem_size.
   3. Re-encode logical address for that chunk element index.
   4. Build read request with chunk transfer size.
   5. Submit and validate response.
   6. Copy chunk response into destination at bytes_done.
   7. Increment bytes_done.

### 4.3 Write Chunk Loop

1. bytes_done = 0.
2. While bytes_done < total_bytes:
   1. Compute chunk_bytes for remaining data.
   2. Compute chunk element index.
   3. Re-encode logical address.
   4. Build write request with chunk transfer size and source slice.
   5. Submit and validate write ack.
   6. Increment bytes_done.

### 4.4 PLC-Type Field Rules During Chunking

PLC5 direct:
1. Transfer size fields are words where required by function code.
2. Maintain word alignment.

SLC/MicroLogix direct:
1. Transfer size fields are bytes.

Logix-over-PCCC:
1. Keep request shape used today.
2. Prefer element-index progression first.
3. Use pccc_offset progression only after target validation.

DH+ variants:
1. Same chunking loop as direct PLC5/SLC.
2. Include DH+ header bytes in overhead calculations.

## 5. Pseudocode

Read:

    function enip_pccc_chunked_read(tag):
        budgets = calc_pccc_budgets(tag)
      chunk = calc_aligned_read_chunk(budgets.response_budget, tag.elem_size)
        done = 0

        while done < tag.size:
            size = min(chunk, tag.size - done)
            elem = tag.base_elem + (done / tag.elem_size)
            req = build_pccc_read(tag, elem, size)
            resp = send_wait_validate(req)
            copy_into_tag(tag, done, resp.payload)
            done += size

Write:

    function enip_pccc_chunked_write(tag):
        budgets = calc_pccc_budgets(tag)
      chunk = calc_aligned_write_chunk(budgets.request_budget, tag.elem_size)
        done = 0

        while done < tag.size:
            size = min(chunk, tag.size - done)
            elem = tag.base_elem + (done / tag.elem_size)
            req = build_pccc_write(tag, elem, size, tag.data[done:done+size])
            send_wait_validate(req)
            done += size

> **DEPRECATED.** This document predates the current EtherNet/IP design and is
> retained for historical reference only. The authoritative design is
> [ENIP-SESSION-DESIGN.md](../src/libplctag/protocols/enip/ENIP-SESSION-DESIGN.md). Do not use this document to guide new work.

# ENIP+PCCC Read, Write, Bit Write, and Manual Chunking Design

Date: 2026-05-28
Status: Index and overview

## Split Documents

This write-up has been split into dedicated documents:
1. Contract: [docs/enip-pccc-contract.md](docs/enip-pccc-contract.md)
2. Implementation cookbook: [docs/enip-pccc-implementation-cookbook.md](docs/enip-pccc-implementation-cookbook.md)
3. Test plan: [docs/enip-pccc-test-plan.md](docs/enip-pccc-test-plan.md)

Bit write coverage per PLC type is documented in:
1. [docs/enip-pccc-implementation-cookbook.md](docs/enip-pccc-implementation-cookbook.md#L42)

Logix-over-PCCC bit write note:
1. No dedicated masked bit-write path was found in [src/libplctag/protocols/ab/eip_lgx_pccc.c](src/libplctag/protocols/ab/eip_lgx_pccc.c).
2. This is called out explicitly in [docs/enip-pccc-implementation-cookbook.md](docs/enip-pccc-implementation-cookbook.md#L90).

## Original Consolidated Content

The original consolidated content is retained below for backward reference.

## 1. Scope

This document describes current and target behavior for ENIP+PCCC operations used by:
1. Logix-class PLC acting as PCCC execution endpoint (Logix-over-PCCC).
2. PLC5 direct ENIP PCCC access.
3. SLC500 and MicroLogix direct ENIP PCCC access.
4. DH+ bridged variants (Logix chassis to DH+ to remote PCCC/DF1 PLC).

Primary source files:
1. [src/libplctag/protocols/ab/eip_lgx_pccc.c](src/libplctag/protocols/ab/eip_lgx_pccc.c)
2. [src/libplctag/protocols/ab/eip_plc5_pccc.c](src/libplctag/protocols/ab/eip_plc5_pccc.c)
3. [src/libplctag/protocols/ab/eip_slc_pccc.c](src/libplctag/protocols/ab/eip_slc_pccc.c)
4. [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c)
5. [src/libplctag/protocols/ab/pccc.h](src/libplctag/protocols/ab/pccc.h)
6. [src/libplctag/protocols/ab/eip_plc5_dhp.c](src/libplctag/protocols/ab/eip_plc5_dhp.c)
7. [src/libplctag/protocols/ab/eip_slc_dhp.c](src/libplctag/protocols/ab/eip_slc_dhp.c)

## 2. Architecture Map

Thin wrappers:
1. PLC5 direct ENIP PCCC vtable wrapper is in [src/libplctag/protocols/ab/eip_plc5_pccc.c](src/libplctag/protocols/ab/eip_plc5_pccc.c).
2. SLC direct ENIP PCCC vtable wrapper is in [src/libplctag/protocols/ab/eip_slc_pccc.c](src/libplctag/protocols/ab/eip_slc_pccc.c).
3. PLC5 DH+ vtable wrapper is in [src/libplctag/protocols/ab/eip_plc5_dhp.c](src/libplctag/protocols/ab/eip_plc5_dhp.c).
4. SLC DH+ vtable wrapper is in [src/libplctag/protocols/ab/eip_slc_dhp.c](src/libplctag/protocols/ab/eip_slc_dhp.c).

Common implementation:
1. Direct PCCC read/write/bit-write and DH+ read/write/bit-write live in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c).
2. Address parsing and encoding for PLC5 vs SLC/MicroLogix are in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c).
3. Protocol struct definitions (CIP+PCCC headers, DH+ headers) are in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c) and interfaces in [src/libplctag/protocols/ab/pccc.h](src/libplctag/protocols/ab/pccc.h).

Dedicated Logix-over-PCCC path:
1. Logix encapsulated PCCC typed read/write path is implemented separately in [src/libplctag/protocols/ab/eip_lgx_pccc.c](src/libplctag/protocols/ab/eip_lgx_pccc.c).

## 3. Data Model and Addressing

PCCC logical address format:
1. Data type + file number + element + optional sub-element and optional bit.
2. The address parser supports table files such as N, F, B, T, C, R, ST, and others.
3. Parsed result is represented by pccc_addr_t with file_type, file, element, sub_element, is_bit, bit, and element_size_bytes.

Address encoding styles:
1. PLC5 uses multi-level logical addressing encoding.
2. SLC/MicroLogix uses file/type/element/subelement encoding.

Implication for chunking:
1. Chunking can be done by changing element index and transfer size while keeping file type and file number stable.
2. For SLC/MicroLogix, element stepping in the encoded address is the natural mechanism.
3. For PLC5 and Logix-over-PCCC, element stepping can be combined with offset fields if needed.

## 4. Current Read Flow

### 4.1 Direct PLC5 and SLC/MicroLogix reads

Implemented by pccc_tag_read_start and pccc_check_read_status in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c).

Request build:
1. Build unconnected ENIP CPF request carrying Execute PCCC service.
2. For PLC5 read function:
   1. Command is typed command.
   2. Function is PLC5 range read.
   3. Transfer size is words.
   4. Offset field exists and is currently zero.
   5. Encoded address is appended.
   6. A data size byte is appended.
3. For SLC/MicroLogix read function:
   1. Command is typed command.
   2. Function is SLC range read.
   3. Transfer size is bytes.
   4. Encoded address is appended.

Validation behavior:
1. Code computes response overhead and rejects if expected payload cannot fit.
2. Code explicitly rejects oversize with message that PCCC does not support fragmentation.
3. Read status requires received data length to match tag size exactly.

### 4.2 Logix-over-PCCC reads

Implemented by tag_read_start and check_read_status in [src/libplctag/protocols/ab/eip_lgx_pccc.c](src/libplctag/protocols/ab/eip_lgx_pccc.c).

Request build:
1. Build unconnected ENIP CPF request.
2. Embed Execute PCCC route (class 0x67 instance 0x01).
3. PCCC command is typed command.
4. PCCC function is Logix typed read.
5. PCCC offset is present and currently set to zero.
6. PCCC transfer size is element count.
7. Encoded name and element count are appended.

Response handling:
1. Validate ENIP command and status.
2. Validate CIP general status.
3. Validate PCCC status.
4. Decode PCCC data type bytes, including nested decode when array type is returned.
5. Copy payload into tag data and capture encoded type info.

Fragmentation behavior:
1. File comment states fragments are not supported.
2. One request in flight, one full response expected.

### 4.3 DH+ bridged reads

Implemented by pccc_dhp_tag_read_start and pccc_dhp_check_read_status in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c).

Differences from direct PCCC reads:
1. Uses connected ENIP send path.
2. Prepends DH+ routing header with destination node from session dhp_dest.
3. PCCC payload format after routing header is otherwise the same style as direct PLC5 or SLC reads.
4. Same explicit no-fragment assumption and oversize rejection.

## 5. Current Write Flow

### 5.1 Direct PLC5 and SLC/MicroLogix writes

Implemented by pccc_tag_write_start and pccc_check_write_status in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c).

Request build:
1. Build unconnected Execute PCCC request.
2. For PLC5:
   1. Function is PLC5 range write.
   2. Transfer size is words.
   3. Offset is currently zero.
   4. Encoded address then write bytes are appended.
3. For SLC/MicroLogix:
   1. Function is SLC range write.
   2. Transfer size is bytes.
   3. Encoded address then write bytes are appended.

Validation behavior:
1. Request payload capacity is checked.
2. Oversize writes are rejected with too large status.
3. Write completion checks CIP and PCCC status only.

### 5.2 Logix-over-PCCC writes

Implemented by tag_write_start and check_write_status in [src/libplctag/protocols/ab/eip_lgx_pccc.c](src/libplctag/protocols/ab/eip_lgx_pccc.c).

Request build:
1. Unconnected Execute PCCC request routed through class 0x67 instance 0x01.
2. Function is Logix typed write.
3. PCCC offset exists and is currently zero.
4. PCCC transfer size is element count.
5. Encoded name, encoded type info, and write payload are appended.

Validation behavior:
1. First write may force pre-read to populate type info.
2. Oversize requests rejected up front.
3. Write status checks CIP and PCCC status.

### 5.3 DH+ bridged writes

Implemented by pccc_dhp_tag_write_start and pccc_dhp_check_write_status in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c).

Differences from direct PCCC writes:
1. Connected ENIP CPF path.
2. DH+ routing header prepended.
3. PCCC write command that follows matches PLC5 vs SLC style.
4. Same no-fragment assumption and up-front payload fit checks.

## 6. Current Bit Write Flow

### 6.1 PLC5 direct bit writes

Implemented by plc5_tag_write_bit_start in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c).

Behavior:
1. Uses PLC5 read-modify-write function.
2. Encoded address is followed by two masks:
   1. Reset mask (AND mask).
   2. Set mask (OR mask).
3. Masks are built from target bit index in current element bytes.

### 6.2 SLC/MicroLogix direct bit writes

Implemented by slc_tag_write_bit_start in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c).

Behavior:
1. Requires 2-byte element size.
2. Uses SLC range write mask function.
3. Payload includes transfer size byte, encoded address, mask bytes, then set bytes.

### 6.3 DH+ bit writes

Implemented by plc5_dhp_tag_write_bit_start and slc_dhp_tag_write_bit_start in [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c).

Behavior:
1. Same bit-mask semantics as direct writes.
2. Adds DH+ routing header at start of embedded packet.
3. Uses connected ENIP send framing.

## 7. Why Large Requests Fail Today

Current code intentionally rejects operations when full data does not fit one PCCC packet:
1. Read and write flows compare requested size against available payload after protocol overhead.
2. Comments and checks state PCCC does not support fragments.
3. In practice, request and response limits are usually around 250 bytes for PCCC payload after overhead.

This is a transport/protocol limitation in current implementation, not a hard protocol impossibility. Manual chunking can be implemented by issuing multiple legal non-fragment packets.

## 8. ENIP+PCCC Manual Chunking Design

## 8.1 Design Rules

1. Keep request and response each within negotiated ENIP explicit messaging payload budget.
2. Do not add fragmented service behavior.
3. Instead, split one logical read or write into multiple legal PCCC range requests.
4. Advance file element index per chunk.
5. Keep one outstanding chunk request per tag (same in-flight model as current code).
6. Bit writes are single-element operations and should not be chunked.

## 8.2 Shared Chunk Math

Inputs:
1. Total bytes requested: total_bytes.
2. Element size in bytes: elem_size_bytes.
3. Base file element index: base_elem.
4. Per-packet data budget from request side and response side.

Compute:
1. chunk_data_max = min(request_data_max, response_data_max).
2. Align to element boundaries:
   1. chunk_data_aligned = floor(chunk_data_max / elem_size_bytes) * elem_size_bytes.
3. If chunk_data_aligned < elem_size_bytes, fail with too large.

Loop:
1. bytes_done = 0.
2. while bytes_done < total_bytes:
   1. remaining = total_bytes - bytes_done.
   2. chunk_bytes = min(chunk_data_aligned, remaining).
   3. chunk_elem_advance = bytes_done / elem_size_bytes.
   4. chunk_elem_index = base_elem + chunk_elem_advance.
   5. Build encoded address for chunk_elem_index.
   6. Send chunk read or write for chunk_bytes.
   7. On read, copy returned bytes to destination at bytes_done.
   8. On write, copy source bytes from bytes_done into request payload.
   9. bytes_done += chunk_bytes.

## 8.3 PLC5 direct chunking details

Request encoding:
1. Re-encode PLC5 logical address each chunk with updated element index.
2. Set transfer size in words: chunk_words = chunk_bytes / 2.
3. Keep pccc_offset at zero for first implementation.
4. Keep data size byte equal to chunk_bytes where required by current packet shape.

Budget math:
1. request_data_max = cip_payload_space - direct_plc5_request_overhead.
2. response_data_max = cip_payload_space - direct_plc5_response_overhead.
3. chunk_data_max = min(request_data_max, response_data_max).
4. If element size is odd, align to element size first, then ensure transfer words are valid.

## 8.4 SLC500 and MicroLogix chunking details

Request encoding:
1. Re-encode SLC logical address each chunk with updated element index.
2. Set transfer size in bytes: chunk_bytes.
3. No pccc_offset field is used in SLC range request headers.

Budget math:
1. request_data_max = cip_payload_space - direct_slc_request_overhead.
2. response_data_max = cip_payload_space - direct_slc_response_overhead.
3. chunk_data_max = min(request_data_max, response_data_max).
4. Align chunk size to element size.

## 8.5 Logix-over-PCCC chunking details

Request encoding option A (recommended first):
1. Re-encode logical address each chunk with updated element index.
2. Keep pccc_offset at zero.
3. Set transfer size to chunk element count.
4. Include chunk element count field in payload where current shape expects it.

Request encoding option B (later optimization):
1. Keep logical address fixed.
2. Advance pccc_offset and transfer size per chunk.
3. Use only if tested against target Logix PCCC executor behavior.

Budget math:
1. request_data_max = cip_payload_space - lgx_over_pccc_request_overhead.
2. response_data_max = cip_payload_space - lgx_over_pccc_response_overhead.
3. chunk_data_max = min(request_data_max, response_data_max).
4. Convert chunk bytes to chunk element count before writing transfer fields.

## 8.6 DH+ chunking details

Apply same chunking loop as direct PLC5/SLC, but with larger overhead:
1. Include DH+ routing header bytes in request overhead.
2. Use connected ENIP CPF framing sizes for response overhead.
3. Re-encode element index per chunk exactly as direct mode.
4. Keep one chunk in flight.

## 8.7 Bit write policy

1. Do not chunk bit writes.
2. Bit write is a single-element masked operation.
3. If payload cannot fit one request, return too large.
4. For large multi-bit edits, caller should use word/array writes with chunking instead.

## 9. Pseudocode

### 9.1 Chunked read

    function chunked_pccc_read(tag):
        budgets = compute_pccc_budgets(tag)
        chunk_bytes_aligned = compute_aligned_chunk_size(tag, budgets)
        bytes_done = 0

        while bytes_done < tag.size:
            remaining = tag.size - bytes_done
            chunk_bytes = min(chunk_bytes_aligned, remaining)
            elem_advance = bytes_done / tag.elem_size
            chunk_elem = tag.base_elem + elem_advance

            req = build_read_request_for_chunk(tag, chunk_elem, chunk_bytes)
            send(req)
            resp = wait_and_validate_response(tag)

            copy resp.data into tag.data at offset bytes_done
            bytes_done += chunk_bytes

        return OK

### 9.2 Chunked write

    function chunked_pccc_write(tag):
        budgets = compute_pccc_budgets(tag)
        chunk_bytes_aligned = compute_aligned_chunk_size(tag, budgets)
        bytes_done = 0

        while bytes_done < tag.size:
            remaining = tag.size - bytes_done
            chunk_bytes = min(chunk_bytes_aligned, remaining)
            elem_advance = bytes_done / tag.elem_size
            chunk_elem = tag.base_elem + elem_advance

            req = build_write_request_for_chunk(
                tag,
                chunk_elem,
                chunk_bytes,
                source = tag.data[bytes_done : bytes_done + chunk_bytes]
            )
            send(req)
            wait_and_validate_write_ack(tag)

            bytes_done += chunk_bytes

        return OK

## 10. Integration Notes for ENIP+PCCC Module

1. Keep manufacturer-specific logic in ENIP+PCCC strategy module only.
2. Do not mix PCCC chunking rules into generic ENIP packetizer logic.
3. Generic packetizer should provide budget numbers; PCCC strategy owns chunk element math and request layout.
4. Use existing pccc logical address parser and encoder routines from [src/libplctag/protocols/ab/pccc.c](src/libplctag/protocols/ab/pccc.c) for chunk address progression.
5. Maintain existing one-request-per-tag in-flight model.
6. Surface partial progress through tag offset style state in PCCC strategy state object.

## 11. Test Matrix for New Chunking

Read:
1. PLC5 file read where total size fits one packet.
2. PLC5 file read where total size requires multiple chunks.
3. SLC integer file read across multiple chunks.
4. Logix-over-PCCC typed read across multiple chunks.
5. DH+ read across multiple chunks.

Write:
1. PLC5 write one packet and multi-chunk write.
2. SLC write one packet and multi-chunk write.
3. Logix-over-PCCC typed write one packet and multi-chunk write.
4. DH+ write one packet and multi-chunk write.

Bit write:
1. PLC5 bit write success and oversize rejection.
2. SLC bit write success and invalid-size rejection.
3. DH+ bit write success.

Failure handling:
1. PCCC status error mid-sequence stops sequence and reports failure.
2. Response too small or too large per chunk reports failure.
3. Retry policy re-sends current chunk only.

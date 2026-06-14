> **DEPRECATED.** This document predates the current EtherNet/IP design and is
> retained for historical reference only. The authoritative design is
> [ENIP-SESSION-DESIGN.md](../src/libplctag/protocols/enip/ENIP-SESSION-DESIGN.md). Do not use this document to guide new work.

# ENIP Linear Vector-First Implementation Contract

Date: 2026-05-25
Status: Decision-locked implementation contract
Primary plan: docs/enip-linear-vector-first-implementation-plan.md

## 1. Purpose

This file is the implementation contract used to execute the ENIP linear vector-first plan with minimal ambiguity for:
1. Junior engineers.
2. Small local LLM-assisted coding workflows.

This contract does not replace existing library-wide behavior. It constrains how the new ENIP path must use existing behavior.

## 2. Why This Contract Exists

Many required behaviors already exist in other library code. This contract is still required because:
1. Existing behavior is distributed across AB, Modbus, Omron, and generic library layers.
2. The ENIP linear vector-first path combines those behaviors in a new execution model.
3. We need explicit, local, phase-gated decisions so implementation and review are deterministic.
4. Existing mappings and conventions are reused; this contract prevents accidental divergence in the new path.

## 3. Locked Decisions

### 3.1 Status and Error Mapping

1. Reuse existing platform status mappings already implemented in the library.
2. Do not create a parallel status mapping layer unless a missing mapping is proven.
3. Wrapper APIs may normalize outcomes for local control flow, but final externally visible statuses remain library-consistent.

### 3.2 Restart and Wake Semantics

1. Restart state is for external wake interruption only.
2. Restart state does not survive reconnect.
3. Any socket error causes disconnect, socket close, retry wait, and fresh operation state.
4. WAKE should preserve enough local state to resume the interrupted operation in the same connection lifecycle.

### 3.3 Locking and Lifetime Rules

Rules:
1. Never hold both tag API mutex and PLC/connection mutex at the same time unless there is no other way.
2. Allowed exception: incoming calls through vtables may hold API mutex first and then obtain PLC mutex for vector insertion.
3. Do not hold any mutex across operations that can cause delays (I/O waits, condition waits, long loops, callbacks).
4. To prevent deletion while processing, acquire a reference, process, then release the reference.
5. Taking a reference is often under mutex protection; releasing does not need to be.

Required implementation guardrails:
1. Add debug assertions at key boundaries for lock-order and no-blocking-under-mutex expectations.
2. Use refcount hand-off patterns consistently around vector scans and tag processing.

### 3.4 Timeout Model

1. Application-level timeout behavior is authoritative.
2. The app may provide timeout values to plc_tag_read()/plc_tag_write() or enforce its own timing and call plc_tag_abort().
3. Internal waits should use explicit timeout arguments and a large default (30 seconds) when no tighter call-site timeout is required.
4. Internal timeout defaults must not override or weaken application-visible timeout semantics.

### 3.5 Packet Budget Rules

For multi-service packing:
1. Each request contributes 2 bytes for offset-table entry plus encoded request bytes.
2. **Unified Trimming Logic:** All PLC types (Logix, Omron, PCCC) use a shared trimming implementation.
3. **Read Trimming:** Read requests must be trimmed/altered so that the resulting read data fits into the response packet. This solves the problem once for Omron/PCCC and applies identically to AB/Logix without performance loss.
4. **Write Trimming:** Write request payloads are trimmed by sending smaller portions of write data so that the request fits into the request packet.

For response budgeting:
1. Each response contributes 2 bytes for offset-table entry plus embedded CIP response bytes.
2. Success write response: 4 bytes.
3. Failure response: 6 to 8 bytes.
4. Read response: Large, depends on requested size and clamped to remaining space in the response packet.
5. Packetizer must estimate worst-case response sizes (e.g., full buffer for large reads) to ensure total response aggregate fits in the negotiated communication size.
6. Budgeting assumes no variable-sized arrays (arrays in PLCs are fixed-size).

### 3.6 Canonical Tag Name Rules

1. Canonical tag names follow PLC hardware behavior.
2. Names and named fields are case sensitive.
3. Only program-scope namespaces apply (Program: style).
4. Array references must have either no indexes or all indexes present.
5. Other separators and formatting follow existing library behavior.
6. Program-scope handling should match existing list_tags_logix behavior.

### 3.7 Connection Error Policy

1. Any socket error is a connection-level fault: disconnect, close socket, wait retry, reconnect.
2. Retry timing should follow calc_retry_time() behavior used in AB protocol.
3. Tag-level protocol errors (for example NOT_FOUND, privilege/write-denied) are tag failures, not connection teardown triggers.
4. Read-only tags must reject write requests.

### 3.8 Metadata Strategy

1. Phase 1 must include metadata.
2. Metadata should be implemented before general tag read/write execution.
3. Read/write packing remains blocked until required metadata is available.
4. **AB Metadata (Class 0x6B):** Use Service 0x55 (GetInstanceAttributeList) against Symbol Class 0x6B. Fetch Attribute 1 (Tag Name) for all root instances.
5. **Phase 2 Details:** For deep metadata (type, size, dimensions), use attributes defined in the `@tag` implementation (see `src/libplctag/protocols/ab/eip_cip_special.[ch]`).
6. **Negative Cache:** Use a hash table (`src/utils/hashtable.h`). Clear the cache immediately on connection break or error.

### 3.9 Response Matching (Multi-Service)

1. Use Sender Context + Packet Index + Offset:
   - When using 0x0A (Multi-Service), the 8-byte Sender Context is shared by several requests.
   - Tags must record the session `sender_context` AND their own `index` and `offset` within the multi-service request frame.
   - Matching a response requires confirming both the context and the position within the multiplexed result.

### 3.10 Capability Matrix

1. Profile includes:
   - `supports_0x0A` (Multi-Service)
   - `supports_extended_fo` (Forward Open Extended)
   - `max_packet_buffer_size` (negotiated communication size)

### 3.11 Manufacturer Isolation Rule

1. Shared ENIP code may contain only manufacturer-neutral fields and flow control.
2. Shared tag/connection state may include generic fields such as `byte_offset`, pending state, deadlines, and correlation keys.
3. CIP service choices, path segment formats, request encoding, response decoding, and chunk progression logic are manufacturer specific.
4. Do not implement shared logic with PLC type branching patterns such as `if AB do X, if OMRON do Y`.
5. Manufacturer-specific behavior must be isolated behind manufacturer strategy entry points selected at connection setup.
6. New PLC behaviors must be added by implementing a new strategy module, not by extending shared code branches.

Pseudo-code (required shape):

```text
struct enip_mfg_ops {
   encode_read_chunk(tag, arena, budget_req, budget_resp) -> req_desc
   decode_read_chunk(tag, resp_bytes) -> chunk_result
   encode_write_chunk(tag, arena, budget_req) -> req_desc
   decode_write_chunk(tag, resp_bytes) -> write_result
   needs_more_read(chunk_result) -> bool
   needs_more_write(tag) -> bool
}

on_connection_start(identity):
   conn.mfg_ops = select_mfg_ops(identity)

build_cycle(tag):
   req = conn.mfg_ops.encode_read_chunk(tag, arena, budget_req, budget_resp)
   send(req)

receive_cycle(tag, resp):
   result = conn.mfg_ops.decode_read_chunk(tag, resp)
   tag.byte_offset += result.bytes_consumed
   if conn.mfg_ops.needs_more_read(result):
      schedule_again(tag)
```

AB strategy rules:
1. Read service uses 0x52 and includes byte offset after element count.
2. Request can keep full logical element count while next request offset advances by actual bytes returned.
3. Continue while CIP status indicates fragmented response.

OMRON strategy rules:
1. Use simple data segment 0x80 in request path with offset and chunk length.
2. Client must select chunk length before sending so both request and expected response fit budgets.
3. Offset advances by requested/accepted chunk progression according to strategy contract.

### 3.12 CIP Path Encode/Decode Contract (Tag and Route)

This section locks path parsing/encoding behavior for the ENIP linear path and must remain compatible with existing AB CIP parser behavior.

Tag path encode/decode rules:
1. Symbolic segment encoding uses ANSI Extended Symbol format: `0x91`, length byte, symbol bytes, and one zero pad byte when symbol length is odd.
2. Array index encoding uses element logical segments:
   - `0x28` + 1-byte value for 0..255.
   - `0x29` + 1-byte pad + 2-byte little-endian value for 256..65535.
   - `0x2A` + 1-byte pad + 4-byte little-endian value for larger values.
3. Tag decode must reverse the above forms and preserve canonical rules already locked in section 3.6.

Route path encode/decode rules:
1. Route paths are parsed as ordered pairs of `(port, link)` semantics.
2. Numeric path segments remain valid as-is.
3. Extended IP routing supports numeric forms `18,<ipv4>` and `19,<ipv4>`.
4. Aliases `A`/`a` and `B`/`b` are accepted for the extended IP port selector and are equivalent to:
   - `A` == `18`
   - `B` == `19`
5. Encoding for extended IP segment remains: one byte port selector (`18` or `19`), one byte ASCII length, ASCII IPv4 bytes, then zero padding to 16-bit boundary when needed.
6. Decode must normalize aliases to their numeric selector values for transport encoding and expose equivalent semantics.

Pseudo-code (normative intent):

```text
encode_tag_path("myTag[4].field1"):
  sym("myTag") -> 0x91 len bytes [pad if odd]
  idx(4) -> 0x28 0x04
  sym("field1") -> 0x91 len bytes [pad if odd]

encode_route_tokens(tokens):
  for each token pair:
    if token is "A"/"a": token = 18
    if token is "B"/"b": token = 19
    if token is 18 or 19 and next token is ipv4:
       emit [port][ascii_len][ascii_ip][pad_if_needed]
    else:
       emit numeric segment bytes
```

Byte-level worked examples (normative for review):

1. Tag path: `myTag[4].field1`
    - Symbol `myTag`:
       - `91 05 6D 79 54 61 67 00`
    - Index `[4]`:
       - `28 04`
    - Symbol `field1`:
       - `91 06 66 69 65 6C 64 31`
    - Encoded path bytes (without leading word count):
       - `91 05 6D 79 54 61 67 00 28 04 91 06 66 69 65 6C 64 31`
    - Encoded path bytes (with leading word count byte used by tag-name encoding):
       - `09 91 05 6D 79 54 61 67 00 28 04 91 06 66 69 65 6C 64 31`

2. Route path with alias: `1,3,A,192.168.1.2,1,0`
    - Alias normalization:
       - `A -> 18` (`0x12`)
    - Equivalent normalized route: `1,3,18,192.168.1.2,1,0`
    - Segment bytes:
       - `1,3` -> `01 03`
       - `18,192.168.1.2` -> `12 0B 31 39 32 2E 31 36 38 2E 31 2E 32 00`
       - `1,0` -> `01 00`
    - Encoded route bytes:
       - `01 03 12 0B 31 39 32 2E 31 36 38 2E 31 2E 32 00 01 00`

## 4. Existing Code Anchors (Do Not Re-Define)

These references are normative anchors for behavior reuse:
1. Retry calculation function: src/libplctag/protocols/ab/session.c (calc_retry_time).
2. Program-scope listing and naming behavior: src/tools/list_tags_logix/list_tags_logix.c.
3. Public abort semantics entrypoint: src/libplctag/lib/lib.c (plc_tag_abort).
4. Existing AB CIP tag parser grammar and segment handling: src/libplctag/protocols/ab/cip.c.
5. **Encoding/Decoding Patterns:** [src/poc/ab_server_fiber](src/poc/ab_server_fiber) uses `Bytes` functions for packet processing. Implement the **mirror image** (client-side) of these patterns.
6. **Packet Layouts:** See [src/libplctag/protocols/ab/defs](src/libplctag/protocols/ab/defs) for C-struct representations of ENIP/CIP headers and commands.

## 5. Acceptance Expectations

Primary validation target:
1. Most tests in src/tests/scripts/run_simulator_tests.sh and src/tests/scripts/run_hardware_tests.sh should pass.

Known exceptions:
1. Tests hardcoding protocol types not applicable to the new ENIP path may fail and must be explicitly listed.

Evidence required per failure:
1. Test name.
2. Reason for expected non-applicability.
3. Follow-up action (fix, defer, or waive).

## 6. Implementation Checklist (Actionable)

1. Add and wire ENIP protocol mapping aliases enip-tcp and enip_tcp.
2. Implement/land metadata phase-1 and phase-2 gating before normal read/write flow.
3. Implement wait wrappers using existing status behavior and explicit timeout arguments.
4. Enforce lock/lifetime rules with assertions and refcount hand-offs.
5. Implement packet budget formulas exactly as locked above.
6. Enforce canonical name and array-index rules from existing behavior anchors.
7. Apply socket error disconnect policy plus retry scheduling via calc_retry_time()-style logic.
8. Run simulator and hardware script suites; record expected exclusions.

## 7. Open Items Remaining

After locking decisions in this contract, what remains is execution work:
1. File-level implementation task breakdown per phase.
2. Code changes and focused tests.
3. Full simulator/hardware validation and evidence collection.
4. Performance delta documentation for merge gate.

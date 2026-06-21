> **DEPRECATED.** This document predates the current EtherNet/IP design and is
> retained for historical reference only. The authoritative design is
> [ENIP-SESSION-DESIGN.md](../src/libplctag/protocols/enip/ENIP-SESSION-DESIGN.md). Do not use this document to guide new work.

# ENIP Linear Vector-First File-by-File Coding Task List

Date: 2026-05-25
Status: Ready for implementation
Depends on:
1. docs/enip-linear-vector-first-implementation-plan.md
2. docs/enip-linear-vector-first-implementation-contract.md
3. coding_guidelines.md

## 0. Working Rules for Implementation

1. Follow the decision-locked contract exactly.
2. Keep changes small and compile after each file group.
3. Do not hold delay-prone operations under mutexes.
4. Restart state is only for WAKE in the current connection lifecycle.
5. Any socket error triggers disconnect and reconnect flow.
6. Shared ENIP code must not contain PLC-type branching logic such as `if AB ... else if OMRON ...`.
7. Shared code may reuse only manufacturer-neutral fields (for example byte offset and generic pending state).
8. Service selection, request/response encoding, and chunking behavior must be implemented per manufacturer strategy module.

## 1. Build and Protocol Registration

### 1.1 Edit src/libplctag/lib/init.c

Tasks:
1. Add ENIP header include for new constructor.
2. Add protocol map entries for enip-tcp and enip_tcp to the ENIP constructor.
3. Add ENIP module init/teardown calls in initialize_modules() and destroy_modules().
4. Keep existing protocol mappings unchanged.

Done when:
1. create-tag with protocol=enip-tcp resolves to ENIP constructor.
2. create-tag with protocol=enip_tcp resolves to ENIP constructor.

### 1.2 Edit src/libplctag/lib/lib.c

Tasks:
1. Include ENIP protocol header.
2. Extend create-from-tag protocol dispatch switch to select ENIP constructor for ENIP protocol types.
3. Keep existing AB/MB/Omron behavior unchanged.

Done when:
1. create-from-tag works for ENIP source tags.

### 1.3 Edit src/libplctag/lib/tag.h

Tasks:
1. Add ENIP protocol enum values for normal and connection-tag variants.
2. Keep numeric ordering stable unless there is a hard reason to change.
3. Update related comments to include ENIP.

Done when:
1. ENIP tag and ENIP connection tag can be identified in generic dispatch.

### 1.4 Edit src/libplctag/CMakeLists.txt

Tasks:
1. Add ENIP protocol sources to library build lists.
2. Add new src/utils files for wait wrappers and Arena/Bytes utilities.
3. Keep AB protocol source wiring untouched.

Done when:
1. plctag_dyn and plctag_static compile with new ENIP and utils files.

## 2. New ENIP Protocol Module Skeleton

### 2.1 Create src/libplctag/protocols/enip/CMakeLists.txt

Tasks:
1. Define ENIP source list variable in the same style as existing protocol CMake files.
2. Export source list to parent scope.

Done when:
1. Parent CMake can append ENIP sources cleanly.

### 2.2 Create src/libplctag/protocols/enip/enip.h

Tasks:
1. Declare ENIP public constructor and init/teardown entry points.
2. Provide forward declarations for ENIP tag and connection structures.

Done when:
1. init.c and lib.c can include this header with no warnings.

### 2.3 Create src/libplctag/protocols/enip/tag.h

Tasks:
1. Define ENIP tag struct embedding TAG_BASE_STRUCT.
2. Add ENIP operation state fields for REQUEST/RESPONSE plus metadata states.
3. Add correlation fields and metadata readiness flags.
4. Add reconnect rearm bookkeeping fields.

Done when:
1. ENIP tag state machine fields exist and compile.

### 2.4 Create src/libplctag/protocols/enip/enip_tag.c

Tasks:
1. Implement ENIP constructor that parses required attributes and allocates tag.
2. Implement vtable methods: read, write, status, abort, wake_plc, tickler, tag_data_written.
3. Ensure write requests are rejected for read-only tags.
4. Ensure protocol-level logical errors fail the tag without tearing down connection.

Done when:
1. ENIP tags can be created and moved into pending work vectors.

### 2.5 Create src/libplctag/protocols/enip/enip_connection_tag.c

Tasks:
1. Implement ENIP connection-tag support for @connection status and metrics.
2. Expose callback latency and queue-depth telemetry fields.

Done when:
1. connection-tag status transitions are observable and callbacks fire.

### 2.6 Create src/libplctag/protocols/enip/enip_conn.c

Tasks:
1. Implement the linear connection thread loop from plan section 1.2.
2. Implement connect/session bootstrap: TCP connect, Register Session, Identity, FOEx, FO fallback.
   - Reference `src/libplctag/protocols/ab/defs` for packet structs.
   - Implement client-side "mirror image" of encoding logic in `src/poc/ab_server_fiber`.
3. Implement vector-based scheduling, send, receive, match, complete, retry, idle disconnect.
   - Matching response for 0x0A requires context + multi-request index/offset.
4. Select manufacturer strategy once per connection and store function table pointer on connection object.
5. Invoke strategy hooks for request build and response decode; do not branch by PLC type in this file.
6. Enforce lock/refcount rules from contract.
7. On reconnect, rearm RESPONSE tags back to REQUEST state.

Done when:
1. Simulator can run basic read/write cycles through ENIP connection loop.
2. Reconnect simulation safely rearms pending tags.
3. No AB/OMRON branching appears in shared connection loop logic.

Pseudo-code checkpoint:

```text
conn.mfg_ops = select_mfg_ops(identity)

for tag in due_tags:
   req = conn.mfg_ops.encode_request(tag, budgets)
   if req.fits:
      packet.add(req)

for resp in packet.responses:
   result = conn.mfg_ops.decode_response(tag, resp)
   tag.byte_offset += result.bytes_progress
```

### 2.7 Create src/libplctag/protocols/enip/enip_metadata.c

Tasks:
1. Implement phase-1 metadata inventory build for all root symbols.
   - Call Service 0x55 (GetInstanceAttributeList) on Symbol Class 0x6B (Attribute 1).
2. Implement phase-2 deep metadata fetch on first seen tag/path use.
   - Reference `@tag` attributes in `src/libplctag/protocols/ab/eip_cip_special.c`.
3. Block read/write packing until required metadata exists.
4. Implement root-symbol negative cache using `src/utils/hashtable.c`.
   - Clear cache on reconnect or connection error.

Done when:
1. Metadata is required before general read/write packetization.
2. Negative cache follows invalidation rules.

### 2.8 Create src/libplctag/protocols/enip/enip_packetizer.c

Tasks:
1. Implement deterministic request budget estimation.
2. Implement response budget estimation based on operation type:
   - Success write: 4 bytes.
   - Failure: 6-8 bytes.
   - Read: Requested size (clamped to remaining packet space).
3. Enforce per-request and aggregate limits under negotiated size.
   - Overhead: 2 bytes (count) + 2 bytes per entry (offset).
4. Apply fixed size rules for packing and response estimates (arrays are fixed size).
5. Keep this file manufacturer-neutral: budget math only (no service/path/chunking decisions).
6. Trimming decisions are requested from manufacturer strategy hooks and validated against shared budgets.
7. Request entry adds 2-byte offset plus embedded request bytes.
8. Response entry adds 2-byte offset plus embedded response bytes.
9. Allow write trimming for oversized writes through strategy-specific chunk builders.
10. Reference `src/poc/ab_server_fiber` for `Bytes` usage patterns.

Done when:
1. Packing decisions are deterministic and respect both request and response budgets.
2. No AB/OMRON conditional logic exists in shared packetizer code.

### 2.9 Create src/libplctag/protocols/enip/enip_mfg_ops.h

Tasks:
1. Define manufacturer strategy interface (`encode_read`, `decode_read`, `encode_write`, `decode_write`, `needs_more`).
2. Define shared neutral structs for chunk/result descriptors.
3. Define strategy selection helper used by connection bootstrap.

Done when:
1. Shared code can call strategy hooks without manufacturer conditionals.

### 2.10 Create src/libplctag/protocols/enip/enip_mfg_ab.c

Tasks:
1. Implement AB read/write request builders with AB services and AB field ordering.
2. For reads, use AB semantics: request total element count, parse actual returned payload, advance byte offset by actual bytes returned.
3. Continue read chunks while AB response indicates fragmented status.
4. Apply fixed path encode/decode rules for AB-compatible symbolic and array segments:
   - Symbolic `0x91` + length + bytes + odd-length pad.
   - Array indexes `0x28/0x29/0x2A` by index width.

Done when:
1. AB behavior matches existing AB CIP semantics in isolated strategy tests.

### 2.11 Create src/libplctag/protocols/enip/enip_mfg_omron.c

Tasks:
1. Implement OMRON 0x80 simple data segment request path encoding.
2. Compute request chunk sizes client-side so request and expected response both fit packet budgets.
3. Advance offset according to strategy-managed chunk progression contract.

Done when:
1. OMRON chunking works without shared-code AB/OMRON branches.

### 2.12 Create src/libplctag/protocols/enip/enip_name.c

Tasks:
1. Implement canonical name handling for ENIP path encoding.
2. Enforce case sensitivity for names and fields.
3. Preserve program-scope namespace handling compatible with existing Program: behavior.
4. Enforce array index rule: either no indexes or all indexes.
5. Reuse existing separators and formatting semantics already accepted by library behavior.
6. Implement tag path encode/decode helpers that round-trip symbolic and numeric index segments.
7. Implement route token normalization helpers for pair-based parsing and alias handling:
   - `A`/`a` normalize to `18`.
   - `B`/`b` normalize to `19`.
8. Ensure extended IP route encoding emits `[port][ascii_len][ascii_ip][pad_if_needed]` for `18,<ipv4>` and `19,<ipv4>` forms.

Done when:
1. Name parsing and canonicalization follow contract constraints.
2. Tag and route encode/decode behavior matches the contract section for path segments and alias handling.

## 3. Wait Wrappers and Buffer Utilities

### 3.1 Create src/utils/enip_wait.h

Tasks:
1. Define wrapper status enum and restart-state struct.
2. Define operation kind and progress fields needed to resume after WAKE.
3. Document lifecycle rule: restart state is discarded on reconnect/error.

Done when:
1. ENIP code can include the wrapper API and compile.

### 3.2 Create src/utils/enip_wait.c

Tasks:
1. Implement socket_read_wait().
2. Implement socket_write_wait().
3. Implement socket_connect_wait().
4. Implement socket_connect_tcp_start_wait().
5. Use explicit timeout args and absolute-deadline accounting internally.
6. Use existing platform status mapping behavior.
7. Return WAKE with restart state for external wake.
8. Treat socket errors as connection-level failures.

Done when:
1. Wrapper tests pass on current platform.
2. ENIP connection loop can resume after WAKE without reconnect.

### 3.3 Create src/utils/arena.h and src/utils/arena.c

Tasks:
1. Copy and adapt Arena utility from src/poc/ab_server_fiber.
2. Keep API minimal and library-safe.
3. Add high-water tracking hooks used by ENIP before arena_reset.

Done when:
1. ENIP code can allocate and reset bounded scratch memory.

### 3.4 Create src/utils/bytes.h and src/utils/bytes.c

Tasks:
1. Copy and adapt Bytes utility from src/poc/ab_server_fiber.
2. Keep typed pack/unpack helpers required by ENIP encoder/decoder.
3. Preserve bounds checking and null-on-failure behavior.

Done when:
1. ENIP encode/decode paths can run without raw packet-buffer pointer arithmetic.

## 4. Integration and Validation Files

### 4.1 Edit src/tests/scripts/run_simulator_tests.sh

Tasks:
1. Add ENIP coverage where protocol selection is currently hardcoded.
2. Keep existing AB/MB/Omron runs unchanged.
3. Record explicitly expected exclusions.

Done when:
1. ENIP path is included in default simulator regression flow.

### 4.2 Edit src/tests/scripts/run_hardware_tests.sh

Tasks:
1. Add ENIP hardware validation entry points.
2. Keep existing hardware suites unchanged.
3. Record expected exclusions where scripts are protocol-hardcoded.

Done when:
1. ENIP can be validated on at least one modern Logix endpoint.

### 4.3 Create src/tests/test_enip_wait_wrappers/CMakeLists.txt and src/tests/test_enip_wait_wrappers/test_enip_wait_wrappers.c

Tasks:
1. Add focused tests for connect/read/write wrapper WAKE behavior.
2. Add timeout budget drift tests.
3. Add restart-state resume tests.
4. Add reconnect/error invalidation tests for restart state.

Done when:
1. Wrapper contract behavior is validated by isolated tests.

### 4.4 Create src/tests/test_enip_metadata_gating/CMakeLists.txt and src/tests/test_enip_metadata_gating/test_enip_metadata_gating.c

Tasks:
1. Verify read/write is blocked before metadata readiness.
2. Verify phase-1 then phase-2 ordering.
3. Verify negative cache key and invalidation behavior.

Done when:
1. Metadata ordering and gating requirements are test-covered.

### 4.5 Create src/tests/test_enip_packet_budgets/CMakeLists.txt and src/tests/test_enip_packet_budgets/test_enip_packet_budgets.c

Tasks:
1. Verify request and response aggregate budgeting.
2. Verify offset-table overhead accounting.
3. Verify write trimming and read non-trimming behavior.
4. Verify minimum/maximum embedded response sizing assumptions.

Done when:
1. Packetizer decisions are deterministic and validated.

### 4.6 Create src/tests/test_enip_reconnect_rearm/CMakeLists.txt and src/tests/test_enip_reconnect_rearm/test_enip_reconnect_rearm.c

Tasks:
1. Verify socket error path forces close and retry.
2. Verify RESPONSE tags rearm to REQUEST on reconnect.
3. Verify WAKE resume does not trigger reconnect.

Done when:
1. Reconnect behavior matches contract rules.

## 5. Suggested Execution Order (Small, Safe PRs)

1. PR-1: Build wiring and protocol registration (Section 1).
2. PR-2: Utility landing for Arena/Bytes and wait wrappers (Section 3).
3. PR-3: ENIP module skeleton and connection thread loop (Section 2.1-2.6).
4. PR-4: Metadata and packetizer (Section 2.7-2.9).
5. PR-5: Test additions and script integration (Section 4).

## 6. Exit Gate Checklist

1. ENIP protocol aliases are resolved and tag creation works.
2. Metadata gating is in place before normal read/write flow.
3. Socket error policy and retry behavior are correct.
4. WAKE-only restart behavior is correct.
5. Simulator and hardware scripts pass except documented exclusions.
6. Pass/fail matrix in the plan is filled with evidence.

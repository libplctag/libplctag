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
3. Implement vector-based scheduling, send, receive, match, complete, retry, idle disconnect.
4. Enforce lock/refcount rules from contract.
5. On reconnect, rearm RESPONSE tags back to REQUEST state.

Done when:
1. Simulator can run basic read/write cycles through ENIP connection loop.
2. Reconnect simulation safely rearms pending tags.

### 2.7 Create src/libplctag/protocols/enip/enip_metadata.c

Tasks:
1. Implement phase-1 metadata inventory build for all root symbols.
2. Implement phase-2 deep metadata fetch on first seen tag/path use.
3. Block read/write packing until required metadata exists.
4. Implement root-symbol negative cache with reconnect and reload invalidation.

Done when:
1. Metadata is required before general read/write packetization.

### 2.8 Create src/libplctag/protocols/enip/enip_packetizer.c

Tasks:
1. Implement deterministic request budget estimation.
2. Implement deterministic expected-response budget estimation.
3. Enforce per-request and aggregate limits under negotiated size.
4. Apply fixed size rules for packing and response estimates.
5. Request entry adds 2-byte offset plus embedded request bytes.
6. Response entry adds 2-byte offset plus embedded response bytes.
7. Embedded response minimum is 4 bytes; error maximum is 8 bytes.
8. Allow write trimming for oversized writes; do not trim read request shape.

Done when:
1. Packing decisions are deterministic and respect both request and response budgets.

### 2.9 Create src/libplctag/protocols/enip/enip_name.c

Tasks:
1. Implement canonical name handling for ENIP path encoding.
2. Enforce case sensitivity for names and fields.
3. Preserve program-scope namespace handling compatible with existing Program: behavior.
4. Enforce array index rule: either no indexes or all indexes.
5. Reuse existing separators and formatting semantics already accepted by library behavior.

Done when:
1. Name parsing and canonicalization follow contract constraints.

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

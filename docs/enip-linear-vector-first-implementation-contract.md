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
2. Read request payloads are already minimal and are not trimmed smaller.
3. Write request payloads may be trimmed by sending smaller portions of write data.

For response budgeting:
1. Each response contributes 2 bytes for offset-table entry plus embedded CIP response bytes.
2. Minimum embedded CIP response size is 4 bytes.
3. Error embedded CIP response maximum is 8 bytes.

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

### 3.8 Metadata Ordering

1. Phase 1 must include metadata.
2. Metadata should be implemented before general tag read/write execution.
3. Read/write packing remains blocked until required metadata is available.

## 4. Existing Code Anchors (Do Not Re-Define)

These references are normative anchors for behavior reuse:
1. Retry calculation function: src/libplctag/protocols/ab/session.c (calc_retry_time).
2. Program-scope listing and naming behavior: src/tools/list_tags_logix/list_tags_logix.c.
3. Public abort semantics entrypoint: src/libplctag/lib/lib.c (plc_tag_abort).
4. Existing AB CIP tag parser grammar and segment handling: src/libplctag/protocols/ab/cip.c.

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

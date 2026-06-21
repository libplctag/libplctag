# Generic EtherNetIP + Modbus Scheduler Implementation Plan

Date: 2026-05-24
Status: Proposed implementation plan
Primary source: docs/generic-eip-and-modbus-scheduler-rework-request.md

Superseded:
- This plan is superseded by docs/enip-linear-vector-first-implementation-plan.md for current execution.
- Modbus linked-list-first work is deferred; current first phase is new ENIP protocol implementation using Modbus-style vector processing and a linear blocking-like flow.

## 1. Objective

Deliver the requested scheduler and protocol rework in phases with low regression risk.

Priority order:
1. Phase 1 migrates Modbus from active_tags vector scheduling to linked-list scheduling and lands stress-testable behavior.
2. Later phases implement the new generic EtherNetIP backend and metadata model.

## 2. Current State Analysis (Code-Backed)

Current Modbus implementation in src/libplctag/protocols/mb/modbus.c:
1. Uses plc->active_tags (vector), sorted by op_time.
2. Tracks list membership via tag->in_active_vector.
3. Schedules via insert_tag_sorted, move_tag_sorted, remove_tag_from_active, and tickle_all_tags.
4. Uses one handler thread per PLC connection for scheduler and I/O.
5. Uses @connection event ring and generic tag tickler wakeups, but no dedicated per-PLC connection-tag linked list.

Implication:
- The request-list linked-list refactor is substantive and requires replacing core scheduling helpers and all call sites that currently depend on vector index movement.

## 3. Design Principles

1. Keep lock ordering stable: tag api_mutex before plc mutex where currently established.
2. Keep list pointer mutation under plc mutex only.
3. Hold temporary rc_inc reference for any tag processed outside plc mutex.
4. Use only cursor_next as persistent iterator cursor. No persistent current cursor.
5. Preserve existing behavioral semantics where not explicitly changed: request pacing, timeout handling, reconnect policy, partial read/write behavior.
6. Stage changes so each phase is independently testable and bisectable.

## 4. Phase Plan Overview

## Phase 1: Modbus Linked-List Scheduler Conversion (first delivery)
Goal:
- Replace active_tags vector scheduler with intrusive linked lists for request scheduling and @connection fanout, then run stress testing.

Exit criteria:
1. No active_tags vector dependency remains in scheduling path.
2. Request list supports concurrent add/remove during iteration with cursor_next repair.
3. rc_inc returning NULL is handled by skipping candidate and advancing cursor_next.
4. Existing Modbus simulator tests pass.
5. New list-cursor concurrency stress tests pass.
6. @connection callback latency telemetry exists and is validated against <100ms target in simulator tests.

## Phase 2: Generic EtherNetIP Core (new protocol selector and base state machine)
Goal:
- Introduce enip-tcp/enip_tcp path with session lifecycle, Get Identity, FOEx fallback, reconnect semantics, and no dependency on legacy global tickler for protocol progression.

Exit criteria:
1. Protocol mapping in init table is functional.
2. Identity-derived capability profile exists.
3. Forward Open policy implemented (FOEx then fallback).
4. Core error handling and reconnect policy match request.

## Phase 3: Packetizer and Capability-Driven Scheduling
Goal:
- Add request packing with strict negotiated-size budgeting and per-device capability gates.

Exit criteria:
1. Request/response budget checks enforce negotiated limits.
2. 0x0A unsupported downgrade behavior implemented per live connection.
3. Fragmentation remains transparent.

## Phase 4: Metadata Phase 1 and Phase 2
Goal:
- Implement two-phase metadata flow and scheduler-managed metadata operations.

Exit criteria:
1. Per-connection metadata cache exists.
2. Root symbol inventory and on-demand deep metadata work.
3. Path resolver supports nested arrays/fields/bit selector checks.
4. Read/write ops block on required metadata and fail fast on metadata errors.

## Phase 5: Vendor Extensions and @metadata Special Tag
Goal:
- Add AB and Omron extension behavior and replace legacy metadata pseudo tags with @metadata.

Exit criteria:
1. @metadata JSON format includes required top-level keys and normalized type fields.
2. Legacy pseudo tags return unsupported for new protocol.
3. AB and Omron deltas are encapsulated behind extension layer.

## Phase 6: Full Validation and Merge Gates
Goal:
- Complete simulator, fault-injection, and hardware validation matrix; enforce merge gates and performance policy.

Exit criteria:
1. Required CI fault injection coverage is green.
2. At least one modern Logix hardware endpoint passes full new protocol suite.
3. Performance delta policy documented and accepted.

## 5. Detailed Phase 1 Design (Modbus linked lists first)

## 5.1 Data Structure Changes

In modbus_plc_t:
1. Replace active_tags vector with request_list and connection_list structures.
2. Keep existing counters and connection-state atomics.

Proposed structs (in modbus.c or private header):

```c
typedef struct modbus_tag_list_t {
    struct modbus_tag_t *head;
    struct modbus_tag_t *tail;
    struct modbus_tag_t *cursor_next;
} modbus_tag_list_t;
```

```c
typedef struct modbus_connection_tag_list_t {
    struct modbus_connection_tag_s *head;
    struct modbus_connection_tag_s *tail;
    struct modbus_connection_tag_s *cursor_next;
} modbus_connection_tag_list_t;
```

In modbus_tag_t:
1. Replace single next pointer and in_active_vector with intrusive request-list links and membership flags.
2. Add list_prev to support O(1) unlink and move-to-tail.

```c
struct modbus_tag_t {
    ...
    struct modbus_tag_t *req_next;
    struct modbus_tag_t *req_prev;
    bool in_request_list;
    ...
};
```

In modbus_connection_tag_t:
1. Add intrusive links and membership flag for connection_list.

```c
typedef struct modbus_connection_tag_s {
    ...
    struct modbus_connection_tag_s *conn_next;
    struct modbus_connection_tag_s *conn_prev;
    bool in_connection_list;
    ...
};
```

## 5.2 Scheduler Traversal Model

Single persistent cursor:
1. cursor_next points to the next candidate in request_list.
2. current is a local variable only, acquired as rc_inc(candidate) under plc mutex.
3. On rc_inc success, advance cursor_next under plc mutex before unlock.
4. On rc_inc NULL, advance cursor_next and continue scan.

Removal safety:
1. Any remove operation under plc mutex checks if cursor_next equals removed node.
2. If yes, cursor_next is rebound to removed_node->next before unlink.

Fairness model:
1. Only tags that complete work move to tail.
2. Tags with no work stay in place.
3. Response state tags remain schedulable with same pending/in-flight semantics.

## 5.3 Request-List Operations

New helpers (all require plc mutex held unless otherwise specified):
1. request_list_append(plc, tag)
2. request_list_unlink(plc, tag)
3. request_list_move_to_tail(plc, tag)
4. request_list_schedule_now_or_time(plc, tag, op_time)
5. request_list_pick_next_due_tag(plc, now, out_tag, out_wait_time)
6. request_list_find_response_by_tid(plc, tid)

Behavioral mapping from current code:
1. insert_tag_sorted and move_tag_sorted are replaced by O(1) list operations plus due-time filtering in traversal.
2. remove_tag_from_active becomes request_list_unlink.
3. vector scan for response owner becomes request_list_find_response_by_tid traversal.

## 5.4 Connection-List Operations

New helpers:
1. connection_list_add(plc, conn_tag)
2. connection_list_remove(plc, conn_tag)
3. connection_list_notify_state_change(plc, status, timestamp)

Event model:
1. Keep current ring events for compatibility in first cut.
2. Add direct per-PLC connection_list walk on state-change trigger for low-latency callback dispatch.
3. Keep immediate post-create state callback behavior.

## 5.5 RC and Locking Rules in Phase 1

Rules to enforce:
1. Any pointer stored or used outside plc mutex must be protected by rc_inc.
2. Any list pointer mutation is under plc mutex only.
3. remove path order:
   - repair cursor_next if needed,
   - unlink list pointers,
   - clear membership flags,
   - drop list-held reference with rc_dec.
4. Destructor path must tolerate asynchronous cleanup-thread execution.

## 5.6 Phase 1 Code Work Breakdown

Work package A: Structure and helper scaffolding
1. Add request/connection list structs.
2. Add intrusive links and membership fields.
3. Add list helper functions and debug dump utilities.

Work package B: Replace request scheduling internals
1. Rewrite tickle_all_tags to list traversal.
2. Replace response-tag lookup logic.
3. Replace read/write start and auto-sync scheduling call sites.
4. Replace abort/destructor removal call sites.

Work package C: Integrate @connection list
1. Add connection tag add/remove at create/destructor.
2. Add triggered state fanout path and latency tracking.
3. Preserve existing callback ordering constraints.

Work package D: Cleanup
1. Remove active_tags vector creation/destruction and all vector helper usage.
2. Keep temporary compatibility logging where useful.

## 5.7 Phase 1 Test Plan (stress-focused)

Existing regression set (must pass):
1. run_simulator_tests.sh full Modbus section.
2. test_auto_sync_modbus.
3. test_modbus_multiple.
4. test_connection_tag with Modbus attributes.
5. test_connection_tag_late_join with Modbus attributes.
6. test_shutdown_modbus.
7. test_fairness Modbus mode.
8. thread_stress Modbus mode.
9. test_connection_stress Modbus mode.

New tests to add in src/tests:
1. test_modbus_list_cursor_remove_current
   - remove currently processed node while handler is mid-cycle.
2. test_modbus_list_cursor_remove_next
   - remove node pointed by cursor_next and verify no skip/corruption.
3. test_modbus_rapid_create_destroy
   - high churn create/destroy/read/write across threads, assert no deadlock/crash.
4. test_modbus_connection_latency
   - induce state changes and verify callback latency measurements; warn if >100ms.

Recommended execution pattern:
1. Targeted new tests locally first.
2. Full simulator script run.
3. Repeat selected stress tests for multiple loops to catch races.

## 5.8 Phase 1 Rollout and Safety

1. Land in small commits by work package to preserve bisectability.
2. Keep temporary debug assertions under debug build flags only.
3. If fairness regresses, gate merge until parity is restored or documented with accepted rationale.

## 5.9 Phase 1 Risks and Mitigations

Risk: Hidden coupling to vector ordering assumptions.
Mitigation: map each vector call site to explicit list semantics before removal.

Risk: Lock-order regressions.
Mitigation: retain current lock order and use try-lock path where currently relied on.

Risk: RC lifecycle bug on asynchronous destruction.
Mitigation: enforce rc_inc around every out-of-lock usage and add stress_rc_mem run in validation.

Risk: Connection callback latency regression.
Mitigation: direct list-trigger fanout plus telemetry attributes for observability.

## 6. Deliverables by Phase

Phase 1 deliverables:
1. Modbus scheduler linked-list implementation.
2. Cursor-safe removal semantics with cursor_next only.
3. New Modbus cursor/concurrency stress tests.
4. Updated test scripts/CMake entries for new tests.
5. Short implementation notes in docs describing invariants.

Phase 2-6 deliverables:
1. New enip-tcp backend and protocol selector mapping.
2. Capability and packetizer layers.
3. Metadata model and @metadata special tag.
4. Vendor extension layers and validation matrix outputs.

## 7. Suggested Execution Sequence (practical)

1. Implement Phase 1A and 1B in one branch, with old vector code removed only after list path compiles and runs.
2. Add Phase 1C connection-list callbacks and telemetry.
3. Add Phase 1D new stress tests and run full simulator suite.
4. Merge Phase 1 only after stress and fairness acceptance.
5. Start generic EtherNetIP phases after Phase 1 stabilizes.

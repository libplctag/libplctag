# ENIP Linear Vector-First Implementation Plan

Date: 2026-05-24
Status: Draft for execution
Supersedes strategy: Modbus linked-list-first refactor

Source requirements documents:
1. docs/generic-eip-and-modbus-scheduler-rework-request.md
2. docs/generic-eip-and-modbus-scheduler-implementation-plan.md (superseded plan, still used as requirement detail source)

Requirement inheritance note:
1. This ENIP-first plan is authoritative for execution order.
2. Behavioral and packing requirements defined in the two source documents remain mandatory unless this plan explicitly tightens them.

## 0.1 ENIP Requirement Traceability (Behavior and Packing)

Mandatory imported requirements:
1. Two-phase metadata model is required:
1. Phase 1 metadata at connection scope: minimal root inventory for all tags (root name plus instance identity).
2. Phase 2 metadata on first seen operation for a tag handle: deep metadata resolution for type/size/dimensions/structure path details.
2. Packetizer must estimate both request encoded size and expected response encoded size per candidate request.
3. Packing must enforce aggregate request and response budgets under negotiated connection size.
4. Read/write operations are blocked until required metadata is available.
5. Metadata retrieval is scheduler-managed, not ad hoc blocking I/O in API threads.
6. Negative metadata cache key is root symbol only, invalidated by metadata reload/reconnect policy.

Why this is required:
1. Without phase-1/phase-2 metadata, request/response budget calculations are not deterministic.
2. Deterministic budgeting is required for safe multi-request packing and response sizing.

## 0. Coding Guidelines

Read the coding_guidelines.md document and follow those standards for all code.

## 1. Highly Detailed Outline of the New Linear Flow (Replaces switch/case)

Design objective:
- Build the new enip-tcp/enip_tcp backend first.
- Keep scheduler mechanics low-risk by reusing the same vector-based active work model used by Modbus.
- Use a linear, blocking-like control flow in the connection thread so the logic is easy to read and reason about.
- Do not introduce request-object queues in this implementation path.

### 1.1 Connection Thread Top-Level Loop

Thread model:
1. One connection thread per ENIP connection object.
2. Thread owns socket progression, request scheduling, packet send/receive, response matching, and reconnection.
3. API threads only mutate tag intent and wake the thread.

Main loop structure:
1. Check terminate conditions.
2. Ensure connected session context (TCP + Register Session + identity + forward open policy).
3. Build outgoing packet(s) from active_tags vector (no request object queue).
4. Send packet(s) using blocking-like wrappers.
5. Wait for I/O or wake events through wrappers per operation (connect, read, write...).
6. Receive bytes and assemble ENIP/CIP frames.
7. Match response frames back to tags in active_tags.
8. Complete/reschedule tags in-place in active_tags.
9. Handle reconnect, retry, downgrade, and idle-disconnect transitions.
10. Repeat.

### 1.2 Linear Flow Step-By-Step (Executable Outline)

Phase A: Preconditions and wake reason
1. Read wake reason flags (external wake, timeout, socket ready, terminate).
2. If terminate, go to shutdown path.
3. If library is shutting down, go to shutdown path.

Phase B: Ensure transport/session capability state
4. If socket absent, create socket.
5. Start nonblocking connect with socket_connect_tcp_start.
6. Call wait_connect_complete wrapper until connected, timeout, wake, or fatal error.
7. On connect failure: close (not destroy!  close leaves the wake pipe intact) socket, schedule retry deadline, wait_retry_or_wake wrapper, then restart at step 4.
8. On TCP success: perform Register Session request/response.
9. On Register Session failure: hard-close and retry.
10. Perform Get Identity request/response.
11. Build capability profile from identity (manufacturer/device type first, product string second).
12. Perform phase-1 metadata inventory build for all root tags (minimal cache: root name + root identity/instance mapping).
13. Mark metadata inventory generation complete for this connection attempt.
14. Attempt Forward Open Extended once.
15. If FOEx fails, attempt old Forward Open once for this attempt.
16. If both fail: hard-close and retry.
17. Set connection status UP and notify connection-tag callbacks.
18. If reconnect happened, rearm RESPONSE-state tags back to REQUEST policy as required.

Phase C: Build outgoing work from vector
17. Capture now_ms.
18. Lock plc mutex.
19. Find first due REQUEST-state tag(s) in active_tags, preserving current vector sort semantics.
20. Stop selection when packet budget reached, in-flight limit reached, or no due tags.
21. For each selected tag candidate:
22. Take rc_inc(candidate) under plc mutex.
23. Unlock plc mutex.
24. Lock candidate api_mutex.
25. Revalidate tag still eligible (not aborted/destroying/op changed).
26. If deep metadata for this tag/path is missing, schedule metadata phase-2 operation state and skip read/write packing for this tag until metadata completes.
27. If deep metadata is available, estimate request bytes and expected response bytes for this candidate.
28. Pack only if aggregate request and response budgets remain within negotiated connection size.
29. Encode request payload into an arena-backed Bytes tx buffer (no request object allocation).
30. Record correlation fields on tag (sequence, transaction id, op state).
31. Relock plc mutex briefly to update pending counters/op ordering as needed.
32. Unlock plc mutex.
33. Unlock api_mutex.
34. rc_dec(candidate).
35. Relock plc mutex and continue scan for more due candidates.
36. Unlock plc mutex when build pass ends.

Phase D: Send
34. If tx buffer has bytes, call socket_write_wait wrapper.
35. socket_write_wait loops with nonblocking socket_write plus socket_wait_event until exact byte count is sent, timeout expires, or wake/error occurs.
36. On partial sends continue from offset.
37. On socket error or hard protocol violation, hard-close and go to reconnect path.
38. Emit send started/completed connection events.

Phase E: Wait
39. Compute next_wait_ms from earliest due REQUEST tag, pending response expectations, idle timeout, retry deadline.
40. Call wait_io_or_wake wrapper with read/write masks and timeout.
41. On wake-only return, restart at scheduling step (step 17).
42. On timeout, continue loop for periodic duties (idle checks, callback scans, retries).

Phase F: Receive and frame assembly
43. If readable, call socket_read_wait wrapper to read the EIP encapsulation header first.
44. Caller allocates an arena-backed 24-byte Bytes buffer for EIP header and passes it to socket_read_wait.
45. Parse payload length from the 24-byte EIP header.
46. Caller allocates a second arena-backed Bytes buffer sized to payload length and calls socket_read_wait again.
47. If either read is interrupted by WAKE, return immediately with restartable operation state (buffers + offsets) so the caller can resume.
48. Once full header and payload are available, dispatch by ENIP command (RegisterSession response, SendRRData, SendUnitData, UnregisterSession response, etc.).

Phase G: Match and complete tag work
48. Extract correlation key (transaction id/sequence and route context).
49. Lock plc mutex.
50. Find matching RESPONSE-state tag in active_tags (vector scan, same model as Modbus response matching).
51. If no match, mark orphan response metric and discard frame.
52. If match, rc_inc(tag) and unlock plc mutex.
53. Lock tag api_mutex.
54. Revalidate tag pending id still matches.
55. Decode protocol response into tag data and status.
56. Unlock tag api_mutex.
57. Lock plc mutex.
58. Decrement pending counter and update pacing counters.
59. Reschedule tag by op policy:
60. Auto-sync read/write tag: update next op_time and reposition in active_tags.
61. One-shot tag: remove from active_tags and set idle.
62. Unlock plc mutex.
63. rc_dec(tag).
64. Continue draining received Bytes slices before sleeping again.

Phase H: Connection-state callbacks and telemetry
65. On any status transition, enqueue or directly fan out connection callbacks.
66. Track callback latency last/max and warn if >100ms.
67. Update queue_depth and required telemetry counters.

Phase I: Idle disconnect and retry behavior
68. If no activity past inactivity timeout and no pending work, gracefully close connected session where possible.
69. Keep reconnect policy aligned with request spec.
70. If 0x0A unsupported detected, mark downgrade flag for this live connection only.

Phase J: Shutdown
71. Publish DOWN/terminated events as needed.
72. Clear pending states safely.
73. Close socket and session resources.
74. Exit thread.

### 1.3 Blocking-Like Wait Wrappers (Built on socket_wait_event)

Required wrappers:
1. socket_read_wait(sock_p s, Bytes *dst, int timeout_ms, socket_wait_state_t *io_state)
2. socket_write_wait(sock_p s, Bytes *src, int timeout_ms, socket_wait_state_t *io_state)
3. socket_connect_wait(sock_p s, int timeout_ms, socket_wait_state_t *io_state)
4. socket_connect_tcp_start_wait(sock_p s, const char *host, int port, int timeout_ms, socket_wait_state_t *io_state)
5. Optional helper: socket_wait_event_deadline(sock_p s, int events, int64_t deadline_ms)

Wrapper implementation rules:
1. Wrappers live in src/utils and wrap existing platform.h socket API calls.
2. Wrappers are thin orchestration around existing socket_create/socket_connect_tcp_start/socket_connect_tcp_check/socket_wait_event/socket_read/socket_write.
3. Wrappers do not alter underlying socket semantics; they only normalize retry, wait, wake, and deadline handling.
4. Timeout inputs are explicit on every call; wrappers do not read protocol defaults.
5. Event mask composition is internal to wrapper implementation (callers do not pass event masks for routine read/write/connect waits).

socket_read_wait behavior:
1. Goal: read exactly dst.len bytes unless timeout, wake, disconnect, or error occurs.
2. First call socket_read(s, dst.data+offset, remaining, 0).
3. If bytes read > 0, advance offset and continue immediate zero-timeout reads.
4. If read would block/timeout with 0 timeout, call socket_wait_event for CAN_READ plus default events and remaining timeout budget.
5. If wait reports WAKE_UP, return immediately with wake/interrupted status and current offset in io_state so caller can restart.
6. If wait reports DISCONNECT or ERROR, return corresponding error.
7. If overall timeout budget expires before exact dst.len, return timeout with offset recorded in io_state.

socket_write_wait behavior:
1. Goal: write exactly src.len bytes unless timeout, wake, disconnect, or error occurs.
2. First call socket_write(s, src.data+offset, remaining, 0).
3. If bytes written > 0, advance offset and continue immediate zero-timeout writes.
4. If write would block/timeout with 0 timeout, call socket_wait_event for CAN_WRITE plus default events and remaining timeout budget.
5. If wait reports WAKE_UP, return immediately with wake/interrupted status and current offset in io_state so caller can restart.
6. Handle DISCONNECT, ERROR, and timeout identically to socket_read_wait policy.

socket_connect_wait behavior:
1. Assumes socket_connect_tcp_start was already called.
2. Repeatedly call socket_connect_tcp_check(s, 0) first.
3. If check returns pending, wait with socket_wait_event for CONNECT plus default events and remaining timeout budget.
4. On CONNECT event, re-run socket_connect_tcp_check(s, 0) and return final status.
5. On WAKE_UP, return interrupted status with restartable connect wait state in io_state.
6. On timeout budget expiry, return timeout.

socket_connect_tcp_start_wait behavior:
1. Call socket_connect_tcp_start(s, host, port).
2. If immediate success, return OK.
3. If pending, call socket_connect_wait(s, timeout_ms).
4. On any error, return underlying status code.

Wrapper behavior contract:
1. All wrappers can return early on external wake (socket_wake called from API thread).
2. All wrappers preserve absolute-deadline semantics (avoid timeout drift).
3. All wrappers classify outcomes into: OK, TIMEOUT, WAKE, SOCKET_ERROR, REMOTE_CLOSED, TERMINATE.
4. Wrappers do not own business logic; they only normalize wait/read/write/connect behavior.
5. Wrappers return sufficient restart state (current buffer offset and phase) through socket_wait_state_t.
6. All packet I/O buffers are represented as Bytes; wrappers do not expose raw packet-buffer pointers.

Suggested restart-state shape:
1. status
2. buffer (Bytes) for in-progress read/write
3. bytes_done (cursor within buffer)
3. deadline_ms
4. operation_kind (READ/WRITE/CONNECT)
5. optional sub-phase for connect start/check

### 1.4 Arena and Bytes Buffer Lifecycle (ab_server_fiber style)

Required buffer model:
1. Move Arena and Bytes utilities into src/utils (copied from src/poc/ab_server_fiber as first step) and use them as template for ENIP request/response buffer handling.
2. Keep one fixed-capacity arena per connection thread for protocol frame assembly/parsing scratch space.
3. Cap memory by arena capacity; on overflow fail request cleanly instead of heap growth.
4. Initial arena capacity is 32KB.
5. Log arena high-water usage at DEBUG_DETAIL immediately before each arena_reset.
6. Use Bytes for all packet buffers and all encode/decode paths; do not use raw uint8_t* packet buffers in protocol logic.

Per-cycle usage:
1. Before building a new request packet, call arena_reset on tx arena.
2. Build ENIP/CIP payload using bytes_pack and related helpers into arena-backed Bytes.
3. Send with socket_write_wait.
4. After send completion (or send failure), reset arena again before receive parse work.
5. Allocate 24-byte arena-backed Bytes for EIP header; read via socket_read_wait.
6. Parse payload length, allocate second arena-backed Bytes for payload, read via socket_read_wait.
7. Decode using Bytes helpers (bytes_unpack plus bytes_slice/bytes_skip); encode using bytes_pack/bytes_pack_into.
8. Reset arena after response processing completes.

Why this model:
1. Bounded memory with predictable peak usage.
2. Very low allocator churn in hot loop.
3. Cleaner encode/decode logic than manual offset arithmetic.

### 1.5 Why this linear flow is lower risk now

1. Reuses proven active_tags vector scheduling approach.
2. Avoids simultaneous architectural shifts (no list migration + new protocol in same first phase).
3. Removes opaque state-machine jumps and centralizes progression in one readable path.
4. Makes external wake handling explicit and uniform via wrappers.

## 2. Phased Delivery Plan (ENIP First, Vector-Based)

## Phase 0: Baseline and Scaffolding

Goals:
1. Create new protocol module skeleton (enip generic core).
2. Wire protocol mapping for enip-tcp and enip_tcp.
3. Establish per-connection object with active_tags vector and thread lifecycle copied from Modbus shape.

Deliverables:
1. New protocol files and constructor entrypoint.
2. init.c mappings for enip-tcp and enip_tcp.
3. Basic create/destroy and wake plumbing.

Exit criteria:
1. Tag create for protocol=enip-tcp reaches constructor.
2. Connection thread starts and terminates cleanly.

## Phase 1: Linear Wait Wrapper Layer

Goals:
1. Implement socket_read_wait/socket_write_wait/socket_connect_wait/socket_connect_tcp_start_wait wrappers around existing platform socket APIs.
2. Ensure wrappers support external wake and absolute deadlines.

Deliverables:
1. Wrapper API and implementation in src/utils, consuming platform.h socket APIs.
2. Unit-style tests or focused simulator tests for wake timeout and reconnect behavior.

Exit criteria:
1. External wake interrupts waits deterministically.
2. Connect/read/write wrappers behave identically on posix and windows contracts.

## Phase 2: Linear Session Flow (No request queue)

Goals:
1. Implement linear control flow for: TCP connect, Register Session, Get Identity, FOEx fallback, ready state.
2. Build connection-scope phase-1 metadata inventory (minimal root tag identity map).
3. Use active_tags vector for request scheduling and response matching.

Deliverables:
1. Connection loop implementing steps in section 1.2.
2. Response matching and reschedule logic without queue objects.
3. Scheduler-managed metadata phase-2 trigger on first seen tag operation.

Exit criteria:
1. Basic read/write flows complete against simulator/hardware.
2. Reconnect path resets pending RESPONSE tags safely.
3. First seen tag operations enter metadata phase-2 before read/write packing when required metadata is absent.

## Phase 3: Capability and Packetization

Goals:
1. Add capability profile derivation from identity.
2. Add packet budget logic and selective multi-request packing using deterministic metadata-backed size estimates.
3. Add 0x0A downgrade behavior (connection-local, nonpersistent).

Deliverables:
1. Capability profile structure.
2. Packetizer integrated into build phase.

Exit criteria:
1. Budget limits enforced by negotiated size for both request and expected response aggregates.
2. Unsupported 0x0A downgrades correctly.

## Phase 4: Metadata Scheduler Integration

Goals:
1. Add metadata operation states into same vector pipeline.
2. Keep metadata retrieval scheduler-managed, not API-thread blocking I/O.
3. Preserve phase-1 inventory + phase-2 on-demand deep metadata model from source requirements.

Deliverables:
1. Metadata op states and dependencies in tag lifecycle.
2. Root-negative-cache by symbol and reconnect invalidation.

Exit criteria:
1. Read/write blocks on metadata readiness.
2. Metadata failures surface immediately.
3. Negative cache behavior matches root-symbol key and invalidation policy requirements.

## Phase 5: @metadata and Vendor Extensions

Goals:
1. Implement @metadata unified tag behavior.
2. Add AB and Omron extension hooks over common core.

Exit criteria:
1. Required JSON schema produced.
2. Legacy pseudo tags rejected for new protocol.

## Phase 6: Full Validation and Merge Gate

Goals:
1. CI fault injection and simulator coverage.
2. Hardware validation on at least one modern Logix endpoint.

Exit criteria:
1. Required matrix passes.
2. Performance delta documented per policy.

## 3. Detailed Design Decisions for Vector-Based ENIP Core

Vector semantics (same model as Modbus):
1. active_tags contains only pending or scheduled operations.
2. op_time controls due ordering.
3. REQUEST and RESPONSE states are in-tag state fields.
4. One-shot ops retire from vector after completion.
5. Auto-sync ops remain scheduled and repositioned.
6. Duplicate insertion is warned and ignored.

No request queue objects in first cut:
1. Request metadata lives in tag state and arena-backed Bytes tx/rx buffers.
2. No heap queue of per-request structs.
3. Correlation key is recorded directly on tag.

## 4. Interface Sketch for Wrapper and Loop APIs

Wrapper result enum:
1. ENIP_WAIT_OK
2. ENIP_WAIT_TIMEOUT
3. ENIP_WAIT_WAKE
4. ENIP_WAIT_REMOTE_CLOSED
5. ENIP_WAIT_SOCKET_ERROR
6. ENIP_WAIT_TERMINATE

Core functions:
1. enip_conn_loop(enip_conn_p conn)
2. enip_conn_ensure_connected(enip_conn_p conn)
3. enip_conn_build_tx_from_vector(enip_conn_p conn, int64_t now)
4. enip_conn_send_tx(enip_conn_p conn)
5. enip_conn_wait_io(enip_conn_p conn, int64_t now)
6. enip_conn_receive_frames(enip_conn_p conn)
7. enip_conn_process_frames(enip_conn_p conn)
8. enip_conn_handle_disconnect(enip_conn_p conn, int reason)

## 5. Question Burn-Down (No Unanswered Questions)

Format:
- Question
- Answer
- Confidence
- Validation action

1. Should we do Modbus linked-list changes first?
- Answer: No. Deferred. ENIP starts first using Modbus-like vector scheduling to reduce simultaneous risk.
- Confidence: High.
- Validation action: Keep Modbus list migration in separate future proposal.

2. Should ENIP first cut use request object queues?
- Answer: No. Use in-tag op state + vector + thread-local buffers.
- Confidence: High.
- Validation action: Code review gate disallows new queue-object allocations in ENIP scheduler path.

3. How do we preserve easy-to-read control flow?
- Answer: Single linear loop with function blocks per phase instead of large switch/case state machine.
- Confidence: High.
- Validation action: Enforce phase-ordered function call sequence and trace logs.

3a. Do we inherit metadata and packing requirements from source docs?
- Answer: Yes. Phase-1 minimal inventory for all roots and phase-2 first-seen deep metadata are mandatory; packet budgets require metadata-backed request/response size estimates.
- Confidence: High.
- Validation action: Add phase gates that fail if read/write can bypass required metadata.

4. How do waits become blocking-like but externally wakeable?
- Answer: Wrappers call socket_wait_event internally and return WAKE on socket_wake.
- Confidence: High.
- Validation action: Add wake-interrupt tests for connect wait, read wait, write wait, retry wait.

5. Do we use blocking socket mode?
- Answer: No explicit blocking mode change required; wrappers provide blocking-like semantics with deadline loops.
- Confidence: Medium.
- Validation action: Measure CPU and latency vs true blocking mode in test harness.

6. What timeout model do wrappers use?
- Answer: Absolute deadlines to avoid drift across repeated partial waits.
- Confidence: High.
- Validation action: Timeout accuracy test under repeated wake/partial I/O.

7. How is pending response correlation done?
- Answer: Tag-local correlation ids (session seq/transaction ids) and vector scan match, same style as Modbus.
- Confidence: High.
- Validation action: Fault-injection with orphan and late frames.

8. How do we handle tag destruction mid-processing?
- Answer: rc_inc before dropping plc mutex, api_mutex guarded tag mutation, rc_dec after work.
- Confidence: High.
- Validation action: Stress create/destroy parallel with heavy reads/writes.

9. How do we avoid deadlock between plc mutex and api_mutex?
- Answer: Never hold both at once for blocking operations; use refcount hand-off pattern.
- Confidence: High.
- Validation action: Static review checklist and runtime lock-order assertions in debug builds.

10. What should happen to RESPONSE tags after reconnect?
- Answer: Move back to REQUEST state and clear pending correlation ids for retransmit eligibility.
- Confidence: High.
- Validation action: Reconnect simulation while requests are in-flight.

11. Should we support request pipelining in first cut?
- Answer: Yes, bounded by max_requests_in_flight and negotiated packet budgets.
- Confidence: Medium.
- Validation action: Throughput/latency benchmark across 1,2,4,8 in-flight levels.

12. Should first cut enable 0x0A Multi-Service packing?
- Answer: Yes when capability says supported, else single-request mode; downgrade on runtime unsupported.
- Confidence: Medium.
- Validation action: Capability-matrix tests with simulated unsupported responses.

13. Where do we source capability profile initially?
- Answer: Identity mapping priority: manufacturer/device_type, then product string.
- Confidence: High.
- Validation action: Hardware identity capture and mapping audit.

14. Do we require active capability probes in first cut?
- Answer: No; runtime fallback/downgrade handles surprises.
- Confidence: High.
- Validation action: Track fallback counters and unexpected-downgrade logs.

15. Should metadata be in first executable ENIP milestone?
- Answer: No. First milestone is read/write core with vector scheduler and linear loop.
- Confidence: High.
- Validation action: Phase gate blocks metadata scope creep before core stabilization.

16. How are metadata misses cached?
- Answer: Root symbol negative cache per connection, invalidated on reconnect/metadata reload.
- Confidence: High.
- Validation action: Metadata tests for repeated missing symbol lookup behavior.

17. How do we handle callback latency target (<100ms)?
- Answer: Track callback_latency_last_ms and callback_latency_max_ms and warn if strictly above 100ms.
- Confidence: High.
- Validation action: Add latency instrumentation tests using induced state transitions.

18. Should connection callback fanout be tickler-thread dependent?
- Answer: No for new ENIP path. Connection thread drives status updates directly or through lightweight shared event mechanism.
- Confidence: Medium.
- Validation action: Measure callback delay with and without tickler dependency.

19. Can one loop handle both metadata and data ops later?
- Answer: Yes, by extending op state enum and using same active_tags vector scheduling policy.
- Confidence: Medium.
- Validation action: Prototype metadata states on small subset before full rollout.

20. What is best tx/rx buffer ownership model?
- Answer: Connection-thread-owned arena-backed Bytes buffers only (header/payload/request scratch).
- Confidence: High.
- Validation action: Heap allocation telemetry under stress.

21. Should we parallelize parsing and socket read across threads?
- Answer: No in first cut. Single-thread-per-connection keeps ordering and simplicity.
- Confidence: High.
- Validation action: Revisit only if profiling shows clear bottleneck.

22. What should be done when wrapper returns WAKE during a long send?
- Answer: Continue respecting deadline and retry send unless terminate/abort conditions force exit.
- Confidence: Medium.
- Validation action: Add partial-send + wake race tests.

23. Are platform wrappers identical between posix and windows?
- Answer: Interface identical, internals differ. ENIP wrapper layer normalizes outcome semantics.
- Confidence: High.
- Validation action: Run same wrapper test matrix on both platforms in CI.

24. Should enip protocol require plc attribute in tag path?
- Answer: No; protocol string selects implementation and identity drives capability.
- Confidence: High.
- Validation action: Constructor validates no plc attr dependency.

25. Do we have unresolved architecture questions?
- Answer: Yes. See Drill-Down Questions section.
- Confidence: High.
- Validation action: Resolve each question before coding wrapper and memory layers.

## 6. Drill-Down Questions Requiring Decisions

Status: resolved.

1. socket_read_wait and socket_write_wait return shape:
- Decision: return status plus restart state via socket_wait_state_t (includes bytes_completed).

2. Zero-timeout nonblocking behavior assumptions:
- Decision: confirmed.

3. Wake policy during in-progress read/write:
- Decision: return immediately with WAKE status and restart state.

4. Connect wrapper placement:
- Decision: place wrappers in src/utils.

5. Arena source integration method:
- Decision: copy to src/utils now.

6. Arena sizing strategy:
- Decision: 32KB initial size; log high-water at DEBUG_DETAIL before reset.

7. Receive buffer ownership split:
- Decision: read EIP header into 24-byte arena-backed Bytes, then allocate/read payload-sized arena-backed Bytes, and keep encode/decode in Bytes helpers only.

8. Timeout budget source:
- Decision: explicit timeout args only.

9. Event mask composition:
- Decision: internal event masks.

10. API compatibility rollout:
- Decision: ENIP-only first; AB/Omron later via ENIP code path; Modbus later.

## 7. Phase-1 Execution Checklist (Immediate)

1. Add protocol mapping for enip-tcp and enip_tcp.
2. Create ENIP connection and tag scaffolding with active_tags vector.
3. Implement wait wrapper layer and wake semantics tests.
4. Implement linear connect/session flow (TCP, Register Session, Identity, FO policy).
5. Implement vector-based request build/send/receive/match/complete loop.
6. Add reconnect and response-rearm behavior.
7. Add telemetry and callback latency warnings.
8. Run simulator + targeted hardware smoke tests.
9. Freeze and review before metadata/features expansion.

## 8. Acceptance Checklist (Requirement-to-Validation Map)

Protocol selector and bootstrap:
1. enip-tcp and enip_tcp map to new ENIP constructor in init table.
2. New path does not require plc attribute for tag creation.
3. Connection bootstrap sequence executes in order: TCP connect -> Register Session -> Get Identity -> FOEx attempt -> FO fallback when needed.

Linear wrapper behavior:
1. socket_read_wait, socket_write_wait, socket_connect_wait, and socket_connect_tcp_start_wait are implemented in src/utils and consume existing platform socket_* APIs.
2. Wrappers return WAKE immediately when socket_wake is received.
3. Wrappers return restartable state (Bytes buffer plus bytes_done cursor and phase markers).
4. Wrapper timeouts are explicit call arguments, not implicit connection defaults.
5. Wrapper event masks are internally composed.

Bytes and arena memory model:
1. All packet buffers are Bytes objects.
2. All packet encode/decode paths use bytes_* helpers (pack/unpack/slice/skip), not raw packet-buffer pointer arithmetic in protocol logic.
3. Arena/Bytes utilities are available in src/utils for ENIP use.
4. Connection arena size defaults to 32KB.
5. Arena high-water usage is logged at DEBUG_DETAIL immediately before arena_reset.

Receive framing behavior:
1. EIP header is read into a 24-byte arena-backed Bytes.
2. Payload length is parsed from header.
3. Payload is read into a newly allocated arena-backed Bytes sized to parsed payload length.
4. WAKE during either read returns immediately with restart state and no state corruption.

Vector scheduler behavior:
1. active_tags vector remains scheduling source (no request-object queue).
2. Read/write requests are selected by due time and in-flight constraints.
3. Response matching is done against RESPONSE-state tags via correlation fields.
4. On reconnect, RESPONSE-state tags are rearmed safely back to REQUEST logic.

Metadata and packing dependency:
1. Phase-1 metadata inventory is built for all root tags (minimal root identity map).
2. Phase-2 deep metadata retrieval is triggered on first seen operation for a tag/path when details are missing.
3. Read/write packing is blocked until required metadata is available.
4. Packetizer computes and enforces both request and expected-response aggregate budgets under negotiated connection size.
5. Negative metadata cache key is root symbol only and invalidates on metadata reload/reconnect policy.

Connection behavior and error policy:
1. Protocol-level logical errors complete the affected request/tag without tearing down healthy connection.
2. Socket/framing/state-corruption faults force hard close and reconnect.
3. Idle timeout follows graceful disconnect policy when possible.
4. 0x0A unsupported detection downgrades only the live connection (nonpersistent across reconnect/process restart).

Telemetry and callbacks:
1. @connection status callbacks propagate state transitions from ENIP connection thread path.
2. callback_latency_last_ms and callback_latency_max_ms are reported.
3. WARN is emitted when callback latency is strictly greater than 100ms.
4. queue_depth and required first-cut telemetry fields are exposed per policy.

Validation execution gates:
1. Simulator coverage includes capability-matrix behaviors and reconnect/fault-injection minimums from source requirements.
2. Hardware gate includes at least one modern Logix-class endpoint full-suite pass before merge.
3. FOEx-fallback behavior is explicitly validated on an endpoint expected to require old Forward Open.
4. Performance delta is measured and documented against merge policy.

## 9. Pass/Fail Matrix Template

Use this matrix during implementation review, test execution, and merge signoff.

| Area | Requirement | Validation Method | Status | Evidence | Notes |
| --- | --- | --- | --- | --- | --- |
| Protocol mapping | `enip-tcp` and `enip_tcp` resolve to new ENIP constructor | Targeted create-tag test | TODO |  |  |
| Bootstrap flow | TCP connect -> Register Session -> Get Identity -> FOEx -> FO fallback if needed | Debug trace + simulator/hardware test | TODO |  |  |
| Wrapper wakeability | `socket_*_wait` wrappers return WAKE immediately and preserve restart state | Focused wrapper tests | TODO |  |  |
| Wrapper restart state | Restart state includes `Bytes` buffer and cursor/progress | Focused wrapper tests | TODO |  |  |
| Explicit timeouts | All wrapper calls use explicit timeout args | Code review + targeted tests | TODO |  |  |
| Internal event masks | Wrapper event masks are internal only | Code review | TODO |  |  |
| Bytes-only buffers | All packet I/O buffers are `Bytes` | Code review | TODO |  |  |
| Bytes-only encode/decode | All encoding/decoding uses `bytes_*` helpers | Code review + trace review | TODO |  |  |
| Arena placement | `Arena`/`Bytes` utilities live in `src/utils` | Build + code review | TODO |  |  |
| Arena sizing | ENIP connection arena defaults to 32KB | Code review + runtime trace | TODO |  |  |
| Arena telemetry | High-water usage logged at `DEBUG_DETAIL` before reset | Debug run | TODO |  |  |
| EIP header read | 24-byte header read into arena-backed `Bytes` | Focused receive test | TODO |  |  |
| Payload read | Payload-length-derived `Bytes` allocation and read works | Focused receive test | TODO |  |  |
| Wake during read | WAKE during header/payload read returns restartable state | Focused wrapper/integration test | TODO |  |  |
| Vector scheduling | `active_tags` vector remains scheduler source | Code review + scheduler tests | TODO |  |  |
| Response matching | RESPONSE-state matching via correlation fields works | Integration test | TODO |  |  |
| Reconnect rearm | In-flight RESPONSE tags rearm safely after reconnect | Reconnect fault test | TODO |  |  |
| Metadata phase 1 | Minimal root inventory is built for all root tags | Hardware/simulator-supported test | TODO |  |  |
| Metadata phase 2 | Deep metadata fetch triggers on first seen tag operation | Metadata integration test | TODO |  |  |
| Metadata gating | Read/write packing is blocked until required metadata is present | Metadata gating test | TODO |  |  |
| Request budgeting | Request encoded-size budget enforced | Packetizer test | TODO |  |  |
| Response budgeting | Expected response-size budget enforced | Packetizer test | TODO |  |  |
| 0x0A downgrade | Unsupported multi-service downgrades only live connection | Capability/fault test | TODO |  |  |
| Protocol error policy | Logical/tag errors do not tear down healthy connection | Protocol error test | TODO |  |  |
| Hard-fault policy | Socket/framing corruption triggers hard close and reconnect | Fault injection test | TODO |  |  |
| Idle disconnect | Graceful idle disconnect behavior works | Idle timeout test | TODO |  |  |
| Callback latency | `@connection` callbacks stay within target or warn when >100ms | Callback latency test | TODO |  |  |
| Telemetry fields | Required connection telemetry attributes are exposed | Attribute test | TODO |  |  |
| Simulator gate | Required simulator matrix passes | CI/local simulator run | TODO |  |  |
| Hardware gate | At least one modern Logix endpoint passes full suite | Hardware run | TODO |  |  |
| FOEx fallback gate | Old Forward Open fallback validated on expected endpoint | Hardware run | TODO |  |  |
| Performance gate | Performance delta measured and documented | Benchmark run | TODO |  |  |

Status values:
1. `TODO` = not yet run or not yet evaluated.
2. `PASS` = requirement satisfied with recorded evidence.
3. `FAIL` = requirement not satisfied; merge blocked unless explicitly waived.
4. `WAIVED` = temporarily accepted with documented rationale and follow-up issue.

# Generic EtherNetIP + Modbus Scheduler Rework Request

Date: 2026-05-21

## 1. Context

This library exposes a common public API for multiple industrial protocols through libplctag.h.

Current implementation families:

- Allen-Bradley and Omron EtherNetIP paths use a front-end request object queue plus a back-end processor model.
- Modbus uses a simpler single back-end loop model that tickles tags and performs I/O in one thread per connection.

Primary motivation:

- Reduce complexity and memory pressure.
- Improve thread-safety reasoning.
- Avoid unbounded request object growth.
- Preserve quick connection-state propagation.

Terminology assumption for this document:

- @connection is the canonical special tag name.
- Any historical @device naming is considered already migrated outside this document scope.

## 2. Current Code Findings (Ground Truth)

Based on current source:

- Modbus currently uses one handler thread per PLC/connection and does scheduling/I/O in that same thread in src/libplctag/protocols/mb/modbus.c.
- Modbus currently stores scheduled work in active_tags (vector), sorted by op_time, and only tags with pending/scheduled work are present.
- Modbus already has a special connection-state tag implementation with a ring-buffer event mechanism and connection-state events.
- Modbus standard tags use skip_tickler=1, so generic tag tickler scheduling is bypassed for standard Modbus tags.
- AB/Omron connection/session code still uses request vectors and wakes the global tickler thread for completion/event processing.
- AB special pseudo tag behavior for @tags and @udt exists in AB protocol code.

Implication: Part of the requested behavior is already partially present in Modbus (single-thread I/O+tickle and connection-event model), but the requested list structure and metadata flow changes are not yet implemented.

## 3. Requested Change A: Modbus Tag Scheduler Rework

### 3.1 Goal

Replace the active-only vector model with linked lists and preserve current fairness behavior while improving connection-tag responsiveness and preserving RC semantics.

### 3.2 Functional Requirements

1. Use two connection-owned linked lists:
  - request list: tags with outstanding or scheduled requests (including auto-timed requests),
  - connection list: all @connection tags.
2. Scheduler iteration remains single-threaded (PLC handler thread).
3. Tag insertion/removal may occur concurrently from API threads under mutex protection.
4. Cursor-safe removal behavior:
   - If removed tag == current cursor, advance cursor safely.
   - If removed tag == next cursor target, rebind next cursor to next valid element.
5. Must preserve reference-count lifecycle correctness with rc_alloc/rc_inc/rc_dec.
6. Connection tag callback emission latency target is <100ms after connection state changes.

### 3.3 Data Structures

Per PLC connection:

- request_list:
  - head
  - tail
  - cursor_current
  - cursor_next
- connection_list for @connection tags, same fields.

Per tag:

- next pointer (and optional prev pointer if doubly-linked is selected).
- flags for in_list type and pending op state.

Rationale for two lists:

- A single list would require scanning all tags each cycle and is not efficient.
- The request_list allows focused scheduling only for tags that need service.
- The connection_list can be iterated quickly on state changes for low callback latency.

### 3.4 Scheduler Policy and Fairness

- On each loop cycle:
  1. Process request_list with current Modbus fairness semantics.
  2. Build/send requests and receive/process responses.
  3. Process connection_list only when triggered by connection-state change events.
- Additional trigger: run one immediate connection_list scan after @connection tag creation to emit initial current-state callback.
- Callback latency target is soft (<100ms preferred).
- If callback latency is strictly >100ms, emit WARN each time (no rate limit by default), including measured latency and connection identifier.

Fairness requirement (from existing behavior):

- Tags completing requests are moved later in the request_list.
- Outstanding requests naturally reach the front and are processed.
- If two tags have the same periodicity, they should converge to equal completed operation counts over time.
- Existing fairness test programs remain acceptance criteria.

### 3.5 Thread-Safety and RC Rules

- PLC mutex protects list structure and cursor mutation.
- API mutex on tag continues to guard tag-local state transitions.
- Lock ordering must stay consistent to avoid deadlock.
- Any pointer captured from a list and used outside mutex scope must hold a temporary rc_inc reference.
- Removal path must:
  - update list links,
  - repair cursor pointers,
  - clear list-membership flags,
  - then allow rc_dec flow.

### 3.6 Compatibility / Migration Notes

- Keep current behavior for request pipelining limits, timeout handling, and reconnection semantics.
- Preserve existing callback/event semantics for Modbus tags and @connection tags.
- Preserve ability to handle partial read/write requests.

## 4. Requested Change B: New Generic EtherNetIP Backend Model

### 4.1 Goal

Add an additional EtherNetIP implementation that uses the Modbus-style single-thread-per-connection scheduler model and no separate global tickler dependency for protocol I/O progression.

Protocol selector requirements:

- Add enip-tcp and enip_tcp mappings in src/libplctag/lib/init.c lookup table.
- PLC type must not be required in tag attributes for this implementation.
- Connection startup must use Get Identity data to populate capability/profile state.
- New behavior is selected by protocol string only.
- Existing protocol mappings remain unchanged in first cut:
  - ab-eip / ab_eip continue to use legacy AB path.
  - omron-njnx / omron_njnx continue to use legacy Omron path.
- First PR for enip-tcp/enip_tcp must not be blocked on AB/Omron naming alignment work.

### 4.2 Required Core Protocol Sequence

Per connection:

1. Open TCP socket.
2. Register EtherNetIP session.
3. Perform Get Identity.
4. Derive PLC capability profile from identity (using lookup data from identity fields such as manufacturer, device type, and product identity strings as needed).
5. Attempt Forward Open Extended/Long; fallback to old Forward Open where needed.
6. Maintain connected messaging where supported.
7. Support request packing via CIP Multi-Service (0x0A) where supported.
8. Handle idle timeout graceful disconnect.
9. On socket/protocol hard fault, hard close and reconnect.

### 4.3 Multi-Request Packing Constraints

For each candidate request in a packet:

- Must estimate request encoded size.
- Must estimate expected response encoded size.
- Aggregate request and response budget must stay under negotiated connection size.
- Pack only requests valid for current PLC capability profile.

This requires deterministic metadata before first packed read/write on a tag.

### 4.4 Metadata Strategy (Two-Phase)

Phase 1 (enumeration cache):

- Minimal inventory: root tag instance ID + root tag name.

Phase 2 (on-demand deep metadata):

- Triggered only when an application first creates/uses a specific tag handle.
- Resolve root name -> instance ID.
- Fetch type, size, dimensions, structure references, and nested field metadata as needed.

Path resolution must support expressions like:

name=MyArrayTag[42][10].field1.field6[12].1

Resolution algorithm:

1. Parse root symbol and array subscripts.
2. Resolve root metadata and root type.
3. Traverse UDT fields/sub-elements and index segments.
4. Determine final target type and byte/bit addressability.
5. If terminal ".bit" selector is used, permit only integral scalar terminal types; otherwise return error.

Metadata scheduling requirement:

- Metadata retrieval must be scheduler-managed, not blocking ad hoc I/O in API thread paths.
- Extend operation state model beyond READ_REQUEST/READ_RESPONSE and WRITE_REQUEST/WRITE_RESPONSE.
- Minimum planned additions are metadata request/response states; finer-grained metadata substates are allowed when needed.
- Read/write completion is blocked until required metadata is available.
- Metadata fetch failures are surfaced immediately (no automatic metadata retry loop).
- Negative metadata cache (tag-not-found) key is the root symbol only.
- Negative metadata cache invalidation event is metadata reload only.

### 4.5 Vendor-Specific Extensions

Common layer:

- Shared EtherNetIP framing, session management, connected messaging lifecycle, timeout/error policy.

AB extension layer:

- AB partial read/write services and payload forms.
- Capability handling for old PLCs, Micro800 constraints, old-only Forward Open, limited buffer sizes.

Omron extension layer:

- Omron-specific CIP segment/encoding differences and partial transfer behavior.

### 4.6 Negotiated Buffer Size Handling

Implementation must support broad ranges, including:

- Legacy AB around 504 bytes.
- Modern AB around 4002 bytes.
- Omron around 1890 bytes and potentially much larger models.

Packetizer must compute per-request/per-response budgets dynamically from actual negotiated size, not hard-coded assumptions.

### 4.7 Fiber-Based I/O Requirement

Use the I/O architecture approach demonstrated in src/poc/ab_server_fiber:

- Fiber-based connection handling.
- Explicit buffer objects and bounded memory handling.
- Minimized thread count with scalable cooperative I/O.

## 5. Error and Connection State Policy

1. Protocol-level errors that do not imply transport/session corruption (for example tag-not-found) must complete request with proper tag error but keep connection alive.
2. Socket errors and unrecoverable protocol framing/state violations trigger hard close and reconnect path.
3. Idle timeout uses graceful connection close policy where possible.
4. Connection status changes must be surfaced promptly to @connection tags and callbacks, targeting callback emission latency <100ms.
5. If 0x0A Multi-Service is detected as unsupported by the PLC, permanently downgrade that live connection to single-request mode.
6. Fragmentation must be transparent to applications; oversized operations are internally split/continued as needed.
7. 0x0A downgrade does not persist across reconnect or process restart.
8. Forward Open policy per connection attempt:
  - Try FOEx once.
  - On failure, fallback to old Forward Open once for that attempt.
  - On a new connection attempt, try FOEx again.
9. Legacy pseudo tags on new protocol (@tags, @udt, @identity) fail create immediately with PLCTAG_ERR_UNSUPPORTED.

## 6. Test and Validation Expectations

### 6.1 Modbus Scheduler Rework

- Existing Modbus tests must pass, including connection-tag state transition tests.
- Add list-cursor focused concurrency tests:
  - remove current node during iteration,
  - remove next node during iteration,
  - rapid create/destroy under parallel API calls.
- Validate <100ms @connection callback emission latency after state changes.

### 6.2 Generic EtherNetIP Implementation

- Capability matrix tests:
  - old Forward Open only,
  - Forward Open Extended capable,
  - Multi-Service unsupported (Micro800 path),
  - varying negotiated buffer sizes.
- Metadata tests:
  - root-only tags,
  - nested UDT field paths,
  - multidimensional arrays,
  - bit selection validity checks.
- Fault injection:
  - socket disconnect mid-flight,
  - protocol error response with continued healthy connection,
  - reconnect after hard close.

Hardware and scope notes from requester:

- AB hardware available locally includes 10.206.1.36, 10.206.1.37, 10.206.1.39, 10.206.1.40, and possibly 10.206.1.38.
- Some devices are older DF1/PCCC-family devices (PLC5/SLC class) and are out of first-cut scope for this new generic EtherNetIP implementation.
- Omron hardware is not currently available; Omron-specific validation requires simulator/research-first strategy.
- Primary Logix-class chassis is reachable through 10.206.1.37, 10.206.1.39, and 10.206.1.40. Any of these gateway IPs can reach individual slots by path.
- Chassis slot inventory (via path 1,slot):
  - Slot 0: 1756-L61/B LOGIX5561 (Vendor 1, Device Type 14, Product Code 54, Rev 20.12)
  - Slot 1: 1756-ENBT/A (Vendor 1, Device Type 12, Product Code 58, Rev 6.6)
  - Slot 2: 1756-DHRIO/E (Vendor 1, Device Type 12, Product Code 18, Rev 7.2)
  - Slot 3: Empty
  - Slot 4: 1756-L81E/B (Vendor 1, Device Type 14, Product Code 164, Rev 31.11)
  - Slot 5: 1756-L55/A 1756-M22/A LOGIX5555 (Vendor 1, Device Type 14, Product Code 51, Rev 16.22)
  - Slot 6: 1756-ENBT/A (Vendor 1, Device Type 12, Product Code 58, Rev 4.8)
- Merge gate requires at least one modern Logix-class hardware endpoint to pass full new-protocol suite locally before merge.
- Hardware expectation: the 1756-L55 family may not support Large Forward Open/FOEx-sized connections. For this endpoint, FOEx failure with fallback to old Forward Open is expected behavior and should be validated explicitly in hardware tests.
- PLC5/SLC/PCCC endpoints are explicitly excluded from first-cut enip-tcp failure gating.
- CI fault-injection minimums for first cut: socket drop mid-request, partial frame receive, malformed response service code.
- FO reject / duplicate-connection simulation exists in current AB simulator and is considered already covered.
- Simulator limitation: current simulators do not support tag enumeration/metadata fetch; those validations require real hardware for first cut.

## 7. Proposed Delivery Plan

1. Design and implement Modbus linked-list scheduler conversion first (lower protocol risk, immediate architectural gain).
2. Introduce generic EtherNetIP core connection/session/state machine with no vendor-specific metadata yet.
3. Add capability detection + packetizer budget engine.
4. Add metadata phase 1 (minimal enumeration cache).
5. Add metadata phase 2 (on-demand deep fetch + path resolver, scheduler-driven metadata op states).
6. Add AB extension behaviors.
7. Add Omron extension behaviors.
8. Expand tests and run simulator suites.

Performance and merge policy:

- Target performance delta relative to legacy ab-eip path is <=5% for existing performance workloads.
- Regression >5% does not automatically block merge, but requires documented cause and a follow-up optimization issue before merge.

## 8. Metadata Special Tag Requirement

Replace legacy metadata pseudo tags with a unified @metadata special tag:

- @metadata supersedes @tags, @udt, and @identity for this new implementation.
- When created against a device/connection, @metadata returns device identity data and tag/structure metadata as JSON.
- Single-tag targeted metadata retrieval can be a future enhancement.

Resolved JSON content requirements:

- Top-level keys include at minimum:
  - tags
  - structs
  - identity
- Additional top-level programs key is not required in first cut.
- Program entries are represented in global tags with type=program and include nested tags inline.
- Struct entries include struct_id/type_id when available.
- Type encoding schema uses Example C (both string and structured object):
  - tag objects include tag_type (normalized string) and tag_type_info (raw/object details where available).
  - struct member objects include member_type (normalized string) and member_type_info (raw/object details where available).
  - Normalization examples: DINT -> int32, BOOL -> bool, UDT -> struct with name.
- identity includes normalized fields (no raw packet dump) plus capability flags such as supports_0x0a and supports_foex.
- Negotiated packet size and current connection state are exposed via attributes, not in @metadata JSON.
- @metadata JSON is cached and rebuilt only on metadata reload/invalidation.
- No output-size cap in first cut; out-of-memory is an error condition.

Note: @connection naming is used throughout this document; equivalent legacy terms are omitted intentionally.

## 9. Strict Memory Handling Constraints (RC System)

The requester requires strict adherence to shared-memory RC semantics in src/utils/rc.h and src/utils/rc.c.

Mandatory constraints for new Modbus scheduler and generic EtherNetIP code:

1. Shared objects must be allocated with rc_alloc and released with rc_dec.
2. Reference acquisition must use rc_inc and must handle rc_inc returning NULL when count is non-positive.
3. rc_dec does not directly free objects in normal runtime:
  - it enqueues headers to a cleanup_queue,
  - a dedicated cleanup thread drains the queue,
  - destructor callbacks execute in that cleanup thread context.
4. Destructor code must be safe to run asynchronously on cleanup thread.
5. Destructors must release nested RC references and owned resources deterministically.
6. During teardown, refcount_teardown joins cleanup thread, drains remaining queued headers, and then destroys queue/synchronization primitives.
7. Never assume object destruction occurs in caller thread at rc_dec time.
8. Any pointer retained across lock boundaries or asynchronous scheduling points must hold a strong RC reference.
9. Thread termination and object teardown must avoid races where handler code touches objects whose last reference may have been dropped.
10. New list/scheduler code must preserve these invariants in all add/remove/iteration paths.

Additional RC/debug policy:

- RC assertion instrumentation for new code paths is compiled out of non-debug builds via compile-time flag.
- In debug builds, high-frequency RC assertion output is intended for SPEW-level diagnostics.

Implementation note:

- Because cleanup/destructors run out-of-band, cursor/list code must not use raw pointers without ownership guarantees when operating outside list mutex critical sections.

## 10. Resolved Decisions and Remaining UNKNOWNs

Resolved implementation decisions:

1. Identity capability precedence in first cut:
  - static mapping priority is manufacturer/device_type first, then product string.
  - no active capability probes in first cut.
  - runtime-observed unsupported behavior can trigger in-session fallback/downgrade.
2. Metadata cache scope is per-connection only and is discarded on reconnect.
3. Metadata reload triggers in first cut: reconnect only.
4. A metadata staleness wall-clock interval of 300 seconds may be added later as a read/write tag attribute; not enabled in first cut.
5. Duplicate request-list insertion policy: WARN and ignore.
6. Telemetry attribute naming convention: snake_case.
7. Telemetry attribute scope: only @connection tags.
8. Telemetry attributes listed below are required minimum first-cut additions:
  - queue_depth
  - callback_latency_last_ms
  - callback_latency_max_ms
  - fragment_count_total
  - downgrade_0x0a_flag
  - foex_fallback_count
  - metadata_cache_hits
  - metadata_cache_misses

Remaining UNKNOWN items:

1. None.

## 11. Where I Can Mine More Details If You Want

If you want me to pre-fill additional specifics before implementation, I can mine:

- AB and Omron session/connection state machines and request packing internals.
- Existing @tags and @udt payload details for exact metadata field parity.
- Existing simulator behaviors to derive an executable acceptance matrix.

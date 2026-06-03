# ENIP Linear Vector-First Implementation Execution Plan

**Date:** 2026-05-28  
**Status:** Ready for execution  
**Source Authority:** Decisions locked in:
- docs/enip-linear-vector-first-implementation-plan.md
- docs/enip-linear-vector-first-implementation-contract.md
- docs/enip-linear-vector-first-file-by-file-task-list.md
- docs/enip-pccc-*.md (PCCC variant requirements)

---

## 0. Executive Summary

Build an alternate ENIP-TCP/ENIP_TCP C implementation for libplctag that:
- Uses a vector-based active-work scheduler (same model as Modbus)
- Implements linear, blocking-like control flow in the connection thread
- Supports AB/Logix, OMRON, and PCCC devices through manufacturer strategy hooks
- Enforces deterministic multi-request packing via phase-1/phase-2 metadata
- Demonstrates feature parity with existing AB-EIP path for simulator and hardware tests

**Key Constraint:** All PLC-type branching must be isolated in manufacturer strategy modules. Shared ENIP code contains only manufacturer-neutral fields and flow control.

**Architecture Note:** The ENIP protocol implementation uses a three-layer separation for clarity and testability:
- **Layer 1 (EIP):** RFC 3171 24-byte encapsulation header (command, length, session, status, context, options)
- **Layer 2 (CPF):** Common Packet Format item framing (NAI, UDI, connected address/data items)
- **Layer 3 (CIP):** Common Industrial Protocol service/path/data (delegated to manufacturer strategy modules via `mfg_ops` callbacks)

Request building proceeds **inside-out** (CIP → CPF → EIP) and response parsing proceeds **outside-in** (EIP → CPF → CIP) using zero-copy `bytes_slice()` for efficiency. All serialization uses the type-safe Bytes API (`bytes_pack`, `bytes_unpack`, `bytes_concat`) with no raw byte manipulation. See [src/libplctag/protocols/enip/LAYER-ARCHITECTURE.md](src/libplctag/protocols/enip/LAYER-ARCHITECTURE.md) for design details.

---

## 1. Execution Phases

### Phase A: Foundation & Utilities (Prerequisite)
Build reusable infrastructure isolated from ENIP-specific logic.

**Tasks:**
- [A.1] Copy Arena + Bytes utilities from src/poc/ab_server_fiber → src/utils/
- [A.2] Implement wait wrappers (socket_read_wait, socket_write_wait, socket_connect_wait)
- [A.3] Add these modules to CMakeLists.txt build

**Dependencies:** None  
**Accepts:** Standalone tests of Arena/Bytes and wait wrappers  

**Exit Criteria:**
- Wrappers compile and basic tests pass on target platform
- ENIP code can include and use both modules

---

### Phase B: Protocol Registration & Framework (2–3 files)
Register ENIP protocol variants and scaffold the module structure.

**Tasks:**
- [B.1] Edit src/libplctag/lib/tag.h: Add ENIP_TCP and ENIP_TCP_CONN enum values
- [B.2] Edit src/libplctag/lib/init.c: Map "enip-tcp" and "enip_tcp" to ENIP constructor
- [B.3] Edit src/libplctag/lib/lib.c: Add ENIP dispatcher to create_from_tag switch
- [B.4] Create src/libplctag/protocols/enip/CMakeLists.txt and export source list
- [B.5] Create src/libplctag/protocols/enip/enip.h: Public API + init/teardown declarations
- [B.6] Edit src/libplctag/CMakeLists.txt: Add ENIP source wiring
- [B.7] Compile checkpoint: plctag_dyn and plctag_static build without ENIP implementations

**Dependencies:** None (uses existing framework)  
**Accepts:** Protocol selector integration; tags cannot be created yet  

**Exit Criteria:**
- `plc_tag_create(uri_with_protocol=enip-tcp)` succeeds in returning a tag handle
- Tag vtable is wired but methods not yet implemented

---

### Phase C: ENIP Core Tag & Connection Types (3–4 files)
Define ENIP-specific tag, connection, and operational state.

**Tasks:**
- [C.1] Create src/libplctag/protocols/enip/tag.h: ENIP tag struct (state, correlation, metadata flags)
- [C.2] Create src/libplctag/protocols/enip/enip_tag.c: Constructor + vtable stubs (read, write, status, abort, wake_plc)
- [C.3] Create src/libplctag/protocols/enip/enip_connection_tag.c: @connection tag support
- [C.4] Create src/libplctag/protocols/enip/enip_conn.h: Connection struct skeleton (mfg_ops, active_tags, session state)

**Dependencies:** Phase B  
**Accepts:** Tags can be created and destroyed safely; vtable methods are stubs  

**Exit Criteria:**
- ENIP tags move from creation to active_tags vector
- Connection object persists across multiple tags
- No crashes on tag destroy or connection close

---

### Phase D: Manufacturer Strategy Interface (1 file)
Define strategy hooks that isolate PLC-type logic from shared code.

**Tasks:**
- [D.1] Create src/libplctag/protocols/enip/enip_mfg_ops.h:
  - struct enip_mfg_ops with encode_read, decode_read, encode_write, decode_write, needs_more callbacks
  - struct req_desc and chunk_result for strategy communication
  - Strategy selector function: select_mfg_ops(identity)

**Dependencies:** Phase C  
**Accepts:** Shared code can call manufacturer hooks without type branches  

**Exit Criteria:**
- No compiler warnings for missing strategy implementations
- Can trace calls from shared packetizer to mfg_ops hooks

---

### Phase E: Linear Connection Loop & I/O (Primary: enip_conn.c)
Implement the heart of the new architecture.

**Tasks:**
- [E.1] Create src/libplctag/protocols/enip/enip_conn.c: Implement connection thread entry and main loop
  - Phase A: Wake reason, terminate checks
  - Phase B: TCP connect, Register Session, Get Identity, FOEx/FO, phase-1 metadata
  - Phase C: Build outgoing requests from active_tags with refcount hand-offs
  - Phase D: Send with socket_write_wait wrapper
  - Phase E: Wait for I/O or wake
  - Phase F: Receive and frame assembly with socket_read_wait wrapper
  - Phase G: Match responses and complete tag work
  - Phase H: Callbacks and telemetry
  - Phase I: Idle disconnect and retry
  - Phase J: Shutdown
- [E.2] Implement metadata phase-1 inventory (Service 0x55, Class 0x6B) in enip_conn.c or separate enip_metadata.c
- [E.3] Implement capability profile derivation (supports_0x0A, supports_extended_fo, max_packet_buffer_size)
- [E.4] Implement reconnect rearm: Move RESPONSE-state tags back to REQUEST state on reconnect

**Dependencies:** Phases A–D  
**Accepts:** Simulator can run basic connect/disconnect cycles  

**Exit Criteria:**
- TCP connect and Register Session succeed
- Phase-1 metadata is fetched and cached
- Forward Open succeeds or fails gracefully with retry
- Tags in REQUEST state are processed and moved to RESPONSE state
- Reconnect preserves pending tags

---

### Phase F: Packetizer & Budget Enforcement (1–2 files)
Implement deterministic packet budget calculations and multi-request packing decision.

**Tasks:**
- [F.1] Create src/libplctag/protocols/enip/enip_packetizer.c:
  - Estimate request encoded size per candidate tag (call mfg_ops hook)
  - Estimate response size per candidate (based on read size, write ack, failure response)
  - Enforce aggregate request and response budgets under negotiated connection size
  - Decide which tags fit in outgoing packet
  - Format multi-service (0x0A) packet with offset table if supported
- [F.2] Implement write-trimming logic for oversized writes (call mfg_ops for chunk sizing)
- [F.3] Enforce read-trimming so expected response fits budget

**Dependencies:** Phases E–F (packetizer calls mfg_ops)  
**Accepts:** Deterministic packing decisions; tags split into chunks when necessary  

**Exit Criteria:**
- Single request and multi-request (0x0A) packets respect budget constraints
- Reads/writes that exceed single-packet size are automatically chunked
- No AB/OMRON branching in shared packetizer code

---

### Phase G: Metadata Phase-2 & Negative Cache (1 file)
Implement on-demand deep metadata and cache negative lookups.

**Tasks:**
- [G.1] Create src/libplctag/protocols/enip/enip_metadata.c (if not in E.2):
  - Implement phase-2 deep metadata fetch on first seen tag/path
  - Reference attributes defined in @tag implementation (src/libplctag/protocols/ab/eip_cip_special.c)
  - Block read/write packing until deep metadata is available for a tag
  - Implement root-symbol negative cache using src/utils/hashtable.c
  - Invalidate negative cache on reconnect or metadata reload

**Dependencies:** Phase E  
**Accepts:** Tags with missing metadata do not attempt packed operations; metadata is fetched on demand  

**Exit Criteria:**
- Negative cache is populated after metadata fetch fails
- Metadata fetch is scheduler-managed, not blocking API threads
- Tags block until their deep metadata is available

---

### Phase H: AB/Logix Strategy Implementation (1 file)
Implement the primary manufacturer-specific path for AB Logix devices.

**Tasks:**
- [H.1] Create src/libplctag/protocols/enip/enip_mfg_ab.c:
  - Implement AB read request builder (service 0x52 + path + element_count + byte_offset)
  - Implement AB write request builder (service 0x53)
  - Implement AB response parser
  - Apply fixed path encoding rules (0x91 for symbolic, 0x28/0x29/0x2A for array indexes)
  - Implement AB chunking continuation check (parse fragmented status and needs_more logic)
  - Register callbacks in select_mfg_ops for AB identity match

**Dependencies:** Phases D–F  
**Accepts:** AB symbolic reads/writes work; chunked operations continue until completion  

**Exit Criteria:**
- AB tag reads succeed end-to-end
- AB tag writes succeed end-to-end
- Chunked AB operations handle fragmented responses correctly
- Response matching uses sender_context + multi-service index/offset

---

### Phase I: OMRON Strategy Implementation (1 file)
Implement manufacturer-specific path for OMRON devices.

**Tasks:**
- [I.1] Create src/libplctag/protocols/enip/enip_mfg_omron.c:
  - Implement OMRON read/write request builders (0x80 simple data segment)
  - Compute chunk sizes client-side so both request and response fit budgets
  - Implement offset and size tracking per chunk
  - Register callbacks for OMRON identity match

**Dependencies:** Phases D–F  
**Accepts:** OMRON devices are supported via strategy isolation  

**Exit Criteria:**
- OMRON chunked reads work correctly
- OMRON chunked writes work correctly
- No shared-code branching for OMRON-specific behavior

---

### Phase J: ENIP+PCCC Strategy Implementation (1 file)
Implement manufacturer-specific path for PCCC variants (PLC5, SLC, Logix-over-PCCC, DH+).

**Tasks:**
- [J.1] Create src/libplctag/protocols/enip/enip_mfg_pccc.c:
  - Implement PLC5 direct read/write (service 0x01/0x00 with byte offset)
  - Implement SLC/MicroLogix direct read/write
  - Implement Logix-over-PCCC read/write
  - Implement DH+ bridged PLC5/SLC variants (add routing header)
  - Implement manual chunking per PLC-type field rules (reference docs/enip-pccc-*.md)
  - Register callbacks for PCCC identity match
  - **Bit Write:** Implement PLC5, SLC, and DH+ masked bit-write operations
  - **Logix-over-PCCC Bit Write:** Document fallback to element write (no dedicated masked path)

**Dependencies:** Phases D–F  
**Accepts:** All PCCC variants supported via strategy isolation; manual chunking for large operations  

**Exit Criteria:**
- PLC5 direct reads/writes work
- SLC/MicroLogix direct reads/writes work
- Logix-over-PCCC reads/writes work
- DH+ bridged operations work with routing header
- Bit writes succeed for PLC5 and SLC (and DH+ variants)
- Chunking respects PCCC field rules (word transfers for PLC5, byte transfers for SLC)
- No shared-code branching for PCCC-specific behavior

---

### Phase K: CIP Path Encoding & Normalization (1 file)
Implement tag and route path encoding per contract rules.

**Tasks:**
- [K.1] Create src/libplctag/protocols/enip/enip_name.c:
  - Implement canonical name handling (case-sensitive, program-scope namespace)
  - Enforce array index rules (no partial indexes)
  - Implement tag path encode/decode (symbolic 0x91, array indexes 0x28/0x29/0x2A)
  - Implement route token normalization (A→18, B→19)
  - Implement extended IP route encoding ([port][ascii_len][ascii_ip][pad])
  - Provide round-trip encode/decode verification

**Dependencies:** Phases A–H  
**Accepts:** Tag and route names are correctly encoded for ENIP transport  

**Exit Criteria:**
- Tag path encoding matches contract worked examples
- Route aliasing works correctly
- Canonical rules prevent invalid operations

---

### Phase L: Integration & Testing (2–3 files)
Wire ENIP into test harnesses and validate end-to-end behavior.

**Tasks:**
- [L.1] Edit src/tests/scripts/run_simulator_tests.sh: Add ENIP protocol coverage
- [L.2] Edit src/tests/scripts/run_hardware_tests.sh: Add ENIP protocol coverage
- [L.3] Document expected test exclusions (if any) with justification
- [L.4] Run simulator regression suite and capture results
- [L.5] Run hardware regression suite and capture results (if hardware available)

**Dependencies:** All prior phases  
**Accepts:** Feature parity with existing AB-EIP for test coverage  

**Exit Criteria:**
- Simulator tests pass with ENIP protocol (or documented exclusions are explicit)
- Hardware tests pass with ENIP protocol (or documented exclusions are explicit)
- No performance regression vs. existing AB-EIP path

---

## 2. Cross-Phase Dependencies & Ordering

```
Phase A (Utils) → Phase B (Registration)
                     ↓
Phase B → Phase C (Tag/Conn Types)
             ↓
Phase C → Phase D (Mfg Strategy Interface)
             ↓
       Phase D → Phase E (Connection Loop)
       Phase D → Phase F (Packetizer)
       Phase E → Phase F (Packetizer calls mfg_ops)
            ↓
Phase F → Phase G (Metadata & Cache)
Phase F → Phase H (AB Strategy)
Phase F → Phase I (OMRON Strategy)
Phase F → Phase J (PCCC Strategy)

Phase H, I, J → Phase K (Path Encoding)
Phase K → Phase L (Integration & Testing)
```

**Critical Path:** A → B → C → D → E → F → G → H

---

## 3. Compilation Checkpoints (Per Phase)

After each phase, all code must compile without errors:
- **After B:** plctag_dyn, plctag_static compile (no implementations yet)
- **After C:** Tags can be created/destroyed; vtables wired
- **After E:** Connection thread can start and process events
- **After F:** Multi-request packing logic compiles and hooks to mfg_ops
- **After H, I, J:** Manufacturer-specific modules compile and register
- **After K:** Path encoding compiles and is available to all manufacturers
- **After L:** All tests compile and execution begins

---

## 4. Testing Strategy

### Simulator Tests (Run After Each Phase)
- **After E:** Connect/disconnect cycles, basic tag state transitions
- **After F:** Packet budget enforcement, request packing decisions
- **After H/I/J:** Full read/write cycles for each device type
- **After K:** Path encoding validation, route normalization
- **After L:** Regression suite (existing tests + ENIP variants)

### Hardware Tests (Run After L)
- Validate feature parity with existing AB-EIP path
- Capture performance metrics
- Document any expected platform-specific exclusions

### Key Test Areas
1. **Metadata:** Phase-1 inventory, phase-2 deep metadata, negative cache
2. **Packing:** Single-request, multi-request (0x0A), budget enforcement
3. **Chunking:** Large reads/writes, PCCC field-specific rules, continuation logic
4. **Error Handling:** Socket errors, protocol errors, timeouts, retry behavior
5. **Reconnect:** Pending tag rearm, session re-establishment
6. **Bit Writes:** Per-PLC-type masked operations
7. **Callbacks:** Connection state transitions, latency <100ms target

---

## 5. Critical Guardrails & Review Points

### Lock & Lifetime Rules (Per Contract 3.3)
- [ ] No mutex held across I/O waits
- [ ] rc_inc/rc_dec hand-offs around vector operations
- [ ] Restart state discarded on reconnect

### Manufacturer Isolation (Per Contract 3.11)
- [ ] Shared ENIP code contains zero PLC-type branching (if/else AB vs. OMRON)
- [ ] All CIP service, path, and chunk logic behind mfg_ops callbacks
- [ ] New PLC types added by implementing new strategy module, not extending shared code

### Packet Budgets (Per Contract 3.5)
- [ ] Request aggregate = 2 (count) + N*2 (offset entries) + Sum(request bytes)
- [ ] Response aggregate = 2 (count) + N*2 (offset entries) + Sum(response bytes)
- [ ] Writes trimmed if needed; reads trimmed for response space
- [ ] Response estimates account for success (4 bytes), failure (6–8 bytes), large reads (full buffer)

### Metadata (Per Contract 3.8)
- [ ] Phase-1 required before general read/write packing
- [ ] Phase-2 blocks specific tag read/write until available
- [ ] Negative cache invalidated on reconnect, not reset on transient errors
- [ ] Metadata fetch is scheduler-managed, not blocking

---

## 6. Acceptance Criteria (Final)

### Feature Completeness
1. ENIP-TCP/ENIP_TCP protocol variant is registered and selectable
2. All four manufacturer paths (AB, OMRON, PCCC, PCCC-DH+) are implemented
3. Single-request and multi-request packing works
4. Metadata phase-1 and phase-2 are enforced
5. Chunking handles oversized reads/writes correctly per PLC type
6. Bit writes work for AB-capable, PLC5, SLC, and DH+ variants

### Testing
1. Simulator regression suite passes with ENIP protocol (or exclusions are documented)
2. Hardware regression suite passes with ENIP protocol (or exclusions are documented)
3. No performance regression vs. existing AB-EIP path
4. Key test areas (above) all pass

### Code Quality
1. Coding guidelines followed (src/libplctag/docs/coding_guidelines.md)
2. Manufacturer isolation enforced (no branching in shared code)
3. Lock ordering and lifetime rules verified
4. All compiler warnings resolved
5. Refcount hygiene verified (no leaks or double-frees)

---

## 7. File Inventory & Rough Size Estimates

| Phase | File | Lines | Notes |
|-------|------|-------|-------|
| A | src/utils/arena.h + .c | ~300 | Copied from poc |
| A | src/utils/bytes.h + .c | ~400 | Copied from poc |
| A | src/utils/enip_wait.h + .c | ~600 | New blocking-like wrappers |
| B | Tag.h, init.c, lib.c edits | ~50 | Small registration changes |
| B | enip/CMakeLists.txt | ~20 | Build wiring |
| B | enip/enip.h | ~30 | Public API stub |
| C | enip/tag.h | ~150 | ENIP tag struct |
| C | enip/enip_tag.c | ~300 | Tag constructor + vtables |
| C | enip/enip_connection_tag.c | ~200 | @connection support |
| C | enip/enip_conn.h | ~150 | Connection struct |
| D | enip/enip_mfg_ops.h | ~100 | Strategy interface |
| E | enip/enip_conn.c | ~2000–2500 | Main loop (Phase A–J) |
| E | enip/enip_metadata.c | ~500 | Phase-1, phase-2, cache |
| F | enip/enip_packetizer.c | ~600 | Budget, packing, trimming |
| H | enip/enip_mfg_ab.c | ~600 | AB request/response logic |
| I | enip/enip_mfg_omron.c | ~400 | OMRON 0x80 segment logic |
| J | enip/enip_mfg_pccc.c | ~1000 | PCCC + DH+ + bit write logic |
| K | enip/enip_name.c | ~500 | Path encoding/decoding |
| L | Test harness edits | ~100 | Script updates |
| **Total** | | **~9,000–10,500** | Excluding tests |

---

## 8. Risk & Mitigation

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Mutex deadlock in refcount hand-offs | Medium | High | Debug assertions at lock boundaries; code review |
| Manufacturer branching in shared code | Medium | High | Grep/lint for PLC-type conditionals in main loop |
| Packet budget miscalculation | Medium | High | Detailed audit vs. contract; simulator tests |
| Metadata blocking I/O in API threads | Medium | High | Verify metadata is scheduled, not ad hoc |
| Large memory footprint from arena | Low | Medium | Monitor arena high-water; cap at 32KB |
| Reconnect not reaming RESPONSE tags | Medium | Medium | Explicit refcount of rearm logic in tests |
| Path encoding round-trip failures | Low | Medium | Comprehensive path encoding tests |

---

## 9. Notes for Reviewers & Future Maintainers

1. **Manufacturer Isolation is Non-Negotiable:** Any temptation to add `if (is_ab) { ... } else if (is_omron) { ... }` in shared code must be rejected. Add a new strategy module instead.

2. **Lock Order:** Always PLC mutex before tag API mutex. If you need both, acquire in that order and document exceptions.

3. **Restart State is Ephemeral:** After any socket error, reconnect, or timeout, discard restart state and start fresh. Do not persist it across lifecycle boundaries.

4. **Metadata Scheduling is Non-Blocking:** Never block API threads waiting for metadata I/O. The scheduler thread fetches metadata; API thread marks tag as needing metadata and returns to application.

5. **Test-Driven Validation:** Use simulator regression tests as primary validation gate. Hardware tests are secondary confirmation. Document exclusions explicitly.

6. **Performance Baseline:** Capture metrics for AB-EIP path before ENIP merge. Compare throughput, latency, and memory usage post-merge. No regressions acceptable.

---

## 10. Success Metrics

### Before Merge to Main
- [ ] All code compiles without warnings
- [ ] Simulator regression suite passes (with documented exclusions if any)
- [ ] Hardware regression suite passes (if hardware available)
- [ ] Performance parity or better vs. AB-EIP path
- [ ] Code review confirms manufacturer isolation
- [ ] Lock order review confirms no deadlock paths
- [ ] Metadata lifecycle review confirms no blocking

### Post-Merge Monitoring
- [ ] User issues reported via GitHub tracked and resolved
- [ ] Performance monitoring in CI/CD confirms no regression
- [ ] Feature parity with AB-EIP path maintained and documented

---

## 11. Next Steps

1. **Create Session Memory:** Save this plan and progress tracking to /memories/session/enip-execution-plan.md
2. **Assign Phases:** Break into individual tasks and assign to team members or sequential milestones
3. **Start Phase A:** Copy utilities and implement wait wrappers (no ENIP-specific code)
4. **Weekly Checkpoints:** Review completed phases, validate compilation, run simulator tests
5. **Document Decisions:** Any deviations from this plan require explicit decision note (date, author, reason)

---

**End of ENIP Implementation Execution Plan**

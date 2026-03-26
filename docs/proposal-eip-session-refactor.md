# Proposal: Migrate EtherNet/IP to Session-Driven Tag Processing

## Goal

Eliminate the global `tag_tickler_func` thread by moving all EtherNet/IP tag processing into the per-session I/O thread (`session_handler`), aligned with the Modbus architecture. The end state:

- No `tag_tickler_func` thread.
- No request objects (`ab_request_t`).
- Sorted active-tag list per session (matching Modbus pattern).
- Full support for auto-sync, `plc_tag_read()`/`plc_tag_write()`, all special tag types (listing, raw, UDT, identity), and all PLC types (Logix, PLC/5, SLC, Micro800, Omron).

## Current Architecture Summary

### Modbus (target pattern)

- Per-PLC I/O thread (`modbus_plc_handler`) owns a **sorted `active_tags` vector**, ordered by `op_time`.
- Tags have an `op` state machine: `IDLE → READ_REQUEST → READ_RESPONSE → IDLE`.
- The I/O thread calls `tickle_all_tags()`, which scans only the front of the sorted vector. Tags waiting for responses sort to the front (past `op_time`). Tags with future auto-sync reads sort toward the back.
- The I/O thread builds packets directly from tag data, sends them, receives responses, and parses responses back into tag buffers — all in one thread.
- Auto-sync is driven by `op_time` scheduling. `tag_data_written()` vtable entry handles dirty-data write scheduling.
- Tags set `skip_tickler = 1`, completely bypassing `tag_tickler_func`.
- The thread sleeps precisely via `socket_wait_event(timeout)` until the next scheduled operation.

### EtherNet/IP (current)

- Per-session I/O thread (`session_handler`) manages socket I/O and a **FIFO request queue** (`session->requests` vector of `ab_request_t*`).
- A separate global `tag_tickler_func` thread iterates **all** tags in the hashtable every 10–100ms:
  - Calls `plc_tag_generic_tickler()` — handles auto-sync scheduling, starts reads/writes.
  - Calls `vtable->tickler()` — polls `req->resp_received`, parses responses.
  - Detects `read_complete`/`write_complete`, raises events, fires callbacks.
- Request flow: tag builds an `ab_request_t`, queues it to the session, session packs/sends/receives, copies response back into request, sets `resp_received = 1`, calls `plc_tag_tickler_wake()`.
- The tickler thread wakes, eventually finds the tag, calls its tickler to parse the response.
- **11 vtables** across CIP, PCCC, DHP, listing, raw, UDT, identity tags.

### Why `tag_tickler_func` is Expensive

1. **O(n) scan every cycle** — iterates every tag in the global hashtable, even idle ones.
2. **Mutex contention** — acquires `tag_lookup_mutex` to snapshot all tags, then `tag->api_mutex` per tag.
3. **Cross-thread latency** — response completion requires the session thread to wake the tickler thread, which then re-acquires the tag mutex. This adds an OS scheduling round-trip.
4. **Polling, not event-driven** — sleeps up to 100ms between cycles even when work is available.

## PLC Type and Protocol Compatibility Reference

This section documents the capabilities and constraints of each PLC type and how they map to session and tag structures.

### Tag Type Compatibility by PLC Type

| Tag Type / Protocol | ControlLogix / CompactLogix | Micro800 | PLC/5 Direct | PLC/5 DH+ Routed | SLC500 / MicroLogix Direct | SLC500 DH+ Routed | *Logix PCCC-Mapped |
|---------------------|---------------------------|----------|--------------|------------------|---------------------------|-------------------|--------------------|
| **CIP Standard** (`ab_cip_tag_t`) | ✓ | ✓ (variant) | ✗ | ✗ | ✗ | ✗ | ✗ |
| **CIP Listing** (`ab_listing_tag_t`) | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| **CIP UDT** (`ab_udt_tag_t`) | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| **CIP Raw** (`ab_raw_tag_t`) | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| **CIP Identity** (`ab_identity_tag_t`) | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| **PCCC PLC/5** (`ab_pccc_plc5_tag_t`) | ✗ | ✗ | ✓ | ✓ (same tag) | ✗ | ✗ | ✗ |
| **PCCC SLC** (`ab_pccc_slc_tag_t`) | ✗ | ✗ | ✗ | ✗ | ✓ | ✓ (same tag) | ✗ |
| **PCCC *Logix** (`ab_pccc_logix_tag_t`) | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ |
| **Session Type** | `ab_session_controllogix_t` | `ab_session_micro800_t` | `ab_session_pccc_plc5_t` | `ab_session_dhplus_plc5_t` | `ab_session_pccc_slc_t` | `ab_session_dhplus_slc_t` | `ab_session_pccc_logix_t` |
| **Transport Protocol** | CIP/EIP | CIP/EIP | CIP/EIP → PCCC | CIP/EIP → DH+ → PCCC | CIP/EIP → PCCC | CIP/EIP → DH+ → PCCC | CIP/EIP → PCCC |

**Note**: Same tag type (e.g., `ab_pccc_plc5_tag_t`) works with both direct and DH+ routed sessions for that command set. The session adds the routing wrapper; the tag builds identical PCCC payloads.

### Messaging Mode by PLC Type

| Capability | ControlLogix / CompactLogix | Micro800 | PLC/5 Direct | PLC/5 DH+ Routed | SLC500 / MicroLogix Direct | SLC500 DH+ Routed | *Logix PCCC-Mapped |
|------------|---------------------------|----------|--------------|------------------|---------------------------|-------------------|--------------------|
| **Connected Messaging** | ✓ Optional | ✓ Required | ✗ Never | ✗ Never | ✗ Never | ✗ Never | ✓ Optional |
| **Unconnected Messaging** | ✓ Optional | ✗ Never | ✓ Always | ✓ Always | ✓ Always | ✓ Always | ✓ Optional |
| **Forward Open Required?** | Optional (tag-dependent) | ✓ YES (always) | ✗ NO | ✗ NO | ✗ NO | ✗ NO | Optional (tag-dependent) |

### Request Packing by PLC Type

| Capability | ControlLogix / CompactLogix | Micro800 | PLC/5 Direct | PLC/5 DH+ Routed | SLC500 / MicroLogix Direct | SLC500 DH+ Routed | *Logix PCCC-Mapped |
|------------|---------------------------|----------|--------------|------------------|---------------------------|-------------------|--------------------|
| **Multi-Request Packing** | ✓ YES | ✗ NO | ✗ NO | ✗ NO | ✗ NO | ✗ NO | ✗ NO |
| **All Tag Types Packable?** | ✓ YES (CIP, listing, UDT, raw, identity) | N/A | N/A | N/A | N/A | N/A | N/A |
| **Max Tags Per Packet** | ~10-50 (space-limited) | 1 (always) | 1 (always) | 1 (always) | 1 (always) | 1 (always) | 1 (always) |

**Important**: Request packing is determined by the session's PLC type, not by individual tag types. On ControlLogix, **all** CIP tag types (standard, listing, UDT, raw, identity) can be packed together in a single multi-request packet.

### Connection Buffer Size and Fragmentation

| Capability | ControlLogix / CompactLogix | Micro800 | PLC/5 Direct | PLC/5 DH+ Routed | SLC500 / MicroLogix Direct | SLC500 DH+ Routed | *Logix PCCC-Mapped |
|-----------|---------------------------|----------|--------------|------------------|---------------------------|-------------------|--------------------|
| **Connection Buffer Size** | 500B (default)<br>~4KB (Large FO) | 500B (default)<br>~4KB (Large FO) | Fixed ~500B<br>(no negotiation) | Fixed ~460B<br>(DH+ overhead) | Fixed ~500B<br>(no negotiation) | Fixed ~460B<br>(DH+ overhead) | 500B (default)<br>~4KB (Large FO) |
| **Forward Open Type** | Standard or Large | Standard or Large | N/A (unconnected) | N/A (unconnected) | N/A (unconnected) | N/A (unconnected) | Standard or Large |
| **CIP Fragment Services** | ✓ YES | ✓ YES | ✗ NO | ✗ NO | ✗ NO | ✗ NO | ✗ NO |
| **PCCC Element-Range Splitting** | N/A | N/A | ✓ YES | ✓ YES | ✓ YES | ✓ YES | ✓ YES |
| **Payload Per Packet** | ~470B (std FO)<br>~3900B (Large FO) | ~470B (std FO)<br>~3900B (Large FO) | ~240B | ~200B | ~240B | ~200B | ~240B |
| **Max Total Transfer** | ~64KB<br>(CIP 16-bit offset) | ~64KB<br>(CIP 16-bit offset) | Tag buffer size<br>(memory-limited) | Tag buffer size<br>(memory-limited) | Tag buffer size<br>(memory-limited) | Tag buffer size<br>(memory-limited) | Tag buffer size<br>(memory-limited) |

**Connection Buffer Size**: Negotiated during Forward Open (Standard: ~500B, Large: ~4KB). Unconnected messaging uses fixed EIP packet sizes (~500B).

**Rockwell/AB CIP Fragmentation**: Uses Read/Write Fragment services with 16-bit offset, limiting transfers to ~64KB per tag operation.

**PCCC Element-Range Splitting**: Library-implemented fragmentation that splits large PCCC transfers into multiple element-range requests. No protocol limit; constrained only by memory and network timeout.

### Protocol Stack and Header Overhead

| Header Layer Stack | ControlLogix | Micro800 | PLC/5 Direct | PLC/5 DH+ | SLC500 Direct | SLC500 DH+ | *Logix PCCC |
|-------------------|--------------|----------|--------------|-----------|---------------|------------|-------------|
| **EIP Header** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| **CIP Header** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| **DH+ Routing Header** | ✗ | ✗ | ✗ | ✓ (~8B) | ✗ | ✓ (~8B) | ✗ |
| **PCCC/DF1 Header** | ✗ | ✗ | ✓ (~10B) | ✓ (~10B) | ✓ (~10B) | ✓ (~10B) | ✓ (~10B) |
| **Payload Protocol** | CIP | CIP | PCCC PLC/5 | PCCC PLC/5 | PCCC SLC | PCCC SLC | PCCC *Logix |
| **Total Overhead** | ~24B | ~24B | ~34B | ~42B | ~34B | ~42B | ~34B |

**DH+ Routing**: Adds intermediate routing layer between CIP and PCCC. The underlying PCCC commands (PLC/5 or SLC) remain identical; only the transport wrapper changes.

### Summary: Key Differentiators by PLC Type

| PLC Type | Native Protocol | Messaging Mode | Packing | Fragmentation Method | Payload/Packet | Max Total Transfer |
|----------|----------------|----------------|---------|---------------------|----------------|-------------------|
| **ControlLogix / CompactLogix** | CIP | Connected or Unconnected | ✓ YES | CIP Fragment | ~470B or ~3900B | ~64KB |
| **Micro800** | CIP | Connected (required) | ✗ NO | CIP Fragment | ~470B or ~3900B | ~64KB |
| **PLC/5 Direct** | PCCC (PLC/5 commands) | Unconnected | ✗ NO | Element-range | ~240B | Memory-limited |
| **PLC/5 DH+ Routed** | PCCC (PLC/5 commands) | Unconnected + DH+ routing | ✗ NO | Element-range | ~200B | Memory-limited |
| **SLC500 / MicroLogix** | PCCC (SLC commands) | Unconnected | ✗ NO | Element-range | ~240B | Memory-limited |
| **SLC500 DH+ Routed** | PCCC (SLC commands) | Unconnected + DH+ routing | ✗ NO | Element-range | ~200B | Memory-limited |
| ***Logix PCCC-Mapped** | PCCC (*Logix-mapped) | Connected or Unconnected | ✗ NO | Element-range | ~240B | Memory-limited |

**Key Insight**: PLC type determines session capabilities (packing, connection mode). The session type then determines which tag types are compatible.

---

## Migration Plan: 10 Incremental Steps

Each step produces a buildable, testable intermediate. Steps 1–6 run the old and new paths in parallel. Steps 7–10 remove the old path.

---

### Step 1: Add Sorted Active-Tag List to Session

**What**: Add a `vector_p active_tags` and a `mutex_p` (or reuse `session_mutex`) to `ab_session_t`. Add `tag_op_type_t op`, `int64_t op_time`, and `bool in_active_vector` fields to `ab_tag_t`. Port `insert_tag_sorted()`, `move_tag_sorted()`, `remove_tag_from_active()` from Modbus (these are data-structure helpers, not protocol-specific).

**Why**: Establishes the foundation. No behavioral change yet.

**Test**: Build and run the existing test suite. Assert that the new fields initialize to zero/defaults and do not affect existing behavior. Add a unit test that exercises insert/move/remove on a mock vector.

**Files touched**:

- `src/libplctag/protocols/ab/session.h` — add `active_tags` vector, add sorted-list helpers.
- `src/libplctag/protocols/ab/tag.h` — add `op`, `op_time`, `in_active_vector` to `ab_tag_t`.
- New file `src/libplctag/protocols/ab/ab_tag_list.c` (or inline in `session.c`) — sorted vector operations.

---

### Step 2: Implement `tag_data_written` for AB Tags

**What**: Implement an `ab_tag_data_written()` function and wire it into all 10 AB vtables (currently all have `.tag_data_written = NULL`). When `auto_sync_write_ms > 0` and data is dirtied, this function inserts/moves the tag in the session's `active_tags` with `op = TAG_OP_WRITE_REQUEST` and `op_time = now + auto_sync_write_ms` (debounce), then wakes the session thread. This mirrors `mb_tag_data_written()`.

**Why**: Decouples auto-sync write scheduling from the tickler. The tickler currently detects `tag_is_dirty` by polling; this makes it event-driven. The tickler's auto-sync write code in `plc_tag_generic_tickler()` will still fire but will find the tag already has a pending write (no conflict — it checks `write_in_flight`).

**Test**: Run `test_auto_sync` / `test_auto_sync_modbus`-equivalent tests. Verify that auto-sync writes still fire correctly. Add timing instrumentation to confirm writes happen at the expected intervals. Since the tickler is still running, the system has a safety net.

**Files touched**:

- `src/libplctag/protocols/ab/ab_common.c` — new `ab_tag_data_written()`.
- Every vtable definition (10 files) — set `.tag_data_written = ab_tag_data_written`.

---

### Step 3: Extract Blocking Session Helpers

**What**: The current `session_handler` implements a 12-state state machine where each state does a small piece of work, sets `state = NEXT_STATE`, and falls through to a shared 100ms `cond_wait` at the loop bottom. Convert the multi-state connection setup/teardown sequences into self-contained blocking functions:

1. `session_connect_blocking(session)` — Combines `SESSION_OPEN_SOCKET_START` + `SESSION_OPEN_SOCKET_WAIT` + `SESSION_REGISTER`. Opens the TCP socket, polls `socket_connect_tcp_check()` in a loop with short sleeps until connected, then sends the EIP RegisterSession and validates the response. Returns `PLCTAG_STATUS_OK` or error.

2. `forward_open_blocking(session)` — Combines `SESSION_SEND_FORWARD_OPEN` + `SESSION_RECEIVE_FORWARD_OPEN`. Handles the retry loop for error 0x0109 (connection size too large — retry with PLC's suggested size), error 0x0100 (duplicate connection ID — retry with new ID), and extended→old Forward Open fallback. Returns OK or error.

3. `session_disconnect_blocking(session)` — Combines `SESSION_DISCONNECT` + `SESSION_UNREGISTER` + `SESSION_CLOSE_SOCKET`. Performs Forward Close (if connected), unregisters, and closes socket. Always succeeds (errors logged but ignored, matching current behavior).

4. `wait_for_requests(session)` — Replaces `SESSION_WAIT_IDLE_RECONNECT`. Loops on `cond_wait(session->session_wait_cond, ...)` until `vector_length(session->requests) > 0` or `session->terminating`. Woken by `session_add_request()`'s existing `cond_signal()`.

The existing low-level helpers (`session_open_socket`, `session_register`, `send_forward_open_request`, `receive_forward_open_response`, `perform_forward_close`, `session_unregister`, `session_close_socket`) are already separate functions called from state cases. The new blocking wrappers compose them sequentially with internal wait loops instead of returning to the state machine.

**Why**: Pure refactoring step. Makes the connection flow explicit as sequential function calls. Each blocking wrapper is testable in isolation. Prepares for Step 4's linearization. The state machine still exists but each state body becomes a trivial forwarding call.

**Test**: Full existing test suite. Zero behavioral change. Specifically test:

- Normal connection establishment (ControlLogix connected, PLC/5 unconnected).
- Forward Open retry scenarios (`ab_server --reject_fo=N`).
- Connection timeout (unreachable gateway).
- Socket errors during setup.

**Files touched**:

- `src/libplctag/protocols/ab/session.c` — new blocking wrapper functions.

---

### Step 4: Linearize session_handler as Blocking Main Loop

**What**: Replace the 12-state `switch(state)` in `session_handler()` with a blocking linear flow using nested loops and goto-based cleanup:

```c
THREAD_FUNC(session_handler) {
    ab_session_p session = (ab_session_p)arg;
    int retry_count = 0;
    int auto_disconnect = 0;

    while (!session->terminating && atomic_get_bool(&lib_active)) {
        // --- Stage 1: Connect + Register ---
        int rc = session_connect_blocking(session);
        if (rc != PLCTAG_STATUS_OK) goto retry_wait;

        // --- Stage 2: CIP Forward Open (if connected messaging) ---
        if (session->use_connected_msg) {
            rc = forward_open_blocking(session);
            if (rc != PLCTAG_STATUS_OK) goto clean_socket;
        }

        // --- Stage 3: Steady-State Request Processing ---
        retry_count = 0;
        atomic_set_int32(&session->connection_status, PLCTAG_CONN_STATUS_UP);

        while (!session->terminating && atomic_get_bool(&lib_active)) {
            rc = process_requests(session);
            if (rc != PLCTAG_STATUS_OK) break;

            if (should_idle_disconnect(session)) {
                auto_disconnect = 1;
                break;
            }

            // Sleep until next event or timeout
            cond_wait(session->session_wait_cond, wait_time);
            if (session->terminating || !atomic_get_bool(&lib_active)) break;
        }

        atomic_set_int32(&session->connection_status, PLCTAG_CONN_STATUS_DOWN);

        // --- Cleanup Stack (fallthrough + goto targets) ---
        if (session->use_connected_msg) {
            perform_forward_close(session);
        }

    clean_socket:
        session_disconnect_blocking(session);

    retry_wait:
        if (session->terminating) break;
        if (auto_disconnect) {
            wait_for_requests(session);
            auto_disconnect = 0;
            retry_count = 0;
        } else {
            int64_t retry_ms = calc_retry_time(retry_count++);
            cond_wait(session->session_wait_cond, (int)retry_ms);
        }
    }

    THREAD_RETURN(0);
}
```

Key behaviors preserved:

- **Idle disconnect**: When no requests arrive within `connection_inactivity_timeout_ms`, the inner loop breaks, the connection is cleanly torn down, and the thread sleeps in `wait_for_requests()` until `session_add_request()` signals (matching current `SESSION_WAIT_IDLE_RECONNECT`).
- **Error retry with exponential backoff**: On connection-stage failure, cleanup falls through to `retry_wait` where `calc_retry_time()` provides `RETRY_WAIT_INITIAL_MS * 2^retry_count` capped at `RETRY_WAIT_MAX_MS`, with jitter (matching current `SESSION_START_RETRY` + `SESSION_WAIT_ERR_RETRY`).
- **Clean shutdown**: `session->terminating` is checked at every stage boundary and in both loops.
- **Forward Open negotiation**: Retry logic for connection size, duplicate connections, and extended→old fallback is encapsulated in `forward_open_blocking()` from Step 3.

The steady-state inner loop is where subsequent steps (5, 6) add inline response processing and active-tag scheduling. In the final state, the inner loop becomes:

```c
while (!session->terminating && atomic_get_bool(&lib_active)) {
    int64_t wait_time = tickle_active_tags(session);  // Step 6: auto-sync scheduling
    rc = process_requests(session);                    // + Step 5: inline response parsing
    if (rc != PLCTAG_STATUS_OK) break;
    if (should_idle_disconnect(session)) { auto_disconnect = 1; break; }
    cond_wait(session->session_wait_cond, wait_time);
}
```

**Why**: The linear flow is dramatically easier to reason about than the 12-state machine. Each stage's cleanup scope is visible in the goto structure. This aligns with Modbus's `modbus_plc_handler` which uses the same connect → steady-state loop → cleanup → retry pattern. The steady-state inner loop becomes a clear, localized section for subsequent modifications.

**Test**: Full existing test suite, focusing on:

- `test_reconnect`, `test_reconnect_after_outage_async/sync` — error recovery.
- `test_idle_disconnect` — idle disconnect and reconnect.
- `test_shutdown_cip`, `test_shutdown_restart` — clean shutdown at each stage.
- `test_connection_stress` — rapid connect/disconnect cycles.
- Normal operation with all PLC types.

**Files touched**:

- `src/libplctag/protocols/ab/session.c` — replace `session_handler` body, remove `session_state_t` enum.

---

### Step 5: Session Thread Processes Response Parsing Inline

**What**: After `unpack_response()` copies data into request buffers, instead of just setting `resp_received = 1` and calling `plc_tag_tickler_wake()`, the session thread **also** calls the tag's response-parsing logic directly.

Concretely, for each completed request:

1. Look up the tag by `request->tag_id` (via `lookup_tag()`).
2. Try-lock `tag->api_mutex`.
3. Call `vtable->tickler(tag)` — which checks `resp_received` and parses the response.
4. Detect `read_complete`/`write_complete`, raise events, signal `tag->tag_cond_wait`.
5. Release `api_mutex`.
6. **Drop session mutex** if held, then call `plc_tag_generic_handle_event_callbacks(tag)`.
7. Re-acquire session mutex to continue processing.
6. Release tag reference.

**Critical Note on Step 9 integration:** Once the tickler thread is removed in Step 9, the "fallback" must change. If the try-lock fails, the tag must remain in the `active_tags` vector with a `NEEDS_PARSE` flag so the session thread attempts to process it again on the next loop iteration.

**Why**: This is the critical latency optimization. Most of the time, the session thread will process responses immediately without waiting for the tickler. The tickler becomes a fallback, not the primary path.

**Test**: Run the full test suite. Add timing tests that measure read-completion latency — it should drop by roughly one tickler cycle (10–100ms). The tickler is still running, so missed inline processing is caught. Run `test_callback` and `test_callback_ex` to verify events fire correctly from both paths.

**Files touched**:

- `src/libplctag/protocols/ab/session.c` — modify `process_requests()` to call through to tag tickler after unpacking.

---

### Step 6: Session Thread Manages Auto-Sync Scheduling

**What**: Add a `tickle_active_tags()` function to the session thread (modeled on Modbus `tickle_all_tags()`). At the top of each steady-state inner loop iteration (from Step 4), scan the session's `active_tags` vector:

For tags in `TAG_OP_READ_REQUEST` or `TAG_OP_WRITE_REQUEST` state whose `op_time <= now`:

1. Try-lock `tag->api_mutex`.
2. Call `vtable->read(tag)` or `vtable->write(tag)` to build and queue the request.
3. Transition to `TAG_OP_READ_RESPONSE` / `TAG_OP_WRITE_RESPONSE`.

For tags in response states (handled by Step 5's inline processing), no additional work.

For tags with `response_needs_parsing == true`, attempt the `api_mutex` lock and process the response as described in Step 5.

After processing, compute `next_wake_time` from the first future-scheduled tag and adjust the session thread's wait timeout accordingly, replacing the fixed 100ms.

Also: for auto-sync reads, when a tag is created with `auto_sync_read_ms > 0`, insert it into the session's `active_tags` with `op_time = now + random_jitter(auto_sync_read_ms)` and `op = TAG_OP_READ_REQUEST`. After a read completes, reschedule the next read by advancing `op_time` by whole multiples of the period (same jitter-compensation as Modbus and the current `plc_tag_generic_tickler`).

**Why**: The session thread now drives auto-sync timing. The generic `plc_tag_generic_tickler()` auto-sync code is redundant for tags in `active_tags`. The sorted vector means only tags with imminent work are examined.

**Test**: Run `test_auto_sync` tests. Verify auto-sync reads and writes still fire at correct intervals. Monitor that the session thread's sleep time is adaptive (not always 100ms). Stress-test with many tags at different intervals to verify the sorted list is maintained correctly.

**Files touched**:

- `src/libplctag/protocols/ab/session.c` — add `tickle_active_tags()`, call from the steady-state inner loop.
- `src/libplctag/protocols/ab/session.h` — declare `tickle_active_tags()` if needed.

---

### Step 7: Set `skip_tickler = 1` for AB Tags

**What**: In `ab_tag_create()`, after all setup is complete, set `tag->skip_tickler = 1`. This removes all AB tags from the `tag_tickler_func` scan.

**Prerequisite**: Steps 2–6 must be complete. The session thread must handle:

- Auto-sync read scheduling (Step 6).
- Auto-sync write scheduling (Step 2).
- Response parsing and completion (Step 5).
- Event raising and callback dispatch.

Also implement the `skip_tickler` path for `plc_tag_read()` / `plc_tag_write()` direct calls:

- In `vtable->read` / `vtable->write`: insert the tag into the session's `active_tags` in the REQUEST state with `op_time = now` (immediate). Wake the session thread.
- The session thread picks it up on its next `tickle_active_tags()` pass.
- `plc_tag_read()` with timeout: the synchronous wait loop in `lib.c` uses `cond_wait(tag->tag_cond_wait, ...)`. The session thread signals this after inline response processing (Step 5). Also, `plc_tag_status_impl()` already calls `vtable->tickler()` inline as a fallback pump.

Handle `plc_tag_abort()`:

- `ab_tag_abort_request()` must also remove the tag from `active_tags` and reset its `op` state.

Handle `plc_tag_destroy()`:

- Same: remove from `active_tags` before the tag is freed.

**Why**: This is the switch-over. AB tags are no longer polled by the tickler.

**Test**: Run the **complete** test suite including all PLC types. This is the most critical test point. Specifically verify:

- `test_auto_sync` — auto-sync reads/writes still work.
- `test_callback`, `test_callback_ex`, `test_callback_ex_logix` — events fire correctly.
- `test_reconnect`, `test_reconnect_after_outage_async/sync` — reconnection still works.
- `test_idle_disconnect` — idle sessions disconnect correctly.
- `test_shutdown_cip`, `test_shutdown_restart` — clean shutdown works.
- `test_simultaneous_rw_ab_server` — concurrent reads/writes work.
- `test_fairness` — no starvation.
- `perf_benchmark`, `test_emulator_performance` — performance is same or better.
- All PCCC tests (`plc5`, `slc500`, multithread_plc5, etc.).
- Special tags: `list_tags_logix`, `test_tag_type_attribute` (UDT), `test_raw_cip`, `get_identity`.

---

### Step 8: Do the Same for Omron Tags

**What**: Apply the same pattern to Omron tags. Omron uses a `conn.c` connection object similar to `session.c`. Add `active_tags` to the Omron connection, implement `omron_tag_data_written`, inline response parsing, auto-sync scheduling in the Omron I/O thread, and set `skip_tickler = 1`. Also linearize the Omron connection handler following the same blocking-style pattern from Steps 3–4.

**Why**: Omron has the same architecture as AB (request objects, separate tickler thread). It needs the same migration.

**Test**: Run all Omron simulator tests. Verify auto-sync, callbacks, and standard operations.

**Files touched**:

- `src/libplctag/protocols/omron/conn.c` — add active_tags, tickle_active_tags, linearize handler.
- `src/libplctag/protocols/omron/omron_common.c` — set skip_tickler.
- `src/libplctag/protocols/omron/omron_standard_tag.c` — tag_data_written.

---

### Step 9: Remove `tag_tickler_func` Thread

**What**: With all protocol tags setting `skip_tickler = 1`, the `tag_tickler_func` thread does no useful work. Remove it:

1. Delete `tag_tickler_func()` from `lib.c`.
2. Remove `tag_tickler_thread`, `tag_tickler_wait`, and related globals.
3. Remove `plc_tag_tickler_wake()` / `plc_tag_tickler_wake_impl()`.
4. Remove the tickler thread creation from `lib_init()` and teardown from `lib_teardown()`.
5. Remove `plc_tag_generic_tickler()` (auto-sync scheduling now in session threads).
6. Clean up `plc_tag_status_impl()` — it still needs to call `vtable->tickler()` inline for the synchronous polling path, but the background thread is gone.
7. The `skip_tickler` field can be removed from `TAG_BASE_STRUCT` or left for compatibility.

Also move the remaining generic responsibilities:

- Event raising (`tag_raise_event`) — already called from session threads (Steps 5–6).
- Callback dispatch (`plc_tag_generic_handle_event_callbacks`) — already called from session threads and from `plc_tag_status_impl` / `plc_tag_read` / `plc_tag_write`.

**Test**: Full test suite. The system must behave identically to Step 7 since the tickler was already bypassed.

**Files touched**:

- `src/libplctag/lib/lib.c` — remove tickler thread, `plc_tag_generic_tickler`, wake functions.
- `src/libplctag/lib/lib.h` or `tag.h` — remove `plc_tag_tickler_wake` declarations, optionally remove `skip_tickler`.

---

### Step 10: Remove Request Objects

**What**: Replace `ab_request_t` with direct buffer management in the session, mirroring Modbus's `read_data[]`/`write_data[]` pattern.

This is the most invasive step and can be broken into sub-steps:

**10a: Session builds packets directly from tag data.**

- Instead of the tag building a request and the session copying it into its buffer, the session calls a tag vtable method (`build_request`) that writes directly into `session->data`.
- The tag's `op` state tells the session what kind of packet to build.
- Remove `session_create_request()` and `session_add_request()`.
- **Note**: Fragmented reads/writes require the tag to track `fragment_offset` and `total_fragment_size` internally.

**10b: Session parses responses directly into tag data.**

- Instead of unpacking into a request buffer for the tickler to parse later, the session parses CIP responses directly into `tag->data`.
- The per-tag-type response parsers (`check_read_status_connected`, etc.) are refactored to accept a buffer pointer and length instead of reading from `tag->req->data`.

**10c: Request packing without request objects.**

- The CIP multi-request packing in `pack_requests()` builds directly from tag data in the `active_tags` vector.
- For each tag in REQUEST state, the session calls the tag's build function to append its CIP payload to the multi-request buffer.
- Response unpacking routes sub-packets to the corresponding tag's parse function.

**10d: Remove `ab_request_t` and related infrastructure.**

- Delete `session_create_request()`, `session_add_request()`, `ab_request_t`.
- Remove `tag->req` from `ab_tag_t`.
- Remove `check_request_status()` from `ab_common.c`.
- Remove `ab_tag_abort_request()` / `ab_tag_abort_request_only()` (replaced by `remove_tag_from_active`).

**Why**: Request objects are the main remaining architectural difference from Modbus. Removing them eliminates allocation overhead, ref-counting overhead, cross-thread polling, and simplifies the code.

**Test**: Full test suite after each sub-step. Pay special attention to:

- Multi-packet fragmentation (CIP reads/writes > 1 packet).
- Request packing (multiple tags in one EIP packet).
- Error recovery (socket failure mid-request).
- Special tags (listing, UDT use multi-step request sequences).

**Files touched**:

- `src/libplctag/protocols/ab/session.c` — rewrite `process_requests`, remove request infrastructure.
- `src/libplctag/protocols/ab/session.h` — remove request type.
- `src/libplctag/protocols/ab/eip_cip.c` — refactor build/check functions.
- `src/libplctag/protocols/ab/eip_cip_special.c` — refactor special tag build/check.
- `src/libplctag/protocols/ab/pccc.c` — refactor PCCC build/check.
- `src/libplctag/protocols/ab/ab_common.c` — remove `check_request_status`, `ab_tag_abort_request`.

---

## Risk Analysis

| Step | Risk | Mitigation |
|------|------|------------|
| 1 | Low — pure data structure addition | No behavioral change, existing tests pass |
| 2 | Low — additive, tickler still runs as fallback | Both paths cooperate via `write_in_flight` guard |
| 3 | Low — pure refactoring, extracting existing code into blocking wrappers | Each wrapper composes existing tested helpers; no new logic |
| 4 | Medium — structural rewrite of session_handler | Behavior-preserving, but subtle edge cases in goto cleanup ordering; test all connection/disconnection paths |
| 5 | Medium — response parsing on session thread could expose lock ordering bugs | Try-lock with fallback to tickler; gradual rollout |
| 6 | Medium — auto-sync timing must be precise | Compare timing against baseline; tickler still active as double-check |
| 7 | **High** — this is the cutover, tickler no longer processes AB tags | Must pass full test suite; can revert by clearing `skip_tickler` |
| 8 | Medium — Omron follows same pattern | Omron has fewer tag types, lower complexity |
| 9 | Low — tickler already bypassed | Removing dead code |
| 10 | **High** — invasive refactor touching all protocol files | Break into 4 sub-steps, test after each; can stop after 10a/10b if needed |

## Handling Special Tag Types

All special tag types (listing, raw, UDT, identity) follow the same request/response/tickler pattern as standard CIP tags. They differ only in:

- **What CIP service they send** (e.g., Get_Instance_Attribute_List for listing, Get_Attributes_All for identity)
- **How they parse responses** (listing builds a growing buffer, UDT has a two-phase metadata+fields read)
- **Multi-step sequences** (listing uses `next_id` for continuation, UDT reads metadata then fields)

These differences are encapsulated in their vtable `tickler`/`read`/`write` functions. The migration preserves these functions — only the *scheduling and dispatch* changes (from tickler-polled to session-driven). Specifically:

- **Listing tags**: `listing_tag_tickler` does continuation reads. In the new model, after parsing a partial listing response, the tag stays in `TAG_OP_READ_REQUEST` with `op_time = now` (immediate next request). The session thread picks it up on the next cycle.
- **UDT tags**: `udt_tag_tickler` has a two-phase read. Same approach — the tag's op state drives the session to issue the next request.
- **Raw tags**: Write-only, single packet. Straightforward.
- **Identity tags**: Read-only, single packet. Straightforward.

## Performance Impact

| Metric | Before | After |
|--------|--------|-------|
| Threads per session | 2 (session + tickler) | 1 (session only) |
| Per-tag overhead (idle) | O(1) per tickler cycle (hashtable scan) | Zero (not in active_tags) |
| Response latency | +10–100ms (tickler wake cycle) | <1ms (inline processing) |
| Auto-sync jitter | Up to 100ms (tickler period) | Sub-ms (session wait precision) |
| Memory per request | `ab_request_t` + data buffer allocation | None (session buffer only) |
| Lock contention | tickler vs API vs session (3-way) | session vs API (2-way) |

## Dependency Graph

```text
Step 1 (sorted list) ──────────┐
                               │
Step 2 (tag_data_written) ─────┤
                               ├──→ Step 7 (skip_tickler) ──→ Step 9 (remove tickler)
Step 3 (blocking helpers) ─┐   │                                       │
                           ├───┤                                       ↓
Step 4 (linear loop) ──────┘   │                              Step 10 (remove requests)
                               │
Step 5 (inline response) ──────┤
                               │
Step 6 (session auto-sync) ────┘

Step 8 (Omron) ── depends on Steps 1–7 pattern, parallel with Step 9
```

Steps 1–2 and Steps 3–4 are independent tracks that can be developed in parallel. Steps 5–6 depend on both tracks (they modify the session thread's steady-state loop from Step 4 and use the sorted list from Step 1). All must be integrated before Step 7.

# 2.7.3 release plan

Planned 2026-11-10.  No code written.

Sources:

- `docs/architecture_review_2026_11.md` (the review): defect IDs (C, P, R, M, U) and the
  target design in its sections 1 and 2.
- `docs/release_2_7_3_must_do.md` (must-do): the release gate items, by number.

## Order and why

1. Fix defects that do not depend on the refactor first, so the refactor does not carry them.
2. Refactor from the bottom up: building blocks, then the creation seam, then one family at a
   time.  Each phase leaves the library working and the suites green.
3. Run the performance gate last, on the final code.  Numbers taken before the refactor do not
   describe what ships.

## Rules for every phase

- Build: gcc with `SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk`.  Check the
  build's exit code; a failed build leaves stale binaries in `bin_dist`.
- Simulator suite passes:
  `rm -rf /tmp/logs && caffeinate -i python3 src/tests/scripts/run_simulator_tests_parallel.py ./build/bin_dist/ /tmp/logs --max-stress=200`.
  Compare wall-clock time with the runner's total; if they disagree, the machine slept.
- Phases that change wire traffic also need the hardware suite, which the user runs:
  `python3 src/tests/scripts/run_hardware_tests.py ./build/bin_dist/ /tmp/logs`.
- Each non-trivial fix leaves one test that fails without it.  Client-observable behavior gets
  a `--tag=`-style CLI test wired into the runner.
- No commits; the user commits between phases.
- Mark each step done in this file as it lands.

---

## Phase 0: decisions and evidence

No library code changes.

| Step | Work | Output |
| --- | --- | --- |
| 0.1 | Audit the 20 `mutex_create` / `mutex_destroy` sites: can any lock a legitimately NULL mutex? | **Done.** See "0.1 result" below.  **Q1 decided 2026-11-10: Option C, remove the wrapper.** |
| 0.2 | Test for R1: read `@tags` twice on one handle, compare size, `elem_count` and bytes | **Done; R1 confirmed.** `src/tests/test_tag_listing_reread`, in the Logix matrix of `run_hardware_tests.py` (hardware only; `ab_server` has no tag listing).  Hardware run 2026-11-10: fails on all four Logix cells, `elem_count` 1 + N then 1 + 2N; size and bytes match.  No other hardware test failed. |
| 0.3 | Shutdown test | **Done; fails today as intended.** `src/tests/test_shutdown_drain`, in the simulator suite (timing group).  See "0.3 result" below. |
| 0.4 | Fix the stale must-do #16 entry: the treap is committed (c4753f13) | **Done.** |

### 0.1 result

Every lock of a NULL mutex found is one of two kinds; tag mutexes have neither.

1. **A destructor running on a half-built object.**  `session_create()` (`conn.c:2604`) calls
   `rc_dec(conn)` when `mutex_create()` fails, and `session_destroy()` then locks the NULL
   `session_mutex` (`conn.c:1068`).  The Modbus PLC is the same: `modbus_plc_destructor()`
   locks `plc->mutex` (`modbus.c:788`) after a failed create at `modbus.c:662`.  The lock is
   skipped silently (U5), which is harmless only because the body has nothing to clean yet.
2. **A module lock outside startup to teardown.**  `cleanup_mutex` (`rc.c`), the CIP
   `list->mutex` and `mb_mutex` are NULL before startup and after teardown.  Callers guard
   them with unlocked `if(mutex)` reads.  0.3 reproduces this: 11 to 12 "null mutex pointer"
   lock and unlock pairs per run.
3. **Tag `api_mutex` and `ext_mutex`:** created in `plc_tag_generic_init_tag()` before the tag
   is published, destroyed in the destructor; no destructor locks them.  Not NULL when locked.

Option C removes both kinds, with two rules:

- An object initializes its embedded lock before any step that can fail.  If the lock itself
  fails to initialize, the object is freed directly, without its destructor.
- Module locks are static and statically initialized, never destroyed while the library is
  loaded.  None of the three is taken recursively once U2 is fixed, so `PTHREAD_MUTEX_INITIALIZER`
  and `SRWLOCK_INIT` work.  Phase 2 confirms that by running the suites with error-checking
  mutexes.

Tag `api_mutex` must stay recursive: callbacks run inside it and call the API.

Found on the way: **M4**, `plc_count` goes negative (see the review).

### 0.3 result

`test_shutdown_drain --conns=60 --max-ms=3000` against `ab_server --delay-ms=100`:

- Today: `plc_tag_shutdown()` takes about 6.3 s.  The log shows "Timeout waiting for 12
  connections to terminate", then 11 to 12 NULL-mutex lock and unlock pairs as the late
  destructors run (U7, U5).  Thread count before and after is equal, so every thread is
  joined in the end.
- Pass after Phase 1: under 3 s and no thread left over.
- The delay must stay under the 150 ms Forward Close reply timeout hardcoded at
  `conn.c:761`.  At 300 ms every close times out and the test measures that timeout instead.

### Baseline simulator run (2026-11-10, no library changes)

332 tests, 330 passed, 226 s (wall clock agreed).  Failures:

- Test 59, `test_shutdown_drain`: the intended failure (6280 ms, threads 1 and 1).
- Test 47, "auto-sync reads at mixed periods (AB ControlLogix)" (stress group): 200 of 700 tags
  read under half their expected count; the 50 ms and 100 ms tags had gaps up to 885 ms.  Run
  alone twice against its own `ab_server`, it passes with every period on target.  A failure
  under the stress phase's load, not caused by Phase 0.  Not yet tracked anywhere else; watch it
  in later runs, and look at it if it fails under the Phase 9 fairness gate.
- Test 47's server log also shows P1 happening for real: at shutdown a Forward Close
  arrived with session handle 0 and the server rejected it ("request session handle is 0").

## Phase 1: memory safety, shutdown, Modbus init

| Step | Defects | Work |
| --- | --- | --- |
| 1.1 | C1, C2 | **Done.** The five string functions and both raw-byte functions check and update under `api_mutex` and touch nothing after `rc_dec()`, through two helpers, `check_string_access_unsafe()` and `check_raw_bytes_access_unsafe()`.  Also fixed: two "Done." logs read `tag->tag_id` after `rc_dec()` in `plc_tag_register_callback_ex()` and `plc_tag_unregister_callback()`. |
| 1.2 | C3 | **Done.** The 19 getters that read `byte_order` return `PLCTAG_ERR_UNSUPPORTED` when it is NULL.  Test: three checks in `test_connection_tag`'s attribute rules. |
| 1.3 | C4 | **Done.** `plc_tag_set_string()` returns `PLCTAG_ERR_BUSY` when the string changes size while an operation is in flight. |
| 1.4 | C5, C6, C7, M2 | **Done.** |
| 1.5 | C8 | **Done.** |
| 1.6 | M1, M3 | **Done.** |
| 1.7 | P6 | **Done.** `session_handler()` closes its own connection on the way out, with a handler-only `closing` flag that `wait_for_socket()` honours instead of toggling `terminating`.  `session_destroy()` joins and frees, with no network I/O and no lock.  Modbus needs no change: its close is only a socket close.  `test_shutdown_drain`: 6275 ms to 117 ms. |
| 1.8 | U7, U8, U9, M4 | **Done.** `destroy_modules()` runs `refcount_teardown()` first (the one wait, no timeout), then frees module state, then the registry.  No module signals or waits: handlers stop on `lib_active`.  Deleted: `refcount_drain()`, Modbus `plc_count`, `plc_cleanup_cond` and `handler_threads_active`, CIP `handler_count` and `SESSION_TEARDOWN_TIMEOUT_MS`.  The cleanup queue is a list linked through the refcount header under a static `lock_t` spinlock (`LOCK_INIT`, no resource, never freed), so queueing cannot allocate or fail.  That replaces the planned static mutex. |
| 1.9 | U2 | **Done** by 1.8: queueing cannot fail, so the fallback that ran a destructor under the lock is gone. |

Left for Phase 2: a destructor that runs after module teardown (a user thread holding a tag reference across `plc_tag_shutdown()`) still finds the module list mutex NULL and skips the unlink silently.  Option C's static module locks fix it.

Exit: 0.3 passes; suites green; hardware suite green (1.7 changes Forward Close timing).

**Phase 1 done 2026-11-11.**  Simulator: 331 of 332 (only test 47, the known load failure).
Hardware: only the four R1 listing tests fail, as expected until 3.5.

## Phase 2: platform, atomics, locks

| Step | Defects | Work |
| --- | --- | --- |
| 2.1 | U1 | **Done.** Two paths only: MSVC uses Interlocked calls for every operation, loads included; every other compiler uses C11 `<stdatomic.h>`.  Found on the way: MSVC builds (`/std:c11` without `/experimental:c11atomics`) took the fallback, whose bool compare-and-set returned "swapped" instead of the original value.  So `socket_wake()` skipped the first wake, and `debug.c` ran its one-time setup on the wrong branch. |
| 2.2 | U5, U6 | **Done.** `mutex_t` is embedded by value (`pthread_mutex_t` / `CRITICAL_SECTION`, both recursive) with an `initialized` flag, so destroying a zeroed, never-initialized mutex is a no-op.  `mutex_lock()` returns nothing; `mutex_try_lock()` and `mutex_unlock()` keep an `int` for `plc_tag_lock()` / `plc_tag_unlock()` (unlock by a non-owner is an error, checked through `OwningThread` on Windows).  `critical_block` and `spin_block` use per-line names (`LINE_ID`).  Module locks (`mb_mutex`, the CIP list mutex) are initialized once and never destroyed: a late destructor still finds them, and `CRITICAL_SECTION_NO_DEBUG_INFO` keeps them free of allocations across an unload.  That replaces the planned static non-recursive locks: one lock type, all recursive. |
| 2.3 | U3 | **Done.** Only `standard_tag.c` assigned the realloc result straight to `tag->data`, after changing `tag->size`.  Fixed.  The other callers already used a temporary. |
| 2.4 | U4 | **Done, partly.** `str_dup()` moved out of `reuse_lock`.  Left, with `ponytail:` comments: `hashtable_put()` can grow the table under the spinlock, and `publish_tag()` probes long only when the table is nearly full.  Measure before moving either. |
| 2.5 | — | **Done.** `mem_*` sizes are `int32_t`; `find_entry()` is `static`. |

Exit: suites green, plus a Windows CI build.

**Phase 2 2026-11-11:** simulator 331 of 332 (only test 47, the known load failure); hardware:
only the four R1 listing tests fail.  Waiting on the Windows CI build.

> **Redone 2026-11-16 in the current tree.**  The rollback (6d34134c) had reverted most of
> Phases 3 to 6; the text that described the abandoned code path (69edc10d) is replaced below.
> Steps marked "skipped" were judged against this tree, not copied from the old verdict.
> Found on the way, from Phase 2: U3 (2.3) was back in `eip_cip.c`'s read; fixed.  Phases 1 and
> 2 were re-checked 2026-11-17: most of Phase 1 had regressed (C1, C2, C4 to C8, M1 to M4, and
> 1.8) and is redone in this tree; Phase 2 held.  Each regression and its guard test is in
> `regression_log.md`.  Simulator 370/370; hardware suite green 2026-11-17.

## Phase 3: building blocks

Review section 2.  Each block is extracted only where the copies are actually identical.
Behavior does not change, so the existing suites are the test.

| Step | Block | Result |
| --- | --- | --- |
| 3.1 | Gateway split | **Reverted 2026-11-17.**  `attr_split_gateway()` put a gateway-only parser in the attribute code.  CIP `session_open_socket()` and Modbus `connect_plc()` are back on `str_split()` from the platform code, as before 3.1.  That allocates once per connect, and the port is not range-checked; `socket_connect_tcp_start()` gets whatever `str_to_int()` returns.  Simulator 370/370, hardware suite green 2026-11-17. |
| 3.2 | Idle tracker, retry wait | **Skipped, not identical.**  When the timeout changes, CIP restarts the deadline from now and Modbus from the last packet.  The retry wait is already `backoff_t`. |
| 3.3 | Unlock, back off, relock | **Skipped: one copy.**  Only Modbus tries the tag lock from the handler; CIP has no such loop in this tree. |
| 3.4 | Framed receive, send what fits | **Skipped, not identical.**  Both move what they can and return PENDING now, but the header size, the length field and the bookkeeping (events against timestamps) differ; the shared part is `socket_read()`/`socket_write()`. |
| 3.5 | Fragment accumulator | **Skipped: its defects are fixed directly.**  R1 (`elem_count = 0` per read) and R3 (one `append_bytes()` for the member area) are in place; every realloc site keeps the old buffer on failure.  `utils/byte_buffer.h` and its test are unused by the library: Phase 8 deletes them or a measurement brings them back. |
| 3.6 | CIP reply-header check | **Done:** `cip_check_reply()` (`ab/error_codes.c`) in the listing, the UDT metadata and fields reads, the OMRON listing and the OMRON size probe.  It also refuses a reply shorter than its header.  Not used where the shape differs: the standard read and write accept several services. |
| 3.7 | Global connection list becomes intrusive | **Done for CIP**, linked through `cip_conn_t.next`; Modbus already was.  The vector, its unchecked `vector_set()` and the ABA FIXME are gone. |

## Phase 4: the creation seam

| Step | Work | Result |
| --- | --- | --- |
| 4.1 to 4.4 | `plc_tag_create_from_tag()` without protocol switches | **Done, smaller than planned.**  The tag base has `conn` (the tag's CIP session or Modbus PLC; the protocol's own field holds the reference) and `create_func`, both set once during create.  `plc_tag_create_from_tag()` refuses a source with no `conn` (`PLCTAG_ERR_NOT_ALLOWED`) and calls its `create_func`; each protocol takes `rc_inc(src_tag->conn)`.  A clone takes its PLC type from the connection.  No `conn_base_t` or per-connection `create_tag`: each AB PLC type already has its own `tag_create`. |
| 4.5 | `protocol_type` goes | **Done.**  The `tag_protocol_t` enum and field, all seven switches and `ab_connection_tag_view_t` are gone.  `tag_can_auto_destroy()` reads a `can_auto_destroy` bit set by the AB and Modbus data tag creates. |
| 4.6 | Debug module IDs | **Already true.** |

Exit: `test_create_from_tag` (including the system-tag source), `test_find_or_create_tag`,
`test_connection_tag` and `test_connection_tag_late_join` in the simulator suite.

## Phase 5: one family at a time

| Step | Family | Defects | Result |
| --- | --- | --- | --- |
| 5.1 | OMRON NJ/NX | R2 | **Already true.**  Both listings reset `elem_count` to 0 per read and count entries.  `@identity` and `@raw` have one vtable each. |
| 5.2 | Logix and Micro800 | R4, P5 | **R4 done:** `udt_fields_size()` returns 0 under 6 words or over 16 bits, and the fields read is `PLCTAG_ERR_BAD_REPLY` before it asks for a request; the metadata must be at least 6 bytes.  Unit test `test_udt_size`, re-enabled.  **P5 already true:** 0 to 255 at parse, a bit past the element is `PLCTAG_ERR_OUT_OF_BOUNDS` after the first read, and PCCC checks each file's width.  Special tags are matched per PLC type's `tag_create`. |
| 5.3 | Modbus | — | **Done.**  "Send the next piece" is `PLCTAG_STATUS_PENDING`, not `PLCTAG_ERR_PARTIAL`; exception 0x05 keeps asking again.  Found on the way: a write reply for another request returned PENDING, which completed the write with status PENDING; it is `PLCTAG_ERR_NO_MATCH`, as the read path was.  No test: the handler matches the reply to the tag by transaction ID first. |
| 5.4 | Vtable casts | P7 | **Done.**  No `(tag_vtable_func)` cast remains (44 removed).  Functions only reached through a vtable take `plc_tag_p`; `ab_tag_abort_request()`, with 43 direct callers, gets `ab_tag_vtable_abort()`.  Codec structs and `canonical_name` skipped: no codec is in the vtable, and the one `plc_type` attribute switch (`ab_get_elem_type()`) is plain. |
| 5.5 | Decide which `tag_vtable_t` entries remain | — | **Decided 2026-11-16: all stay; no code change.**  Each has callers in `lib.c` and an implementation that does real work.  `wake_plc` cannot fold into `abort`: `plc_tag_abort()` wakes the PLC thread before it waits for `api_mutex`.  `activate` publishes the Modbus tag only after the generic setup. |

## Phase 6: PCCC

| Step | Work | Defects | Result |
| --- | --- | --- | --- |
| 6.1 | PLC-5 codecs separate from SLC | — | **Skipped.**  Each PCCC encoder shares about 80 lines and differs in one header struct and one size byte; splitting duplicates the shared part. |
| 6.2 | DH+ prefix | — | **Already one place:** `pccc_prefix_size()` and `pccc_write_prefix()`. |
| 6.3 | PCCC `elem_type` reads `file_type` | — | **Already true** (`ab_get_elem_type()`). |
| 6.4 | TNS | P3, P4 | **P3 done:** `session_get_new_seq_id()` returns `uint16_t`; the unused 64-bit `_unsafe` variant is deleted.  **P4 not a defect here:** PCCC never packs, so every encoded request is sent; one an abort drops only skips a TNS. |
| 6.5 | DH+ handler | — | **Decided: neither.**  Unchanged. |

Also done: `pccc.h` is `#pragma once` with no `//` comments (from the Phase 8 list).

Exit: hardware suite on PLC-5, SLC, MicroLogix and DH+.

## Phase 7: CIP handler

| Step | Work | Defects | Status |
| --- | --- | --- | --- |
| 7.1 | Connected and unconnected handshakes as separate direct code; the probe is a phase in front of either | — | **Done 2026-11-15** by the state machine rewrite (`cip_state_machine_design.md` step 5): `connected_step()`, `unconnected_step()`, and `identify_step()` in front.  Hardware passed. |
| 7.2 | Single-request and Multiple Service Packet exchange builders, chosen at connection creation | — | **Done 2026-11-15** (same doc, step 4): `requests_single_step()` and `requests_packed_step()` on `exchange_step()`; packing follows the PLC type, not the framing.  Hardware passed. |
| 7.3 | Forward Close fills the session handle and clears the buffer | P1 | **Done 2026-11-16.**  `session_forward_close()` zeroes the header and path, then sets the handle.  Unit test `test_conn_path_fc` (stale 0xAA buffer).  Hardware passed 2026-11-16. |
| 7.4 | 16-bit connection path size | P2 | **Done 2026-11-16, not as planned: the size stays 8 bits.**  `cip_encode_path()` refuses a route over 248 bytes (`PLCTAG_ERR_TOO_LARGE`), so the pad byte and the DH+ or Message Router suffix still fit 255.  It used to allow 260 plus the suffix and wrap.  Same unit test.  Hardware passed 2026-11-16. |

Exit: hardware suite.

## Phase 8: deletion pass

Review sections 5 to 8.

- Unused functions and their tests (review section 8 table).  `test_conn_leak.c` gets another
  leak oracle or goes with `cip_conn_live_count()`.
- Dead code, duplicate constants, the `path.c` 256-entry table, the duplicate Identity encoder,
  the second Multiple Service Packet validator in `conn.c`.
- `error_codes.c`: drop the copied long descriptions.
- `pccc.h` to `#pragma once` and `/* */` comments.
- Refcount cleanup queue becomes a ring or an index.

Exit: suites green.

**Done 2026-11-17 in the current tree** (the review's file names are from the abandoned tree).
Simulator 370/370, unit 17/17, hardware suite green 2026-11-17.

- Deleted, unused: `thread_stop()`, `thread_kill()`, `thread_detach()` (both platforms),
  `mem_cmp()`, `str_concat()` and the `COUNT_NARG` macros, `str_to_float()` (and its test case),
  `add_session()`, `session_get_max_payload()`, `attr_clamp_int32()`,
  `cip_lookup_data_element_size()`, `pccc_calculate_bcc()`, `pccc_calculate_crc16()` and its
  table, `pccc_encode_dt_byte()`, `debug_set_all_modules()` (its test uses `set_debug_level()`),
  `logix_tag_listing_byte_order`, `DEFAULT_NUM_RETRIES` / `DEFAULT_RETRY_INTERVAL`,
  `USE_GNU_VARARG_MACROS`, Modbus `PLC_BUILD_REQUEST` and `modbus_tag_list_t`, commented-out
  globals and code.  Step 1.8 already deleted the module teardowns and the AB `io_handler_thread`.
- `error_codes.c`: long descriptions and `decode_cip_error_long()` gone; logs use the short one.
- `cip.c`: the 256-entry type table is a range check; `match_ip_addr_segment()`'s four octet
  blocks are one loop, which also stops a long digit run overflowing the octet value.  New
  unit test `test_ip_addr_segment` in `test_conn_path_fc`.
- `sub_element_lookup` is `static const`; `modbug_cmd_t` is `modbus_cmd_t`.
- Kept: `utils/byte_buffer.h` (to be used later); `vector_swap_element()` and `time_us()` (in
  use here); the two six-byte identity requests (constants, not encoders).
- Already true: refcount queue is a linked list (1.8); `pccc.h` (Phase 6).
- Not in this tree: `path.c`, the `conn.c` Multiple Service Packet validator,
  `tag_schedule_*`, `cip_conn_live_count()`, `omron_common.c`.
- Not deletions, left: Modbus disconnect deadline in four places, `socket_wait_event()`
  classification shared across platforms, `elem_size` in bits.
- **Decided 2026-11-17:** moving every encoder and decoder to `byte_slice` (review section 8) is
  after 2.7.3.  Guideline gaps (`known_issues.md` 1, 2) are fixed as files are touched.
- **Done 2026-11-17:** session lookup matches on `use_connected_msg` (`known_issues.md` 3);
  `pdebug_impl()` has a printf format attribute (21 mismatches, `known_issues.md` 4).
  Simulator 370/370, hardware suite green 2026-11-18.
- **Decided 2026-11-18:** the two allocations per CIP tag read (`known_issues.md` 5) are fixed
  after 2.7.3.

## Phase 9: release gate

Runs on the code from Phase 8.  Items are from the must-do list.

| Item | Work | Who |
| --- | --- | --- |
| #6 | Connection-count ramp on the L81, 1 to 8 connections | Built 2026-11-18 (`run_connection_ramp.py`, in `run_hardware_tests.py`); user runs |
| #8 | Modbus tag ramp, 50 to 50000 tags | Built 2026-11-18 (`run_tag_ramp.py --protocol=modbus`); user runs on Linux |
| #13 | Allocation counting by interposing, macOS and Linux | Built 2026-11-18 (`liballoc_count`, `run_tag_ramp.py --alloc-count`); macOS checked, Linux needs `-DUSE_VALGRIND=1` |
| #10 | v2.7.2 vs HEAD, no regressions | User, Linux |
| #11 | Fairness gate thresholds, final on Linux | User, Linux |
| #14, #15 | Linux fairness and Modbus scale matrices; final performance numbers | User, Linux |
| #18 | Backoff unit tests in Windows and ARM CI | Check the CI run |

Release when all of Phase 9 passes.

---

## Scope

**Decided 2026-11-10:** everything, Phases 0 to 9 including the refactor, ships in 2.7.3.

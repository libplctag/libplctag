# Architecture review and defect ledger, 2026-11-10

Scope: every `.c`/`.h` under `src/libplctag`, `src/utils` and `src/platform` on branch
`slot_table` (c4753f13).  `tools/`, `tests/`, `poc/`, `wrappers/` and `examples/` were not
reviewed.  Line numbers are against that commit.

Decisions from the first review round (2026-11-10) are folded in and marked **Decided**.

Sections:

1. Where device and protocol knowledge lives
2. Building blocks instead of a shared high-level flow
3. Defects to fix (confirmed by reading; one suspected, marked)
4. `critical_block` failure semantics
5. CIP cleanup
6. AB and OMRON cleanup (later)
7. Modbus cleanup
8. utils and platform cleanup
9. Public API: debug module IDs

---

## 1. Where device and protocol knowledge lives

### The rule

Device and protocol knowledge enters at two dispatches, each made once:

1. **Protocol dispatch.**  `protocol=` selects a module's create function (`lib/init.c`,
   `tag_type_map`).  This is the only place the core names a protocol.
2. **Dialect dispatch, CIP only.**  The CIP module picks a family from `plc=` or from the
   Identity probe, once, when it creates the connection.  The connection stores the family's
   `create_tag` function.  Every tag on that connection is created by calling it.  Nothing
   after that asks which family it is.

No generic code branches on a device or protocol type.  No device code branches on a sibling
device's type.  Direct code is preferred to `if`/`switch` on a type.  Where two devices
differ, each gets its own function, even at the cost of some duplicated code.

### The shape

- **Every connection object exposes its tag creation function.**  CIP connections and Modbus
  PLC objects begin with a common header that the core can see:

  ```c
  typedef struct conn_base_t conn_base_t;

  struct conn_base_t {
      plc_tag_p (*create_tag)(conn_base_t *conn, attr attribs);
  };
  ```

  The tag base struct holds a counted reference to its `conn_base_t`.

- **Create from an attribute string.**  `protocol=` selects the module.  The module finds or
  creates the connection by its key and calls `conn->create_tag(conn, attribs)`.  For CIP,
  the module's create function is the one dialect dispatcher: it picks the family (or starts
  a probe) and creates the connection with that family's `create_tag`.  Modbus has one
  dialect, so its PLC object always holds the Modbus `create_tag`.

- **Create from a parent tag.**  `plc_tag_create_from_tag(parent, partial_attribs)` takes
  `parent->conn` and calls `conn->create_tag(conn, partial_attribs)`.  No type test, no
  copied `create_func`, no new vtable entry.  This deletes the switches at `lib.c:527`,
  `cip_tag_create.c:387`, `cip/connection_tag.c:68` and the two in `modbus.c`
  (`mb_tag_create()`, `mb_connection_tag_create()`).

- **A family's `create_tag` owns everything about its tags**, written as direct code in the
  family's module:
  - Special tags (`@tags`, `@udt/`, `@identity`, `@raw`, `@connection`) are matched by name
    in the family's own function.  Matching a tag name is not a device branch.  A family that
    does not serve a special tag simply does not match it, so `is_logix` goes.
  - The attribute table, byte order, debug module ID and data-tag kind are chosen there.
  - Where families share a tag kind, the family calls the shared constructor directly, for
    example `cip_identity_tag_create(conn, attribs, &omron_attribs)`.  The duplicated
    `@identity` and `@raw` vtables in `ab_common.c` and `omron_common.c` go.
  - `@connection` is created by the connection's own `create_tag`.  The family supplies the
    protocol and debug module, so `is_omron` and `TAG_PROTOCOL_*_CONNECTION` go.

- **The CIP family descriptor** (`cip/family.h`) keeps only what the shared connection code
  reads: payload sizes, Forward Open parameters, `use_connected_msg`, the `dhp` link,
  `canonical_name` and `create_tag`.  `setup_tag` and `plc_type` go.  `canonical_name`
  replaces `ab_plc_type_name()` and `omron_plc_type_name()`.

- **Probed connections.**  Until the Identity reply arrives, `conn->create_tag` is a CIP
  module function that makes a deferred tag, as `deferred_vtable` does today.  When the probe
  completes, the connection stores the family's `create_tag` and finishes each deferred tag
  through it.  The deferred tag is the only tag allowed a NULL `byte_order` (see C3).

- **Tag processing belongs to the connection.**  The connection's handler thread already runs
  code specific to the device at the other end, so it encodes requests and decodes replies.
  `encode_read`, `decode_read`, `encode_write` and `decode_write` leave the core
  `tag_vtable_t`.  Each tag kind's codec lives in the device module and is reached through a
  typed pointer in that module's own tag struct, set by `create_tag`.  The codecs take
  `byte_slice` arguments (section 8), so no function-pointer casts are needed (P7).
  - Open: which of `abort`, `read`, `status`, `tickler`, `write`, `wake_plc`, `activate`,
    `tag_data_written` remain in `tag_vtable_t` once processing is in the connection.  Decide
    after the CIP handler is rewritten, not before.

- **PCCC is split by device, not by flag.**
  - PLC-5 gets its own codecs.  SLC and MicroLogix share theirs.
  - Each DH+ family gets its own request-prefix function, chosen by its `create_tag`.  Today
    `pccc_write_prefix()` and every decoder re-ask `is_dhp`.
  - PCCC families get their own attribute table, whose `elem_type` reads `file_type`.  Logix
    and Micro800 get one that reads `elem_type`.

- **`elem_size` has one unit (bytes).**  Modbus keeps its bit count in its own struct.

### Branch sites this deletes

| Site | Branch today | Removed by |
| --- | --- | --- |
| `lib.c:527` | switch on `src_tag->protocol_type` in `plc_tag_create_from_tag()` | `parent->conn->create_tag` |
| `cip_tag_create.c:387` | switch on `src_tag->protocol_type`, casts to `cip_tag_p` / `connection_tag_p` | `parent->conn->create_tag` |
| `cip_tag_create.c` `@connection` path | `is_omron` from protocol type or `&omron_family_njnx` | family creates its own `@connection` |
| `cip/connection_tag.c:68` | which source tags it adopts a connection from | `parent->conn` |
| `modbus.c` `mb_tag_create()`, `mb_connection_tag_create()` | switch on `src_tag->protocol_type` | `parent->conn` |
| `ab_common.c:151` | `is_logix` from `plc_type` | family matches only the special tags it serves |
| `ab_common.c:196` | `plc_type != CIP_PLC_LGX_PCCC` | per-family `create_tag` |
| `ab_common.c:407`, `:429` | `ab_get_elem_type`, `ab_get_raw_tag_type_bytes_size` switch on `plc_type` | per-family attribute tables |
| `ab_common.c:482` | names `"omron-njnx"` | `canonical_name` |
| `pccc.c` 1462, 1476, 1503, 1600, 1609, 1624 | `tag->plc_type == CIP_PLC_PLC5` | separate PLC-5 codecs |
| `pccc.c` every codec | `tag->session->is_dhp` | DH+ prefix function chosen at creation |
| `tag.h:169` | CIP-only codecs in the core vtable | codecs move to the device module |
| `tag.h:315` | `elem_size` "bytes, except Modbus: bits" | one unit |

The generic engine (`tag_driver.c`, `tag_schedule.c`, `conn_watch.c`, `connection_tag.c`,
`conn_attribs.c`, `backoff.c`) has no device branches today and needs none of this.

---

## 2. Building blocks instead of a shared high-level flow

The connection handlers look alike at a glance (connect, handshake, exchange, idle
disconnect, back off), but the differences matter:

- Modbus has no handshake.
- CIP always registers an EIP session.  It sends a Forward Open only for connected
  messaging, with DH+-specific parameters for DH+.  A probed connection reads the Identity
  object first.
- Some CIP families pack several requests into one packet; others never do.
- Modbus pipelines independent requests by transaction ID.
- PCCC sits inside CIP with its own TNS and requester checks.

So each protocol, and where needed each dialect, keeps its own handler function written as
direct code.  What gets shared is the pieces those handlers are built from: small functions
over common data structures, called from the handler, never calling back into it.

**Decided:** a block is extracted only where the copies are actually identical.  Anything that
differs stays in the handler.

### Already shared, and correct

- `tag_driver_t`, `tag_schedule`: due-tag list, auto-sync, aborts, re-arming, deferred callbacks.
- `conn_event_ring_t`, `conn_watch_*`, `connection_tag_*`: connection status and `@connection`.
- `conn_attribs.c`: the inactivity and retry attribute accessors.
- `backoff_t`.
- `tag_op_start` / `tag_op_end` / `tag_op_abort` / `tag_created`.

### To extract

| Building block | Common data | Repeated in |
| --- | --- | --- |
| `conn_config_from_attribs()` | `conn_config_t`: gateway, group ID, inactivity ms, retry max ms, sharing flag | `find_or_create_on_list()` (`conn.c`), `find_or_create_plc()` (`modbus.c`); both read and clamp the same attributes |
| `conn_split_host_port()` | host string and port | `session_open_socket()` (`conn.c`), `connect_plc()` (`modbus.c`) |
| Idle tracker: `conn_idle_touch()`, `conn_idle_expired()`, `conn_idle_refresh_timeout()` | `conn_idle_t`: last activity, atomic timeout, cached timeout, deadline | `session_handler()` (several places); Modbus updates `disconnect_at_time_ms` in four separate places |
| Retry wait: `conn_retry_start()`, `conn_retry_due()` | `backoff_t` plus the until-time, reading the live maximum | `SESSION_START_RETRY` / `SESSION_WAIT_ERR_RETRY`; `PLC_CONNECT_START` / `PLC_CONNECT_WAIT` / `PLC_ERR_WAIT` |
| `tag_driver_lock_tag_yielding()` | yield counter, `TAG_DRIVER_LOCK_YIELDS` | `dispatch_replies_unsafe()`, `fail_waiting_tags_unsafe()`, Modbus `dispatch_reply_unsafe()`, the yield blocks at the bottom of both handler loops |
| Framed receive: read a fixed header, compute total length, read the rest, bound by capacity | buffer, offset, capacity, a header size and a length computed from the header | `recv_eip_response()`, Modbus `receive_response()` |
| Send all with deadline | buffer, offset, length | `send_eip_request()`, Modbus `send_request()` |
| Fragment accumulator: append payload at an offset, grow up to a cap, count no-progress replies | buffer, size, offset, retry count, cap | `standard_tag.c` decode_read, `listing_tag.c`, two decoders in `udt_tag.c`, `omron_listing.c` `append_bytes()`, raw and identity decoders |
| CIP reply header check: service matches, status OK or allowed partial, map error | the 4-byte CIP reply header | every CIP decoder, written out each time |

Not extracted:

- The global connection lists (CIP vector in `cip_conn_list_t`, Modbus `plcs` linked list):
  both become an intrusive linked list (the link lives in the connection object).
  Connections are created rarely, and any wait falls inside `plc_tag_create*()`, so a linear
  find is fine.  Each module keeps its own list; whether the find/add/remove functions are
  identical enough to share is decided after the change.  Each connection's *tag* list is the
  treap.
- Pending-request ID set: Modbus `in_flight[]`, PCCC TNS and CIP sender context are not
  identical.
- Handler thread start and stop: the CIP and Modbus teardown waits differ, and both change
  with the P6 fix.  Revisit once both use the new form.

### Where CIP's own variation goes

Within CIP, the handshake and exchange differences are fixed when the connection is created,
so they become function pointers or direct calls chosen then, not tests inside the state
machine:

- **Handshake.**  `session_handler()` branches on `conn->family == NULL` (probe) and on
  `conn->use_connected_msg` in five places.  Two handlers (connected and unconnected), or a
  handshake function chosen at creation, each written directly, remove those branches.  The
  probe is a phase in front of either one.
- **Exchange building.**  `build_exchange_unsafe()` decides per tag whether it may bundle.
  Bundling depends on the connection (connected only) and on the tag kind (`plan.packable`).
  The connection-level half can be decided once, with two builders: single-request and
  Multiple Service Packet.

Open question: whether DH+ gets its own handler or only its own Forward Open encoder.  The
only DH+-specific handshake difference found is `CIP_EIP_PLC5_PARAM` in
`cip_encode_forward_open_old()`, which suggests the encoder is enough.

---

## 3. Defects to fix

All confirmed by reading the code unless marked.

### Core and API (`src/libplctag/api`, `src/libplctag/lib`)

| # | Where | Defect | Effect |
| --- | --- | --- | --- |
| C1 | `api/lib.c`, `plc_tag_get_string_capacity()` (from 2875), `plc_tag_get_string_length()`, `plc_tag_get_string_total_length()`, `plc_tag_set_raw_bytes()`, `plc_tag_get_raw_bytes()` | Early-return paths call `rc_dec(tag)` and then write `tag->status` | Use after release if another thread destroys the tag between the two.  Breaks the "every API call memory-safe" rule. |
| C2 | `api/lib.c`, `plc_tag_get_string()`, `plc_tag_set_string()` and the functions in C1 | `tag->byte_order`, `tag->data` and `tag->is_bit` are read, and `tag->status` written, before `api_mutex` is taken | Races the handler, which reallocates `tag->data` (`standard_tag.c:702`, `listing_tag.c`, `udt_tag.c`, `omron_listing.c`). |
| C3 | `api/tag_attribs.c:568` onward | Every `str_*` getter, `write_pad_alignment_bytes` and the five `*_byte_order` getters dereference `tag->byte_order` without a NULL check | `@connection` tags and CIP tags still waiting for a probed connection have `byte_order == NULL`.  `plc_tag_get_int_attribute(conn_tag, "str_is_counted")` crashes. |
| C4 | `api/lib.c:2722` | `plc_tag_set_string()` resizes the tag buffer without the `tag_op_busy()` check that `plc_tag_set_size()` has | Can shrink the buffer under a fragmented write in progress; the encoder then uses `tag->offset` against the new size. |
| C5 | `api/lib.c:2060` | `"Illegal new size %d bytes ..."` has no argument for `%d` | Undefined behavior whenever WARN logging is on. |
| C6 | `api/lib.c:117` | `plc_tag_generic_wake_tag_impl()` logs `tag->tag_id` inside its `if(!tag)` branch, and also in the SPEW line before it | NULL dereference when logging is on. |
| C7 | `api/lib.c:1139`, and the other `pdebug` in `plc_tag_register_logger()` / `plc_tag_unregister_logger()` | Arguments are `(DEBUG_MODULE_LIB, 0, DEBUG_DETAIL, ...)` with level and tag ID swapped | Logged at level 0 with tag ID 4. |
| C8 | `lib/byte_order.c:95` | `*new_byte_order = *(tag->byte_order)` copies `is_allocated` and then replaces the pointer.  If the tag already owned an allocated order, it leaks.  The deferred CIP create path calls `set_tag_byte_order()` after `setup_tag()`. | Memory leak per tag on that path if the family's setup allocated an order. |

**Decided (C1, C2):** fix each function in place.  The rule: take `api_mutex` before touching
any tag field, and touch nothing after `rc_dec()`.  No helper.

### CIP (`src/libplctag/modules/cip`)

| # | Where | Defect | Effect |
| --- | --- | --- | --- |
| P1 | `cip.c:237` `cip_encode_forward_close()` | Never sets `encap_session_handle` and never clears the buffer before filling it | Works only when the previous reply left the same session handle at that offset.  Reserved fields carry stale bytes.  Seen 2026-11-10 in the simulator suite: a Forward Close sent with session handle 0, rejected by `ab_server`. |
| P2 | `path.c:177` | `*tmp_conn_path_size = (uint8_t)conn_path_index`.  The encoded path plus the DH+ (6 bytes) or router (4 bytes) suffix can exceed 255 within `MAX_CONN_PATH` | The path length is silently truncated and the wrong path goes on the wire.  `conn.h` has a FIXME about the 8-bit size. |
| P3 | `conn.c:166` `session_get_new_seq_id()` | Returns `uint64_t` but stores the result in a `uint16_t` first | Truncated to 16 bits.  Callers use it as the PCCC TNS, so this happens to be what they want, but the function's contract is wrong. |
| P4 | PCCC encoders (`pccc.c`, `eip_lgx_pccc.c`) | Draw a TNS and write `tag->req_pccc_seq_num` while encoding; `tag.h` says encoding has no side effects | A request that is encoded and then not sent (it does not fit the packet) burns a TNS.  Harmless today because PCCC is never packed, but the contract is violated. |
| P5 | `path.c:960` | The message says the bit must be 0–255; the check allows 0–65535; nothing checks the bit against the element width | A bad bit number reaches the encoder.  `encode_write_bit()` then indexes `tag->data[tag->bit / 8]` against `elem_size`, not `tag->size`. |
| P6 | `conn.c` `session_destroy()`, around 1078 | Runs Forward Close (network I/O, up to about 250 ms) and `thread_join()` while holding `session_mutex`, on the single global refcount cleanup thread | Every other destructor in the process waits behind one slow PLC. |
| P7 | Every CIP vtable, e.g. `.abort = (tag_vtable_func)cip_data_tag_abort`, `.encode_read = (tag_encode_func)...`, cast back in `conn.c` as `(cip_tag_encode_func)encode` | Functions are called through pointer types that do not match their definitions | Undefined behavior in C.  Works on every ABI in use but needs `-Wno-cast-function-type`. |

**Decided (P6):** the handler thread owns the socket, so it sends Forward Close itself when it
sees the terminate flag.  `session_destroy()` only sets the flag and joins, without holding
`session_mutex`.  The deferred cleanup thread stays: it exists so that a handler thread
never has to join itself.  Worst case per destructor is one join, bounded by the close
timeout.  Modbus teardown takes the same form.

**Decided (P7):** the codec casts go with the move of the codecs off the core vtable
(section 1).  Any vtable entries that remain take `plc_tag_p` and cast inside, as Modbus does.

### Rockwell and OMRON (`src/libplctag/modules/ab`, `src/libplctag/modules/omron`)

| # | Where | Defect | Effect |
| --- | --- | --- | --- |
| R1 | `ab/listing_tag.c:302`, `:368` (confirmed on hardware 2026-11-10 by `test_tag_listing_reread`: L55, L61 and L81 all report `elem_count` 1 + N then 1 + 2N, e.g. 15 then 29; size and bytes match) | A finished read resets `offset` and `next_id` but not `size` or `elem_count` (`listing_tag.c:412`).  `size` only grows, `elem_count` only increments.  Only the create probe resets them (`:388`). | The second read reports `elem_count` 1 + 2N.  If the listing shrank, `size` keeps the old length and stale entries follow the new end. |
| R2 | `ab/listing_tag.c` vs `omron/omron_listing.c` | AB starts `elem_count` at 1 and increments per entry; OMRON resets to 0 and increments | The same attribute on the same tag kind means different things per family. |
| R3 | `omron/omron_listing.c:501` | `decode_udt_header()` appends `num_members * 8` zero bytes one byte at a time through `append_bytes()`, each a `mem_realloc()`.  `num_members` comes from the PLC. | Up to about 524,000 reallocations for one UDT, triggered by the device.  Fragments the heap, against the static-pool goal. |
| R4 | `ab/udt_tag.c` `udt_tag_encode_read_fields()` | `total_size = (4 * words) - 23` in `uint32_t`, with `words` from the PLC | Underflows when `words < 6`; the request asks for about 65,000 bytes. |

**Decided (R3):** append the whole run in one call, and grow the buffer geometrically: double
the capacity until a step would add more than about 2 kB, then grow in 2 kB steps.  No cap on
`num_members`.  This is the growth rule for the fragment accumulator (section 2).

R1, R4 and P2, P5 are the same class: a length or index from the wire or the caller is used
without a bounds check.  The fix for the class is `byte_slice` (section 8).

### Modbus (`src/libplctag/modules/modbus/modbus.c`)

| # | Where | Defect | Effect |
| --- | --- | --- | --- |
| M1 | `:662` | `tag_driver_init()` failure sets `rc = PLCTAG_ERR_NO_MEM`, then `rc = mutex_create(...)` overwrites it | The PLC goes live with `driver.notify == NULL`.  `notify_later_unsafe()` (`tag_driver.c:383`) takes a reference with `rc_inc()`, `vector_set()` fails, and the reference leaks: the tag is never freed and its callbacks never run. |
| M2 | `:486` `modbus_tag_destructor()`; also `modbus_plc_destructor()` | Logs `tag->tag_id` before the NULL check | NULL dereference when logging is on. |
| M3 | `:248` | `mb_mutex`, `plcs` and `modbus_tag_byte_order` are non-static globals | Exported symbols; collide with an application or another library. |
| M4 | `:722`, `:833` | `plc_count` is incremented only after the handler thread starts, but `modbus_plc_destructor()` decrements it for every PLC, including one whose create failed earlier | The count goes negative, so `mb_teardown()`'s wait for `plc_count > 0` ends while PLCs are still alive.  Deleted with the count by the U7 fix. |

### utils and platform

| # | Where | Defect | Effect |
| --- | --- | --- | --- |
| U1 | `utils/atomic_utils.c:46` onward, the non-C11 branch | POSIX: compare-exchange uses the *weak* form and returns `expected`, so a spurious failure reads as success.  POSIX and Windows: `get`/`set` for bool, int32 and int64 are plain loads and stores, and `set` is a read followed by a write, not an exchange. | Lost updates and false CAS success on ARM.  Torn int64 reads and writes on 32-bit targets, Windows x86 included. |
| U2 | `utils/rc.c:208` | When queueing a cleanup fails, `refcount_cleanup()` runs while `cleanup_mutex` is held | A destructor that blocks (see P6) blocks every `rc_dec()` that reaches zero on any thread. |
| U3 | `platform/platform_common.c:71` `mem_realloc()`, and callers such as `standard_tag.c:702` | Callers write `tag->data = mem_realloc(tag->data, n)`.  `mem_realloc(p, 0)` returns NULL without freeing. | On failure the old buffer leaks and `tag->data` is NULL while `tag->size` is already the new size.  `plc_tag_set_size(id, 0)` fails with `PLCTAG_ERR_NO_MEM`. |
| U4 | `lib/tag_registry.c` `add_tag_lookup()` | `publish_tag()` can loop up to `PLCTAG_MAX_TAGS` (262,144) times, and `hashtable_put()` allocates, while `reuse_lock`, a spinlock, is held | Other threads spin for the whole allocation and probe loop. |
| U5 | `platform/posix/platform.h:144` and the Windows equivalent | See section 4 | Silent failure. |
| U6 | `platform/posix/platform.h:144`, `spin_block` | `__sync_flag_nargle_##__LINE__` pastes the literal token `__LINE__`; `##` prevents expansion | Every block uses the same identifier names, so the per-line uniqueness the names intend does not exist; nested blocks shadow each other.  Identifiers starting with `__` are also reserved. |
| U7 | `api/lib.c:893` `plc_tag_shutdown()`, `lib/init.c` `destroy_modules()`, `cip/conn.c:2794` `session_list_teardown()`, `modbus.c:2793` `mb_teardown()` | Shutdown has no explicit wait for deferred destructors; `refcount_drain()` is never called.  It waits by proxy: Modbus waits for `plc_count == 0`, CIP for its connection vector to empty, then each for its handler count, every wait capped at 5 s.  On timeout each module goes on and frees its list, mutex and condition variable, while connection destructors are still queued on the cleanup thread and handler threads may still be running.  `refcount_teardown()` then joins the cleanup thread with no timeout, so those destructors run *after* their module's state is gone. | Late `session_destroy()` / `modbus_plc_destructor()` lock a destroyed or NULL mutex (silently skipped, U5) or use freed list state.  The timeout is reachable: destructors run one at a time on the single cleanup thread, each doing a Forward Close of up to about 250 ms (P6), so about 20 slow connections exceed 5 s.  If `plc_tag_shutdown()` returns while a handler thread still runs, a LabVIEW unload removes code that thread is executing. |
| U8 | `utils/rc.c:200` `rc_dec()` vs `refcount_teardown()` | `rc_dec()` reads `cleanup_thread_running`, `cleanup_mutex` and `cleanup_queue` outside any lock, then locks.  Teardown can stop the thread, drain, `vector_destroy()` the queue and `mutex_destroy()` the mutex in between. | An API call that drops the last reference to a tag during shutdown locks a freed mutex or inserts into a freed vector, or, if it reads the NULL, skips the body and the object is never freed. |
| U9 | `utils/rc.c:266` cleanup thread loop, used by `refcount_drain()` | `cleanup_processing` is set after the entry leaves the queue, outside the lock | `refcount_drain()` can see an empty queue and `cleanup_processing == 0` while an entry is in hand, and return early.  Moot if `refcount_drain()` is deleted. |

**Decided (U7, U8):** make the order do the waiting, with no timeouts that go on regardless.

1. `tag_registry_destroy_all_tags()`, as today.
2. Each module signals terminate to every connection and returns.  No waiting.
3. `refcount_teardown()`: the cleanup thread drains everything, including connection
   destructors, each of which (per P6) joins a handler that sends its own Forward Close and
   exits.  This is the one wait.  It is unbounded but finite: each handler is bounded by its
   socket and close timeouts.  A timeout that carries on regardless is the unsafe choice under
   library unload.
4. Each module frees its list and locks; nothing can reference them now.
5. `tag_registry_teardown()`.

`refcount_drain()` and every module's count and busy-wait loop are then deleted.  For U8,
`rc_dec()` checks `cleanup_thread_running` under `cleanup_mutex`, teardown clears it under
the same lock, and `cleanup_mutex` is a static, statically initialized lock that teardown
does not free, so a late `rc_dec()` always finds it valid and runs the destructor inline.
Since the library is unloaded in-process, the lock must own no heap memory or OS resource:
`PTHREAD_MUTEX_INITIALIZER` on POSIX, `SRWLOCK_INIT` on Windows.  Neither is recursive; this
lock does not need to be.

**Decided (U1):** every non-Windows compiler supports C11.  Delete the POSIX half of the
non-C11 branch and use `<stdatomic.h>`.  The Windows half uses Interlocked calls for every
operation: `InterlockedExchange`/`InterlockedExchange64` for `set`, and
`InterlockedCompareExchange`/`InterlockedCompareExchange64` with equal arguments for `get`.

---

## 4. `critical_block` failure semantics

```c
#define critical_block(lock)                                                  \
    for(int __sync_flag_nargle_##__LINE__ = 1; __sync_flag_nargle_##__LINE__; \
        __sync_flag_nargle_##__LINE__ = 0, mutex_unlock(lock))                \
        for(int __sync_rc_nargle_##__LINE__ = mutex_lock(lock);               \
            __sync_rc_nargle_##__LINE__ == PLCTAG_STATUS_OK && __sync_flag_nargle_##__LINE__; __sync_flag_nargle_##__LINE__ = 0)
```

If `mutex_lock()` fails, the body is skipped and execution continues after the block as if it
had run.  It also calls `mutex_unlock()` on a mutex it never locked.  Most callers initialize
`rc = PLCTAG_STATUS_OK` before the block and set it inside, so a lock failure is reported as
success.  There are 94 call sites.

`mutex_lock_impl()` (`posix/platform.c:121`) fails only on a NULL mutex, an uninitialized
one, or a `pthread_mutex_lock()` error, which on a recursive mutex is essentially EAGAIN
(too many recursive locks).  The first two mean the owning object is already dead, and
reading `m->initialized` on a freed mutex is itself a use after free.

### Option A: a sentinel set before the block

```c
int32_t rc = PLCTAG_ERR_MUTEX_LOCK;
critical_block(tag->api_mutex) {
    rc = do_the_work(tag);
}
if(rc == PLCTAG_ERR_MUTEX_LOCK) { /* the lock failed */ }
```

No macro change.  Every call site has to be audited and its initializer changed, and the
body must always assign `rc`, including on every `break` path.  Sites that compute a value
other than `rc` (for example `created = tag->created`) need their own sentinel.

### Option B: an `if` in the innermost loop, so a trailing `else` handles failure

```c
#define critical_block(lock)                                                                        \
    for(int32_t cb_rc_ = mutex_lock(lock), cb_outer_ = 1; cb_outer_;                                \
        cb_outer_ = 0, (cb_rc_ == PLCTAG_STATUS_OK ? (void)mutex_unlock(lock) : (void)0))           \
        for(int32_t cb_inner_ = 1; cb_inner_; cb_inner_ = 0)                                        \
            if(cb_rc_ == PLCTAG_STATUS_OK)
```

Use:

```c
critical_block(tag->api_mutex) {
    rc = do_the_work(tag);
} else {
    rc = PLCTAG_ERR_MUTEX_LOCK;
}
```

- Unlocks only what it locked.
- `break` inside the body leaves the inner loop, and the outer loop's increment still
  unlocks, as today.
- The `else` is optional, so existing call sites compile unchanged.  That also means every
  site without an `else` keeps the silent skip until it is converted.
- Hazard: dangling else.  `if(x) critical_block(m) { ... } else { ... }` binds the `else` to
  the macro's `if`, not to `if(x)`.  Braces around the outer `if` body avoid it; the coding
  guidelines already require braces.
- The fixed names `cb_rc_`, `cb_outer_`, `cb_inner_` shadow in nested blocks.  That is
  correct C (each loop sees its own), but `-Wshadow` will warn.  Generating per-line names
  needs a two-level paste macro, since `##__LINE__` does not expand (U6).
- `spin_block` has the same shape and the same issue; `lock_acquire()` cannot fail today,
  but it should get the same form for consistency.

### Option C (open): make the lock unable to fail

The `platform.h` mutex is a thin heap-allocated wrapper around the OS lock plus an
`initialized` flag.  Drop the wrapper and embed the OS lock by value in its owner, which the
static-pool goal moves toward anyway.  A lock can then be invalid only if its owner is
already freed, which is a lifetime bug that no return code can make safe.  `mutex_lock()`
returns `void` and logs ERROR on an internal failure; neither the sentinel nor the `else` is
needed.

Sizes of what would be embedded:

| Platform | Type | Size |
| --- | --- | --- |
| macOS arm64 (measured) | `pthread_mutex_t` | 64 bytes, 8-byte aligned |
| Linux glibc, 64-bit | `pthread_mutex_t` | 40 bytes |
| Linux glibc, 32-bit | `pthread_mutex_t` | 24 bytes |
| Windows x64 | `CRITICAL_SECTION` | 40 bytes |
| Windows x86 | `CRITICAL_SECTION` | 24 bytes |

The Windows wrapper today holds a `HANDLE` from `CreateMutex()`: a kernel object, so every
lock and unlock is a system call.  Embedding means switching it to `CRITICAL_SECTION`, which
is recursive like the POSIX mutex and cannot fail to lock on Vista and later.  `SRWLOCK`
(pointer-sized) is not recursive, so it does not fit while the code relies on recursion.

Audit of the 20 `mutex_create`/`mutex_destroy` sites (2026-11-10, plan step 0.1): NULL
locks happen only in destructors of half-built objects and on module locks outside startup to
teardown.  Option C removes both if objects initialize their lock first and module locks are
static.  Details in `ephemeral_docs/release_2_7_3_plan.md`, "0.1 result".

All options also need a rule: no `return` or `goto` inside a `critical_block`, since either
skips the unlock.

---

## 5. CIP cleanup

- `conn.c:1296`: `process_requests()` re-validates the Multiple Service Packet reply (about
  130 lines) that `find_reply()` and `validate_response_cpf()` in `cip.c` validate again.
  Delete the `conn.c` copy.
- The Identity request is encoded twice: `cip.c:853` `cip_encode_identity_request()` and
  `identity_tag.c:79` `cip_identity_tag_encode_read()`.
- Duplicate constants: `CIP_ERR_UNSUPPORTED_SERVICE` / `CIP_STATUS_UNSUPPORTED_SERVICE`,
  `CIP_ERR_PARTIAL_ERROR` / `CIP_STATUS_PARTIAL_ERROR`, `CIP_SVC_MULTI` /
  `CIP_EIP_CMD_CIP_MULTI`.  `MAX_CONN_PATH` and `MAX_IP_ADDR_SEG_LEN` are defined in both
  `conn.h` and `path.c`.  The Connection Manager path bytes `0x20,0x06,0x24,0x01` are written
  out six or more times.
- `path.c:570`: a 256-entry table (about 260 lines) for what is three ranges plus a few
  singles.  A `switch` of a few lines does it.
- `path.c` `match_ip_addr_segment()`: four copy-pasted octet blocks; one loop.
- `conn.c:926` `session_unregister()` is a no-op with its own state, `SESSION_UNREGISTER`.
- `conn.c` `session_list_remove_unsafe()` carries a "FIXME potential ABA problem".
- `cip_encode_forward_open_ex()` and `cip_encode_forward_open_old()` are near-duplicates.
  Acceptable under the direct-code preference; listed only for awareness.
- **Decided:** `error_codes.c` drops the long descriptions copied from the "Productivity
  Suite CPU" manual.  The short descriptions stay.

---

## 6. AB and OMRON cleanup (later)

Type-branch removals are in section 1.  The rest:

- The three Rockwell fragment decoders (`listing_tag.c`, two in `udt_tag.c`) and the standard
  tag decoder share one shape; use the fragment accumulator from section 2.
- `cip_tag_t` carries every kind's private state: PCCC `file_type` and `req_pccc_seq_num`,
  listing `next_id`, UDT `udt_id` and `udt_get_fields`, OMRON `size_probe`,
  `deferred_attribs`.  `omron_listing.c:70` reuses `udt_get_fields` as a listing-phase enum.
  Give each kind its own state.
- Dead code:
  - `pccc.c:399` `CRC16Bytes`, unused.
  - `ab/ab_common.c:83` and the copy in `omron_common.c`: `DEFAULT_NUM_RETRIES`, `DEFAULT_RETRY_INTERVAL`, unused.
  - `logix_tag_listing_byte_order` (`ab_common.c`), unused; `listing_tag.c` has its own copy.
  - Commented-out globals at the top of `ab_common.c` and `omron_common.c`.
  - Orphaned doc blocks for functions that no longer exist (`cip_tag_status`, `cip_tag_abort`, "Check the status of the request") in both.
- `omron_common.c:82` is headed "Generic Rockwell/Allen-Bradley protocol functions", copied
  from `ab_common.c`.
- `pccc.c:949` `sub_element_lookup` is a non-static global with `// NOLINT`.
- `pccc.c` `parse_pccc_file_type()` is a large `switch` carrying its own "FIXME refactor into
  a table".  The digit-parsing loop is repeated four times ("FIXME - we do this a lot").
- `pccc.h` uses a header guard and `//` comments; the guidelines require `#pragma once` and
  `/* */`.  It also documents a function ("Attach a submitted request...") that no longer
  exists.
- `eip_plc5_pccc.c` and `eip_slc_pccc.c` each contain one byte-order struct.
  `eip_slc_pccc.c` and `eip_lgx_pccc.h` are commented "PLC-5".

---

## 7. Modbus cleanup

- **Decided:** "send the next piece" is `PLCTAG_STATUS_PENDING` everywhere.  Modbus stops
  returning `PLCTAG_ERR_PARTIAL` for it.
- Modbus builds requests straight into `plc->write_data`.  Under section 1 its codecs live in
  the Modbus module and use `byte_slice` (section 8).
- The disconnect deadline is recomputed in four places in `modbus_plc_handler()` and the
  send/receive functions; use the idle tracker from section 2.
- Two teardown waits: a condition variable on `plc_count`, then a busy-wait on
  `handler_threads_active`.  Replaced by the P6 teardown form.
- Dead: the `PLC_BUILD_REQUEST` state, `modbus_tag_list_t` (declared, never defined),
  `PLC_READ_DATA_LEN`/`PLC_WRITE_DATA_LEN` beside the spec maxima, the commented-out
  `cond_wait` and `request_in_flight` field, the orphaned "Microsecond timing" comment.
- Typo: `modbug_cmd_t` (line 194).
- `elem_size` in bits on the core struct (section 1).

---

## 8. utils and platform cleanup

- **Decided:** every CIP and Modbus encoder and decoder moves to the `byte_slice` APIs and
  structures (`utils/byte_slice.h`, prototyped in `ab_server`).  This is the fix for the
  wire-length class of defects (R1, R4, P2, P5) and replaces the hand-rolled `(intptr_t)`
  pointer comparisons.
- **Decided:** delete every function the library does not use, and any test that exists only
  to test it.  Not deleted yet.  Uses outside `src/libplctag`, `src/utils` and
  `src/platform` (whole repository, `git grep -w`):

  | Function | Uses outside the core library |
  | --- | --- |
  | `vector_swap_element()` | none |
  | `refcount_drain()` | none, and no caller anywhere; its comment saying shutdown uses it is stale.  Shutdown waits by proxy instead (U7).  Deleted by the U7 fix. |
  | `thread_kill()`, `thread_stop()` | none |
  | `thread_detach()` | none; `tools/ab_server` has its own `thread_detach()` in `ab_server/thread.c` |
  | `time_us()` | none |
  | `str_concat_impl()` and its `str_concat` macro | none |
  | `mem_cmp()` | none |
  | `tag_schedule_move()` | `tests/unit/test_tag_schedule.c` only |
  | `tag_schedule_next_period()` | `tests/unit/test_tag_schedule.c` only |
  | `cip_conn_live_count()` | `tests/unit/test_conn_leak.c` only |
  | `str_to_float()` | `tests/unit/test_platform_str.c` only |

  The last four have tests that use them.  For each, delete the test cases that exist only to
  test it; `test_conn_leak.c` uses `cip_conn_live_count()` as its leak oracle, so that test
  needs another oracle or goes with it.
- The refcount cleanup queue is a vector drained with `vector_remove(queue, 0)`, a memmove per
  item.  A ring or an index is enough.
- `mem_alloc`, `mem_realloc`, `mem_set`, `mem_copy` and `mem_move` take bare `int` sizes,
  against the sized-types guideline.
- `utils/attr.h` declares `find_entry()` without `extern` or `static`, exporting a generic name.
- `platform/posix/platform.c` and `platform/windows/platform.c` are about 1,850 lines each.
  `socket_wait_event()` alone is about 200 lines in each, and most of it is the same event
  classification around a `poll()` or `select()` call.  The classification can move to
  `platform_common.c`.

---

## 9. Public API: debug module IDs

`PLCTAG_MODULE_*` in `libplctag.h` (172–200) shipped in v2.7.2 and names files and layers
that no longer exist: `AB_SESSION`, `AB_EIP_PLC5_DHP`, `AB_EIP_SLC_DHP`, `OMRON_CONN`,
`OMRON_CIP` and others.

**Decided:** the ABI stays the same.  Every enum value stays defined with its number, so
callers that filter on them still compile and run.  IDs for modules that no longer exist are
not emitted.  The IDs that are emitted come from the code that owns them: shared CIP code
uses the CIP ID, and each family's `create_tag` sets its own (section 1).

This deletes the structure that exists only to preserve old IDs:

- `TAG_PROTOCOL_AB_CONNECTION` and `TAG_PROTOCOL_OMRON_CONNECTION` as separate protocols.
- `cip_connection_tag_ops_t` and the `is_omron` decision in `cip_tag_create.c`.
- The comment in `standard_tag.c` that the shared engine "must not merge their logging".

New modules do not add IDs per file.

> **DEPRECATED.** This document predates the current EtherNet/IP design and is
> retained for historical reference only. The authoritative design is
> [ENIP-SESSION-DESIGN.md](../src/libplctag/protocols/enip/ENIP-SESSION-DESIGN.md). Do not use this document to guide new work.

# ENIP Forward Plan

**Date:** 2026-06-02
**Status:** Proposed
**Audience:** implementer (assumes familiarity with C, not with CIP)

**Prerequisite reading — the `Bytes`/`Arena` helpers.** All packet building and
parsing in this plan uses two small utilities; skim their headers before starting:

- `src/utils/bytes.h` — a length+pointer `Bytes` view with type-safe pack/unpack.
  Key calls: `bytes_alloc(arena, len)`, `bytes_pack(arena, endian, …)` /
  `bytes_pack_into(buf, …)`, `bytes_unpack(data, endian, &a, &b, …)` (returns the
  remaining `Bytes`), `bytes_slice(b, off, len)`, `bytes_skip(b, n)`,
  `bytes_is_null(b)`, `bytes_concat(...)`, `bytes_hexdump(b, label)`. Endian is
  `BYTES_LE` for everything on the wire here.
- `src/utils/arena.h` — bump allocator. `arena_init(&a, size)`,
  `arena_alloc(a, size)` (NULL on overflow), `arena_save`/`arena_restore`,
  `arena_reset(a)` (the only "free"; called once per request cycle).

**Line numbers in this document are approximate** — code shifts. When a citation
reads `enip_conn.c:428`, treat it as "find the named function/symbol nearby," not
a literal line.

This plan reorganizes the ENIP client around clean protocol layers — EIP, CPF,
CIP, the 0x0A multi-service wrapper, and a per-manufacturer fragmentation
strategy — sitting on top of a Modbus-style connection/tag-queue engine. The
`src/poc/ab_server_fiber` device simulator implements the *server* side of these
same layers; it is the reference for every packet layout below and the
end-to-end test target for each phase.

## 0. Coding standards (read first)

All new and changed code follows `src/external_docs/coding_guidelines.md`. The
points that shape this plan's interfaces:

- Status-returning functions return `int32_t` using `PLCTAG_STATUS_OK` /
  `PLCTAG_STATUS_PENDING` / `PLCTAG_ERR_*` (and `PLCTAG_ERR_PARTIAL` for
  "more data needed"). The guideline allows a `bool`-plus-out-param form for
  chainable accessors; for an operation whose only result *is* a status (such as
  `accept_chunk`), fold the signal into the `int32_t` return rather than adding a
  separate out-param — the return code already carries the meaning and it matches
  how the rest of the codebase reports completion.
- Explicitly sized integers only (`uint8_t`, `int32_t`, `size_t`, …); never bare
  `int`/`short`. `bool` for flags. `char` only for strings.
- `/* */` comments only. `#pragma once` in headers. `static` for internal,
  `extern` (in the `.h`) for public. `mem_alloc`/`mem_free`, not malloc/free.
- ENIP debug output uses `DEBUG_MODULE_ENIP` via `pdebug()` — never
  `DEBUG_MODULE_LIB` in ENIP files (several current files violate this).
- File section order per the guideline; one blank line at end of file.
- "Duplication requiring the same fix in multiple places is a code smell" — the
  driver for the consolidation in §2.

## 1. Target architecture

Layers, bottom to top. Each is a file pair with no knowledge of the layers above it.

| Layer | Module | Responsibility |
| --- | --- | --- |
| L0 Transport | `enip_conn.c` (reader) | stream framing: read 24-byte header, then exactly `length` more bytes |
| L1 EIP | `enip_eip.[ch]` | 24-byte encapsulation header; RegisterSession/UnregisterSession |
| L2 CPF | `enip_cpf.[ch]` | unconnected (NAI+UDI) and connected (CAI+CDI+seq) item framing |
| L3 CIP | `enip_cip.[ch]` | path encode; ReadTag/WriteTag/0x52/0x53; ForwardOpen/Close; Unconnected_Send; response parse |
| L3a Multi | `enip_multi.[ch]` *(new)* | 0x0A service: pack N sub-requests / unpack N sub-responses |
| L4 Strategy | `enip_mfg_*.c` via `enip_mfg_ops.h` | manufacturer fragmentation + service selection (AB byte-offset, OMRON data-segment, PCCC 0x4B) |
| L5 Engine | `enip_conn.c` (state machine) + `enip_tag.c` | connection lookup, tag queue, correlation, reconnect, metadata |

**Rule (already stated in `enip_mfg_ops.h`, must be enforced):** shared code does
zero branching on manufacturer. All vendor differences live behind `mfg_ops`.

## 1.5 Public tag API, control flow, and the pseudo-blocking I/O model

This section is the orientation a new implementer needs before reading the
per-packet detail in §3. It answers: how does a caller drive a tag, what do the
five vtable entry points actually do, and where does the protocol work happen?

### 1.5.1 Pseudo-blocking I/O — use the wait wrappers, never raw sockets

All ENIP socket I/O goes through the **wait wrappers in `src/utils/enip_wait.h`**,
not the raw `platform.h` calls. These are already written and tested; use them as
the only socket interface from the engine:

```c
/* All return PLCTAG_STATUS_OK on full completion, PLCTAG_ERR_TIMEOUT on
 * deadline, PLCTAG_ERR_* on socket failure / remote close. */
int socket_connect_wait(sock_p s, const char *host, int port, int timeout_ms, socket_wait_state_t *io_state);
int socket_read_wait (sock_p s,       Bytes *dst, int timeout_ms, socket_wait_state_t *io_state);
int socket_write_wait(sock_p s, const Bytes *src, int timeout_ms, socket_wait_state_t *io_state);
```

What "pseudo-blocking" means here: each wrapper loops internally on
`socket_wait_event()` until the operation completes or the `timeout_ms` deadline
passes. `socket_read_wait` fills the **entire** `dst` buffer (`dst->len` bytes);
`socket_write_wait` drains the entire `src`. That is exactly the stream-framing
primitive §3 A asks for — read a 24-byte header buffer, parse `length`, then read
a `length`-sized buffer; each call returns only when that many bytes have arrived.
The caller writes straight-line code (send, then read header, then read body) and
the wrapper hides the readiness polling.

The `socket_wait_state_t *io_state` arg makes a wrapper **restartable**: if a call
returns `PLCTAG_ERR_TIMEOUT` partway through a buffer, `io_state` records how many
bytes are done and the original deadline, so calling again with the same
`Bytes` + `io_state` resumes rather than restarting. The handler thread keeps one
`socket_wait_state_t` per in-flight operation. Pass `timeout_ms` large enough to
cover one request/response (the existing code uses `5000`; §6 says name it).

Confirmed present: `src/utils/enip_wait.c` implements all three —
`socket_read_wait`, `socket_write_wait`, and `socket_connect_wait` (which starts
the TCP connect and blocks until it completes, with the polling loop in a static
`connect_poll_wait` helper) — and they already wrap `socket_wait_event`. No new
socket plumbing is required. (One guideline cleanup:
`enip_wait.h` still uses an `#ifndef` guard and lacks the copyright block — convert
to `#pragma once` per §0 when touched.)

### 1.5.2 How a caller creates and drives a tag

Callers use the public `plc_tag_*` API; the library dispatches by the `plc=`/
protocol attribute to `enip_protocol_tag_create` (the ENIP entry point) and
thereafter to the vtable. A complete read looks like this from the caller side:

```c
/* 1. Create. Returns a tag id > 0, or a negative PLCTAG_ERR_* code. The string
 *    carries gateway/path/plc/name/elem attributes (see §7 for real ones). */
int32_t tag = plc_tag_create(
    "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&name=MyDINT",
    0 /* create timeout 0 => return immediately, status PENDING */);
if(tag < 0) { /* handle create error */ }

/* 2. Wait until metadata + connection are ready (status leaves PENDING). */
while(plc_tag_status(tag) == PLCTAG_STATUS_PENDING) { util_sleep_ms(1); }

/* 3. Request a read. Non-blocking: returns PLCTAG_STATUS_PENDING immediately. */
int32_t rc = plc_tag_read(tag, 0 /* async */);

/* 4. Poll status until the engine completes the operation. */
while(plc_tag_status(tag) == PLCTAG_STATUS_PENDING) { util_sleep_ms(1); }
if(plc_tag_status(tag) != PLCTAG_STATUS_OK) { /* handle */ }

/* 5. Read decoded data out of the tag's buffer. */
int32_t value = plc_tag_get_int32(tag, 0);

plc_tag_destroy(tag);
```

`enip_protocol_tag_create` (today `enip_tag.c:142`) does only synchronous,
cheap setup: encode the symbolic path once into storage allocated contiguously
with the tag, extract the root name, install `enip_tag_vtable`, set
`protocol_type = TAG_PROTOCOL_ENIP` and `metadata_required`, call
`plc_tag_generic_init_tag`, raise `PLCTAG_EVENT_CREATED`. It does **not** touch the
network. In the Phase-6 engine it additionally calls `find_or_create_connection`
and inserts the tag into `conn->active_tags` so the handler thread can find it.

### 1.5.3 What the five vtable entry points do

These run on the **caller's** thread, under the tag's API mutex. They are
intentionally tiny: they mutate the tag's `op_state`/flags and return immediately.
All real protocol work happens later on the connection's handler thread (§1.5.4).
Per §0 they should be typed `int32_t` and log under `DEBUG_MODULE_ENIP` (current
code uses bare `int` and `DEBUG_MODULE_LIB` — fix when rewritten).

| Entry point | Sets | Returns | Meaning |
| --- | --- | --- | --- |
| `read` | `op_state = ENIP_TAG_OP_REQUEST`, `read_in_flight = 1` | `PLCTAG_STATUS_PENDING` | "queue a read"; engine will encode + send |
| `write` | `op_state = ENIP_TAG_OP_REQUEST`, `write_in_flight = 1` | `PLCTAG_STATUS_PENDING` | "queue a write"; tag data buffer already holds the value to send |
| `status` | — (reads only) | `OK` when idle, `PENDING` while `read/write_in_flight`, else stored `PLCTAG_ERR_*` | lets the caller poll completion |
| `abort` | `op_state = ENIP_TAG_OP_ERROR`, clears both in-flight flags | `PLCTAG_STATUS_OK` | cancel any queued/in-flight op; engine drops it on next tickle |
| `tickler` | — | stored status | per-tag housekeeping hook called from the engine |

`read`/`write` must also update the tag's `op_time` and (re)insert it into
`conn->active_tags` so the handler thread wakes and services it — that wiring is
the Phase-6 work; the current stubs only flip flags.

### 1.5.4 Main event loop outline (per-connection handler thread)

One handler thread per connection, mirroring `modbus_plc_handler`
(`modbus.c:1011`) but written in the **pseudo-blocking** style the wait wrappers
enable: instead of Modbus's fine-grained non-blocking state machine, each
request/response is straight-line code, because `socket_*_wait` blocks (up to a
deadline) inside the thread without stalling caller threads.

The struct is the existing `enip_connection_t` (`enip_conn.h:70`). The outline below
uses these **fields that already exist**: `socket`, `active_tags`,
`active_tags_mutex`, `tx_arena`/`rx_arena`, `host`/`port`, `mfg_ops`,
`shutdown_requested`, `last_message_time_ms`. It also uses four **fields Phase 6
must ADD** (none exist yet) — marked `/* NEW */` below:

- a connection-state enum `state` (e.g. `ENIP_CONN_DISCONNECTED`/`OPENING`/`READY`);
- the negotiated per-message CIP budget `cip_budget` (derived from the ForwardOpen
  size, §3 D);
- a per-thread `socket_wait_state_t io` for the wait wrappers' restart state;
- a wakeup condvar `wake` (plus its mutex) so `read`/`write` can signal the thread.

Outline:

```c
static THREAD_FUNC(enip_conn_handler) {
    enip_connection_t *conn = (enip_connection_t *)arg;

    while(!conn->shutdown_requested && lib_active) {
        /* 1. Ensure the transport+session is up (lazy: only if work is queued). */
        if(conn->state != ENIP_CONN_READY) {              /* state: NEW */
            if(no_tags_waiting(conn)) { cond_wait(conn->wake, IDLE_WAIT_MS); continue; } /* wake: NEW */
            rc = enip_conn_open(conn);          /* socket_connect_wait + RegisterSession + */
                                                /* (Phase 3) ForwardOpen; on fail -> backoff */
            if(rc != PLCTAG_STATUS_OK) { enip_conn_backoff(conn); continue; }
            enip_metadata_reload_if_needed(conn);   /* Phase 7: root-symbol inventory */
        }

        /* 2. Pick the next due tag (front of active_tags, sorted by op_time). */
        enip_tag_t *tag = next_due_tag(conn);   /* holds conn->active_tags_mutex briefly */
        if(!tag) {
            /* nothing due: sleep until the soonest op_time, or idle-disconnect. */
            if(idle_timeout_expired(conn)) { enip_conn_close(conn); }
            cond_wait(conn->wake, wait_until_next_due(conn));   /* wake: NEW */
            continue;
        }

        /* 3. Drive one tag operation as a straight-line blocking exchange. */
        switch(tag->op_state) {
            case ENIP_TAG_OP_METADATA_PHASE1:   /* lazy: resolve instance id   */
            case ENIP_TAG_OP_METADATA_PHASE2:   /* lazy: resolve type/dims      */
            case ENIP_TAG_OP_REQUEST: {
                arena_reset(&conn->tx_arena);
                Bytes req = mfg_ops->encode_chunk(tag, &conn->tx_arena, conn->cip_budget); /* cip_budget: NEW */
                /* build EIP/CPF framing around req, then: */
                rc = socket_write_wait(conn->socket, &frame, timeout_ms, &conn->io);       /* io: NEW */
                if(rc == PLCTAG_STATUS_OK) { rc = enip_recv_frame(conn, &resp); }
                if(rc != PLCTAG_STATUS_OK) { fail_tag(conn, tag, rc); break; }

                rc = mfg_ops->accept_chunk(tag, resp_cip);
                if(rc == PLCTAG_ERR_PARTIAL) { /* keep op_state REQUEST, re-queue */ }
                else { finish_tag(conn, tag, rc); }   /* OK or error: wake caller */
                break;
            }
            default: break;
        }

        /* 4. Note activity for the idle-disconnect clock. */
        conn->last_message_time_ms = time_ms();
    }

    enip_conn_close(conn);
    THREAD_RETURN(0);
}
```

Key properties: (a) **lazy connect** — the thread does not open the socket until a
tag is queued, and after an idle timeout it closes and waits for the next request
(§5 Phase 6). (b) `enip_recv_frame` is the §3 A stream-framer built on two
`socket_read_wait` calls (header, then body). (c) The fragmentation loop is the
`encode_chunk`/`accept_chunk` cycle from §4 — the handler repeats step 3 for a tag
while `accept_chunk` returns `PLCTAG_ERR_PARTIAL`. (d) Caller threads never block on
the socket; they block (if at all) only in their own `plc_tag_status` poll loop,
and the handler signals completion via the tag's status + event callback.

## 2. Salvage / rewrite / remove

### Keep (correct or nearly so)

- `enip_eip.[ch]` — header pack/parse, `enip_eip_build_request`. Correct.
- `enip_cpf.[ch]` — unconnected + connected build, `find_item` scanner, UDI/CDI extract. Correct; matches simulator.
- `enip_cip.[ch]` — path encoding (`enip_cip_encode_tag_path`), ReadTag/WriteTag/0x52/0x53 builders, `enip_cip_parse_response`. Correct as far as it goes (see §3 G for the missing type-code strip).
- `enip_packetizer.[ch]` — budget math for single and 0x0A frames. Correct; currently unused — wire it in Phase 5.
- `enip_metadata.c` — `enip_metadata_fetch_root_symbols` (Phase-1) and `enip_metadata_fetch_tag_info` (Phase-2) are real, blocking, and broadly correct. Keep; see §3 L for filtering note.
- `enip_name.c` — root extraction + route encoding. Keep; route encoding is currently never called (Phase 3 wires it).

### Rewrite

- `enip_conn.c` — the main loop. Remove all hand-rolled EIP/CPF framing in
  `register_session`, `get_identity`, `build_requests` and route through L1/L2/L3.
  Replace the fixed-buffer read with stream framing (§3 A). Implement real
  sender-context correlation. Add connection lookup + thread launch + host/port
  plumbing (currently hardcoded `192.168.1.100:44818`).
- `enip_tag.c` / `tag.h` — add a connection pointer, byte-order, data buffer
  init, and a generic chunk cursor; bind tags into the connection's queue.
- `enip_mfg_ab.c` — keep the encode/decode skeleton but (a) replace its stub
  `fetch_phase1_metadata` (which hardcodes `tag_count=1`) with a call to the real
  `enip_metadata_fetch_root_symbols`; (b) implement fragmentation continuation.

### Remove / replace wholesale

- `enip_mfg_omron.c`, `enip_mfg_pccc.c` — **do not compile today** (call
  `enip_cip_read_tag_request`/`write` with a stale extra `sequence_id` argument;
  OMRON passes a `char*` where `uint32_t` is required). Delete and rewrite against
  the current `enip_cip.h` and the new strategy interface.
- The duplicated framing blocks in `enip_conn.c` listed above.
- The unused `cip_payloads` vector in `build_requests` (allocated, never used).
- The phantom `sender_context_base` member: it is **used but never declared**, so
  the tree does not compile (this is the current build break). Keep the one real
  counter `conn->sender_context` (`uint64_t`, declared at `enip_conn.h:77`) — see
  Phase 0 for the exact edit.

## 3. Packet-definition gaps and parsing bugs (detailed)

Byte offsets are little-endian. "hdr(4)" = `reply_service|0x80, reserved=0, status, ext_status_size`.

**A. Transport framing is wrong.** The loop does one `bytes_alloc(max)` +
`socket_read_wait`. EIP over TCP is a byte stream; a read may return a partial
packet or several. Correct algorithm: read exactly 24 bytes → parse `length`
(offset 2) → read exactly `length` more bytes → that is one EIP frame. Loop on
short reads. Apply everywhere (bootstrap path and main loop).

**B. GetIdentity CIP header off-by-one** (`enip_conn.c:428`). It unpacks 3 bytes
(`cip_reserved, cip_status, ext_status_size`); the CIP reply header is 4 bytes.
Every field after is shifted. Fix by calling `enip_cip_parse_response`, which is
already correct.

**C. Identity attribute layout** (after the 4-byte CIP header). GetAttributeAll
on Identity (class 0x01, inst 0x01) returns: `vendor_id(2) device_type(2)
product_code(2) rev_major(1) rev_minor(1) status(2) serial(4)
name(SHORT_STRING: len(1)+chars)`. The library parses this correctly *once B is
fixed*. **But** `supports_extended_forward_open = (status & 0x0080)` is bogus —
the Identity status word does not advertise Forward_Open_Ex. Remove the guess;
detect capability by attempting FO_Ex (0x5B) and falling back to FO (0x54) on
error (§3 D). The two reference ControlLogix units in §7 differ exactly here.

**D. ForwardOpen request — gaps.**

- Only standard FO (0x54) is built, with hardcoded conn params `0x43F8` (504
  bytes). Add Extended FO (0x5B): its O→T and T→O *network connection parameters*
  are **4 bytes** each (vs 2 for 0x54), and the connection-size field is the low
  **12 bits** (mask `0x0FFF`) vs low **9 bits** (mask `0x01FF`) for 0x54. Logix
  needs 0x5B to negotiate the 4002-byte connected buffer.
- Connection path is hardcoded to Message Router `{0x20,0x02,0x24,0x01}` with an
  optional `{0x01,slot}` prefix. Real routing (EN2T→backplane→CPU) must use the
  user `path`/route attribute. `enip_name_encode_route` already produces the
  segment bytes but is never called. Plumb route attribute → `conn->conn_path` →
  FO connection path.
- After FO, `max_packet_buffer_size` is set to the literal `504` regardless of
  what the device returned. Parse the actual returned size and derive the CIP
  budget. Mirror `eip_session_set_connected_sizes()` in the simulator
  (`poc/.../eip.c:161`): `max_cip = raw - (CDI_header 4 + seq 2)`.

**E. ForwardOpen response field order** (after hdr(4)): `O→T_conn_id(4)
T→O_conn_id(4) conn_serial(2) orig_vendor(2) orig_serial(4) O→T_API(4)
T→O_API(4) app_reply_size(1) reserved(1) [app_reply]`. For subsequent connected
sends, the **Connected Address Item carries the O→T connection id** (first
field) — confirmed by the simulator check at `poc/.../cpf.c:161`. Store that one
as the id to send; the library currently mislabels the two and never uses them.

**F. Connected send (SendUnitData 0x0070) unimplemented in the client.**
`enip_cpf_build_connected` exists and is correct (CAI 0x00A1/len4/conn_id; CDI
0x00B1/len=`2+payload`; seq(2); CIP). Nothing calls it. The connected path must:
increment `cip_conn_seq_num`, build via `enip_cpf_build_connected`, and on
response validate the returned seq. Connected replies strip via
`enip_cpf_extract_cdi_payload` (also already correct).

**G. Read reply contains a 2-byte (or 4-byte) type code that must be stripped.**
ReadTag reply = `hdr(4) + type_code(2) + data` (simulator `handle_read` packs
`tag_type` right after the header). `enip_cip_parse_response` returns *everything
after hdr+ext-status* as `data`, so `data` still begins with the type code.
`enip_mfg_ab_decode_response` copies `data` straight into `tag->data`, corrupting
the first elements. Fix: after `parse_response`, read the type code, store it in
`tag->data_type`, then copy the remainder. Note the structured-type case: when
the first type byte is `0xA0` (struct), the type is **4 bytes**
(`0xA0 0x02 <struct_handle:2>`); atomic types are 2 bytes. Handle both.

**H. AB fragmentation continuation is missing.** CIP status `0x06` = "partial
data, more available". `decode_response` always reports complete and never
advances `byte_offset`. AB method: on `0x06`, append the received data, advance
`byte_offset` by bytes received, re-issue service 0x52 (read) / 0x53 (write) with
the new offset, repeat until status `0x00`. `tag->byte_offset` already exists.

**I. OMRON fragmentation uses a trailing data segment, not 0x52/0x53.** OMRON
NJ/NX use the standard 0x4C/0x4D services and append a CIP **data segment** to
the request that carries the total element count and a byte offset into the tag.
Wire layout of the appended segment (8 bytes in this example):

```text
0x80         data segment type
0x03         length of the remaining segment data in 16-bit words (3 words = 6 bytes)
0x0005       total number of elements (uint16)   -- constant across all chunks
0x00000010   byte offset into the tag (uint32)   -- advances each chunk
```

So a fragmented OMRON read is `service 0x4C + path(symbolic) + element_count(2) +
{0x80, len_words, total_elem_count(2), byte_offset(4)}`; each chunk re-emits the
segment with `byte_offset` advanced by the bytes already received. The segment
data length is `(2 + 4) / 2 = 3` words; `len_words` counts only the bytes after
itself. OMRON connected buffer is ~1996 bytes. This different mechanism is the
reason fragmentation must be a strategy (§4), not shared code. Confirm exact
encoding against the vendored `src/external_docs/aphytcomm` Python library.

**J. Multi-service (0x0A) not implemented on the client.** `build_requests` has
a `TODO` and instead emits one malformed aggregate. Layout (both directions,
offsets measured from the start of the count word): request = `count(2) +
offset_table(2×N) + concatenated sub-requests`; response = `hdr(4) + count(2) +
offset_table(2×N) + concatenated sub-responses`. Reference encode/decode:
simulator `handle_multi` (`poc/.../cip.c:870`). Budget already computed by
`enip_packetizer_plan`.

**K. Unconnected_Send routing wrapper missing.** Unconnected reads to a CPU
behind a bridge must wrap the real ReadTag in an Unconnected_Send: service `0x52`
to the Connection Manager (`path = 0x20 0x06 0x24 0x01`), body = `priority_tick(1)
timeout_ticks(1) embedded_msg_len(2) <embedded CIP msg> [pad if len odd]
route_path_size_words(1) reserved(1) <route_path>`. The simulator *unwraps* this
at `poc/.../cip.c:148`. Without it, unconnected access only works to a device
with no routing (and `enip_name_encode_route` stays dead).

**L. Root-symbol list filtering** (`enip_metadata.c`). The parse is correct, but
real Logix returns system/internal symbols. Filter entries whose name starts with
`__` or contains `:`, and treat `symbol_type & 0x1000` as a system tag to skip.
The high bit `0x8000` of `symbol_type` marks a structure (vs atomic); record it.

**M. PCCC client encode is entirely wrong.** `enip_mfg_pccc.c` calls the standard
ReadTag/WriteTag with a CIP symbolic path. PCCC must use service `0x4B` (Execute
PCCC) with: requestor id (`vendor_id(2) serial(4)`) then PCCC packet `cmd(1)=0x0f
sts(1)=0x00 tns(2) fnc(1) <addr+size>`. The CIP symbolic path stored on the tag
is meaningless here; the address is a PCCC logical address (file type/number/
element). Decode/encode reference: simulator `pccc.c` (PLC5 `0x01`/`0x00`/`0x26`,
SLC `0xa2`/`0xaa`/`0xab`); element size by file-type letter is the only salvageable
piece of the current code.

**N. Two context schemes + unused correlation.**
`enip_connection_find_pending_request` (`enip_conn.c:124`) exists and already
matches a reply by `tag->transaction_id` (`enip_conn.c:138`), but nothing calls it
on the receive path — `decode_response` instead feeds the same payload to *every*
REQUEST-state tag. Meanwhile the send path increments a second, undeclared
counter, `sender_context_base`. Collapse to the one real counter
`conn->sender_context`: on send, post-increment it, write the value into the EIP
header **and** into `tag->transaction_id`; on receive, extract the header context
via `enip_connection_extract_sender_context` (`enip_conn.c:111`) and route the
reply through `enip_connection_find_pending_request`. The exact Phase 0 mechanics
are below.

## 4. Fragmentation strategy interface (L4)

Add to `enip_mfg_ops_t` (replacing the dead `needs_more` callback). The chunk
consumer returns an `int32_t` status; `PLCTAG_ERR_PARTIAL` means "another chunk
is required", `PLCTAG_STATUS_OK` means done. No out-param `bool` — the return
code carries the meaning, consistent with the existing codebase.

```c
/*
 * Build the next chunk request for this tag given the current cursor and the
 * per-message CIP budget. Returns the CIP request bytes, or bytes_null() when
 * there is nothing left to send (normal end of data).
 *
 * encode_chunk does not fail: the arena is sized from cip_budget before the call
 * so allocation cannot run short, and the cursor/element math is validated when
 * the tag is set up. bytes_null() therefore means end-of-data only, never error,
 * keeping the return value unambiguous (per the "differentiable return/error"
 * rule in the coding guidelines). Any condition that would be an error is caught
 * earlier, at tag setup, and reported there as a PLCTAG_ERR_* status.
 */
Bytes (*encode_chunk)(enip_tag_t *tag, Arena *arena, size_t cip_budget);

/*
 * Consume one chunk response: append data to tag->data and advance the cursor.
 * Returns PLCTAG_STATUS_OK when the operation is complete, PLCTAG_ERR_PARTIAL
 * when another chunk must be requested, or a PLCTAG_ERR_* code on failure.
 */
int32_t (*accept_chunk)(enip_tag_t *tag, Bytes cip_response);
```

- **Shared driver** (in `enip_conn.c`): call `encode_chunk` → send → `accept_chunk`;
  while it returns `PLCTAG_ERR_PARTIAL`, repeat. The driver never knows the scheme.
- **AB impl:** `encode_chunk` emits 0x52/0x53 at `tag->chunk_offset` (bytes);
  `accept_chunk` strips the type code (§3 G), copies, advances `chunk_offset`, and
  returns `PLCTAG_ERR_PARTIAL` while CIP status is `0x06`, else `PLCTAG_STATUS_OK`.
- **OMRON impl:** `encode_chunk` emits 0x4C/0x4D plus the trailing `0x80` data
  segment (§3 I) with `byte_offset = tag->chunk_offset`; `accept_chunk` copies,
  advances `chunk_offset` by bytes received, and returns `PLCTAG_ERR_PARTIAL` until
  the full element count is satisfied.
- Generalize `tag->byte_offset` to `tag->chunk_offset` (byte units for both
  strategies; OMRON's segment already expresses the cursor in bytes).

## 5. Phased plan

Phases map to layers and are each testable end-to-end against `ab_server_fiber`
(and `modbus_server` for the engine in Phase 6). **Phases 1–5 can be validated
with a temporary synchronous request/response harness** (the same blocking style
`enip_metadata.c` already uses) before the async engine lands in Phase 6 — this
keeps each protocol layer independently testable.

### Phase 0 — Compile & scaffold

- **Fix the build break (the `sender_context_base` phantom, §3 N).** The member is
  used but never declared. The surviving counter is `conn->sender_context`
  (`uint64_t`, `enip_conn.h:77`). Concretely:
  - Replace every `conn->sender_context_base` with `conn->sender_context`. As of
    this writing the four uses are at `enip_conn.c:307` (init to 1), `:949`
    (`req_desc.sequence_id = ... + tags_encoded`), `:1014` (value placed in the
    EIP header), and `:1046` (advance by `tags_encoded`). Grep for the symbol to
    confirm — line numbers drift.
  - Where the per-request value is computed (`:949`/`:1014`), also store it into
    that tag's `tag->transaction_id` so the existing
    `enip_connection_find_pending_request` (`enip_conn.c:124`) can match the reply.
  - This is the minimum to compile; full receive-path correlation (calling
    `find_pending_request` instead of broadcasting to all REQUEST tags) is Phase 6,
    but doing the store now means Phase 6 only has to wire the lookup.
- Delete the broken `enip_mfg_omron.c` / `enip_mfg_pccc.c` bodies (stub them to
  return `PLCTAG_ERR_UNSUPPORTED`) so the tree builds.
- Remove the duplicated framing and the unused `cip_payloads` vector.
- Stand up a unit-test target that links the L1–L3 modules and runs against
  captured/known byte buffers.
- **Done when:** library compiles (`make plctag_static`); layer unit tests run.

### Phase 1 — Transport + EIP session

- Implement stream framing (§3 A) as the only read path.
- RegisterSession / UnregisterSession through `enip_eip`.
- **Test:** connect to simulator, register a session (non-zero handle), unregister.

### Phase 2 — CPF + CIP unconnected, single tag

- GetAttributeAll identity via L2/L3, parsed with `enip_cip_parse_response` (§3 B/C).
- Single unconnected ReadTag/WriteTag; response parse with type-code strip (§3 G).
- Select `mfg_ops` from identity (existing selector).
- **Test:** read and write a scalar tag on the simulator; verify value round-trips.

### Phase 3 — Routing + connected messaging

- Unconnected_Send wrapper with route path from the `path` attribute (§3 K).
- ForwardOpen + Forward_Open_Ex + ForwardClose (§3 D/E); derive CIP budget from
  the negotiated size.
- Connected SendUnitData with sequence numbers (§3 F).
- **Test:** connected read/write through a routed path on the simulator, then
  against both reference ControlLogix units (§7) — the `.39` unit exercises the
  FO fallback, the `.40` unit exercises Forward_Open_Ex.

### Phase 4 — Fragmentation strategy

- Add `encode_chunk`/`accept_chunk` to `mfg_ops`; implement the shared driver and
  the AB byte-offset impl (§3 H, §4).
- **Test:** read and write an array larger than one connected packet; verify it
  reassembles (simulator returns status 0x06 mid-transfer).

### Phase 5 — Multi-service (0x0A)

- New `enip_multi.[ch]`: pack/unpack per §3 J, gated by `enip_packetizer_plan`.
- **Test:** batch-read several tags in one frame; compare to N single reads.

### Phase 6 — Connection/session engine + tag queue (Modbus model)

- `find_or_create_connection` keyed by host/port/path/connection-group; tag stores
  `tag->conn`; insert into `conn->active_tags`; launch one handler thread per
  connection (mirror `modbus_plc_handler`).
- Real sender-context correlation (§3 N).
- Auto-reconnect with tag-list reload; idle disconnect then **lazy** reconnect on
  next request; negative cache of names known-absent; lazy Phase-2 on first access.
- Plumb host/port/slot/path from attributes into `conn` (remove hardcoded host).
- **Test:** existing `test_reconnect*`, `test_idle_disconnect`, `test_indexed_tags`
  semantics, pointed at the simulator.

### Phase 7 — Metadata & identity-driven feature enable

- Run root-symbol inventory at connect; link `tag->tag_instance_id` from the cache;
  filter system symbols (§3 L).
- Capability detection: FO_Ex→FO fallback; multi-service from identity status bit.
- **Test:** create a tag by name only (no type attrs) and confirm element size/
  dims come from metadata.

### Phase 8 — OMRON

- OMRON identity path, data-segment fragmentation impl (§3 I, §4), data-type/string
  differences. Validate against `aphytcomm`.
- **Test:** OMRON read/write of scalar + array (against an OMRON capture or device).

### Phase 9 — PCCC

- Service 0x4B encode/decode for PLC5 and SLC per §3 M and the simulator `pccc.c`.
- **Test:** PLC5/SLC read/write/RMW on the simulator.

## 6. Constants to name or negotiate (do not hardcode)

Collect into a header / per-connection config; several must be negotiated, not fixed:
host/port, all `5000` ms timeouts, `60000` ms idle, arena `32768`; the post-FO
`max_packet_buffer_size` (negotiate, §3 D); FO literals `0xF33D`, `0x21504345`,
RPI `1000000`, conn params `0x43F8`, `0x0A/0x0E` ticks, transport `0xA3`; the
`64`-bytes/tag estimate, `overhead=50`, the `>2048` cap, the 10-tag batch limit,
and the `512`/`4096`/`256` scratch buffer sizes. Raw `0x006F`/`0x00B2`/`0x0065`/
`0x6B` literals should use the existing `ENIP_CMD_*` / `ENIP_CPF_ITEM_*` /
`CIP_SVC_*` defines.

## 7. Reference devices (real PLCs, for testing)

Keep these connection strings for end-to-end testing. More (a PLC5 and a
MicroLogix among them) will be added later.

| Attribute string | Device | Notes |
| --- | --- | --- |
| `gateway=10.206.1.39:44818&path=1,5&plc=ControlLogix` | older ControlLogix | does **not** support Extended Forward Open — exercises the FO (0x54) fallback path |
| `gateway=10.206.1.40:44818&path=1,4&plc=ControlLogix` | newer ControlLogix | supports Extended Forward Open (0x5B) |

`path=1,<slot>` is backplane port 1 to the CPU slot; it drives both the
Unconnected_Send route path (§3 K) and the ForwardOpen connection path (§3 D).

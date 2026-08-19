<!--
  Copyright (C) 2026 by Kyle Hayes - kyle.hayes@gmail.com
  This software is available under the MPL 2.0 or LGPL 2 (or later) license.
-->

# ENIP redesign plan — seams, size, and safety

**Date:** 2026-08-12
**Status:** In progress — Phase 0 done, Phase 1 next (§4, §6). Supersedes
[ENIP-UPDATES-PLAN.md](ENIP-UPDATES-PLAN.md) §3 and its
"Target architecture" section, which described the same diagnosis and was not built.
[ENIP-SESSION-DESIGN.md](ENIP-SESSION-DESIGN.md) remains authoritative for the parts
this plan does not move (scheduler, rc lifetime, locking, connection state machine).

This plan restructures the ENIP tree against three goals: safety by construction,
materially less code, and a dialect seam that makes a new PLC family cheap to add.
The code is not in production. Where the delta to reshape a file exceeds the cost of
rewriting it, this plan says rewrite and treats the existing file as the specification.

---

## 0. Ground rules

1. **Server tags are a product feature.** `role=server` and the device simulator stay
   in the library. The consequence is that the CIP/CPF/EIP encoders and decoders must
   be shared between the client and the simulator — two codecs in one shipping library
   is the defect, not the simulator itself.
2. **Wire encoders are preserved through the risky phases.** `enip_cip.c`'s request
   builders, the PCCC address encoders, `cip_path.c`, `identity.c` and `enip_type.c`
   are the tested part. Phase 1 moves *who calls them*, not what they emit. Phase 2
   merges them with their server-side counterparts without changing bytes on the wire.
3. **No backward compatibility constraint.** Attribute strings and the public
   `plc_tag_*` API are the only fixed surface. Every internal type, file and function
   is in scope.
4. **`protocols/ab` and `protocols/omron` are out of scope.** They keep shipping,
   unchanged, alongside the refactored `enip/`. Nothing in this plan deletes them or
   makes them share code with `enip/` — see §9 for what that costs, because it is the
   largest single code-size item in the library and it is deliberately not addressed
   here.
5. **Every phase ends green** on the in-sandbox suites (§7). A phase that cannot is
   split until it can.

---

## 1. Current state

16,230 physical lines across `client/`, `common/`, `server/`, `dialects/`. That is the
wrong number to optimize against:

| | lines | |
|---|---|---|
| license header blocks (59 files × 32) | 1,888 | 11.6% |
| comments | 2,854 | 17.6% |
| blank | 2,551 | 15.7% |
| **code** | **8,846** | **54.5%** |

**All targets in this plan are code lines.** License headers and comments are not
reduction targets — the comments carry invariants and provenance the design depends on,
and cutting them to hit a line count trades knowledge for a smaller number.

The ten largest files by code lines:

| file | code |
|---|---|
| `client/enip_session.c` | 1,643 |
| `client/enip_tag.c` | 904 |
| `common/cip.c` | 795 |
| `client/enip_discover.c` | 582 |
| `client/enip_pccc_addr.c` | 527 |
| `server/device_sim.c` | 484 |
| `server/eip_server_tag.c` | 399 |
| `dialects/pccc/pccc.c` | 302 |
| `client/enip_cip.c` | 282 |
| `common/identity.c` | 260 |

The four structural defects this plan addresses:

**D1 — the dialect seam is inverted.** `enip_logix_apply` (`dialects/rockwell/logix_client.c:120`)
checks a status byte and calls back into the core's `apply_tag_reply`
(`client/enip_session.c:1641`), which dispatches to `apply_open_probe` / `_frag` /
`apply_open_bulk` / `apply_read` / `apply_write` — ~380 lines of Rockwell CIP-symbolic
semantics living in the core. To make that work the core exports `write_window_count`
and `apply_tag_reply` back to the dialects and `enip_connection_internal.h` deliberately
breaks the opaque-connection rule.

**D2 — dialect knowledge is scattered across seven places.** `dialects/`,
`op_traits`/`is_batch_eligible`/`batch_req_size`/`batch_resp_size`
(`enip_session.c:538–579`, hardcoded Rockwell framing), `enip_plc_prefers_connected`
and `enip_plc_is_pccc` (`common/plc_type.h`), four dialect-selection sites
(`enip_dialect_select`, `listing_dialect_for` at `:1668`, ternaries at `:1694` and
`:1198`), ~230 lines of PLC-5 record presentation in `client/enip_tag.c:591–919`, and
fifteen per-dialect fields on the shared tag struct.

**D3 — two CIP codecs.** `common/cip.c:281 parse_cip_request` /`:522 cip_error` and the
`handle_*` responders are the server's; `client/enip_cip.c` is the client's; they never
meet. `common/` is server code in a directory named for sharing.

**D4 — the tag data buffer bypasses `Bytes`.** Nine sites realloc to an exact size and
index with raw arithmetic (`logix_client.c:178,217,240`, `omron_client.c:125,141`,
`pccc_client.c:356`, `enip_session.c:1318,1551`, `enip_discover.c:281`). Each feeds
`mem_realloc(void*, int)` an unchecked `(int)need`. Related: `device_sim_cip_cb`
(`server/device_sim.h:167`) — the simulator's main extension point — is a raw
pointer+length callback.

**D5 — data written as control flow, and one-copy-per-consumer tables.**
`parse_pccc_file_type` (`client/enip_pccc_addr.c:351`) is a ~190-line switch expressing
{prefix → file type, element size, default file number}; the same file already uses the
table idiom at `:70`. Four near-identical bounded-decimal parsers follow it (`:538`,
`:571`, `:664`, `:737`) — the `FIXME` at `:556` notes it. The same PCCC file-type
vocabulary is spelled a third time as a switch in `enip_tag.c:591`. Separately, 36
`attr_get_*` sites across four files each carry their own defaulting, `elem_type` is
parsed independently on the client and server sides over the same `CIP_TYPES[]` table,
and `str_split(gateway, ":")` is written out four times.

**D6 — `bytes_concat` is not null-absorbing.** `bytes_slice` and `bytes_pack_into_impl`
both return `{NULL,0}` for a null input, but `bytes_concat_impl` (`utils/bytes.c`) skips
the `memcpy` for a null member while still advancing the offset — a null member with a
nonzero length yields a **non-null result containing uninitialized bytes**, which is a
wire-corruption hazard. Because null is not reliably absorbing, 60 pure-propagation
guards (`if(bytes_is_null(x)) { return bytes_null(); }`) exist to compensate, 24 of them
in `enip_cip.c` and 20 in `enip_session.c`.

---

## 2. Rewrite vs. rework

Sizes are code lines.

| file | code | decision | why |
|---|---|---|---|
| `client/enip_session.c` | 1,643 | **rewrite**, split 3 ways | The op machinery, batch sizing and selection sites leave; the remainder re-partitions along different lines. Patching leaves the old shape. |
| `dialects/rockwell/logix_client.c` | 149 | **rewrite** | Grows to own the probe/window/fragment machinery lifted out of the core. |
| `dialects/omron/omron_client.c` | 83 | **rewrite** | Same, plus its listing walk stops touching the shared tag. |
| `dialects/pccc/pccc_client.c` | 214 | **rewrite** | Becomes a connection-level dialect; the four `if(t->op != …) return null` guards go away. |
| `client/enip_tag.c` | 904 | **rewrite** | Five hand-rolled vtables collapse to two; PCCC presentation moves to `dialects/pccc/`. |
| `client/enip_discover.c` | 582 | **rewrite** | D1-adjacent: a parallel copy of the tag layer (own struct, vtable, `abort`/`read`/`status`/`get_int_attrib`, formatted-data quad) for what is a transport difference. Becomes a dialect. |
| `client/enip_pccc_addr.c` | 527 | **rewrite** | D5. ~190-line switch that is a table, plus four copies of one number parser. The parsing *rules* are the specification; the structure is not. |
| `common/cip.c` | 795 | **split** | Request/reply parse and emit primitives to `wire/`; `handle_*` responders stay server-side. |
| `client/enip_cip.c` | 282 | **rework → absorbed** | Encoders survive verbatim into `wire/cip.c`; the file stops existing. |
| `common/identity.c` | 260 | **rework** | 10 models × ~17 lines of designated initializers become compact rows. Provenance comments stay verbatim. |
| `client/enip_eip.c` + `.h` | 113 | **delete** | Five struct-literal wrappers around `eip_encode`, plus `typedef eip_hdr_t enip_eip_hdr_t`. |
| `client/enip_connection_internal.h` | — | **delete** | Exists only to leak the connection to dialects. |
| `client/enip_dialect.h` | — | **rewrite** | New vtable. |
| `common/{cip_path,plc_classify,cpf,eip}.c` | 419 | **keep**, move to `wire/` | Already direction-free. |
| `server/*` | ~1,500 | **keep** | Except `device_sim_cip_cb`'s signature (Phase 4) and the attribute table (Phase 3). |
| `dialects/*/{ab,omron}_listing.c`, `pccc.c` | 674 | **rework** | Retarget onto `wire/` in Phase 2; `pccc.c` also gives up its inline address decoding. |

---

## 3. Target layout

```text
protocols/enip/
  wire/         eip.c cpf.c cip.c cip_path.c identity.c type.c pccc_addr.c
                  direction-free codecs: Bytes -> struct, struct -> Bytes.
                  No I/O, no device_t, no connection, no session.
  client/       session.c      transport (TCP + UDP) + EIP session + state machine
                scheduler.c    sorted list, due-tag service, rc/abort discipline
                dispatch.c     request build funnel, MSP packing, reply distribution
                xfer.c         enip_xfer_t and the tag data sink
                tag.c          plc_tag vtable, create, attribute table, accessors
  server/       device.c dispatch.c server_tag.c endpoint.c discovery.c
  dialects/     dialect.h
                rockwell/  client.c  listing.c   (client + simulator halves)
                omron/     client.c  listing.c
                pccc/      client.c  device.c
                discover/  client.c              (enip-udp List Identity)
```

`common/` disappears as a name; everything in it either moves to `wire/` (shared, no
`device_t`) or to `server/` (dispatch).

`client/discover.c` also disappears: discovery is a dialect (§4, Phase 1.5), and the
UDP-vs-TCP difference lives in `session.c` alongside the connected-vs-unconnected one.
`wire/pccc_addr.c` serves both the client's encoder and the simulator's decoder
(Phase 2.3).

---

## 4. Phases

### Phase 0 — sink and dead layers — **done** (2026-08-12)

Mechanical, no design risk, lands first because it shrinks every later diff.

Landed as 0.3 alone first, then 0.1/0.2/0.4 together, both green against the full
suite (§7) plus a new `unit_test_bytes` covering the null-absorption case. Measured
delta in `protocols/enip/`: **−89 code lines** (`enip_eip.c`/`.h` deleted: −81; nine
`mem_realloc(t->data, …)` sites plus one previously-uncounted tenth
(`enip_tag.c`'s `@identity` copy) converted to `tag_data_reserve`/`tag_data_append`;
`bytes_concat` now null-absorbing). Below the −140 estimate in §5 because that
estimate bundled in collapsing the ~60 now-redundant `if(bytes_is_null(x)) return
bytes_null()` propagation guards (24 in `enip_cip.c`, 20 in `enip_session.c`, 8 in
`pccc_client.c`) — deliberately **not** done in this pass, per 0.4's own rule ("fix
as each file is touched, not as a sweep"): removing a guard is safe but is a
judgment call at each call site (some guards also serve as early-exit clarity, not
just null propagation), and touching 52 sites across 3 files broadens this
otherwise-mechanical phase's diff for no behavior change. Left for whoever next
touches those files, or a dedicated pass before Phase 1 starts moving the same
lines.

`tag_data_reserve`/`tag_data_append` ended up as `static inline` in `enip_tag.h`
(shared by `enip_session.c`, `enip_tag.c`, and all three dialect `*_client.c` files
via `enip_connection_internal.h`) rather than `static` duplicates per file — the
plan's snippet showed `static`, but a shared inline avoids re-deriving the same
~15 lines in six translation units. `enip_discover.c` — not an `enip_tag_t`, so it
cannot use the shared helpers' `plc_tag_p`+`size_t*` signature for free — got its
own `buf_cap` field and includes `enip_tag.h` for the two inline functions; that
include (and the field) both disappear when 1.5 folds discovery into a dialect.
Same treatment for `eip_frame()` (0.2): one `static inline` in `common/eip.h`
replacing the five `enip_eip_*` wrappers, used by both `enip_session.c` and
`enip_discover.c`, instead of a `static` copy in each.

0.1 also deleted the redundant `read_off` byte-cursor tracking in the three
listing/UDT accumulation sites (Rockwell `@tags`/`@udt`, OMRON `@tags`/`@udt`, PCCC
File-0 listing) per §1's note that `t->size` already carries the same value —
`enip_logix_build_listing`'s `UDT_FIELDS` case now derives the request offset from
`t->size - 14` instead of a separately maintained field. `read_off`'s other job (the
element-count bulk/read/write cursor) is untouched.

**0.1 Tag data sink.** Add `size_t buf_cap` to the ENIP tag and two static helpers:

```c
static bool tag_data_reserve(enip_tag_p t, size_t need);   /* doubling, floor 64, rejects > INT32_MAX */
static bool tag_data_append(enip_tag_p t, Bytes src);      /* reserve + bytes_slice + bytes_pack_into */
```

No new type in `utils/`: `bytes_from_buf` + `bytes_slice` + `bytes_pack_into` already
give a bounds-checked write path over heap memory, and `bytes_pack_into` returns
`{NULL,0}` on overflow. Only growth was missing.

Convert all nine sites. In the listing paths this deletes the byte-cursor use of
`read_off` entirely — `t->size` is already kept in step with bytes written, so
`read_off` was a redundant second copy. It reverts to meaning only what
`enip_tag.h` documents: the element cursor for bulk/read/write.

Invariant to write into the header: **never cache a remainder `Bytes` across a
reserve.** `mem_realloc` may move the block. Recompute the slice from `t->data` on
each append.

**0.2 Delete `client/enip_eip.c` / `.h`.** Replace the five wrappers with one call:

```c
static Bytes eip_frame(Arena *a, uint16_t cmd, uint32_t session_handle, Bytes payload);
```

Drop the `enip_eip_hdr_t` alias; use `eip_hdr_t`.

**0.3 Make null absorbing across the `Bytes` API (D6).** Three lines in
`bytes_concat_impl` (`utils/bytes.c`) to return `{NULL,0}` if any member is null. This
is a correctness fix first — today a null member with a nonzero length produces a
non-null buffer with uninitialized bytes in it, which reaches the wire. Once null
propagates reliably through `slice`, `pack_into` and `concat` alike, the 60
pure-propagation guards collapse to one check at the end of each build chain (24 in
`enip_cip.c`, 20 in `enip_session.c`, 8 in `pccc_client.c`).

Do the `bytes.c` change and its unit-test coverage first, in isolation, since every
other protocol in the tree uses `bytes_concat` too.

**0.4 Declaration hygiene.** 39 function definitions in the client tree are neither
`static` nor `extern` (14 in `enip_cip.c`, 10 in `enip_session.c`, 5 in `enip_eip.c`,
…), against `coding_guidelines.md`. Fix as each file is touched, not as a sweep.

*Delta:* **−140 code** (−100 sink and `enip_eip.c`, −40 guards); no relocation.
*Verify:* full suite (§7); `omron_udt_walk` and `server_udt` drive multi-fragment
accumulation across the growth boundary. Add a `bytes_concat` null-member case to the
`utils` unit tests.

---

### Phase 1 — invert the dialect seam

The core of the plan. Lands as one change: the old `apply` seam and the new `absorb`
seam cannot coexist, because the core op state machine is exactly what is removed.

**1.1 The transfer.** `client/xfer.h`:

```c
typedef struct {
    /* set by the core before plan() */
    enip_op_t op;
    Bytes     path;              /* encoded IOI, or PCCC address bytes */
    uint32_t  elem_count;
    size_t    max_payload;       /* max_cip_packet_size - cip_overhead */

    /* the sink (Phase 0), read by the tag layer */
    uint8_t  *data;
    int32_t   size;
    size_t    cap;

    /* published to the tag when the transfer first completes */
    uint32_t  elem_size;
    const tag_byte_order_t *byte_order;

    /* dialect-private; the core never reads these bytes */
    uint8_t   priv[ENIP_XFER_PRIV_SIZE];
} enip_xfer_t;
```

Each dialect casts `priv` to its own struct behind a `_Static_assert` on size.
Rockwell and PCCC need ~40 bytes. OMRON's `udt_walk_pending[32]` is the outlier at 128
and must allocate lazily rather than set the ceiling for every tag — today every tag
carries that queue unconditionally.

`byte_order` closes a live gap: `enip_type_decode` already returns a per-type order and
`enip_session.c:1336` decodes it into a local and discards it; all ENIP tags share one
static table (`enip_tag.c:73`). OMRON's differing float/string ordering has nowhere to
live until this field exists.

**1.2 The vtable.** `dialects/dialect.h`:

```c
typedef struct {
    size_t   request_size;    /* Large Forward Open request; 0 = engine default */
    uint16_t max_batch;       /* 0 = engine default, 1 = never pack */
    bool     connected;       /* ForwardOpen at bring-up */
    bool     msp;             /* may be packed into a 0x0A Multiple Service Packet */
    uint8_t  frag_align;
} enip_caps_t;

typedef struct {
    const char *name;
    enip_caps_t caps;

    /* PENDING = at least one request to send; OK = already complete;
     * PLCTAG_ERR_UNSUPPORTED = this dialect does not do this op. */
    int32_t (*plan)(enip_xfer_t *x);

    /* Next request into dest. Returns the used prefix, or bytes_null() if it does
     * not fit dest OR the expected reply exceeds resp_budget.
     * MUST NOT advance any cursor in priv. */
    Bytes (*emit)(enip_xfer_t *x, Bytes dest, size_t resp_budget);

    /* Consume one CIP reply. PENDING = another round trip; OK = complete. */
    int32_t (*absorb)(enip_xfer_t *x, Bytes reply);
} enip_dialect_t;
```

Two invariants carry the design:

- **`emit` returning null is the packing test.** The MSP packer walks due transfers
  calling `emit` into the remaining slot until one declines. This deletes
  `batch_req_size`, `batch_resp_size` and the size half of `is_batch_eligible` —
  ~40 lines of hardcoded Rockwell framing that silently mis-sizes PCCC sub-requests
  today.
- **`emit` is side-effect-free; `absorb` advances cursors.** The packer must be able
  to call `emit` speculatively and discard the result.

**1.3 Move the op machinery into the dialects.**

| from (core) | to |
|---|---|
| `apply_open_probe` (`:1333`, 106 ln) | `rockwell/client.c` — `plan(READ)` emits count=1 ReadFrag; `absorb` decodes the type header, computes both windows, transitions |
| `apply_open_probe_frag` (`:1446`) | `rockwell/client.c` `priv` state |
| `apply_open_bulk` (`:1495`) | `rockwell/client.c` `priv` state |
| `apply_read` (`:1531`), `apply_write` (`:1610`) | `rockwell/client.c` `absorb` |
| `apply_tag_reply` (`:1641`) + `op_traits` (`:538`) | deleted |
| `frag_reset`, `frag_append`, `write_window_count` | `rockwell/client.c` statics |
| PCCC's `plan` | sets `elem_size` from `pccc_addr_t`; no probe at all |

`ENIP_OP_OPEN_PROBE`, `ENIP_OP_OPEN_PROBE_FRAG` and `ENIP_OP_OPEN_BULK` leave the
shared enum — they are Rockwell states, not operations a caller requests. The enum
reduces to `READ`, `WRITE`, `LIST`, `UDT_META`, `UDT_FIELDS`.

The core keeps one piece of the probe: a tag is typed once `x->elem_size != 0`, and the
core raises `PLCTAG_EVENT_CREATED` on that transition. That is tag lifecycle, not
protocol.

**1.4 One selection site.** `enip_dialect_select(plc_type)` at identity, full stop.
PCCC becomes a connection-level dialect — it could not be one before only because the
connection dialect also had to serve ops PCCC rejects. Deletes `listing_dialect_for`,
both ternaries, and `ENIP_TAG_KIND_PCCC` / `_LISTING` / `_UDT`; those three "kinds" are
one dialect with different entry points, not three tag types.

`enip_plc_prefers_connected()` and `enip_plc_is_pccc()` fold into `caps.connected` and
dialect identity. Micro800 gains its own dialect entry — same functions as Logix,
`caps.max_batch = 1` — which is the case ENIP-SESSION-DESIGN.md §16.4 records as never
done because there was nowhere to put it that wasn't a new core branch.

**1.5 Discovery becomes a dialect.** `enip_discover.c` (582 code lines) carries its own
tag struct, its own vtable, its own `abort` / `read` / `status` / `get_int_attrib`, and
its own formatted-data quad — a parallel copy of the tag layer for a tag whose only real
difference is that the transport is UDP. As a dialect it is `plan` = send List Identity,
`absorb` = append one record to the sink, and the UDP/TCP difference moves to the
transport where it belongs.

This folds into Phase 1 rather than trailing it: the duplicate tag machinery only
disappears once there is a transfer model to replace it with, and leaving it for later
means converting the discovery tag twice.

Note `enip_discover.c`'s tag is deliberately *not* an `enip_tag_t` today, and
`lib/tag.h:128` documents a hazard that depends on that (`enip_tag_get_conn` would read
the wrong struct if a UDP tag matched). Collapsing the two removes the hazard; check
that comment is retired with it.

**1.6 Split `enip_session.c`.** 1,643 code lines → `session.c` (~500: socket, EIP
session, connect/register/identity/open/close state machine, reconnect, idle),
`scheduler.c` (~300: sorted list, `service_due_tags`, rearm, abort, rc), `dispatch.c`
(~250: build funnel, `wrap_tag_frame`, MSP pack, reply distribution). Delete
`enip_connection_internal.h`; `emit`/`absorb` see only `enip_xfer_t`.

*Delta:* **−430 code** — but the composition matters:

| | code |
|---|---|
| deleted: `op_traits`, batch sizing trio, three selection sites, pruned tag fields | −100 |
| deleted: discovery's duplicate tag machinery (1.5) | −330 |
| *relocated*: core `apply_*` set → dialects | ±250 |
| *relocated*: `enip_session.c` → three files | ±1,643 |

**The seam inversion itself is close to size-neutral.** It is a design fix: it changes
how many places a new PLC family touches, from nine to one directory. Judge it on that,
not on line count. The −430 comes from the two deletions above.

*Risk:* highest of any phase. *Verify:* full suite. `server_udt` test 5 covers connected
and unconnected on the Rockwell path; `omron_udt_walk` the OMRON continuation walk;
`discover_identity` / `scan_eip_network` for the discovery collapse; `test_fairness`
against real hardware for scheduler behaviour under load before declaring the phase done.

---

### Phase 2 — one wire codec

Now that the client's call sites are stable, merge the two codecs.

**2.1 Create `wire/`** by moving `common/{eip,cpf,cip_path,plc_classify}.c` and
`client/enip_type.c` unchanged. Rename only.

**2.2 `wire/cip.c` — the merge.** Symmetric pairs, arena-allocating, no `device_t`,
no session:

```c
extern bool  cip_req_decode(Bytes in, cip_req_t *out);        /* from common/cip.c:281 */
extern Bytes cip_req_encode(Arena *a, cip_req_t *req);        /* from client/enip_cip.c */
extern bool  cip_reply_decode(Bytes in, cip_reply_t *out);    /* from client/enip_cip.c */
extern Bytes cip_reply_encode(Arena *a, cip_reply_t *rep);    /* from common/cip.c:522 + handle_* tails */
extern Bytes cip_msp_encode(Arena *a, Bytes *subs, uint16_t count);
extern bool  cip_msp_decode(Bytes in, uint16_t *count, Bytes *subs, uint16_t cap);
```

The client uses `req_encode`/`reply_decode`; the simulator uses `req_decode`/
`reply_encode`. Both use the same path, segment and status primitives. Today
`common/cip.c:304 extract_path` and `client/enip_cip.c:77 enip_cip_encode_tag_path`
are the two halves of the same format and share nothing.

`common/cip.c`'s `handle_forward_open` / `handle_read` / `handle_write` /
`handle_identity` / `handle_multi` / `handle_omron_variable_attrs` stay behind as
`server/dispatch.c` — they are policy over a device, not wire format.

Also fold `cip_path.c`'s "single source of truth" claim into reality: it currently owns
only logical segments while symbolic IOI encoding lives in the client.

**2.3 `wire/pccc_addr.c` — the same merge one layer down (D5/D3).**
`client/enip_pccc_addr.c` (527) *encodes* PLC-5 and SLC logical addresses;
`dialects/pccc/pccc.c:251–264,435–440` *decodes* the same format off the wire with
inline `bytes_unpack`. Same format, two directions, nothing shared. Merge, then rewrite
the parser per D5:

- `parse_pccc_file_type` (`:351`, ~190 lines) becomes a table of
  `{prefix, file_type, elem_size, default_file}` with longest-prefix, case-insensitive
  match — ~25 rows plus a ~15-line matcher. The file already uses this idiom at `:70`.
- The four bounded-decimal parsers (`:538`, `:571`, `:664`, `:737`) become one helper.
- `pccc_file_type_name` (`enip_tag.c:591`) is a third spelling of the same vocabulary;
  it reads the `name` column of the new table instead.

**2.4 `wire/identity.c`** keeps its catalog but as compact rows rather than 10 blocks of
designated initializers. Every provenance comment moves across verbatim — those record
which rows came from a real capture and which are defaults, and one of them documents a
previously mistyped length byte.

**2.5 Retarget the dialect simulator halves** (`ab_listing.c`, `omron_listing.c`,
`pccc.c`) onto `wire/`, so each dialect directory holds a client half and a simulator
half over one codec.

*Delta:* **−630 code**, all genuine deletion:

| | code |
|---|---|
| CIP parse/emit primitives deduplicated (`common/cip.c` + `enip_cip.c`) | −250 |
| PCCC address switch → table, four number parsers → one, third spelling removed | −250 |
| identity catalog as rows | −130 |

*Risk:* moderate and wide; every file compiles against new headers.
*Verify:* `test_enip_eip_cpf`, `test_enip_cip_path`, `test_enip_type`,
`test_enip_pccc_addr` (existing unit tests) plus the full suite. Golden-byte
assertions in those unit tests are the safety net for "no wire change" — and
`test_enip_pccc_addr` specifically guards the address rewrite, which is the highest-risk
edit in this phase because the parsing rules are the specification and only the
structure is meant to change. Extend it with the odd cases first (`I`/`O` with an
omitted file number, `B` vs `BT`, `S` vs `ST`, subelement mnemonics) before touching
the parser.

---

### Phase 3 — collapse the tag layer

**3.1 One vtable, not five.** `enip_tag.c` carries separate vtables for data,
`@connection`, `@identity`, listing and PCCC-listing tags. After Phase 1 only two
behaviours remain genuinely distinct: transfer-backed tags (data, `@tags`, `@udt` — all
just a transfer with a different op) and cache-backed tags (`@connection`, `@identity`,
which issue no network op). Collapse to those two.

**3.2 Move PCCC presentation out.** `enip_tag.c:591–919` — `pccc_file_type_name`,
`pccc_decode_plc5_file_record`, `pccc_record_view_init`, and the four
`enip_pccc_listing_get_*` entry points — is ~230 lines of PLC-5 knowledge in the
generic tag file. It belongs in `dialects/pccc/`. Reached through a per-dialect
`format` hook alongside `plan`/`emit`/`absorb`, or a small table keyed on dialect;
decide when the second dialect needs one, not before.

**3.3 One formatted-data implementation, not three.** Twelve near-identical entry
points — `enip_tag.c:461,471,497,504` (identity), `:816,836,871,880` (PCCC listing),
`enip_discover.c:678,687,708,714` (discovery) — each build a view struct and call the
*already table-driven* `utils/cbor_schema.c`. Collapse to one implementation
parameterised by a descriptor hung off the tag:

```c
typedef struct {
    const cbor_field_t *fields;
    size_t count;
    const char *schema_name;
    bool (*build_view)(plc_tag_p tag, void *view_out);
} cbor_view_t;
```

The line saving is modest; the point is that adding JSON — already designed in
`ENIP-METADATA-AND-DISCOVERY-DESIGN.md` §0 and unbuilt — becomes one change instead of
three, and the fourth consumer (a `@tags`/`@udt` schema, also designed and unbuilt) costs
a table rather than another quad.

**3.4 One attribute-parsing table (D5).** 36 `attr_get_*` sites across
`eip_server_tag.c` (18), `enip_tag.c` (10), `enip_session.c` (5) and `enip_discover.c`
(3), each with its own defaulting and validation. `elem_type` is parsed independently on
the client and server sides over the same `CIP_TYPES[]` table (`server/device_types.c`).
`str_split(gateway, ":")` appears four times counting `mb/modbus.c:1503` — that one is a
`utils` helper, not an ENIP concern, but the ENIP copies fold into the table's parser.

Replace with a declarative descriptor per tag kind: `{name, type, default, offset,
validator}`, walked once at create.

**3.5 Prune the tag struct.** The union's data variant loses `read_off`,
`window_elems`, `write_window_elems`, `frag_offset`, `frag_write_chunk`, `frag_align`,
`fragmented_elem`, `frag_more`, `type_header[4]`, `type_header_len`, `list_next_id`,
`list_total`, `udt_walk_pending[32]`, `udt_walk_pending_count`, `pccc_addr`,
`pccc_plc5`, `is_bit`, `bit` — all now in `priv` or gone.

*Delta:* **−390 code** deleted (−150 vtable collapse, −150 attribute table, −90
formatted-data), plus ~230 *relocated* to `dialects/pccc/`.
*Verify:* full suite; `server_tag_basic` and `test_auto_sync_param` exercise the generic
accessors and event paths hardest. The attribute table needs negative cases —
malformed `gateway`, unknown `elem_type`, out-of-range `elem_count` — since the per-site
validation it replaces is currently untested.

---

### Phase 4 — server extension point

`device_sim_cip_cb` (`server/device_sim.h:167`) takes
`const uint8_t *path, uint32_t path_len, uint8_t *resp, uint32_t resp_cap, uint32_t *resp_len`
— the last raw pointer+length interface in the tree, and the one every dialect's
simulator half implements. Convert to:

```c
typedef int32_t (*device_sim_cip_cb)(device_sim_t *sim, uint8_t service, Bytes path,
                                     Bytes req, Bytes resp, size_t *resp_len, void *user_data);
```

Three implementations to update (`ab_listing.c`, `omron_listing.c`, `pccc.c`).

*Delta:* ~0 lines, pure safety. *Verify:* `omron_aphyt_metadata`, `server_udt`,
`omron_udt_walk`.

---

## 5. Expected size

Code lines only, and **`protocols/enip/` only** — these numbers are not library-wide. The
shipped library also carries `protocols/ab` (94.2 KiB) and `protocols/omron` (53.3 KiB)
doing the same job as `enip/` (107 KiB); retiring those is out of scope (§9.1) and is
where library-level size actually lives.

Estimates except Phase 0 (measured), not commitments; the reasoning matters more
than the number.

| phase | deleted | relocated |
|---|---|---|
| 0 — sink, `enip_eip.c`, null propagation | **−89 (measured)** | 0 |
| 1 — dialect seam + discovery collapse | −430 | ~1,900 |
| 2 — one wire codec, PCCC address, identity rows | −630 | ~1,100 |
| 3 — tag layer, formatted data, attribute table | −390 | ~230 |
| 4 — server extension point | ~0 | 0 |
| **total** | **≈ −1,540** | |

**8,846 → ≈ 7,300 code lines, about 17%.** Phase 0 landed −89 rather than the
original −140 estimate because the propagation-guard collapse it bundled in was
deliberately deferred (§4 Phase 0 note) rather than swept in one pass; the other
−51 is `tag_data_reserve`/`tag_data_append`/`eip_frame` themselves (~55 new code
lines in headers) netted against the sink conversions and `enip_eip.c`/`.h`'s
deletion.

Two things this table is careful about:

1. **Deleted ≠ relocated.** Roughly 3,200 code lines move between files across Phases 1
   and 2 without shrinking. An earlier draft of this plan counted that movement as
   reduction and claimed 16,171 → 12,300; that number was inflated both by counting
   relocation and by counting license and comment lines.
2. **The dialect directories grow, and that is the intended outcome.** Rockwell, OMRON
   and PCCC roughly triple as the op machinery lands in them. The win is that a new
   family becomes three functions and a caps struct in one directory, instead of edits
   to the op enum, the tag struct, `op_traits`, `is_batch_eligible`, three family
   predicates, four selection sites and `enip_tag.c`. Phase 1 should be judged on that,
   not on line count.

Where the reduction actually comes from, largest first: the discovery tag collapse
(−330), the CIP codec merge (−250), the PCCC address rewrite (−250), the tag-layer
vtable collapse (−150), the attribute table (−150), the identity catalog (−130), the
sink and `enip_eip.c` (−100), the formatted-data collapse (−90), the propagation guards
(−40), and the Phase 1 seam deletions (−100).

Non-targets, stated so they don't get optimized by accident: the 1,888 lines of license
headers, the 2,854 comment lines, and the 393 `pdebug` sites. The comments carry
invariants and provenance the design depends on, and the logging is the only diagnostic
available for a protocol that cannot be stepped through against live hardware.

---

## 6. Order and dependencies

```
Phase 0  ──> Phase 1 ──> Phase 2 ──> Phase 3
                            └──────> Phase 4
```

Phase 0 first because the sink touches every site the later phases rewrite, and because
the `bytes_concat` fix within it is a correctness change that should not be entangled
with anything else. Phase 1 before Phase 2 because it moves ~1,900 code lines between
files; merging codecs first would mean migrating call sites that are about to move
anyway. Phase 3 after Phase 2 because the tag layer's formatted-data paths call into
codecs that Phase 2 relocates. Phase 4 is independent of 3 and can run in parallel.

Phases 1 and 2 each land as a single change. Phases 0, 3 and 4 subdivide freely — and
0.3 (`bytes_concat`) should land on its own before the rest of Phase 0.

If only part of this gets done, the ordering by value-per-risk is different from the
ordering by dependency: **0.3** (a real bug, three lines), then **1** (the seam, which is
the whole point even though it barely moves the line count), then **2**. Phase 3 is
mostly cleanup and Phase 4 is pure safety hygiene; both can wait indefinitely without
blocking anything.

---

## 7. Verification

The in-sandbox suites are the net for every phase. All run against a local
`device_sim` with no hardware:

| test | covers |
|---|---|
| `server_tag_basic` | `role=server` lifecycle, client read/write, fault injection, 26 assertions |
| `server_udt` | `udt=` templates, `@udt/<id>` walk, **and** the connected/unconnected transport pair (test 5) |
| `omron_udt_walk` | OMRON sibling/nested UDT continuation over real wire bytes |
| `omron_aphyt_metadata` | server-side class 0x6A/0x6C listing, no sockets |
| `devsim_with_plctag` | end-to-end smoke |
| `unit/test_enip_{eip_cpf,cip_path,type,pccc_addr}` | golden-byte codec assertions — the Phase 2 safety net |

Gaps to close as the phases land, since none of these currently exercise the paths
being rewritten hardest:

- **A batch-packing test.** Nothing in-sandbox asserts 0x0A Multiple Service behaviour;
  Phase 1 rewrites the packer. Add one: N server tags on one endpoint, one client
  connection, assert all N complete from one round trip.
- **A PCCC end-to-end test.** `device_sim` emulates PCCC families; there is no
  in-sandbox client test for the PCCC data path, which Phase 1 rewrites.
- **Real hardware before declaring Phase 1 done.** `run_enip_tests.sh` against the
  ControlLogix at `10.206.1.40` path `1,4`, plus `test_fairness` for scheduler
  behaviour under load. The simulator cannot produce a `CIP_STATUS_FRAG` reply
  (no aggregate or STRING wire type), so byte fragmentation is untested in-sandbox
  either way — that is a pre-existing gap this plan does not close.

**CI does not build this tree.** `.github/workflows/ci.yml` never sets
`LIBPLCTAG_FEATURE_ENIP`, and the feature defaults to `0` in the top-level
`CMakeLists.txt`. Add an ENIP+SERVER configuration to CI before Phase 1, or every phase
is verified only by whoever last ran a local build.

---

## 8. Risks

| risk | mitigation |
|---|---|
| Phase 1 is one large change with no partial landing | Wire encoders unchanged, so the diff is control flow only; three dialects convert together against a green suite |
| `priv` is a fixed ceiling | `_Static_assert` per dialect makes overflow a compile error; OMRON's walk queue allocates rather than sets the ceiling |
| Abort mid-transfer | Contract: a dialect holds nothing across round trips except `priv` and the sink, so the core may drop a transfer between any two round trips |
| Speculative `emit` in the packer | Contract: `emit` does not mutate `priv`; only `absorb` advances cursors |
| Phase 2 changes bytes on the wire by accident | Existing golden-byte unit tests run first; CIP encoder logic is relocated and given one signature, not edited |
| The PCCC address rewrite (2.3) *does* edit parsing logic | Highest-risk edit in the plan: the parsing rules are the specification and only the structure should change. Extend `test_enip_pccc_addr` with the odd cases (`I`/`O` omitted file number, `B` vs `BT`, `S` vs `ST`, subelement mnemonics) **before** touching the parser |
| `bytes_concat` null-propagation change affects every protocol | Land it alone, first, with its own unit-test case; Modbus and AB both use `bytes_concat` |
| The attribute table replaces untested per-site validation | Add negative cases (malformed `gateway`, unknown `elem_type`, out-of-range `elem_count`) as part of 3.4, not after |
| Fragmentation has no in-sandbox coverage | Pre-existing. Either add a STRING/aggregate wire type to `device_sim` (also unblocks the STRING/UDT client gap) or accept hardware-only verification |

---

## 9. Out of scope

### 9.1 Retiring `protocols/ab` and `protocols/omron`

Out of scope, and the largest thing this plan does not fix. Recorded here so the size
numbers in §5 are not read as library-wide.

Measured with `bloaty` against a `RelWithDebInfo` build of `libplctag.2.7.2.dylib`
(568 KiB total; note bloaty 1.1 cannot read clang's DWARF 5, so the build needs
`-gdwarf-4` and a `dsymutil` pass):

| tree | binary |
|---|---|
| `protocols/enip/` | 107 KiB |
| `protocols/ab/` | 94.2 KiB |
| `protocols/omron/` | 53.3 KiB |
| `protocols/mb/` | 29.9 KiB |

**45% of the shipped library is three overlapping EtherNet/IP implementations**, and
`enip/` is already larger than either of the drivers it was written to replace. Bloaty
names concrete duplicate pairs:

- `_parse_pccc_logical_address` (1.72 KiB, `ab/pccc.c`) and
  `_enip_pccc_parse_logical_address` (1.71 KiB, `enip/client/enip_pccc_addr.c`) — the
  same function, copied rather than shared, both linked.
- `_cip_encode_path` (2.68 KiB) + `_cip_encode_tag_name` (1.26 KiB) against
  `_enip_cip_encode_path` (1.21 KiB).
- `_cip_type_lookup` (6.63 KiB, `omron/cip.c`) — the single largest ENIP-related symbol
  in the library, and `enip/` already carries two more type tables of its own
  (`client/enip_type.c`, `server/device_types.c`'s `CIP_TYPES[]`).

Three PCCC address parsers and three CIP type tables ship today.

The copying was deliberate — `enip/` was built by copying from `ab` specifically so that
`ab` could be deleted later without `enip/` depending on it. That decision stands: this
plan does not introduce a dependency between them, and the duplication is the price of
keeping the two independently deletable.

**Consequence to keep in view: none of the reductions in §5 shrink the shipped library.**
They shrink `enip/`. The library gets smaller when `ab` and `omron` are retired, which is
a product decision about protocol coverage and user migration, not a refactor. If that
retirement is ever scheduled, do it *after* Phase 2 — `wire/` is the thing that would let
a successor absorb the PCCC and CIP-type knowledge from `ab`/`omron` instead of copying
it a fourth time.

### 9.2 Other deferrals

- Multi-dimensional array linearization (`ENIP-SESSION-DESIGN.md` §16a.5) and arrays of
  oversized elements. Unrelated to the seams; easier after Phase 1 because the
  windowing logic will live in one dialect.
- The `enip-udp` read hang. Pre-existing bug, tracked separately.
- `@listidentity`, JSON format, `@tags`/`@udt` CBOR schemas. Feature work.
- APHYT compatibility Phases 3–5. Server-side feature work on top of Phase 4.

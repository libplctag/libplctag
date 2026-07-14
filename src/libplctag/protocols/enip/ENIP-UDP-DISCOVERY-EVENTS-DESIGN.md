# enip-udp / @identity — incremental record delivery via the event callback

Design note for the streaming behavior of the new-ENIP `@identity` /
`@listidentity` tag — `enip-udp` broadcast discovery, plus `enip-udp` and
TCP `enip` unicast identity, all returning the same header+records format
(§1). When an event callback is registered, fire it once per newly received
record, with the tag data buffer updated *before* the callback runs. This
document decides which event to use and how to deliver it.

The design uses the protocol attribute to note that this is UDP:

'enip-udp' - use UDP transport.  Unicast or broadcast depending on the CIDR subnet specified.

We add to the schema for the gateway.  If it has a CIDR '/<n>' notation, it indicates the subnet for UDP discovery messages, so the gateway knows which network range to broadcast discovery requests to.  If it does not have a CIDR notation or the CIDR subnet is '/32', then this is a unicast UDP message.


`gateway=<ip>(/<cidr>)(:<port>)`

Both the CIDR mask and the port are optional. The port defaults to 44818.  The CIDR mask defaults to '/32', indicating a unicast message if not specified.

We overload the meaning of the '@identity' tag name.  If the protocol is 'enip-udp' _and_ there is a CIDR subnet specified that is not 32, then the '@identity' name means we are doing broadcast discovery for UDP discovery within that subnet.  We do not need an additional new special tag name.

To avoid as much surprise as possible, we introduce the '@listidentity' tag name as an alias of '@identity'.  Users who know CIP well can use '@listidentity' to indicate they are interested in the list identity semantics, while others can continue using '@identity'.  Users who are unaware of the distinction can continue using '@identity' without needing to understand the list identity semantics.

In order to make this easier to use, '@identity' (unicast or TCP) must return the identity data in the exact same format below.  A header with a version and record count, then each record starts with a length and known structure, so that the parsing logic can be identical regardless of whether the discovery is unicast or broadcast.

Note that these changes to '@identity' only affect the new ENIP protocol (both UDP and TCP).  The existing AB and Omron implementations of '@identity' are unchanged; this is only for the new ENIP protocol handling.

## 1. Problem

Discovery collects 0..N replies over a `discover_wait_ms` window (see the
`enip-udp` design). A caller with an event callback wants to react to each
device as it appears, not only at the end of the window. Requirements:

1. One callback invocation per new (deduped) record.
2. The tag data buffer — header `record_count` and the appended record — is
   consistent and visible *before* the callback fires.
3. A caller with **no** callback still works: it reads `record_count` /
   walks the buffer after `READ_COMPLETED`.
4. A synchronous `plc_tag_read(tag, timeout)` still returns exactly once,
   after the full window.

## 2. Constraints from the existing event model

Two facts (verified in `lib/lib.c`) shape every option below:

- **The generic event path coalesces.** `tag_raise_event` sets a per-event
  boolean latch (`tag->event_read_complete`, …); `plc_tag_generic_handle_event_callbacks`
  fires the callback and clears the latch. N raises of the same event before
  one dispatch collapse into **one** callback. Any per-record event routed
  through this path would drop records.
- **`READ_COMPLETED` is a one-shot terminal.** It clears `read_in_flight`,
  sets `read_complete`, and wakes a blocked synchronous `plc_tag_read`. The
  first one returned ends the read.

There is precedent for stepping outside the coalescing path: the `@connection`
tag calls `tag->callback(...)` **directly**, once per conn-status ring entry,
using event codes in a reserved offset block (`PLCTAG_EVENT_CONN_STATUS_*` =
100–105). Per-record discovery delivery follows the same shape.

## 3. Why not reuse `READ_COMPLETED`

Firing `READ_COMPLETED` per record is wrong on every requirement:

- **Breaks synchronous reads.** The first `READ_COMPLETED` returns
  `plc_tag_read`, so the caller gets one device and the collection window is
  defeated (requirement 4).
- **Coalesces.** Routed through the latch, N completions collapse to one
  (requirement 1). Called directly to avoid that, it still lies about the
  read being finished.
- **Corrupts read state.** `READ_COMPLETED` clears `read_in_flight` /
  sets `read_complete`; doing that mid-window desynchronizes the read
  lifecycle.

`READ_COMPLETED` must keep its meaning: **the window closed, the buffer is
final.** It fires exactly once, at the end.

## 4. Decision: a new, general incremental-data event

Add one base event:

```c
PLCTAG_EVENT_DATA_RECEIVED = 8    /* incremental data appended; buffer updated;
                                     the current read is still in progress */
```

Semantics: "the tag produced more data and its buffer was updated; more may
follow; the read is not yet complete." It slots between `READ_STARTED` and
the terminal `READ_COMPLETED`. It is intentionally **general**, not
discovery-specific — a future streaming/implicit-I/O or subscription tag can
reuse the same event rather than each adding its own.

Rejected alternatives:

- **Reuse `READ_COMPLETED`** — §3.
- **An offset block** like `PLCTAG_EVENT_CONN_STATUS_*` (e.g.
  `DISCOVERY_RECORD_OFFSET`) — the offset scheme exists to encode a *state
  value* into the event code. A record carries no such small enum; the payload
  is in the buffer. A single flat event is simpler and reusable.
- **Discovery-specific name** (`PLCTAG_EVENT_DISCOVERY_RECORD`) — narrower for
  no benefit; the mechanism isn't discovery-specific.

ABI note: adding an enum value is backward compatible. Existing callbacks that
`switch` on the event code and ignore the `default` simply never see it unless
they create a streaming tag. `PLCTAG_EVENT_MAX` stays 105 (the conn-status
block); value 8 is free and does not collide.

## 5. Delivery mechanism

Per record, on the discovery worker thread:

```c
critical_block(tag->api_mutex) {
    append record to tag->data
    header.record_count += 1              /* buffer now consistent */
    if(tag->callback)
        tag->callback(tag->tag_id, PLCTAG_EVENT_DATA_RECEIVED,
                      PLCTAG_STATUS_OK, tag->userdata)
} /* end critical_block */
```

Direct call, **not** `tag_raise_event` — that is what avoids coalescing and
guarantees one callback per record, in arrival order. This mirrors
`enip_conn_tag_tickler`.

**Buffer-before-callback (requirement 2):** the append and count bump happen
before the callback, under the same lock, so the callback observes the new
record. The callback identifies "what's new" by reading the header: on the
k-th invocation `record_count == k` and the newest record is the last one in
the buffer. (The callback signature carries no payload; `status` stays
`PLCTAG_STATUS_OK`. Encoding the record index into `status` was considered and
rejected — it overloads a field that means "status code" everywhere else.)

**Lock held across the callback:** consistent with the generic dispatch, which
already invokes callbacks under `api_mutex`. The platform mutex is
`PTHREAD_MUTEX_RECURSIVE`, so the callback may call `plc_tag_get_*` on *this*
tag re-entrantly without deadlock. Standard callback guidance still applies:
keep it short; do not start new operations (e.g. another `plc_tag_read`) on
this tag from inside it.

**Thread:** the callback runs on whichever library-owned thread produces the
record — the UDP discovery worker for `enip-udp`, or the session/identity path
for a TCP `enip` `@identity` (which yields a single record). Same model as
`@connection` (IO thread) and the documented "callbacks run on library
threads" contract.

## 6. Event sequence for one read

Broadcast discovery (`enip-udp` with a CIDR mask `< /32`) produces N records:

```text
plc_tag_read(tag, timeout)                 (API thread)
  → READ_STARTED                           via the normal latch path

  ── collection window (worker thread) ──
  → DATA_RECEIVED   (record 1)   buffer: count=1, +record
  → DATA_RECEIVED   (record 2)   buffer: count=2, +record
      ...
  → DATA_RECEIVED   (record N)   buffer: count=N, +record

  → READ_COMPLETED                         window closed, buffer final
```

A unicast `enip-udp` (`/32` or no CIDR) or TCP `enip` `@identity` read is the
degenerate N=1 case: one `DATA_RECEIVED`, then `READ_COMPLETED`. The record
layout is identical (§1, §8), so a consumer's parsing path does not branch on
transport or unicast/broadcast.

Ordering guarantees:

- `READ_STARTED` fires before any `DATA_RECEIVED`: the read vtable raises it on
  the API thread before signaling the worker to send.
- `DATA_RECEIVED` events are strictly ordered and 1:1 with records (direct,
  serialized under `api_mutex`).
- `READ_COMPLETED` fires last, once, buffer complete. Callers that only want
  the final set ignore `DATA_RECEIVED` and use `READ_COMPLETED`; the two
  consumption models coexist.

## 7. Edge cases

- **No callback:** the direct call is guarded by `if(tag->callback)`; the
  buffer still fills. Read `record_count` after `READ_COMPLETED`.
- **Duplicate replies** (broadcast): deduped by `(source_ip, serial)` before
  append, so a dup fires no event and adds no record.
- **Abort mid-read** (`plc_tag_abort`): the worker stops the recv loop,
  emits no further `DATA_RECEIVED`, and the read terminates with `ABORTED` /
  `READ_COMPLETED(PLCTAG_ERR_ABORT)` per the normal abort path.
- **Destroy mid-read:** the destructor sets the terminate flag and
  **joins** the worker before the generic layer raises `DESTROYED`. This
  guarantees no `DATA_RECEIVED` can fire after `DESTROYED`, and the worker
  never touches a freed buffer.
- **Re-read:** each `plc_tag_read` resets the buffer (new `version`/
  `record_count=0`, cleared dedup set) and re-sends the request (broadcast or
  unicast); the sequence in §6 repeats.

## 8. Buffer header (unchanged from the enip-udp design)

```text
off  field
0    u16  version          (record format version)
2    u16  record_count     (incremented before each DATA_RECEIVED)
4    ... length-prefixed records ...
```

`record_count` is the single source of truth the callback reads, also exposed
as the `record_count` tag attribute. Unicast `enip-udp` and TCP `enip`
`@identity` use this identical layout with `record_count == 1` (§1), so
buffer-parsing code is the same for all three cases.

## 9. Compatibility: TCP `@identity` return format changes

Adopting the shared §8 envelope changes the data an existing new-ENIP TCP
`enip` `@identity` read returns. Today `create_identity_tag` /
`enip_identity_tag_copy` (`client/enip_tag.c`) copy the **raw
Get_Attributes_All payload** into the tag buffer verbatim. Under this design a
TCP `@identity` read instead returns the **version + `record_count` header
followed by one length-prefixed record** (§8), so unicast, broadcast, and TCP
all parse the same way.

This is a breaking change to the buffer layout of that one tag, scoped to the
new ENIP module only:

- **AB and OMRON `@identity` are unchanged** (§ intro) — they are separate
  implementations and keep their current formats.
- The new ENIP module is behind `LIBPLCTAG_FEATURE_ENIP` (experimental/beta),
  so the `@identity` buffer format is not yet a shipped, stable contract —
  this is the moment to change it before it hardens.
- The record body still contains the same identity fields; a caller that
  previously parsed the raw Get_Attributes_All bytes at offset 0 must now skip
  the 4-byte header + the per-record length/address prefix. The identity
  fields inside the record are the §8 layout, not the raw CIP attribute order,
  so field offsets shift too.

Implementation touch points: `enip_identity_tag_copy` wraps the cached payload
in the envelope instead of copying it raw; the `record_count` attribute getter
is added to the `@identity`/`@listidentity` vtable; the shared record encoder
(the same one the UDP worker uses) is the single place that lays out a record
so TCP and UDP cannot drift.

## 10. Open decisions

1. **Event name:** `PLCTAG_EVENT_DATA_RECEIVED` (recommended, general) vs.
   `PLCTAG_EVENT_READ_UPDATED` (reads as "incremental read progress") vs. a
   discovery-specific name. Recommend the general name.
2. **Index in `status`:** keep `status = PLCTAG_STATUS_OK` and have the
   callback read `record_count` (recommended) vs. passing the new record's
   0-based index in `status`.
3. **Coalescing opt-in for slow consumers:** if a callback is slower than the
   arrival rate, direct per-record calls serialize behind it under the lock
   (back-pressure on the worker, which is fine for a bounded scan). No batching
   is proposed; revisit only if a future high-rate streaming tag needs it.

# ENIP metadata, formats, and discovery

Design for how the new-ENIP module presents structured metadata — tag
listings, UDT/struct definitions, identity, and `enip-udp` discovery — to
callers. Three pieces:

1. **A format/schema C API** (§0): the tag's native buffer stays raw; callers
   pull it back in a chosen encoding (`raw`, `cbor`, `json`, …) rendered on
   demand from raw + a schema. `format` is a runtime parameter, not a
   create-time attribute.
2. **`enip-udp` discovery and `@identity`/`@listidentity`** (§ intro, below):
   UDP unicast/broadcast List Identity, sharing the same record model as
   TCP `enip` `@identity`.
3. **Incremental record delivery via the event callback** (§1–§10): when an
   event callback is registered, fire it once per newly received record, with
   the raw buffer updated *before* the callback runs. This document decides
   which event to use and how to deliver it.

The design uses the protocol attribute to note that this is UDP:

'enip-udp' - use UDP transport.  Unicast or broadcast depending on the CIDR subnet specified.

We add to the schema for the gateway.  If it has a CIDR '/<n>' notation, it indicates the subnet for UDP discovery messages, so the gateway knows which network range to broadcast discovery requests to.  If it does not have a CIDR notation or the CIDR subnet is '/32', then this is a unicast UDP message.

`gateway=<ip>(/<cidr>)(:<port>)`

Both the CIDR mask and the port are optional. The port defaults to 44818.  The CIDR mask defaults to '/32', indicating a unicast message if not specified.

We overload the meaning of the '@identity' tag name.  If the protocol is 'enip-udp' _and_ there is a CIDR subnet specified that is not 32, then the '@identity' name means we are doing broadcast discovery for UDP discovery within that subnet.  We do not need an additional new special tag name.

To avoid as much surprise as possible, we introduce the '@listidentity' tag name as an alias of '@identity'.  Users who know CIP well can use '@listidentity' to indicate they are interested in the list identity semantics, while others can continue using '@identity'.  Users who are unaware of the distinction can continue using '@identity' without needing to understand the list identity semantics.

In order to make this easier to use, '@identity' (unicast, broadcast, or TCP) must present its records in the exact same model, so the parsing logic is identical regardless of transport. In the native `raw` format that model is bare length-prefixed records concatenated together (no stored header — see §0 and §8); the version + record-count + records envelope exists only in the structured `cbor`/`json` renderings produced on demand by the format/schema API.

Note that these changes to '@identity' only affect the new ENIP protocol (both UDP and TCP).  The existing AB and Omron implementations of '@identity' are unchanged; this is only for the new ENIP protocol handling.

## 0. Formatted-data and schema C API

Format and schema are fundamental enough to be first-class C API, not attributes
wedged into the create string. `format` is a **runtime parameter** — the same
tag can be pulled back as `PLCTAG_FORMAT_RAW`, `PLCTAG_FORMAT_CBOR`, or (later)
a JSON entry on successive calls. It is an **enum, not a string**: a typo in a
format name is then a compile error, not a silent `PLCTAG_ERR_UNSUPPORTED` at
run time. Path accessors were considered and rejected: with a structured,
self-describing encoding the wrapper language generates its own idiomatic
objects from the CBOR/JSON, so C-level field navigation buys nothing.

```c
typedef enum {
    PLCTAG_FORMAT_RAW = 0,  /* native tag bytes; always supported, no schema needed */
    PLCTAG_FORMAT_CBOR = 1, /* RFC 8949 CBOR, rendered from raw data + schema */
} plc_tag_format_type_t;

/* _size returns size-or-negative-error; get/set return PLCTAG_STATUS_OK or
 * negative, with PLCTAG_ERR_TOO_SMALL when buffer_length is short. */
LIB_EXPORT int plc_tag_get_formatted_data_size(int32_t tag, plc_tag_format_type_t format);
LIB_EXPORT int plc_tag_get_formatted_data(int32_t tag, plc_tag_format_type_t format, uint8_t *buffer, int buffer_length);
LIB_EXPORT int plc_tag_set_formatted_data(int32_t tag, plc_tag_format_type_t format, const uint8_t *buffer, int buffer_length);

/* format here = the encoding of the SCHEMA itself, independent of the data's
 * format (a schema stored as PLCTAG_FORMAT_CBOR can still drive
 * plc_tag_get_formatted_data(tag, PLCTAG_FORMAT_CBOR, ...) for the data --
 * the two format arguments are independent choices that happen to share a type). */
LIB_EXPORT int plc_tag_get_schema_size(int32_t tag, plc_tag_format_type_t format);
LIB_EXPORT int plc_tag_get_schema(int32_t tag, plc_tag_format_type_t format, uint8_t *buffer, int buffer_length);
LIB_EXPORT int plc_tag_set_schema(int32_t tag, plc_tag_format_type_t format, const uint8_t *buffer, int buffer_length);
```

**Build gating.** This subsystem's only implementation today is the new ENIP
module's built-in schemas, so the whole six-function surface is compiled
behind `LIBPLCTAG_FEATURE_ENIP` (the same experimental/beta gate the module
itself builds behind). `libplctag.h` always declares the six functions --
that header is the only publicly installed one (`plctag_features.h` is a
build-internal generated file, never installed), so the symbols exist in
every build for ABI stability. With the feature off, `lib.c` compiles a
`#else` stub for each that returns `PLCTAG_ERR_UNSUPPORTED` unconditionally,
in place of the generic-raw + vtable-dispatch implementation -- including the
protocol-agnostic `PLCTAG_FORMAT_RAW` render, which would otherwise work for
any tag (AB/Modbus/OMRON included) with no ENIP code involved. If a non-ENIP
consumer of the format/schema API turns up later, split the always-available
raw path out from under the gate at that point; until then, one gate covering
the whole subsystem matches its one real consumer.

`format` is coherent across both families: it always names the encoding of
the bytes the call moves — the **data** for `_formatted_data`, the **schema
text** for `_schema`.

**Raw is canonical; structured formats render on demand.** The tag's internal
buffer holds native bytes (wire data, or concatenated metadata records). A
`get_formatted_data(tag, PLCTAG_FORMAT_CBOR, …)` encodes that raw buffer into
CBOR at call time, using the tag's schema; `get_formatted_data(tag,
PLCTAG_FORMAT_RAW, …)` is the identity render (overlaps
`plc_tag_get_raw_bytes`, intentionally — one uniform API for every format, and
raw needs no schema). Nothing structured is ever stored, so there is no
dual-format buffer to keep in sync and no in-place CBOR mutation to manage.

**Schema source.** The library ships **built-in schemas** for ENIP metadata
(identity, tag list, UDT); `get_schema` renders them in the requested
`format`, `set_schema` overrides, and a plain data tag with no built-in
schema must `set_schema` before any structured `get_formatted_data`. Requesting
a structured format with no schema is a **call-time error** (not a create-time
one) — it fails at `get_formatted_data`, the call that actually needs it.

**Built-in schemas shipped so far:** `identity` (§9) and `pccc-file-list` --
`@tags` against a PLC-5/SLC/MicroLogix connection, covering client PCCC
support. PCCC has no symbol-object equivalent to Logix's class 0x6B; instead
it reads the File 0 system directory with an ordinary PCCC word-range read,
per a vendor protocol technical report not independently verified against
real hardware in this tree (see `enip_pccc_build_listing`'s doc comment in
`client/enip_session.c` for the full wire-format reasoning and known
limitations -- notably, MicroLogix is treated uniformly with SLC's 6-byte
directory record; the report's extended 8-byte record for specific newer
MicroLogix variants is not distinguished or supported).

`pccc-file-list` is deliberately **one record shape for every PCCC platform**
(`client/enip_tag.c`'s `pccc_listing_record_cbor_size`/`_write`): `file_number`,
`file_type` (a `pccc_file_t` code -- the same vocabulary
`enip_pccc_addr.c` already uses to parse logical addresses like `N7:0`),
`file_type_name`, `element_count` (when known), and `raw` (the untouched
native record bytes, the escape hatch for platform-specific extras this
schema doesn't name -- PLC-5's attribute byte, SLC/MicroLogix's unexplained
trailing "reserved" field). SLC/MicroLogix carry `file_type` directly on the
wire; PLC-5 has no type-ID byte at all, so its records are decoded first
(`pccc_decode_plc5_file_record`: file-number convention for files 0-2, then
the attribute byte's structure-class nibble + radix bits, corroborated by
word-count divisibility) and mapped onto the same `pccc_file_t` codes before
either platform's record is emitted -- so a caller reading `@tags` against a
PLC-5 and an SLC never sees a different field name for "what type is this
file". One known loss from unifying: PLC-5's own inactive-vs-
unrecognized-structure distinction collapses into the same generic
`PCCC_FILE_UNKNOWN` both platforms already share for "not resolvable"; a
caller that needs that distinction back decodes `raw`'s attribute byte
(bit 6) itself.

Rockwell `@tags`/`@udt` (class 0x6B/0x6C) and OMRON's own enumeration (§7/§8
tasks) do not have CBOR schemas yet -- `PLCTAG_FORMAT_CBOR` on those still
returns `PLCTAG_ERR_UNSUPPORTED`; only `PLCTAG_FORMAT_RAW` works today.

**Symmetry.** `set_formatted_data(…, PLCTAG_FORMAT_CBOR, …)` decodes CBOR→raw
for the wire, so a schema/codec must round-trip both directions. Read-only
tags (identity, tag list, discovery) reject `set_formatted_data`.

**CBOR envelope** (string keys, per decision): a top-level map
`{ "schema": <string>, "schema-version": <int>, "records": [ … ] }`, one
envelope for every metadata kind (`"tag list"`, `"struct definition"`,
`"identity"`, …). `records` is a normal array; its length is the record count.
CDDL (RFC 8610) is the reference language if a written schema of the CBOR itself
is ever wanted, but is not required — the CBOR is self-describing.

**Streaming interaction.** For an incrementally collecting tag (§1–§6), raw tail
concatenation is the only mutation; `get_formatted_data` renders a point-in-time
snapshot whenever called. A structured `get` on a still-collecting tag may race
record arrival — `TOO_SMALL` + required size lets the caller retry.

## 1. Problem

Discovery collects 0..N replies over a `discover_wait_ms` window (see the
`enip-udp` design). A caller with an event callback wants to react to each
device as it appears, not only at the end of the window. Requirements:

1. One callback invocation per new (deduped) record.
2. The tag's raw buffer — the appended record — is consistent and visible
   *before* the callback fires; the library-tracked `record_count` is bumped
   first.
3. A caller with **no** callback still works: it reads `record_count` /
   walks the raw buffer (or calls `get_formatted_data`) after `READ_COMPLETED`.
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
    append raw record to tag->data          /* tail concatenation */
    tag->record_count += 1                  /* library-tracked; buffer consistent */
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
record. A callback that wants the structured form calls `get_formatted_data`;
one that walks raw identifies "what's new" from `record_count`: on the
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
- **Re-read:** each `plc_tag_read` resets the raw buffer (`record_count=0`,
  cleared dedup set) and re-sends the request (broadcast or unicast); the
  sequence in §6 repeats.

## 8. Raw record layout

The native (`format=raw`) buffer is **bare length-prefixed records
concatenated together — no stored header**:

```text
per record:
+0   u16  record_len       (bytes after this field; next record at +2+record_len)
+2   u8   ip[4]            (source octets a.b.c.d — drop into gateway=)
+6   u16  port
+8   u16  vendor_id
+10  u16  device_type
+12  u16  product_code
+14  u8   revision_major
+15  u8   revision_minor
+16  u16  status
+18  u32  serial
+22  u8   state
+23  u8   name_len
+24  ..   product_name[name_len]   (ASCII, not NUL-terminated)
```

All multi-byte fields LE; `ip[4]` as dotted octets. Records are walked one at a
time (same as tag-listing data today). The record **count** is *not* in the
buffer — it is library-tracked and exposed via the `record_count` int
attribute. Unicast `enip-udp` and TCP `enip` `@identity` produce this identical
record with `record_count == 1`, so raw-parsing code is the same for all three.

The version + `schema` + `schema-version` + `records` envelope lives **only** in
the `cbor`/`json` renderings from `get_formatted_data` (§0); it is generated
on demand from these raw records and never stored.

## 9. Compatibility: TCP `@identity` return format changes

The new-ENIP TCP `enip` `@identity` read currently returns the **raw
Get_Attributes_All payload** verbatim (`create_identity_tag` /
`enip_identity_tag_copy`, `client/enip_tag.c`). Under this design its `raw`
buffer instead holds **one §8 record** (identity fields in the §8 layout), so
unicast, broadcast, and TCP share one record model; the structured view comes
from `get_formatted_data(…, PLCTAG_FORMAT_CBOR, …)`.

Breaking change to that one tag's buffer layout, scoped to the new ENIP module
only:

- **AB and OMRON `@identity` are unchanged** (§ intro) — separate
  implementations, current formats kept.
- The new ENIP module is behind `LIBPLCTAG_FEATURE_ENIP` (experimental/beta),
  so the `@identity` buffer layout is not a shipped contract yet — change it
  now, before it hardens.
- A caller that previously parsed the raw Get_Attributes_All bytes at offset 0
  must now read the §8 record layout (or, better, switch to
  `get_formatted_data(PLCTAG_FORMAT_CBOR)` and stop parsing bytes by hand). Field offsets
  differ from raw CIP attribute order.

Implementation touch points: `enip_identity_tag_copy` lays the cached payload
into a §8 record instead of copying it raw; the shared record encoder (the same
one the UDP worker and the CBOR renderer use) is the single place a record is
laid out, so TCP, UDP, and the structured view cannot drift; the identity
built-in schema (§0) drives the CBOR/JSON rendering.

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
4. **First codec scope:** `raw` + `cbor` (identity schema) for the initial
   cut; `json` and the tag-list / UDT schemas follow with their own tasks.

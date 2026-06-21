<!--
  Copyright (C) 2026 by Kyle Hayes - kyle.hayes@gmail.com
  This software is available under the MPL 2.0 or LGPL 2 (or later) license.
-->

# Rockwell (Logix / Micro800) — dialect-specific design

This document specifies everything about the Rockwell EtherNet/IP dialect that is
**not** common to all CIP devices. The common machinery — IO thread, scheduler,
rc/lifetime, locking, EIP/CPF framing, the OPEN_PROBE → OPEN_BULK → READ/WRITE
windowed state machine, and the `0x0A` Multiple Service batch — lives in
[ENIP-SESSION-DESIGN.md](ENIP-SESSION-DESIGN.md) and is reused verbatim. Read
§16a ("Common vs. dialect-specific") of that document first; this file only fills
in the Rockwell `enip_dialect_t` hooks and the Rockwell-only enumeration
subsystem.

"Rockwell" here covers ControlLogix / CompactLogix (Logix) and Micro800. They
share one dialect; the few differences between them are expressed as numbers, not
branches (§2).

---

## 1. What Rockwell shares with the common code (do not re-implement)

The current generic implementation (ENIP-SESSION-DESIGN.md §16) was built and
tested against a Logix L81E, so the entire fits-the-buffer path is already the
Rockwell path:

- **Symbol path encoding** — `0x91`/`0x28`/`0x29`/`0x2A` segments
  (`enip_cip_encode_path`).
- **Non-fragmented Read/Write** — Read Tag `0x4C` / Write Tag `0x4D`
  (`enip_cip_read` / `enip_cip_write`).
- **Reply framing** — 2-byte atomic header (`Cx 00`) or 4-byte abbreviated
  structure header (`A0 02 <template_handle>`), parsed by the shared
  `enip_type_decode` and replayed verbatim on write via the captured
  `type_header[]`.
- **Element-granular windowing** — whole-element windows `[off .. off+n]` for
  arrays larger than the buffer.
- **`0x0A` Multiple Service batch** — Logix supports it; Micro800 does not (§2).

Rockwell needs custom code in only three places: the connection-size *numbers*
(§2), byte-granular fragmentation for oversized single elements (§3), and tag/UDT
enumeration (§5).

---

## 2. Connection bring-up

### 2.1 Large Forward Open and the try/fallback

Newer Logix and Micro800 support Large Forward Open and large buffers; older
ControlLogix support only the standard Forward Open and ~500-byte buffers. This
is **not** modeled with a per-model capability flag. The common code always
attempts Large Forward Open (service `0x5B`); if the target replies with CIP
status `0x08` (Service Not Supported), it falls back to the standard Forward Open
(service `0x54`) once, for the life of the connection. See ENIP-SESSION-DESIGN.md
§16a. Old ControlLogix take the fallback; everything else keeps Large FO.

```
CONN_OPEN:
    build Large Forward Open (0x5B) with requested_cip_size
    -> CONN_SENDING (resume CONN_OPEN)

on_open_reply:
    if cip_status == 0x08 (Service Not Supported):
        requested_cip_size = min(requested_cip_size, 504)
        rebuild standard Forward Open (0x54); retry once
    elif cip_status == 0:
        max_cip_packet_size = granted size from reply   /* not the requested! */
    else:
        reset_connection
```

### 2.2 Requested connection size (the only per-model numbers)

| model | typical max CIP payload | notes |
|---|---|---|
| ControlLogix/CompactLogix, older | ~500 | standard FO only; takes the §2.1 fallback |
| ControlLogix/CompactLogix, newer | ~4000 | Large FO |
| Micro800 | up to ~64000 | Large FO; **no `0x0A`** (§2.3) |

`requested_cip_size` is a dialect *number*. The **granted** size from the Forward
Open reply sets `max_cip_packet_size`.

### 2.3 Micro800 has no Multiple Service Packet

Micro800 does not support the `0x0A` aggregate service. This is expressed as a
*number*, not a bool: `max_batch_cap = 1`. The common `pick_batch` already routes
a one-tag "batch" through the single-in-flight path (the `count == 1` case), so a
cap of 1 disables batching with **zero** new branches in common code. Logix sets
`max_batch_cap = ENIP_MAX_BATCH`.

### 2.4 ForwardOpen wire layout

The connected-path ForwardOpen is already implemented (`build_forward_open` in
`enip_session.c`). The standard variant (`0x54`) is what ships today; the Large
variant (`0x5B`) differs only in:

- service byte `0x5B` instead of `0x54`;
- the O→T and T→O network connection parameters are **32-bit** fields (allowing
  sizes > 511) instead of the 16-bit `0x43F8`-style words.

Everything else (connection path = route + Message Router class/instance,
priority/tick, connection ids, RPI) is identical.

---

## 3. Fragmentation — Read/Write Tag Fragmented (`0x52` / `0x53`)

Needed only when a **single element exceeds the usable buffer** (e.g. a 1128-byte
UDT instance on a ~500-byte old ControlLogix, or any element on a small
connection). Uncommon; **not** an MVP feature. Specified here in full.

Unlike OMRON (which rides a path segment), Rockwell uses dedicated fragmented
*services* and signals "more to come" with a general status of `0x06`.

### 3.1 Read Tag Fragmented (`0x52`)

```
+------+-------------+----------------+----------------+----------------+
| 0x52 | path_words  |   path (IOI)   | count (u16 LE) | offset(u32 LE) |
+------+-------------+----------------+----------------+----------------+
 svc    path size       symbol+index    element count    BYTE offset
        (words)                         (whole tag)       into the tag
```

- `count` is the **element** count for the whole tag (not the fragment).
- `offset` is the **byte** offset of this fragment into the tag's data.
- Reply: general status `0x06` (partial transfer) means "data attached, more
  remains"; `0x00` means "this is the last fragment"; anything else is an error.
  The first reply carries the type header (`Cx 00` or `A0 02 <handle>`), exactly
  as a normal read.

### 3.2 Write Tag Fragmented (`0x53`)

```
+------+------------+----------+-------------+----------------+----------------+--------+
| 0x53 | path_words |  path    | type_header | count (u16 LE) | offset(u32 LE) |  data  |
+------+------------+----------+-------------+----------------+----------------+--------+
```

- `type_header` is the captured 2- or 4-byte type/handle, replayed verbatim.
- `count` = whole-tag element count; `offset` = byte offset of this chunk.

### 3.3 Read loop (pseudocode)

`usable = max_cip_packet_size - CIP_CONNECTED_ITEM_OVERHEAD - CIP_READ_REPLY_OVERHEAD
        - type_header_len` (the `0x52` header adds only the fixed 6 bytes of
count+offset to the request, which is smaller than the reply budget, so the reply
budget governs).

The `chunk` is aligned to the element's largest-scalar boundary per the common
rule in ENIP-SESSION-DESIGN.md §16a.6 (`chunk = floor(usable / frag_align) *
frag_align`); atomic scalars are never byte-fragmented, so this runs only for an
oversized structure/string. The shared planner computes `(offset, chunk)`; the
`0x52`/`0x53` builder just places them in the count/offset fields.

```
total = elem_size * elem_count
frag  = t->frag_offset           /* byte cursor, starts at 0 */

build (ENIP_OP_READ / OPEN_* in byte-fragment mode):
    emit 0x52: path, count = elem_count, offset = frag

apply (reply Bytes):
    parse CIP reply
    if status != 0x00 and status != 0x06: error
    if first fragment: capture type_header; strip it
    else: strip nothing (continuation fragments carry no type header)
    copy payload into t->data[frag ..]
    frag += payload.len
    *more = (status == 0x06)        /* Rockwell's explicit "more" signal      */
    /* sanity: when status==0x00, frag should == total                        */
```

The key difference from OMRON: `*more` comes from the **`0x06` status**, not from
comparing the cursor to a known total. The driver still tracks `frag` to place
the bytes, but the device decides when it is done.

> **Important — only the first fragment carries the type header.** Logix
> prepends the 2/4-byte type header to the *first* fragment's data only;
> continuation fragments are raw element bytes. `apply` must strip the header on
> the first fragment and not on the rest.

### 3.4 Write loop

Symmetric with `0x53`. Walk `frag` by `usable`-sized chunks, sending
`count = elem_count`, `offset = frag`, and `data = t->data[frag .. frag+chunk]`,
with the type header on every write request (it is part of the `0x53` request
format, unlike the read reply). Completion is a single `0x00` status on the last
chunk.

---

## 4. Multi-dimensional array linearization

Identical need and identical helpers as OMRON (and shared, not per-vendor):
ported from `src/tools/ab_server/cip.c:1217`, row-major, dimensions as CIP DINT
(`int32_t dimensions[3]`). Used to compute the byte `offset` for a
partially-indexed element and to emit the correct `0x28` index segments when
windowing a multi-dimensional array. See OMRON-SPECIFIC-DESIGN.md §4 for the
formulas; the code is in the common layer.

---

## 5. Tag and UDT enumeration (separate subsystem — build last)

Not needed for named read/write (the probe supplies size and the type header).
Needed only for `@tags`-style listing and UDT member decode. Self-contained
module behind the dialect's `list_tags` hook. Rockwell uses the **same class
numbers** as OMRON (`0x6B` symbol/variable, `0x6C` template/type) but completely
different services and attribute layouts — nothing here is shared with OMRON.

### 5.1 Symbol enumeration — class `0x6B`

- Service **`0x55`** (GetInstanceAttributeList) on class `0x6B`, instance
  `0x0000`, requesting attributes 1 (symbol name) and 2 (symbol type).
- Reply is a stream of `instance_id(u32 LE) || name_len(u16 LE) || name || type(u16 LE)`
  records; the reply is paged — a general status `0x06` means "more instances,
  re-request starting at last_instance_id + 1".
- The 2-byte symbol `type` encodes: atomic type code, or a structure bit
  (`0x8000`) plus a template instance id in the low bits; array dimension bits
  (`0x6000`) give the dimension count. (This is the classic libplctag Logix tag
  listing format — see `attic/` for the legacy decoder.)

### 5.2 Template (UDT) decode — class `0x6C`

For each structure type discovered in §5.1:

1. **Template attributes:** GetAttributeList (service `0x03`) on class `0x6C`,
   instance = template id, attributes 4 (definition size, in 32-bit words),
   5 (structure size in bytes), 2 (member count), 1 (structure handle / CRC).
2. **Template definition:** Read Template service **`0x4C`** on class `0x6C`,
   instance = template id, with an offset/byte-count pair in the request data —
   itself fragmented exactly like §3 (templates are usually larger than the
   buffer). The definition is: a member-info array (per member: `type(u16)`,
   `info(u16)`, `offset(u32)`), then a NUL-delimited string blob containing the
   structure name (terminated by `;`) followed by each member name.

This is materially more involved than OMRON's recursive instance walk and is the
single largest Rockwell-specific component; build it last, behind `list_tags`.

---

## 6. Rockwell `enip_dialect_t` instances

```c
static const enip_dialect_t ENIP_DIALECT_LOGIX = {
    .name              = "logix",
    .requested_cip_size = 4000,         /* newer; old units take §2.1 fallback */
    .max_batch_cap     = ENIP_MAX_BATCH,/* Logix supports 0x0A                 */
    .build             = rockwell_build,
    .apply             = rockwell_apply,
    .list_tags         = rockwell_list_tags,  /* §5; NULL until built          */
};

static const enip_dialect_t ENIP_DIALECT_MICRO800 = {
    .name              = "micro800",
    .requested_cip_size = 64000,
    .max_batch_cap     = 1,             /* §2.3: no Multiple Service Packet     */
    .build             = rockwell_build,
    .apply             = rockwell_apply,
    .list_tags         = rockwell_list_tags,
};
```

`rockwell_build` / `rockwell_apply` call the shared `enip_build_symbolic` /
`enip_apply_symbolic` for the fits-the-buffer case and use the `0x52`/`0x53`
fragmented path (§3) only when the tag is in byte-fragment mode
(`elem_size > usable`). The mode test is generic; the `0x06`-status handling is
the dialect's. Logix and Micro800 differ only in the two numbers, exactly as
intended — no `if(micro800)` anywhere.

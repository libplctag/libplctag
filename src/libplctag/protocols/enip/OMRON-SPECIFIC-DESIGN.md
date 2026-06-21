<!--
  Copyright (C) 2026 by Kyle Hayes - kyle.hayes@gmail.com
  This software is available under the MPL 2.0 or LGPL 2 (or later) license.
-->

# OMRON NJ/NX (N-series) — dialect-specific design

This document specifies everything about the OMRON EtherNet/IP dialect that is
**not** common to all CIP devices. The common machinery — IO thread, scheduler,
rc/lifetime, locking, EIP/CPF framing, the OPEN_PROBE → OPEN_BULK → READ/WRITE
windowed state machine, and the `0x0A` Multiple Service batch — lives in
[ENIP-SESSION-DESIGN.md](ENIP-SESSION-DESIGN.md) and is reused verbatim. Read
§16a ("Common vs. dialect-specific") of that document first; this file only
fills in the OMRON `enip_dialect_t` hooks and the OMRON-only enumeration
subsystem.

The reference implementation studied for this design is the `aphytcomm` Python
library (`~/Projects/aphytcomm`, `src/aphyt/omron/n_series.py`,
`src/aphyt/cip/cip.py`) plus the two Node.js libraries
`omron-ethernet-ip` and `omron-eip`.

---

## 1. What OMRON shares with the common code (do not re-implement)

These are byte-for-byte identical to the generic symbolic-CIP path and need no
OMRON code at all:

- **Symbol path encoding.** OMRON uses the same ANSI extended-symbol segment
  `0x91 len name [pad]` and the same array-index segments `0x28 / 0x29 / 0x2A`
  as Logix (`variable_request_path_segment` in `cip.py`). `enip_cip_encode_path`
  is shared.
- **Non-fragmented Read/Write.** OMRON uses Read Tag (`0x4C`) and Write Tag
  (`0x4D`) for any access that fits the connection buffer — not a vendor
  service. `enip_cip_read` / `enip_cip_write` are shared.
- **Reply framing.** OMRON's CIP Common Format puts an atomic on the wire as
  `<type_lo> <type_hi> <data…>` (e.g. an INT is `C3 00 …`), i.e. the 2-byte
  atomic header the shared `enip_type_decode` already parses; and a structure as
  `A0 02 <crc16_lo> <crc16_hi> <data…>` — the *same four bytes* as a Logix
  abbreviated-structure header, except the 2-byte handle is a CRC16 rather than
  a template handle. The shared probe captures those four bytes as
  `type_header[]` and replays them verbatim on write, so OMRON structure writes
  need **no** CRC computation and **no** special code.
- **Element-granular windowing.** Arrays larger than the buffer are read/written
  as whole-element windows `[off .. off+n]` by the common state machine. OMRON
  needs custom code **only** when a *single element* exceeds the buffer (§3).

The practical consequence: an OMRON tag whose elements fit the negotiated buffer
works through the existing generic path with zero OMRON-specific code beyond
dialect selection and Large Forward Open (which OMRON also supports — §2).

---

## 2. Connection bring-up

### 2.1 Large Forward Open

OMRON NJ/NX support Large Forward Open. This is **not** modeled as an OMRON
capability flag — the common code always attempts Large Forward Open and falls
back on a `0x08` (Service Not Supported) CIP status (see ENIP-SESSION-DESIGN.md
§16a, "capabilities are numbers, try/fallback, or function pointers, never
vendor bools"). OMRON simply succeeds at the Large FO attempt.

### 2.2 Requested connection size

`enip_dialect_omron.requested_cip_size` is the only OMRON-specific *number* for
bring-up. Typical granted sizes by model family:

| model | typical max CIP payload |
|---|---|
| NJ/NX older | ~1444 |
| NJ/NX newer | ~1900 |
| large/newer NX | ~8000 |

Request the family default (overridable by the `max_packet_size` / connection
attribs). The **granted** size from the Forward Open reply sets
`max_cip_packet_size`; never assume the requested size was honored.

### 2.3 Status code dictionary

OMRON returns a rich set of general/extended status codes (see
`cip_status_dictionary` in `cip.py`). The dialect's `apply` maps them; the ones
that matter for the data path:

| status | meaning | dialect action |
|---|---|---|
| `0x00` | success | normal |
| `0x06` | **not used by OMRON** (this is the Rockwell partial-transfer marker) | n/a |
| `0x11` | `REPLY_DATA_TOO_LARGE` | should not happen — we right-size; treat as error |
| `0x80 0x05`-style extended | various internal/segment errors | map to `PLCTAG_ERR_REMOTE_ERR`, log ext status |

OMRON never signals "more data to come" via status — fragmentation progress is
driven entirely by the offset cursor the driver itself maintains (§3).

---

## 3. Fragmentation — Simple Data Segment (`0x80`)

This is the one piece of OMRON data-path code that has no Logix equivalent in
shape. It is needed only when a **single element exceeds the usable buffer**
(e.g. an 8 KB UDT instance on a 1900-byte connection). This is uncommon and is
**not** an MVP feature, but it is specified here in full.

### 3.1 Mechanism

OMRON does not use a fragmented *service*. It uses the ordinary Read/Write Tag
service and appends a **Simple Data Segment** to the request path that carries
the byte offset and the byte count. The driver itself walks the offset from 0 to
the variable size; there is no "partial transfer" status to react to.

### 3.2 Simple Data Segment wire format

Appended to the symbolic path, after all `0x91`/`0x28` segments:

```
+------+------+------------------+------------+
| 0x80 | 0x03 |  offset (u32 LE) | size (u16) |   8 bytes total
+------+------+------------------+------------+
  type   len    byte offset        byte count
         (words: 0x03 = 3 words = the 6 bytes that follow)
```

- `0x80` — Simple Data Segment type.
- `0x03` — segment length in **words** of the payload that follows (offset(4) +
  size(2) = 6 bytes = 3 words). Fixed.
- `offset` — **byte** offset into the variable's data.
- `size` — number of **bytes** requested in this fragment.

So the full read request path is `variable_path || simple_data_segment`, and the
service stays `0x4C` (read) / `0x4D` (write).

### 3.3 Read loop (pseudocode)

`usable = max_cip_packet_size - CIP_CONNECTED_ITEM_OVERHEAD - CIP_READ_REPLY_OVERHEAD
        - type_header_len - SIMPLE_DATA_SEG_LEN(8)` — i.e. the same accounting as
the common windowing math, with the 8-byte segment added to overhead. (aphytcomm
uses `MAXIMUM_LENGTH - 8` as a conservative stand-in.)

The `chunk` size below is **not** simply `min(usable, total - frag)`: it must be
aligned to the element's largest-scalar boundary per the common rule in
ENIP-SESSION-DESIGN.md §16a.6 (`chunk = floor(usable / frag_align) * frag_align`,
final fragment runs to `total`). Atomic scalars are never byte-fragmented; this
path only runs for an oversized structure/string element. The shared fragment
planner computes `(offset, chunk)`; OMRON only encodes them into the `0x80`
segment.

```
total = elem_size * elem_count          /* whole-tag byte size            */
frag  = t->frag_offset                  /* byte cursor, starts at 0       */

build (ENIP_OP_READ / OPEN_*):
    chunk = min(usable, total - frag)
    path  = symbol_path || simple_data_segment(offset = frag, size = chunk)
    emit Read Tag (0x4C) with that path, count = 1   /* count is elements; whole var */

apply (reply Bytes):
    parse CIP reply; status != 0 -> error (no 0x06 special case)
    strip CIP Common Format: data_type + addl_info_len + addl_info, leaving payload
    if first fragment: type_header = data_type(+addl_info)   /* as today        */
    if CIPString element: first 2 payload bytes are a length -> skip them
    copy payload into t->data[frag ..]
    frag += payload.len
    *more = (frag < total)
```

Set `t->frag_offset = frag` between round trips. The common state machine's
existing `CONN_WAITING → build → CONN_SENDING` continuation loop (§5, §11.2 of
the main doc) drives this with no new states — byte-fragment mode is just a
second cursor regime alongside element windowing, selected generically when
`elem_size > usable`.

### 3.4 Write loop

Symmetric, with `0x4D` and a conservative `max_write_size` (aphytcomm uses 400).
The request data is `CIPCommonFormat(type_header, data_chunk)`; for an
abbreviated structure the `additional_info` is the 2-byte CRC handle captured at
probe (replayed, never recomputed). The Simple Data Segment in the path carries
the destination byte offset.

### 3.5 Notes / caveats

- The `0x80` segment count `size` is **bytes**, while the Read Tag `count` field
  remains **elements** (1 for a whole oversized variable). Do not conflate them.
- OMRON packed/variable-length strings break the fixed-`elem_size` assumption;
  for a `CIPString` the leading 2-byte length in each fragment must be stripped
  (§3.3). This is the only place element layout is not fixed-width, and it is
  confined to the dialect's `apply`.

---

## 4. Multi-dimensional array linearization

Needed by §3 to turn a partially-indexed array element (e.g. `Arr[2,3]`) into the
flat **byte** offset the Simple Data Segment addresses, and to turn a flat
element offset back into per-dimension `0x28` index segments when windowing a
multi-dimensional array.

These helpers are **common, not OMRON-specific** (ported from
`src/tools/ab_server/cip.c:1217`), and live in the shared layer; OMRON's `build`
calls them. Row-major:

```
linear(idx[], dim[], n):
    n==1: idx[0]
    n==2: idx[0]*dim[1] + idx[1]
    n==3: idx[0]*dim[1]*dim[2] + idx[1]*dim[2] + idx[2]

delinear(linear, dim[], n) -> idx[]:   /* inverse, for emitting index segments */
    for k = n-1 down to 0:
        idx[k] = linear mod dim[k];  linear /= dim[k]
```

Dimensions are CIP **DINT** (`int32_t dimensions[3]`, `uint8_t num_dimensions`),
sourced from the tag attribs / the Variable Object (§5.2).

---

## 5. Tag and UDT enumeration (separate subsystem — build last)

Enumeration is the largest OMRON-specific piece and is **not needed for named
read/write** (the probe supplies element size and the type header). It is needed
only for listing tags (`@tags`-style) and decoding UDT member layouts. It is a
self-contained module behind the dialect's `list_tags` hook and recurses through
OMRON's vendor CIP classes. Logix uses the same class *numbers* with entirely
different services and attribute layouts — see ROCKWELL-SPECIFIC-DESIGN.md — so
none of this is shared.

### 5.1 Tag Name Server / instance list

- **Variable count:** GetAttributeAll on class `0x6A`, instance `0x0000`;
  `reply_data[2:4]` (u16 LE) is the number of variables.
- **Instance list:** GetInstanceListEx2 service `0x5F` on class `0x6A`,
  instance `0x0000`. Request data:
  `start_instance(u32 LE) || count(u32 LE) || kind(u16 LE)` where
  `kind = 2` for user variables, `1` for system variables.

  Reply data layout:

  ```
  instance_count (u16 LE)
  status_byte    (u8)   /* reply_data[2]; 0 => no more after this batch */
  reserved       (u8)
  repeated instance records:
      data_length (u16 LE)          /* of class_id+instance_id+name block */
      class_id    (u16 LE)
      instance_id (u32 LE)
      name_length (u8)
      name        (name_length bytes, UTF-8)
  ```

  Walk batches of ~100, advancing `start_instance += instance_count`, until the
  status byte is 0 or `instance_count == 0`. (See
  `_get_instance_list_subset`.)

### 5.2 Variable Object — class `0x6B` (per-variable properties)

GetAttributeAll on class `0x6B`, instance = the variable's instance id. Reply:

```
size                (u32 LE)   /* bytes in memory                          */
cip_data_type       (u8)       /* reply_data[4]                            */
cip_data_type_array (u8)       /* element type if this is an array         */
array_dimension     (u8)       /* reply_data[6]; 1 byte pad follows        */
(pad to offset 8)
number_of_elements[array_dimension] (u32 LE each, from offset 8)
... bit_number (u8) at 16 + array_dimension*4 ...
variable_type_instance_id (u32 LE) at 20 + array_dimension*4   /* -> 0x6C  */
start_array_elements[array_dimension] (u32 LE) at 24 + array_dimension*4
```

This gives the array shape (dimensions are DINT — see §4) and a pointer
(`variable_type_instance_id`) into the Variable **Type** Object for UDTs.

### 5.3 Variable Type Object — class `0x6C` (UDT/type definitions)

GetAttributeAll on class `0x6C`, instance = `variable_type_instance_id`. Reply:

```
size_in_memory      (u32 LE)
(reserved byte at 4)
cip_data_type       (u8)  at 5
cip_data_type_array (u8)  at 6
array_dimension     (u8)  at 7
number_of_elements[array_dimension] (u32 LE each, from offset 8)
number_of_members   (u16 LE) at 8 + array_dimension*4
... 
crc_code            (u16 LE) at 14 + array_dimension*4   /* the struct handle */
var_type_name_length(u8)     at 16 + array_dimension*4
var_type_name       (name_length bytes) at 17 + array_dimension*4
pad                 (1 byte if name_length even, else 0)
next_instance_id    (u32 LE)   /* iterate sibling members                   */
nesting_variable_type_instance_id (u32 LE)  /* recurse for nested UDT member */
start_array_elements[array_dimension] (u32 LE each)
```

- `crc_code` is the 2-byte handle that appears in the `A0 02 <crc>` structure
  header on the wire — consistent with what the probe captures for writes (§1).
- Member enumeration walks `next_instance_id` until it reaches 0
  (`_structure_instance_from_variable_type_object`, loop
  `while member_instance_id != 0`); nested UDT members recurse through
  `nesting_variable_type_instance_id`.
- The CRC16 (poly `0xA001`, `cip_crc16` in `cip.py`) is only needed if the
  driver ever constructs a structure handle from a type name itself. Because we
  capture the handle from the probe reply, the driver does **not** need to
  compute CRCs for the read/write path — CRC16 is enumeration-only and optional.

---

## 6. OMRON `enip_dialect_t` instance

```c
static const enip_dialect_t ENIP_DIALECT_OMRON = {
    .name              = "omron-njnx",
    .requested_cip_size = 1900,        /* family default; attrib-overridable  */
    .max_batch_cap     = ENIP_MAX_BATCH,/* OMRON supports 0x0A Multiple Service */
    .build             = omron_build,   /* symbolic helper, or §3 frag path     */
    .apply             = omron_apply,   /* symbolic helper, or §3 frag path     */
    .list_tags         = omron_list_tags, /* §5; NULL until built               */
};
```

`omron_build` / `omron_apply` are thin: for the common (fits-the-buffer) case
they call the shared `enip_build_symbolic` / `enip_apply_symbolic`; only when the
tag is in byte-fragment mode (`elem_size > usable`) do they encode/parse the
Simple Data Segment per §3. The byte-fragment-mode test is generic, not a vendor
branch.

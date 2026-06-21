> **DEPRECATED.** This document predates the current EtherNet/IP design and is
> retained for historical reference only. The authoritative design is
> [ENIP-SESSION-DESIGN.md](../src/libplctag/protocols/enip/ENIP-SESSION-DESIGN.md). Do not use this document to guide new work.

# ENIP Implementation Execution Plan — Part 2 (Full-Path Metadata, UDT Cache, Shared Storage, Auto-Sync)

**Date:** 2026-06-07
**Status:** Authoritative for the work that follows Phase 9 of the original plan.
**Companion docs:**
- `docs/ENIP-IMPLEMENTATION-EXECUTION-PLAN.md` — Part 1 (Phases 0–9, the engine,
  framing, transaction seam, registry, strategies). Read it first; this document
  assumes its structures and rules (§ references below point into it).
- `src/libplctag/protocols/enip/TAG-CREATION.md` — the design rationale for
  everything here. This plan is the *how*; TAG-CREATION.md is the *why*.

---

## A. Why there is a Part 2

Part 1 (Phases 0–9) shipped a working data path for one shape of tag: a **bare
root symbol** or a **fully-indexed array** of a base CIP type. It made three
simplifying decisions that the production design must reverse:

1. **Root-only resolution.** Part 1 resolves a tag name to a root symbol's
   instance id and single-instance attributes (type/size/dims). It cannot
   resolve a path that descends below the root (`a.b`, `a[i].b`, `a[i].b[j].c`).
2. **No connection-level metadata cache.** Part 1 (§4.4, §11.7) deliberately
   keeps no shared metadata; each tag fetches and stores its own type inline in
   `enip_tag_meta_t` (§3.3).
3. **No UDT awareness.** A tag that is, or contains, a UDT has no way to learn
   the UDT's member layout.

Part 2 adds, in dependency order:

- **Shared, refcounted, offset-addressed metadata blocks** (Phase 10) — the
  storage primitive everything else builds on.
- **Connection-level full-shape cache + recursive UDT cache** (Phase 11).
- **Full-path resolution + path/elem_count validation** (Phase 12).
- **Auto-sync read/write** (Phase 13).
- **Special tags `@connection` / `@identity` / `@raw`** (Phase 14).
- **Per-tag metadata as a JSON string attribute** (Phase 15).

Each phase ends with a clean compile and a stated acceptance test, exactly like
Part 1. Do them in order; later phases depend on earlier ones.

### A.1 What does *not* change

- The engine loop, queue, transaction seam, framing, budget/packing math, and
  the manufacturer vtable shape (Part 1 §5–§9, §15) are untouched.
- The single-allocation-per-tag rule and "zero allocation in steady state" hold.
  All new allocation happens during the CONNECTING bootstrap and during phase-2
  resolution; a tag that has reached `ENIP_META_READY` does zero-allocation I/O.
- **Manufacturer isolation (Part 1 §9) is absolute, including metadata.** *All*
  metadata **retrieval** — what CIP services/classes to query, how to parse the
  reply, how to discover and read UDT/structure definitions — is
  manufacturer- **and model-specific** and lives behind the strategy vtable. The
  shared layer never issues a metadata query and never parses a device reply. It
  owns only the manufacturer-neutral pieces: the block **format** (§B.1), the
  caches and their generation lifecycle (§B.3), and the path-walk/validation over
  an **already-built** block (Phase 12). Device CIP specifics named in this plan
  (Symbol object, Template object 0x6C, ReadTemplate, etc.) are **examples of an
  AB/Logix implementation of a hook**, not shared code.
- `tag->data` / `tag->size` ownership and the generic-API contract (Part 1 §2)
  are unchanged: metadata remains the source of truth for type, and the tag
  layer writes the derived total size through to `tag->size`.

### A.2 Carried-over invariant additions

These refine Part 1 rules; apply them everywhere from Phase 10 on:

- **Default extent is a single element.** When a name carries no array index and
  no `elem_count` attribute, the tag addresses element 0 with `elem_count = 1`.
  (Part 1 implicitly read the whole array; this is a deliberate change — see
  TAG-CREATION.md "Path and elem_count validation".)
- **Gateway carries the port.** The TCP port is parsed from the `gateway` string
  as `host:port` (default 44818). There is no separate `port` attribute. If
  Part 1 left a `port` attribute path, retire it.
- **Generation discards caches.** A `metadata_generation` bump (reconnect) drops
  the connection's refs on every cached shape/UDT block; they re-resolve lazily.

---

## B. Data-model changes (apply incrementally across the phases)

### B.1 The shared metadata block — `enip_meta_block_t`

One immutable, reference-counted, **offset-addressed** allocation. It holds
either a root symbol's complete level-set (members, dimensions, element sizes,
child references) or a single UDT definition. There are **no internal pointers**:
every cross-reference inside the block is a byte offset or index, so the block
can be accumulated in an arena during resolution and then emitted as one
`rc_alloc` copy without pointer fixups, and shared freely once frozen.

```c
/* enip_meta.h — engine + metadata + strategy only, not the app path. */

/* A node within a block: one symbol level (root, member, or UDT field). */
typedef struct enip_meta_node_t {
    uint16_t data_type;        /* NATIVE device type code (e.g. CIP 0x00C4); I/O only */
    uint8_t  generic_type;     /* enip_generic_type_t — the PRESENTED type (§B.5)     */
    uint8_t  num_dims;         /* 0 = scalar, 1..3                                   */
    uint8_t  flags;            /* ENIP_META_F_IS_UDT, ENIP_META_F_IS_ARRAY, ...      */
    uint32_t array_dims[3];    /* element counts per active dimension                */
    int32_t  elem_size;        /* bytes per element of THIS node's type             */
    uint32_t byte_offset;      /* offset of this member within its parent's data    */
    uint32_t udt_id;           /* template/UDT id if IS_UDT, else 0                 */
    uint32_t first_child_off;  /* block-relative offset to first child node, or 0   */
    uint32_t next_sibling_off; /* block-relative offset to next sibling node, or 0  */
    uint32_t name_off;         /* block-relative offset to this node's name bytes   */
    uint32_t type_name_off;    /* offset to struct/UDT type name (informational), 0 */
} enip_meta_node_t;

typedef struct enip_meta_block_t {
    /* rc_alloc-managed; refcount lives in the rc header, not here. */
    int32_t  generation;       /* conn->metadata_generation it was built under       */
    uint32_t total_bytes;      /* size of the whole emitted block                    */
    uint32_t root_node_off;    /* offset to the root node (0 for a UDT-only block)   */
    /* tail: packed enip_meta_node_t[] and NUL-terminated name bytes, all addressed
     * by the *_off fields above. */
} enip_meta_block_t;
```

Rules:

- **Immutable after emit.** Once a block is frozen and published into a cache, it
  is never mutated. Sharing is therefore lock-free for readers.
- **Refcounted lifetime.** `rc_alloc`/`rc_inc`/`rc_dec` (Part 1 §7.2). The
  connection cache holds one ref; each tag that resolved against the block holds
  one ref. On a generation bump the connection drops its ref; the block frees
  when the last tag releases it. This is what makes pointer-sharing safe across
  reconnect (TAG-CREATION.md "Metadata storage and sharing").
- **Offsets, never pointers.** Validate during emit that every `*_off` lands
  inside `[0, total_bytes)`.

### B.2 `enip_tag_meta_t` becomes a reference, not a copy

Replace the inline type fields of Part 1 §3.3 with a reference into the shared
block plus the few scalars the generic getters touch on the hot path (so value
get/set never has to walk the block or take a ref on the caller thread):

```c
typedef struct enip_tag_meta_t {
    /* shared shape; holds an rc ref while the tag points at it. NULL until resolved. */
    enip_meta_block_t *shape;
    uint32_t node_off;        /* offset of the *addressed* node within `shape`        */

    /* addressing of the final element (built from the name at create time)           */
    uint32_t instance_id;     /* resolved leaf instance id for the request path       */
    int32_t  elem_count;      /* requested element count (default 1)                   */
    uint32_t linear_start;    /* linear element offset of the first addressed element  */

    /* hot-path scalars copied once at resolve (immutable for tag life)               */
    int32_t  elem_size;       /* bytes per addressed element                          */
    uint16_t data_type;       /* bare CIP type of the addressed element               */

    int32_t  generation;      /* generation `shape` was resolved under                */
    uint8_t  state;           /* enip_meta_state_t (Part 1 §3.3)                      */
} enip_tag_meta_t;
```

The validity gate (Part 1 §3.3) gains one clause:

> A tag's metadata is **usable** iff `state == ENIP_META_READY` **and**
> `generation == conn->metadata_generation` **and** `shape != NULL`.

On a generation mismatch the tag must `rc_dec(shape)`, NULL it, and re-resolve.

### B.3 Connection gains two caches (reverses Part 1 §4.4)

```c
/* added to enip_connection_t (Part 1 §4.3) */
hashtable_p root_shape_cache;   /* hash(root_name) -> enip_meta_block_t* (rc held)   */
hashtable_p udt_cache;          /* hash(udt_id)    -> enip_meta_block_t* (rc held)   */
mutex_p     shape_cache_mutex;  /* guards both caches                                 */
```

The phase-1 `root_symbol_cache` (name → instance_id, Part 1 §4.4) stays as is;
it is the cheap first hop. The new `root_shape_cache` holds the lazily-fetched
full shape; `udt_cache` holds UDT definitions shared across roots. **Both caches
are shared infrastructure; what goes into them is produced by the strategy
(§B.4).**

### B.4 New strategy vtable hooks (extends Part 1 §9)

Because retrieval is manufacturer/model-specific, add two hooks to
`enip_mfg_ops_t`. The shared layer calls them and stores/caches what they return;
it never inspects how they got it.

```c
/* ---- metadata retrieval (act on a connection; device-specific) ---- */

/* Build the COMPLETE shape of one root symbol: query whatever the device needs
 * (e.g. AB: Symbol instance attrs 2/7/8, descend structures), assemble nodes
 * into `arena` via the enip_meta builder (§B.1), and emit one block. The shared
 * caller publishes the result into root_shape_cache and owns its lifetime.
 * Return NULL + set status on failure. May call back into resolve_udt (below)
 * for member UDTs, or leave UDT members marked so the shared walker lazy-loads
 * them on demand — the strategy chooses. */
enip_meta_block_t *(*fetch_root_shape)(struct enip_connection_t *conn,
                                       const char *root_name, Arena *arena);

/* Build the definition of one UDT/template id the same way (e.g. AB: Template
 * object 0x6C GetAttributeList + ReadTemplate). The shared caller publishes into
 * udt_cache and owns lifetime. Recursion into member UDTs may be done here or
 * deferred to the shared walker; either way each distinct id is fetched at most
 * once per generation. Return NULL if the device has no UDT concept. */
enip_meta_block_t *(*fetch_udt)(struct enip_connection_t *conn,
                                uint32_t udt_id, Arena *arena);
```

A device without structures/UDTs (PCCC, some OMRON paths) implements
`fetch_root_shape` to emit a single-node block for the addressed item and leaves
`fetch_udt` NULL. The shared cache/walker code is identical regardless. Model
differences inside a vendor (ControlLogix vs Micro800 vs PLC5/SLC) are handled
inside the strategy file, or by the selector picking finer-grained ops — the
selector already keys on vendor id **and** device type (Part 1 §9).

The existing `fetch_tag_metadata` hook (Part 1 §9) is **retired/absorbed**: its
single-instance fetch becomes the trivial `fetch_root_shape` for a bare-root tag.

### B.5 Canonical generic type vocabulary

Presented metadata must **never** use manufacturer/protocol/device type names
(`DINT`, `REAL`, `BOOL`, PCCC `N`/`F`, OMRON-specific names). Every node carries a
**generic** type drawn from one documented, protocol-neutral vocabulary. The
native code is retained only in `data_type` for request encoding; it is **not**
presented.

**Translation is the strategy's responsibility.** The manufacturer layer is the
only code that understands native type codes, so the `fetch_root_shape` /
`fetch_udt` hooks set each node's `generic_type` as they build the block. Shared
code (the serializer, the validity gate, the path walker) only ever reads
`generic_type` and never sees a native name — this is the §A.1 isolation rule
applied to type naming.

```c
/* enip_meta.h — the documented, stable vocabulary. Numeric values are part of
 * the contract (exposed via get_int_attrib "type"); never renumber. */
typedef enum {
    ENIP_T_UNKNOWN = 0,  /* "unknown" — unmapped native type; data_type still set    */
    ENIP_T_BOOL    = 1,  /* "bool"   — single boolean                                */
    ENIP_T_I8      = 2,  /* "i8"                                                     */
    ENIP_T_I16     = 3,  /* "i16"                                                    */
    ENIP_T_I32     = 4,  /* "i32"                                                    */
    ENIP_T_I64     = 5,  /* "i64"                                                    */
    ENIP_T_U8      = 6,  /* "u8"                                                     */
    ENIP_T_U16     = 7,  /* "u16"                                                    */
    ENIP_T_U32     = 8,  /* "u32"                                                    */
    ENIP_T_U64     = 9,  /* "u64"                                                    */
    ENIP_T_F32     = 10, /* "f32"                                                    */
    ENIP_T_F64     = 11, /* "f64"                                                    */
    ENIP_T_STRING  = 12, /* "string" — character string (encoding in node flags)     */
    ENIP_T_BYTES   = 13, /* "bytes"  — opaque/raw octet block                        */
    ENIP_T_STRUCT  = 14, /* "struct" — composite; members are this node's children   */
} enip_generic_type_t;
```

Token rules:
- The serialized token is the lowercase string in the comment (`"i32"`, `"struct"`,
  …). The numeric enum is the stable contract for `get_int_attrib("type")`.
- **Arrays are not a type.** Dimensionality is `num_dims` / `array_dims`; an
  `i32[10]` node is `generic_type = i32` with `num_dims = 1`. The presenter renders
  the type plus its dims separately.
- **Bit-string types** (AB `BYTE`/`WORD`/`DWORD`/`LWORD`) map to the unsigned int
  of matching width (`u8`/`u16`/`u32`/`u64`) with the `ENIP_META_F_BITSTRING` flag
  set, so width is exact and the bit-collection nature is still recoverable.
- **`struct`** carries an optional `type_name` (the user/UDT name) purely as
  informational text; the structural truth is its child nodes. Built-in predefined
  structures are presented by their member layout the same way.
- Timestamps and other width-defined scalars map to the matching integer/float
  width (e.g. a 64-bit microsecond time → `i64`/`u64`).

**Documented reference mapping (AB/Logix `fetch_*` implements this):**

| Native (CIP) | code   | generic | Native (CIP) | code   | generic        |
|--------------|--------|---------|--------------|--------|----------------|
| BOOL         | 0x00C1 | bool    | UDINT        | 0x00C8 | u32            |
| SINT         | 0x00C2 | i8      | ULINT        | 0x00C9 | u64            |
| INT          | 0x00C3 | i16     | REAL         | 0x00CA | f32            |
| DINT         | 0x00C4 | i32     | LREAL        | 0x00CB | f64            |
| LINT         | 0x00C5 | i64     | BYTE         | 0x00D1 | u8 (bitstring) |
| USINT        | 0x00C6 | u8      | WORD         | 0x00D2 | u16 (bitstring)|
| UINT         | 0x00C7 | u16     | DWORD        | 0x00D3 | u32 (bitstring)|
|              |        |         | LWORD        | 0x00D4 | u64 (bitstring)|
| STRING / structured string | (struct/0x02xx) | string |  STRUCT / UDT | 0x02xx | struct |

OMRON and PCCC strategies provide their **own** native→generic mapping tables in
their respective files; the generic vocabulary above is the shared, documented
target for all of them.

---

## Phase 10 — Shared metadata storage primitive

**Goal:** introduce `enip_meta_block_t` and its build/emit/refcount machinery
with **no behavior change** to resolution yet. This is pure scaffolding so later
phases have a safe place to put metadata.

**STATUS: ☐ PLANNED**

Tasks:
- Create `enip_meta.h` / `enip_meta.c` with `enip_meta_block_t`,
  `enip_meta_node_t`, and the `ENIP_META_F_*` flags (§B.1).
- Implement an **arena builder**: append nodes + name bytes into `tx`/a scratch
  arena, tracking offsets, then `enip_meta_block_emit(arena, ...) ->
  enip_meta_block_t*` that `rc_alloc`s one contiguous block and copies the arena
  contents in. Validate all `*_off` on emit.
- Implement `enip_meta_block_node(block, off) -> enip_meta_node_t*`,
  `..._first_child`, `..._next_sibling`, `..._name` accessors (offset → pointer,
  bounds-checked in debug).
- Wire `rc` lifetime: emit returns refcount 1; provide no custom destructor
  beyond the rc free (the block owns no sub-allocations).
- Convert `enip_tag_meta_t` to the reference form (§B.2). Keep Part 1's resolver
  writing the **addressed-element scalars** as before, and additionally build a
  trivial **single-node block** for the root so `shape`/`node_off` are populated.
  (Root-only tags now go through the block path; deeper paths still unsupported —
  that is Phase 12.) The single-node block is still produced by the **strategy**,
  not shared code — at this phase it can reuse Part 1's existing
  `fetch_tag_metadata` body, renamed/adapted toward the §B.4 `fetch_root_shape`
  hook in Phase 11. Shared code only emits/stores the block.
- Update `enip_tag_get_int_attrib` to read `elem_size`/`elem_count`/`data_type`
  from the new fields, still gated on "usable".
- Update `enip_tag_destructor` to `rc_dec(meta.shape)` if non-NULL.

**Accept:** a bare-root / fully-indexed array tag reads end-to-end exactly as in
Part 1, but its metadata now lives in a refcounted block the tag references.
ASan clean; no block leaks across create/destroy and across a forced reconnect.

---

## Phase 11 — Connection full-shape cache + recursive UDT cache

**Goal:** fetch a root symbol's **complete** shape once per connection and share
it; fetch UDT definitions lazily and recursively; key both on
`metadata_generation`.

**STATUS: ☐ PLANNED**

Tasks:

*Shared (manufacturer-neutral) — `enip_meta.c` / `enip_conn.c`:*
- Add `root_shape_cache`, `udt_cache`, `shape_cache_mutex` to
  `enip_connection_t` (§B.3); create/destroy with the connection.
- Add the `fetch_root_shape` / `fetch_udt` hooks to `enip_mfg_ops_t` (§B.4).
- Implement `enip_meta_get_root_shape(conn, root_name) -> enip_meta_block_t*`:
  cache lookup under the mutex; on miss call
  `conn->mfg_ops->fetch_root_shape(conn, root_name, arena)`, then publish the
  returned block with a **double-checked insert** (so two threads can't
  double-fetch) and keep one cache ref. The shared code does **not** know what
  CIP queries the hook ran.
- Implement `enip_meta_get_udt(conn, udt_id) -> enip_meta_block_t*` the same way
  over `conn->mfg_ops->fetch_udt` (returns NULL cleanly when the hook is NULL).
  Own the cycle guard (in-progress set) and max-depth bound here so every
  strategy inherits them.
- **Generation handling:** on `metadata_generation` bump, walk both caches,
  `rc_dec` and clear all entries (the connection drops its refs; live tags keep
  theirs until they re-resolve). Add `enip_meta_caches_clear(conn)` and call it
  from the bootstrap right after the bump (Part 1 §5.6 / Phase 4).

*Strategy (per manufacturer/model) — `enip_mfg_ab.c` first:*
- Implement AB `fetch_root_shape`: phase-1 root_name → instance_id (existing
  `root_symbol_cache`), then Symbol instance attrs 2/7/8 for the root node; if the
  root is a structure, emit a UDT-typed node and let the shared walker lazy-load
  it (or descend here). Build nodes via the §B.1 builder; return the emitted block.
- While building, **translate every native CIP type code into `generic_type`**
  per the §B.5 mapping table (and set `ENIP_META_F_BITSTRING` for BYTE/WORD/…).
  The native code stays in `data_type` for I/O; shared code only reads
  `generic_type`. This translation is the AB strategy's job and lives only here.
- Implement AB `fetch_udt` via the **Template object (Class 0x6C)**:
  GetAttributeList for member count / definition size / structure handle, then
  ReadTemplate to pull the member descriptor array + names; parse members into
  nodes (member UDTs resolved through the shared `enip_meta_get_udt`, so the
  cycle/depth guards apply). These CIP specifics are AB-only and live entirely in
  this file.
- OMRON/PCCC: stub `fetch_root_shape` to a single-node block; `fetch_udt = NULL`
  (full handling stays in their own phases).

- **Memory rule:** all of the above runs on the connection thread during phase-2
  resolution only. No allocation on the read/write hot path.

**Accept:** resolving two tags into the same root (`my_struct.a` and
`my_struct.b`, or `my_array[0]` and `my_array[1]`) fetches the root shape and any
UDT definitions **once** (verify via a fetch counter / debug log); a reconnect
clears and re-fetches. ASan clean; cache blocks freed when the last referencing
tag is destroyed.

---

## Phase 12 — Full-path resolution + validation

**Goal:** resolve a tag name of arbitrary depth against the cached shape, store
only the addressed leaf in the tag, and enforce path/elem_count rules.

**STATUS: ☐ PLANNED**

Tasks:
- Implement a **path walker** (shared, manufacturer-neutral — it interprets an
  already-built block and never queries the device) over the parsed name (root,
  then a sequence of `.member` and `[index...]` steps):
  - start at the root node of `enip_meta_get_root_shape` (which dispatches to the
    strategy hook on a cache miss, §B.4);
  - for each `.member` step, find the child node by name; if that node is a UDT,
    ensure its definition is loaded via `enip_meta_get_udt` (shared getter →
    strategy hook) and continue into it;
  - for each `[index]` step, validate against the node's dims and fold into a
    running linear element offset and byte offset;
  - the final node is the **addressed node**: copy its `elem_size`/`data_type`
    into `meta`, set `meta.node_off`, `meta.linear_start`, `meta.instance_id`,
    and take an rc ref on the shape block.
- **Validation (reject at create/resolve, return a clear error):**
  - *Array indexing is all-or-none per level.* A node with `num_dims = N` accepts
    either zero indices or exactly `N`. Partial indexing (`a[1]` on a 3-D array,
    `a[][2][3]`) is an error.
  - *elem_count vs linear size.* Reject unless
    `linear_start + elem_count <= total_elem_count`, where `total_elem_count` is
    the product of the addressed node's active dims. Use the linear-offset math in
    `src/tools/ab_server/` as the reference implementation.
  - *Default extent.* No index + no `elem_count` → element 0, `elem_count = 1`
    (§A.2).
- **Request encoding:** ensure the encoded CIP path in the tag tail (Part 1 §3.2,
  §11.8) reflects the full path (member `0x91` segments + index segments), and
  that **writes use the bare CIP type** (strip the `0x2000` dimension bit) — fold
  in the previously-noted AB write-type fix here.
- **tag->size write-through:** total size = `elem_size * elem_count` of the
  addressed leaf; (re)allocate `tag->data` to match on resolve (Part 1 §2.1).

**Accept:** against the real ControlLogix (Test PLCs memory), read and write a
structure member (`udt_tag.member`), a nested member (`a.b.c`), an array of UDTs
element (`arr[3].field`), and a multi-dimensional array slice with `elem_count`;
out-of-range index and partial-index names are rejected with a clear error;
`plc_tag_get_int_attribute` returns the leaf's type/size.

---

## Phase 13 — Auto-sync (auto-read / auto-write)

**Goal:** support `auto_sync_read_ms` / `auto_sync_write_ms` semantics, modeled
on the Modbus module.

**STATUS: ☐ PLANNED**

Tasks:
- **Auto-read:** after a read completes, if `auto_sync_read_ms > 0`, re-arm the
  op: set `op_time = now + auto_sync_read_ms`, `op_state = REQUEST`, `kind =
  READ`, and re-link into the queue (Part 1 §6). The connection thread services
  it on the next due tick.
- **Auto-write:** on `plc_tag_write` with `auto_sync_write_ms > 0`, mark the tag
  dirty and schedule `op_time = now + auto_sync_write_ms`, `op_state = REQUEST`,
  `kind = WRITE` instead of writing immediately.
- **Write-vs-pending-read precedence:** a write while an auto-read is pending
  cancels the read (`op_state` back to IDLE/READY) and re-arms as WRITE — a read
  must never clobber locally-modified, not-yet-flushed data. Mirror the exact
  dirty/abort mechanics in `src/libplctag/protocols/mb/modbus.c`.
- Raise the standard read/write completion events on each auto cycle so callbacks
  fire as they do for AB.

**Accept:** an auto-read tag refreshes on its interval without app calls; an
auto-write tag flushes a changed value within its interval; a write during a
pending auto-read is not lost and the stale read does not overwrite it. ASan
clean over a sustained auto-sync run.

---

## D. Special tags and the metadata attribute (design)

Three special tags are supported; two legacy ones are not. All supported special
tags are **manufacturer-neutral** and connection-scoped — they do not depend on
device dialect (Identity object 0x01, connection statistics, and raw CIP are all
generic), so they live in shared ENIP code, **not** behind the strategy vtable.

### D.1 `@connection` — diagnostic/control handle

Already routed in `enip.c` to `enip_connection_tag.c`
(`TAG_PROTOCOL_ENIP_CONNECTION = 9`). Bound to the shared `enip_connection_t`; no
data path, no metadata block. Surfaces connection state and statistics through
`get_int_attrib`: `state`, `metadata_generation`, `messages_sent`/`_received`,
queue depth, negotiated `cip_size_o_to_t`/`_t_to_o`, `session_handle`, reconnect
count, last latency. Read/write are no-ops (or a control poke). It only needs the
connection back-pointer wired (the TODO already noted in that file).

### D.2 `@identity` — read-only device identity

Formats the `enip_identity_t` already fetched during bootstrap (Part 1 §11.5) —
**no new I/O**. Mirror legacy AB: place the raw Identity `GetAttributeAll` byte
payload in `tag->data` (so `plc_tag_get_*` works), and expose fields through
`get_int_attrib` (`vendor_id`, `device_type`, `product_code`, `revision_major`,
`revision_minor`, `serial_number`, `status`). Product name string is available via
the §D.4 string-attribute path once that exists.

### D.3 `@raw` — CIP passthrough

The app writes a raw CIP request into `tag->data`; on write/read the engine ships
those exact bytes through the transaction seam (Part 1 §8) and copies the CIP
response back into `tag->data`. **Bypasses** metadata resolution and
`encode_chunk`/`accept_chunk`. Single frame, bounded by the negotiated cap (no
fragmentation); messaging mode comes from `mfg_ops->messaging_mode`. The meaning
of the response bytes is the application's responsibility.

### D.4 Per-tag metadata as a JSON string attribute

**Not** a special tag. Any normal ENIP data tag answers a string attribute
(name TBD, e.g. `"metadata"`) with a JSON document describing its full resolved
structure: the path, the addressed leaf, and the complete type tree — root shape
plus nested UDT members with names, **generic types** (§B.5), dimensions, element
sizes, and byte offsets. It serializes the **manufacturer-neutral**
`enip_meta_block_t` tree (Phases 10–12) and the referenced UDT blocks, so the
serializer is shared even though the shape was produced behind the vtable.

The presented `type` is always a §B.5 token (`i32`, `f64`, `struct`, …) — **never**
a native name like `DINT`. The native code is not exposed in the metadata; type
translation already happened in the strategy when the block was built (§B.5).

Two consequences:

- **API gap (shared-library change).** There is no string-attribute API today;
  the vtable has only `get_int_attrib` (`tag.h:68`) and the public header has no
  `plc_tag_get_string_attribute`. This feature requires a generic
  `get_str_attrib(tag, name, buf, buf_len)` vtable hook plus a public
  `plc_tag_get_string_attribute(id, name, buf, len)` with **length-probe**
  semantics (call with `len = 0` / NULL buffer to learn the required size, then
  call again with a buffer), mirroring `plc_tag_get_string` /
  `_get_string_length`. It is generic and benefits every protocol, but it touches
  `lib.c`, `tag.h`, and `libplctag.h` — larger than Part 1's single
  create-dispatch edit, so coordinate it.
- **Forces a full fetch.** A leaf tag needs only its leaf type for I/O, but a
  metadata request needs the whole root shape and every referenced UDT. So a
  metadata request may trigger the full Phase 11 fetch if not already cached.

The output can be **very large**. Build the JSON into a transient buffer on
demand and free it after copy-out; never hold it resident. This keeps it off the
steady-state path (it is an explicit out-of-band query, like resolution, not
hot-path I/O).

### D.5 Explicitly unsupported

`@tags` (symbol enumeration) and `@udt/<id>` (UDT dump) from the legacy AB module
are **not** implemented. Their use cases are served by the §D.4 metadata
attribute on an ordinary tag.

---

## Phase 14 — Special tags: `@connection`, `@identity`, `@raw`

**Goal:** finish `@connection` and add `@identity` and `@raw`, all in shared
manufacturer-neutral ENIP code.

**STATUS: ☐ PLANNED**

Tasks:
- **`@connection`:** wire the `enip_connection_t` back-pointer into
  `enip_connection_tag_t`; implement the `get_int_attrib` set in §D.1; ensure the
  tickler reads live connection stats.
- **`@identity`:** add a small read-only tag type (or fold into the connection-tag
  vtable family) that copies the cached raw Identity payload into `tag->data` at
  create/first-read and answers the §D.2 `get_int_attrib` fields. No new socket
  I/O — the identity is already fetched at bootstrap.
- **`@raw`:** add a raw op path — an `ENIP_OP_KIND_RAW` (or a `raw` flag) whose
  build step emits `tag->data[0..size)` as the CIP request verbatim and whose
  accept step copies the CIP response slice back into `tag->data`, sets
  `tag->size`, and completes. No metadata, no fragmentation; error
  `PLCTAG_ERR_TOO_LARGE` if request or response exceeds the cap. Mode from
  `mfg_ops->messaging_mode`.
- Route all three in `enip_tag_create` alongside the existing `@connection`
  branch; keep the data-tag path unchanged for normal names.

**Accept:** `@connection` reports live state/stats; `@identity` returns the PLC's
vendor/device/serial and raw identity bytes with no extra round trip; `@raw`
round-trips a hand-built CIP request (e.g. a GetAttributeAll) and returns the
device's response bytes. ASan clean.

---

## Phase 15 — Per-tag metadata JSON attribute

**Goal:** expose the full resolved structure of any tag as JSON through a generic
string-attribute API.

**STATUS: ☐ PLANNED** — depends on Phases 10–12 (the shape/UDT blocks exist).

Tasks:
- **Shared-library API (generic, coordinate outside ENIP):** add
  `get_str_attrib` to the tag vtable (`tag.h`); add
  `plc_tag_get_string_attribute(id, name, buf, len)` to `libplctag.h` / `lib.c`
  with length-probe semantics (§D.4); dispatch to the vtable like the int variant
  (`lib.c:2104`).
- **ENIP serializer (shared, manufacturer-neutral):** implement
  `enip_tag_get_str_attrib(tag, "metadata", buf, len)` that walks the tag's
  `meta.shape` block and referenced UDT blocks (§B.1) and emits the document —
  node name, **generic type token (§B.5)**, dims, element size, byte offset,
  optional struct `type_name`, children, and the addressed leaf / path summary.
  It reads only `generic_type`; it must not emit native type names or codes.
  Produce into a transient buffer; honor the length probe; free after copy-out.
- Optionally expose the leaf's generic type as an integer via
  `get_int_attrib("type")` returning the `enip_generic_type_t` value (§B.5), so
  callers that don't parse the document still get a protocol-neutral type.
- **On-demand full fetch:** if the tag resolved only its leaf, ensure the full
  root shape + UDTs are loaded (call the Phase 11 getters) before serializing.
  This may issue device queries (behind the vtable) — it is an explicit query,
  not steady-state I/O.
- Bound recursion/output with the same cycle guard and max-depth used in Phase 11.

**Accept:** `plc_tag_get_string_attribute(tag, "metadata", …)` on a UDT/array tag
returns well-formed JSON describing the complete nested structure; the length
probe returns the exact size; a leaf-only tag triggers the full fetch on first
request and is cached thereafter. No steady-state allocation; transient buffer
freed. ASan clean.

---

## C. Cross-references back into Part 1

The Part 1 sections that these phases supersede or extend (note them when editing
that file later, or leave Part 1 as the historical baseline):

- **§3.3 `enip_tag_meta_t`** — superseded by §B.2 (reference, not inline copy).
- **§4.3 `enip_connection_t`** — extended by §B.3 (two new caches).
- **§4.4 "Phase-2 type info is not here / no connection-level cache"** — reversed
  by Phase 11.
- **§11.7 `enip_metadata_fetch_tag_info` "no connection-level cache"** — reversed
  by Phase 11; resolution becomes full-path (Phase 12).
- **§9 vtable / `fetch_tag_metadata`** — extended by §B.4: metadata *retrieval*
  becomes the `fetch_root_shape` / `fetch_udt` hooks; `fetch_tag_metadata` is
  absorbed. The rule that retrieval is manufacturer/model-specific is reaffirmed
  (§A.1) — shared code never queries or parses a device.
- **Default read extent** — Part 1 read the whole array; §A.2 changes the default
  to a single element.

---

**End of Part 2.**

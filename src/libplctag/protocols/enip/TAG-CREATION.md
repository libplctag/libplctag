# ENIP Tag Creation

How a tag is created and driven to readiness in the ENIP module.

This document mixes **current behavior** with **planned design**. Anything not
yet in the code is called out in the *Implementation status* section at the end.

## Tag attribute summary

The pieces of the attribute string that matter to creation:

- `gateway` — host of the ENIP endpoint. The TCP port is **not** a separate
  attribute; it rides on the gateway string as `host:port` and defaults to
  `44818` when omitted.
- `path` — CIP route to the target (e.g. `1,4`), parsed into `route_path` /
  `cpu_slot`.  Only required on ControlLogix-class devices. OMRON TBD.
- `name` — the tag path. May be a bare symbol (`my_tag`), an array element
  (`my_array[10][20][30]`), a structure member (`my_struct.member1`), or any
  nesting of the two (`my_array[3].field[1]`).
- `elem_count` — number of elements to address starting at the resolved
  position. Defaults to `1`.
- byte-order overrides — `int16_byte_order`, `int32_byte_order`,
  `int64_byte_order`, `float32_byte_order`, `float64_byte_order`, and the
  `str_*` family (see *Byte order* below).

## Shared entry path (both cases)

`enip_tag_create` (enip.c:145) → `enip_protocol_tag_create` (enip_tag.c:274).
This sequence is identical regardless of whether the connection already exists:

1. Encode the tag path string into a temporary buffer (enip_tag.c:280).
2. Extract the root tag name (`TagName` from `TagName[i].member`) (enip_tag.c:292).
3. Single `rc_alloc` of `sizeof(enip_tag_t) + encoded_path + name` (enip_tag.c:311);
   copy the encoded path and name into the tail of the allocation.
4. Init metadata: `meta.state = ENIP_META_NONE`, `needs_metadata = true`;
   set the vtable and `protocol_type = TAG_PROTOCOL_ENIP`.
5. Point `tag->byte_order = &enip_logix_byte_order` **before**
   `plc_tag_generic_init_tag` (enip_tag.c:341) so the default is in place and
   never NULL.
6. `plc_tag_generic_init_tag` (enip_tag.c:343) — generic field setup, assigns
   `tag_id`, registers the callback.
7. `enip_registry_find_or_create(attribs)` (enip_tag.c:361) — **the two cases
   diverge here.**
8. `tag->conn = conn`; `tag->skip_tickler = 1` (the global tickler will not touch
   it — the connection thread owns this tag, mirroring Modbus).
9. Arm an outstanding read: `op_state = REQUEST`, `kind = READ`, `op_time = now`
   (enip_tag.c:378).
10. Insert into `conn->active_tags` under `active_tags_mutex` (enip_tag.c:383).
11. Return to the generic `plc_tag_create`, which **blocks on the tag cond var
    until `status` leaves PENDING**. The ENIP connection thread (not the caller)
    drives the tag to completion.

After the ENIP vtable create returns, the generic create path applies any
byte-order overrides (see below) — the ENIP module does not handle those itself.

## Byte order

The ENIP module supplies a single static default, `enip_logix_byte_order`
(`is_allocated = 0`), and points every fresh tag at it in step 5.

Overrides are handled **generically** in `lib.c:set_tag_byte_order` (called from
the create path after the vtable create returns, lib.c:1102 / lib.c:4074):

- If any byte-order attribute (`int*_byte_order`, `float*_byte_order`, `str_*`)
  is present, lib.c allocates a fresh `tag_byte_order_t`, copies the default
  into it, applies the overrides, repoints `tag->byte_order` at the copy, and
  sets `is_allocated = 1`.
- On destroy, the generic teardown frees the struct iff `is_allocated`.

So two tags on the same connection can carry different byte orders. The ENIP
module needs no special handling beyond providing a valid, statically-allocated
default.

## Connection metadata model

All metadata lives on the **connection**, keyed so that many tag handles into
the same top-level symbol share one fetch. Three caches, all stamped with the
connection's `metadata_generation`:

- **Root symbol cache** — `conn->root_symbol_cache`
  (`hash(name) → enip_root_symbol_entry_t`, enip_conn.h:110). Populated by the
  phase-1 walk (`enip_metadata_fetch_root_symbols`, enip_metadata.c:148). Maps a
  root symbol name to its `instance_id` and symbol type.
- **Full tag metadata cache** *(planned)* — the complete, lazily-loaded shape of
  a top-level symbol (dimensions, member layout, element sizes). Needed because
  multiple handles may open against different parts of the same symbol
  (`my_array[32]`, `my_array[0]`, `my_array[1].field4`); the full shape is
  fetched once and individual handles resolve against it. Also lets tags larger
  than the max read/write size be addressed piecewise.
- **UDT definition cache** *(planned)* — UDT templates, fetched lazily and
  recursively (an array of UDTs pulls in the UDT definition, which may reference
  further UDTs). Cached per connection.

### Memory model

The "single allocation per tag / zero runtime allocation in steady state /
bounded memory" rule is preserved by confining cache allocation to
**resolution**:

- The metadata and UDT caches may allocate during the CONNECTING bootstrap and
  during phase-2 resolution (including lazy UDT fetches the first time a given
  root/UDT is seen).
- **Steady state** is defined as a tag that has reached `ENIP_META_READY`:
  reads and writes against it perform **no allocation**.
- The caches grow only when a previously-unseen root or UDT is first resolved,
  never during I/O.

### Generation changes (reconnect)

A successful (re)connect bumps `conn->metadata_generation` (enip_conn.h:130). On
a bump the cached root/full/UDT metadata is **discarded** (or marked stale) and
re-resolved lazily as tags come due against the new generation. This is
self-correcting if the PLC program changed between connects.

A tag's metadata is usable iff
`meta.state == ENIP_META_READY && meta.generation == conn->metadata_generation`.
A generation mismatch forces the tag back through phase-2 resolution before its
next read/write.

## Two-phase resolution

Resolution happens in `enip_ensure_due_tags_metadata` for any tag with
`op_time <= now` whose metadata is not READY-at-current-generation.

- **Phase 1 — root name → instance_id.** Look up the root symbol
  (`my_struct`, `my_array`) in `conn->root_symbol_cache`. If the connection has
  already walked the symbol list, this is a cache hit with no round trip.
- **Phase 2 — full path → final instance_id, type, element size, dims.**
  Resolve the **complete** tag path, not just the root. When the path descends
  below the root (`a.b.c`, `a[i].b`, ...), the metadata for **every level along
  the path** must be obtained — each intermediate member's type, offset, and
  (for arrays) dimensions — so the final member can be located. Resolve against
  the cached full metadata for the root, fetching and caching that full metadata
  on first use. Only the metadata for the **exact** tag path is stored in the
  tag's `meta`; the root's full shape stays in the connection cache for reuse by
  sibling handles.

A bare root path still needs only the root's own metadata. Anything deeper
requires the full chain — resolving just the root is **not** sufficient.

### Recursive UDT resolution

Whenever a level along the path is a UDT (the root itself, or any intermediate
or final member), the UDT definition for that type must be fetched and cached.
This is **recursive**: a UDT whose members are themselves UDTs (including arrays
of UDTs) requires fetching each of those member UDT definitions in turn, to
whatever depth the path and the type tree require, until every type needed to
locate and decode the addressed element is known. UDT definitions are cached per
connection (stamped with `metadata_generation`) and fetched lazily — each
distinct UDT is fetched at most once per generation, shared across all tags.

Example: `name=my_struct.member1` → phase-1 resolves `my_struct` →
`instance_id 1234`, full metadata cached at the connection (generation 1). If
`my_struct` is a UDT, its definition is fetched and cached; if `member1` is
itself a UDT (or an array of UDTs), that member's UDT definition is fetched too,
and so on recursively. Phase-2 then resolves `my_struct.member1` against the
assembled shape → `instance_id`, `data_type`, `elem_size`, etc., stored in the
tag's `meta`.

Once resolved, `tag->data` is allocated, `meta.generation` and
`meta.state = READY` are stamped. On `NOT_FOUND` / fetch failure / `NO_MEM`,
the tag is completed with the error instead.

## Metadata storage and sharing (planned)

How the resolved metadata is laid out and referenced. Two complementary ideas:
**share connection-owned metadata by pointer** instead of copying it into each
tag, and **build each metadata unit in an arena, then emit it as one immutable
block**.

### Tag points into connection-owned metadata (no per-tag copy)

Instead of copying the resolved shape into every tag handle, a tag stores a
small reference into the connection's metadata: `{block, node_offset,
generation}`. Many handles into the same root/UDT share one stored copy.

- **Pro — memory scales with distinct roots/UDTs, not handle count.** A
  50-member UDT array opened by 100 handles is stored once, not 100×.
- **Pro — keeps single-allocation-per-tag honest.** The tag carries a reference
  plus the generation it resolved against, not a metadata blob (whose size isn't
  known at create time anyway).
- **Pro — cheap sibling resolution.** Resolving another handle into a cached
  root is "walk to the node, store the offset" — no copy.
- **Con — lifetime, not just validity.** The generation check tells you a
  reference is *logically* stale; it does **not** make it *safe to dereference*.
  Reading the generation guard is itself a dereference of connection-owned
  memory, so the old block must not be freed (and its address reused) while any
  tag might touch it.
- **Con — cross-thread access.** Value decode (`plc_tag_get_int32`, offset/size
  checks) runs on the **caller** thread; request building runs on the
  **connection** thread. A reconnect that frees/replaces metadata on the
  connection thread races a caller dereferencing it.

To make this safe, the shared block must be **reference-counted**: each tag
holds a ref. On a generation bump the connection drops its ref and builds a new
block; the old block is freed only when the last tag releases it. Tags
re-resolve lazily and acquire the new block's ref. This fits the existing `rc_*`
machinery, and the tag itself remains a single allocation (the shared block is
separate and shared). The generation check then gates *logical* reuse while the
refcount guarantees *memory* safety across reconnect.

(The alternative — restricting all metadata access to the connection thread so
blocks can be swapped between ticks — does not fit, because value get/set runs
on caller threads.)

### Build in an arena, emit one immutable block

The final size of a root's level set, or of a UDT, isn't known up front
(variable member counts, dimensions, nested UDTs). Accumulate the shape in a
temporary arena during resolution, then emit **one contiguous block** per root
and per UDT.

- **Pro — no repeated realloc during resolution**, and the result is a single
  cache-friendly unit that is freed/refcounted as a whole. This immutability and
  single-unit lifetime is exactly what makes pointer-sharing (above) safe.
- **Pro — transient double memory** (arena + final block) occurs only during
  resolution, which is **not** steady state, so no constraint is violated.
- **Con — pointer relocation.** If nodes reference each other by absolute
  pointers, copying the arena to the final block breaks them (they still point
  into the freed arena). Use **offsets/indices within the block** instead of
  pointers (self-relative layout); these survive the copy untouched and are
  position-independent. (A two-pass "size then fill directly" approach avoids the
  arena and copy but walks the shape twice.)

The same applies to UDTs: gather one UDT in an arena, emit one block, refcount
it, and share it across all tags and across other UDTs that reference it.

### Net design

- Arena-build → emit one **immutable, offset-addressed** block per root and per
  UDT.
- **Refcount** each block; a tag stores `{block, node_offset, generation}`
  rather than a copy.
- Generation gates logical reuse; the refcount gives memory safety across
  reconnect.

This yields bounded memory (per distinct root/UDT, not per handle), preserves
single-allocation-per-tag and zero-allocation steady state, and stays
thread-safe for caller-side value access. The cost over a plain per-tag copy is
the refcounting plus offset-based layout; a per-tag copy is simpler but loses
when many handles target large or deep UDTs — which are precisely the cases this
module must handle (arrays of UDTs, many handles into one large structure).

## Path and elem_count validation (planned)

Enforced at creation time so the PLC never sees a malformed request:

- **Array indexing is all-or-none per level.** A 3-D array accepts either no
  index (`my_array`) or all three (`my_array[10][20][30]`) — never a partial
  index (`my_array[10]`, `my_array[][20][30]`). The PLC rejects partial
  indexing, and the all-or-none rule keeps resolution simple.
- **Default extent is a single element.** With no index and no `elem_count`, the
  tag addresses element 0 with `elem_count = 1`. To read more, pass `elem_count`
  explicitly (or a full index plus `elem_count`).
- **elem_count is checked against the linear size.** For a multi-dimensional
  array the starting index is converted to a linear offset, and the request is
  rejected unless `linear_start + elem_count <= total_elem_count`, where
  `total_elem_count` is the product of the dimensions. The linear-offset math in
  `src/tools/ab_server/` can be used as a reference.

## Case 1 — first tag (no matching connection)

At step 7, `enip_registry_find_or_create` (enip.c:95) finds no match and calls
`enip_connection_create` (enip_conn.c):

- Parse `gateway` (`host[:port]`, default port 44818) and `path` →
  `route_path` / `cpu_slot`; `state = DOWN`.
- `thread_create(enip_connection_thread_entry)` (enip_conn.c:2034) — starts the
  background connection thread.
- Link into the registry list and `rc_inc` so the list and the tag each hold a
  ref (enip.c:135–137).

The **connection thread** (enip_conn.c:1807) then runs the one-time bootstrap,
unblocked by the new tag via `enip_wait_for_work`. The connection stays in the
CONNECTING state for the whole bootstrap:

1. `tcp_connect` (1833)
2. `register_session` (1839)
3. `get_identity` (1845) — vendor / device → manufacturer vtable selection
4. `forward_open` (1851) — Extended (0x5B) first, fallback to standard (0x54)
5. `phase1_metadata` (1857) — walk the root symbol list (name → instance_id cache)
6. `metadata_generation++`, status → **UP** (1864–1868)

It then enters the inner serve loop and resolves this tag's phase-2 metadata in
`enip_ensure_due_tags_metadata` (1614). Once the armed read is built
(`build_requests`), sent, and the response dispatched, `enip_tag_signal_complete`
fires → `status` leaves PENDING → the blocked `plc_tag_create` returns.

## Case 2 — subsequent tag (matching connection exists)

At step 7, `enip_registry_find_or_create` (enip.c:95) finds a connection whose
`link.host` and `route_path` match and whose `rc_inc` succeeds (enip.c:111–116),
and returns the existing connection. No thread is created; the bootstrap
(TCP / session / identity / ForwardOpen / phase-1) is **skipped** — it already ran.

The new tag is inserted into the same `active_tags` with `op_state = REQUEST`.
The already-running connection thread, sitting in its inner serve loop, picks it
up:

1. `enip_next_due_wait` (1873) sees the fresh tag has work
   (`meta.state != READY`) → returns due-now.
2. `enip_ensure_due_tags_metadata` (1883) resolves only this tag's phase-2
   metadata against the current `metadata_generation`. If the new tag is a child
   of an already-cached root (e.g. `my_struct.member1` when `my_struct` is
   cached), both phase-1 (root → instance_id) and the root's full metadata are
   served from the connection cache with no extra round trip — only the member /
   element resolution is local work. A re-walk or re-fetch is needed only if the
   root is uncached or the generation changed.
3. `build_requests` (1887) encodes the read (batched with other due tags via the
   0x0A Multiple Service Request when supported), `send_all`, `recv_dispatch`.
4. `enip_tag_signal_complete` → `status` leaves PENDING → `plc_tag_create`
   returns.

## Asynchronous creation

Tag creation does not block on the connection being fully up. A tag created
during the CONNECTING bootstrap simply sits in `active_tags` in PENDING state;
when the bootstrap completes and `metadata_generation` is bumped, the resolution
pass picks up every still-pending tag and drives it to READY. This is how the
asynchronous-create API contract is honored without holding the caller across
the whole connect.

## Auto-sync tags (planned)

Modeled on the Modbus module:

- **Auto-read.** After each completion the tag is re-inserted into `active_tags`
  with a new `op_time = now + auto_read_interval` and `op_state = REQUEST`; the
  next due tick issues the read.
- **Auto-write.** A write marks the tag dirty and schedules it with
  `op_time = now + auto_write_delay`, `op_state = REQUEST`, `kind = WRITE`.
- **Write vs pending read.** A write while an auto-read is pending cancels the
  read (`op_state = READY`) and re-arms the tag as a WRITE. A read must never
  overwrite local state the application has changed, so a pending read is
  aborted once the tag is dirty.

## Key differences

- Case 2 pays only the phase-2 metadata fetch plus the first read. Case 1
  additionally pays the full connection bootstrap and the phase-1 symbol walk.
- Reconnects re-run the bootstrap and bump `metadata_generation`, discarding the
  metadata caches and forcing every existing tag back through phase-2
  resolution before its next read/write.

## Implementation status

Currently implemented:

- Shared entry path, single-allocation tag, default byte order + generic
  override.
- Connection registry, ref-counting, background connection thread, bootstrap.
- Phase-1 root symbol cache and phase-2 resolution for **root** symbols.
- Armed read on create, blocking create until PENDING clears, async create.

Planned / not yet implemented:

- Connection-level full metadata cache and recursive UDT definition cache
  (a prior per-connection metadata cache was removed — enip_metadata.c:40).
- Structure-member and array-element path resolution (`name=a.b`, `a[i].b`).
- Array index all-or-none validation and `elem_count` linear-size validation.
- Auto-read / auto-write.

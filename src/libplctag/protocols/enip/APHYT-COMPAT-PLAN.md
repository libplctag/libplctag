# aphyt-compatible server tags — implementation plan

Goal: let the aphyt Python driver (Sysmac Studio-style "real OMRON" client) connect
to a libplctag device-sim server and **list tags and UDTs**, then read/write them,
using the exact CIP dialect aphyt speaks on the wire.

Scope note: the libplctag *client* and the device-sim *server* today speak a matched
**private** subset of the OMRON tag/UDT protocol (class 0x6A service `0x5F`; class
0x6C by template id). aphyt speaks the **stock-firmware** subset (class 0x6A service
`0x01`; metadata by symbolic name; class 0x6C by real `variable_type_instance_id`).
The two do not overlap on class 0x6A, so aphyt fails on its first list request today.
This plan makes the server answer *both* dialects — the `0x5F`/client path is untouched
and additive-only.

---

## 0. Ground truth — the aphyt source *is* the spec

No further captures were available, so the wire formats below come from the aphyt
source itself (`github.com/aphyt/aphytcomm`), which is authoritative: its parser byte
offsets are exactly what it expects on the wire. Files:
`src/aphyt/omron/n_series.py` (call graph) and its reply classes
`InstanceIDAttributes`, `VariableObjectReply`, `VariableTypeObjectReply`.

**Correction to the earlier draft:** the `aphyt_omron_get_variable_list.pcapng` capture
(class 0x6A service `0x01`, per-instance walk) is an **older** aphyt. Current aphyt lists
via **`0x5F` GetInstanceList**, the same service the libplctag server already implements.
This shrinks the work substantially and removes the "need more captures" blocker.

### aphyt's actual enumeration call graph (current source)

```
variable_list()                       -> reads a dict populated by:
update_variable_dictionary():
  _get_instance_list_subset(user=True)   -> 0x5F GetInstanceList, class 0x6A, kind=user, paged 100
  _get_instance_list_subset(user=False)  -> 0x5F GetInstanceList, class 0x6A, kind=system
  for each variable:
    _get_instance_from_variable_name(name):
        get_attribute_all(0x01) on SYMBOLIC path (0x91 name)
        variable_type_object_instance_id = reply_data[8:12]  (u32 LE)
        if nonzero: _structure_instance_from_variable_type_object(id):
            get_attribute_all(0x01) on class 0x6C, instance = id.to_bytes(2)  <-- 16-bit only
            walk next_instance_id (u32) as 2-byte instances
            recurse nesting_variable_type_instance_id (u32) as 2-byte instances
```

### Confirmed wire formats (aphyt reply-parser offsets, all LE)

`InstanceIDAttributes` — one 0x5F record:
`data_length(u16)@0 | class_id(u16)@2 | instance_id(u32)@4 | name_len(u8)@8 | name@9`.
→ **Byte-for-byte identical** to the server's existing emitter
(`omron_listing.c:179`). ✅

`VariableTypeObjectReply` — class 0x6C GetAttributeAll:
`size@0(u32) | rsvd@4 | cip_data_type@5 | cip_type_of_array@6 | array_dim@7 |
number_of_elements[dim]@8 | number_of_members(u16)@8+dim*4 | crc(u16)@14+dim*4 |
name_len(u8)@16+dim*4 | name@17+dim*4 | pad(1 if name_len even) |
next_instance_id(u32) | nesting_variable_type_instance_id(u32) | start_array[dim]`.
→ **Byte-for-byte identical** to the server's `encode_variable_type_reply`
(`omron_listing.c:268`), including the `pad-when-even` rule. ✅

`_get_instance_from_variable_name` reads the symbolic `0x01` reply's
`variable_type_instance_id` at **`[8:12]`** (note: a *different* offset from the class
0x6B `VariableObjectReply`, which puts it at `[20+dim*4]` — the server can ignore 0x6B;
aphyt's live path uses the symbolic form).

---

## 1. Gap analysis

Server dispatch: `common/cip.c:cip_dispatch_unconnected` calls `try_cip_object`
(`cip.c:460`) first (matches a registered class handler by parsed class id), else the
service `switch` (`cip.c:162`). A symbolic `0x91` path fails `cip_path_parse`
(`cip_path.c:120`) so it bypasses `try_cip_object` and lands in the switch. A registered
handler returns `DEVICE_SIM_NOT_HANDLED (1)` to decline; any other non-OK becomes a CIP
`0x08` error reply.

Only what current aphyt's `variable_list` / `update_variable_dictionary` path actually
calls. (The older-aphyt `0x01`-on-0x6A walk and class 0x6B are **not** on this path and
are dropped.)

| # | aphyt call (wire) | current server | work | format? |
|---|---|---|---|---|
| 1 | `0x5F` GetInstanceList on class 0x6A, **kind=user** and **kind=system** (two calls) | ✅ answers 0x5F, but **ignores kind** → returns all tags for *both* calls → aphyt concatenates → **duplicate** variables | filter by kind (user vs system) | ✅ exact |
| 2 | `0x01` on **symbolic** path (`0x91` name) → variable attrs; aphyt reads type-id at `[8:12]` | symbolic path bypasses `try_cip_object`, switch routes to `handle_identity` → wrong | new by-name metadata handler emitting `size@0 | cip_data_type@4 | … | variable_type_instance_id@8:12` | ✅ offsets known |
| 3 | `0x01` on class 0x6C, **2-byte** instance; walk `next_instance_id`, recurse `nesting_…_id`, all re-queried as 2-byte | ✅ reply format exact, **but** member/nested ids are synthetic **>0xFFFF** (`member_id_encode`) → aphyt's `id.to_bytes(2)` overflows/crashes | give members **real ≤0x0FFF** instance ids | ✅ exact (except ids) |
| 4 | `0x4D` write / `0x4C` read, symbolic + `0x80` Simple Data Segment offset | symbolic 0x4C/0x4D handled; `0x80`-offset parsing unverified | verify/extend `0x80` offset path | from write capture |

Primary user goal ("list the tags and UDTs") = **1, 2, 3**. Full round-trip adds **4**.
No gap here is inferred — 1 and 3 are already byte-exact in the server; the real work is
kind-filtering (1), a new symbolic handler (2), and real 16-bit member ids (3).

---

## 2. Phase 1 — kind filtering on the 0x5F list (gap 1)

The server already answers `0x5F` in the exact `InstanceIDAttributes` format. The one
defect: `handle_tag_name_server` (`omron_listing.c:136`) unpacks `kind` and drops it
(`(void)kind;`, line 155). aphyt calls the list **twice** — `user=True` (kind=2) and
`user=False` (kind=1) — and concatenates, so every tag comes back duplicated.

Fix: honor `kind`.
- Add a `bool system` flag to `tag_def_t` (device_sim), defaulting false; set it for any
  tag the sim should report as a system variable (probably none by default — a device
  configured with only user tags is the common case).
- In the walk, skip tags whose `system` flag ≠ (`kind == 1`). Result: kind=2 returns
  user tags, kind=1 returns system tags (empty by default), no duplicates.

Ponytail: no separate system-tag machinery unless a test needs one — default all tags to
user and let kind=1 return an empty, well-formed list (count 0, status 0). That alone
kills the duplication.

---

## 3. Phase 2 — variable metadata by name (gap 2)

aphyt `_get_instance_from_variable_name`: `Get_Attributes_All` on a `0x91` symbol path,
then reads `reply_data[8:12]` (u32 LE) as the `variable_type_instance_id` (0 = atomic;
else the class-0x6C instance to walk). Confirmed reply shape (from the write capture,
`TestString` = 256-byte STRING: `00010000 d0 00 00 00 00000000 …`):

```
size            (u32 LE) @ 0     = in-memory size (256)
cip_data_type   (u8)     @ 4     = OMRON type byte (STRING = 0xD0)
cip_type_array  (u8)     @ 5
array_dimension (u8)     @ 6     (+1 pad @ 7)
variable_type_instance_id (u32 LE) @ 8   = 0 for atomics; 0x6C instance for a UDT
```

Routing: a `0x91` path fails `cip_path_parse`, so it bypasses `try_cip_object`
(`cip.c:158`) and hits the switch. Add a branch in the `CIP_SRV_GET_ATTRS_ALL` case
(`cip.c:163`) **before** `handle_identity`: if the first path segment byte is `0x91`,
call a new `omron_variable_attrs_by_name(dev, name, resp…)`. OMRON-guard it (family known
at endpoint create, `eip_server_tag.c`, or gate on "0x6A/0x6C handlers registered") so
Logix/PCCC servers are untouched.

The handler: `device_tag_find_by_name`, emit the block above from `tag_def_t` —
`instance_size` → size, atomic `tag_type_t` → OMRON `cip_data_type` byte, and for a UDT
tag put its **template id** at `[8:12]` (must be ≤0x0FFF — it already is, see Phase 3).

**No new type-map file needed** — `tag_type_t`'s low byte *is* the CIP type byte already
(confirmed identical to the table below), so the handler reads it directly
(`tag->tag_type & 0xFF`); struct tags substitute `CIP_DATA_TYPE_STRUCT` (0xA0). Implemented
as `handle_omron_variable_attrs` in `common/cip.c` (not a separate dialect file), reached
from both `cip_dispatch_unconnected` and `cip_dispatch_connected`'s `CIP_SRV_GET_ATTRS_ALL`
case, gated on `dev->plc_type == ENIP_PLC_OMRON_NJNX && svc_path.data[0] == CIP_SYMBOLIC_SEGMENT`.

**Confirmed type-byte map** (from aphyt's `src/aphyt/cip/cip_datatypes.py` — these are
the standard CIP elementary type codes, not OMRON-vendor-specific, so they cover every
entry in `device_types.c`'s `CIP_TYPES[]` directly; `omron_datatypes.py` only adds
OMRON-vendor extras — TIME/DATE/UNION/ENUM/BCD — not present in that table):

| tag_type_t | byte | tag_type_t | byte |
|---|---|---|---|
| BOOL | 0xC1 | REAL | 0xCA |
| SINT | 0xC2 | LREAL | 0xCB |
| INT | 0xC3 | STRING | 0xD0 |
| DINT | 0xC4 | BYTE | 0xD1 |
| LINT | 0xC5 | WORD | 0xD2 |
| USINT | 0xC6 | DWORD | 0xD3 |
| UINT | 0xC7 | LWORD | 0xD4 |
| UDINT | 0xC8 | | |
| ULINT | 0xC9 | | |

Also confirms `OMRON_CIP_DATA_TYPE_STRUCT = 0xA0` already in `omron_listing.c` matches
`CIPAbbreviatedStructure` — no change needed there.

---

## 4. Phase 3 — real member instance ids (gap 3)

**Correction during implementation:** member ids are minted from the *same*
`next_template_id` counter templates already use, capped at **12 bits**
(`0x0FFF`) — not a standalone 16-bit space, and not the full ≤0xFFFF range
this section originally proposed. Reason: Rockwell's struct-tag encoding
(`DEVICE_SIM_STRUCTURE_TYPE`, `tag_type_t`'s high nibble + low 12 bits) already
caps template ids to 12 bits (the 16-bit CIP type word only leaves 12 bits for
the id once the structure-tag flag bit is carved out), so sharing one counter
keeps template and member ids trivially disjoint without a flag-bit scheme,
and both Rockwell and OMRON dialects draw from the one id space instead of
each inventing its own. OMRON's wire format could in principle use the full
16 bits, but narrowing both to 12 bits lets them share code, which is the
adopted tradeoff.

Implemented as (`server/device.h` for declarations, `server/device_sim.c` for logic):
- `udt_member_id_t { instance_id, template_id, member_index, next }` — a linked list on
  `device_t.udt_member_ids`, one node per member, minted individually
  (`mem_alloc(sizeof(udt_member_id_t))` each, not a block) inside the same
  `critical_block(dev->tags_mutex)` as template-id assignment, so template and member ids
  can never collide.
- `device_udt_find_member(dev, instance_id, &template_id_out, &member_index_out)` —
  linear-scan lookup, instance → (template, member index).
- `device_udt_member_instance_id(dev, template_id, member_index, &instance_id_out)` —
  reverse lookup, (template, member index) → instance, used when encoding replies.
- `device_sim_add_udt_type`'s cap check: `next_template_id + num_members >= 0x0FFF`.
- Teardown: `free_udt_member_ids()` walks and frees each node individually (must match the
  one-alloc-per-member minting above, or `device_sim_destroy` corrupts the allocator).

The class-0x6C reply format already matches `VariableTypeObjectReply` byte-for-byte
(`encode_variable_type_reply`, incl. the pad-when-even rule). The **only** problem:
`_structure_instance_from_variable_type_object` re-queries `next_instance_id` and
`nesting_variable_type_instance_id` as **2-byte** instances
(`instance_id.to_bytes(2,'little')`), but the server hands back **synthetic ids
>0xFFFF** (`member_id_encode` sets bit 24, `omron_listing.c:213`, since removed). aphyt's
`.to_bytes(2)` overflows → crash.

Fix: replaced synthesis with the real ≤0x0FFF instance registry above.
- `omron_listing.c`'s `handle_variable_type_template`/`handle_variable_type_member` now
  compute `next_id` via `device_udt_member_instance_id` instead of `member_id_encode`
  (removed, along with `OMRON_MEMBER_ID_FLAG`).
- `handle_variable_type` (dispatcher) tries `device_udt_find_member` first, falling back
  to `handle_variable_type_template` on a miss (top-level template id) — replaces the old
  `member_id_decode` call.
- Keep the `next_instance_id` **chain** (one member per reply) — aphyt expects exactly
  that (`while member_instance_id != 0`), and it's what the server already emits; only
  the id values changed.

The libplctag client currently consumes these ids too — `omron_udt_walk` (existing test,
unmodified) still passes against the new real ids, confirming the client's
`next_instance_id` handling is opaque and round-trips fine.

**Bugs caught and fixed during implementation** (self-caught, no user involvement):
1. Use-after-free: `device_sim_add_udt_type` originally linked the new `tmpl` into
   `sim->dev.udt_templates` *before* minting member ids; a mid-loop allocation failure
   then `mem_free(tmpl)`'d it while still linked into the live list. Fixed by deferring
   the link until after the member-id loop succeeds.
2. Allocator corruption risk: originally minted all of a template's member ids as one
   contiguous block; teardown frees each list node individually, which would corrupt the
   allocator against a block allocation. Fixed by allocating one node per member.

No class-0x6C-instance-0 "class metadata" handler is needed — aphyt's live UDT path
reaches 0x6C only via a variable's `variable_type_instance_id`, never instance 0. (The
instance-0 query in the write capture is a separate aphyt bootstrap; add it only if the
acceptance run shows aphyt calling it — cheap to add later, `reply_data[2:4]` = template
count.)

---

## 5. Phase 4 — read/write data path (gap 4)

aphyt write: service `0x4D`, path = `0x91` name + `0x80` Simple Data Segment carrying the
byte offset (`OMRON-SPECIFIC-DESIGN.md §3`), then the data; read is the `0x4C` mirror.

- Verify `handle_write`/`handle_read` + `parse_tag_path` (`cip.c:343`) accept a `0x80`
  segment trailing the `0x91` symbol and apply it as a byte offset; if only the Rockwell
  `0x52`/`0x53` separate-offset form is handled, add `0x80` parsing.
- STRING payload framing from the capture: aphyt wrote `d0 00 01 00 00 01 <string>`
  (type 0xD0, length words, bytes) — align server STRING read/write with it.
- Small deltas; verify-then-patch, not a rewrite.

---

## 6. Phase 5 — tests

Behavior is **server-observable** and the "client" is aphyt (external Python), so this
is not a `src/examples` client tool (`feedback_enip_client_test_placement` covers the
client case, not this):

1. **device_sim self-test** (`src/tests`, libplctag API only): stand up a device-sim
   OMRON server with a few atomic tags + one UDT tag, drive the exact aphyt request byte
   sequences (0x5F user/system, symbolic 0x01, 0x6C chain) at the CIP dispatch, and check
   the reply bytes at the offsets aphyt reads (list record fields, `[8:12]` type-id,
   `next_instance_id` ≤0x0FFF, member names). In-sandbox, no network.
   **Implemented** for gaps 1–2 as `src/tests/omron_aphyt_metadata` (plain executable,
   not cmocka — the `unit/` cmocka suite only builds on Linux+Debug with network access
   for `FetchContent`, neither available in this sandbox; and plain asserts are compiled
   out under this build's `-DNDEBUG`, so the test uses an explicit `CHECK()` macro that
   counts and prints failures instead). Calls `cip_dispatch_unconnected` directly
   (bypasses EIP/CPF socket framing) with hand-packed `Bytes` requests. Gap 3 is covered
   by the pre-existing `omron_udt_walk` test (full client-server round trip over a real
   socket). Both pass clean under ASAN/UBSAN.
2. **aphyt acceptance** (manual): `pip install aphyt`, point its `NSeries` at a running
   device-sim server, call `update_variable_dictionary()` + `variable_list()` and the
   UDT walk, diff against the configured tag/UDT set. This is the real "done" gate — the
   self-test only proves the bytes we *believe* aphyt sends; running aphyt proves it.
   Document under `run_enip_tests.sh` (network/HW section).

---

## 7. Risks / open questions

- **Member-id registry (Phase 3)** was the one non-trivial change: disjoint ≤0x0FFF ids
  for templates vs members (shared counter, not separate ranges), and keeping the
  libplctag *client* walk working with the new ids. Resolved — covered by the existing
  `omron_udt_walk` test (client walk, unmodified, still passes) + the new
  `omron_aphyt_metadata` self-test (server-side kind filter + symbolic metadata).
- ~~OMRON CIP data-type byte map~~ resolved — see the table in Phase 2, pulled from
  aphyt's `cip_datatypes.py`.
- **Family gating**: the symbolic-`0x01` and `0x80`-segment branches must not regress
  Logix/PCCC servers — gate on OMRON-family / 0x6A-0x6C-registered.
- **aphyt bootstrap calls** not on the enumerate path (class-0x6C instance-0, class 0x6B,
  `_get_number_of_variables`) — the acceptance run will reveal if aphyt actually issues
  any; each is a small additive handler if so. Not blocking.

---

## 8. Files touched

- `dialects/omron/omron_listing.c` — Phase 1 (kind filter in `handle_tag_name_server`),
  Phase 3 (`handle_variable_type*` use the real member-id registry; `member_id_encode`/
  `member_id_decode`/`OMRON_MEMBER_ID_FLAG` removed).
- `common/cip.c` — Phase 2: new `handle_omron_variable_attrs` (symbolic `0x01` handler,
  reads `tag_type_t`'s low byte directly, no separate type-map file needed) plus routing
  in both `cip_dispatch_unconnected` and `cip_dispatch_connected`'s
  `CIP_SRV_GET_ATTRS_ALL` case. Phase 4 `0x80` verify/patch — not started.
- `server/device.h` — Phase 1: `bool system` on `tag_def_t`. Phase 3: `udt_member_id_t`
  struct, `device_t.udt_member_ids` field, `device_udt_find_member`/
  `device_udt_member_instance_id` extern decls.
- `server/device_sim.c` — Phase 3: member-id registry implementation
  (`device_udt_find_member`, `device_udt_member_instance_id`, `free_udt_member_ids`,
  restructured `device_sim_add_udt_type`).
- `src/tests/omron_aphyt_metadata/` (new plain-executable test, unconditionally built
  under `LIBPLCTAG_FEATURE_SERVER`, mirrors `omron_udt_walk`'s pattern) — Phase 5 for
  Phases 1/2. `run_enip_tests.sh` — Phase 5 aphyt acceptance run, not yet added (manual
  step, gated on running real aphyt).
- `OMRON-SPECIFIC-DESIGN.md` — record the aphyt (stock-firmware) dialect, citing the
  aphyt source offsets, alongside the existing client subset. **Not yet done.**

---

## 9. Suggested order

1. **Phase 1** (kind filter) — smallest, self-contained; makes aphyt `variable_list`
   return the right set (no dupes) immediately. Highest value, lowest risk. **Done.**
2. **Phase 2** (symbolic metadata) — unlocks aphyt resolving each variable's type;
   required before the UDT walk fires. **Done.**
3. **Phase 3** (real member ids) — the one structural change; UDT walk works end to end.
   **Done** (see the id-space correction above); existing `omron_udt_walk` test still
   passes unmodified against the new real ids.
4. **Phase 4** (read/write `0x80`) — round-trip; independent, can slot anytime after 2.
   Not started.
5. **Phase 5** self-test alongside each phase; the aphyt acceptance run gates "done".
   Phase 3 is covered by the existing `omron_udt_walk` test. Phases 1 (kind filter) and
   2 (symbolic metadata) are now covered by `src/tests/omron_aphyt_metadata` — drives
   `cip_dispatch_unconnected` directly (no socket framing) with hand-packed CIP request
   bytes for `kind=1`/`kind=2` GetInstanceListEx2 and symbolic `0x01` GetAttributesAll
   against atomic, UDT, and unknown-tag names. Passes clean under ASAN/UBSAN. **Done.**

Stop-and-confirm after Phase 2 (below) still applies — real aphyt hasn't been run
against this yet.

Stop-and-confirm after Phase 2: run real aphyt `update_variable_dictionary()` +
`variable_list()` against the server. If the tag list is correct, the dispatch-integration
approach is proven and Phase 3 is just the UDT extension.

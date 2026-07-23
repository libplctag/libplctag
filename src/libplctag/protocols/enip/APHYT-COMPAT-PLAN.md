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
| 3 | `0x01` on class 0x6C, **2-byte** instance; walk `next_instance_id`, recurse `nesting_…_id`, all re-queried as 2-byte | ✅ reply format exact, **but** member/nested ids are synthetic **>0xFFFF** (`member_id_encode`) → aphyt's `id.to_bytes(2)` overflows/crashes | give members **real ≤0xFFFF** instance ids | ✅ exact (except ids) |
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
`instance_size` → size, atomic `tag_type_t` → OMRON `cip_data_type` byte (new small map,
confirmed below), and for a UDT tag put its **template id** at `[8:12]` (must be ≤0xFFFF
— it already is, see Phase 3).

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
`next_template_id` counter templates already use, capped at 12 bits
(`0x0FFF`) — not a standalone 16-bit space. Rockwell's struct-tag encoding
(`DEVICE_SIM_STRUCTURE_TYPE`, `tag_type_t`'s high nibble + low 12 bits) already
caps template ids to 12 bits, so sharing one counter keeps template and
member ids trivially disjoint without a flag-bit scheme, and both dialects
draw from the one id space instead of each inventing its own. See
`device.h`'s `udt_member_id_t` / `device_udt_find_member` /
`device_udt_member_instance_id`.

The class-0x6C reply format already matches `VariableTypeObjectReply` byte-for-byte
(`encode_variable_type_reply`, incl. the pad-when-even rule). The **only** problem:
`_structure_instance_from_variable_type_object` re-queries `next_instance_id` and
`nesting_variable_type_instance_id` as **2-byte** instances
(`instance_id.to_bytes(2,'little')`), but the server hands back **synthetic ids
>0xFFFF** (`member_id_encode` sets bit 24, `omron_listing.c:213`). aphyt's `.to_bytes(2)`
overflows → crash.

Fix: replace synthesis with a real ≤0xFFFF instance registry.
- device_sim allocates a sequential 16-bit id per synthetic member entry at UDT-register
  time, each mapping to `(template_id, member_index)`; nested-UDT members point
  `nesting_variable_type_instance_id` at the nested template's own (already ≤0xFFFF) id.
  Cap top-level template ids and member ids into disjoint ≤0xFFFF ranges.
- `handle_variable_type` (`omron_listing.c:382`) looks up the incoming 16-bit instance in
  that registry instead of `member_id_decode`.
- Keep the `next_instance_id` **chain** (one member per reply) — aphyt expects exactly
  that (`while member_instance_id != 0`), and it's what the server already emits; only
  the id values change.

The libplctag client currently consumes the synthetic ids too — check
`enip_omron_apply_listing`/`omron_udt_reply_links` still round-trips with real 16-bit ids
(it reads `next_instance_id` opaquely, so it should; verify in the omron_udt_walk test).

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
   sequences (0x5F user/system, symbolic 0x01, 0x6C chain) at the CIP dispatch, and
   `assert` the reply bytes at the offsets aphyt reads (list record fields, `[8:12]`
   type-id, `next_instance_id` ≤0xFFFF, member names). One check per gap 1–3. In-sandbox,
   no network.
2. **aphyt acceptance** (manual): `pip install aphyt`, point its `NSeries` at a running
   device-sim server, call `update_variable_dictionary()` + `variable_list()` and the
   UDT walk, diff against the configured tag/UDT set. This is the real "done" gate — the
   self-test only proves the bytes we *believe* aphyt sends; running aphyt proves it.
   Document under `run_enip_tests.sh` (network/HW section).

---

## 7. Risks / open questions

- **Member-id registry (Phase 3)** is the one non-trivial change: disjoint ≤0xFFFF id
  ranges for templates vs synthetic members, and keeping the libplctag *client* walk
  (`omron_udt_reply_links`) working with the new ids. Covered by the existing
  `omron_udt` test + the new self-test.
- ~~OMRON CIP data-type byte map~~ resolved — see the table in Phase 2, pulled from
  aphyt's `cip_datatypes.py`.
- **Family gating**: the symbolic-`0x01` and `0x80`-segment branches must not regress
  Logix/PCCC servers — gate on OMRON-family / 0x6A-0x6C-registered.
- **aphyt bootstrap calls** not on the enumerate path (class-0x6C instance-0, class 0x6B,
  `_get_number_of_variables`) — the acceptance run will reveal if aphyt actually issues
  any; each is a small additive handler if so. Not blocking.

---

## 8. Files touched

- `dialects/omron/omron_listing.c` — Phase 1 (kind filter), Phase 3 (real member-id
  registry lookup). Bulk of the work.
- `common/cip.c` — Phase 2 routing (symbolic `0x01` → new handler), Phase 4 `0x80`
  verify/patch.
- `dialects/omron/omron_client.c` or new `omron_types.c` — `tag_type_t → OMRON CIP
  data-type byte` map (Phase 2), sourced from aphyt `omron_datatypes.py`.
- `server/device_sim.{c,h}` — `bool system` on `tag_def_t` (Phase 1); 16-bit member-id
  registry (Phase 3).
- `src/tests/…` + `run_enip_tests.sh` — Phase 5.
- `OMRON-SPECIFIC-DESIGN.md` — record the aphyt (stock-firmware) dialect, citing the
  aphyt source offsets, alongside the existing client subset.

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
   2 (symbolic metadata) have **no dedicated self-test yet** — both build clean and are
   logically straightforward, but nothing in-tree exercises `kind=1` vs `kind=2` or a
   symbolic `0x01` request end-to-end. A raw-socket test (RegisterSession + hand-built
   CIP request bytes, same loopback pattern as `omron_udt_walk`) is the natural fit;
   not yet written.

Stop-and-confirm after Phase 2 (below) still applies — real aphyt hasn't been run
against this yet.

Stop-and-confirm after Phase 2: run real aphyt `update_variable_dictionary()` +
`variable_list()` against the server. If the tag list is correct, the dispatch-integration
approach is proven and Phase 3 is just the UDT extension.

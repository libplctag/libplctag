# ENIP Updates & Refactor Plan

Scope: the new ENIP engine under `src/libplctag/protocols/enip/`
(`client/`, `common/`, `server/`, `dialects/`) plus its public API surface in
`src/libplctag/lib/`. The `attic/` tree is dead code slated for deletion and is
out of scope. Numbering follows the request.

---

## 1. "New chunk of data" event

### Current state
- `PLCTAG_EVENT_DATA_RECEIVED` (value 8, `libplctag.h`) already exists for the
  **inbound** direction and is documented as "one or more may follow, terminated
  by READ_COMPLETED." It is currently only fired from `enip_discover.c`
  (discovery listing), *not* from the ordinary read/fragmented-read path in
  `enip_session.c`.
- There is **no** event for the **outbound** direction — the batch packer
  (`build_batch_request`, CIP Multiple Service Packet, service `0x0A`) and the
  fragmented-write path (`CIP_WRITE_FRAG`, `frag_offset` loop) emit nothing per
  chunk.

### The question: package-time vs. send-time
**Recommendation: fire at actual send time, not at packaging time.**

The client is a single-connection state machine
(`enip_session.c: step_sending`). A chunk is "packaged" when it is assembled
into the tx buffer, but packaging can still be abandoned before transmission
(budget overflow forcing a re-split, connection reset, retry). Firing at
packaging would report progress that did not happen.

The observable, non-retractable milestone is the socket write completing —
`step_sending` reaching `c->tx_off == c->tx_len` and transitioning to
`CONN_WAITING`. That is also symmetric with `DATA_RECEIVED`, which fires when
bytes actually *arrived*, not when a request was queued. "Sent" here means
"handed to the OS socket buffer" — the same best-available guarantee
`DATA_RECEIVED` gives for the inbound side. Good enough; the peer-ack level is
not observable to us and no other event promises it.

### Plan
1. Add `PLCTAG_EVENT_DATA_SENT` to the enum in `libplctag.h` (next free value;
   bump `PLCTAG_EVENT_MAX`). Document it as the outbound mirror of
   `DATA_RECEIVED`: fired once per EIP packet actually transmitted, zero or more
   times before `WRITE_COMPLETED` (or `READ_COMPLETED` for the request side of a
   read).
2. Raise it from the one place all outbound EIP traffic funnels through —
   `step_sending`, at the `tx_off == tx_len` completion point — carrying the tag
   whose op owns the current tx. This covers batch (`0x0A`) and fragmented
   writes with no per-callsite duplication.
3. **Open decision (one line needed):** do you want it per-*sub-request* inside a
   Multiple Service Packet, or per-*EIP-packet* (one `0x0A` = one event)?
   Per-packet is the lazy correct default (it matches "a chunk of data sent");
   per-sub-request means unpacking the batch we just packed to count members.
   Recommend per-packet unless a caller needs sub-request granularity.

*Skipped:* per-sub-request granularity. Add when a caller actually needs to
correlate individual batched writes to progress ticks.

---

## 2. Registering UDTs on server / device_sim tags

### Current state
- The internal API already supports it: `device_sim_add_udt_type()` +
  `DEVICE_SIM_STRUCTURE_TYPE(id)` as a tag's type (`device_sim.h`). The server
  correctly serves such a template over CIP class `0x6C` and it walks under
  `@tags`/`@udt` (verified by `src/tests/omron_udt_walk`).
- **The gap:** that API is *not installed* and is only reachable from C code that
  links the internals. The public `role=server` path
  (`eip_server_tag_create`, driven entirely by attribute strings) has no way to
  (a) declare a template or (b) mark a tag as an instance of one. Every server
  tag today is a flat atomic-type buffer.

### Plan (attribute-string driven, matching the rest of `role=server`)
Two new attributes on `role=server` tags, parsed in `eip_server_tag_create`:

- `udt=<name>:<member>,<member>,...` where each `<member>` is
  `name:type[:count]` (type = the same `elem_type` vocabulary already parsed by
  `lookup_cip_type`, or `@<udtname>` to nest a previously-declared template).
  Offsets and `instance_size` are computed from member sizes + natural
  alignment (a controller-accurate packer, since real UDT decoders read the
  offsets off the wire — this is the one place laziness would produce wrong
  bytes, so it gets the real alignment logic).
- A tag whose `elem_type=@<udtname>` becomes an instance of that template
  (resolve name → `template_id`, set `DEVICE_SIM_STRUCTURE_TYPE(id)`).

Template registration is endpoint-scoped (like `sim_delay_ms`): the first tag
to name a given `udt=` on an endpoint registers it via
`device_sim_add_udt_type` before `device_sim_start`; later tags on the same
endpoint resolve by name. Ordering constraint (templates before the tags that
use them, before start) already matches the existing "all `add_*` before
`start`" rule.

**Alternative considered and rejected:** exposing `device_sim_add_udt_type`
as an installed public C function. Rejected — it bakes a struct-array ABI
(`udt_member_t`) into the public surface for an experimental feature, and the
whole point of `role=server` is that FFI callers drive everything through
attribute strings, not structs.

*Skipped:* a standalone template-declaration API and nested-array-of-UDT
members. Add nesting depth / arrays-of-structs when a test needs them; the
`udt=` grammar above leaves room (`@name` + `:count`).

*Check to leave behind:* extend `src/tests/omron_udt_walk` (or a sibling
`server_udt` test) to declare a UDT purely via the `role=server` attribute
string and read it back through a client tag — one assert that the walked bytes
contain the member names.

---

## 3. Refactor — deep dive, driven by total code size

Baseline (excluding `attic/`): the new ENIP tree is **15,265 lines**
(client 8,050 / common 2,900 / server 2,600 / dialects 1,700 including
headers). `attic/` is another **6,699 dead lines**. Guiding constraint
(3.1–3.3): same functionality, shorter call chains, less duplicated
null-checking, sharing without a forest of `if(dialect==...)` — table/vtable
dispatch over branch ladders; one funnel function over N callers repeating
the same guard.

Findings below are ordered by estimated net line savings. Total estimated
reduction: **~7,900 lines deleted outright (attic) plus ~1,700–2,100 lines of
live code** (~13% of the live tree), with every remaining wire format defined
in exactly one place.

### 3.0 — Delete `attic/` (−6,699 lines, zero risk)

Not referenced by any CMakeLists.txt; pure dead weight from the abandoned
first design. It also keeps polluting every grep/audit of the module (the
`int`/`unsigned` counts, symbol searches, etc.). Delete the directory; git
history preserves it. **Do this first** — every later measurement gets
cleaner.

### 3.a — One EIP/CPF codec instead of two (−~350 lines, kills a whole class of drift bugs)

The client and server each carry a **complete, independent implementation of
the same two wire formats**:

| Layer | Client copy | Server copy |
|---|---|---|
| EIP 24-byte header encode+decode | `client/enip_eip.c` (141 ln, `enip_eip_hdr_t`, `enip_eip_encode/decode`) | `common/eip.c` (`eip_hdr_t`, `eip_parse_hdr`/`eip_encode_hdr`) |
| CPF wrap/unwrap (null-addr + unconnected data; conn-addr + seq + connected data) | `client/enip_cpf.c` (99 ln, `CPF_NULL_ADDR`…) | `common/cpf.c` (249 ln, `CPF_ITEM_NULL_ADDR`…, parse/encode struct pairs) |

Same offsets, same constants under two names, same length math — a change to
either side can silently diverge from the other (the exact bug family the
`eip_session_set_unconnected_sizes` comment describes fixing). **Plan:** make
`common/eip.{c,h}` and `common/cpf.{c,h}` the single codec: one header struct,
one encode, one decode, one set of item-type constants, used by both the
client session and the server dispatcher. The server keeps its *dispatch*
functions (`eip_dispatch`, `cpf_handle_*`) but they call the shared codec;
`client/enip_eip.c` shrinks to the 5 tiny request constructors
(register/unregister/list-identity/rr-data/unit-data), `client/enip_cpf.c` is
deleted. Direction-independence is what makes this safe: a codec has no
client/server opinion.

### 3.b — One CIP logical-path codec instead of five (−~250 lines)

Five hand-rolled parsers/encoders of the *same* logical segment grammar
(0x20/0x21/0x22 class, 0x24/0x25/0x26 instance, 0x28/0x29/0x2A index,
0x30/0x31 attribute — all "1-byte tag, optional pad, N-byte value"):

1. `common/cip.c: parse_class_instance_path_wide` (66 ln, full width support)
2. `common/cip.c: parse_class_instance_path` (17 ln, 8-bit only — already
   subsumed by #1 in the same file; delete, point `handle_identity` at #1)
3. `dialects/rockwell/ab_listing.c: parse_start_instance` (33 ln)
4. `dialects/omron/omron_listing.c: parse_instance` (similar, plus 0x26)
5. `common/cip.c: parse_tag_path`'s index-segment loop (0x28/0x29/0x2A arms,
   ~45 ln of the same skip-pad-read-value shape)

Plus **three encoders** of the same segments on the client:
`enip_cip.c: cip_class_inst_path` (16-bit hardcoded), the inline 0x26 special
case in `enip_cip_omron_udt_get_all`, and `enip_cip_encode_array_index`.

**Plan:** one table-driven pair in `common/cip_path.{c,h}`:

```c
/* seg kind × width from one 9-row table {tag, pad, width, field} */
extern bool  cip_path_parse(Bytes path, cip_path_ids_t *out);   /* class/instance/attr/member-indexes */
extern Bytes cip_path_encode(Arena *a, uint32_t class_id, uint32_t instance_id,
                             int64_t attr_id /* <0 = none */);  /* picks narrowest width per value */
```

All five parse sites become one call each; the three encode sites likewise
(`cip_path_encode` picking width-by-magnitude automatically also deletes the
0x26 special case added for the OMRON member walk). The width-selection logic
appears once instead of being re-derived at each site — this is also where the
OMRON bit-24 flag bug of last session lived; a single codec means a single
place to test.

### 3.c — CBOR presentation: field tables instead of hand-rolled size/write pairs (−~450 lines)

Three read-only metadata tag kinds render CBOR (`@identity` in `enip_tag.c`,
PCCC listing in `enip_tag.c`, UDP discovery in `enip_discover.c`). Each carries
the same six hand-written functions: `record_cbor_size`, `record_cbor_write`,
`envelope_cbor_size`, `envelope_cbor_write`, `schema_cbor_size`,
`schema_cbor_write`, plus 2–4 vtable adapters — **~750 lines total**, and every
field name is spelled **four times** (record size, record write, schema, field-name
array). The primitives (`utils/cbor.{c,h}`) are already shared; it's the layer
above that's copy-pasted.

**Plan:** a small descriptor-driven emitter in `utils/cbor_schema.{c,h}`
(protocol-agnostic — item 3.5):

```c
typedef struct { const char *name; uint8_t kind /* uint|text */;
                 uint16_t offset, len_offset; } cbor_field_t;
extern bool   cbor_emit_record(Bytes dest, size_t *pos, const cbor_field_t *fields, size_t n, const void *rec);
extern size_t cbor_record_size(const cbor_field_t *fields, size_t n, const void *rec);
extern bool   cbor_emit_schema(Bytes dest, size_t *pos, const char *schema_name, uint32_t version,
                               const cbor_field_t *fields, size_t n);
/* + one shared envelope writer: {"schema","schema-version","records":[...]} */
```

Each tag kind then supplies one `cbor_field_t[]` table (~10 lines) plus its
existing raw-bytes→struct parse function. Fields are named once. A future
schema'd tag kind (e.g. item 2's `@udt` CBOR, if ever wanted) costs a table,
not 250 lines. Estimated: ~750 → ~300 (engine ~150 + 3 tables + adapters).

*Cheaper fallback if the table feels heavy:* give `cbor_write_*` a "null dest
= count only" mode and delete all `*_size` twins — halves the duplication
(~-250) with zero new abstraction. The table version is preferred because the
4× field-name spelling is the actual maintenance hazard, but either is a
strict improvement; pick one, don't do both.

### 3.d — Move the dialects out of `enip_session.c` and finish the seam (−~1,100 lines from the file; ~−150 net)

`enip_session.c` (3,183 lines) currently contains, beyond the session/state
machine it's named for:

- Logix build/apply (`enip_logix_build`, `enip_logix_apply`, ~190 ln)
- The whole client PCCC dialect (`pccc_write_header`,
  `enip_pccc_build_plc5_bit_write`, `enip_pccc_build_slc_bit_write`,
  `enip_pccc_build`, `enip_pccc_apply`, + listing pair, ~350 ln, plus its own
  `#define PCCC_*` constant block that **re-defines opcodes already defined in
  `dialects/pccc/pccc.c`** — 0x4B/0x0F/read/write/RMW fncs, vendor id/sn)
- All three listing dialects' build/apply (`enip_logix_build_listing`/
  `apply_listing`, `enip_omron_*`, `enip_pccc_*_listing`, + the OMRON walk
  helpers, ~400 ln)
- Identity classify glue, `identity_plc_type_name` (~120 ln)

**Plan:** move each dialect's build/apply into its `dialects/<family>/`
directory as `<family>_client.c`, reachable *only* through the existing
`enip_dialect_t` vtable (`phase12_dialect_seam` did the seam; this finishes
the migration so the seam is the only contact point — directly item 3.1's
"share without if/else ladders"). Merge the duplicated PCCC opcode constants
into `dialects/pccc/pccc_defs.h` used by both client and server halves. Net
line change is modest (code moves more than it shrinks; the deletions are the
duplicate constants and the per-dialect `listing_dialect_for` glue), but
`enip_session.c` drops to ~1,900 lines of actual session logic, and 3.e/3.f
below become reviewable.

### 3.e — Op-completion table (fixes the recurring bug class; ~−60 lines)

`complete_tag`, `is_batch_eligible`, `is_network_op_kind`, `rearm_time`, and
`apply_tag_reply`'s outer dispatch all re-enumerate `ENIP_OP_*` in `||`
ladders. The OMRON hang fixed last session was precisely a missing arm in one
of them. **Plan:** one static table `op_traits[ENIP_OP_COUNT]` with flags
(`is_read_like`, `is_write_like`, `batchable`, `terminal`, `special`) and the
apply handler pointer. Every ladder becomes a table lookup; adding an op
forces (via designated-initializer + `_Static_assert` on count) a conscious
choice for every trait. This is item 3.3 verbatim.

### 3.f — Split `apply_tag_reply` (311 lines, the single biggest function)

It handles OPEN_PROBE, OPEN_PROBE_FRAG, READ, READ_FRAG continuation, WRITE,
WRITE_FRAG windowing, and LIST/UDT ops in one `if/else if` chain with repeated
"unpack header, check FRAG status, grow buffer, copy chunk, advance
frag_offset" blocks. **Plan:** extract the shared fragment-assembly helper
(`frag_accumulate(t, data, header_len, status)` — the grow/copy/advance dance
appears 3×) and split the rest per op into the 3.e table's handlers. Net size
roughly flat, but max function length drops ~311→~60 and the null/bounds
checks (`bytes_is_null` after every unpack) collapse into the one helper —
item 3.3's "avoid null-check duplication".

### 3.g — Server PCCC handler skeleton (−~120 lines)

`dialects/pccc/pccc.c` has six handlers (`handle_plc5_read/write/rmw`,
`handle_slc_read/write/rmw`, ~40–50 ln each) sharing the same skeleton:
parse address fields → `find_tag_by_file_num` → bounds check → copy/modify →
`pccc_error` on each failure. **Plan:** one `pccc_locate(cmd, dev, &tag,
&start, &len, &err)` helper for the shared front half; the six handlers keep
only their genuinely distinct back halves (PLC5 vs SLC addressing width, RMW
masking). Same pattern, smaller scale, as 3.f.

### 3.h — Type tables: one source of truth (−~60 lines)

Three places map CIP/PCCC type ↔ element size: `eip_server_tag.c`'s
`cip_type_table[]`/`pccc_type_table[]` (name→type+size),
`device_sim.c: device_elem_size_for_type` (type→size), and item 2 will need
name→type again for UDT members. **Plan:** move the two tables into
`server/device_types.{c,h}` (or into `device_sim.c` next to
`device_elem_size_for_type`, deriving that function from the table) and let
`eip_server_tag.c` and item 2's UDT parser share the lookup. Element sizes
stop being spellable in two places.

### 3.i — Identity: shared field codec (−~80 lines)

The identity body (vendor/device_type/product_code/rev/status/serial/name
/state) is encoded in `common/identity.c` (server) and **independently parsed
in three client places**: `enip_session.c: on_identity_reply`,
`enip_discover.c: enip_udp_parse_record_at`, and
`enip_tag.c: enip_identity_parse_fields`. **Plan:** add
`identity_decode(Bytes, identity_t *out)` next to the existing
`identity_encode_*` in `common/identity.c`; all three client sites call it and
keep only their site-specific handling (IP extraction for discovery, CBOR for
the tag). Encode and decode of one wire format live in one file — same
principle as 3.a.

### 3.4 + 3.5 — Factor out the protocol-agnostic TCP server

The piece the request calls out explicitly. `server/server.c` currently
hard-codes an EIP-shaped loop, but the
**listener + connection-registry + accept-loop + blocking recv/send helpers are
protocol-agnostic**. Only two things are CIP-specific:
- the framing (read 24-byte EIP header → length-prefixed payload), and
- the dispatch (`eip_dispatch`).

**Plan:** split `server.c` into

- `common/tcp_server.{c,h}` (protocol-agnostic): `registry_*`, `recv_exact`,
  `send_all`, the listener thread, the accept→spawn loop, per-connection thread
  lifecycle, terminate/wake plumbing. It calls back into the protocol via a
  small vtable:
  ```c
  typedef struct {
      /* read exactly one framed request into the arena; return payload Bytes
       * or null on close. Owns the "how many bytes is this message" logic. */
      Bytes (*read_frame)(Arena *a, sock_p sock, void *dev, /*io helpers*/);
      /* produce a response for one request; null = close connection. */
      Bytes (*dispatch)(Arena *a, Bytes frame, void *session, void *dev);
      size_t session_size;   /* per-conn session struct size to zero-init */
      void (*session_init)(void *session, void *dev, sock_p sock);
  } tcp_proto_ops_t;
  ```
- `server/eip_server.c` keeps only the EIP `read_frame` (header+payload) and
  wires `eip_dispatch` in. ~40 lines instead of the current 383.

**Shareable with Modbus / future S7:** Modbus TCP is currently **client-only**
(`protocols/mb/modbus.c` has no listener), so there is no server to dedup
against *today*. But a Modbus-TCP server would be *only* a `read_frame` (6-byte
MBAP header → length field) + a dispatch function over this same
`tcp_server.c`. Same for a hypothetical S7. So the win is "future server
protocols are ~one framing function each," not "delete duplicate code that
exists now." Worth doing now because it is nearly free (it is a cut, not new
abstraction) and it is what makes item 2's testing and any future protocol
cheap.

*Ladder check:* one implementation exists today (EIP). Normally "no interface
for one impl" — but the request explicitly asks for this seam and Modbus/S7 are
named concrete second/third consumers, so the abstraction is requested, not
speculative. Keep the vtable to exactly the two callbacks above; do not add
knobs no framing needs.

**UDP twin:** the same split applies to the two UDP loops —
`server/discovery.c: discovery_thread` and
`client/enip_discover.c: enip_discover_run_scan` each hand-roll
bind/recvfrom/timeout/terminate handling. Smaller win (~80 lines) but the same
shape; fold a `udp_poll_loop(sock, on_datagram, terminate_flag)` helper into
the same `tcp_server.c`/`net_server.c` file rather than a separate module.

Other protocol-agnostic helpers already correctly factored (`arena.h`,
`bytes.h`, `cbor.h`) — leave them. Additions surfaced during this audit:

- `host:port` splitting: duplicated in `enip_session_create`,
  `eip_server_tag_create`, and `modbus.c` (~25 ln each with error handling) →
  one `split_host_port()` in utils. Removes three copies including their
  subtly different `mem_free` cleanup paths (item 3.3: the eip_server_tag
  version needs the free at five early returns — a helper returning owned
  strings collapses that).
- `utils/enip_wait.{c,h}` is protocol-named code sitting in the *generic*
  utils directory — the inverse misplacement. Nothing about
  `socket_read_wait/socket_write_wait/socket_connect_wait` is ENIP-specific;
  rename to `utils/socket_wait.{c,h}` (types `sock_wait_*`) or fold into
  platform. Cheap, and stops the next protocol from either duplicating it or
  importing "enip_" symbols.

### 3.1–3.3 — What remains of `enip_session.c` after 3.d–3.f

Sections 3.d (dialects out), 3.e (op-trait table), and 3.f (apply_tag_reply
split) do most of the work items 3.1–3.3 ask for. After them,
`enip_session.c` is ~1,700–1,900 lines of genuinely session-shaped code:
connection registry, scheduler (`sched_*`, `pick_batch`, `service_due_tags`),
socket state machine (`step_*`), batch pack/unpack, forward open/close. Only
split further if that file still reads poorly afterwards — a second-round
`enip_conn.c` (socket steps) / `enip_batch.c` (MSP pack/unpack) cut is
mechanical at that point, but don't pre-commit; the module boundary that
matters (dialects behind the seam) is already 3.d. (YAGNI on file-splitting
for its own sake.)

Call-chain note (3.2): today a read reply travels
`io_thread → step_waiting → handle_tag_reply → apply_tag_reply →
dialect apply → complete_tag`, with `handle_batch_reply` adding a parallel
near-copy of `handle_tag_reply`'s unwrap (EIP hdr → CPF unwrap → CIP parse).
After 3.a (single codec) both funnel through one
`unwrap_cip_reply(payload, connected, &cip)` — the fan-in the request calls
out.

Sequencing for all of section 3: **3.0 (attic) → 3.4/3.5 (server cut, low
risk) → 3.a/3.b/3.i (codecs; pure dedup, each independently testable against
the loopback tests) → 3.d (move dialects) → 3.e/3.f (op table + apply split)
→ 3.c (CBOR tables) → 3.g/3.h (small server dedups)**. Each step leaves the
build green and the `src/tests` loopback suite passing; no step depends on a
later one.

### 3.6 — `int`/`unsigned` → fixed-width; `bool` for flags

Audit (excluding `attic/`): ~205 raw `int`/`unsigned` occurrences —
client 85, common 44, server 49, dialects 27. Mechanical but must be
reviewed, not sed'd, because some are genuinely correct (`int rc` from platform
APIs that return `int`, `int` params dictated by the vtable signatures in
`tag.h`, loop indices over `vector_length` which returns `int`). Rules
(from memory `feedback_coding_guidelines` / `feedback_bool_not_uint8`):
- sizes/offsets/counts on the wire → `uint32_t`/`size_t`/`uint16_t` as
  appropriate;
- any variable used as a flag → `bool` + `true`/`false`, never `int 0/1`;
- leave `int` only where a platform/vtable boundary dictates it, with the
  conversion localized at the boundary.

Do this **per-file as each file is touched** by the steps above, not as a
separate sweeping pass — a standalone 200-site diff is unreviewable and
collides with the restructuring. The vtable boundary in `lib/tag.h` (`int`
params) stays as-is; it's the public-ish internal ABI.

### Section 3 size ledger (estimates)

| Step | Change | Net lines |
|---|---|---|
| 3.0 | delete `attic/` | **−6,699** |
| 3.a | one EIP/CPF codec | −350 |
| 3.b | one CIP path codec | −250 |
| 3.c | CBOR field tables | −450 |
| 3.d | dialects out of session file (+ dup PCCC consts) | −150 |
| 3.e | op-trait table | −60 |
| 3.f | apply_tag_reply split | ~0 (clarity) |
| 3.g | PCCC handler skeleton | −120 |
| 3.h | type-table merge | −60 |
| 3.i | identity decode shared | −80 |
| 3.4/3.5 | tcp_server extraction + host:port + UDP loop | −150 |
| | **live-code total** | **~−1,700** |

---

## 4. Structured-data API must not exist when ENIP is off

### Current state (this is exactly the anti-pattern the request forbids)
`lib.c:4158` guards the six functions with `#if LIBPLCTAG_FEATURE_ENIP` **but
the `#else` branch stubs them** to return `PLCTAG_ERR_UNSUPPORTED`, and
`libplctag.h` **always declares** all six + the `plc_tag_format_type_t` enum.
So with ENIP off the symbols still exist and the interface is frozen — precisely
the "stub bakes in the interface" problem.

Root cause: `libplctag.h` is installed **COPYONLY** and cannot see
`plctag_features.h` (which is generated but **not installed**), so the header
has no compile-time knowledge of the feature.

### Plan
Make the declarations and definitions physically absent when the feature is off:

1. **Generate the public header instead of copying it.** Change
   `src/libplctag/CMakeLists.txt` to `CONFIGURE_FILE(... @ONLY)` `libplctag.h`
   (from a `.in`) rather than `COPYONLY`, and wrap the enum + the six
   declarations in `#if @LIBPLCTAG_FEATURE_ENIP@ ... #endif` (stamped to `#if 1`
   / `#if 0` at configure time). When ENIP is off, the installed header does not
   declare them at all — a caller referencing them gets a compile error, not a
   silent `UNSUPPORTED`. This is the option that best matches "do not exist."
2. **Delete the `#else` stubs in `lib.c`.** Keep only the `#if
   LIBPLCTAG_FEATURE_ENIP` implementation; no fallback.
3. Nothing else references these six symbols internally when ENIP is off (the
   only implementors are the ENIP tag vtables), so no other guards are needed.

**Alternative considered:** install `plctag_features.h` and `#include` it from
`libplctag.h`. Rejected — it leaks every internal feature macro into the public
namespace and adds a second installed header, for no gain over stamping one
value into the header we already install.

*Trade-off to confirm:* option 1 means the installed `libplctag.h` becomes
build-configuration-specific (an ENIP-on install and an ENIP-off install ship
different headers). That is the *intended* semantics of "the API doesn't exist
when the feature is off," but it does mean you can't ship one universal header.
Acceptable given the feature is explicitly experimental/beta-gated.

---

## Target architecture (complete-refactor end state)

The structural diagnosis behind all of section 3: **today's `common/` is not
common.** Every entry point in `common/eip.c`, `common/cpf.c`, and
`common/cip.c` takes a `device_t *` (and `eip_session_t *` — the *server's*
session) — it is server code with a misleading directory name. That is *why*
the client grew its own second copy of every codec. The fix is a real layering:
wire knowledge in direction-free codecs, behavior in two small engines,
dialects as plug-ins that ship with their own simulator half and loopback test.

Rules that hold the design together:

1. Every function in `wire/` is `Bytes → struct` or `struct → Bytes`,
   arena-allocated, **no I/O, no `device_t`, no session pointer**. Fully
   unit-testable with golden packets, shared by both engines by construction.
2. A dialect's client half and simulator half live in one directory and are
   reached only through the `enip_dialect_t` vtable — never `if(plc_type==)`.
3. Two engines keep two runtime models on purpose: client = nonblocking state
   machine on the shared io thread; server = blocking thread-per-connection
   (a simulator optimizes for obvious correctness, not scale). They share
   `wire/` and `net/`, nothing else.
4. No protocol abstraction above ENIP/Modbus beyond `lib/tag.h`'s existing
   vtable — that boundary already exists; a "session superclass" is the
   speculative framework the attic design died of.

### Directory and file layout

```
src/net/                          (~600 ln, protocol-agnostic — item 3.4/3.5)
src/utils/cbor_schema.{c,h}       (~150 ln — item 3.c)
protocols/enip/
    wire/       eip.c cpf.c cip.c cip_path.c identity.c pccc.c   (~1,400 ln)
    client/     session.c scheduler.c ops.c tags.c discover.c    (~3,300 ln)
    server/     device.c dispatch.c server_tag.c endpoint.c      (~2,300 ln)
    dialects/   dialect.h rockwell/ omron/ pccc/                 (~2,200 ln)
```

### `src/net/` — protocol-agnostic I/O

**`net/tcp_server.{c,h}`** (from `server/server.c` minus EIP framing)
- Socket registry (`registry_*`), `recv_exact`/`send_all`, listener thread,
  accept→spawn loop, per-connection thread lifecycle, terminate/wake plumbing.
- Protocol plugs in via the two-callback `tcp_proto_ops_t` (§3.4): `read_frame`
  (owns "how many bytes is one message") and `dispatch` (frame → response, null
  = close). Per-conn session zero-init via `session_size`/`session_init`.

**`net/tcp_client.{c,h}`** (from `enip_session.c: step_connect/step_sending/
step_waiting` + `utils/enip_wait.c` renamed)
- Nonblocking connect/send/recv steps over a tx/rx buffer pair; framing
  callback `size_t (*frame_total)(Bytes partial)` returns the full-message
  length once enough header has arrived (ENIP: 24-byte header's length field;
  Modbus: MBAP; S7: TPKT). Returns PENDING/OK/error; owns no protocol bytes.

**`net/udp_poll.{c,h}`** (from `server/discovery.c: discovery_thread` loop +
`client/enip_discover.c: enip_discover_run_scan` loop)
- `udp_poll_loop(sock, timeout_ms, terminate_flag, on_datagram, ctx)` —
  bind/recvfrom/timeout/terminate handling written once. Callers keep only
  their datagram handlers.

Also here: `split_host_port()` (the three copies in `enip_session_create`,
`eip_server_tag_create`, `modbus.c`).

### `protocols/enip/wire/` — pure codecs, direction-free

**`wire/eip.c,h`** (merge of `common/eip.c`'s codec half + `client/enip_eip.c`)
- One `eip_hdr_t`; `eip_hdr_encode/decode`; `ENIP_CMD_*` constants (one set).
- Request/response constructors: register/unregister session, list identity,
  send-RR-data, send-unit-data, plus `eip_error()`.
- The session-size bookkeeping (`eip_session_set_*_sizes`) moves to the
  engines — it is state policy, not wire format.

**`wire/cpf.c,h`** (merge of `common/cpf.c` codec half + `client/enip_cpf.c`)
- Generic item-list walker: `cpf_parse(Bytes, cpf_items_t *out)` handling any
  item count/order (today's server rejects anything but exactly
  [addr, data]; the walker keeps that check in the *caller*), plus
  `cpf_wrap_unconnected/connected`. One set of `CPF_ITEM_*` constants.

**`wire/cip.c,h`** (from `client/enip_cip.c` builders + `common/cip.c`'s
parse/error helpers + both MSP implementations)
- Request/reply header codec: `cip_req_parse` (= today's `parse_cip_request`),
  `cip_reply_parse` (= `enip_cip_parse_reply`), `cip_error()` builder.
- Request builders: read/write/read-frag/write-frag, tag-list, UDT meta/fields,
  OMRON get-all, forward open/close bodies (from `build_forward_open/close` —
  the *bytes*, not the session-state updates).
- Multiple Service Packet: `cip_msp_pack(a, subs[], n)` /
  `cip_msp_unpack(data, subs[], max)` — one offset-table implementation
  replacing `handle_multi`'s and `build_batch_request`'s independent copies.
- One `CIP_SRV_*` / `CIP_ERR_*` constant set (today: `common/cip.c` and the
  client each define their own).

**`wire/cip_path.c,h`** (item 3.b — replaces 5 parsers + 3 encoders)
- Logical paths: `cip_path_parse(Bytes, cip_path_ids_t *)` /
  `cip_path_encode(a, class, instance, attr)` — width (8/16/32-bit segment)
  from one 9-row table, chosen by magnitude on encode.
- Symbolic paths: `cip_symbolic_encode(a, "TagName[3].member")` (from
  `enip_cip_encode_tag_path`) and `cip_symbolic_parse(Bytes, name_out,
  indexes_out)` — the parse half of today's `common/cip.c: parse_tag_path`
  **without** the tag lookup: resolving a parsed name to a `tag_def_t` is the
  server engine's job (`device.c`), not the codec's. That split is what makes
  this file direction-free.

**`wire/identity.c,h`** (from `common/identity.c` + item 3.i)
- `identity_t` codec: `identity_encode_attrs_all/attr_single` (existing),
  new `identity_decode(Bytes, identity_t *)` used by all three client parse
  sites, list-identity CPF item encode/decode, and the model table
  (`IDENTITY_TABLE`) with its classify counterpart (`plc_classify.c` merges in
  here — it is the decode-side of the same table).

**`wire/pccc.c,h`** (constants + frame codec only)
- One set of PCCC opcodes/cmd/fnc/vendor constants (today duplicated between
  `enip_session.c` and `dialects/pccc/pccc.c`), execute-PCCC (0x4B) wrapper
  codec, PCCC response header parse, typed-data encode helpers.
- The logical-address *string* parser ("N7:0", `enip_pccc_addr.c`) stays in
  the dialect (below) — it is libplctag attribute syntax, not wire format.

### `protocols/enip/client/` — client engine

**`client/session.c`** (~700 ln)
- Connection registry (`s_conns`, key match, create/destructor), lifecycle
  steps driven over `net/tcp_client`: register session, identity probe +
  classify, ForwardOpen/Close (wire bytes from `wire/cip.c`; state updates
  here), reset/idle-disconnect, conn-status ring, io thread.

**`client/scheduler.c`** (~400 ln)
- `sched_insert_sorted/unlink`, `pick_batch`, `service_due_tags`,
  `rearm_time`, `next_special_wait`, the schedule/unschedule API. Pure
  policy — no wire bytes at all.

**`client/ops.c`** (~500 ln — items 3.e/3.f)
- `op_traits[ENIP_OP_COUNT]` table: `{batchable, terminal, is_read_like,
  is_write_like, apply_fn}` + `_Static_assert` on count.
- One reply funnel: `unwrap_cip_reply(payload, connected, &cip)` (EIP→CPF→CIP
  via wire codecs) used by both single and batch paths; `frag_accumulate()`
  (the grow/copy/advance dance, written once); `complete_tag`/`complete_batch`
  reading the traits table. This file is where item 1's `DATA_SENT` event and
  READ/WRITE_COMPLETED are raised — one place.

**`client/tags.c`** (~900 ln)
- All plc_tag vtables (data, connection, identity, listing) and create-string
  parsing. CBOR presentation shrinks to `cbor_field_t[]` tables +
  `utils/cbor_schema` calls (item 3.c).

**`client/discover.c`** (~400 ln)
- `@discover` tag: scan orchestration over `net/udp_poll`, record dedup,
  its CBOR field table. Identity bytes parsed by `wire/identity.c`.

### `protocols/enip/server/` — server engine

**`server/device.c,h`** (from `device_sim.c` + `device.h`, ~700 ln)
- The model: `device_t`, tag list ops, UDT template registry, CIP object
  registry, identity storage, type tables (name→type→size, single source —
  item 3.h; `device_elem_size_for_type` derived from it), and
  `device_tag_resolve(dev, name, indexes)` — the lookup half split out of
  `parse_tag_path`. Public `device_sim_*` API unchanged.

**`server/dispatch.c`** (from the dispatch halves of `common/eip.c`,
`common/cpf.c`, `common/cip.c`, ~800 ln)
- The funnel: `eip_dispatch` → cpf handling → `cip_dispatch_un/connected` →
  {read, write, frag, forward open/close, multi, identity, object registry,
  PCCC routing}. All parsing/encoding via `wire/`; this file keeps only
  decisions (bounds checks, fault injection, session state, `try_cip_object`
  fallthrough). Six PCCC handlers keep their distinct halves over one
  `pccc_locate()` front (item 3.g).

**`server/server_tag.c`** (from `eip_server_tag.c`)
- `role=server` plc_tag adapter; item 2's `udt=` attribute parsing lands here,
  resolving member type names against `device.c`'s single type table.

**`server/endpoint.c`** — unchanged (already clean: registry + rc lifecycle).

### `protocols/enip/dialects/` — one directory per family

`dialect.h` — the existing `enip_dialect_t` vtable, extended to cover
build/apply for data ops as well as listing (finishing the phase-12 seam).

Each family directory contains exactly three things:

| File | Contents (source) |
|---|---|
| `rockwell/client.c` | `enip_logix_build/apply` + listing build/apply + UDT meta/fields walk (from `enip_session.c`) |
| `rockwell/sim.c` | class 0x6B/0x6C handlers (= today's `ab_listing.c`) |
| `rockwell/defs.h` | family service codes (0x55, template attrs...) used by both halves |
| `omron/client.c` | listing build/apply, member-walk pending stack, `member_id_encode/decode` (from `enip_session.c`) |
| `omron/sim.c` | Variable Type Object handlers (= today's `omron_listing.c`) — encode side of the same §5.3 format the client half decodes, now adjacent |
| `omron/defs.h` | shared §5.3 layout constants + the member-id flag bit |
| `pccc/client.c` | `enip_pccc_build/apply` + bit-write variants (from `enip_session.c`) + logical-address string parser (= `enip_pccc_addr.c`) |
| `pccc/sim.c` | PLC5/SLC read/write/RMW handlers (= today's `pccc.c`) |
| `pccc/defs.h` | opcodes — the set currently defined twice |

Plus one loopback test per family in `src/tests/` (rockwell and omron exist;
pccc gets one) — the drift-catcher that both halves living together enables.

### What this deletes outright

`client/enip_eip.c` + `client/enip_cpf.c` (superseded by `wire/`),
`common/plc_classify.c` (into `wire/identity.c`), `utils/enip_wait.{c,h}`
(into `net/`), the duplicate constant sets (EIP cmd, CPF item, CIP service,
PCCC opcode), and `attic/`. End state ≈ **13k lines on disk vs today's 22k**,
with every wire format defined exactly once.

### Getting there

Not a rewrite — `attic/` is the corpse of the last big-bang attempt. The
section-3 ladder *is* the migration: each step is one move toward this layout
with the build green and loopback tests passing. Mapping: 3.0 → deletes;
3.4/3.5 → `net/`; 3.a/3.b/3.i → `wire/`; 3.d → `dialects/`; 3.e/3.f →
`client/ops.c`; 3.c → `cbor_schema` + tables; 3.g/3.h → `server/` cleanups.
The only *new* work the end state adds beyond section 3 is the final directory
renames (`common/` dispatch halves → `server/dispatch.c`, session file split
into session/scheduler/ops/tags) — do those last, when they are pure `git mv`
plus include fixes.

---

## Suggested order of execution

1. **#3.0** — delete `attic/` (biggest single win, zero risk).
2. **#4** (small, isolated, removes a standing wart).
3. **#3.4 / #3.5** — extract `common/tcp_server.c` (clean cut, low risk).
4. **#2** — UDT-via-attributes (builds on the now-simpler server, adds a test).
5. **#1** — `DATA_SENT` event (needs the send funnel; trivial once #3 hasn't
   moved it, so can also go first if preferred).
6. **#3.a–3.i + 3.6** — the dedup ladder in the sequence given at the end of
   section 3, fixing `int`/`bool` per file as
   touched. Largest and riskiest; do last, behavior-preserving, leaning on the
   existing enip test suite each step.

Two open questions need a one-line answer before starting (both flagged above):
**#1** per-packet vs. per-sub-request granularity, and **#2** whether
arrays-of-UDT / deeper nesting are in scope now or deferred.

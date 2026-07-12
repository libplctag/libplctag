<!--
  Copyright (C) 2026 by Kyle Hayes - kyle.hayes@gmail.com
  This software is available under the MPL 2.0 or LGPL 2 (or later) license.
-->

# Server tags — folding the device simulator into the libplctag API

This document supersedes `src/libdevsim/DESIGN_deprecated.md`. `libdevsim`
ceases to exist as a separate library; its functionality returns to the user as
**server tags** created through the ordinary `plc_tag_create_ex` API. A server
tag *hosts* a value that remote masters read and write; a normal (client) tag
*addresses* a value on a remote PLC. Same API, opposite direction.

The whole feature is gated so embedded users can compile it out.

---

## 1. The model in one paragraph

A server tag is a normal `plc_tag` whose `data` buffer **is** the backing store
that the embedded server hands to remote clients. `plc_tag_set_int32(srv,…)`
writes that buffer; a remote master's read serves from it; a remote master's
write updates it and the local handle sees the change on its next read. There is
no second object type, no `device_sim_t`, no parallel tag list — the libplctag
id table, refcount, `api_mutex`, status, and `create_ex` callback are reused
verbatim. The only genuinely new code is the **server runtime** (listener
thread + protocol decode) and a thin **server tag constructor** per protocol.

---

## 2. Feature gates (`#if`)

Three compile-time switches. Default **all on**; embedded users turn off what
they don't need.

| Macro | Covers | Depends on |
|-------|--------|------------|
| `LIBPLCTAG_FEATURE_EIP` | AB + OMRON client tags **and** the EtherNet/IP server (CPF/CIP, identity, discovery, dialect listing) | — |
| `LIBPLCTAG_FEATURE_MODBUS` | Modbus/TCP client tag **and** the Modbus server | — |
| `LIBPLCTAG_FEATURE_SERVER` | server-tag machinery: endpoint registry, listener thread, server-side decode for whichever protocols are enabled | at least one protocol |

That's the whole switch list. **No** separate `DISCOVERY` or `PCCC` flag —
discovery is part of the EIP server, PCCC is part of the AB/EIP path; splitting
them is YAGNI. `SERVER` with no protocol enabled is a CMake configure error.

**Guarding strategy — files first, `#if` only at seams:**

- A disabled feature's `.c` files are **omitted from the CMake source list**, not
  `#if`'d out wholesale. The file simply isn't compiled.
- `#if` appears only where code from different features meets in one file:
  1. the `tag_type_map[]` rows in `lib/init.c` (a disabled protocol's rows
     vanish → its `protocol=` fails to match → clean `PLCTAG_ERR_NOT_FOUND`);
  2. the `PLCTAG_MODULE_*` debug-enum entries for disabled code;
  3. the `role=server` dispatch hook (compiled only with `LIBPLCTAG_FEATURE_SERVER`).
- Macros come from a generated `plctag_features.h` (CMake `option()` →
  `configure_file`), included where needed. No runtime cost, no stub functions.

---

## 3. Code layout — dissolving libdevsim into the protocols tree

Protocol is the top axis. Below each protocol sit `common/` (shared,
direction-agnostic codec), `client/`, `server/`, and — for EtherNet/IP —
`dialects/`. Client and server share as much as possible **through `common/`**;
neither couples to the legacy `protocols/ab/` tree (which Phase B deletes). The
only server code allowed above the protocol level is the raw TCP accept-loop /
wake-pipe plumbing — platform mechanics, not protocol logic — which factors into
`utils/`, not `protocols/`.

| New location | From libdevsim | Notes |
|--------------|----------------|-------|
| `protocols/enip/common/eip.c cpf.c cip.c` | `protocols/{eip,cpf,cip}.c` | direction-agnostic EIP/CPF/CIP codec — server decode now, client encode later |
| `protocols/enip/common/identity.c` | `protocols/identity.c` | CIP Identity object encode/decode — shared by the server reply and the client `@identity` read |
| `protocols/enip/server/server.c/.h` | `protocols/server.c` | EIP endpoint: per-connection thread + endpoint registry (accept-loop mechanics come from `utils/`) |
| `protocols/enip/server/server_tag.c` | `lib/device_sim.c` | the `(ab-eip, role=server)` constructor + backing store; public `device_sim_*` API **deleted** |
| `protocols/enip/server/device.h` | `lib/device.h` | `tag_def_t`, endpoint/store structs (internal) |
| `protocols/enip/server/discovery.c` | `protocols/discovery.c` | UDP List Identity/Services **responder** (server-only for now — see note) |
| `protocols/enip/dialects/{rockwell,omron,pccc}/*.c` | `dialects/*` | AB/OMRON/PCCC listing + PCCC data path — unchanged, plugged in via the CIP object registry |
| `utils/` accept-loop helper (name TBD) | accept loop in `protocols/server.c` | cross-protocol TCP accept + wake-pipe; platform mechanics, not protocol logic (option A) |
| `../utils/arena.c bytes.c` | already shared | add to libplctag sources under `LIBPLCTAG_FEATURE_SERVER` |

**Discovery note:** the List Identity/Services **responder** is server-only today
and lives in `enip/server/discovery.c`. Client-side discovery (active scanning,
à la `scan_eip_network`) is planned for the future — not built now. When it lands,
the List Identity item **encode/decode** moves into `enip/common/` (next to
`identity.c`) and the client scanner lives in `enip/client/`, so responder and
scanner share one codec. Place the encode/decode with that future split in mind.

The Modbus server lives at `protocols/modbus/server/modbus_server.c`, with framing
shared from `protocols/modbus/common/` (the client moves to
`protocols/modbus/client/`). Modbus has no `dialects/` and no CIP.

CMake: append each protocol's `common/` + `server/` sources to `libplctag_SRCS`
only when both `LIBPLCTAG_FEATURE_SERVER` and that protocol's feature are set. The
cross-protocol accept-loop helper compiles under `LIBPLCTAG_FEATURE_SERVER` alone.
The moved `enip/common/` codec is gated under `SERVER` for now (its only consumer
is the server decode); it graduates to plain `LIBPLCTAG_FEATURE_EIP` in Phase B
when the client adopts it.

---

## 4. Dispatch — `(protocol, role)` keying

Extend the `tag_type_map[]` in `lib/init.c` with a `role` column; default role
is `"client"`. `find_tag_create_func` reads `role=` (default `client`) and
matches `(protocol, role)`:

```c
struct { const char *protocol; const char *role; ... tag_create_function ctor; } tag_type_map[] = {
    { "ab-eip",     "client", ..., ab_tag_create },
#if defined(LIBPLCTAG_FEATURE_SERVER) && defined(LIBPLCTAG_FEATURE_EIP)
    { "ab-eip",     "server", ..., eip_server_tag_create },
#endif
    { "modbus-tcp", "client", ..., mb_tag_create },
#if defined(LIBPLCTAG_FEATURE_SERVER) && defined(LIBPLCTAG_FEATURE_MODBUS)
    { "modbus-tcp", "server", ..., modbus_server_tag_create },
#endif
};
```

AB and OMRON share `protocol=ab-eip&role=server`; they diverge through the
existing CIP object registry (dialect listing), so there is one EIP server
constructor, not two. New PLC family later = one row + one constructor.

---

## 5. The server tag constructor and vtable

`eip_server_tag_create(attr, cb, userdata)`:

1. Parse identity/endpoint/tag attributes (§7).
2. `endpoint = find_or_create_endpoint(bind_addr, port)` — first server tag at an
   endpoint starts the listener + discovery threads; later ones just register.
3. Allocate the `plc_tag` (standard base struct), size `data` to
   `elem_size * elem_count`, register the tag def with the endpoint's store so
   the buffer is shared, set the server vtable, store `cb`/`userdata`.
4. Endpoint refcount++. Last `plc_tag_destroy` at the endpoint stops the threads.

Server vtable (`tag.h struct tag_vtable_t`) — almost all no-ops because the data
is always live:

| vtable func | server behaviour |
|-------------|------------------|
| `read`   | copy backing store → `tag->data` under the store lock; raise read-complete locally |
| `write`  | copy `tag->data` → backing store under the store lock |
| `status` | `PLCTAG_STATUS_OK` while the endpoint thread is healthy, else the thread's error |
| `abort` / `tickler` / `wake_plc` | no-op (`PLCTAG_STATUS_OK`) |

No network state machine on the local handle — the listener thread owns the
socket; the local handle only touches shared memory.

---

## 6. Hooking `create_ex` callbacks — events from the other direction

The `create_ex` callback (`void(*)(int32_t,int event,int status,void*)`) is
delivered by the existing `tag_raise_event(tag, event, status)`. Server tags
reuse it unchanged; only the **meaning** is mirrored, which is documentation,
not code.

### 6.1 Read / write events (required)

When the listener thread services a remote request for a tag, it calls
`tag_raise_event` on that server tag:

| Remote action on the server tag | Event raised | Server-side meaning |
|---------------------------------|--------------|---------------------|
| master **reads** our value | `PLCTAG_EVENT_READ_STARTED` → `READ_COMPLETED` | a peer pulled this value *from* us |
| master **writes** our value | `PLCTAG_EVENT_WRITE_STARTED` → `WRITE_COMPLETED` | a peer pushed a value *into* us |

So a libplctag *client* fires READ when *it* reads a PLC; a *server* tag fires
READ when a *peer* reads *it*. Same enum, inverted source. The app's callback
(e.g. recompute a derived value on incoming write, or log on outgoing read) fires
exactly as for a client tag. The store lock is released before the event is
raised so the callback may call `plc_tag_get_*` without deadlock.

Implementation note: `tag_raise_event` sets per-tag event flags that the normal
tag tickler flushes to the registered callback under `api_mutex` — the listener
thread sets the flag, the existing delivery path runs the callback. No new
delivery mechanism.

### 6.2 Connection / session events (optional, endpoint-scoped — default OFF)

The client connection events (`PLCTAG_EVENT_CONN_STATUS_UP/DOWN`, 100/101)
describe *one* client→PLC link. A server endpoint has 0..N concurrent client
sessions and a tag may be served by many of them, so per-tag, per-session
connect/disconnect **does not map** and we do not invent events for it.

If session visibility is wanted, expose it **endpoint-scoped** on the first
server tag of an endpoint only, reusing the existing enum:

- `PLCTAG_EVENT_CONN_STATUS_UP` — session count went 0 → ≥1 (a client connected)
- `PLCTAG_EVENT_CONN_STATUS_DOWN` — session count went ≥1 → 0 (last client left)

YAGNI: ship **without** this. Most simulator users care about read/write hits,
not socket churn. Add the endpoint-scoped form only when a user actually asks.

---

## 7. New tag attributes

All optional except `role`. Everything else reuses existing client attributes.

| Attribute | Default | Use |
|-----------|---------|-----|
| `role` | `client` | `server` selects the server constructor |
| `gateway` / `port` | `0.0.0.0` / proto default | bind address + listen port for the endpoint |
| `name`, `elem_type`, `elem_size`, `elem_count`, `dimensions` | — | tag definition (same grammar as client) |
| `make`, `model`, `vendor_id`, `device_type`, `product_code`, `serial`, `revision` | per-protocol defaults | identity object seed |
| `sim_delay_ms` | 0 | delay every response (latency simulation); endpoint-scoped (see below) |
| `sim_max_packet` | proto max | force fragmentation / partial-transfer paths; endpoint-scoped, applies to both directions |
| `sim_fault` | none (0) | force a CIP general-status error (e.g. `0x05`) for reads/writes on this specific tag |
| `transport` | `tcp` | not implemented — see note below |

Modbus maps `name`/`elem_type` to register file + address range; CIP maps to a
symbol the dialect listing advertises.

Implementation notes (ENIP, `eip_server_tag_create`):
- `sim_delay_ms`/`sim_max_packet` set fields on the shared `device_t`
  (`device_sim_set_response_delay`/`device_sim_set_max_packet`), so they take
  effect only for the tag that starts a *new* endpoint — a tag joining an
  already-running endpoint cannot change its delay/packet-size, matching
  `sim_delay_ms`/`sim_max_packet`'s status as endpoint-, not tag-, properties.
- `sim_fault` is genuinely per-tag (`tag_def_t.fault_status`): `common/cip.c`'s
  `handle_read`/`handle_write` return that CIP status immediately, before
  touching data or the read/write callbacks, for every remote request against
  that tag only — sibling tags on the same endpoint are unaffected.
- `transport=loopback` is deliberately not implemented: it would require an
  in-process transport parallel to the real TCP/UDP listener, and per-project
  guidance the socket path must be solid first. The existing loopback-*style*
  coverage (`server_tag_basic`, §9) already exercises everything through a
  real `127.0.0.1` socket instead.
- Modbus's `modbus_server_tag_create` equivalent does not exist yet — these
  attributes are ENIP-only for now.

---

## 8. Debug module additions

Appended to the `PLCTAG_MODULE_*` enum in `lib/libplctag.h` (continues at 27);
the CMake generator (`ParseLibplctagHeader.cmake`) picks it up automatically
into `debug_module_t` and the name table — no other file needed editing.

```
PLCTAG_MODULE_SERVER = 28,   /* server tag listener/endpoint registry (server/endpoint.c, server/eip_server_tag.c) */
```

As-built this is a single module, not the four originally sketched here. The
ENIP protocol codec (`common/cip.c`, `common/eip.c`, `common/cpf.c`,
`dialects/*`) — server- and client-side alike — was already consolidated onto
`PLCTAG_MODULE_ENIP` in an earlier pass (see git history); adding
`SERVER_EIP`/`SERVER_DIALECT` would mean re-splitting that already-deliberate
consolidation for no operational benefit. `PLCTAG_MODULE_SERVER` covers only
the layer that's genuinely new and separate: the endpoint find-or-create
registry and the `role=server` tag constructor/vtable, neither of which
existed before this session's P3/P4 work. `PLCTAG_MODULE_SERVER_MODBUS` is
deferred until a Modbus server-tag constructor exists.

Discovery logs under `PLCTAG_MODULE_SERVER` (not its own module — see §2).

---

## 9. Testing

As-built (`src/poc/server_tag_basic/server_tag_basic.c`, wired into the CMake
build via `src/poc/CMakeLists.txt`, run manually — not yet ported into the
CTest-driven part of the harness): a single localhost-socket process exercises
`role=server` tags entirely through `plc_tag_*`, no `device_sim_*`:

- **Test 1/2** — basic round trip, and a second server tag joining an
  already-running endpoint (endpoint find-or-create + shared-endpoint tag
  isolation).
- **Test 3** — callback direction: a client write to a server tag fires
  `WRITE_COMPLETED` on *that tag's own* callback (§6.1).
- **Test 4** — destroy one server tag while a sibling on the same endpoint
  keeps serving requests.
- **Test 5** — `sim_fault` forces a CIP error status for one tag without
  affecting its endpoint siblings.

`run_device_sim_tests.sh` (11 scenarios against the `device_sim` CLI) is still
run alongside this, not retired — but as of P5, the `device_sim` CLI and the
`devsim_with_plctag` POC no longer call `device_sim_*` themselves: both build
`plc_tag_create("...&role=server&...")` attribute strings instead
(`src/tools/device_sim/args.c`'s `args_build_tag_attr_str`), so this test still
exercises `role=server` end to end, just through a different front door than
`server_tag_basic`. `device_sim_create`/`_start`/`_stop`/`_destroy` etc. remain
in the tree as the internal endpoint runtime that `server/endpoint.c` and
`server/eip_server_tag.c` build on (see `device_sim.h`'s header comment) — they
were never a second, parallel public API to retire, so "delete `device_sim_*`"
from the original P5 wording didn't apply once P3/P4 turned out to reuse it
rather than duplicate it. `device_sim.h` was already not part of the installed
public API (only `lib/libplctag.h` is installed), confirming there was nothing
external depending on it to migrate off of.

Not yet done:
- Not ported into the `ctest`-run part of the suite (currently a manual binary,
  like the `ab_server`/`modbus_server*` POCs it sits next to).
- No feature-gate build test (`SERVER`/`EIP`/`MODBUS` each off) confirming
  `role=server` returns `PLCTAG_ERR_NOT_FOUND` when the feature is disabled.
- `sim_max_packet`/`sim_delay_ms` (endpoint-scoped) have no automated
  assertion of client-visible fragmentation/latency yet — only `sim_fault` has
  a test (Test 5 above).
- No Modbus server-tag test (no Modbus server-tag constructor yet).
- `transport=loopback` intentionally not implemented — see §7's note.

New test files live with the existing tests (e.g. `src/tests/server_tag_*.c`),
not under a `libdevsim` tree.

---

## 10. Migration & doc updates

1. Move libdevsim sources per §3; `src/libdevsim/CMakeLists.txt` and the
   standalone `devsim` build target are already gone (folded into
   `protocols/enip/CMakeLists.txt` under `LIBPLCTAG_FEATURE_SERVER`, P2).
   `device_sim_*` itself was not deleted — see §9's note — but the
   `device_sim` CLI and `devsim_with_plctag` POC no longer call it directly
   (P5).
2. Keep `DESIGN_deprecated.md` for protocol-wire reference (identity bytes, CIP
   dialect formats) — it is still the authoritative wire-format record.
3. Add the three `LIBPLCTAG_FEATURE_*` options to the top-level CMake with a
   one-table feature matrix in the README.
4. Document `role=server` and the `sim_*` attributes in the public API docs and
   add one server-tag example to the examples directory.

---

## 11. Implementation steps (ordered)

1. Add `LIBPLCTAG_FEATURE_{EIP,MODBUS,SERVER}` CMake options + generated
   `plctag_features.h`; gate the **current** AB/OMRON/Modbus client sources under
   EIP/MODBUS (no behaviour change — proves the gates).
2. Move libdevsim sources into `protocols/enip/{common,server,dialects}/` and
   `protocols/modbus/server/` (§3); extract the direction-agnostic CIP/EIP codec
   into `protocols/enip/common/`. Compile the server sources under `SERVER` (+ the
   protocol flag) and the cross-protocol accept-loop helper under `SERVER`.
3. Replace `device_sim_*` object API with `eip_server_tag_create` /
   `modbus_server_tag_create` + the server vtable (§5). As-built,
   `device_sim.[ch]` was not deleted: `eip_server_tag_create`/`endpoint.c`
   call into it as the shared internal endpoint runtime rather than
   duplicating it, and it was never part of the installed public headers, so
   there was no external API to retire it away from. What *was* migrated off
   `device_sim_*` (P5): the `device_sim` CLI and `devsim_with_plctag` POC,
   which now build `role=server` attribute strings instead of calling
   `device_sim_create`/`_add_tag`/etc. directly.
4. Add the `role` column + server rows to `tag_type_map` (§4).
5. Wire `tag_raise_event` read/write calls into the listener's request servicing
   (§6.1).
6. Map the new attributes (§7) in the constructors.
7. Add debug modules (§8).
8. Port tests to loopback + localhost (§9); add callback and gate-build tests.
9. Doc updates (§10).

Steps 1–4 are the structural merge; 5–6 deliver the callback behaviour; 7–9
finish attributes, observability, and tests.

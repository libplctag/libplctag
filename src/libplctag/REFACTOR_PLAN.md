<!--
  Copyright (C) 2026 by Kyle Hayes - kyle.hayes@gmail.com
  This software is available under the MPL 2.0 or LGPL 2 (or later) license.
-->

# Refactor plan — server tags, then the EtherNet/IP client rewrite

This plan covers two large pieces of work and, more importantly, the **order**
they must happen in:

1. **Server tags** — fold the device simulator into the `plc_tag_*` API
   (design: `SERVER_TAGS.md`).
2. **EtherNet/IP client rewrite** — move AB and OMRON off the heap-allocated
   request-object model onto the Modbus-style tag-queued model, share their
   common code, and move all encode/decode to arena/bytes.

**Decision: server tags first.** The rest of this document is the why and the
sequencing.

---

## 1. Why server tags go first

### 1.1 It builds the test harness the client rewrite cannot proceed without

The EtherNet/IP rewrite is a 3–4 month, high-risk change whose dangers are
*silent* — throughput regressions from broken request packing, and rare
use-after-free / race bugs from removing the request object. The only guard
rails that catch those are:

- golden wire-byte capture/replay,
- a loopback throughput bench,
- abort / timeout / fault-injection fuzzing.

Every one of those needs something to test *against*. That something is the
in-process simulator. Build the client rewrite first and you are testing it
against real hardware and hand-rolled fixtures; build server tags first and the
rewrite lands on a deterministic, fault-injecting, no-hardware test bed that is
already in the tree. **Server tags are the safety net for the client work.**

### 1.2 It establishes arena/bytes + a proven CIP codec at low risk

The server code already exists, already runs on arena/bytes, already passes its
11/11 suite. Integrating it is mostly a *move + `#if`-gate* job, not a rewrite.
That lands the encode/decode foundation — and the `LIBPLCTAG_FEATURE_*` gating
scaffolding — on code that already works, so the client rewrite **adopts** a
battle-tested codec instead of inventing one mid-flight.

### 1.3 It delivers user value immediately and independently

Server tags ship a feature users can use now. The client rewrite ships (at best)
identical behaviour with cleaner internals. Front-load the usable, lower-risk
work.

---

## 2. The caveat that makes the ordering pay off

Build the server's CIP encode/decode from day one as a **shared,
direction-agnostic codec** under `protocols/cip/`, consumed by both server-side
decode and (later) client-side encode — **not** buried inside the server
runtime.

- If the codec is shareable, the client rewrite's arena/bytes migration becomes
  "adopt the existing codec" instead of "write a new one."
- If the server codec becomes server-only, the foundation gets built twice and
  the ordering buys nothing.

So: **server tags first, but architect their codec as the shared layer the
client will later stand on.** This turns two sequential-and-duplicative projects
into foundation-then-reuse.

Consequence for the earlier estimate: the arena/bytes "warm-up" that was step 1
of the client-refactor sequence moves *into* the server-tags work — same task,
done once, on the lower-risk side.

---

## 3. Sequencing

### Phase A — Server tags (delivers the test bed)

Per `SERVER_TAGS.md §11`:

1. Add `LIBPLCTAG_FEATURE_{EIP,MODBUS,SERVER}` CMake options + generated
   `plctag_features.h`; gate the existing client sources (no behaviour change —
   proves the gates).
2. Move libdevsim sources into `protocols/server/`; **extract the CIP codec into
   a shared `protocols/cip/` from the start** (the §2 caveat).
3. Replace the `device_sim_*` object API with `eip_server_tag_create` /
   `modbus_server_tag_create` + the server vtable; delete `device_sim.[ch]`.
4. Add the `role` column + server rows to `tag_type_map`.
5. Wire `tag_raise_event` read/write into the listener's request servicing.
6. Map server attributes; add debug modules; port tests to loopback + localhost.

Exit criteria: server tags work, the loopback simulator + fault injection +
golden-byte harness exist, the shared CIP codec is in `protocols/cip/`.

### Phase B — Client rewrite (stands on Phase A's foundation)

Per the EIP-refactor estimate:

1. **Adopt the shared codec** in the AB client encode/decode (this is the
   former arena/bytes "warm-up", now mostly reuse). Land golden wire-byte tests.
2. **AB tag-queued engine** — delete the `ab_request_t` lifecycle, push state
   onto the tag `op` + a session state machine with a batching/packing step.
   Extract the shared transport layer as you go. Golden tests green at each step.
3. **OMRON as a thin dialect** on the shared engine. If OMRON does not collapse
   to a small dialect, the layering in B2 is wrong — fix it before proceeding.
4. **PLC5 / SLC / PCCC** re-validation on the shared transport (tag logic
   largely unchanged; budget time to confirm, not rewrite).

Phases B1 and B4 are low-risk bookends; the risk and the payoff are in B2–B3.

---

## 4. Guard rails (apply throughout Phase B, built in Phase A)

- **Golden wire-byte tests** — record exact request/response bytes the *current*
  AB/OMRON code emits (read, write, packed multi-tag, fragmented large tag,
  connected + unconnected, per PLC family); assert byte-identical from the new
  encoder. Highest-value single guard rail.
- **Loopback + localhost server tests** — fast, deterministic, no hardware.
- **Throughput regression bench** — tags/sec for 1/10/100/1000-tag batches,
  recorded as a number that must not drop. The only thing that catches a packing
  regression.
- **Abort / timeout / disconnect fuzz under ASan/TSan** — where request-object
  removal hides use-after-free.
- **Concurrency stress under TSan** — many threads create/read/abort/destroy on
  one session.
- **Fragmentation + packing boundary tests** — at, one under, and far over the
  buffer / MSP size, both directions.
- **Real-hardware end-to-end** per milestone — the ControlLogix/OMRON test PLCs;
  the simulator cannot catch firmware-quirk regressions.

---

## 5. Rough effort

| Phase | Est. | Risk |
|-------|------|------|
| A — server tags (incl. shared codec extraction, test bed) | ~4–6 wk | Low–Med |
| B1 — adopt shared codec in client + golden tests | ~1–2 wk | Low |
| B2 — AB tag-queued engine + batching | ~4–6 wk | **High** |
| B3 — OMRON as dialect | ~2–3 wk | Med |
| B4 — PLC5/SLC/PCCC re-validation | ~1–2 wk | Low |

Phase A pays for itself twice: a shipped feature *and* the infrastructure that
de-risks Phase B. Total ≈ 3–4 months, front-loaded with the low-risk,
value-delivering half.

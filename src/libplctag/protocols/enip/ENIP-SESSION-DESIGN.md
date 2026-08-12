<!--
  Copyright (C) 2026 by Kyle Hayes - kyle.hayes@gmail.com
  This software is available under the MPL 2.0 or LGPL 2 (or later) license.
-->

# enip_session — connection, scheduling, and lifetime design

`enip_session` is the heart of the generic EtherNet/IP client. It owns the TCP
socket, the EIP session, the per-connection IO thread, and the scheduler that
decides which tag talks to the wire next. Everything else in the `enip/` tree
(`enip_cpf`, `enip_cip`, `enip_type`, `enip_tag`) is stateless request/response
code that the session drives.

This document specifies the model. Two non-negotiable properties shape it:

1. **No per-request allocation.** A `plc_tag_read()` / `plc_tag_write()` /
   `plc_tag_abort()` storm must not grow memory. The AB/Omron backends allocate a
   request object per call and enqueue it; an app that starts and aborts faster
   than the IO thread drains the queue grows the queue (and the aborted-request
   purge list) without bound. That is a DoS surface. We never allocate per
   request — the **tag is the unit of work**, and a tag can be scheduled at most
   once at a time.

2. **At most one request in flight per connection.** EtherNet/IP over a single
   session is naturally serial. Fixing one-in-flight deletes the entire
   transaction-id matching apparatus that makes the Modbus backend complex
   (`find_response_tag`, `pending_transaction_id`, `pending_request_count`,
   request slots, deferred-response tag): the in-flight tag is just a pointer.

The Modbus backend is the reference for *lifetime* and *thread structure*; we
keep those and discard its multi-in-flight machinery.

---

## 1. Module position

```
enip_tag.c        tag struct + vtable; create/abort/read/write/status; attribs
   |  (enqueues tags, never allocates requests)
   v
enip_session.c    THIS FILE: socket, EIP session, IO thread, scheduler, lifetime
   |  (builds requests into a per-cycle arena, parses responses from rx_buf)
   v
enip_cpf.c        CPF item framing
enip_cip.c        CIP path encode, ForwardOpen/Close, Read/Write/ReadFrag
enip_type.c       CIP type code -> element size + byte order
```

The session calls *down* into cpf/cip/type to format and parse; it calls *up*
into the generic lib (`tag_raise_event`, `plc_tag_generic_*`) to report
completion. It never calls into `enip_tag.c`.

---

## 2. Core principle: the tag is the work item

A tag carries everything a request needs, all allocated once at create time:

- the encoded CIP IOI path (in the tag's tail bytes — see `tag.h`)
- the data buffer (`TAG_BASE_STRUCT.data`)
- an `op` field saying what it wants (READ / WRITE / IDLE)
- scheduler links and an `op_time`

So `plc_tag_read()` does not build or allocate anything. It sets `op`, sets
`op_time`, and links the tag into the connection's scheduler. The IO thread later
builds the actual bytes into a reusable arena. There is no request object to
allocate, enqueue, match, or purge.

### Tag scheduler fields (added to `enip_tag_t`)

```c
/* scheduler membership — protected by conn->sched_mutex */
enip_tag_p   sched_prev;     /* doubly linked so abort/remove is O(1)        */
enip_tag_p   sched_next;
int64_t      op_time;        /* absolute ms: when this tag next wants the wire */
uint8_t      op;             /* ENIP_OP_IDLE | ENIP_OP_READ | ENIP_OP_WRITE   */
uint8_t      scheduled : 1;  /* currently linked in the sorted list?          */
uint32_t     frag_offset;    /* Read Tag Fragmented continuation cursor        */
/* abort_requested is already an atomic_bool in TAG_BASE_STRUCT                */
```

`scheduled` is the DoS guard: a tag is linked **at most once**. Re-issuing a read
on an already-scheduled tag just updates `op`/`op_time` in place; it never adds a
second list node. Live work items are therefore hard-capped at the tag count, no
matter how fast the app calls.

---

## 3. The scheduler: one op_time-sorted intrusive list

All schedulable tags for a connection live in a single doubly-linked list kept
**sorted ascending by `op_time`**. `op_time` means "the moment this tag next
wants the wire":

- explicit `plc_tag_read()/write()` → `op_time = now` (sorts ahead of all future
  auto-syncs, behind anything already due — FIFO among equal times)
- auto-sync tag → `op_time =` its next scheduled fire time; it stays resident in
  the list and is re-inserted with a fresh `op_time` each time it completes

The head is always the next event. The IO thread never scans:

```c
now = time_ms();
if(conn->in_flight == NULL && conn->sched_head && conn->sched_head->op_time <= now) {
    pop sched_head -> in_flight;          /* dispatch exactly one */
} else {
    wait_deadline = conn->sched_head ? conn->sched_head->op_time : FOREVER;
    /* mask ALWAYS includes SOCK_EVENT_WAKE_UP | SOCK_EVENT_TIMEOUT so a freshly
       linked tag (which calls socket_wake after linking) breaks the wait at once,
       and so terminate is honored within one wait cycle. CAN_READ/CAN_WRITE are
       added per state (§5). The head op_time IS the wait deadline. */
    socket_wait_event(conn->sock, mask, wait_deadline - now);
}
```

This is the only blocking point in the whole IO thread. Every socket operation in
every state funnels through this single `socket_wait_event`; nothing else ever
blocks (see §5). That is what makes shutdown and wake responsive: the destructor
sets `terminate`, calls `socket_wake`, and the thread returns from this wait within
one cycle regardless of what transaction was in progress.

This is the Modbus scheduling rule ("walk from the front until op_time is in the
future, then wait on that op_time") reduced to its minimum: single-in-flight pops
at most one per cycle, and the head is by construction the earliest, so there is
nothing to walk.

| operation | when | cost |
|---|---|---|
| sorted insert | enable auto-sync, explicit read/write, re-arm after completion | O(n) walk |
| pop head | dispatch | O(1) |
| remove | abort, disable, destroy | **O(1)** (doubly linked) |

Insert is the only O(n), and it happens per *completed operation*, never per
cycle. If a large, fast-cycling tag set ever makes that cost matter, swap the list
for a fixed-size binary min-heap of tag pointers embedded in the connection (no
malloc, O(log n) insert/pop, plus a `heap_idx` field for O(log n) abort-remove).
Ship the list first.

Two correctness details:
- **Re-arm against the schedule, not the clock:** `op_time += interval`, never
  `now + interval`, or service latency accumulates as drift.
- **Insert-after-equal** on `op_time` ties so equal-interval tags keep FIFO order
  and none can starve another.

---

## 4. Connection structure

```c
struct enip_connection_t {
    enip_connection_t *next;          /* registry singly-linked list           */

    /* identity (registry key) */
    char *gateway;
    char *path;

    /* transport — sock is created once and lives for the whole connection.
       socket_close() tears down only the TCP data connection (on error);
       socket_destroy() is called ONLY in the connection destructor. The wake
       channel inside sock survives socket_close(), so socket_wake() keeps
       working across reconnects. */
    sock_p   sock;
    thread_p thread;
    uint32_t session_handle;          /* from RegisterSession                   */
    uint32_t cip_conn_id;             /* from ForwardOpen (connected path)      */
    uint16_t conn_seq;                /* monotonic; sanity-check only           */

    /* scheduler */
    mutex_p     sched_mutex;          /* guards sched_* list + in_flight        */
    enip_tag_p  sched_head, sched_tail;
    enip_tag_p  in_flight;            /* the single tag being serviced; rc-pinned */

    /* one arena, reset twice per transaction (build, then receive). The tx and
       rx buffers are NOT fixed arrays — they are temporary arena allocations
       sized to the negotiated ForwardOpen capacity (§5.2). The arena BACKING
       buffer is allocated once at connection create, large enough for the
       largest packet we will ever request (the connection size we ask for in
       ForwardOpen) plus EIP/CPF framing — that upper bound is known up front
       because we choose the requested connection size. */
    Arena    arena;

    /* lifecycle */
    enum { CONN_CONNECT, CONN_REGISTER, CONN_OPEN, CONN_READY,
           CONN_SENDING, CONN_WAITING, CONN_CLOSING } state;
    atomic_bool terminate;
};
```

The `arena` (which backs the temporary tx/rx buffers — §5.2) and the scheduler
nodes (which live inside the tags) are the *only* memory the request path touches.
The arena is `arena_reset()` **twice per transaction** — once before building the
request and again before receiving the reply (§5.2) — so each transaction reuses
the same backing bytes.

---

## 5. IO thread state machine

One thread per connection, created at registry-create time, looping until
`terminate`:

```
CONN_CONNECT  : if(!sock) socket_create(&sock);   /* create once; reuse across reconnects */
                socket_connect_tcp_start/_check -> CONN_REGISTER
CONN_REGISTER : build RegisterSession into tx_buf; resume_state = CONN_REGISTER;
                -> CONN_SENDING.  (reply handler stores session_handle -> CONN_OPEN)
CONN_OPEN     : (connected path) build ForwardOpen into tx_buf;
                resume_state = CONN_OPEN; -> CONN_SENDING.
                (reply handler stores cip_conn_id + sizing -> CONN_READY)
                (unconnected path: set sizing directly -> CONN_READY, no I/O)
CONN_READY    : pick next due tag (section 3). If none, socket_wait_event on the
                head's op_time. If one: rc_inc it, store in in_flight,
                arena_reset; tx_buf = arena_alloc(rx_cap); build request into tx_buf,
                tx_off = 0; resume_state = CONN_READY -> CONN_SENDING
CONN_SENDING  : ONE non-blocking socket_write of the remaining request bytes;
                advance tx_off by the count written. If would-block (0/timeout),
                stay in CONN_SENDING and return to the top loop's wait (mask adds
                SOCK_EVENT_CAN_WRITE). When tx_off == tx_len:
                  arena_reset;                       /* frees tx_buf            */
                  rx_buf = arena_alloc(rx_cap); rx_len = 0;   /* §5.2           */
                  -> CONN_WAITING.
CONN_WAITING  : ONE non-blocking socket_read appending into rx_buf; advance rx_len.
                Framing (§5.1): until rx_len >= 24 we are still reading the EIP
                header; once we have it, total = 24 + eip_hdr.len; keep returning to
                the wait (mask adds SOCK_EVENT_CAN_READ) until rx_len == total.
                Only then parse. If would-block before complete, stay in
                CONN_WAITING and return to the wait.
                On a complete packet, dispatch on resume_state (§5.0):
                - CONN_REGISTER/CONN_OPEN: parse the setup reply, advance the phase.
                - CONN_READY (a tag transaction): parse zero-copy out of rx_buf,
                  copy what we keep into in_flight->data BEFORE any arena_reset:
                    if(in_flight->abort_requested)  -> finish_aborted()
                    else if(partial/continuation)  copy returned elements into
                          in_flight->data; set frag_offset/read_off; arena_reset;
                          tx_buf = arena_alloc(rx_cap); build next request,
                          tx_off = 0 -> CONN_SENDING
                    else  copy decoded bytes into in_flight->data; set status;
                          raise READ/WRITE_COMPLETED
                    complete_tag() (§13.4): rc_dec(in_flight); in_flight = NULL;
                    re-arm if auto-sync; -> CONN_READY
CONN_CLOSING  : ForwardClose (if open) + UnregisterSession; socket_close(sock); exit
                /* socket_destroy(&sock) happens later, in the destructor */
```

**No state ever blocks in a socket call.** SENDING and WAITING each do at most one
non-blocking socket op per loop iteration and otherwise return to the single
`socket_wait_event` in §3. A multi-round-trip operation (ReadFrag continuation,
OPEN_BULK windows) is expressed as `CONN_WAITING → build next → CONN_SENDING`
transitions, **not** as an internal blocking loop — so `terminate`/`socket_wake`
are still honored between every round trip.

The response routes to `in_flight` by pointer; there is no id table. `conn_seq`
is incremented and checked only as a sanity gate.

### 5.0 Transport states are shared; `resume_state` says who to return to

`CONN_SENDING` and `CONN_WAITING` are **generic transport sub-states** — they only
move `tx_buf` out and a framed EIP packet in. They have no idea whether the bytes
belong to RegisterSession, ForwardOpen, or a tag read. The initiating state
(`CONN_REGISTER`, `CONN_OPEN`, or `CONN_READY`) records where to return by setting
a single field before entering `CONN_SENDING`:

```c
uint8_t resume_state;   /* CONN_REGISTER | CONN_OPEN | CONN_READY: the phase whose
                           reply handler runs when CONN_WAITING has a full packet */
```

On a complete packet, `CONN_WAITING` switches on `resume_state` and calls that
phase's reply handler. Only `resume_state == CONN_READY` involves a tag
(`in_flight`); the session-setup phases have no tag and just advance the
handshake. This is what lets the session-setup packets and every tag transaction
share one non-blocking send/receive path without duplicating it, and it resolves
the apparent split between "phase states" and "transport states."

### 5.1 Packet framing

`rx_buf` accumulates across iterations via `rx_len` (Modbus does exactly this:
modbus.c:2231–2277). Read the 24-byte EIP header first; its `len` field gives the
payload size, so the full packet is `24 + len` bytes. Keep appending until
`rx_len == 24 + len`, then parse once. A would-block read (`PLCTAG_ERR_TIMEOUT`
from a non-blocking socket) is normal and just means "stay in CONN_WAITING and
wait again." `rx_len` is reset to 0 when entering CONN_WAITING (§5.2). A
`24 + len` larger than `rx_cap` is a framing error → `reset_connection` (it cannot
happen for our own right-sized requests, since `rx_cap` is the negotiated packet
size plus framing).

### 5.2 Arena lifecycle: tx and rx are both arena allocations

The arena is reset **twice per transaction**, and the tx/rx buffers are temporary
allocations out of it — there are no fixed packet buffers in the connection struct:

1. **Before build** (entering CONN_SENDING from CONN_READY, or building a
   continuation): `arena_reset(&c->arena)`, then
   `c->tx_buf = arena_alloc(&c->arena, c->rx_cap)` and encode the request into it.
2. **After the send completes** (tx_off == tx_len, entering CONN_WAITING):
   `arena_reset(&c->arena)` — this frees `tx_buf`, which is safe because the bytes
   are already on the wire — then `c->rx_buf = arena_alloc(&c->arena, c->rx_cap)`
   and `rx_len = 0`.

So `tx_buf` and `rx_buf` are never live simultaneously, and the arena only ever
holds one packet-sized buffer (plus build scratch) at a time. `rx_cap` is the
whole-packet buffer size, `max_cip_packet_size + ENIP_FRAMING_OVERHEAD` (§14.3): it
is derived once when `max_cip_packet_size` is set — from the ForwardOpen reply at
`CONN_OPEN`, or from `ENIP_UNCONNECTED_CIP_MAX` on the unconnected path — **before**
the first `CONN_READY`. The session-setup packets (RegisterSession, ForwardOpen
itself) run before negotiation, so they use a bootstrap `rx_cap` = a small fixed
`ENIP_BOOTSTRAP_PACKET` constant large enough for those replies; `rx_cap` is
raised to the negotiated value once ForwardOpen succeeds. The arena backing buffer
is allocated once (§4) for the largest `rx_cap` we will ever use — the connection
size we *request* in ForwardOpen, plus framing — so neither `arena_alloc` ever
fails or grows.

The parse step reads zero-copy slices out of `rx_buf` (§14.4/§14.5 return `Bytes`
into the input), so all bytes we intend to keep must be copied into
`in_flight->data` *before* the next `arena_reset`. The state machine already does
this: it copies into `tag->data` in CONN_WAITING, and only the subsequent build
(CONN_READY or a continuation) resets the arena.

---

## 6. Reference counting and lifetime (from Modbus)

This is the subtle part and the place the Modbus model earns its keep. The danger:
the API thread can `plc_tag_destroy()` a tag at any instant, including while the IO
thread is mid-transaction on it. The destructor frees `tag->data` — exactly the
buffer the IO thread is about to write into.

Modbus solves this with `rc`:

- The scheduler list holds **raw** tag pointers — list membership is a *weak*
  reference and does not keep a tag alive.
- A tag's destructor removes the tag from the scheduler list (under
  `sched_mutex`) before freeing anything. So a tag that is merely *scheduled but
  not in flight* unlinks itself cleanly on destroy; the IO thread never sees it.
- The instant the IO thread selects a tag to service, it takes a **strong**
  reference with `rc_inc()` and stores the result as `in_flight`. It releases that
  reference with `rc_dec()` only when the transaction finishes (success, error, or
  abort). While that reference is held, the final `rc_dec()` from
  `plc_tag_destroy()` cannot run the destructor, so `tag->data` and the tag struct
  stay valid for the whole transaction.

```c
/* IO thread, picking work — under sched_mutex */
enip_tag_p t = conn->sched_head;
if(t && t->op_time <= now) {
    unlink t from list; t->scheduled = 0;
    conn->in_flight = rc_inc(t);     /* strong pin for the transaction */
}
/* ... run the transaction ... */
conn->in_flight = rc_dec(conn->in_flight);   /* release; destructor may now run */
```

The pick/complete pseudocode here is **illustrative** — it shows the `rc` pinning
intent but omits the full locking. The exact, normative locking for the pick,
`in_flight`, `abort_requested`, and re-arm is §13; where this section and §13
appear to differ, §13 governs.

The tag holds an `rc_inc` reference on the connection (`tag->conn`), so the
connection outlives all its tags; the connection's IO thread is torn down only
after the last tag releases it (Modbus's deferred-cleanup ordering — the handler
thread references the connection through its task parameter and does not
`rc_dec` it from inside the loop). Replicate that ordering exactly.

The same rule extends to the optional Multi-Service batch: every tag pulled into
the fixed `batch[]` array is `rc_inc`'d for the duration and `rc_dec`'d when its
sub-reply is distributed.

---

## 7. Locking discipline

Two mutexes are involved per transaction: the tag's `api_mutex` (held by the API
thread across `plc_tag_*` calls) and the connection's `sched_mutex`.

- **API side** (read/write/abort): holds `api_mutex` (the library takes it), then
  briefly takes `sched_mutex` to (re)link the tag. Lock order: `api_mutex →
  sched_mutex`.
- **IO side**: takes `sched_mutex` to pick + `rc_inc` the tag, releases it, then
  uses `mutex_try_lock(tag->api_mutex)` before touching tag buffers/state. If the
  try-lock fails (API thread is mid-call), the IO thread backs off and retries
  next cycle rather than blocking — this is exactly how the global tickler
  (lib.c:555) and the Modbus handler avoid the deadlock that strict ordering
  would otherwise create.

`sched_mutex` is never held across socket I/O.

---

## 8. Abort and DoS resistance

```c
/* plc_tag_abort -> enip vtable abort, api_mutex held */
critical_block(conn->sched_mutex) {
    if(tag->scheduled && tag != conn->in_flight) {
        unlink tag; tag->scheduled = 0;            /* O(1), no alloc */
        tag->status = PLCTAG_ERR_ABORT;
    } else if(tag == conn->in_flight) {
        atomic_set_bool(&tag->abort_requested, true);  /* IO thread drops the reply */
    }
}
```

An abort is either an O(1) unlink or a single atomic flag the IO thread already
checks in `CONN_WAITING`. There is no request object to free and no purge list to
walk. Because a tag occupies the list at most once (`scheduled` bit), an app
spinning `read(); abort(); read(); abort()` merely toggles bits on the same fixed
tag struct; the working set is bounded by tag count, independent of call rate.

---

## 9. The tag tickler is not needed

Setting `tag->skip_tickler = 1` at create time removes the tag from the global
tickler's active set entirely (lib.c:541 — the tag is never even added to
`active_tags`). As a direct consequence, for an ENIP tag the global tickler does
**not**:

- call `plc_tag_generic_tickler(tag)` (lib.c:558),
- call `tag->vtable->tickler(tag)` (lib.c:561), or
- raise the read/write-complete events (lib.c:563–590).

So the per-tag `tickler` vtable slot has no purpose in this model. Modbus proves
this directly: its `mb_tickler` is a literal no-op flagged "not used", and the
vtable guard `if(tag->vtable && tag->vtable->tickler)` (lib.c:561) tolerates a
NULL slot. **We set `vtable.tickler = NULL` and do not write one.**

Everything the tickler would have done is owned by the IO thread instead, because
the IO thread is the only place that knows when a transaction actually finished:

- it sets `tag->status`, `tag->read_complete` / `tag->write_complete`;
- it raises events with `tag_raise_event(tag, PLCTAG_EVENT_READ_COMPLETED, status)`
  (and the WRITE / ABORTED variants);
- it flushes them with `plc_tag_generic_handle_event_callbacks(tag)`;
- it wakes any blocking `plc_tag_read()` caller with
  `plc_tag_generic_wake_tag(tag)` (which signals `tag_cond_wait`).

This mirrors Modbus's `tickle_tag()` tail (modbus.c:2199–2215) exactly. All of it
runs while the IO thread holds the tag's `api_mutex` (via the try-lock from
section 7) and its strong `rc` pin (section 6), so it is safe against concurrent
API calls and against destruction.

The vtable we register is therefore:

```c
static struct tag_vtable_t enip_tag_vtable = {
    .abort           = enip_tag_abort,
    .read            = enip_tag_read,
    .write           = enip_tag_write,
    .status          = enip_tag_status,
    .tickler         = NULL,                 /* IO thread owns the state machine */
    .wake_plc        = enip_tag_wake,        /* socket_wake the IO thread          */
    .tag_data_written = enip_tag_data_written, /* auto_sync_write re-schedule       */
    .get_int_attrib  = enip_tag_get_int_attrib,
    .set_int_attrib  = enip_tag_set_int_attrib,
    .get_byte_array_attrib = enip_tag_get_byte_array_attrib,
};
```

`enip_tag_read/write` set `op`/`op_time`, (re)link under `sched_mutex`, then
`socket_wake(conn->sock)` and return `PLCTAG_STATUS_PENDING`.

---

## 10. Disconnect / reconnect recovery and socket lifetime

The socket object outlives any single connection attempt. There are two distinct
operations, and conflating them is a bug:

- **`socket_close(conn->sock)`** — tears down only the TCP data connection. The
  `sock_p` object stays allocated, and **the wake channel inside it stays live**,
  so `socket_wake()` keeps working while we are disconnected and reconnecting.
  This is what the IO thread does on a network error.
- **`socket_destroy(&conn->sock)`** — frees the object *and* the wake channel.
  This is called in **exactly one place: the connection destructor** (§6), when
  the whole connection is going away. The IO thread never destroys the socket.

This mirrors Modbus exactly: `reset_plc` calls `socket_close` (modbus.c:962),
`connect` calls `socket_create` only when `!sock` (modbus.c:1477–1479), and
`socket_destroy` appears only in the PLC destructor (modbus.c:862).

On socket error the IO thread:

1. takes `sched_mutex`, walks the list once setting each tag's status to a
   retryable error (or leaves auto-sync tags armed to refire), clears `in_flight`
   (with its `rc_dec`), all under `sched_mutex`;
2. calls `socket_close(conn->sock)` — **not** `socket_destroy` — and returns to
   `CONN_CONNECT`, which re-uses the same `sock` object (`if(!sock)` guard).

Because `op` has no REQUEST/RESPONSE split (the connection state, not the tag,
tracks in-flight), there is no per-tag rollback — the Modbus RESPONSE→REQUEST
reset (modbus.c:937–952) is unnecessary.

### Why `socket_wake` from the API thread is then race-free

`enip_tag_wake` / `enip_tag_read` wake the IO thread with `socket_wake(conn->sock)`
from an API thread while the IO thread may be mid-reconnect. This is safe **without
a socket mutex** for the same reason it is in Modbus (`wake_plc_thread`,
modbus.c:1407–1414):

- the waker holds an `rc` reference on the connection (the tag owns `tag->conn`),
  so the connection — and therefore `conn->sock` — cannot be destroyed underneath
  it; `socket_destroy` runs only in the destructor, which cannot start while any
  ref is held;
- the waker checks `conn->terminate` first and skips the wake if the connection is
  tearing down (avoids racing the destructor's own final wake);
- `socket_close` never frees the object, so even a wake that lands between a close
  and the next `socket_connect` targets a valid object and a valid wake channel —
  it just gets drained by the next `socket_wait_event`;
- concurrent `socket_wake()` calls from multiple threads are themselves safe.

So the only ordering requirement is: **`socket_destroy` happens once, in the
destructor, after the IO thread has joined** (the destructor sets `terminate`,
wakes the socket, `thread_join`s, then `socket_destroy`s — §6 / modbus.c:837–863).

---

## 11. Tag open / priming sequence (probe-then-bulk)

A freshly created tag is **not usable until it has been read once, hidden**, at
create time. AB/Omron already do a hidden open read for two reasons; we keep both
and add a third:

1. **Type reuse for writes.** A CIP write must prepend the element's type header
   (2 bytes for an atomic type, 4 bytes for a structure: `0xA0 0x02` + 2-byte
   structure handle). We learn it on open and store it, so writes reproduce it
   exactly.
2. **Never write zeros over live data.** Open fully populates `tag->data` with the
   device's current values, so an immediate `plc_tag_write()` after create sends
   real data back, not an all-zero buffer.
3. **Right-size every read so no response ever overflows.** This is the new part,
   and it is what lets us unify Rockwell and OMRON without manufacturer-specific
   fragmentation.

### 11.1 Why right-sizing, and one rule for all CIP PLCs

When a read response would exceed the negotiated packet, OMRON returns an error
with no data and Rockwell returns a partial-transfer status with the data that
fit. We do not want to depend on or special-case either behavior. **For this
phase we proactively size every request so the response always fits the
negotiated CIP capacity, and we treat any non-zero CIP status as a failure** —
the same fit calculation and the same handling for every CIP PLC. No
vendor-specific code paths.

This "no vendor-specific code paths" claim is scoped to the right-sized common
path described here. The deferred byte-fragmentation phase (§16a, §16.4) *does*
use the vendor-specific encodings — Rockwell ReadFrag with its `0x06`
partial-transfer status, OMRON Simple Data Segment — but only for single elements
too large to fit a packet, and only behind the `enip_dialect_t` seam (§16a.4), so
the common core still never branches on manufacturer.

To size correctly we must know one element's size, which we don't know up front
for a UDT. Hence: **probe one element, then bulk-read the rest in correctly sized
windows.**

### 11.2 The sequence

This is one logical transaction made of several request/reply round trips, driven
entirely inside the in-flight servicing of the tag (same pattern as Read Tag
Fragmented continuation in section 5 — the tag stays `in_flight`, rc-pinned, and
the IO thread loops `CONN_WAITING → build next → CONN_SENDING` without returning
to the scheduler).

```
OPEN_PROBE:
    build Read Tag on element 0 only:  path = base (+ [0] for arrays), count = 1
    on reply:
        if cip_status != 0: fail open with decoded CIP error
        header_len = (first 2 type bytes == 0x02A0) ? 4 : 2     /* enip_type */
        store type_header bytes + header_len                    /* for writes */
        elem_size  = reply_data_len - header_len
        elem_count = attr "elem_count"
        tag->data  = alloc(elem_size * elem_count)              /* one-time     */
        copy element 0 -> tag->data[0]
        one_elem_resp_size = CIP_READ_REPLY_OVERHEAD + header_len + elem_size
        window = clamp( (cap - CIP_READ_REPLY_OVERHEAD - header_len) / elem_size,
                        1, elem_count )                          /* cap = conn->max_cip_packet_size */
        if elem_size + header_len + CIP_READ_REPLY_OVERHEAD > cap:
            status = PLCTAG_ERR_TOO_LARGE  /* one element won't fit; needs */
            finish                          /* fragmentation — later phase  */
        read_off = 1                       /* element 0 already populated by the probe */
        op = (elem_count == 1) ? <ready> : OPEN_BULK

OPEN_BULK:   /* resume at element 1; loop until read_off == elem_count */
    n = min(window, elem_count - read_off)
    build Read Tag on element read_off:  path = base[read_off], count = n
    on reply:
        if cip_status != 0: fail open with decoded CIP error
        returned = reply_data_len_after_header / elem_size
        copy returned elements -> tag->data[read_off ..]
        read_off += returned                /* never re-reads what we already have */
    when read_off == elem_count:
        meta.ready = 1
        op = IDLE
        raise PLCTAG_EVENT_CREATED(OK); release the connection to other tags
```

`OPEN_BULK` resumes at element 1 — element 0 is already in `tag->data` from the
probe, and round trips and bandwidth are both scarce, so we never re-read it. A
scalar or any `elem_count == 1` tag skips `OPEN_BULK` and is ready after the probe.

**Abort during open.** The probe→bulk sequence is several round trips, so
`abort_requested` is checked at each `CONN_WAITING` boundary (§5), exactly as for a
ReadFrag continuation — not only at the end. If an abort lands mid-open, the IO
thread discards the in-flight reply, raises `ABORTED`, leaves `ready = 0`, and
releases the connection; the half-filled `tag->data` is never exposed because the
ready gate (§11.6) still blocks the API. The create call therefore fails rather
than handing back a partially primed tag.

### 11.3 Window is computed, not discovered

After the probe we know `elem_size` exactly and we know the negotiated capacity
exactly, so the window is *computed once and is correct*. There is no adaptive
"shrink and retry": a correctly accounted response cannot overflow. The only
inputs to the window are fixed, knowable quantities, and the same formula is used
for every CIP PLC:

```text
window = clamp( (cap - CIP_READ_REPLY_OVERHEAD - header_len) / elem_size, 1, elem_count )
```

If `cap`, `CIP_READ_REPLY_OVERHEAD`, or `header_len` is wrong, the cure is to fix
that constant — not to probe the boundary at runtime. Reply handling for this
phase is therefore minimal and vendor-neutral:

| reply | action |
| --- | --- |
| CIP status `0x00` | `returned = data/elem_size`; copy and advance |
| any non-zero CIP status | **fail the open** with the decoded CIP error; do not retry, do not special-case any vendor |
| one element exceeds `cap` (pre-send check) | `PLCTAG_ERR_TOO_LARGE` → byte fragmentation, a later phase (§16a.6, §16.4) |

Because the window is sized to fit, a non-zero status means something genuinely
wrong (bad path, privilege, a too-small overhead constant, an unsupported type),
not a routine boundary condition — so failing fast is correct. A later phase adds
byte-granular fragmentation (§16a.6); until then, anything that cannot be made to
fit one packet fails with `PLCTAG_ERR_TOO_LARGE`.

One caveat worth stating: this exactness assumes a **fixed** `elem_size` across
the array, which holds for atomics and fixed UDTs. Element types whose size varies
per element (e.g. OMRON packed variable-length strings) break the fixed-window
assumption and are deferred to the same later phase as fragmentation.

### 11.4 Tag meta fields this adds

```c
/* in enip_tag_t, owned by the open sequence, read by getters and writes */
uint8_t  type_header[4];   /* 2 or 4 bytes, replayed verbatim on write */
uint8_t  type_header_len;  /* 2 (atomic) or 4 (structure)             */
uint32_t elem_size;        /* one element, bytes                       */
uint32_t elem_count;       /* from create attribs                      */
uint32_t window_elems;     /* max elements per read response          */
uint32_t read_off;         /* bulk-phase cursor, elements             */
uint8_t  ready : 1;        /* set when open completes; gates the API   */
```

`op` (section 2) gains `ENIP_OP_OPEN_PROBE` and `ENIP_OP_OPEN_BULK`. The create
path enqueues the tag with `op = ENIP_OP_OPEN_PROBE`, `op_time = now`; the
`PLCTAG_EVENT_CREATED` event fires only when `ready` is set.

### 11.5 Writes reuse the primed type

`enip_tag_write` builds a Write Tag (0x4D) request as
`service + path + type_header[0..len] + count + data`, replaying the stored
`type_header`. Because open already filled `tag->data`, a write issued before the
app has touched the buffer round-trips the device's own current values rather than
zeros. Large writes are windowed by the same `window_elems` math (write capacity
is symmetric); per-element-too-large writes are the same later-phase
fragmentation case.

### 11.6 Ready gate

Until `meta.ready`, value getters/setters and explicit `plc_tag_read/write`
return `PLCTAG_STATUS_PENDING` (the open is already in flight, so there is nothing
extra to schedule — the create-time open will complete and raise `CREATED`). This
matches the existing libplctag contract that a tag is not usable until its
creation read completes.

---

## 12. Allocation budget

| memory | lifetime | when allocated |
|---|---|---|
| connection struct | per PLC | registry create |
| `arena` backing buffer | per PLC | registry create, sized for the largest ForwardOpen connection size we request; reset twice per transaction (§5.2) |
| tx buffer | — | **none separate** — arena alloc, freed by `arena_reset` after send |
| rx buffer | — | **none separate** — arena alloc sized to negotiated `rx_cap` (§5.2) |
| tag struct + path tail | per tag | tag create |
| tag `data` buffer | per tag | sized once at open (section 11) from probed `elem_size * elem_count`; reallocated only if the type later changes |
| scheduler list nodes | — | **none** — links live inside the tag struct |
| request objects | — | **none** |

Per request cycle: **zero malloc.** That is the whole point.

---

## 13. Concurrency invariants

The strategy in §6–§8 is correct but under-specifies *which lock covers which
field* and *the lifecycle of the two shared hand-off variables*
(`in_flight`, `abort_requested`). This section is normative: implement to it
exactly. Getting it "mostly right" reintroduces the races called out inline below.

### 13.1 Threads

- **API threads** — any number, one at a time per tag. The library holds the
  tag's `api_mutex` across every `plc_tag_*` call, so `vtable->read/write/abort`
  and the value getters/setters all run under `api_mutex`.
- **IO thread** — exactly one per connection. Owns the socket and the state
  machine. Touches tag state only while holding that tag's `api_mutex` (via
  `mutex_try_lock`).
- **Registry / teardown** — `find_or_create` and the connection destructor run on
  API threads but mutate connection-global state.

### 13.2 Locks

| lock | guards |
| --- | --- |
| `registry_mutex` (file-static) | the connection list (`conn->next`, list head) and the find-or-create transaction |
| `conn->sched_mutex` | `sched_head/tail`, every tag's `sched_prev/next` + `scheduled` bit, `op`, `op_time`, and `conn->in_flight` |
| `tag->api_mutex` | `tag->data`, `tag->status`, `read_complete`/`write_complete`, all `meta.*` (incl. `ready`, `elem_size`, `type_header`, `read_off`), and event flags |
| `tag->abort_requested` | atomic; no lock, but see §13.5 for its lifecycle |
| `conn->terminate` | atomic |

Lock order is fixed and total: **`api_mutex` → `sched_mutex`**. `registry_mutex`
is never held with either of the others. `sched_mutex` is never held across socket
I/O or across `socket_wait_event`.

### 13.3 Field ownership rule for `op` / `op_time` (fixes finding #7)

`op` and `op_time` are written by **both** the API thread (in `read`/`write`) and
the IO thread (open-sequence transitions, completion re-arm). To make those writes
race-free there is one rule: **every write to `op`/`op_time` is made while holding
`api_mutex` *and* `sched_mutex`.** The IO thread already holds `api_mutex` (try-lock)
while servicing a tag; it must additionally take `sched_mutex` for the moment it
mutates `op`/`op_time`. The scheduler picker reads them under `sched_mutex`.

### 13.4 `in_flight` is sched_mutex-only (fixes finding #4)

Every read and write of `conn->in_flight` — the pick in `CONN_READY`, the
`tag == conn->in_flight` test in abort (§8), and the clear at completion (§5) —
is under `sched_mutex`. The clear and its `rc_dec` are one critical section:

```c
critical_block(conn->sched_mutex) {
    enip_tag_p done = conn->in_flight;
    conn->in_flight = NULL;
    /* re-arm under the same lock — see 13.6 */
    rc_dec(done);          /* release the strong pin last */
}
```

### 13.5 `abort_requested` lifecycle (fixes finding #1)

The flag must be **cleared on pick and reconciled on completion under
`sched_mutex`**, or a completion that races a late abort leaves the flag set and
silently kills the tag's *next* operation.

- **Pick** (`CONN_READY`, under `sched_mutex`): `atomic_set(&tag->abort_requested, false)`
  at the instant the tag becomes `in_flight`. A new operation starts clean.
- **Completion** (under `sched_mutex`, before clearing `in_flight`): re-read the
  flag. If it is now `true`, the abort won the race — discard the reply, raise
  `ABORTED`, set `op = IDLE`. If `false`, complete normally. Because both the
  abort write and this re-read happen under `sched_mutex`, there is no window in
  which the flag is set after the tag is no longer `in_flight`.
- **Abort** (§8): only ever sets the flag while `tag == conn->in_flight` *and*
  holding `sched_mutex`; for a merely-scheduled tag it unlinks instead and never
  touches the flag.

An optional early-out: the IO thread may also check `abort_requested` just before
`CONN_SENDING` to skip a doomed round trip. That is an optimization, not a
correctness requirement.

### 13.6 Completion re-arm must respect `scheduled` (fixes finding #2)

While a tag is `in_flight` it has `scheduled == 0`, so a concurrent
`plc_tag_read()` may legally re-link it (sets `scheduled = 1`, `op = READ`,
`op_time = now`). Therefore the completion re-arm **must not blindly link**:

```c
/* under sched_mutex */
if(auto_sync && !tag->scheduled) {
    tag->op_time += interval;     /* schedule-relative, not now-relative */
    insert_sorted(conn, tag);     /* sets scheduled = 1 */
}
/* if already scheduled, a fresh request arrived mid-flight; leave it linked */
```

Linking an already-linked node corrupts `sched_prev/next`; the `scheduled` check
is the guard. The same guard applies anywhere a tag is inserted.

### 13.7 Registry: lock + `rc_inc`-returns-NULL (fixes finding #3)

`find_or_create` runs under `registry_mutex` for the whole find-then-maybe-create
transaction, so two API threads cannot create duplicate connections for the same
`gateway+path`:

```c
critical_block(registry_mutex) {
    for(c = head; c; c = c->next) {
        if(matches(c, key)) {
            enip_connection_t *got = rc_inc(c);   /* may be NULL: c is dying */
            if(got) { return got; }                /* else fall through and create */
            break;
        }
    }
    c = create_connection(key);   /* alloc, start IO thread, link into list */
    return c;
}
```

`rc_inc()` returns NULL when the object's count has already reached zero (it is
between final `rc_dec` and destructor completion). Treat that as "not found" and
create a fresh connection — never hand back a dying one. This is the
`!rc_inc(*walker)` pattern from Modbus (`find_or_create_plc`).

A connection removes itself from the registry list **in its destructor, under
`registry_mutex`**, before freeing anything — symmetric with how a tag removes
itself from `sched_mutex`'s list (§6).

### 13.8 `socket_wake` must be latching (fixes finding #5)

`read`/`write` link the tag under `sched_mutex`, release it, then
`socket_wake(conn->sock)`. The IO thread reads the next deadline under
`sched_mutex`, releases it, then calls `socket_wait_event`. A wake that lands in
the gap between release and wait **must not be lost**. This requires
`socket_wake` to be *latching* — it leaves the wake channel readable so the next
`socket_wait_event` returns immediately even if the wake preceded it. This is
**confirmed**, not an assumption: the Modbus IO thread depends on exactly this
behavior (it waits with `SOCK_EVENT_WAKE_UP` in the mask and drains the wake at
modbus.c:1256/1346), so the platform `socket_wake`/`socket_wait_event` pair is
already latching. The wait mask must therefore always include
`SOCK_EVENT_WAKE_UP` (§3). The deadline is always recomputed under `sched_mutex`
on the next loop, so a spurious early wake only costs one extra iteration.

### 13.9 Invariant summary

1. `op`, `op_time`, `sched_*`, `scheduled`, `in_flight` — written only under
   `api_mutex` + `sched_mutex` (picker/abort read `in_flight`/links under
   `sched_mutex` alone).
2. `tag->data`, `status`, `meta.*`, completion flags — only under `api_mutex`.
3. `abort_requested` — cleared on pick, reconciled on completion, both under
   `sched_mutex`; set by abort only while in-flight under `sched_mutex`.
4. A tag is linked into the scheduler **at most once** (`scheduled` bit); never
   link an already-linked tag.
5. `in_flight` and its `rc_dec` change together in one `sched_mutex` section.
6. Registry mutations and `rc_inc`-returns-NULL handling under `registry_mutex`.
7. `socket_destroy` once, in the destructor, after `thread_join` (§10);
   `socket_close` everywhere else; `socket_wake` is latching and rc-safe (§10).
8. Lock order `api_mutex → sched_mutex`; `registry_mutex` independent; no lock
   held across socket I/O.

---

## 14. Files and function-by-function spec

This section is the build sheet. Each file lists its purpose, the structs/enums it
owns, and every function with a one- or two-line contract. Signatures use the
project's conventions (`int32_t` status returns, `bool` flags, explicitly sized
ints, `/* */` comments, `static` for internal, `extern` only in headers). All
encode/decode goes through `utils/bytes.h` over a per-cycle `Arena`; all logging
through `utils/debug.h` with `DEBUG_MODULE_ENIP` (add it to `debug.h` if absent).

### 14.0 File list

The tree is split four ways: `common/` is direction-agnostic codec shared by the
client and the device simulator, `client/` is this document's subject, `server/`
is the simulator runtime (see `SERVER_TAGS.md`), and `dialects/` holds the
per-manufacturer plug-ins reached only through `enip_dialect_t` (§16a.4). Each
dialect keeps its client half and its simulator half in one directory.

```text
enip/
  CMakeLists.txt                 ENIP_PROTOCOL_SOURCES (client) + ENIP_SERVER_SOURCES
  common/                        direction-free: no I/O, no thread, no connection state
    eip.{h,c}                    EIP encapsulation header encode/decode, command codes
    cpf.{h,c}                    Common Packet Format wrap/unwrap
    cip.{h,c}                    CIP request dispatch + object registry (server-facing)
    cip_path.{h,c}               IOI / symbolic + logical path encode and decode
    identity.{h,c}               CIP Identity payload encode/decode, product-name catalog
    plc_type.h                   enip_plc_type_t — the one family enum both directions use
    plc_classify.{h,c}           Identity reply → enip_plc_type_t
  client/
    enip.{h,c}                   module entry points; thin forwarders (§14.1)
    enip_session.{h,c}           socket, IO thread, scheduler, EIP encap, lifetime (largest)
    enip_connection_internal.h   full struct enip_connection_t; enip_session.c + dialects only
    enip_dialect.h               the per-connection dialect vtable (§16a.4)
    enip_eip.{h,c}               client-side EIP framing helpers
    enip_cip.{h,c}               CIP path encode, ForwardOpen/Close, Read/Write/ReadFrag
    enip_type.{h,c}              CIP type code → element size + tag_byte_order_t fill
    enip_tag.{h,c}               tag struct, op enum, vtable, create, accessors, open
    enip_pccc_addr.{h,c}         PCCC logical address ("N7:0", "F8:0") parse → pccc_addr_t
    enip_discover.{h,c}          enip-udp List Identity scan; its own tag struct and vtable
  server/                        device simulator runtime — see SERVER_TAGS.md
  dialects/
    rockwell/                    logix_client.c (build/apply) + ab_listing.c (simulator)
    omron/                       omron_client.c (listing hooks) + omron_listing.c (simulator)
    pccc/                        pccc_client.c (Execute-PCCC) + pccc.{h,c} (simulator)
```

`enip_session.c` is the only client file that includes the socket platform
header and spawns a thread. `enip_connection_internal.h` is the one deliberate
crack in the opaque-connection rule: `enip_session.c` and the three
`dialects/*/…_client.c` files see the full struct, everything else sees the
opaque type from `enip_session.h`.

**Note on the subsection names below.** §14.1–§14.9 were written before the
directory split and still use the flat pre-split filenames. They remain correct
as *function* specs; for the file each one now lives in, read `enip_cpf.h/.c` as
`common/cpf.{h,c}` and prefix every other `enip_*` name with `client/`.

### 14.1 `enip.h` / `enip.c` — protocol entry and registry

Entry points the library already calls (keep these exact names — `init.c`
references them):

```c
/* enip.h */
extern int    enip_init(void);     /* one-time module init; create registry_mutex */
extern void   enip_teardown(void); /* join+destroy all connections; destroy mutex */

extern plc_tag_p enip_tag_create(attr attribs,
        void (*cb)(int32_t, int, int, void *), void *userdata, plc_tag_p src_tag);
```

`enip.c` internals:

| function | contract |
| --- | --- |
| `static enip_connection_t *registry_find_or_create(attr a)` | §13.7: under `registry_mutex`, match `gateway+path`; `rc_inc` a live match (NULL ⇒ create fresh); else `enip_session_create()` and link. Returns a connection with a ref held by the caller. |
| `static bool conn_key_matches(enip_connection_t *c, const char *gw, const char *path)` | string-compare identity. |
| `enip_tag_create(...)` | parse `protocol`; delegate the whole tag build to `enip_tag_create_impl` in `enip_tag.c` (this function is a thin forwarder kept because `init.c` names it). |

Registry globals: `static enip_connection_t *s_conns;` and
`static mutex_p s_registry_mutex;`. `enip_teardown` walks `s_conns`, sets each
`terminate`, wakes, joins, and destroys (mirrors Modbus teardown ordering).

### 14.2 `enip_session.h` — the connection API used by the tag layer

```c
typedef struct enip_connection_t enip_connection_t;   /* opaque to enip_tag.c */

/* lifetime */
extern enip_connection_t *enip_session_create(attr attribs); /* alloc + start IO thread */
/* (no explicit destroy: rc_dec drops the last ref; the destructor joins the thread) */

/* scheduling — called from the tag vtable, with tag->api_mutex held by caller */
extern int32_t enip_session_schedule(enip_connection_t *c, enip_tag_p tag,
                                     uint8_t op, int64_t op_time);
   /* §13.3/13.6: under sched_mutex set op/op_time; if !scheduled insert sorted;
      then socket_wake(c->sock). Returns PLCTAG_STATUS_PENDING. */

extern int32_t enip_session_unschedule(enip_connection_t *c, enip_tag_p tag);
   /* abort path (§8/§13.5): under sched_mutex, if scheduled && != in_flight unlink
      and return OK; else if == in_flight set abort_requested; else OK (idle). */

extern void enip_session_tag_detach(enip_connection_t *c, enip_tag_p tag);
   /* tag destructor: under sched_mutex remove from list if linked. Never called
      while the tag is in_flight (the rc pin prevents the destructor from running). */

extern size_t enip_session_max_cip(enip_connection_t *c); /* negotiated cap, for window math */
```

### 14.3 `enip_session.c` — connection, IO thread, scheduler

Owns `struct enip_connection_t` (full definition in §4 plus the fields below), the
`CONN_*` state enum, and the packet-sizing constants `ENIP_BOOTSTRAP_PACKET`
(session-setup replies, §5.2), `ENIP_UNCONNECTED_CIP_MAX` (unconnected CIP cap),
and `ENIP_FRAMING_OVERHEAD` (EIP header + CPF/CIP item bytes added to a CIP
payload to get the whole-packet `rx_cap`). The connected-path `max_cip_packet_size`
is set from the ForwardOpen reply, not a constant; `rx_cap` is derived from it.

Connection-private fields beyond §4:

```c
char    *gateway, *path;          /* registry key (also in §4)             */
int      tcp_port;                /* default 44818                          */
uint8_t  is_connected_path : 1;   /* true ⇒ ForwardOpen + SendUnitData      */
size_t   max_cip_packet_size;     /* negotiated CIP payload cap; see note    */
int64_t  reconnect_at_ms;         /* backoff deadline after an error        */

/* current-request transmit/receive cursors (§5) — IO thread only, no lock  */
uint8_t  resume_state;            /* §5.0: phase to return to after a reply  */
uint8_t *tx_buf;                  /* arena alloc; the built request          */
size_t   tx_len, tx_off;          /* total request bytes / bytes sent so far */
uint8_t *rx_buf;                  /* arena alloc; full-packet buffer          */
size_t   rx_cap, rx_len;          /* buffer size / bytes received so far     */
```

`tx_buf` and `rx_buf` both point **into `arena`**. They are never alive at the
same time: `tx_buf` is freed (by `arena_reset`) the moment the request is fully
sent, immediately before `rx_buf` is allocated. See §5.2 for the reset/alloc
lifecycle.

**Two distinct sizes — keep them separate:**

- `max_cip_packet_size` is the **CIP payload capacity** the device granted; it is
  the `cap` the §11 window math divides by `elem_size`.
- `rx_cap` is the **whole-packet buffer size** used for `arena_alloc` of `tx_buf`
  and `rx_buf` and as the §5.1 framing bound. It is
  `max_cip_packet_size + ENIP_FRAMING_OVERHEAD` (the 24-byte EIP header + CPF/CIP
  item framing). `rx_cap` is (re)computed whenever `max_cip_packet_size` is set.

`max_cip_packet_size` is set authoritatively by **ForwardOpen** on the connected
path (the device returns the connection size it granted). On the unconnected path
there is no negotiation, so it is fixed to a defined constant
`ENIP_UNCONNECTED_CIP_MAX` (the EIP-level payload cap; conservative default for
Logix is 504, configurable via attribs). Both paths must populate
`max_cip_packet_size` (and therefore `rx_cap`) before the first `CONN_READY`;
until then the handshake uses the bootstrap size (§5.2).

Thread + main loop:

| function | contract |
| --- | --- |
| `static void io_thread_func(void *arg)` | the per-connection thread; `arg` is the connection (referenced via the task param, **not** rc'd inside — §6). Loops on `state` until `terminate`. |
| `static void conn_destructor(void *arg)` | set `terminate`, `socket_wake`, `thread_join`, `thread_destroy`, then `socket_destroy`, free `gateway/path`, `arena_free`, unlink from registry under `registry_mutex` (§13.7). |
| `static int32_t step_connect(enip_connection_t *c)` | `if(!c->sock) socket_create`; `socket_connect_tcp_start/_check`; on done → `CONN_REGISTER`. During `reconnect_at_ms` backoff it does **not** sleep — it returns and lets the §3 `socket_wait_event(WAKE_UP\|TIMEOUT, …)` wait out the backoff so `terminate` stays responsive (modbus.c:1346). |
| `static int32_t step_register(enip_connection_t *c)` | build RegisterSession into `tx_buf`; `resume_state = CONN_REGISTER`; → `CONN_SENDING` (§5.0). The reply is processed by `on_register_reply`. Does no socket I/O. |
| `static int32_t step_open(enip_connection_t *c)` | connected path: build ForwardOpen into `tx_buf`, `resume_state = CONN_OPEN`, → `CONN_SENDING`; `on_open_reply` stores `cip_conn_id` + `max_cip_packet_size`/`rx_cap` and → `CONN_READY`. Unconnected path: set `max_cip_packet_size = ENIP_UNCONNECTED_CIP_MAX` (and `rx_cap`) directly, → `CONN_READY`, no I/O. |
| `static void on_register_reply(enip_connection_t *c)` / `on_open_reply(...)` | session-setup reply handlers called from `step_waiting` when `resume_state` is `CONN_REGISTER`/`CONN_OPEN`; parse the EIP/CIP reply and advance the phase. No tag involved. |
| `static enip_tag_p pick_due_tag(enip_connection_t *c, int64_t now, int64_t *wait_ms)` | §3/§13.4: under `sched_mutex`, if head due and `in_flight==NULL`, unlink head, clear its `abort_requested`, `rc_inc` into `in_flight`; else compute `*wait_ms` from head `op_time`. |
| `static int32_t build_request(enip_connection_t *c)` | tag transactions only: `try_lock(api_mutex)` on `in_flight` (back off and stay in CONN_READY if busy); `arena_reset`; encode the request for the tag's `op` (read/write/open-probe/open-bulk/read-frag) into `tx_buf`; set `tx_len`, `tx_off = 0`, `resume_state = CONN_READY`; → CONN_SENDING. **Does no socket I/O.** (RegisterSession/ForwardOpen are built by `step_register`/`step_open`, not here.) |
| `static int32_t step_sending(enip_connection_t *c)` | ONE non-blocking `socket_write` of `tx_buf+tx_off .. tx_len`; advance `tx_off`. Would-block ⇒ stay in CONN_SENDING. Complete ⇒ `arena_reset`, alloc `rx_buf`, `rx_len = 0`, → CONN_WAITING. Never blocks. |
| `static int32_t step_waiting(enip_connection_t *c)` | ONE non-blocking `socket_read` appending into `rx_buf`; advance `rx_len`. Apply §5.1 framing; if the packet is incomplete, stay in CONN_WAITING. On a full packet, dispatch on `resume_state` (§5.0): setup replies → `on_register_reply`/`on_open_reply`; a tag transaction → parse, handle abort/continuation/completion, and on a continuation rebuild the next request (→ CONN_SENDING). Never blocks. |
| `static void complete_tag(enip_connection_t *c, enip_tag_p t, int8_t status)` | §13.5/13.6: under `sched_mutex` reconcile `abort_requested`, set completion flags, re-arm auto-sync if `!scheduled`, clear `in_flight`, `rc_dec`. Raise `READ/WRITE_COMPLETED` or `ABORTED`, `plc_tag_generic_handle_event_callbacks`, `plc_tag_generic_wake_tag` (under `api_mutex`, before releasing it). |
| `static void reset_connection(enip_connection_t *c)` | §10: under `sched_mutex` fail/disarm tags and clear `in_flight`; `socket_close` (never destroy); → `CONN_CONNECT`. |
| `static void sched_insert_sorted(enip_connection_t *c, enip_tag_p t)` | insert by ascending `op_time`, after equal (§3); set `scheduled=1`. Caller holds `sched_mutex`. |
| `static void sched_unlink(enip_connection_t *c, enip_tag_p t)` | O(1) doubly-linked removal; clear `scheduled`. Caller holds `sched_mutex`. |

EIP encapsulation (also in this file — it is small and stateful with the session):

```c
typedef struct { uint16_t cmd, len; uint32_t session, status;
                 uint64_t context; uint32_t options; } eip_hdr_t;   /* 24 bytes */

static Bytes eip_encode(Arena *a, eip_hdr_t *h, Bytes payload); /* header+payload */
static bool  eip_decode(Bytes in, eip_hdr_t *h, Bytes *payload); /* split header */
static Bytes eip_register_session(Arena *a);                    /* cmd 0x0065     */
static Bytes eip_send_rr_data(Arena *a, uint32_t sess, Bytes cpf);  /* 0x6F unconn */
static Bytes eip_send_unit_data(Arena *a, uint32_t sess, Bytes cpf);/* 0x70 conn   */
```

These port almost directly from `attic/enip_eip.c` / the PoC `eip.c`; re-check
against this spec before lifting.

### 14.4 `enip_cpf.h` / `enip_cpf.c` — Common Packet Format

```c
/* item type codes */
#define CPF_NULL_ADDR   ((uint16_t)0x0000)
#define CPF_CONN_ADDR   ((uint16_t)0x00A1)
#define CPF_CONN_DATA   ((uint16_t)0x00B1)
#define CPF_UCONN_DATA  ((uint16_t)0x00B2)

extern Bytes enip_cpf_wrap_unconnected(Arena *a, Bytes cip);
   /* iface(4)=0, timeout(2), count(2)=2, null-addr item, B2 data item(cip)        */
extern Bytes enip_cpf_wrap_connected(Arena *a, uint32_t conn_id, uint16_t seq, Bytes cip);
   /* count=2: A1 addr item(conn_id), B1 data item(seq + cip)                       */
extern bool  enip_cpf_unwrap(Bytes in, bool connected, uint16_t *seq_out, Bytes *cip_out);
   /* parse the item array, return the embedded CIP slice (zero-copy) + seq if conn */
```

### 14.5 `enip_cip.h` / `enip_cip.c` — CIP requests and parsing

```c
/* service codes */
#define CIP_READ        ((uint8_t)0x4C)
#define CIP_WRITE       ((uint8_t)0x4D)
#define CIP_READ_FRAG   ((uint8_t)0x52) /* ROCKWELL ONLY! */
#define CIP_FWD_OPEN    ((uint8_t)0x54)
#define CIP_FWD_OPEN_LG ((uint8_t)0x5B)
#define CIP_FWD_CLOSE   ((uint8_t)0x4E)
#define CIP_UNCONN_SEND ((uint8_t)0x52)   /* Connection Manager, same code as ReadFrag */

/* parsed CIP reply header (service reply byte, status, ext status) */
typedef struct { uint8_t service; uint8_t status; uint16_t ext_status;
                 Bytes data; } cip_reply_t;

extern bool   enip_cip_parse_reply(Bytes in, cip_reply_t *out);
   /* split reply: service(1) reserved(1) status(1) ext_size(1) ext(2*size) data   */

extern Bytes  enip_cip_encode_path(Arena *a, const char *name);
   /* "Foo.Bar[3]" → IOI: 0x91 symbolic segments + 0x28/0x29/0x2A index segments,
      word-aligned. Used once at create; the result is cached in the tag tail.     */

extern Bytes  enip_cip_encode_route(Arena *a, const char *route);
   /* gateway routing path to the CPU, e.g. "1,0" → port segment(s) 0x01 0x00 …,
      word-aligned with a length prefix. Distinct from the symbolic tag IOI above.
      Used by ForwardOpen (connected) and Unconnected_Send (unconnected).          */

extern Bytes  enip_cip_unconnected_send(Arena *a, Bytes route, Bytes embedded);
   /* CM Unconnected_Send (0x52) wrapper: service + CM path (0x20 0x06 0x24 0x01) +
      priority/timeout + embedded-message-len + embedded CIP request + route path.
      This is how an unconnected request reaches a routed Logix CPU (§MVP note).
      The connected path does NOT use this — ForwardOpen carries the route, and
      subsequent reads go out as bare CIP over SendUnitData.                        */

extern Bytes  enip_cip_read(Arena *a, Bytes path, uint16_t count);            /* 0x4C */
extern Bytes  enip_cip_read_frag(Arena *a, Bytes path, uint16_t count, uint32_t offset); /* 0x52 */
extern Bytes  enip_cip_write(Arena *a, Bytes path, Bytes type_header,
                             uint16_t count, Bytes data);                     /* 0x4D */

extern int32_t enip_cip_forward_open(enip_connection_t *c);   /* build+send+parse; sets conn_id/size */
extern int32_t enip_cip_forward_close(enip_connection_t *c);

#define CIP_READ_REPLY_OVERHEAD ((size_t)4)  /* service+reserved+status+ext_size; §11.3 */
```

`enip_cip_encode_path` is the trickiest helper — document the segment rules inline
and unit-test it against known tag names (the `attic` `enip_name.c` has a working
version to compare against).

### 14.6 `enip_type.h` / `enip_type.c` — CIP type → layout

```c
/* returns header length (2 atomic, 4 structure) and fills size/order */
extern bool enip_type_decode(Bytes reply_data, uint8_t *header_len_out,
                             uint32_t *elem_size_hint_out, tag_byte_order_t *order_out);
   /* first 2 bytes == 0x02A0 ⇒ structure (4-byte header, opaque bytes, native order);
      else atomic: map the 2-byte type code → width and set the int/float order
      arrays in tag_byte_order_t (little-endian for CIP). elem_size for an atomic
      comes from the code; for a structure the caller derives it from the probe
      (reply_data_len - header_len) — see §11.2.                                   */
```

Keep the atomic type-code table (`0x00C1` BOOL … `0x00CA` REAL …) here as a
`static const` lookup. This file has no I/O and is trivially unit-testable.

### 14.7 `enip_tag.h` — tag struct and enums

```c
typedef enum {
    ENIP_OP_IDLE = 0,
    ENIP_OP_READ,
    ENIP_OP_WRITE,
    ENIP_OP_OPEN_PROBE,   /* §11.2 count=1 probe   */
    ENIP_OP_OPEN_BULK,    /* §11.2 windowed bulk   */
} enip_op_t;

typedef struct enip_tag_t {
    TAG_BASE_STRUCT;                 /* data, size, status, byte_order, vtable, ... */

    enip_connection_t *conn;         /* holds an rc ref; released in destructor      */

    /* scheduler (≤ once linked) — sched_mutex (§13) */
    struct enip_tag_t *sched_prev, *sched_next;
    int64_t  op_time;
    uint8_t  op;                     /* enip_op_t */
    uint8_t  scheduled : 1;

    /* type/layout learned at open — api_mutex (§11.4) */
    uint8_t  type_header[4];
    uint8_t  type_header_len;        /* 2 or 4 */
    uint32_t elem_size, elem_count, window_elems, read_off;
    uint8_t  ready : 1;

    uint32_t frag_offset;            /* ReadFrag cursor for >1-packet elements      */

    char    *tag_name;               /* into the tail                               */
    /* tail: tag_name (NUL), then encoded CIP path bytes (from enip_cip_encode_path) */
} enip_tag_t;
```

This is the original MVP sketch. The **shipped** struct (see `enip_tag.h` and
§16.1) also carries `write_window_elems`, `batch_next`, and a `path` Bytes view
into the tail. The planned multi-dialect work (§16.4) adds
`int32_t dimensions[3]; uint8_t num_dimensions;` (§16a.5) and
`uint8_t frag_align;` (§16a.6, default 8), and repurposes `frag_offset` from a
ReadFrag-only cursor into the live byte-fragment cursor shared by both dialects.
The dialect pointer itself lives on `enip_connection_t`, not the tag.

### 14.8 `enip_tag.c` — vtable, create, accessors, open

The vtable is exactly §9. Functions:

| function | contract |
| --- | --- |
| `plc_tag_p enip_tag_create_impl(attr a, cb, userdata, src_tag)` | `rc_alloc` the tag + tail; `plc_tag_generic_init_tag`; parse `gateway/path/name/elem_count/elem_size`; encode the path into the tail; `registry_find_or_create` → `tag->conn`; set `vtable`, `skip_tickler=1`; schedule `ENIP_OP_OPEN_PROBE` at `now`; return. |
| `static void enip_tag_destructor(void *arg)` | `enip_session_tag_detach`; `rc_dec(conn)`; `mem_free(data)`. No other owned pointers (tag.h note). |
| `static int32_t enip_tag_read(plc_tag_p t)` | if `!ready` return PENDING; else `enip_session_schedule(conn, t, ENIP_OP_READ, now)`. |
| `static int32_t enip_tag_write(plc_tag_p t)` | as read with `ENIP_OP_WRITE`. |
| `static int32_t enip_tag_abort(plc_tag_p t)` | `enip_session_unschedule(conn, t)` (§13.5). |
| `static int32_t enip_tag_status(plc_tag_p t)` | return `tag->status` (PENDING until first op completes / `ready`). |
| `static int32_t enip_tag_wake(plc_tag_p t)` | `socket_wake(conn->sock)` guarded as §10. |
| `static int32_t enip_tag_data_written(plc_tag_p t)` | auto_sync_write: schedule `ENIP_OP_WRITE` at `now + auto_sync_write_ms` if not sooner. |
| `get/set_int_attrib`, `get_byte_array_attrib` | `elem_size`, `elem_count`, `offset`, etc.; unknown attr → default. |

Open-sequence helpers invoked from `build_request` / `step_waiting` (§14.3) when
`op` is an OPEN_* state live here (they touch `meta`, owned by `api_mutex`) or in
`enip_session.c`; keep them next to the state they mutate and document the choice.
They implement §11.2 verbatim: `OPEN_PROBE` parses type via `enip_type_decode`,
sizes `tag->data`, computes `window_elems`; `OPEN_BULK` builds successive windows
from element 1; on completion set `ready`, raise `CREATED`.

### 14.9 `CMakeLists.txt`

Append the new sources to the libplctag target exactly as the old file did (see
`attic/CMakeLists.txt` for the variable name and style), listing:
`enip.c enip_session.c enip_cpf.c enip_cip.c enip_type.c enip_tag.c`. Do **not**
list anything under `attic/`.

### 14.10 Suggested build order for the junior developer

1. `enip_type.c` + unit test (pure, no I/O).
2. `enip_cip_encode_path` + unit test against `attic/enip_name.c` outputs.
3. `enip_cpf.c`, EIP encode/decode (`enip_session.c` static helpers) + round-trip tests.
4. `enip_session.c` connect → register → ForwardOpen → READY (the MVP path, §15)
   with a single hard-coded connected read.
5. `enip_tag.c` create + vtable + the §11 OPEN_PROBE (MVP stops here, §15.4).
6. OPEN_BULK/ReadFrag, writes, auto-sync scheduling, then the unconnected path.
7. Concurrency hardening pass against §13 (run with many tags + read/abort storms).

---

## 15. First end-to-end slice (MVP)

The architecture supports a thin slice that compiles, links, and round-trips
against a real ControlLogix without committing to the full feature set. The slice
is chosen to exercise the load-bearing parts (IO step machine, scheduler, rc
lifetime, ready gate) while deferring everything that only adds CIP surface.

### 15.1 Path choice: connected (ForwardOpen) first

The MVP uses the **connected path**. Rationale: a ControlLogix tag lives behind a
route to the CPU (e.g. backplane `1,0`). On the connected path ForwardOpen carries
that route once at session-open, and every subsequent read goes out as a bare CIP
request over `SendUnitData` — no per-request routing wrapper. The unconnected path
would instead require wrapping **every** request in CM `Unconnected_Send` (§14.5)
with the route path inline, which is more code to reach the same first packet. So
connected-first reaches a real PLC with less surface, not more.

`enip_cip_unconnected_send` / the unconnected path stay specified (§14.5) but are
**not** built in the MVP.

### 15.2 In scope

- IO thread + §5 non-blocking step machine (connect → register → ForwardOpen →
  READY), the §3 single `socket_wait_event`, and §5.1 framing.
- `enip_type`, `enip_cip_encode_path`, `enip_cip_encode_route`, EIP
  encode/decode, CPF **connected** wrap/unwrap, ForwardOpen.
- `enip_tag` create + read + status + abort + the §11 **OPEN_PROBE** only.
  Require the whole tag to fit one negotiated packet; if not,
  `PLCTAG_ERR_TOO_LARGE`.
- §6 rc lifetime, §13 locking, §11.6 ready gate, clean teardown (§10).

### 15.3 Deferred (gate out of the `op` switch)

OPEN_BULK windowing, ReadFrag continuation, writes (`enip_tag_write`,
`tag_data_written`), auto-sync, the unconnected path + Unconnected_Send, the
Multi-Service batch, and the min-heap scheduler. Guard these behind a single
`#define ENIP_MVP` so the deferred `op` cases are compiled out and the build stays
honest about what is real.

### 15.4 Definition of done for the MVP

`plc_tag_create("protocol=enip-tcp&gateway=…&path=1,0&name=…&elem_count=1")`
blocks on the create-time OPEN_PROBE, returns a ready tag, `plc_tag_read` +
`plc_tag_get_*` return live values, `plc_tag_abort` cancels cleanly, and
`plc_tag_destroy` / library shutdown join the IO thread within one wait cycle (no
blocking-read stall). Everything after that is additive and does not touch the
core.

## 16. Implementation Status (as of 2026-08-12)

### 16.1 Completed

All of §15.2 (MVP scope) is shipped and working against real hardware, plus a
large amount that §15.3 originally deferred. The core engine:

- IO thread non-blocking state machine: CONN_CONNECT → CONN_REGISTER →
  CONN_IDENTITY → CONN_OPEN → CONN_READY → CONN_SENDING → CONN_WAITING (§5)
- ForwardOpen / ForwardClose on the connected path (§15.1), plus a polite
  UnregisterSession on clean shutdown
- **Both transports for tag data.** Connected: ForwardOpen, then every request
  in a Connected Data Item over SendUnitData. Unconnected: no ForwardOpen at
  all, every request an `Unconnected_Send` (0x52) carrying the route over
  SendRRData. Bring-up (RegisterSession, Identity) is unconnected either way,
  so the choice is made in `on_identity_reply` from
  `enip_plc_prefers_connected(c->plc_type)` — Logix and OMRON NJ/NX want a
  connection, everything else including an unrecognized device does not. A
  `use_connected_msg=` attribute overrides that per tag and is part of the
  connection registry key, so the two transports never share a connection.
  Both funnel through `wrap_tag_frame`, and `c->cip_overhead` carries the
  envelope difference into every window and batch budget, so 0x0A batching,
  windowing and fragmentation work the same on either.
- `ENIP_OP_OPEN_PROBE` — discovers `elem_size`, `type_header`, `window_elems`,
  `write_window_elems`; raises `PLCTAG_EVENT_CREATED` (§11.2)
- `ENIP_OP_OPEN_BULK` — windowed bulk read of remaining elements from element 1
  onward (§11.2)
- `ENIP_OP_READ` / `ENIP_OP_WRITE` — windowed, respecting `window_elems` /
  `write_window_elems`; loop CONN_WAITING → CONN_SENDING for multi-window tags
- auto-sync read and write re-scheduling (§3, `op_time += interval`)
- Scheduler: sorted intrusive doubly-linked list, `pick_batch`, sched_mutex
  discipline, abort, rc lifetime (§3, §6, §7, §8, §13)
- **CIP Multiple Service Packet (0x0A) batching** — beyond the original §15.3
  deferral list. `pick_batch` accumulates batch-eligible (single-window READ or
  WRITE) tags up to the CIP payload budget; `build_batch_request` wraps them in a
  0x0A request; `handle_batch_reply` / `complete_batch` distribute per-tag
  sub-replies. OPEN_PROBE/OPEN_BULK and fragmented tags are never batch-eligible.
- `lib.c` fairness fix: `tag->read_in_flight` is cleared in
  `plc_tag_generic_handle_event_callbacks` when the `PLCTAG_EVENT_CREATED` handler
  fires. Without this, auto-sync tags whose tickler fired before OPEN_PROBE
  completed had `read_in_flight` permanently stuck set, starving them of all
  subsequent reads.
- `PLCTAG_EVENT_DATA_SENT` — the outbound mirror of `DATA_RECEIVED`, raised once
  per EIP packet from the single `step_sending` completion point, so batch and
  fragmented writes are covered without per-callsite duplication.

Auto-detection and dialects (§16a, and the two dialect-specific documents):

- Identity query during bring-up; `common/plc_classify.c` maps the reply to
  `enip_plc_type_t`; `enip_dialect_select()` picks the connection's dialect. **No
  `plc=` attribute is required** — `model=` exists only as an override.
- `enip_logix_dialect` / `enip_omron_dialect` (shares Logix `build`/`apply`
  verbatim, differing only in `requested_cip_size`) / `enip_pccc_dialect`
  (selected per *tag*, not per connection — see `enip_dialect.h`).
- Large Forward Open (0x5B) try/fallback with per-connection memory of a 0x08
  rejection.
- Byte-granular fragmentation for a single oversized element (§16a.6):
  ReadFrag/WriteFrag with a `frag_offset` cursor, `frag_align` default 8.
- PCCC families (PLC-5 / SLC / MicroLogix) end to end: `enip_pccc_addr.c` parses
  the logical address, `dialects/pccc/pccc_client.c` encodes Execute-PCCC (0x4B),
  including bit writes and chunking of requests that do not fit one PCCC packet.
- Tag and UDT enumeration for **both** dialects — `@tags` and `@udt/<id>`.
  Rockwell walks Symbol class 0x6B / Template class 0x6C
  (`dialects/rockwell/ab_listing.c` on the simulator side); OMRON walks its
  Variable / Variable Type objects with sibling and nested-member continuation
  (`dialects/omron/omron_listing.c`). PCCC enumerates via the File 0 system
  directory.

Surrounding API surface:

- Special tag names: `@connection` (status ring + `CONN_STATUS_*` events),
  `@identity`, `@tags`, `@udt/<id>`.
- `enip-udp` discovery (`client/enip_discover.c`): `gateway=<ip>[/cidr][:port]`,
  unicast or directed broadcast List Identity, one `PLCTAG_EVENT_DATA_RECEIVED`
  per record with the buffer updated before the callback runs. This tag type has
  its own struct and vtable — it is **not** an `enip_tag_t` (see `lib/tag.h`).
- Formatted-data / schema API (`plc_tag_get_formatted_data` and friends),
  `PLCTAG_FORMAT_RAW` + `PLCTAG_FORMAT_CBOR`, rendered from raw bytes plus a
  schema. The whole surface is physically absent from the installed header when
  `LIBPLCTAG_FEATURE_ENIP` is off (`libplctag.h.in` is configured, not copied).
- `role=server` tags on `enip-tcp`, backed by the folded-in device simulator
  under `server/`, including UDT template registration via the `udt=` attribute.

### 16.2 Verified by tests

In-sandbox, no hardware (all pass; these are the regression net for refactors):

- `server_tag_basic`, `server_udt`, `devsim_with_plctag` — `role=server` tags
  served to an `enip-tcp` client tag in the same process.
- `omron_udt_walk` — the OMRON `@udt` sibling/nested walk over real wire bytes.
- `omron_aphyt_metadata` — APHYT-COMPAT-PLAN.md Phases 1–2, driving
  `cip_dispatch_unconnected()` directly with hand-built CIP requests.
- `server_udt` test 5 — the same served DINT read over both transports plus an
  unconnected write, which is what fails if the `Unconnected_Send` envelope or
  its budget accounting is wrong.

Against real hardware (`src/tests/scripts/run_enip_tests.sh`, ControlLogix at
`10.206.1.40` path `1,4`, plus MicroLogix and PLC-5 for the PCCC path):

- `test_fairness` with 100 tags, `auto_sync_read_ms=200`, 10-second run: all 100
  tags received 51–52 completions (spread ≤ 1), std dev 0.34, min/max 0.981.
- `test_fairness` with 200 tags against the `ab_server` emulator.
- `tag_rw2` read/write for scalar DINTs and large arrays.

**Gap worth naming:** CI (`.github/workflows/ci.yml`) never sets
`LIBPLCTAG_FEATURE_ENIP`, so none of this tree is compiled, let alone tested, on
push. The feature also defaults to `0` in the top-level `CMakeLists.txt`. Until
that changes, "it builds" is a claim only about whoever last ran a local
ENIP-enabled configure.

### 16.3 Still deferred

| item | notes |
|---|---|
| **Multi-dim linearization helpers** | §16a.5's `enip_dims_to_linear`/`enip_linear_to_dims` are still unwritten, and the tag still has no `dimensions[]`. This is what blocks the next item. |
| **Arrays of oversized elements** | §16a.6 fragmentation handles `elem_count <= 1` only. An array whose *individual* elements each exceed the buffer still fails `PLCTAG_ERR_TOO_LARGE` at OPEN_PROBE, because per-element fragmentation interleaved with array windowing needs the helpers above. |
| **STRING / UDT reads through plain `plc_tag_read`** | The generic client can address them but cannot present them as typed data; only `@udt/<id>`'s raw walk gets at the definition. This is also what prevents exercising fragmentation in-sandbox — `device_sim` has no aggregate or STRING wire type, so nothing local can produce a `CIP_STATUS_FRAG` reply. |
| **`@listidentity` alias** | Specified in ENIP-UDP-DISCOVERY-EVENTS-DESIGN.md, not implemented: `enip_discover.c` accepts only the exact name `@identity`. |
| **JSON format, and schemas for `@tags`/`@udt`** | `PLCTAG_FORMAT_CBOR` covers identity. The listing tags have no schema at all, so a caller gets raw dialect-specific wire bytes back from `@tags`/`@udt` and must parse them itself. |
| **`enip-udp` read hang** | Open bug, reproduced on real hardware: a `protocol=enip-udp` client read never returns. Not a regression — it has never worked. |
| **Min-heap scheduler** | The scheduler remains an O(n)-insert sorted linked list as designed in §3. Intentional: the fairness runs above confirm the list is sufficient at current tag counts. Upgrade if profiling ever says otherwise. |

### 16.4 Multi-dialect (manufacturer) support — status

The architecture is specified in §16a and detailed in
[ROCKWELL-SPECIFIC-DESIGN.md](ROCKWELL-SPECIFIC-DESIGN.md) and
[OMRON-SPECIFIC-DESIGN.md](OMRON-SPECIFIC-DESIGN.md). All of it is built except
the last row.

| item | status | notes |
|---|---|---|
| **`enip_dialect_t` seam** | done | §16a.4: per-connection `build`/`apply` function pointers + `requested_cip_size`/`max_batch_cap`, selected from CIP Identity (`enip_dialect_select`) or overridden by `model=`. The vtable is in `client/enip_dialect.h`; the three implementations are in `dialects/*/…_client.c`, not in `enip_session.c`. |
| **Capability cleanups (no new features)** | done | `parse_forward_open_reply` sets `max_cip_packet_size` from what was actually requested (accepted, not restated in the reply — ForwardOpen with a Variable size type is accept/reject, not negotiate-down). Micro800 still rides `enip_logix_dialect` (no per-model dialect split), so `max_batch_cap = 1` for it specifically has not been done. |
| **Large Forward Open + try/fallback** | done | `build_forward_open(c, use_large)`: every connection attempts Large FO (`0x5B`) first, requesting `c->dialect->requested_cip_size`; `on_open_reply` falls back to standard FO (`0x54`, 504 bytes) once on CIP status `0x08` and remembers not to retry Large on that connection's later reconnects. Logix/OMRON request 4002/1892 bytes respectively — vendor documentation, not independently verified against real hardware. No per-catalog-model split (NX701's higher documented ceiling, a genuinely Large-FO-incapable pre-"E" Micro800); the try/fallback makes an optimistic per-family request safe either way, just not optimal for every model. |
| **Byte-granular fragmentation** (single element > buffer) | done | §16a.6. OPEN_PROBE always issues ReadFrag (`0x52`, offset 0); a `CIP_STATUS_FRAG` reply switches the tag into `ENIP_OP_OPEN_PROBE_FRAG`, growing `t->data` as fragments arrive, until the final (status-0) fragment sets `elem_count=1`, `fragmented_elem=1`, and computes `frag_write_chunk` = `floor(usable/frag_align)*frag_align`. Post-open reads/writes reuse the same cursor. **Never live-tested** — see the STRING/UDT row in §16.3 for why nothing local can produce a `CIP_STATUS_FRAG` reply. Verified only by full regression (no behavior change) plus code review against the classic `protocols/ab` algorithm. |
| **Tag / UDT enumeration** | done | Both dialects, client and simulator halves. Rockwell: Symbol class `0x6B` service `0x55` + Template class `0x6C`. OMRON: `GetInstanceListEx2` (`0x5F`) on class `0x6A` + Variable Type `0x6C`, with the sibling/nested member walk bounded by `udt_walk_pending[32]`. The client accumulates raw reply bytes only — there is no decode into a typed structure and no CBOR schema (§16.3). |
| **Multi-dim linearization helpers** | planned | §16a.5: `enip_dims_to_linear`/`enip_linear_to_dims` (common, DINT dims), ported from `ab_server/cip.c`. Tag gains `int32_t dimensions[3]; uint8_t num_dimensions;`. The one genuinely unstarted item, and the blocker for arrays of oversized elements. |

---

## 16a. Common vs. dialect-specific (multi-manufacturer support)

The architecture and machinery specified in §1–§14 is **common** — the IO thread,
scheduler, lifetime, framing, and the right-sized state machine are shared,
unconditionally, by every CIP device this client talks to. (A few service-builder
helpers in §14.5, such as `enip_cip_read_frag`, live in the common `enip_cip.c`
but are invoked by only one dialect — common *code*, dialect *use*.) The
manufacturer differences (Rockwell Logix/Micro800, OMRON NJ/NX) are confined to a
small `enip_dialect_t` selected
once per connection from the `plc=` attribute, plus two self-contained
enumeration modules. The full per-vendor detail, packet formats, and pseudocode
live in:

- [ROCKWELL-SPECIFIC-DESIGN.md](ROCKWELL-SPECIFIC-DESIGN.md)
- [OMRON-SPECIFIC-DESIGN.md](OMRON-SPECIFIC-DESIGN.md)

This section is the contract between the common core and those documents.

### 16a.1 Why so little is dialect-specific

The §11 right-sizing decision (probe one element, window every request to fit the
negotiated buffer, treat any unexpected status as failure) is what makes the
vendors converge. After it, the *fits-the-buffer* path is byte-for-byte identical
across Logix, Micro800, and OMRON:

- the symbol path (`0x91`/`0x28` segments) is the same;
- Read Tag `0x4C` / Write Tag `0x4D` are the same services;
- the on-wire reply header is the same (`Cx 00` atomic, `A0 02 <handle>`
  structure — OMRON's handle is a CRC16, Logix's a template handle, but we
  capture and replay it verbatim and never interpret it);
- whole-element array windowing is the same.

So an ordinary named tag that fits the buffer needs **no vendor code at all**
beyond dialect selection. Only three concerns actually diverge.

### 16a.2 What diverges, and where it lives

| concern | common or dialect | why |
|---|---|---|
| socket / IO thread / scheduler / rc / locking / framing (§3–§13) | **common** | transport and lifetime, identical for all CIP |
| EIP encap, CPF wrap, ForwardOpen/Close mechanics | **common** | same wire structure for all CIP |
| symbol path encoding, multi-dim → linear index | **common** | identical segments; linearization ported from `ab_server/cip.c` |
| OPEN_PROBE / OPEN_BULK / element-windowed READ/WRITE | **common** | the right-sizing state machine |
| `0x0A` Multiple Service batch | **common** | Micro800's lack of it is a *number* (§16a.3), not a branch |
| type code → element size, byte order, string layout | **common (data tables)** | `enip_type` table + `tag_byte_order_t`, not code |
| **requested connection size** | dialect **number** | per-model buffer sizes |
| **byte-granular fragmentation** (single element > buffer) | dialect `build`/`apply` | Rockwell `0x52`/`0x53` + `0x06` status vs OMRON Simple Data Segment `0x80` |
| **tag / UDT enumeration** | dialect `list_tags` | different CIP services, classes, attribute layouts; not needed for named I/O |

### 16a.3 How capabilities are expressed — never a vendor bool

A boolean vendor flag in common code *is* the `if/else` we are eliminating.
Every capability is expressed one of three ways, none of which branches on
manufacturer in the common core:

1. **A number feeding existing arithmetic.** `requested_cip_size` flows into the
   ForwardOpen builder; `max_batch_cap` flows into the `min()` that sizes a
   batch. Micro800 sets `max_batch_cap = 1`, and `pick_batch` already routes a
   one-tag batch through the single-in-flight path (`count == 1`), so "no `0x0A`"
   costs zero new branches.
2. **A runtime try/fallback transition.** Large Forward Open is *always*
   attempted; a CIP status `0x08` (Service Not Supported) triggers a one-time
   fall back to standard ForwardOpen. No model needs to be known in advance, and
   the granted size — not the requested size — sets `max_cip_packet_size`.
3. **A function pointer.** `build` / `apply` / `list_tags`. Different behavior is
   reached by pointer, not by `if(vendor)`.

### 16a.4 The dialect interface

```c
typedef struct enip_dialect_t {
    const char *name;

    size_t   requested_cip_size;  /* number -> ForwardOpen builder            */
    uint16_t max_batch_cap;       /* number -> min(); Micro800 = 1            */

    /* Encode the CIP request for t's current op. Covers the shared symbolic
       path AND the vendor byte-fragment path; the tag carries its mode and
       frag cursor, so there is no vendor switch here -- the dialect picks the
       encoding. Caller holds t->api_mutex. */
    Bytes  (*build)(Arena *a, enip_connection_t *c, enip_tag_p t);

    /* Consume ONE already-sliced CIP sub-reply. The common reply handler owns
       all 0x0A iteration: it parses the outer reply, and for a Multiple Service
       reply splits it into per-tag Bytes via the offset table; for a single
       reply it passes the whole CIP reply. Either way the dialect receives a
       raw CIP sub-reply Bytes and parses + interprets status itself (Rockwell
       0x06 partial-transfer is meaningful only to the Rockwell dialect). It
       copies into t->data and sets *more for another round trip. */
    int32_t (*apply)(enip_connection_t *c, enip_tag_p t, Bytes reply, bool *more);

    /* Tag/UDT enumeration; self-contained per vendor, built last. NULL until then. */
    int32_t (*list_tags)(enip_connection_t *c /* ... */);
} enip_dialect_t;
```

Two consequences worth stating:

- **`apply` is handed the response, never fetches it.** Because the common
  handler already iterates `0x0A` sub-replies into `Bytes` slices, the dialect
  parses a value it was given. This unifies the single-reply and batched-reply
  paths and removes the hardcoded `status != 0 → REMOTE_ERR` from common code
  (status meaning is now the dialect's job).
- **Byte-fragment mode is a generic test, not a vendor branch.** When
  `elem_size > usable` the tag enters byte-fragment mode (cursor = `frag_offset`,
  in bytes); when `elem_size <= usable` it stays in element-windowing mode. Both
  dialects' `build`/`apply` consult that generic mode and call the shared
  `enip_build_symbolic` / `enip_apply_symbolic` helpers for the common case,
  diverging only inside fragment mode.

### 16a.5 Shared multi-dimensional helpers

Byte-granular fragmentation addresses a flat byte offset, but windowing a
*multi-dimensional* array requires converting a linear element offset back into
per-dimension indices to emit the right `0x28` segments. The helpers
(`enip_dims_to_linear` / `enip_linear_to_dims`, ported from
`src/tools/ab_server/cip.c:1217`, row-major) are **common**. Dimensions are CIP
**DINT**: the tag carries `int32_t dimensions[3]; uint8_t num_dimensions;`.

### 16a.6 Fragment boundaries must be element-aligned (common rule)

Byte-granular fragmentation cannot cut a request at an arbitrary byte. The wire
protocols (Rockwell `0x52`/`0x53`, OMRON Simple Data Segment) both require each
fragment to end on a value boundary, so this is a **common** rule that the
shared fragment-planning code enforces for both dialects:

- **Atomic scalars are never fragmented.** An `INT`/`DINT`/`REAL`/`LINT`/`LREAL`
  is sent whole. This never constrains us in practice — an atomic is ≤ 8 bytes
  and always fits the buffer; an *array* of atomics is split by whole-element
  windowing (§11), not by byte fragmentation. Byte fragmentation therefore only
  ever applies to a single aggregate element (a structure/UDT or a string) that
  exceeds the buffer.
- **Aggregates fragment only on alignment boundaries.** A fragment offset must be
  a multiple of the element's alignment `A`, where `A` is the size of the
  **largest scalar member** of the structure (1, 2, 4, or 8 bytes). Strings
  obey the same rule (their `LEN` is a `DINT`, so `A = 4`). This guarantees no
  fragment ever splits a scalar member.

Fragment-size computation (shared, used by both dialects' `build`):

```text
A     = t->frag_align          /* largest scalar member; default 8 (see below) */
chunk = floor(usable / A) * A  /* round DOWN to an A-multiple                   */
if chunk == 0:                 /* one aligned value will not fit the buffer     */
    fail with PLCTAG_ERR_TOO_LARGE
last fragment runs to `total` (the trailing remainder is naturally aligned
because every prior offset was an A-multiple and `total` ends the element).
```

**Choosing `A` without enumeration.** Until member layout is known (it requires
the UDT/template enumeration of §5 in the vendor docs), default
`t->frag_align = 8`. Eight is always safe: every CIP scalar size (1/2/4/8)
divides 8, and CIP packs members on natural alignment, so an 8-aligned boundary
can never land inside a member. Enumeration may later lower `A` to the true
largest-scalar size to allow slightly larger chunks, but a too-large `A` only
costs a little payload efficiency — never correctness. The tag carries
`uint8_t frag_align;` (bytes), defaulting to 8.

This rule lives in the common fragment planner; the dialects only encode the
`(offset, chunk)` pair they are handed (Rockwell into the `0x52`/`0x53` request
fields, OMRON into the `0x80` path segment).

---

## Appendix A - Test Hardware and Tools

### Hosts

The following are host/port/path combinations for real hardware:

| Host       | Port | Path    | Notes                         |
| ---------- | ---- | ------- | ----------------------------- |
| `10.206.1.39` | 44818 | `1,0`   | Older L61 ControlLogix   |
| `10.206.1.40` | 44818 | `1,4`   | Newer L81 ControlLogix   |
| `10.206.1.37` | 44818 | `1,5`   | Very old L55 ControlLogix   |

### Tools

#### scan_eip_network

```text
 ./build/bin_dist/scan_eip_network --delay-max-ms=2000 --network=10.206.1.0/24
scan_eip_network(71397,0x1f4cf2240) malloc: nano zone abandoned due to inability to reserve vm space.
IP_Address	Port	Vendor	Device_Type	Product_Code	Revision	Status	Serial	Product_Name	State
10.206.1.39	4783	1	12	58	6.6	48	1916301	1756-ENBT/A	3
10.206.1.37	4783	1	12	58	4.8	48	1906443	1756-ENBT/A	3
10.206.1.36	4783	1	12	185	2.12	4	2627753215	1763-L16BWA B/12.00	0
10.206.1.40	4783	1	14	164	31.11	12384	16112002	1756-L81E/B	3
```

Above, the host at IP address 10.206.1.36 is a MicroLogix 1100.

The port is incorrect.

#### get_identity

```text
./build/bin_dist/get_identity  --tag='protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=generic&name=@identity'

get_identity(75495,0x1f4cf2240) malloc: nano zone abandoned due to inability to reserve vm space.
Using tag string: protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=generic&name=@identity
Tag created successfully (id=11)
Read completed successfully
Identity data size: 26 bytes

=== CIP Identity Object ===

Vendor ID: 1 (0x0001)
Device Type: 14 (0x000E)
Product Code: 164 (0x00A4)
Revision: 31.11
Status: 0x3060
Serial Number: 16112002 (0x00F5D982)
Product Name: 1756-L81E/B

=== Raw Data ===
01 00 0E 00 A4 00 1F 0B 60 30 82 D9 F5 00 0B 31 
37 35 36 2D 4C 38 31 45 2F 42 

SUCCESS!
```

#### list_tags_logix


##### 10.206.1.40/1/4

```text
./build/bin_dist/list_tags_logix 10.206.1.40 1,4

Starting with library version 2.7.0.
Tag "Program:MainProgram.one_second_pulse" Instance 0x0003 Type ID 0x00c1 BOOL: Boolean value.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=1&elem_count=1&name=Program:MainProgram.one_second_pulse"
Tag "Program:MainProgram.Count" Instance 0x0002 element type UDT (0x8f83) TIMER.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=12&elem_count=1&name=Program:MainProgram.Count"
Tag "Program:MainProgram.Routine:MainRoutine" Instance 0x0001 element type SYSTEM (0x106d).  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=0&elem_count=1&name=Program:MainProgram.Routine:MainRoutine"
Tag "TestArray2Dim[3,2]" Instance 0x0020 Type ID 0x40c4 DINT: Signed 32-bit integer value.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=4&elem_count=6&name=TestArray2Dim"
Tag "TestArray3Dim[4,3,2]" Instance 0x001f Type ID 0x60c4 DINT: Signed 32-bit integer value.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=4&elem_count=24&name=TestArray3Dim"
Tag "TestBigSINTArray[6000]" Instance 0x001e Type ID 0x20c2 SINT: Signed 8-bit integer value.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=1&elem_count=6000&name=TestBigSINTArray"
Tag "TestLargeBoolArray[512]" Instance 0x001d Type ID 0x20d3 32-bit bit string.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=4&elem_count=512&name=TestLargeBoolArray"
Tag "TestMESSAGEType" Instance 0x001c element type UDT (0x8fff) MESSAGE.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=240&elem_count=1&name=TestMESSAGEType"
Tag "TestManyBOOLFields" Instance 0x001b element type UDT (0x89a5) ManyBOOLFieldsUDT.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=8&elem_count=1&name=TestManyBOOLFields"
Tag "AnotherTestTag[20]" Instance 0x001a element type UDT (0xaac7) TestUDTMultiLevel2.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=1128&elem_count=20&name=AnotherTestTag"
Tag "AlarmLevelTest" Instance 0x0019 Type ID 0x00c4 DINT: Signed 32-bit integer value.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=4&elem_count=1&name=AlarmLevelTest"
Tag "TestUDTMultiLevel2[3]" Instance 0x0018 element type UDT (0xaac7) TestUDTMultiLevel2.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=1128&elem_count=3&name=TestUDTMultiLevel2"
Tag "TestUDTMultiLevel1[3]" Instance 0x0017 element type UDT (0xa314) TestUDTMultiLevel1.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=56&elem_count=3&name=TestUDTMultiLevel1"
Tag "TestUDTMultiField[3]" Instance 0x0016 element type UDT (0xa685) TestUDTMultiField.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=16&elem_count=3&name=TestUDTMultiField"
Tag "TestUDTBoolArray[3]" Instance 0x0015 element type UDT (0xa86a) TestUDTBoolArray.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=4&elem_count=3&name=TestUDTBoolArray"
Tag "TestLINTArray[10]" Instance 0x0014 Type ID 0x20c5 LINT: Signed 64-bit integer value.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=8&elem_count=10&name=TestLINTArray"
Tag "TestMultiDimArray[10,10,10]" Instance 0x0013 Type ID 0x60c4 DINT: Signed 32-bit integer value.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=4&elem_count=1000&name=TestMultiDimArray"
Tag "stop_barcodes" Instance 0x0012 Type ID 0x00c1 BOOL: Boolean value.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=1&elem_count=1&name=stop_barcodes"
Tag "one_second_timer" Instance 0x0011 element type UDT (0x8f83) TIMER.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=12&elem_count=1&name=one_second_timer"
Tag "barcode_index" Instance 0x0010 element type UDT (0x8f82) COUNTER.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=12&elem_count=1&name=barcode_index"
Tag "barcodes[11]" Instance 0x000f element type UDT (0xafce) STRING.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=88&elem_count=11&name=barcodes"
Tag "barcode" Instance 0x000e element type UDT (0x8fce) STRING.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=88&elem_count=1&name=barcode"
Tag "barcode_processed" Instance 0x000d Type ID 0x00c1 BOOL: Boolean value.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=1&elem_count=1&name=barcode_processed"
Tag "new_barcode" Instance 0x000c Type ID 0x00c1 BOOL: Boolean value.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=1&elem_count=1&name=new_barcode"
Tag "TestSINTArray[1000]" Instance 0x000b Type ID 0x20c2 SINT: Signed 8-bit integer value.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=1&elem_count=1000&name=TestSINTArray"
Tag "TestSSTRING" Instance 0x000a element type UDT (0x8fce) STRING.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=88&elem_count=1&name=TestSSTRING"
Tag "TestINTArray[1000]" Instance 0x0009 Type ID 0x20c3 INT: Signed 16-bit integer value.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=2&elem_count=1000&name=TestINTArray"
Tag "TestBOOL" Instance 0x0008 Type ID 0x00c1 BOOL: Boolean value.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=1&elem_count=1&name=TestBOOL"
Tag "TestBOOLArray[4]" Instance 0x0007 Type ID 0x20d3 32-bit bit string.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=4&elem_count=4&name=TestBOOLArray"
Tag "TestBigArray[1000]" Instance 0x0006 Type ID 0x20c4 DINT: Signed 32-bit integer value.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=4&elem_count=1000&name=TestBigArray"
Tag "TestDINTArray[10]" Instance 0x0005 Type ID 0x20c4 DINT: Signed 32-bit integer value.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=4&elem_count=10&name=TestDINTArray"
Tag "Program:MainProgram" Instance 0x0004 element type SYSTEM (0x1068).  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=0&elem_count=1&name=Program:MainProgram"
Tag "Task:MainTask" Instance 0x0003 element type SYSTEM (0x1070).  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=0&elem_count=1&name=Task:MainTask"
Tag "__CONTAINER" Instance 0x0002 Type ID 0x00c4 DINT: Signed 32-bit integer value.  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=4&elem_count=1&name=__CONTAINER"
Tag "Map:Local" Instance 0x0001 element type SYSTEM (0x1069).  tag string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=0&elem_count=1&name=Map:Local"
UDTs:
 UDT TestUDTMultiLevel1 (ID 314, 56 bytes, struct handle 1aa1):
    Field 0: is_valid, offset 0, array [1] of type Type ID 0x20d3 32-bit bit string.
    Field 1: test_UDTMultiField, offset 8, array [3] of type element type UDT (0xa685) TestUDTMultiField.
 UDT TestUDTMultiField (ID 685, 16 bytes, struct handle 4ec7):
    Field 0: ZZZZZZZZZZTestUDTMul0, offset 0, type Type ID 0x00c2 SINT: Signed 8-bit integer value.
    Field 1: field_BOOL, offset 0:0, type Type ID 0x00c1 BOOL: Boolean value.
    Field 2: field_INT, offset 2, type Type ID 0x00c3 INT: Signed 16-bit integer value.
    Field 3: field_DINT, offset 4, type Type ID 0x00c4 DINT: Signed 32-bit integer value.
    Field 4: field_LINT, offset 8, type Type ID 0x00c5 LINT: Signed 64-bit integer value.
 UDT TestUDTBoolArray (ID 86a, 4 bytes, struct handle 501e):
    Field 0: field_BOOL_Array, offset 0, array [1] of type Type ID 0x20d3 32-bit bit string.
 UDT ManyBOOLFieldsUDT (ID 9a5, 8 bytes, struct handle 2c1a):
    Field 0: ZZZZZZZZZZManyBOOLFi0, offset 0, type Type ID 0x00c2 SINT: Signed 8-bit integer value.
    Field 1: aLongBOOLFieldName1, offset 0:0, type Type ID 0x00c1 BOOL: Boolean value.
    Field 2: aLongBOOLFieldName2, offset 0:1, type Type ID 0x00c1 BOOL: Boolean value.
    Field 3: aLongBOOLFieldName3, offset 0:2, type Type ID 0x00c1 BOOL: Boolean value.
    Field 4: aLongBOOLFieldName4, offset 0:3, type Type ID 0x00c1 BOOL: Boolean value.
    Field 5: aLongBOOLFieldName5, offset 0:4, type Type ID 0x00c1 BOOL: Boolean value.
    Field 6: aLongBOOLFieldName6, offset 0:5, type Type ID 0x00c1 BOOL: Boolean value.
    Field 7: aLongBOOLFieldName7, offset 0:6, type Type ID 0x00c1 BOOL: Boolean value.
    Field 8: aLongBOOLFieldName8, offset 0:7, type Type ID 0x00c1 BOOL: Boolean value.
    Field 9: ZZZZZZZZZZManyBOOLFi9, offset 1, type Type ID 0x00c2 SINT: Signed 8-bit integer value.
    Field 10: aLongBOOLFieldName9, offset 1:0, type Type ID 0x00c1 BOOL: Boolean value.
    Field 11: aLongBOOLFieldName10, offset 1:1, type Type ID 0x00c1 BOOL: Boolean value.
    Field 12: aLongBOOLFieldName11, offset 1:2, type Type ID 0x00c1 BOOL: Boolean value.
    Field 13: aLongBOOLFieldName12, offset 1:3, type Type ID 0x00c1 BOOL: Boolean value.
    Field 14: aLongBOOLFieldName13, offset 1:4, type Type ID 0x00c1 BOOL: Boolean value.
    Field 15: aLongBOOLFieldName14, offset 1:5, type Type ID 0x00c1 BOOL: Boolean value.
    Field 16: aLongBOOLFieldName15, offset 1:6, type Type ID 0x00c1 BOOL: Boolean value.
    Field 17: aLongBOOLFieldName16, offset 1:7, type Type ID 0x00c1 BOOL: Boolean value.
    Field 18: ZZZZZZZZZZManyBOOLFi18, offset 2, type Type ID 0x00c2 SINT: Signed 8-bit integer value.
    Field 19: aLongBOOLFieldName17, offset 2:0, type Type ID 0x00c1 BOOL: Boolean value.
    Field 20: aLongBOOLFieldName18, offset 2:1, type Type ID 0x00c1 BOOL: Boolean value.
    Field 21: aLongBOOLFieldName19, offset 2:2, type Type ID 0x00c1 BOOL: Boolean value.
    Field 22: aLongBOOLFieldName20, offset 2:3, type Type ID 0x00c1 BOOL: Boolean value.
    Field 23: aLongBOOLFieldName21, offset 2:4, type Type ID 0x00c1 BOOL: Boolean value.
    Field 24: aLongBOOLFieldName22, offset 2:5, type Type ID 0x00c1 BOOL: Boolean value.
    Field 25: aLongBOOLFieldName23, offset 2:6, type Type ID 0x00c1 BOOL: Boolean value.
    Field 26: aLongBOOLFieldName24, offset 2:7, type Type ID 0x00c1 BOOL: Boolean value.
    Field 27: ZZZZZZZZZZManyBOOLFi27, offset 3, type Type ID 0x00c2 SINT: Signed 8-bit integer value.
    Field 28: aLongBOOLFieldName25, offset 3:0, type Type ID 0x00c1 BOOL: Boolean value.
    Field 29: aLongBOOLFieldName26, offset 3:1, type Type ID 0x00c1 BOOL: Boolean value.
    Field 30: aLongBOOLFieldName27, offset 3:2, type Type ID 0x00c1 BOOL: Boolean value.
    Field 31: aLongBOOLFieldName28, offset 3:3, type Type ID 0x00c1 BOOL: Boolean value.
    Field 32: aLongBOOLFieldName29, offset 3:4, type Type ID 0x00c1 BOOL: Boolean value.
    Field 33: aLongBOOLFieldName30, offset 3:5, type Type ID 0x00c1 BOOL: Boolean value.
    Field 34: aLongBOOLFieldName31, offset 3:6, type Type ID 0x00c1 BOOL: Boolean value.
    Field 35: aLongBOOLFieldName32, offset 3:7, type Type ID 0x00c1 BOOL: Boolean value.
    Field 36: ZZZZZZZZZZManyBOOLFi36, offset 4, type Type ID 0x00c2 SINT: Signed 8-bit integer value.
    Field 37: aLongBOOLFieldName33, offset 4:0, type Type ID 0x00c1 BOOL: Boolean value.
    Field 38: aLongBOOLFieldName34, offset 4:1, type Type ID 0x00c1 BOOL: Boolean value.
    Field 39: aLongBOOLFieldName35, offset 4:2, type Type ID 0x00c1 BOOL: Boolean value.
    Field 40: aLongBOOLFieldName36, offset 4:3, type Type ID 0x00c1 BOOL: Boolean value.
    Field 41: aLongBOOLFieldName37, offset 4:4, type Type ID 0x00c1 BOOL: Boolean value.
    Field 42: aLongBOOLFieldName38, offset 4:5, type Type ID 0x00c1 BOOL: Boolean value.
    Field 43: aLongBOOLFieldName39, offset 4:6, type Type ID 0x00c1 BOOL: Boolean value.
    Field 44: aLongBOOLFieldName40, offset 4:7, type Type ID 0x00c1 BOOL: Boolean value.
    Field 45: ZZZZZZZZZZManyBOOLFi45, offset 5, type Type ID 0x00c2 SINT: Signed 8-bit integer value.
    Field 46: aLongBOOLFieldName41, offset 5:0, type Type ID 0x00c1 BOOL: Boolean value.
    Field 47: aLongBOOLFieldName42, offset 5:1, type Type ID 0x00c1 BOOL: Boolean value.
    Field 48: aLongBOOLFieldName43, offset 5:2, type Type ID 0x00c1 BOOL: Boolean value.
    Field 49: aLongBOOLFieldName44, offset 5:3, type Type ID 0x00c1 BOOL: Boolean value.
    Field 50: aLongBOOLFieldName45, offset 5:4, type Type ID 0x00c1 BOOL: Boolean value.
    Field 51: aLongBOOLFieldName46, offset 5:5, type Type ID 0x00c1 BOOL: Boolean value.
    Field 52: aLongBOOLFieldName47, offset 5:6, type Type ID 0x00c1 BOOL: Boolean value.
    Field 53: aLongBOOLFieldName48, offset 5:7, type Type ID 0x00c1 BOOL: Boolean value.
    Field 54: ZZZZZZZZZZManyBOOLFi54, offset 6, type Type ID 0x00c2 SINT: Signed 8-bit integer value.
    Field 55: aLongBOOLFieldName49, offset 6:0, type Type ID 0x00c1 BOOL: Boolean value.
    Field 56: aLongBOOLFieldName50, offset 6:1, type Type ID 0x00c1 BOOL: Boolean value.
    Field 57: aLongBOOLFieldName51, offset 6:2, type Type ID 0x00c1 BOOL: Boolean value.
    Field 58: aLongBOOLFieldName52, offset 6:3, type Type ID 0x00c1 BOOL: Boolean value.
    Field 59: aLongBOOLFieldName53, offset 6:4, type Type ID 0x00c1 BOOL: Boolean value.
    Field 60: aLongBOOLFieldName54, offset 6:5, type Type ID 0x00c1 BOOL: Boolean value.
    Field 61: aLongBOOLFieldName55, offset 6:6, type Type ID 0x00c1 BOOL: Boolean value.
    Field 62: aLongBOOLFieldName56, offset 6:7, type Type ID 0x00c1 BOOL: Boolean value.
    Field 63: ZZZZZZZZZZManyBOOLFi63, offset 7, type Type ID 0x00c2 SINT: Signed 8-bit integer value.
    Field 64: aLongBOOLFieldName57, offset 7:0, type Type ID 0x00c1 BOOL: Boolean value.
    Field 65: aLongBOOLFieldName58, offset 7:1, type Type ID 0x00c1 BOOL: Boolean value.
    Field 66: aLongBOOLFieldName59, offset 7:2, type Type ID 0x00c1 BOOL: Boolean value.
    Field 67: aLongBOOLFieldName60, offset 7:3, type Type ID 0x00c1 BOOL: Boolean value.
 UDT TestUDTMultiLevel2 (ID ac7, 1128 bytes, struct handle d15b):
    Field 0: field_UDTBoolArray, offset 0, array [10] of type element type UDT (0xa86a) TestUDTBoolArray.
    Field 1: field_UDTMultLevel1, offset 40, array [10] of type element type UDT (0xa314) TestUDTMultiLevel1.
    Field 2: field_AnotherOne, offset 600, array [6] of type element type UDT (0xafce) STRING.
 UDT COUNTER (ID f82, 12 bytes, struct handle f82):
    Field 0: Control, offset 0, type Type ID 0x00c4 DINT: Signed 32-bit integer value.
    Field 1: PRE, offset 4, type Type ID 0x00c4 DINT: Signed 32-bit integer value.
    Field 2: ACC, offset 8, type Type ID 0x00c4 DINT: Signed 32-bit integer value.
    Field 3: CU, offset 3:7, type Type ID 0x00c1 BOOL: Boolean value.
    Field 4: CD, offset 3:6, type Type ID 0x00c1 BOOL: Boolean value.
    Field 5: DN, offset 3:5, type Type ID 0x00c1 BOOL: Boolean value.
    Field 6: OV, offset 3:4, type Type ID 0x00c1 BOOL: Boolean value.
    Field 7: UN, offset 3:3, type Type ID 0x00c1 BOOL: Boolean value.
 UDT TIMER (ID f83, 12 bytes, struct handle f83):
    Field 0: Control, offset 0, type Type ID 0x00c4 DINT: Signed 32-bit integer value.
    Field 1: PRE, offset 4, type Type ID 0x00c4 DINT: Signed 32-bit integer value.
    Field 2: ACC, offset 8, type Type ID 0x00c4 DINT: Signed 32-bit integer value.
    Field 3: EN, offset 3:7, type Type ID 0x00c1 BOOL: Boolean value.
    Field 4: TT, offset 3:6, type Type ID 0x00c1 BOOL: Boolean value.
    Field 5: DN, offset 3:5, type Type ID 0x00c1 BOOL: Boolean value.
 UDT STRING (ID fce, 88 bytes, struct handle fce):
    Field 0: LEN, offset 0, type Type ID 0x00c4 DINT: Signed 32-bit integer value.
    Field 1: DATA, offset 4, array [82] of type Type ID 0x20c2 SINT: Signed 8-bit integer value.
 UDT MESSAGE (ID fff, 240 bytes, struct handle fff):
    Field 0: offsettodata, offset 0, type Type ID 0x00c4 DINT: Signed 32-bit integer value.
    Field 1: Flags, offset 4, type Type ID 0x00c3 INT: Signed 16-bit integer value.
    Field 2: EW, offset 4:2, type Type ID 0x00c1 BOOL: Boolean value.
    Field 3: ER, offset 4:4, type Type ID 0x00c1 BOOL: Boolean value.
    Field 4: DN, offset 4:5, type Type ID 0x00c1 BOOL: Boolean value.
    Field 5: ST, offset 4:6, type Type ID 0x00c1 BOOL: Boolean value.
    Field 6: EN, offset 4:7, type Type ID 0x00c1 BOOL: Boolean value.
    Field 7: TO, offset 5:0, type Type ID 0x00c1 BOOL: Boolean value.
    Field 8: EN_CC, offset 5:1, type Type ID 0x00c1 BOOL: Boolean value.
    Field 9: ERR, offset 6, type Type ID 0x00c3 INT: Signed 16-bit integer value.
    Field 10: EXERR, offset 8, type Type ID 0x00c4 DINT: Signed 32-bit integer value.
    Field 11: exerrlength, offset 12, type Type ID 0x00c2 SINT: Signed 8-bit integer value.
    Field 12: ERR_SRC, offset 13, type Type ID 0x00c2 SINT: Signed 8-bit integer value.
    Field 13: DN_LEN, offset 14, type Type ID 0x00c3 INT: Signed 16-bit integer value.
    Field 14: REQ_LEN, offset 16, type Type ID 0x00c3 INT: Signed 16-bit integer value.
    Field 15: DestinationLink, offset 18, type Type ID 0x00c3 INT: Signed 16-bit integer value.
    Field 16: DestinationNode, offset 20, type Type ID 0x00c3 INT: Signed 16-bit integer value.
    Field 17: SourceLink, offset 22, type Type ID 0x00c3 INT: Signed 16-bit integer value.
    Field 18: Class, offset 24, type Type ID 0x00c3 INT: Signed 16-bit integer value.
    Field 19: Attribute, offset 26, type Type ID 0x00c3 INT: Signed 16-bit integer value.
    Field 20: Instance, offset 28, type Type ID 0x00c4 DINT: Signed 32-bit integer value.
    Field 21: LocalIndex, offset 32, type Type ID 0x00c4 DINT: Signed 32-bit integer value.
    Field 22: Channel, offset 36, type Type ID 0x00c2 SINT: Signed 8-bit integer value.
    Field 23: Rack, offset 37, type Type ID 0x00c2 SINT: Signed 8-bit integer value.
    Field 24: Group, offset 38, type Type ID 0x00c2 SINT: Signed 8-bit integer value.
    Field 25: Slot, offset 39, type Type ID 0x00c2 SINT: Signed 8-bit integer value.
    Field 26: Path, offset 40, type element type UDT (0x8fce) STRING.
    Field 27: Reserved1, offset 128, type Type ID 0x00c4 DINT: Signed 32-bit integer value.
    Field 28: RemoteIndex, offset 132, type Type ID 0x00c4 DINT: Signed 32-bit integer value.
    Field 29: RemoteElement, offset 136, type element type UDT (0x8fce) STRING.
    Field 30: Reserved2, offset 224, type Type ID 0x00c4 DINT: Signed 32-bit integer value.
    Field 31: UnconnectedTimeout, offset 228, type Type ID 0x00c4 DINT: Signed 32-bit integer value.
    Field 32: ConnectionRate, offset 232, type Type ID 0x00c4 DINT: Signed 32-bit integer value.
    Field 33: TimeoutMultiplier, offset 236, type Type ID 0x00c2 SINT: Signed 8-bit integer value.
SUCCESS!
```

#### tag_rw2

```text
build/bin_dist/tag_rw2 --type=uint32 '--protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=4&elem_count=512&name=TestLargeBoolArray'

Library version 2.7.0.
Processing argument 1 "--type=uint32".
Processing argument 2 "--tag=protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=4&elem_count=512&name=TestLargeBoolArray".
data[0]=1 (0x00000001)
data[1]=0 (0x00000000)
data[2]=0 (0x00000000)
data[3]=0 (0x00000000)
data[4]=0 (0x00000000)
data[5]=1 (0x00000001)
data[6]=0 (0x00000000)
data[7]=0 (0x00000000)
data[8]=0 (0x00000000)
data[9]=0 (0x00000000)
data[10]=1 (0x00000001)
data[11]=0 (0x00000000)
data[12]=0 (0x00000000)
data[13]=0 (0x00000000)
data[14]=0 (0x00000000)
data[15]=0 (0x00000000)
data[16]=0 (0x00000000)
data[17]=0 (0x00000000)
data[18]=0 (0x00000000)
data[19]=0 (0x00000000)
data[20]=0 (0x00000000)
data[21]=0 (0x00000000)
data[22]=0 (0x00000000)
data[23]=0 (0x00000000)
data[24]=0 (0x00000000)
data[25]=0 (0x00000000)
data[26]=0 (0x00000000)
data[27]=0 (0x00000000)
data[28]=0 (0x00000000)
data[29]=0 (0x00000000)
data[30]=0 (0x00000000)
data[31]=0 (0x00000000)
data[32]=0 (0x00000000)
data[33]=0 (0x00000000)
data[34]=1 (0x00000001)
data[35]=0 (0x00000000)
data[36]=0 (0x00000000)
data[37]=0 (0x00000000)
data[38]=0 (0x00000000)
data[39]=0 (0x00000000)
data[40]=0 (0x00000000)
data[41]=0 (0x00000000)
data[42]=0 (0x00000000)
data[43]=0 (0x00000000)
data[44]=0 (0x00000000)
data[45]=0 (0x00000000)
data[46]=0 (0x00000000)
data[47]=0 (0x00000000)
data[48]=0 (0x00000000)
data[49]=0 (0x00000000)
data[50]=0 (0x00000000)
data[51]=0 (0x00000000)
data[52]=0 (0x00000000)
data[53]=0 (0x00000000)
data[54]=0 (0x00000000)
data[55]=0 (0x00000000)
data[56]=0 (0x00000000)
data[57]=0 (0x00000000)
data[58]=0 (0x00000000)
data[59]=0 (0x00000000)
data[60]=0 (0x00000000)
data[61]=0 (0x00000000)
data[62]=0 (0x00000000)
data[63]=0 (0x00000000)
data[64]=0 (0x00000000)
data[65]=0 (0x00000000)
data[66]=0 (0x00000000)
data[67]=0 (0x00000000)
data[68]=0 (0x00000000)
data[69]=0 (0x00000000)
data[70]=0 (0x00000000)
data[71]=0 (0x00000000)
data[72]=0 (0x00000000)
data[73]=0 (0x00000000)
data[74]=0 (0x00000000)
data[75]=0 (0x00000000)
data[76]=0 (0x00000000)
data[77]=0 (0x00000000)
data[78]=0 (0x00000000)
data[79]=0 (0x00000000)
data[80]=0 (0x00000000)
data[81]=0 (0x00000000)
data[82]=0 (0x00000000)
data[83]=0 (0x00000000)
data[84]=0 (0x00000000)
data[85]=0 (0x00000000)
data[86]=0 (0x00000000)
data[87]=0 (0x00000000)
data[88]=0 (0x00000000)
data[89]=0 (0x00000000)
data[90]=0 (0x00000000)
data[91]=0 (0x00000000)
data[92]=0 (0x00000000)
data[93]=0 (0x00000000)
data[94]=0 (0x00000000)
data[95]=0 (0x00000000)
data[96]=0 (0x00000000)
data[97]=0 (0x00000000)
data[98]=0 (0x00000000)
data[99]=0 (0x00000000)
data[100]=0 (0x00000000)
data[101]=0 (0x00000000)
data[102]=0 (0x00000000)
data[103]=0 (0x00000000)
data[104]=0 (0x00000000)
data[105]=0 (0x00000000)
data[106]=0 (0x00000000)
data[107]=0 (0x00000000)
data[108]=0 (0x00000000)
data[109]=0 (0x00000000)
data[110]=0 (0x00000000)
data[111]=0 (0x00000000)
data[112]=0 (0x00000000)
data[113]=0 (0x00000000)
data[114]=0 (0x00000000)
data[115]=0 (0x00000000)
data[116]=0 (0x00000000)
data[117]=0 (0x00000000)
data[118]=0 (0x00000000)
data[119]=0 (0x00000000)
data[120]=0 (0x00000000)
data[121]=0 (0x00000000)
data[122]=0 (0x00000000)
data[123]=0 (0x00000000)
data[124]=0 (0x00000000)
data[125]=0 (0x00000000)
data[126]=0 (0x00000000)
data[127]=0 (0x00000000)
data[128]=0 (0x00000000)
data[129]=0 (0x00000000)
data[130]=0 (0x00000000)
data[131]=0 (0x00000000)
data[132]=0 (0x00000000)
data[133]=0 (0x00000000)
data[134]=0 (0x00000000)
data[135]=0 (0x00000000)
data[136]=0 (0x00000000)
data[137]=0 (0x00000000)
data[138]=0 (0x00000000)
data[139]=0 (0x00000000)
data[140]=0 (0x00000000)
data[141]=0 (0x00000000)
data[142]=0 (0x00000000)
data[143]=0 (0x00000000)
data[144]=0 (0x00000000)
data[145]=0 (0x00000000)
data[146]=0 (0x00000000)
data[147]=0 (0x00000000)
data[148]=0 (0x00000000)
data[149]=0 (0x00000000)
data[150]=0 (0x00000000)
data[151]=0 (0x00000000)
data[152]=0 (0x00000000)
data[153]=0 (0x00000000)
data[154]=0 (0x00000000)
data[155]=0 (0x00000000)
data[156]=0 (0x00000000)
data[157]=0 (0x00000000)
data[158]=0 (0x00000000)
data[159]=0 (0x00000000)
data[160]=0 (0x00000000)
data[161]=0 (0x00000000)
data[162]=0 (0x00000000)
data[163]=0 (0x00000000)
data[164]=0 (0x00000000)
data[165]=0 (0x00000000)
data[166]=0 (0x00000000)
data[167]=0 (0x00000000)
data[168]=0 (0x00000000)
data[169]=0 (0x00000000)
data[170]=0 (0x00000000)
data[171]=0 (0x00000000)
data[172]=0 (0x00000000)
data[173]=0 (0x00000000)
data[174]=0 (0x00000000)
data[175]=0 (0x00000000)
data[176]=0 (0x00000000)
data[177]=0 (0x00000000)
data[178]=0 (0x00000000)
data[179]=0 (0x00000000)
data[180]=0 (0x00000000)
data[181]=0 (0x00000000)
data[182]=0 (0x00000000)
data[183]=0 (0x00000000)
data[184]=0 (0x00000000)
data[185]=0 (0x00000000)
data[186]=0 (0x00000000)
data[187]=0 (0x00000000)
data[188]=0 (0x00000000)
data[189]=0 (0x00000000)
data[190]=0 (0x00000000)
data[191]=0 (0x00000000)
data[192]=0 (0x00000000)
data[193]=0 (0x00000000)
data[194]=0 (0x00000000)
data[195]=0 (0x00000000)
data[196]=0 (0x00000000)
data[197]=0 (0x00000000)
data[198]=0 (0x00000000)
data[199]=0 (0x00000000)
data[200]=0 (0x00000000)
data[201]=0 (0x00000000)
data[202]=0 (0x00000000)
data[203]=0 (0x00000000)
data[204]=0 (0x00000000)
data[205]=0 (0x00000000)
data[206]=1 (0x00000001)
data[207]=0 (0x00000000)
data[208]=0 (0x00000000)
data[209]=0 (0x00000000)
data[210]=0 (0x00000000)
data[211]=0 (0x00000000)
data[212]=0 (0x00000000)
data[213]=0 (0x00000000)
data[214]=0 (0x00000000)
data[215]=0 (0x00000000)
data[216]=0 (0x00000000)
data[217]=0 (0x00000000)
data[218]=0 (0x00000000)
data[219]=0 (0x00000000)
data[220]=0 (0x00000000)
data[221]=0 (0x00000000)
data[222]=0 (0x00000000)
data[223]=0 (0x00000000)
data[224]=0 (0x00000000)
data[225]=0 (0x00000000)
data[226]=0 (0x00000000)
data[227]=0 (0x00000000)
data[228]=0 (0x00000000)
data[229]=0 (0x00000000)
data[230]=0 (0x00000000)
data[231]=0 (0x00000000)
data[232]=0 (0x00000000)
data[233]=0 (0x00000000)
data[234]=0 (0x00000000)
data[235]=0 (0x00000000)
data[236]=0 (0x00000000)
data[237]=0 (0x00000000)
data[238]=0 (0x00000000)
data[239]=0 (0x00000000)
data[240]=0 (0x00000000)
data[241]=0 (0x00000000)
data[242]=0 (0x00000000)
data[243]=0 (0x00000000)
data[244]=0 (0x00000000)
data[245]=0 (0x00000000)
data[246]=0 (0x00000000)
data[247]=0 (0x00000000)
data[248]=0 (0x00000000)
data[249]=0 (0x00000000)
data[250]=0 (0x00000000)
data[251]=0 (0x00000000)
data[252]=0 (0x00000000)
data[253]=0 (0x00000000)
data[254]=0 (0x00000000)
data[255]=0 (0x00000000)
data[256]=0 (0x00000000)
data[257]=0 (0x00000000)
data[258]=0 (0x00000000)
data[259]=0 (0x00000000)
data[260]=0 (0x00000000)
data[261]=0 (0x00000000)
data[262]=0 (0x00000000)
data[263]=0 (0x00000000)
data[264]=0 (0x00000000)
data[265]=0 (0x00000000)
data[266]=0 (0x00000000)
data[267]=0 (0x00000000)
data[268]=0 (0x00000000)
data[269]=0 (0x00000000)
data[270]=0 (0x00000000)
data[271]=0 (0x00000000)
data[272]=0 (0x00000000)
data[273]=0 (0x00000000)
data[274]=0 (0x00000000)
data[275]=0 (0x00000000)
data[276]=0 (0x00000000)
data[277]=0 (0x00000000)
data[278]=0 (0x00000000)
data[279]=0 (0x00000000)
data[280]=0 (0x00000000)
data[281]=0 (0x00000000)
data[282]=0 (0x00000000)
data[283]=0 (0x00000000)
data[284]=0 (0x00000000)
data[285]=0 (0x00000000)
data[286]=0 (0x00000000)
data[287]=0 (0x00000000)
data[288]=0 (0x00000000)
data[289]=0 (0x00000000)
data[290]=0 (0x00000000)
data[291]=0 (0x00000000)
data[292]=0 (0x00000000)
data[293]=0 (0x00000000)
data[294]=0 (0x00000000)
data[295]=0 (0x00000000)
data[296]=0 (0x00000000)
data[297]=0 (0x00000000)
data[298]=0 (0x00000000)
data[299]=0 (0x00000000)
data[300]=1 (0x00000001)
data[301]=1 (0x00000001)
data[302]=0 (0x00000000)
data[303]=0 (0x00000000)
data[304]=0 (0x00000000)
data[305]=0 (0x00000000)
data[306]=0 (0x00000000)
data[307]=0 (0x00000000)
data[308]=0 (0x00000000)
data[309]=0 (0x00000000)
data[310]=0 (0x00000000)
data[311]=0 (0x00000000)
data[312]=0 (0x00000000)
data[313]=0 (0x00000000)
data[314]=0 (0x00000000)
data[315]=0 (0x00000000)
data[316]=0 (0x00000000)
data[317]=0 (0x00000000)
data[318]=0 (0x00000000)
data[319]=0 (0x00000000)
data[320]=0 (0x00000000)
data[321]=0 (0x00000000)
data[322]=0 (0x00000000)
data[323]=0 (0x00000000)
data[324]=0 (0x00000000)
data[325]=0 (0x00000000)
data[326]=0 (0x00000000)
data[327]=0 (0x00000000)
data[328]=0 (0x00000000)
data[329]=0 (0x00000000)
data[330]=0 (0x00000000)
data[331]=0 (0x00000000)
data[332]=0 (0x00000000)
data[333]=0 (0x00000000)
data[334]=0 (0x00000000)
data[335]=0 (0x00000000)
data[336]=0 (0x00000000)
data[337]=0 (0x00000000)
data[338]=0 (0x00000000)
data[339]=0 (0x00000000)
data[340]=0 (0x00000000)
data[341]=0 (0x00000000)
data[342]=0 (0x00000000)
data[343]=0 (0x00000000)
data[344]=0 (0x00000000)
data[345]=0 (0x00000000)
data[346]=0 (0x00000000)
data[347]=0 (0x00000000)
data[348]=0 (0x00000000)
data[349]=0 (0x00000000)
data[350]=0 (0x00000000)
data[351]=0 (0x00000000)
data[352]=0 (0x00000000)
data[353]=0 (0x00000000)
data[354]=0 (0x00000000)
data[355]=0 (0x00000000)
data[356]=0 (0x00000000)
data[357]=0 (0x00000000)
data[358]=0 (0x00000000)
data[359]=0 (0x00000000)
data[360]=0 (0x00000000)
data[361]=0 (0x00000000)
data[362]=0 (0x00000000)
data[363]=0 (0x00000000)
data[364]=0 (0x00000000)
data[365]=0 (0x00000000)
data[366]=0 (0x00000000)
data[367]=0 (0x00000000)
data[368]=0 (0x00000000)
data[369]=0 (0x00000000)
data[370]=0 (0x00000000)
data[371]=0 (0x00000000)
data[372]=0 (0x00000000)
data[373]=0 (0x00000000)
data[374]=0 (0x00000000)
data[375]=0 (0x00000000)
data[376]=0 (0x00000000)
data[377]=0 (0x00000000)
data[378]=0 (0x00000000)
data[379]=0 (0x00000000)
data[380]=0 (0x00000000)
data[381]=0 (0x00000000)
data[382]=0 (0x00000000)
data[383]=0 (0x00000000)
data[384]=0 (0x00000000)
data[385]=0 (0x00000000)
data[386]=0 (0x00000000)
data[387]=0 (0x00000000)
data[388]=0 (0x00000000)
data[389]=0 (0x00000000)
data[390]=0 (0x00000000)
data[391]=0 (0x00000000)
data[392]=0 (0x00000000)
data[393]=0 (0x00000000)
data[394]=0 (0x00000000)
data[395]=0 (0x00000000)
data[396]=0 (0x00000000)
data[397]=0 (0x00000000)
data[398]=0 (0x00000000)
data[399]=0 (0x00000000)
data[400]=0 (0x00000000)
data[401]=0 (0x00000000)
data[402]=0 (0x00000000)
data[403]=0 (0x00000000)
data[404]=0 (0x00000000)
data[405]=0 (0x00000000)
data[406]=0 (0x00000000)
data[407]=0 (0x00000000)
data[408]=0 (0x00000000)
data[409]=0 (0x00000000)
data[410]=0 (0x00000000)
data[411]=0 (0x00000000)
data[412]=0 (0x00000000)
data[413]=0 (0x00000000)
data[414]=0 (0x00000000)
data[415]=1 (0x00000001)
data[416]=0 (0x00000000)
data[417]=0 (0x00000000)
data[418]=0 (0x00000000)
data[419]=0 (0x00000000)
data[420]=0 (0x00000000)
data[421]=0 (0x00000000)
data[422]=0 (0x00000000)
data[423]=0 (0x00000000)
data[424]=0 (0x00000000)
data[425]=0 (0x00000000)
data[426]=0 (0x00000000)
data[427]=0 (0x00000000)
data[428]=0 (0x00000000)
data[429]=0 (0x00000000)
data[430]=0 (0x00000000)
data[431]=0 (0x00000000)
data[432]=0 (0x00000000)
data[433]=0 (0x00000000)
data[434]=0 (0x00000000)
data[435]=0 (0x00000000)
data[436]=0 (0x00000000)
data[437]=0 (0x00000000)
data[438]=0 (0x00000000)
data[439]=0 (0x00000000)
data[440]=0 (0x00000000)
data[441]=0 (0x00000000)
data[442]=0 (0x00000000)
data[443]=0 (0x00000000)
data[444]=0 (0x00000000)
data[445]=0 (0x00000000)
data[446]=0 (0x00000000)
data[447]=0 (0x00000000)
data[448]=0 (0x00000000)
data[449]=0 (0x00000000)
data[450]=0 (0x00000000)
data[451]=0 (0x00000000)
data[452]=0 (0x00000000)
data[453]=0 (0x00000000)
data[454]=0 (0x00000000)
data[455]=0 (0x00000000)
data[456]=0 (0x00000000)
data[457]=0 (0x00000000)
data[458]=0 (0x00000000)
data[459]=0 (0x00000000)
data[460]=0 (0x00000000)
data[461]=0 (0x00000000)
data[462]=0 (0x00000000)
data[463]=0 (0x00000000)
data[464]=0 (0x00000000)
data[465]=0 (0x00000000)
data[466]=0 (0x00000000)
data[467]=0 (0x00000000)
data[468]=0 (0x00000000)
data[469]=0 (0x00000000)
data[470]=0 (0x00000000)
data[471]=0 (0x00000000)
data[472]=0 (0x00000000)
data[473]=0 (0x00000000)
data[474]=0 (0x00000000)
data[475]=0 (0x00000000)
data[476]=0 (0x00000000)
data[477]=0 (0x00000000)
data[478]=0 (0x00000000)
data[479]=0 (0x00000000)
data[480]=0 (0x00000000)
data[481]=0 (0x00000000)
data[482]=0 (0x00000000)
data[483]=0 (0x00000000)
data[484]=0 (0x00000000)
data[485]=0 (0x00000000)
data[486]=0 (0x00000000)
data[487]=0 (0x00000000)
data[488]=0 (0x00000000)
data[489]=0 (0x00000000)
data[490]=0 (0x00000000)
data[491]=0 (0x00000000)
data[492]=0 (0x00000000)
data[493]=0 (0x00000000)
data[494]=0 (0x00000000)
data[495]=0 (0x00000000)
data[496]=0 (0x00000000)
data[497]=0 (0x00000000)
data[498]=0 (0x00000000)
data[499]=0 (0x00000000)
data[500]=0 (0x00000000)
data[501]=0 (0x00000000)
data[502]=0 (0x00000000)
data[503]=0 (0x00000000)
data[504]=0 (0x00000000)
data[505]=0 (0x00000000)
data[506]=0 (0x00000000)
data[507]=0 (0x00000000)
data[508]=0 (0x00000000)
data[509]=0 (0x00000000)
data[510]=0 (0x00000000)
data[511]=0 (0x00000000)
```

```text
./build/bin_dist/tag_rw2 --type=uint32 '--tag=protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=4&elem_count=10&name=TestDINTArray'

Library version 2.7.0.
Processing argument 1 "--type=uint32".
Processing argument 2 "--tag=protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=4&elem_count=10&name=TestDINTArray".
data[0]=3 (0x00000003)
data[1]=5 (0x00000005)
data[2]=5 (0x00000005)
data[3]=7 (0x00000007)
data[4]=5 (0x00000005)
data[5]=5 (0x00000005)
data[6]=5 (0x00000005)
data[7]=5 (0x00000005)
data[8]=5 (0x00000005)
data[9]=5 (0x00000005)
```


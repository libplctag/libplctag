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
| one element exceeds `cap` (pre-send check) | `PLCTAG_ERR_TOO_LARGE` → fragmentation, a later phase |

Because the window is sized to fit, a non-zero status means something genuinely
wrong (bad path, privilege, a too-small overhead constant, an unsupported type),
not a routine boundary condition — so failing fast is correct. A later phase adds
fragmentation; until then, anything that cannot be made to fit one packet fails
with `PLCTAG_ERR_TOO_LARGE`.

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

The old implementation now lives in `attic/` and must not be wired into the build.
Lift only small helpers, re-checked against this spec.

### 14.0 File list

```text
enip/
  CMakeLists.txt        builds the new sources into the libplctag target
  enip.h                protocol entry points (declared extern) + opaque types
  enip.c                init/teardown, attribute → protocol dispatch, registry
  enip_session.h        connection type (opaque), session API used by enip_tag
  enip_session.c        socket, IO thread, scheduler, EIP encap, lifetime  (largest)
  enip_cpf.h/.c         Common Packet Format wrap/unwrap
  enip_cip.h/.c         CIP path encode, ForwardOpen/Close, Read/Write/ReadFrag
  enip_type.h/.c        CIP type code → element size + tag_byte_order_t fill
  enip_tag.h            tag struct + op/state enums
  enip_tag.c            vtable, create, attribs, value accessors, open sequence
```

`enip_tag.h` includes `attic`-free headers only: `lib/tag.h`,
`enip_session.h`. `enip_session.c` is the only file that includes the socket
platform header and spawns a thread.

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
#define CIP_READ_FRAG   ((uint8_t)0x52)
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

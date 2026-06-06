# ENIP Implementation Execution Plan (Single-Allocation, Vector-Engine Rewrite)

**Date:** 2026-06-06
**Status:** Authoritative. Replaces all previous ENIP execution plans.
**Audience:** A junior developer implementing this end to end. Every struct, every packet
field, and every function is specified. Where a rule protects memory safety or
concurrency, it is spelled out, not assumed.

---

## 0. What we are building and why

We are building a new EtherNet/IP (ENIP over TCP) protocol module for libplctag, living
in `src/libplctag/protocols/enip/`. It talks to Allen-Bradley/Rockwell Logix, OMRON
NJ/NX, and PCCC-family controllers (PLC5/SLC/MicroLogix, including DH+ bridged and
Logix-over-PCCC).

It mirrors the **Modbus** module (`src/libplctag/protocols/mb/modbus.c`) in shape:

- One background thread per connection.
- A list of "active" tags the thread services.
- The thread blocks on the socket's wake channel; the API thread wakes it by writing to
  that channel. No condition variables, no polling, no `sleep_ms`.

It differs from Modbus in two deliberate ways:

1. **Linear control flow.** The connection thread is straight-line blocking-style code
   (connect → register → identify → open → serve loop), not a `switch(state)` machine.
   The `PLCTAG_CONN_STATUS_*` values are *reported* to the application as events; they do
   **not** drive control flow.
2. **Manufacturer isolation.** All device-specific behavior lives behind a single
   strategy vtable (`enip_mfg_ops_t`) on the connection. Shared code contains **zero**
   `if(is_ab)…else if(is_omron)…` branching.

### 0.1 The three hard constraints

1. **No runtime allocation in the steady state.** The library runs on embedded systems
   with simple `malloc` implementations where fragmentation is fatal over time. After a
   tag is created, servicing a read or write must not allocate or free anything on the
   heap. We achieve this by:
   - folding the operation state and the type metadata **into the tag's single
     allocation** (§3),
   - encoding requests into **per-connection scratch arenas** that are reset, never freed
     (§5),
   - using an **intrusive linked list** for the active-tag queue so enqueue/dequeue is
     pointer surgery, not a growable vector (§6).

2. **Bounded memory.** Because each tag holds exactly one operation slot and there is no
   separate growable queue of buffered requests, total memory is `O(tags created)` plus
   two fixed per-connection arenas. A buggy or malicious caller cannot inflate memory
   beyond the tags it explicitly creates.

3. **The existing protocols and the generic API keep working.** This rewrite must not
   break AB/Modbus/OMRON. See the caveat in §2.

---

## 1. Separation of concerns (read this before writing any code)

The system is layered. Each layer knows only about the layer directly beneath it. The
"blast radius" of a change is one layer.

```
   Application (libplctag public API in src/libplctag/lib/lib.c)
        │  reads/writes tag->data, tag->size, tag->status  (generic, protocol-agnostic)
        ▼
   ┌─────────────────────────────────────────────────────────────────┐
   │ Tag layer        enip_tag.c                                       │
   │   - the generic vtable (read/write/status/abort/tickler/wake)     │
   │   - owns the tag allocation: app data + metadata + operation      │
   └─────────────────────────────────────────────────────────────────┘
        ▼ enqueues an Operation; reads back results
   ┌─────────────────────────────────────────────────────────────────┐
   │ Engine layer     enip_conn.c                                      │
   │   - the connection thread (linear flow)                           │
   │   - the intrusive active-tag queue                                │
   │   - scheduling: pick due tags, build, send, recv, dispatch        │
   │   - SHARED 0x0A multi-service packing (generic CIP)               │
   └─────────────────────────────────────────────────────────────────┘
        ▼ asks the strategy to encode/parse; calls transact()
   ┌──────────────────────────────┐   ┌──────────────────────────────┐
   │ Strategy layer  enip_mfg_*.c │   │ Transaction seam  enip_txn.c │
   │   - per-device CIP encode/    │   │   - CIP bytes in / out       │
   │     decode + STATUS meaning   │   │   - owns CPF + EIP framing   │
   │     behind one vtable         │   │   - owns send + recv + parse  │
   └──────────────────────────────┘   └──────────────────────────────┘
        ▼ produces/consumes CIP bytes
   ┌─────────────────────────────────────────────────────────────────┐
   │ Framing layer    enip_eip.c / enip_cpf.c / enip_cip.c            │
   │   - pure byte packers/parsers; no socket, no state               │
   └─────────────────────────────────────────────────────────────────┘
        ▼
   Platform socket + utils (rc.h, arena.h, bytes.h, hashtable.h, vector.h)
```

**Rules that keep the layers honest:**

- The **framing layer** never touches a socket and never holds state. It converts
  structs ↔ bytes using `bytes_pack`/`bytes_unpack`. It is trivially unit-testable.
- The **transaction seam** (`enip_txn.c`) is the *only* place that knows the order
  EIP-wraps-CPF-wraps-CIP and the only place that calls `socket_write_wait` /
  `enip_recv_frame`. Identity, ForwardOpen, metadata, and tag I/O all go through it.
  Change framing once, here.
- The **strategy layer** is the *only* place that knows a device's CIP dialect **and the
  meaning of a CIP status code** (see §9.1). Adding a device = add one `enip_mfg_*.c`
  file + one line in the selector. No other file changes.
- The **engine** never branches on manufacturer and never interprets a per-operation CIP
  status. It calls vtable hooks and `transact()`. It *does* own the generic CIP `0x0A`
  multi-service packing, because that service is identical on every device that supports
  it (§9.2, §15).
- The **tag layer** owns the allocation and the generic vtable. It never does I/O.

---

## 2. CAVEAT: keeping the old code and the generic API working

The generic public API in `src/libplctag/lib/lib.c` operates on the fields in
`TAG_BASE_STRUCT` (defined in `src/libplctag/lib/tag.h`). Investigation of `lib.c`
confirms it accesses only:

- `tag->data` and `tag->size` — every typed getter/setter bound-checks against
  `tag->size` (e.g. `lib.c:2485`, `lib.c:3088`) and reads/writes `tag->data`.
- `tag->byte_order`, `tag->bit`, `tag->is_bit` — for decoding.
- the status and event bitfields.

The generic API does **not** read `elem_size`, `elem_count`, or `data_type` directly.
Those values are exposed to the application **only** through the protocol's own vtable
hook:

```
plc_tag_get_int_attribute(id, "elem_size"/"elem_count", default)
    -> tag->vtable->get_int_attrib(tag, attrib_name, default)   (lib.c:2104)
```

AB implements this in `ab_get_int_attrib` (`ab_common.c:989`). **ENIP must implement the
same hook** and read the values from their new home (the metadata sub-struct, §3.3).

### 2.1 The consequence for our design

`size` and `data` **cannot move** into the metadata object — they live in
`TAG_BASE_STRUCT` and the generic API depends on them. Therefore:

> The metadata object is the **source of truth for type** (element size, count, CIP data
> type, dimensions). When metadata is resolved, the tag layer **writes through** the
> derived total size into `tag->size` and (re)allocates `tag->data` to match. The generic
> getters then keep working with no knowledge that ENIP changed anything.

`elem_size`/`elem_count`/`data_type` for AB/Modbus/OMRON remain top-level fields of
*their own* tag structs (`ab_tag_t`, etc.). We are **not** changing those structs. We are
only defining `enip_tag_t`. So the only code that must read ENIP's metadata from the new
location is ENIP's own `get_int_attrib`. Nothing else in the tree is affected, and the
other protocols keep running unchanged.

### 2.2 Registration touch-points (already present — verify, do not duplicate)

- `src/libplctag/lib/tag.h`: `TAG_PROTOCOL_ENIP = 8`, `TAG_PROTOCOL_ENIP_CONNECTION = 9`
  already exist in `tag_protocol_t`. Do not renumber.
- `src/libplctag/lib/lib.c`: the create dispatcher must route `enip-tcp`/`enip_tcp` to
  `enip_tag_create`. Confirm it is wired; if not, add it. This is the only edit to shared
  library files.

---

## 3. The tag allocation: one block, three regions

A tag is a single `rc_alloc` block. Conceptually it has three regions with three
different lifetimes and three different owners. They are separate **types** for clarity,
but physically contiguous so there is exactly one allocation per tag for its whole life.

```
  ┌──────────────────────────── enip_tag_t (one rc_alloc) ───────────────────────────┐
  │ TAG_BASE_STRUCT          generic, owned by lib.c (data, size, status, byte_order) │
  │ enip_tag_meta_t  meta;   type info, owned by the metadata code, read by getters   │
  │ enip_operation_t op;     transient I/O state, owned by the engine thread          │
  │ ── tail bytes ──         tag_name string; base encoded CIP path                   │
  └──────────────────────────────────────────────────────────────────────────────────┘
```

### 3.1 Region ownership and lifetime

| Region        | Owner (who writes)             | Lifetime               | Read by             |
|---------------|--------------------------------|------------------------|---------------------|
| `TAG_BASE_*`  | lib.c + tag layer              | whole tag life         | app, engine         |
| `meta`        | metadata code (engine thread)  | whole tag life; (re)resolved per connection generation | app getters, engine, strategy |
| `op`          | engine thread (exclusively, during an operation) | per operation, reused | engine, strategy |
| tail          | set once at create             | whole tag life, immutable | strategy (path), metadata code (name) |

### 3.2 `enip_operation_t` — engine-owned transient state

No heap pointers it owns. No byte buffer. The request is rebuilt from `chunk_offset` into
the connection arena each cycle; write data is read straight from `tag->data`.

```c
/* enip_op.h — included only by engine + strategy code, NOT by the app path. */

typedef enum {
    ENIP_OP_IDLE     = 0,  /* nothing requested; tag not in the active queue        */
    ENIP_OP_REQUEST  = 1,  /* read/write requested; waiting to be encoded + sent    */
    ENIP_OP_INFLIGHT = 2,  /* request sent; awaiting (more) response                */
    ENIP_OP_DONE     = 3,  /* terminal for this op; result written back to the tag  */
} enip_op_state_t;

typedef enum {
    ENIP_OP_KIND_NONE  = 0,
    ENIP_OP_KIND_READ  = 1,
    ENIP_OP_KIND_WRITE = 2,
} enip_op_kind_t;

typedef struct enip_tag_t enip_tag_t;   /* forward decl */

typedef struct enip_operation_t {
    /* --- intrusive active-queue node (see §6.1). NULL when not queued. --- */
    enip_tag_t *q_next;
    enip_tag_t *q_prev;

    /* --- scheduling --- */
    int64_t  op_time;       /* time_ms() the op became due; queue is sorted by this  */

    /* --- correlation --- */
    uint64_t transaction_id;/* EIP sender_context used for this op; matches response */

    /* --- fragmentation cursor --- */
    uint32_t chunk_offset;  /* bytes of tag->data already transferred this op        */

    /* --- pre-encoded base CIP path (points into the tag tail, immutable) --- */
    const uint8_t *encoded_path;
    uint16_t       encoded_path_len;

    /* --- small scalars --- */
    int32_t  op_state;      /* enip_op_state_t                                       */
    int32_t  kind;          /* enip_op_kind_t                                        */
} enip_operation_t;
```

Why the encoded path lives here: the application never sees it; it exists only to build
requests. (For a fragmented AB read where `chunk_offset > 0`, the strategy re-encodes a
path with an explicit start index into the *arena* — transient — so no extra storage is
needed; see `enip_mfg_ab.c`.)

### 3.3 `enip_tag_meta_t` — type info + the validity gate

```c
typedef enum {
    ENIP_META_NONE     = 0,  /* never resolved on this connection                   */
    ENIP_META_RESOLVING= 1,  /* a phase-2 fetch is in progress                      */
    ENIP_META_READY    = 2,  /* type info valid for `generation`                    */
} enip_meta_state_t;

typedef struct enip_tag_meta_t {
    uint32_t instance_id;    /* Symbol instance ID resolved from the name (phase-1)  */
    uint32_t array_dims[3];  /* element counts per dimension; 0 = dimension unused   */
    int32_t  elem_count;     /* total elements = product of active dims (>=1)        */
    int32_t  elem_size;      /* bytes per element (from Symbol attr 7 / instance)    */
    int32_t  generation;     /* conn->metadata_generation this was fetched under     */
    uint16_t data_type;      /* CIP type code (Symbol attr 2)                        */
    uint8_t  num_dims;       /* 0=scalar, 1..3                                       */
    uint8_t  state;          /* enip_meta_state_t                                    */
} enip_tag_meta_t;
```

**The validity rule (memorize this):**

> A tag's metadata is **usable** iff `meta.state == ENIP_META_READY` **and**
> `meta.generation == conn->metadata_generation`.

`conn->metadata_generation` is bumped once per successful (re)connect (§5.6). Bumping that
single integer instantly marks **every** tag stale without walking them. A stale tag must
re-resolve its name → instance_id and re-fetch its type before its next operation — which
is correct even if the PLC program changed while we were disconnected. This replaces
brittle per-tag boolean flags that go wrong across reconnects.

### 3.4 `enip_tag_t`

```c
typedef struct enip_tag_t {
    TAG_BASE_STRUCT;                 /* generic; provides data, size, status, byte_order */

    struct enip_connection_t *conn;  /* back-pointer; holds an rc_inc ref (see §7)        */

    enip_tag_meta_t meta;            /* §3.3 */
    enip_operation_t op;             /* §3.2 */

    char    *tag_name;               /* points into the tail; root symbol name           */
    /* tail: tag_name bytes (NUL-terminated), then base encoded CIP path bytes           */
} enip_tag_t;
```

The `@connection` status tag keeps its own small struct (`enip_connection_tag_t`,
unchanged from today's file) and is out of scope for the data-path rules above.

---

## 4. The connection: sub-structs grouped by concern

`enip_connection_t` is created once per (gateway, path) pair and shared by every tag that
targets it (§7.1). Group the fields so each function takes the narrow piece it needs,
not the whole object. This keeps signatures honest and shrinks blast radius.

### 4.1 `enip_link_t` — transport

```c
typedef struct enip_link_t {
    sock_p  socket;             /* created ONCE in create; close() keeps the wake pipe   */
    socket_wait_state_t io;     /* restartable I/O state shared by send/recv wrappers    */
    char    host[128];
    uint16_t port;              /* default 44818                                         */

    /* route to the CPU: parsed from the "path" attribute, e.g. "1,0".                   */
    uint8_t  route_path[64];    /* raw CIP segment bytes                                 */
    uint8_t  route_path_words;  /* size in 16-bit words; 0 = no routing                  */
    int8_t   cpu_slot;          /* convenience: backplane slot, or -1 if none            */
} enip_link_t;
```

### 4.2 `enip_session_t` — negotiated EIP + CIP session

```c
typedef struct enip_session_t {
    /* EIP encapsulation session */
    uint32_t session_handle;    /* from RegisterSession                                  */
    uint64_t sender_context;    /* monotonically increasing; stamped into each request   */
    bool     established;

    /* CIP connected messaging (from ForwardOpen) */
    uint32_t cip_targ_conn_id;  /* O->T id returned by the PLC; goes in the CAI          */
    uint32_t cip_orig_conn_id;  /* T->O id we assigned                                   */
    uint16_t cip_conn_serial;   /* connection serial used in ForwardOpen/Close          */
    uint16_t cip_seq_num;       /* incremented before each connected send                */
    bool     cip_connection_open;

    /* Negotiated CIP packet sizes from ForwardOpen, PER DIRECTION (§5.6, §5.9).
     * These are the raw connection sizes the PLC granted; the usable CIP-payload
     * budget is derived from them in §5.9 (subtract the connected-layer overhead). */
    uint32_t cip_size_o_to_t;   /* originator->target: bounds our REQUEST frames         */
    uint32_t cip_size_t_to_o;   /* target->originator: bounds the RESPONSE frames        */

    /* unconnected message cap (no ForwardOpen); device/route dependent, ~504 typical    */
    uint32_t unconnected_cap;

    /* capabilities */
    bool     supports_multi_service;  /* generic CIP service 0x0A packing (§9.2)         */
    bool     used_extended_forward_open;
} enip_session_t;
```

### 4.3 `enip_connection_t`

```c
typedef struct enip_connection_t {
    enip_link_t    link;
    enip_session_t session;

    enip_mfg_ops_t *mfg_ops;        /* selected after GetIdentity (§9)                   */

    /* --- the only connection-level metadata: name -> instance_id --- */
    hashtable_p root_symbol_cache;  /* hash(name) -> enip_root_symbol_entry_t*           */
    mutex_p     root_symbol_mutex;
    int32_t     symbol_count;       /* from class 0x6B attr 3; used to presize the table */
    uint32_t    symbol_max_instance;/* from class 0x6B attr 2; iteration stop bound      */

    /* --- the active-tag queue (intrusive; see §6) --- */
    enip_tag_t *queue_head;         /* sorted ascending by op.op_time                    */
    enip_tag_t *queue_tail;
    mutex_p     queue_mutex;

    /* --- per-direction scratch; reset each cycle, never freed until destroy --- */
    Arena tx_arena;
    Arena rx_arena;

    /* --- reported status + reconnect bookkeeping --- */
    int32_t state;                  /* PLCTAG_CONN_STATUS_*; reporting only              */
    int32_t metadata_generation;    /* bumped per successful (re)connect (§3.3)          */
    int64_t last_message_time_ms;   /* for idle disconnect                               */
    int32_t connect_attempt_count;  /* for backoff                                       */

    /* --- statistics (optional, for the @connection tag) --- */
    uint64_t messages_sent;
    uint64_t messages_received;

    bool     shutdown_requested;
    thread_p thread;
} enip_connection_t;
```

### 4.4 `enip_root_symbol_entry_t` — one symbol-table row

Stored in a **single contiguous block** of `symbol_count` rows (presized; §5.5) so there
is one allocation for the whole table, freed as a unit on disconnect. No per-symbol
`malloc`.

```c
typedef struct enip_root_symbol_entry_t {
    char     name[128];     /* NUL-terminated root symbol name                          */
    uint32_t instance_id;   /* Symbol instance ID                                       */
} enip_root_symbol_entry_t;
```

(Phase-2 type info is **not** here — it lives in each tag's `meta`, §2.1.)

---

## 5. Packet definitions (every field; validated against the wire and the existing code)

All multi-byte integers are **little-endian**. Sizes are exact; an overrun causes a PLC
error. Constants live in `enip_packetizer.h` (`ENIP_PKT_*`).

### 5.1 EIP encapsulation header — 24 bytes (`enip_eip.c`)

| Offset | Size | Field           | Notes                                              |
|-------:|-----:|-----------------|----------------------------------------------------|
| 0      | 2    | command         | 0x0065 RegisterSession, 0x0066 Unregister, 0x006F SendRRData (unconnected), 0x0070 SendUnitData (connected) |
| 2      | 2    | length          | byte count of everything **after** this 24-byte header |
| 4      | 4    | session_handle  | 0 until RegisterSession returns one                |
| 8      | 4    | status          | 0 on request; PLC sets on reply                    |
| 12     | 8    | sender_context  | correlation id; echoed back unchanged              |
| 20     | 4    | options         | 0                                                  |

### 5.2 CPF — unconnected, SendRRData (`enip_cpf.c`)

Follows the EIP header in command 0x006F.

| Size | Field            | Value                                            |
|-----:|------------------|--------------------------------------------------|
| 4    | interface_handle | 0 (CIP)                                           |
| 2    | router_timeout   | 0                                                 |
| 2    | item_count       | 2                                                 |
| 2    | item0 type       | 0x0000 Null Address Item (NAI)                    |
| 2    | item0 length     | 0                                                 |
| 2    | item1 type       | 0x00B2 Unconnected Data Item (UDI)               |
| 2    | item1 length     | length of the CIP payload that follows            |
| N    | CIP payload      | the CIP request                                   |

### 5.3 CPF — connected, SendUnitData (`enip_cpf.c`)

Follows the EIP header in command 0x0070.

| Size | Field            | Value                                            |
|-----:|------------------|--------------------------------------------------|
| 4    | interface_handle | 0                                                 |
| 2    | router_timeout   | 0                                                 |
| 2    | item_count       | 2                                                 |
| 2    | item0 type       | 0x00A1 Connected Address Item (CAI)              |
| 2    | item0 length     | 4                                                 |
| 4    | connection_id    | `session.cip_targ_conn_id` (the O->T id)         |
| 2    | item1 type       | 0x00B1 Connected Data Item (CDI)                 |
| 2    | item1 length     | 2 + CIP payload length                            |
| 2    | sequence_number  | `++session.cip_seq_num` before each send         |
| N    | CIP payload      | the CIP request                                   |

On receive, `enip_cpf_extract_cdi_payload` strips the 2-byte sequence number.

### 5.4 CIP request bodies (`enip_cip.c`)

A CIP request begins with `service(1) + path_size_words(1) + path(path_size_words*2)`.
`path_size_words` counts **16-bit words**; the encoded path is byte-padded to even length.

**Read Tag — service 0x4C:** `service(1) 0x4C`, `path_size_words(1)`, `path(2*w)`,
`element_count(2)`.

**Read Tag Fragmented — service 0x52:** as 0x4C plus a trailing `byte_offset(4)`.

**Write Tag — service 0x4D:** `service(1) 0x4D`, `path_size_words(1)`, `path(2*w)`,
`data_type(2)`, `element_count(2)`, `data(D)`.

**Write Tag Fragmented — service 0x53:** as 0x4D with a `byte_offset(4)` inserted after
`element_count`.

**Encoded path segments** (`enip_cip_encode_tag_path`):
- Symbolic: `0x91, len, name_bytes[, 0x00 pad if len odd]`.
- Array index: `0x28, idx8` | `0x29, 0x00, idx16` | `0x2A, 0x00, idx32`.
- Member after `.` is another `0x91` symbolic segment.

### 5.5 CIP response header (`enip_cip_parse_response`)

| Size | Field              | Notes                                            |
|-----:|--------------------|--------------------------------------------------|
| 1    | reply_service      | request service \| 0x80 (e.g. 0x4C → 0xCC)       |
| 1    | reserved           | 0                                                |
| 1    | general_status     | meaning is **manufacturer-specific** — see §9.1  |
| 1    | ext_status_size    | count of 16-bit words that follow                |
| 2*n  | ext_status         | skipped by shared code                           |
| D    | data               | for a read: 2- or 4-byte type code, then values  |

The shared parser returns `general_status` and the data slice. It does **not** decide what
a status value *means* for fragmentation — that is the strategy's job (§9.1). The read
type-code prefix is stripped by `enip_cip_strip_type_code` on the **first** read chunk.

### 5.6 ForwardOpen — service 0x54 (standard) / 0x5B (extended) to Connection Manager

Path to CM precedes the body: `0x20, 0x06, 0x24, 0x01` (Class 0x06 Instance 0x01).

| Size | Field                         | Notes                                        |
|-----:|-------------------------------|----------------------------------------------|
| 1    | service (0x54 or 0x5B)        |                                              |
| 1    | path_size_words = 2           |                                              |
| 4    | path to CM = 20 06 24 01      |                                              |
| 1    | priority / tick_time          | 0x0A                                         |
| 1    | timeout_ticks                 | 0x0E                                         |
| 4    | O->T connection id            | 0 (the PLC fills the value it assigns)       |
| 4    | T->O connection id            | our `cip_orig_conn_id`                       |
| 2    | connection serial             | `cip_conn_serial`                            |
| 2    | originator vendor id          | a fixed nonzero id                           |
| 4    | originator serial             | a fixed nonzero serial                       |
| 1    | timeout multiplier            | 0x03                                         |
| 3    | reserved                      | 0                                            |
| 4    | O->T RPI                      | 1000000 (µs)                                 |
| 2/4  | O->T network params           | **2 bytes** for 0x54, **4 bytes** for 0x5B   |
| 4    | T->O RPI                      | 1000000                                      |
| 2/4  | T->O network params           | 2 / 4 bytes as above                         |
| 1    | transport class/trigger       | 0xA3                                         |
| 1    | connection_path_size (words)  |                                              |
| 2*w  | connection path               | route to CPU + CM (built by the strategy)    |

The **network params** low bits carry the connection size. Standard FO encodes it in 9
bits (≤ 511; practical 504); extended FO in 16 bits (practical ~4002). Request the large
size with 0x5B first; on CIP error fall back to 0x54.

**ForwardOpen reply (success) data:** `O->T conn id(4)`, `T->O conn id(4)`,
`conn serial(2)`, `orig vendor(2)`, `orig serial(4)`, `O->T API(4)`, `T->O API(4)`,
`app_reply_size(1)`, `reserved(1)`, `app_reply(...)`. Store the first field as
`cip_targ_conn_id`. Store the granted connection sizes (the values we sent, or smaller if
the device reduced them) as `session.cip_size_o_to_t` and `session.cip_size_t_to_o`.

### 5.7 ForwardClose — service 0x4E

Same CM path. Body: priority/reserved(1)=0, timeout_ticks(1)=0x0E, conn serial(2),
orig vendor(2), orig serial(4), connection_path_size words(1), connection path(2*w).
Failure is non-fatal (we are tearing down anyway).

### 5.8 Symbol class queries (Class 0x6B)

**Count query — once per connect, before the inventory walk.** GetAttributeList
(service 0x03) to Class 0x6B, Instance 0 (the class itself):

| Size | Field                | Value                                          |
|-----:|----------------------|------------------------------------------------|
| 1    | service = 0x03       | GetAttributeList                               |
| 1    | path_size_words = 2  |                                                |
| 4    | path = 20 6B 24 00   | Class 0x6B, Instance 0                          |
| 2    | attr_count = 2       |                                                |
| 2    | attr id = 0x0002     | **Max Instance** (highest instance id)         |
| 2    | attr id = 0x0003     | **Number of Instances**                        |

Reply data: `attr_count(2)`, then per attribute `id(2), status(2), value(...)`. Attr 2 →
`symbol_max_instance` (UDINT), attr 3 → `symbol_count` (UDINT). These are the **standard
CIP generic class attributes** (verified: attr 1 = Revision, attr 2 = Max Instance,
attr 3 = Number of Instances). If a device returns a status for attr 3, fall back to using
`symbol_max_instance` as a loose presize bound.

> Pitfall corrected: instance attributes 7 and 8 of the Symbol object are element **size**
> and **dimensions** (used in the per-tag phase-2 fetch). They are *not* the class counts.

**Inventory walk — GetInstanceAttributeList (service 0x55)** to Class 0x6B from instance 0,
requesting only attribute 1 (name): path `20 6B 25 <inst16>`, `attr_count=1`,
`attr id=0x0001`. Reply entries repeat `instance_id(4), name_len(2), name_bytes`. The
device signals "more fragments" via the generic-status convention; the shared walk treats
"non-final → request from `last_instance_id + 1`; final → stop", but it does **not**
hard-code `0x06` — see §9.1. Filter system tags (`__`-prefixed, contains `:`).

**Per-tag phase-2 fetch — GetInstanceAttributeList (service 0x55)** to the specific
instance, attrs 0x02 (data type), 0x07 (element size), 0x08 (dimensions). Reply data:
`symbol_type(2), element_size(2), dims[3](12)`.

### 5.9 Budget accounting (the part the old code got wrong)

The device enforces the **CIP packet size**, not the socket frame size. The relevant
budgets are derived from what ForwardOpen negotiated, per direction, by subtracting only
the overhead that lives **inside** that negotiated size:

```
connected   req_budget  = session.cip_size_o_to_t − CDI_header(4) − seq_id(2)   (= size − 6)
connected   resp_budget = session.cip_size_t_to_o − CDI_header(4) − seq_id(2)   (= size − 6)
unconnected     budget  = session.unconnected_cap − UDI_header(4)               (= cap − 4; no seq id)
```

Rationale: the EIP header (24), CPF header (8), and the Connected Address Item (8) are
pure transport that sits **below** the negotiated CIP size — the device's connection-size
accounting does not include them. They bound only the **socket scratch arena** (kept at
32 KB so it is never the limiting factor), never the CIP budget. Subtracting them from the
CIP budget (as the legacy `max_buffer − 46` did) double-counts and is wrong.

Constants in `enip_packetizer.h`:

```c
#define ENIP_PKT_CONNECTED_CIP_OVERHEAD   ((size_t)6)  /* CDI header(4) + seq id(2)     */
#define ENIP_PKT_UNCONNECTED_CIP_OVERHEAD ((size_t)4)  /* UDI header(4)                 */

/* 0x0A Multiple Service wrapper, INSIDE the CIP budget (generic CIP; §15) */
#define ENIP_PKT_MULTI_REQ_FIXED     ((size_t)7)  /* svc+path(3)+reserved+count(2)      */
#define ENIP_PKT_MULTI_RESP_FIXED    ((size_t)6)  /* svc+reserved+status+ext+count(2)   */
#define ENIP_PKT_MULTI_SLOT_OVERHEAD ((size_t)2)  /* one uint16 offset entry per slot   */

/* Static array bound ONLY — NOT a protocol limit. Packing is bounded by budget (§15). */
#define ENIP_PKT_MAX_SLOTS ((uint32_t)512)
```

---

## 6. The intrusive active-tag queue (no allocation)

### 6.1 Why intrusive

A growable vector reallocates as tags are added. The intrusive list embeds the link
pointers (`op.q_next`, `op.q_prev`) **inside the tag**, so linking/unlinking is pure
pointer assignment — zero allocation, ever. A tag is linked only while it has a pending
operation. The list is kept **sorted ascending by `op.op_time`**, so the head is the
oldest-due tag and "what is due now / how long until due" is O(1) at the head.

### 6.2 Queue API (all in `enip_conn.c`; the only code that touches the links)

```c
/* Insert keeping op_time ascending. Caller holds queue_mutex. No allocation. */
static void enip_queue_link(enip_connection_t *conn, enip_tag_t *tag);
/* Unlink (no-op if not linked). Caller holds queue_mutex. No allocation. */
static void enip_queue_unlink(enip_connection_t *conn, enip_tag_t *tag);
/* Head if its op_time <= now_ms, else NULL. Caller holds queue_mutex. No ref taken. */
static enip_tag_t *enip_queue_peek_due(enip_connection_t *conn, int64_t now_ms);
/* ms until head is due: 0 if due, ENIP_IDLE_WAIT if empty, else delta. Holds mutex. */
static int64_t enip_queue_next_wait(enip_connection_t *conn, int64_t now_ms);
```

---

## 7. Concurrency and the rc_inc safety rule (the part juniors get wrong)

### 7.1 Threads in play

- **API threads** (any number): the application calls `plc_tag_read`, `plc_tag_write`,
  `plc_tag_get_*`, `plc_tag_destroy`. These run the tag vtable functions.
- **One connection thread** per connection: runs the engine loop, does all socket I/O.

They share two things: the **active-tag queue** and each **tag's `op`/`meta`**.

### 7.2 The reference-count rule — why `rc_inc` exists

Tags are reference-counted via `src/utils/rc.h`. `rc_alloc` returns an object with count
1. `rc_inc(p)` adds a reference and returns `p` — **or returns NULL if the object is
already being destroyed**. `rc_dec(p)` drops a reference; at zero the destructor runs.

The danger: the engine finds a tag in the queue, then wants to work on it (`accept_chunk`,
write results) **after releasing `queue_mutex`** (we must never hold a mutex across I/O).
Between releasing the mutex and touching the tag, an API thread could `plc_tag_destroy`
it, freeing the memory. The engine would then use freed memory.

**The rule:**

> Before the engine touches a tag outside `queue_mutex`, take a reference **while holding
> the mutex** with `rc_inc`. If `rc_inc` returns NULL, the tag is being destroyed — skip
> it. When done, `rc_dec`.

Canonical pattern (use it everywhere the engine pulls a tag from the queue):

```c
enip_tag_t *tag = NULL;
critical_block(conn->queue_mutex) {
    enip_tag_t *cand = enip_queue_peek_due(conn, now_ms);
    if(cand) {
        tag = (enip_tag_t *)rc_inc(cand);   /* NULL if being destroyed */
        /* still under the mutex: safe to read cand here */
    }
}                                            /* mutex released here */

if(!tag) { return; }                         /* nothing due, or being destroyed */

/* We hold a reference: the tag cannot be freed under us though the mutex is released. */
int32_t rc = conn->mfg_ops->accept_chunk(tag, cip_response);
/* ... handle rc ... */

rc_dec(tag);                                 /* release our reference */
```

### 7.3 The tag destructor must cooperate

`plc_tag_destroy` eventually drops the application's reference. `enip_tag_destructor` must,
**while holding `queue_mutex`**, `enip_queue_unlink` the tag so the engine never peeks a
half-freed tag. Because the engine `rc_inc`s under the same mutex, they cannot race: either
the destructor unlinks first (engine never sees it) or the engine `rc_inc`s first
(refcount > 0, so the destructor blocks until the engine `rc_dec`s).

The tag holds an `rc_inc` ref on its **connection** (`tag->conn`), taken at create and
released in the destructor, so the connection outlives its tags. The connection destructor
sets `shutdown_requested`, wakes the socket, and **joins the thread before freeing the
queue, arenas, symbol table, or socket** — so the thread is never mid-access during free.
This join-before-free ordering is load-bearing; document it at the destructor.

### 7.4 Who owns `op` and `meta`, and when

One operation per tag means the API thread and the engine take turns; `op_state` is the
baton:

- **API thread** (`enip_tag_read`/`write`): only when `op.op_state == ENIP_OP_IDLE` does
  it set `op.kind`, `op.op_time`, `ENIP_OP_REQUEST`, link to the queue, `socket_wake`.
  After that it does not touch `op`.
- **Engine thread**: owns `op` from REQUEST through send/recv/fragment until it writes the
  result and sets DONE → IDLE, then unlinks. Only the engine writes `chunk_offset`,
  `transaction_id`, `meta` (during phase-2), `tag->status`, `tag->data`, `tag->size`.

All app-observable writes (`tag->status`, `tag->data`, `tag->size`, `meta`) happen under
the tag's existing `api_mutex`, so the application never sees a torn update.

### 7.5 Locking commandments

1. **Never hold a mutex across socket I/O.** `rc_inc`, release the mutex, then do I/O.
2. **Lock order:** `queue_mutex` then `api_mutex`. Never the reverse.
3. **Framing and transaction layers take no locks** — they get plain `Bytes` and a
   link/session by pointer.

---

## 8. The transaction seam (`enip_txn.c`)

One function owns "CIP request bytes in → CIP response bytes out", including CPF + EIP
framing, send, receive, and unwrap, so framing changes touch one place.

```c
typedef enum { ENIP_MSG_UNCONNECTED, ENIP_MSG_CONNECTED } enip_msg_mode_t;

/* Build CPF(mode)+EIP around cip_request, send it, receive one framed reply, strip
 * EIP+CPF, return the CIP response slice (into rx_arena).
 *  - Resets tx_arena, builds the frame, stamps session->sender_context and increments it
 *    (records the used value in *out_context for correlation).
 *  - CONNECTED: increments session->cip_seq_num and uses cip_targ_conn_id.
 *  - Sends with do/while(PENDING) around socket_write_wait using link->io, so a wake
 *    during I/O resumes instead of corrupting the frame.
 *  - Receives via enip_recv_frame (24-byte header then length-driven body).
 *  - Returns the CIP payload (UDI unconnected, CDI minus seq connected).
 * OK + *out_cip_response on success; PLCTAG_ERR_* on socket/frame error. */
int32_t enip_txn(enip_link_t *link, enip_session_t *session, Arena *tx_arena,
                 Arena *rx_arena, enip_msg_mode_t mode, Bytes cip_request,
                 uint64_t *out_context, Bytes *out_cip_response);
```

---

## 9. The manufacturer strategy vtable (`enip_mfg_ops.h`)

The only place device dialect **and CIP-status meaning** live. Grouped by what it acts on.

```c
typedef struct enip_mfg_ops_t {
    const char *name;                 /* "AB/Logix", "OMRON", "PCCC" — logging only      */
    enip_msg_mode_t messaging_mode;   /* connected (AB/OMRON) or unconnected (some PCCC) */

    /* ---- connection lifecycle (act on a connection) ---- */
    int32_t (*configure)(struct enip_connection_t *conn);        /* step 4: route, sizes */
    int32_t (*open_connection)(struct enip_connection_t *conn);  /* step 5: FO / no-op   */
    int32_t (*close_connection)(struct enip_connection_t *conn); /* ForwardClose / no-op */
    int32_t (*fetch_phase1)(struct enip_connection_t *conn);     /* step 6: inventory    */

    /* ---- per-tag operations (act on a tag) ---- */
    int32_t (*fetch_tag_metadata)(struct enip_tag_t *tag);       /* lazy phase-2         */

    /* Build the next CIP request chunk for tag->op given the per-slot REQUEST and
     * RESPONSE budgets. Subtract the device-fixed header sizes (req_fixed / resp_fixed),
     * call enip_chunk_split() for element-aligned dual-budget sizing (§15), emit CIP
     * bytes into arena. Return bytes_null() when the op is complete or one element does
     * not fit. Must also report resp_fixed via *out_resp_fixed so the engine can size
     * the response side of a packed frame (§15). */
    Bytes (*encode_chunk)(struct enip_tag_t *tag, Arena *arena,
                          size_t req_budget, size_t resp_budget, size_t *out_resp_fixed);

    /* Parse one CIP response chunk: check the general status (whose MEANING this hook
     * owns — see §9.1), strip the type code on the first read chunk, copy data into
     * tag->data at op.chunk_offset, advance chunk_offset. Return OK when the op is
     * complete, PLCTAG_ERR_PARTIAL when more chunks are needed, PLCTAG_ERR_* on failure. */
    int32_t (*accept_chunk)(struct enip_tag_t *tag, Bytes cip_response);
} enip_mfg_ops_t;

extern enip_mfg_ops_t enip_mfg_ab;
extern enip_mfg_ops_t enip_mfg_omron;
extern enip_mfg_ops_t enip_mfg_pccc;

/* Map a GetIdentity result to a strategy. Unknown CIP vendors fall back to AB with a
 * warning; never NULL for a reachable device. */
enip_mfg_ops_t *enip_select_mfg_ops(const enip_identity_t *identity);
```

### 9.1 CIP general status meaning is per-manufacturer (do not hard-code 0x06)

The CIP general-status byte's interpretation for fragmentation differs by device, so the
**continuation decision belongs only to `accept_chunk`.** Shared code (the engine, the
metadata walk) must never special-case a status value to mean "more fragments."

- **Rockwell:** `accept_chunk` continues while `status == 0x06` (PLC-side "partial")
  **or** `chunk_offset < total`.
- **OMRON:** never emits `0x06`; `accept_chunk` continues purely on `chunk_offset < total`
  (the client knows the total size and tracks the cursor).
- **PCCC:** its own status/STS/EXT-STS rules, entirely inside `accept_chunk`.

The engine sees only the abstract `OK` / `PLCTAG_ERR_PARTIAL` / error result.

### 9.2 0x0A multi-service packing is generic CIP and lives in shared code

The CIP Multiple Service Packet (service `0x0A` to the Message Router) is identical on
every device that supports it, so its assembly and its `0x8A` reply parsing stay in the
**engine** (§15), not behind the vtable. The only per-device input is the boolean
`session.supports_multi_service` (derived from identity). When false (e.g. PCCC), the
engine sends one request per cycle, built entirely by `encode_chunk`. There is **no**
per-manufacturer "assemble frame" hook.

---

## 10. Implementation phases

Each phase ends with a clean compile and a stated acceptance test. Do them in order.

### Phase 0 — Read and prep
**STATUS: ✓ COMPLETE**
- ✓ Read §1–§9 and `src/utils/{rc,arena,bytes}.h`
- ✓ Confirmed registration touch-points (TAG_PROTOCOL_ENIP=8, TAG_PROTOCOL_ENIP_CONNECTION=9 already present in tag.h)
- ✓ Fixed CMakeLists.txt: added missing `enip_eip.c`, `enip_eip.h`, `enip_cpf.c`, `enip_cpf.h`
- ✓ Created `enip_op.h` with enip_operation_t and enip_tag_meta_t structs
- ✓ Rewrote `tag.h` with new enip_tag_t layout
- ✓ Rewrote `enip_conn.h` with enip_link_t and enip_session_t sub-structs; simplified enip_root_symbol_entry_t
- ✓ Updated `enip_tag.c`: vtable functions, tag creation with tail storage
- ✓ Updated `enip_conn.c`: field renames, removed metadata cache, FO size handling
- ✓ Updated `enip_metadata.c`: removed per-connection cache, simplified root symbol fetch
- ✓ Updated `enip_mfg_ab.c`: all old field refs → new sub-struct paths
- ✓ Checked `enip_mfg_omron.c`, `enip_mfg_pccc.c`, `enip_mfg_selector.c`, `enip_cip.c`: all clean
- ✓ Tree builds successfully
- **Accept:** ✓ Complete

### Phase 1 — Types and framing
**STATUS: ✓ COMPLETE**
- ✓ Created/confirmed structs in §3–§4: tag.h, enip_op.h, enip_conn.h
- ✓ Confirmed framing functions (enip_eip.c, enip_cpf.c, enip_cip.c) present and correct
- ✓ Updated `enip_packetizer.h` constants (§5.9): ENIP_PKT_CONNECTED_CIP_OVERHEAD=6, ENIP_PKT_UNCONNECTED_CIP_OVERHEAD=4
- ✓ Updated `enip_packetizer_cip_budget(cip_size, use_connected)` to subtract only CIP-layer overhead
- ✓ Implemented `enip_chunk_split()` (§15.4) with enip_chunk_split_result_t for dual-budget element-aligned splitting
- ✓ Fixed all compilation errors in enip_conn.c (field refs → sub-struct paths) and enip_mfg_ab.c
- ✓ Tree builds successfully (all warnings are non-fatal sanitizer/conversion warnings)
- **Accept:** ✓ Complete. Tree compiles. Phase 2 may proceed.

### Phase 2 — Transaction seam
- Implement `enip_txn` (§8); re-express identity/ForwardOpen/metadata round trips in terms
  of it; delete the duplicated send/recv blocks.
- **Accept:** GetIdentity returns vendor/device/product.

### Phase 3 — Connection lifecycle + registry
- Global registry in `enip.c`: `enip_init` (registry mutex),
  `enip_registry_find_or_create(attribs)` (shared connection per host/port/route, creates
  thread on first use), `enip_teardown` (drain). Mirror Modbus `plcs`.
- `enip_connection_create`/`_destructor` per §4 and §7.3 (socket once; join-before-free;
  one symbol-table block freed as a unit).
- `enip_tag_create` calls the registry, `tag->conn = rc_inc(conn)`, queues nothing yet.
- **Accept:** two tags to one gateway share one connection/socket/ForwardOpen; destroying
  all tags joins the thread with no leak (leak checker).

### Phase 4 — Bootstrap sequence
- Linear bootstrap: TCP connect → RegisterSession → GetIdentity → `select_mfg_ops` →
  `configure` → `open_connection` (store `cip_size_o_to_t`/`_t_to_o` from the FO reply,
  §5.6/§5.9) → class-0x6B count query → `fetch_phase1` → bump `metadata_generation` →
  status UP.
- Symbol count query (§5.8); presize and **create** `root_symbol_cache` (today's code
  never creates it — fix it); inventory walk into the single-block table.
- **Accept:** after connect, the cache holds the PLC's tags; count matches attr 3.

### Phase 5 — Engine loop + queue
- Intrusive queue API (§6) and the serve loop: `enip_queue_next_wait` →
  `socket_wait_event` → resolve due-tag metadata → build → `enip_txn` → dispatch/fragment
  → idle disconnect → backoff. All tag access uses the `rc_inc` pattern (§7.2).
- `enip_tag_read`/`write` (set `op`, link, wake); `enip_tag_abort` (unlink, IDLE,
  aborted); `enip_tag_wake_plc`.
- **Accept:** a single AB DINT read completes end to end; abort cancels a pending read.

### Phase 6 — Metadata validity gate + app API
- `enip_ensure_due_tags_metadata`: name → instance_id; `fetch_tag_metadata`; write-through
  `tag->size` + one-time allocate `tag->data`; set `meta.generation`/`state=READY`.
- `enip_tag_status` and `enip_tag_get_int_attrib` honor the **usable** predicate (§3.3):
  PENDING (and refuse to expose `elem_size`/`elem_count`) until usable; expose from `meta`
  once ready (§2.1).
- **Accept:** reading before metadata resolves returns PENDING, never garbage; a forced
  reconnect re-resolves before the next read; `plc_tag_get_int_attribute("elem_size")`
  matches the PLC.

### Phase 7 — Budgets + multi-service packing (§9.2, §15)
- Wire `enip_packetizer_plan`/`fits_single` into `enip_connection_build_requests`. Pack
  due tags into a shared `0x0A` frame **when `supports_multi_service`** and both the
  request and response aggregates fit (budget-bound, not count-bound). Single tag → plain
  CIP. Use `encode_chunk`'s reported `resp_fixed` for the real response budget.
- **Accept:** several small reads coalesce into one `0x0A` frame; an oversized read chunks
  across cycles; both budgets respected; a `supports_multi_service=false` device sends one
  request per cycle.

### Phase 8 — Strategies
- **AB** (`enip_mfg_ab.c`): `configure` (backplane route, request large FO), FO_Ex→FO,
  `fetch_phase1` (inventory walk), `fetch_tag_metadata` (phase-2 into `meta`),
  `encode_chunk`/`accept_chunk` (0x4C/0x4D, 0x52/0x53 chunking, `0x06`-or-cursor §9.1).
- **OMRON** (`enip_mfg_omron.c`): connected, no phase-1, `0x80` data-segment chunking,
  cursor-only continuation (no `0x06`).
- **PCCC** (`enip_mfg_pccc.c`): PLC5/SLC/Logix-over-PCCC/DH+; routing in `configure`;
  Execute-PCCC (0x4B)+DF1 in `encode_chunk`; `supports_multi_service=false`; masked
  bit-writes.
- **Accept:** read/write parity with AB-EIP on the simulator for each family.

### Phase 9 — Tests
- ENIP coverage in `run_simulator_tests.sh`/`run_hardware_tests.sh`; document exclusions.
- **Accept:** simulator regression passes; no leak/UAF under sanitizer; no regression vs
  AB-EIP.

---

## 11. Function inventory (stubs with behavior)

### 11.1 Framing — `enip_eip.c`, `enip_cpf.c`, `enip_cip.c` (no socket, no locks)

- `enip_eip_build_request(arena, command, session_handle, *sender_context, cpf) -> Bytes`
  — pack the 24-byte header (§5.1), stamp `*sender_context`, then increment it.
- `enip_eip_extract_cpf_payload(response) -> Bytes` — slice off the 24-byte header.
- `enip_cpf_build_unconnected/_connected(...)` — §5.2/§5.3.
- `enip_cpf_extract_udi_payload/_cdi_payload(...)` — return the CIP slice (CDI strips seq).
- `enip_cip_encode_tag_path(name, buf, sz) -> size_t` — §5.4; 0 on invalid/small buffer.
- `enip_cip_read_tag_request` / `_read_tag_fragmented_request` / `_write_tag_request` /
  `_write_tag_fragmented_request` — §5.4 bodies.
- `enip_cip_parse_response(resp, *status, *ext_sz, *data_out) -> Bytes` — §5.5; returns the
  data slice; does **not** interpret the status (§9.1).
- `enip_cip_strip_type_code(data) -> Bytes` — drop the 2/4-byte read type prefix.

### 11.2 Packetizer — `enip_packetizer.c`

- `enip_packetizer_cip_budget(cip_size, mode) -> size_t` — subtract the §5.9 overhead:
  `cip_size − 6` connected, `cip_size − 4` unconnected. Returns 0 if too small.
- `enip_packetizer_fits_single(req_size, resp_size, req_budget, resp_budget) -> bool`.
- `enip_packetizer_plan(plan) -> int32_t` — verify N slots fit both the request and
  response budgets including the `0x0A` wrapper + offset table (§15.2).
- `enip_chunk_split(req_avail, resp_avail, remaining, elem_size, req_fixed, resp_fixed)
  -> {data_bytes, req_body, resp_body} | NONE` — dual-budget, element-aligned (§15.4).

### 11.3 Transaction — `enip_txn.c`
- `enip_txn(...)` — §8. The only request/response socket I/O outside bootstrap-init.

### 11.4 Engine — `enip_conn.c`
- `enip_connection_create(attribs) -> *conn` — alloc (rc), init arenas/mutexes, create
  socket once, parse gateway/path into `link`, start the thread.
- `enip_connection_destructor(ptr)` — shutdown, wake, **join**, then free (§7.3).
- `enip_queue_link/unlink/peek_due/next_wait` — §6.2.
- `enip_connection_thread_entry(arg)` — the linear loop (§13).
- `enip_ensure_due_tags_metadata(conn, now)` — bring every due tag to **usable**; may call
  `enip_txn` via `fetch_tag_metadata`; `PLCTAG_ERR_BAD_CONNECTION` if the socket died.
- `enip_connection_build_requests(conn, *out_frame)` — select due tags (`rc_inc` pattern),
  call `encode_chunk` per slot under the §5.9 budgets, pack single or shared `0x0A` (§15),
  set packed tags INFLIGHT with their `transaction_id`. A tag that encodes nothing stays
  REQUEST (retried), never INFLIGHT.
- `enip_recv_and_dispatch(conn)` — receive one frame; for `0x8A` map slots 1:1 to inflight
  tags in queue order; otherwise match by `transaction_id`; call `accept_chunk`; on
  PARTIAL encode+send the next chunk and loop; on terminal write `tag->status`, IDLE,
  unlink, raise completion. All tag access uses `rc_inc`.
- `enip_connection_graceful_close(conn)` — `close_connection` → UnregisterSession →
  `socket_close` (data fd only) → reset session flags → requeue inflight ops as REQUEST.
- `enip_backoff_with_jitter` / `enip_wait_for_work` — wake-interruptible waits (§13.3).

### 11.5 Session bootstrap — `enip_session.c`
- `enip_session_register(conn)` — 0x0065; parse handle/status.
- `enip_session_unregister(conn)` — 0x0066; best-effort.
- `enip_session_get_identity(conn, *identity_out)` — Identity GetAttributeAll via
  `enip_txn`; fills `enip_identity_t` (including `supports_multi_service`).

### 11.6 CIP connection + counts — `enip_cipconn.c`
- `enip_cipconn_forward_open(conn)` — FO_Ex then FO (§5.6); store conn ids and
  `cip_size_o_to_t`/`_t_to_o`. Called by AB/OMRON `open_connection`.
- `enip_cipconn_forward_close(conn)` — §5.7.
- `enip_symbol_query_counts(conn)` — class 0x6B instance 0 attrs 2,3 (§5.8).

### 11.7 Metadata — `enip_metadata.c`
- `enip_root_symbol_cache_init(conn, count)` — presize + allocate the single-block table;
  must actually create `root_symbol_cache` (today's code never does — fix it).
- `enip_metadata_fetch_root_symbols(conn)` — inventory walk (§5.8).
- `enip_metadata_find_root_symbol(conn, name) -> *entry` — lookup with name re-check.
- `enip_metadata_fetch_tag_info(conn, instance_id, *type, *size, *dims)` — phase-2 (§5.8);
  writes into the caller's `meta` (no connection-level cache).

### 11.8 Tag layer — `enip_tag.c`
- `enip_protocol_tag_create(...)` — encode base path + name into the tail, zero `meta`/`op`
  (IDLE), find-or-create connection, `rc_inc` into `tag->conn`. Does **not** queue.
- `enip_tag_read`/`write` — if IDLE: set kind, `op_time`, REQUEST; link; `socket_wake`;
  return PENDING.
- `enip_tag_status` — error if set; PENDING while `op` active or metadata not usable; else
  OK.
- `enip_tag_abort` — under `queue_mutex` unlink + IDLE; raise aborted.
- `enip_tag_get_int_attrib` — expose `elem_size`/`elem_count` from `meta` **only when
  usable** (§2.1).
- `enip_tag_destructor` — under `queue_mutex` unlink; free `tag->data`; `rc_dec(tag->conn)`.

---

## 12–15. Reference detail

### 12. Socket-wake threading model
The connection thread waits **only** on the socket via `socket_wait_event`. The socket is
created once in `enip_connection_create` and lives until `socket_destroy` in the
destructor; `socket_close` shuts only the data fd and keeps the wake channel alive, so the
thread can wait while disconnected. `socket_wake` (from `enip_tag_read/write` and on queue
insert) interrupts the wait. No condition variables.

### 13. Linear connection handler
`conn->state` is set only at visible transitions (CONNECTING, UP, DOWN, ERR_WAIT,
DISCONNECTING) and never selects code. Two wait modes:
- **Idle/backoff/waiting-for-work:** `socket_wait_event(DEFAULT_MASK, timeout)` with the
  timeout from `enip_queue_next_wait`. New work and shutdown land here.
- **Mid send/recv:** wrap each `socket_write_wait`/`enip_recv_frame` in
  `do{…}while(rc==PENDING && !shutdown)` using `link->io`, so a wake resumes the same I/O
  instead of derailing it.

### 14. One strategy vtable, not per-tag
Behavior varies by manufacturer (a connection property), not by tag; data varies by tag.
One `mfg_ops` on the connection, the generic vtable on the tag, per-tag data in
`meta`/`op`. Do not add a per-tag behavior vtable.

### 15. Packed frame sizing and chunk splitting

**Packing is budget-bound, not count-bound.** The packer keeps admitting slots until the
request **or** response budget (§5.9) is exhausted. `ENIP_PKT_MAX_SLOTS` is only a static
array bound, never a protocol limit.

A `0x0A` frame has, on each side, a fixed wrapper + a 2-byte offset entry per slot + the
packed bodies, all **inside** the CIP budget. The wrapper is generic CIP, so the engine
builds it (§9.2). **Every candidate is sized against the request budget AND the response
budget; the tighter governs.**

- Aggregate sizes (CIP-budget terms):
  `request  = MULTI_REQ_FIXED  + Σ(SLOT_OVERHEAD + req_body[i])  ≤ req_budget`
  `response = MULTI_RESP_FIXED + Σ(SLOT_OVERHEAD + resp_body[i]) ≤ resp_budget`
  where `req_budget`/`resp_budget` come from §5.9 (FO sizes minus the connected overhead).
- Per-tag body: `req_body = req_fixed + (write ? data : 0)`,
  `resp_body = resp_fixed + (read ? data : 0)`. The strategy reports `req_fixed`
  (implicit in the bytes it emits) and `resp_fixed` (via `out_resp_fixed`, §9).
- `enip_chunk_split` (shared, manufacturer-neutral):
  1. `req_data = req_avail − req_fixed`; `resp_data = resp_avail − resp_fixed`. If either
     ≤ 0 → does not fit.
  2. `usable = min(req_data, resp_data, remaining)`.
  3. `unit = min(elem_size, 8)` — never split inside an atomic element; aggregates split on
     8-byte boundaries.
  4. `chunk_bytes = (usable / unit) * unit`. If `< unit` → does not fit.
- Packing loop walks due tags oldest-first, admitting slots until a tag does not fit. If
  the first slot cannot fit even one element in an empty frame, fail that tag with
  `PLCTAG_ERR_TOO_LARGE`. Feed the chosen sizes to `enip_packetizer_plan` as a final
  assert before transmit.
- A single-slot transfer skips the `0x0A` wrapper entirely (plain CIP request/response);
  use `enip_packetizer_fits_single` for that decision.

Continuation across chunks is decided by `accept_chunk` (§9.1), never by shared code
reading a CIP status.

---

**End of plan.**

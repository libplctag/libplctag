<!--
  Copyright (C) 2026 by Kyle Hayes - kyle.hayes@gmail.com
  This software is available under the MPL 2.0 or LGPL 2 (or later) license.
-->

# device_sim — combined AB / OMRON PLC simulator: design

`device_sim` replaces the two existing simulators
(`src/tools/ab_server`, thread-per-connection + `slice_s`; and
`src/poc/ab_server_fiber`, fibers + `Bytes`) with a single program that keeps the
best of each:

- **Thread-per-connection** with **linear, top-to-bottom request flow** — the
  blocking clarity of `ab_server_fiber`'s `client_fiber`, expressed with OS
  threads instead of fibers.
- **Per-tag mutex protection** of tag data, as in `ab_server`.
- Built **only** on `src/utils` + the core platform layer — no `src/tools/utils`,
  no `src/poc/utils`:
  - `platform.h` for sockets, threads, mutexes, condition vars.
  - `debug.h` for logging (`pdebug`).
  - `arena.h` for per-request scratch allocation.
  - `bytes.h` for the `Bytes` slice type and `bytes_pack`/`bytes_unpack`.

## Decisions (locked)

1. **Extend the core platform layer** (`src/platform/posix/platform.c` **and**
   `src/platform/windows/platform.c`) with TCP-server (`bind`/`listen`/`accept`)
   **and** UDP (unicast + broadcast) sockets. `device_sim` uses `platform.h`
   sockets for everything; `scan_eip_network` can later re-target the same API.
2. **Fork the protocol code from `ab_server_fiber`** (`eip.c`, `cpf.c`, `cip.c`,
   `pccc.c`) — it is already written against `bytes.h` pack/unpack and the
   `Arena`/`Bytes` dispatch shape. Swap its `fiber_net` I/O for `platform.h`
   threads/sockets; add `ab_server`'s per-tag mutex.
3. **CIP first, PCCC data path later.** Early phases deliver full CIP/Logix +
   OMRON read/write/listing. PCCC devices (PLC/5, SLC, MicroLogix) answer
   **List Identity** and the **CIP Identity object** from the start; their full
   PCCC read/write data path is a later phase (port from `ab_server/pccc.c`).
4. **New platform socket calls follow `platform.h` conventions:** `int` return
   of `PLCTAG_STATUS_*`, opaque `sock_p`, readiness via `socket_wait_event()`.
5. **All new code follows** `src/external_docs/coding_guidelines.md`: `int32_t`
   for all error-returning function return types, `extern` on public definitions,
   `static` on file-internal definitions, `/* */` comments only, `#pragma once`
   as the first line of every header, and `mem_set`/`mem_alloc`/`mem_free`
   instead of raw `memset`/`malloc`/`free`.

## Hard requirement: clean shutdown through the wake pipe

Every `sock_p` created by `socket_create()` already owns a **wake pipe**
(`wake_read_fd`/`wake_write_fd` in `struct sock_t`), drained inside
`socket_wait_event()` and fired by `socket_wake()`. `device_sim` must **never**
block in a bare `accept`/`recv`/`read`. Every blocking point waits via
`socket_wait_event(sock, mask | SOCK_EVENT_WAKE_UP, timeout_ms)`. On `SIGINT`
the signal handler sets a global `terminate` flag and calls `socket_wake()` on
the listener socket, the UDP socket, and every live client socket, so all
threads unblock, observe the flag, and exit cleanly. This is the single most
important behavioral constraint and it drives the threading model below.

---

## 1. File layout

The library lives in `src/libdevsim`, a top-level build product whose directory
structure parallels `src/libplctag` (`lib/` = public API + shared context,
`protocols/` = the wire implementation).  The CLI is a thin separate tool.

```
src/libdevsim/                  the library (static; links plctag_static)
  DESIGN.md            this document
  CMakeLists.txt       builds the devsim static lib from lib/ + protocols/ + src/utils
  lib/
    device_sim.h         PUBLIC, FFI-clean API (the only installed header)
    device_sim.c         device_sim_t lifecycle + tag/identity/registry API impl
    device.h             enip_plc_type_t (common/plc_type.h), identity_t, tag_def_t, device_t (shared context)
  protocols/             === vendor-NEUTRAL core (CIP/EIP common to every PLC) ===
    server.c/.h            TCP listener thread + per-connection thread (linear flow)
    discovery.c/.h        UDP List Identity responder (+ ListServices/ListInterfaces)
    identity.c/.h         per-model identity table + CIP Identity object encoder
    eip.c/.h              EIP encapsulation dispatch        (fork of fiber eip.c)
    cpf.c/.h              CPF / connected + unconnected      (fork of fiber cpf.c)
    cip.c/.h              core CIP services: FO/FC, read/write 0x4C/0x4D, multi 0x0A,
                          fragmented 0x52/0x53, and the generic class/instance registry
    pccc.c/.h             PCCC encapsulation framing (data path is a dialect; see below)
  protocols/dialects/    === MANUFACTURER-SPECIFIC, one module per vendor family ===
    ab_listing.c/.h       Rockwell tag + UDT/template listing (class 0x6B/0x6C)
    omron_listing.c/.h    OMRON variable + structure enumeration (its own classes/services)
    pccc_data.c/.h        AB legacy data path (PLC/5, SLC, MicroLogix)

src/tools/device_sim/           the CLI tool (links the devsim library)
  CMakeLists.txt       builds the device_sim executable
  main.c               CLI flag table, identity/tag table build, signal handler, run loop
  args.c/.h            table-driven CLI argument parser (copy of fiber args.c/.h; see §1.1)
```

**Core vs. dialect boundary (load-bearing).** `protocols/` is vendor-neutral:
encapsulation, connection management, the Identity object, and the *generic*
CIP read/write/multi services that every Logix-class and OMRON PLC answers
identically.  Anything where vendors diverge — above all **tag enumeration and
UDT/structure introspection** — lives under `protocols/dialects/`, one module
per manufacturer family, plugged in through the generic CIP object registry
(§7.6) rather than by editing core `cip.c`.  AB and OMRON do *not* share a tag-
or UDT-listing mechanism (different CIP classes, services, and reply encodings),
so they are separate dialect modules, not flags inside one code path.  See §8.

### 1.1 args.c/.h — table-driven argument parser

Copy `src/poc/ab_server_fiber/args.c` and `args.h` verbatim, then apply these
mechanical changes (do not link or include the fiber files directly):

- Remove `#include "err.h"` and `#include "log.h"`.
- Add `#include "libplctag.h"` (for `PLCTAG_STATUS_*`) and
  `#include <path/to/src/utils/debug.h>` (for `pdebug`).
- Change `util_err_t` → `int` everywhere (return types, `args_result_t.error`,
  `args_get_error`).
- Replace error-code constants: `UTIL_OK` → `PLCTAG_STATUS_OK`;
  `UTIL_EINVAL` / `UTIL_ERESOURCE` / `UTIL_EARGS_*` → `PLCTAG_ERR_BAD_PARAM`
  (using a single generic bad-param code is fine; the detail string in
  `args_result_t.error_detail` already carries the precise reason).
- Replace `pdlog(LOG_MODULE_ARGS, LOG_LEVEL_ERROR, ...)` →
  `pdebug(DEBUG_ERROR, "args", ...)` and `LOG_LEVEL_DETAIL`/`LOG_LEVEL_INFO` →
  `DEBUG_DETAIL`/`DEBUG_INFO`.

Everything else — all types, constants (`ARGS_TYPE_*`, `ARGS_REQUIRED`,
`ARGS_ONCE`, `ARGS_MULTIPLE`, `ARGS_MAX_FLAGS`, `ARGS_MAX_REPETITIONS`),
and all accessor functions — stays identical.

Flag table for `device_sim` (defined in `main.c`, passed to `args_parse`):

| flag | type | req | repeat | default | description |
|---|---|---|---|---|---|
| `--plc` | STRING | required | once | — | `controllogix\|micro800\|omron\|plc5\|slc\|micrologix` |
| `--model` | STRING | optional | once | `L81E` | sub-model: `L81E\|L61\|L55` (ControlLogix only) |
| `--port` | INT | optional | once | `44818` | TCP + UDP listen port |
| `--delay-ms` | INT | optional | once | `0` | per-response artificial delay |
| `--debug` | INT | optional | once | `0` | debug verbosity level |
| `--tag` | STRING | optional | **multiple** | — | `name:type:count[:d1[:d2[:d3]]]` |

`--tag` strings are parsed into `tag_def_t[]` in `main.c` after `args_parse`
returns, not inside `args.c`. Type tokens: `DINT`, `REAL`, `BOOL`, `INT`,
`SINT`, `LINT`, `USINT`, `UINT`, `UDINT`, `ULINT`, `STRING`, `BYTE`, `WORD`,
`DWORD`, `LWORD`.

`device.h`'s `tag_def_t` keeps `ab_server/plc.h`'s shape (name, type, elem_size,
elem_count, dimensions[3], `mutex_p data_mutex`) but the surrounding protocol
code uses `Bytes`, not `slice_s`.

---

## 2. Platform layer extensions (the core change)

`struct sock_t` today is `{ int fd; int wake_read_fd; int wake_write_fd; int port; }`.
Add a socket kind and keep the wake channel for **every** socket so
`socket_wait_event()` works uniformly:

```c
typedef enum { SOCK_KIND_TCP_CLIENT, SOCK_KIND_TCP_LISTEN, SOCK_KIND_UDP } sock_kind_t;
/* struct sock_t gains: sock_kind_t kind; */
```

New API in `platform.h` (mirrored in posix + windows `platform.c`), all returning
`PLCTAG_STATUS_*`:

```c
/* TCP server */
extern int32_t socket_listen_tcp(sock_p s, const char *bind_addr, uint16_t port, int32_t backlog);
extern int32_t socket_accept(sock_p listen_s, sock_p *client_s, int32_t timeout_ms);
    /* internally: socket_wait_event(listen_s, SOCK_EVENT_CAN_READ|SOCK_EVENT_WAKE_UP,
       timeout_ms) then accept(); allocates *client_s with its own wake channel.
       Returns PLCTAG_ERR_TIMEOUT on timeout, PLCTAG_ERR_ABORT on wake. */

/* UDP (unicast + broadcast) */
extern int32_t socket_open_udp(sock_p s, const char *bind_addr, uint16_t port, bool enable_broadcast);
extern int32_t socket_send_to(sock_p s, uint8_t *buf, int32_t size, const char *host, uint16_t port);
extern int32_t socket_recv_from(sock_p s, uint8_t *buf, int32_t size, char *src_host,
                                int32_t src_host_len, uint16_t *src_port, int32_t timeout_ms);
    /* recv_from waits via socket_wait_event(... | SOCK_EVENT_WAKE_UP ...) first. */
```

Implementation notes / where to crib from:
- **Wake channel + `socket_wait_event`** already exist and work for any fd — reuse
  `sock_create_event_wakeup_channel()` unchanged for listen/UDP sockets
  (posix `platform.c:1128`, `:2104`).
- **`socket_listen_tcp`**: `socket()`/`SO_REUSEADDR`/`bind`/`listen`. Borrow the
  bind/listen/reuseaddr sequence from `ab_server/socket.c`
  (`socket_open_tcp_server`) and the UDP/broadcast setsockopt sequence from
  `poc/utils/socket.c` + `scan_eip_network.c:setup_broadcast_socket`
  (`SO_BROADCAST`, `SO_REUSEADDR`, non-blocking).
- **`socket_accept`**: model the readiness-then-accept on
  `socket_connect_tcp_check()` (posix `platform.c:1352`) which already drives the
  poll/select around the same fd set; the new call waits for `CAN_READ|WAKE_UP`.
- **Windows parity**: replicate in `src/platform/windows/platform.c`
  (`socket_*` block begins ~`:1140`). Same API, `WSA` calls, `closesocket`.
- Non-blocking fds throughout, exactly as the existing client socket code does,
  so `socket_wait_event` is the only place that blocks.

This keeps `^C` handling identical across TCP-accept, UDP-recv, and
per-connection read: one `socket_wake()` per live socket unblocks them all.

---

## 3. Threading model (thread-per-connection, linear flow, wake-driven)

Three thread roles, all using `platform.h` `thread_create` and the wake pipe:

### 3.1 Main thread
- Parse args, build the identity + tag tables (`device_t`), install the `SIGINT`
  handler (sets `volatile sig_atomic_t terminate = 1`).
- Create + start the TCP listener thread and the UDP discovery thread.
- Keep a small **live-socket registry** (mutex-protected vector of `sock_p`):
  listener, UDP socket, and each accepted client socket register/unregister
  themselves. The signal handler walks the registry and calls `socket_wake()` on
  every entry. (`ab_server` used a shared `done` flag and accept timeouts; the
  wake pipe is cleaner and is the required mechanism here.)
- `thread_join` the listener + discovery threads on shutdown.

### 3.2 Listener thread (`server.c`)
Mirrors `ab_server/tcp_server.c:tcp_server_start` and
`ab_server_fiber.c:listener_fiber`, but wake-driven:

```c
socket_listen_tcp(listen_sock, bind_ip, port, 128);
register_socket(listen_sock);
while(!terminate) {
    sock_p client = NULL;
    int rc = socket_accept(listen_sock, &client, ACCEPT_TIMEOUT_MS);   /* waits on CAN_READ|WAKE_UP */
    if(rc == PLCTAG_ERR_TIMEOUT) continue;
    if(rc == PLCTAG_ERR_ABORT)   break;      /* woken for shutdown */
    if(rc != PLCTAG_STATUS_OK)   break;

    conn_ctx_t *c = calloc(1, sizeof *c);    /* {device, client, registry slot} */
    c->device = device;  c->sock = client;
    register_socket(client);
    thread_create(&c->thread, conn_handler, CONN_STACK, c);  /* detaches itself */
}
```

The per-connection context holds a pointer to the shared `device_t` (tags +
identity) — **not** a deep copy. `ab_server` deep-copied the whole `plc_s` context
per connection (`tcp_server.c:117-123`); we keep one shared `device_t` and rely
on the per-tag `data_mutex` for the only mutable shared state (tag data). Session
state (session handle, connection ids, negotiated sizes) is **per-thread, on the
stack**, exactly like `ab_server_fiber`'s `eip_session_t sess = {0}`.

### 3.3 Per-connection thread (`server.c:conn_handler`)
This is the linear flow lifted from `ab_server_fiber.c:client_fiber`
(`:421-534`), with blocking replaced by wait-then-read and an arena reset per
request:

```c
THREAD_FUNC(conn_handler) {
    conn_ctx_t *c = arg;  thread_detach();
    Arena arena;  arena_init(&arena, CLIENT_ARENA_SIZE);
    eip_session_t sess = {0};
    eip_session_set_unconnected_sizes(&sess, c->device->server_to_client_max_packet);

    while(!terminate) {
        arena_reset(&arena);

        /* Phase 1: read 24-byte EIP header (wait on CAN_READ|WAKE_UP, then read) */
        Bytes hdr = bytes_alloc(&arena, EIP_HEADER_SIZE);
        if(recv_exact(c->sock, hdr) != OK) break;          /* closed / woken / error */

        /* Phase 2: payload_len from header; guard vs negotiated max and arena */
        uint16_t payload_len; bytes_unpack(hdr, BYTES_LE, BYTES_SKIP(2), &payload_len);
        ... guards (copy fiber client_fiber:468-482) ...

        /* Phase 3: read payload */
        Bytes payload = payload_len ? bytes_alloc(&arena, payload_len) : bytes_null();
        if(payload_len && recv_exact(c->sock, payload) != OK) break;

        /* Phase 4: dispatch — pure function of (arena, hdr, payload, sess, device) */
        Bytes resp = eip_dispatch(&arena, hdr, payload, &sess, c->device);

        if(c->device->response_delay_ms) sleep_ms(c->device->response_delay_ms);
        if(bytes_is_null(resp)) break;                     /* UnregisterSession / fatal */

        /* Phase 5: send response */
        if(send_all(c->sock, resp) != OK) break;
    }
    unregister_socket(c->sock);
    socket_close(c->sock);  socket_destroy(&c->sock);
    arena_free(&arena);  free(c);
    THREAD_RETURN(0);
}
```

`recv_exact()` / `send_all()` are thin helpers that loop
`socket_wait_event(sock, SOCK_EVENT_CAN_READ|SOCK_EVENT_WAKE_UP, t)` →
`socket_read(...)` until the requested length is satisfied, returning early on
wake (shutdown), close, or error. They are the **only** new blocking primitives
the protocol code touches, keeping `eip_dispatch` and below pure
`Bytes`-in/`Bytes`-out and trivially testable.

### 3.4 Tag data locking
Identical principle to `ab_server/plc.h`: only `tag_def_t.data` is shared-mutable;
the tag list, names, and types are built once on the main thread and read-only
thereafter. `cip.c handle_read`/`handle_write` (and later `pccc.c`) wrap their
access to a tag's bytes in `critical_block(tag->data_mutex)` (the `platform.h`
macro). Per-tag, not global, so unrelated tags don't contend.

---

## 4. Protocol layer (fork of `ab_server_fiber`)

Copy `eip.c/.h`, `cpf.c/.h`, `cip.c/.h` from `src/poc/ab_server_fiber` almost
verbatim — they already speak `Arena`/`Bytes` and have the dispatch shape we want:

- `eip_dispatch(Arena*, Bytes hdr, Bytes payload, eip_session_t*, device_t*)`
  (fiber `eip.c:101`) — RegisterSession / UnregisterSession / SendRRData /
  SendUnitData. **Add** List Services (`0x0004`) and List Identity over TCP
  (`0x0063`) cases here (§6).
- `cpf_handle_unconnected` / `cpf_handle_connected` (fiber `cpf.c`) — CPF framing.
- `cip_dispatch_unconnected` / `cip_dispatch_connected` (fiber `cip.c:126`,`:192`)
  — Forward Open/Close, Read/Write (`0x4C`/`0x4D`), Read/Write Fragmented
  (`0x52`/`0x53`), Multiple Service (`0x0A`), embedded Unconnected Send.

Changes on fork:
- Rename `plc_config_t` → `device_t`; it already carries the tag list and
  negotiated sizes.
- Replace any `fiber_net_*`/`util_err_t` leakage with the §3.3 helpers (there
  should be none below `eip_dispatch` — the fiber kept I/O in `client_fiber`).
- Logging: the fiber uses `pdlog`/`LOG_MODULE_*`; switch to `pdebug` +
  `debug.h` modules. **Keep every existing trace** — do not drop debug output.

`ab_server`'s `cip.c`/`pccc.c` remain the reference for protocol *details* the
fiber lacks (notably the full PCCC path and multi-dim offset math at
`ab_server/cip.c:1196` `calculate_request_start_and_end_offsets`).

---

## 5. Identity model (`identity.c/.h`)

One `identity_t` per simulated model, selected by `--plc=` (and field-overridable
by CLI). Values below are decoded directly from the `get_identity` captures in the
task. The CIP **Identity object** (GetAttributesAll, class `0x01`, instance `1`)
returns exactly these fields in this order, little-endian, which reproduces the
captured "Raw Data" byte-for-byte:

```c
typedef struct {
    uint16_t vendor_id;       /* 0x0001 Rockwell for all AB models             */
    uint16_t device_type;     /* 0x000E comms adapter / 0x000C for MicroLogix  */
    uint16_t product_code;
    uint8_t  revision_major;
    uint8_t  revision_minor;
    uint16_t status;
    uint32_t serial_number;
    const char *product_name; /* length-prefixed (u8) on the wire              */
} identity_t;
```

| model (`--plc`)        | vendor | dev_type | prod_code | rev   | status | serial      | product_name                     |
|------------------------|--------|----------|-----------|-------|--------|-------------|----------------------------------|
| ControlLogix (L81E)    | 0x0001 | 0x000E   | 0x00A4    | 31.11 | 0x3060 | 0x00F5D982  | `1756-L81E/B`                    |
| ControlLogix (L61)     | 0x0001 | 0x000E   | 0x0036    | 20.12 | 0x3160 | 0x007409F4  | `1756-L61/B LOGIX5561`           |
| ControlLogix (L55)     | 0x0001 | 0x000E   | 0x0033    | 16.22 | 0x3160 | 0x001A049A  | `1756-L55/A 1756-M22/A LOGIX5555`|
| PLC/5                  | 0x0001 | 0x000E   | 0x0012    | 3.11  | 0x0060 | 0xBC033CDF  | `PLC-5/30 C/K - 1785-ENET 2.17 ` |
| MicroLogix             | 0x0001 | 0x000C   | 0x00B9    | 2.12  | 0x0064 | 0x9CA054FF  | `1763-L16BWA B/12.00`            |
| Micro800               | 0x0001 | 0x000E   | (model)   | —     | —      | (gen/CLI)   | e.g. `2080-LC50-48QWB`           |
| OMRON NJ/NX            | 0x002F | 0x000C   | (model)   | —     | —      | (gen/CLI)   | e.g. `NX1P2-9024DT`              |

The L-model variants are selected by an optional sub-flag (e.g.
`--plc=ControlLogix --model=L61`), defaulting to L81E. Micro800/OMRON identities
are reasonable defaults, fully CLI-overridable.

Encoder (`identity.c`):
```c
extern Bytes identity_encode_object(Arena *a, const identity_t *id);   /* §5, 26–46 bytes */
extern Bytes identity_encode_listid_item(Arena *a, const identity_t *id,
                                         uint32_t my_ipv4, uint16_t my_port);  /* §6 CPF item */
```
`identity_encode_object` builds the GetAttributesAll body; the CIP Identity
service path (class `0x01`/inst `1`, services GetAttributesAll `0x01` and
GetAttributeSingle `0x0E`) is dispatched from `cip.c` for the `@identity`-style
reads the captures show.

---

## 6. Discovery: List Identity / List Services (`discovery.c`)

A dedicated UDP thread bound to `0.0.0.0:44818` with broadcast enabled
(`socket_open_udp(..., enable_broadcast=true)`), wake-driven like every other
loop:

```c
while(!terminate) {
    int rc = socket_recv_from(udp, buf, sizeof buf, src_host, ..., &src_port, RECV_TIMEOUT_MS);
    if(rc == PLCTAG_ERR_TIMEOUT) continue;
    if(rc == PLCTAG_ERR_ABORT)   break;       /* woken for shutdown */
    /* parse 24-byte EIP header; on cmd: */
    switch(cmd) {
        case 0x0063: resp = build_list_identity_reply(&arena, device, my_ipv4);  break; /* CPF item 0x000C */
        case 0x0004: resp = build_list_services_reply(&arena);                     break; /* "Communications" */
        case 0x0064: resp = build_list_interfaces_reply(&arena);                   break; /* empty item list */
    }
    socket_send_to(udp, resp.data, resp.len, src_host, src_port);
}
```

- **List Identity reply** = EIP header (echo command/context, length filled in) +
  CPF item count `1` + item type `0x000C` + item length + the
  `identity_encode_listid_item` body (protocol_version, sockaddr-in BE
  family/port/ip, 8 reserved, then the same identity fields as §5, then a 1-byte
  state). The exact item layout is the inverse of
  `scan_eip_network.c:parse_list_identity_item` (`:414-477`) — use that parser as
  the authoritative field order/endianness reference.
- The **same** `0x0063` handler is wired into `eip_dispatch` (§4) so List Identity
  also works over an established TCP session, not just UDP broadcast.
- All four AB classes plus OMRON answer correctly because the reply is just the
  model's `identity_t` from §5; nothing here is model-special beyond table lookup.

`scan_eip_network` (`src/poc/scan_eip_network`) becomes the natural integration
test: once `device_sim` is running, a broadcast scan must list it.

---

## 7. Library API and simulation callbacks

The core of `device_sim` is split into a reusable static library so other
programs can embed simulated devices. The `device_sim` executable becomes a thin
CLI (`main.c` + `args.c`) layered on the same public API; **anything reachable
from the command line is reachable from the API**, because `args.c` is rewritten
to call it.

### 7.1 Library structure

- New static library target `device_sim_core` (CMake) built from
  `device.c cip.c cpf.c eip.c identity.c pccc.c discovery.c server.c` plus
  `arena.c`/`bytes.c`. Public header `device_sim.h` (opaque `device_sim_t *`).
- `device_sim` executable = `main.c` + `args.c` linked against `device_sim_core`.
- `device_t` becomes the internal struct behind the opaque handle. It gains: an
  atomic `terminate` flag, the connect/disconnect callback + device `user_data`,
  an embedded `identity_t` (copied at create from `identity_for_plc_type`), the
  listener/discovery thread handles, the socket registry, and the CIP-object
  registry (§7.6).

### 7.2 De-globalize shutdown

The process-global `volatile sig_atomic_t g_terminate` cannot be shared by two
sims in one process. Replace it with a per-device atomic flag checked by
`server.c`/`discovery.c`/`recv_exact`/`send_all` (all already hold a
`device_t *`). The CLI keeps the `SIGINT`/`SIGTERM` handler; the handler calls
`device_sim_stop(dev)`, which sets the flag, `registry_wake_all`s, and joins the
threads. `device_sim_stop` is idempotent and safe to trigger from a handler.

### 7.3 Lifecycle API

```c
typedef struct device_sim_s device_sim_t;   /* opaque */

typedef struct {
    enip_plc_type_t plc_type;
    uint16_t    port;
    const char *bind_addr;
    int32_t     response_delay_ms;
    uint32_t    client_to_server_max_packet;
    uint32_t    server_to_client_max_packet;
    void       *user_data;                   /* passed to the connect/disconnect cb */
    device_sim_conn_cb conn_cb;
} device_sim_config_t;

extern device_sim_t *device_sim_create(const device_sim_config_t *cfg);
extern int32_t       device_sim_start(device_sim_t *dev);   /* spawn listener + discovery threads */
extern int32_t       device_sim_stop(device_sim_t *dev);    /* idempotent */
extern void          device_sim_destroy(device_sim_t *dev);
```

### 7.4 Tags and read/write callbacks

`tag_def_t` gains `read_cb`, `write_cb`, and a per-tag `user_data`; tag creation
moves out of `args.c` into the library:

```c
typedef void (*device_sim_tag_cb)(device_sim_t *dev, const char *name,
                                  void *data, size_t data_len, void *user_data);

extern int32_t device_sim_add_tag(device_sim_t *dev, const char *name, tag_type_t type,
                                  const size_t *dims, size_t num_dims,
                                  device_sim_tag_cb read_cb, device_sim_tag_cb write_cb,
                                  void *user_data);
```

Hook points in `cip.c` (and the PCCC paths):
- `handle_read` fires `read_cb` **before** serializing the response, letting the
  callback refresh the value the client is about to read.
- `handle_write` fires `write_cb` **after** storing the new bytes, letting the
  program react to what was written.

### 7.5 Connect/disconnect callback and direct access

```c
typedef void (*device_sim_conn_cb)(device_sim_t *dev, const char *client_ip,
                                   bool connected, void *user_data);
```
Fired at TCP granularity (matching the `@connection` tag semantics): at the top
of `conn_handler` (connect) and at its cleanup (disconnect), using the socket
peer address and the device `user_data`.

Direct, self-locking access — the caller never takes a lock:
```c
extern int32_t device_sim_tag_get(device_sim_t *dev, const char *name, size_t offset, void *dst, size_t len);
extern int32_t device_sim_tag_set(device_sim_t *dev, const char *name, size_t offset, const void *src, size_t len);
extern int32_t device_sim_get_identity(device_sim_t *dev, identity_t *out);
extern int32_t device_sim_set_identity(device_sim_t *dev, const identity_t *id);
```
Identity gets direct get/set (the status word is the one field a real PLC changes
dynamically); it deliberately has **no** read callback — identity is read rarely
and a callback there buys little.

Two invariants keep this thread-safe without a recursive mutex or caller locking:
1. The protocol handler invokes a tag's callback **without holding that tag's
   `data_mutex`**, passing a mutable buffer (read: a snapshot to optionally
   modify; write: the just-stored bytes). Inside a tag's own callback, mutate the
   provided buffer directly; use `device_sim_tag_set/get` for *other* tags or
   from outside. This is deadlock-free.
2. Tags may only be added **before** `device_sim_start`, so the tag list is
   immutable while threads run — name lookups need no list lock; only each tag's
   `data_mutex` matters.

### 7.6 Generic CIP object registry

Beyond the built-in Identity object and the symbolic-tag path, a program can
register a handler for an arbitrary `(class_id, instance_id)`. CIP logical
segments encode 8/16/32-bit class (`0x20`/`0x21`/`0x22`) and instance
(`0x24`/`0x25`/`0x26`) forms, so **IDs are 32-bit**:

```c
typedef int32_t (*device_sim_cip_cb)(device_sim_t *dev, uint8_t service,
                                     const uint8_t *path, size_t path_len,
                                     const uint8_t *req, size_t req_len,
                                     uint8_t *resp, size_t resp_cap, size_t *resp_len,
                                     void *user_data);   /* return NOT_HANDLED to fall through */

extern int32_t device_sim_add_cip_object(device_sim_t *dev, uint32_t class_id, uint32_t instance_id,
                                         device_sim_cip_cb cb, void *user_data);
```

- The registry is consulted in `cip_dispatch_*` before the built-in handlers (or
  as the `default:` case). Supported services are **implicit**: the callback
  handles what it knows and returns `NOT_HANDLED` for the rest, which the
  dispatcher turns into the standard CIP "service unsupported" error.
- The existing 8-bit-only `parse_class_instance_path` stays for the Identity path
  (always class 1 / instance 1); a sibling parser decoding all three widths into
  `uint32_t` class/instance/attribute feeds the registry lookup.

This is the minimal raw hook — no typed class/instance/attribute framework — and
it is also the mechanism for the remaining protocol phases (§8 and below).

---

## 8. Manufacturer-specific dialects: tag and UDT listing

Tag enumeration and UDT/structure introspection are **not** core CIP — each
manufacturer family does it with different classes, services, and reply
encodings. They therefore live as separate modules under `protocols/dialects/`,
each registering handlers through the generic CIP object registry (§7.6). Core
`cip.c` is never edited to add a dialect: it routes the dialect's classes to the
registered handler and otherwise returns "service not supported". A model only
advertises a listing dialect if its `plc_type` selects it; everything else gets
"service not supported".

The shared infrastructure that core provides to every dialect:

- the generic class/instance registry (§7.6) as the plug-in point,
- the connection size and the general-status `0x06` "partial transfer"
  continuation convention, so any dialect can page a reply that overflows.

### 8.1 Rockwell / ControlLogix-class (`ab_listing.c`)

- **Symbol listing** — class `0x6B`, service `0x55` (GetInstanceAttributeList),
  attributes 1 (name) + 2 (type). Reply is the paged
  `instance_id(u32) | name_len(u16) | name | type(u16)` record stream; page with
  general status `0x06` when the response would exceed the connection size.
- **Template / UDT** — class `0x6C`: GetAttributeList (`0x03`) for
  size/member-count/handle, then Read Template (`0x4C`) for the member-info array
  + name blob, itself fragmented.

Reference: server side of the client format in `ENIP-SESSION-DESIGN.md` /
`ROCKWELL-SPECIFIC-DESIGN.md §5`; legacy encoder in `attic/`. UDT-bearing tags in
the table need a small member-layout descriptor on `tag_def_t` to synthesize
template replies.

### 8.2 OMRON (`omron_listing.c`)

OMRON NJ/NX controllers expose variables and data-type (structure) details
through a **different** path than Rockwell — its own enumeration classes/services
and reply layout, plus the Simple Data Segment (`0x80`) addressing used on reads.
It shares none of `0x6B`/`0x6C`'s record format, so it is a distinct module, not a
branch inside `ab_listing.c`. Reference: `OMRON-SPECIFIC-DESIGN.md`,
`~/Projects/aphytcomm`.

### 8.3 AB legacy PCCC (`pccc_data.c`)

PLC/5, SLC, and MicroLogix have no symbolic tag list at all — addressing is by
data-table file/element. The PCCC data path is therefore its own dialect module;
`protocols/pccc.c` keeps only the vendor-neutral encapsulation framing.

---

## 9. Build phases

Each phase is independently runnable and testable against the real `libplctag`
client and the L81E test PLC string
(`gateway=10.206.1.40:44818, path=1,4, protocol=ab-eip`).

| Phase | Deliverable | Primary sources to copy/adapt |
|-------|-------------|-------------------------------|
| **0** | `platform.h` TCP-server + UDP API, posix + windows, wake-pipe integrated; unit-smoke with a throwaway echo. | `platform.c` (`socket_connect_tcp_check`, wake channel); bind/listen from `ab_server/socket.c`; UDP/broadcast from `poc/utils/socket.c` + `scan_eip_network.c`. |
| **1** | Skeleton: main, signal handler + live-socket registry, listener thread, per-connection linear loop, arena/session lifecycle. Echoes nothing yet but accepts + clean ^C. | `ab_server_fiber.c` (`listener_fiber`, `client_fiber`); `ab_server/tcp_server.c` (thread-per-conn, `thread_detach`). |
| **2** | EIP/CPF/CIP fork wired in: RegisterSession, Forward Open/Close, CIP Read/Write `0x4C`/`0x4D`, Multiple Service `0x0A`, fragmented `0x52`/`0x53`. ControlLogix read/write works end-to-end. | fiber `eip.c`/`cpf.c`/`cip.c` (near-verbatim); `ab_server/cip.c` for detail gaps + multi-dim offsets (`:1196`). |
| **3** | Per-tag `data_mutex` protection around tag data access. | `ab_server/plc.h` + `ab_server/cip.c` locking sites; `critical_block` from `platform.h`. |
| **4** | Identity model + CIP Identity object (`@identity` reads reproduce the captures byte-for-byte). | new `identity.c`; field order from `scan_eip_network.c:parse_list_identity_item`. |
| **5** | UDP discovery thread: List Identity (broadcast + TCP), List Services, List Interfaces. `scan_eip_network` finds the sim. | new `discovery.c`; `scan_eip_network.c` (inverse of its parser); new `socket_open_udp`/`recv_from`/`send_to`. |
| **6** | Library refactor: opaque `device_sim_t`, lifecycle API (`create`/`start`/`stop`/`destroy`), de-globalized per-device `terminate`, identity moved into `device_t`, `args.c` rewritten onto the API. Existing CLI behavior unchanged. | §7.1–7.3; current `main.c`/`args.c`/`device.h`. |
| **7** | Tag read/write callbacks + self-locking direct access (`device_sim_tag_get/set`, `device_sim_get/set_identity`). | §7.4–7.5; `cip.c handle_read`/`handle_write`. |
| **8** | Client connect/disconnect callback (TCP granularity, client IP + device `user_data`). | §7.5; `server.c conn_handler`; socket peer-address accessor. |
| **9** | Generic CIP object registry (32-bit class/instance, `NOT_HANDLED` fall-through). | §7.6; `cip.c` dispatch; new wide class/instance path parser. |
| **10** | **Dialect:** Rockwell tag + UDT/template listing — `protocols/dialects/ab_listing.c`, registered via §7.6 (`0x6B`/`0x6C`). | `ROCKWELL-SPECIFIC-DESIGN.md §5`; `attic/` legacy encoder. |
| **11** | **Dialect:** OMRON variable/structure enumeration + Simple Data Segment `0x80` — `protocols/dialects/omron_listing.c`. | `OMRON-SPECIFIC-DESIGN.md`; `~/Projects/aphytcomm`. |
| **12** | **Dialect:** AB legacy PCCC data path (PLC/5, SLC, MicroLogix) — `protocols/dialects/pccc_data.c`. | `ab_server/pccc.c` (full), `ab_server_fiber/pccc.c` (Bytes-based partial). |

Phases 0–4 reproduce and clean up current `ab_server` functionality; 5 adds
discovery; 6–9 turn the core into an embeddable library with simulation
callbacks. Phases 10–12 are **manufacturer-specific dialect modules** (§8) — each
self-contained under `protocols/dialects/` and wired in through the generic CIP
registry, so vendor differences never leak into the vendor-neutral core.

### 9.1 Status (current)

| Phase | State |
|-------|-------|
| 0–9   | **Done.** Read/write (incl. tags larger than the comm buffer), Identity, UDP+TCP discovery, library refactor, callbacks, generic CIP registry — all passing the `run_device_sim_tests.sh` suite (11/11). |
| 10    | **Done.** Rockwell dialect: tag listing (class `0x6B`, service `0x55`, paged). Class `0x6C` registered; returns "unsupported" pending UDT template support. |
| 11    | Pending — **OMRON dialect:** variable/structure enumeration + SDS `0x80`. |
| 12    | Pending — **AB PCCC dialect:** PLC/5, SLC, MicroLogix data path. |

The three remaining phases are manufacturer-specific dialect modules
(`protocols/dialects/`), each plugged into the vendor-neutral core through the
generic CIP object registry — AB and OMRON list tags / expose UDT details by
entirely different mechanisms, so neither shares the other's code path.

Post-Phase-9 hardening (not in the original table, **done**):

- `libdevsim` is a top-level static build product parallel to `libplctag`, with a
  thin `device_sim` CLI on top; `src/poc/devsim_with_plctag` proves one executable
  can link both APIs.
- Public header (`device_sim.h`) made FFI-clean: fixed-width types only
  (`uint32_t`, no `size_t`), no structs passed by value/pointer — scalar
  `create()` args plus setters instead of a config struct.
- List Identity reply IP is derived by the library per request from the request's
  arrival path (`socket_local_ipv4` / `socket_local_ipv4_to_peer`, POSIX +
  Windows), so the embedded socket address is always reachable by the querying
  client; the caller-facing `set_local_ipv4` setter was removed.

---

## 11. Future: folding the simulator into the libplctag tag API

Sections 1–10 describe `libdevsim` as a standalone library with its own
`device_sim_*` object API. This section is the architecture-of-record for the
*next* step: exposing simulation through the existing `plc_tag_*` API so users
learn one API, and the simulator reuses the client's tag-object machinery
instead of duplicating it.

### 11.1 Why merge (and when not to)

The standalone `device_sim_*` API already works — a real libplctag client talks
to it over localhost. So the merge is justified **only** by two payoffs:

1. **One mental model** — no second API surface to learn.
2. **Code deletion in the sim** — reuse the client id table, refcount, per-tag
   mutex, `plc_tag_status`, and `plc_tag_register_callback` instead of the
   bespoke `device_sim_tag_cb` / `conn_cb` / id management.

Hard line: **server concerns never enter the client read/write hot path.** Keep
the two apart by *dispatch*, not by `if(is_sim)` branches in `ab_tag_read`. If a
step doesn't deliver payoff 1 or 2, skip it.

### 11.2 Three pillars

**1 — One tag-definition vocabulary (attribute string for both sides).**
A simulated tag is *declared* with the same grammar a client uses to *address*
one (`name=Foo&elem_type=DINT&elem_count=10`). One parser; the listing dialects
(§8) derive symbol metadata from the same `tag_def_t`. The positional
`device_sim_add_tag(type, dims, num_dims)` becomes an internal helper.

**2 — Server role selected by dispatch, not a new object type.**
Add one attribute `role=client` (default) `| server`, and key the
`tag_type_map` (`lib/init.c`) on `(protocol, role)` instead of `protocol`:

```
{ .protocol="ab-eip", .role="server", .tag_constructor = ab_sim_create }
```

`find_tag_create_func` already does a first-match table scan — this is a column,
not a rewrite. The returned handle is a real tag-table entry, so
create/destroy/id/refcount/`plc_tag_status` are reused verbatim;
`plc_tag_destroy`'s per-type destructor is where threads stop and tags free. New
PLC type = one row + one constructor, no dispatcher edits. AB and OMRON share
`protocol=ab-eip` and diverge through the §7.6 CIP registry; Modbus is its own
row.

**3 — Everything is a tag; the server is internal (laziest ownership).**
No public "device handle" object. A `role=server` create is keyed by
`bind_addr:port` in an internal registry: the first one finds-or-creates the
backing server (threads, discovery); later server-role creates register their
tag def with it. Each simulated tag is a normal `plc_tag` whose backing store is
the sim's memory, so `plc_tag_get_int32(sim_tag,…)` reads that store — the
`device_sim_tag_get` behaviour, through the API users already know. Server
refcount = live server-role tags at that endpoint; last destroy tears it down.

```c
int32_t t = plc_tag_create(
    "protocol=ab-eip&role=server&gateway=0.0.0.0&port=44818"
    "&name=PumpSpeed&elem_type=DINT&elem_count=1", 0);
plc_tag_set_int32(t, 0, 1234);   /* seed the simulated value */
```

*Alternative (only if explicit lifecycle is wanted):* a first-class device
handle (`role=server` with no `name=` returns the device; tags attach via
`device=<id>`). More surface, explicit start/stop. Default to the auto model.

### 11.3 Configuration, realism, testing

- **New attributes:** `role`; reuse of `elem_type`/`elem_count`/`name` on the
  server side; fault injection `sim_delay_ms`, `sim_fault=<cip_status>`,
  `sim_drop_rate`, `sim_max_packet` (map onto existing
  `set_response_delay`/`set_max_packet`); identity via `make`/`model`/`serial`.
- **Callbacks:** ride the existing `plc_tag_register_callback` read/write/created
  enum; delete `device_sim_tag_cb` / `device_sim_conn_cb`.
- **Realism:** keep the §8 dialect registry — it's the right seam. Keep the
  partial-transfer (`0x06`) and packet-cap modeling; that's the part not to
  simplify.
- **Testing:** in-process loopback (client + server tag in one process). Add
  `transport=loopback` to bypass the socket entirely (shared in-memory queue)
  for fast unit tests; the real-socket path stays for integration. Reuse
  `run_device_sim_tests.sh`.

### 11.4 Feature gating for embedded use

Single library, compile-time `#if`, default **on**. Do **not** ship multiple
prebuilt artifacts — that is N build configs / artifacts / support matrices
forever, for no source-level benefit.

- One generated `plctag_features.h`:
  `LIBPLCTAG_FEATURE_{SIM,AB,OMRON,MODBUS,PCCC}`.
- `tag_type_map` rows and each protocol `.c` wrapped in `#if`. A disabled feature
  drops its rows → its `protocol=` simply fails to match → clean
  `PLCTAG_ERR_NOT_FOUND`. No runtime cost, no plugin loader.
- CMake options `-DLIBPLCTAG_SIM=OFF` etc.; document a one-table feature matrix.

Trade-off, plainly: single-source + `#if` costs guard discipline; multiple
libraries cost a permanent N× packaging/CI/support burden. One library with
flags is the lazy and correct choice.

### 11.5 Phasing

1. Unify the tag vocabulary — sim consumes attribute strings; `tag_def_t` is the
   single source of symbol metadata. (Unblocks the listing dialects cleanly.)
2. `(protocol, role)` dispatch + endpoint registry — server-role creates route
   to a sim constructor; server tags are real tag-table entries.
3. Replace `device_sim_*` callbacks with `plc_tag_register_callback`; delete the
   duplicate callback types.
4. Feature flags — `plctag_features.h`, CMake options, `#if`-guards.
5. Fault injection + `transport=loopback`.

Steps 1–3 are the merge; 4–5 are independent and land in any order.

### 11.6 Examples

All of these go through the existing `plc_tag_create` / `plc_tag_*` API. The only
new attribute on the create string is `role=server` plus the optional `sim_*`
and identity knobs; everything else is the grammar clients already use.

**A. Minimal: one simulated DINT, seeded and read back in-process**

```c
/* role=server auto-starts the backing server on bind_addr:port. */
int32_t srv = plc_tag_create(
    "protocol=ab-eip&role=server&gateway=0.0.0.0&port=44818"
    "&name=PumpSpeed&elem_type=DINT&elem_count=1", 1000);
if(srv < 0) { /* handle error */ }

plc_tag_set_int32(srv, 0, 1234);          /* seed the simulated value */

/* a normal client elsewhere now reads 1234 over the wire: */
int32_t cli = plc_tag_create(
    "protocol=ab-eip&gateway=127.0.0.1&path=1,0&name=PumpSpeed&elem_count=1", 1000);
plc_tag_read(cli, 1000);
int32_t v = plc_tag_get_int32(cli, 0);    /* == 1234 */

plc_tag_destroy(cli);
plc_tag_destroy(srv);                     /* last server-role tag → server stops */
```

**B. Several tags on one device (same endpoint = same server)**

```c
const char *base = "protocol=ab-eip&role=server&gateway=0.0.0.0&port=44818";
char attr[256];

snprintf(attr, sizeof(attr), "%s&name=Flags&elem_type=BOOL&elem_count=32", base);
int32_t flags = plc_tag_create(attr, 1000);

snprintf(attr, sizeof(attr), "%s&name=Recipe&elem_type=INT&elem_count=10", base);
int32_t recipe = plc_tag_create(attr, 1000);

snprintf(attr, sizeof(attr), "%s&name=Setpoint&elem_type=REAL&elem_count=1", base);
int32_t setpoint = plc_tag_create(attr, 1000);
/* first create started the server; the next two register tags on it. */
```

**C. Multi-dimensional array tag**

```c
/* 2x3 DINT array — same dims grammar the client uses to address it. */
int32_t grid = plc_tag_create(
    "protocol=ab-eip&role=server&gateway=0.0.0.0&port=44818"
    "&name=Grid&elem_type=DINT&dimensions=2,3", 1000);
```

**D. Identity + fault injection (realistic device, exercises client error paths)**

```c
int32_t srv = plc_tag_create(
    "protocol=ab-eip&role=server&gateway=0.0.0.0&port=44818"
    "&make=Rockwell&model=1756-L83E&serial=0x00C0FFEE"
    "&sim_delay_ms=50"          /* every response delayed 50 ms              */
    "&sim_max_packet=508"       /* force fragmentation / partial transfer    */
    "&sim_fault=0x05"           /* return CIP status 0x05 (path destination  */
                                /* unknown) so the client sees a real error  */
    "&name=Temperature&elem_type=REAL&elem_count=1", 1000);
```

**E. OMRON server (same API, dialect selected by the §7.6 CIP registry)**

```c
int32_t srv = plc_tag_create(
    "protocol=ab-eip&role=server&gateway=0.0.0.0&port=44818"
    "&make=Omron&model=NX102&name=Tank.Level&elem_type=REAL&elem_count=1", 1000);
```

**F. Modbus server**

```c
int32_t srv = plc_tag_create(
    "protocol=modbus-tcp&role=server&gateway=0.0.0.0&port=502"
    "&name=HoldingRegs&elem_type=INT16&elem_count=100", 1000);
```

**G. Loopback transport — no socket, fast unit test**

```c
/* transport=loopback shares an in-memory queue; client and server tags in the
 * same process never touch the network.  Ideal for CI. */
int32_t srv = plc_tag_create(
    "protocol=ab-eip&role=server&transport=loopback&name=X&elem_type=DINT&elem_count=1", 0);
int32_t cli = plc_tag_create(
    "protocol=ab-eip&transport=loopback&name=X&elem_count=1", 0);

plc_tag_set_int32(srv, 0, 7);
plc_tag_read(cli, 0);
assert(plc_tag_get_int32(cli, 0) == 7);
```

**H. Write callback on the server side (reuses `plc_tag_register_callback`)**

```c
/* No bespoke device_sim_tag_cb — the server tag fires the same event enum a
 * client tag does.  Here: observe a client's write land on the simulated tag. */
void on_event(int32_t tag, int32_t event, int32_t status, void *udata) {
    if(event == PLCTAG_EVENT_WRITE_COMPLETED) {
        printf("client wrote Setpoint = %d\n", plc_tag_get_int32(tag, 0));
    }
}

int32_t srv = plc_tag_create(
    "protocol=ab-eip&role=server&gateway=0.0.0.0&port=44818"
    "&name=Setpoint&elem_type=DINT&elem_count=1", 1000);
plc_tag_register_callback(srv, on_event);
```

---

## 12. Code-pointer appendix

| Need | Copy / idea from |
|------|------------------|
| CLI argument parser (copy + adapt) | `ab_server_fiber/args.c`, `args.h` — see §1.1 for required changes |
| Thread-per-connection + `thread_detach`, accept loop | `ab_server/tcp_server.c:94,159` |
| Linear per-client request flow (header→len→payload→dispatch→send) | `ab_server_fiber.c:421-534` |
| Listener structure | `ab_server_fiber.c:369-417` |
| EIP/CPF/CIP dispatch on `Arena`/`Bytes` | `ab_server_fiber/{eip,cpf,cip}.c` |
| `bytes_pack`/`bytes_unpack` usage patterns | `ab_server_fiber/eip.c`, `cip.c` |
| Tag struct + per-tag mutex rationale | `ab_server/plc.h:66-128` |
| Multi-dim → linear element offset | `ab_server/cip.c:1196` |
| Full PCCC | `ab_server/pccc.c` |
| TCP bind/listen/reuseaddr | `ab_server/socket.c` (`socket_open_tcp_server`) |
| UDP/broadcast setsockopt + bind | `poc/utils/socket.c`, `scan_eip_network.c:277-328` |
| Wake channel + `socket_wait_event` | `platform.c:1128,1485,1704,2104` (posix) |
| List Identity item field order/endianness | `scan_eip_network.c:414-477` |
| Identity raw values | `get_identity` captures in the task brief |

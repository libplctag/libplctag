# Proposal: New `enip-tcp` Protocol Implementation

## Goal

Create a new, clean EtherNet/IP CIP protocol implementation (`enip-tcp`) alongside the existing `ab-eip` protocol. The new implementation uses:

- **Blocking-style I/O thread** (linear flow, no state machine) — matching the Modbus pattern.
- **Sorted active-tag list** per connection — no request objects, no tickler thread dependency.
- **Arena allocator + Bytes struct** from `~/Projects/data_table` for all packet encoding/decoding.
- **Native OS event handling** — `epoll` (Linux), `kqueue` (macOS), `WaitForMultipleObjects` (Windows) — instead of `select()`.
- **Non-blocking sockets** with `TCP_NODELAY`, `SO_REUSEADDR`, explicit timeout management.

The end state is a complete ControlLogix standard-tag implementation that can fully replace the AB CIP path for standard tags, tested at every phase against `ab_server` and real hardware.

---

## Source Material Reference

### Coding Guidelines

The file `~/Projects/libplctag/external_docs/coding_guidelines.md` contains requirements about the C code for this project.

### Memory

- Prefer heap-based allocation for all data blocks larger than 64 bytes or so.  It is important to keep the stack size as small as possible for embedded systems.
- Any data shared between threads must use rc_alloc and the functions from `src/utils/rc.h`.

### Existing Code to Copy/Adapt

| Source | Location | Used In |
|--------|----------|---------|
| Arena allocator | `~/Projects/data_table/arena.[ch]` | All phases (memory for packet building) |
| Bytes struct + pack/unpack | `~/Projects/data_table/bytes.[ch]` | All phases (packet encoding/decoding) |
| EIP/CIP encoding/decoding | `~/Projects/data_table/cip.[ch]` | Phases 4–10 (EIP headers, CPF, CIP services, Forward Open/Close, path encoding, response parsing) |
| Protocol registration | [init.c](src/libplctag/lib/init.c) | Phase 1 |
| TAG\_BASE\_STRUCT / vtable | [tag.h](src/libplctag/lib/tag.h) | Phase 2 |
| AB session struct | [session.h](src/libplctag/protocols/ab/session.h) | Phase 2 reference |
| AB tag struct | [tag.h](src/libplctag/protocols/ab/tag.h) | Phase 2 reference |
| Modbus sorted list pattern | [modbus.c](src/libplctag/protocols/mb/modbus.c) | Phase 2 (global connection vector, `find_or_create`), Phase 11 (active\_tags scheduling) |
| Platform socket code | [platform/posix/platform.c](src/platform/posix/platform.c) | Phase 3 reference (socket options, wake pipe pattern) |
| `ab_server` simulator | [src/tests/](src/tests/) | All test phases |

### Key Functions from `~/Projects/data_table/cip.[ch]`

These are the building blocks for EIP/CIP packet construction and parsing. They use `Arena` + `Bytes`:

```c
/* EIP encapsulation — 24-byte header + payload */
Bytes eip_encode_header(Arena *a, uint16_t command, uint32_t session_handle, Bytes payload);

/* CPF for unconnected messaging (SendRRData, cmd 0x006F) */
Bytes cpf_encode_unconnected(Arena *a, Bytes payload);

/* CPF for connected messaging (SendUnitData, cmd 0x0070) */
Bytes cpf_encode_connected(Arena *a, uint32_t conn_id, uint16_t conn_seq_num, Bytes payload);

/* CIP service with class/instance path */
Bytes cip_encode_object_service(Arena *a, uint8_t service, uint16_t class_id, uint16_t instance_id, Bytes service_data);

/* Wrap CIP in Unconnected Send (service 0x52 → Connection Manager) */
Bytes cip_encode_unconnected(Arena *a, Bytes payload, Bytes route);

/* Forward Open / Close */
Bytes cip_encode_forward_open_payload(Arena *a, ForwardOpenParams params, Bytes device_mr_route);
Bytes cip_encode_forward_close_payload(Arena *a, uint16_t serial, uint16_t vendor, uint32_t orig_serial, Bytes route);

/* Path/segment builders */
Bytes cip_encode_port_segment(Arena *a, uint8_t port, uint8_t slot);
Bytes cip_encode_mr_route(Arena *a, uint8_t port, uint8_t slot);
Bytes encode_tag_name(Arena *a, const char *tag);

/* Response parsing */
CipResponse eip_parse_response(Bytes response);
Bytes cip_get_response_data(CipResponse cip_resp);
ForwardOpenResponse cip_parse_forward_open_response(CipResponse cip_resp);
```

### Key Functions from `~/Projects/data_table/bytes.[ch]`

```c
/* Core struct: pointer + length, no ownership */
typedef struct { uint8_t *data; size_t len; } Bytes;

/* Python struct.pack-style encoding — format: "<BHI" etc. */
Bytes bytes_pack(Arena *a, const char *fmt, ...);

/* Python struct.unpack-style decoding — returns remaining bytes */
Bytes bytes_unpack(Bytes data, const char *fmt, ...);

/* Concatenation */
Bytes bytes_join(Arena *a, Bytes first, Bytes second);
Bytes bytes_concat(Arena *a, int count, ...);

/* Slicing (no allocation) */
Bytes bytes_from_buf(const uint8_t *buf, size_t len);
Bytes bytes_slice(Bytes b, size_t offset, size_t len);
Bytes bytes_pad_even(Arena *a, Bytes b);
```

### Key `Arena` API from `~/Projects/data_table/arena.[ch]`

```c
typedef struct {
    uint8_t *buffer;
    size_t length;
    size_t capacity;
    size_t high_water;
} Arena;

Arena arena_init(size_t size);
void *arena_alloc(Arena *a, size_t size);
void arena_reset(Arena *a);
size_t arena_save(Arena *a);
void arena_restore(Arena *a, size_t saved);
void arena_free(Arena *a);
```

The arena is used per-packet: `arena_reset()` at the start of each send/receive cycle, build the packet with `bytes_pack()` / `bytes_concat()`, send it, then reset. No per-packet `malloc`/`free`.

---

## PLC Type and Protocol Compatibility Reference

Retained from the original analysis — this defines what the final implementation must support.

### Tag Type Compatibility by PLC Type

| Tag Type / Protocol | ControlLogix / CompactLogix | Micro800 |
|---------------------|---------------------------|----------|
| **CIP Standard** | Yes | Yes (variant) |
| **CIP Listing** | Yes | No |
| **CIP UDT** | Yes | No |
| **CIP Raw** | Yes | No |
| **CIP Identity** | Yes | No |
| **Messaging Mode** | Connected or Unconnected | Connected (required) |
| **Forward Open Required?** | Optional (tag-dependent) | Yes (always) |
| **Multi-Request Packing** | Yes (space-limited) | No |

### Connection Buffer Size and Fragmentation

| Capability | ControlLogix / CompactLogix | Micro800 |
|-----------|---------------------------|----------|
| **Connection Buffer** | 500B (default), ~4KB (Large FO) | 500B (default), ~4KB (Large FO) |
| **CIP Fragment Services** | Yes | Yes |
| **Payload Per Packet** | ~470B (std FO), ~3900B (Large FO) | ~470B (std FO), ~3900B (Large FO) |
| **Max Total Transfer** | ~64KB (CIP 16-bit offset) | ~64KB (CIP 16-bit offset) |

---

## Phase Plan

Each phase produces a buildable, testable result. Phases are strictly incremental — each builds on the previous one's code.

---

### Phase 1: Protocol Registration and Module Skeleton

**Goal**: Register `enip-tcp` as a new protocol in libplctag's dispatch table. The library recognizes `protocol=enip-tcp` in attribute strings and calls the new module's init/teardown.

**What to implement**:

1. **Add entries to `tag_type_map[]`** in [init.c](src/libplctag/lib/init.c#L62-L69):

   ```c
   {.protocol = "enip-tcp", .make = NULL, .family = NULL, .model = NULL, .tag_constructor = enip_tcp_tag_create},
   {.protocol = "enip_tcp", .make = NULL, .family = NULL, .model = NULL, .tag_constructor = enip_tcp_tag_create},
   ```

   This follows the existing pattern where both hyphen and underscore variants are registered (see `ab-eip`/`ab_eip` and `modbus-tcp`/`modbus_tcp`).

2. **Add `#include` and init/teardown calls** to [init.c](src/libplctag/lib/init.c):

   - Add `#include <libplctag/protocols/enip_tcp/enip_tcp.h>` at the top alongside the other protocol includes.
   - In `initialize_modules()` (around line 280), add after the Omron init block:
     ```c
     pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Initializing ENIP-TCP module.");
     rc = enip_tcp_init();
     if(rc != PLCTAG_STATUS_OK) {
         pdebug(DEBUG_MODULE_INIT, DEBUG_ERROR, 0, "Unable to initialize ENIP-TCP module!");
         atomic_set_int32(&library_state, LIB_STATE_UNINITIALIZED);
         return rc;
     }
     ```
   - In `destroy_modules()` (around line 165), add before the `lib_teardown()` call:
     ```c
     pdebug(DEBUG_MODULE_INIT, DEBUG_INFO, 0, "Tearing down ENIP-TCP module.");
     enip_tcp_teardown();
     ```

3. **Create new source directory** `src/libplctag/protocols/enip_tcp/`.

4. **Create `enip_tcp.h`** with:
   ```c
   #pragma once
   #include <libplctag/lib/tag.h>
   #include <utils/attr.h>

   int enip_tcp_init(void);
   void enip_tcp_teardown(void);
   plc_tag_p enip_tcp_tag_create(attr attribs,
       void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
       void *userdata);
   ```

5. **Create `enip_tcp.c`** with stub implementations:
   - `enip_tcp_init()` — logs "ENIP-TCP module initialized", returns `PLCTAG_STATUS_OK`.
   - `enip_tcp_teardown()` — logs "ENIP-TCP module torn down".
   - `enip_tcp_tag_create()` — returns `NULL` (no tag creation yet). Log the attribute string for debugging.

6. **Add to CMakeLists.txt** — add the new `.c` files to the libplctag source list. Follow the pattern used by existing protocol directories.

7. **Add debug module ID** — add `PLCTAG_MODULE_ENIP_TCP` to `plctag_debug_module_t` in [libplctag.h](src/libplctag/lib/libplctag.h#L193-L222) and add the corresponding entry in the debug module name array.

**Files to create**:
- `src/libplctag/protocols/enip_tcp/enip_tcp.h`
- `src/libplctag/protocols/enip_tcp/enip_tcp.c`

**Files to modify**:
- [src/libplctag/lib/init.c](src/libplctag/lib/init.c) — tag_type_map, initialize_modules, destroy_modules
- [src/libplctag/lib/libplctag.h](src/libplctag/lib/libplctag.h) — debug module enum
- `CMakeLists.txt` — source file list

**How to test**: Build the library and run an existing AB test (e.g., `test_simultaneous_rw_ab_server`). The test uses `protocol=ab-eip` so it exercises the existing AB path, but init/teardown of the ENIP-TCP module is triggered by library init. Verify in debug output at `PLCTAG_DEBUG_INFO` level:

```
Initializing ENIP-TCP module.
```

on startup, and:

```
Tearing down ENIP-TCP module.
```

on shutdown. Also verify that `plc_tag_create("protocol=enip-tcp&gateway=127.0.0.1&path=1,0&name=TestTag&elem_type=DINT&elem_count=1", 5000)` returns an error (tag_create returns NULL → `PLCTAG_ERR_CREATE`) since the stub does nothing yet.

---

### Phase 2: Tag and Connection Structs

**Goal**: Define the core data structures for `enip_tcp_tag_t` and `enip_tcp_conn_t` (connection). Support creating multiple tags across multiple connections, with connection sharing based on gateway+path.

**What to implement**:

1. **Create `enip_tcp_tag.h`** with the tag struct:
   ```c
   typedef enum {
       ENIP_TAG_OP_IDLE = 0,
       ENIP_TAG_OP_READ_REQUEST,
       ENIP_TAG_OP_READ_RESPONSE,
       ENIP_TAG_OP_WRITE_REQUEST,
       ENIP_TAG_OP_WRITE_RESPONSE
   } enip_tag_op_t;

   struct enip_tcp_tag_t {
       TAG_BASE_STRUCT;

       enip_tcp_conn_p conn;       /* back-pointer to connection */
       enip_tag_op_t op;           /* current operation state */
       int64_t op_time;            /* when this op should execute (ms since epoch) */
       bool in_active_list;        /* true if in conn->active_tags */

       /* CIP tag identity */
       char *name;                 /* symbolic tag name (e.g. "MyTag") */
       uint16_t elem_type;         /* CIP type code */
       int elem_count;             /* number of elements */
       int elem_size;              /* bytes per element */

       /* fragmentation state */
       int offset;                 /* current byte offset into tag data (for fragmented reads/writes) */

       /* operation tracking */
       int read_in_progress;
       int write_in_progress;
   };
   ```

   The tag embeds `TAG_BASE_STRUCT` (defined in [tag.h](src/libplctag/lib/tag.h#L120-L158)) which provides: `data`, `size`, `tag_id`, `api_mutex`, `tag_cond_wait`, `vtable`, `auto_sync_read_ms`, `auto_sync_write_ms`, `callback`, `userdata`, `event_*_status`, and all the bitfield flags (`read_complete`, `write_complete`, `tag_is_dirty`, `skip_tickler`, etc.).

2. **Create `enip_tcp_tag.c`** with:
   - `enip_tcp_tag_create()` — parse attributes (`gateway`, `path`, `name`, `elem_type`, `elem_count`, `elem_size`), call `plc_tag_generic_init_tag()` (from [lib.c](src/libplctag/lib/lib.c) — same as AB and Modbus), find or create a connection, allocate tag data buffer, wire up the vtable, and set `skip_tickler = 1`.
   - A vtable definition with stubs for all entries:
     ```c
     struct tag_vtable_t enip_tcp_vtable = {
         .abort   = enip_tcp_abort,
         .read    = enip_tcp_read,
         .status  = enip_tcp_status,
         .tickler = enip_tcp_tickler,
         .write   = enip_tcp_write,
         .wake_plc = enip_tcp_wake_plc,
         .tag_data_written = enip_tcp_tag_data_written,
         .get_int_attrib = enip_tcp_get_int_attrib,
         .set_int_attrib = enip_tcp_set_int_attrib,
         .get_byte_array_attrib = NULL,
     };
     ```
   - Stub implementations: `enip_tcp_read()` / `enip_tcp_write()` return `PLCTAG_ERR_NOT_IMPLEMENTED`, `enip_tcp_status()` returns the tag's current status, `enip_tcp_abort()` sets `op = IDLE`, etc.
   - Tag destructor (via `rc_alloc` destructor callback) — removes tag from connection's `active_tags`, decrements connection's tag count.

3. **Create `enip_tcp_conn.h`** with the connection struct:
   ```c
   struct enip_tcp_conn_t {
       /* identity — used for connection sharing */
       char *host;
       int port;                       /* default 44818 */
       char *path;                     /* e.g. "1,0" for backplane port 1, slot 0 */
       int connection_group_id;

       /* encoded CIP path (backplane/slot → port/link segment) */
       uint8_t *conn_path;
       uint8_t conn_path_size;

       /* I/O context — owned by the handler thread, initialized in find_or_create */
       async_t *async;                 /* OS event loop: epoll_fd/kqueue_fd/HANDLE */
       socket_t sock;                  /* TCP socket fd (INVALID_SOCKET when disconnected) */
       Arena arena;                    /* Packet build/parse arena — reset each cycle */

       /* EIP session (to be populated in Phase 4) */
       uint32_t session_handle;

       /* sorted active tag list (matching Modbus pattern) */
       vector_p active_tags;           /* sorted by op_time */
       int64_t next_event_time;        /* earliest op_time across all active tags */

       /* thread management */
       thread_p handler_thread;
       volatile int terminating;
       mutex_p conn_mutex;
       cond_p conn_wait_cond;

       /* connection status */
       atomic_int32_t tag_count;
       atomic_int32_t connection_status;

       /* timing */
       int auto_disconnect_enabled;
       int auto_disconnect_timeout_ms;
       atomic_int32_t connection_inactivity_timeout_ms;

       /* on the global list? */
       int on_list;
   };
   ```

4. **Create `enip_tcp_conn.c`** with:
   - **Global connection list**: A `static vector_p connections` protected by `static mutex_p conn_mutex`, matching the AB pattern in [session.c](src/libplctag/protocols/ab/session.c#L150-L151). The Modbus protocol uses a linked list; use a vector instead for O(1) indexed iteration.
   - **`enip_tcp_conn_find_or_create()`** — walk the global `connections` vector under `conn_mutex`. Match on `connection_group_id`, `host`, `port`, and `path` (same logic as `session_find_or_create()` in [session.c](src/libplctag/protocols/ab/session.c)). If no match, allocate via `rc_alloc()`, create the mutex/condvar/vector, insert into global list, initialize `conn->async = async_create()`, `conn->arena = arena_init(8192)`, `conn->sock = INVALID_SOCKET`, and start the handler thread (stub thread that just sleeps and checks `terminating`). The connection destructor (the `rc_free` callback) calls `arena_free(&conn->arena)` and `async_destroy(conn->async)`.
   - **`enip_tcp_conn_startup()`** — creates the global `conn_mutex` and `connections` vector. Called from `enip_tcp_init()`.
   - **`enip_tcp_conn_teardown()`** — marks all connections `terminating = 1`, wakes their threads, joins them, destroys the global vector. Called from `enip_tcp_teardown()`.
   - **Sorted active-tag helpers** — port from Modbus [modbus.c](src/libplctag/protocols/mb/modbus.c):
     - `insert_tag_sorted(conn, tag)` — binary search by `op_time`, insert at correct position. Set `tag->in_active_list = true`.
     - `move_tag_sorted(conn, tag)` — update `op_time`, swap element to maintain sort order.
     - `remove_tag_from_active(conn, tag)` — find by pointer, remove, set `tag->in_active_list = false`.

5. **Wire `enip_tcp_tag_create()`** into [init.c](src/libplctag/lib/init.c) (the `tag_type_map` entries from Phase 1 already point to it). Move the implementation from the stub in `enip_tcp.c` to `enip_tcp_tag.c`.

**Files to create**:
- `src/libplctag/protocols/enip_tcp/enip_tcp_tag.h`
- `src/libplctag/protocols/enip_tcp/enip_tcp_tag.c`
- `src/libplctag/protocols/enip_tcp/enip_tcp_conn.h`
- `src/libplctag/protocols/enip_tcp/enip_tcp_conn.c`

**Files to modify**:
- `src/libplctag/protocols/enip_tcp/enip_tcp.c` — wire init/teardown to conn_startup/conn_teardown
- `CMakeLists.txt` — add new source files

**How to test**: Write a small test program (or extend an existing one) that creates multiple tags with `protocol=enip-tcp`:

```c
int32_t t1 = plc_tag_create("protocol=enip-tcp&gateway=127.0.0.1&path=1,0&name=Tag1&elem_type=DINT&elem_count=1", 5000);
int32_t t2 = plc_tag_create("protocol=enip-tcp&gateway=127.0.0.1&path=1,0&name=Tag2&elem_type=DINT&elem_count=1", 5000);
int32_t t3 = plc_tag_create("protocol=enip-tcp&gateway=10.0.0.1&path=1,0&name=Tag3&elem_type=DINT&elem_count=1", 5000);
```

Verify with debug logging that:
- `t1` and `t2` share the same connection (same gateway+path).
- `t3` creates a separate connection (different gateway).
- Two handler threads are created (one per connection).
- Destroying all tags causes both connections to teardown.
- `plc_tag_status()` returns `PLCTAG_ERR_NOT_IMPLEMENTED` (reads not implemented yet).

---

### Phase 3: Async Socket Layer and Blocking Connection Handler

**Goal**: Implement cross-platform async socket I/O and a blocking-style handler loop that can connect to a PLC via TCP. This is the I/O foundation for all subsequent phases.

**What to implement**:

#### 3a: Copy `arena.[ch]` and `bytes.[ch]`

Copy from `~/Projects/data_table/`:
- `arena.c`, `arena.h` → `src/libplctag/protocols/enip_tcp/arena.[ch]`
- `bytes.c`, `bytes.h` → `src/libplctag/protocols/enip_tcp/bytes.[ch]`

Modifications needed:
- Replace `fprintf(stderr, ...)` / `exit(1)` in `arena_alloc()` with libplctag error handling — return `NULL` on overflow instead of crashing. Callers must check.
- Replace `fprintf(stderr, ...)` in `arena_free()` with `pdebug()` for stats logging.
- Add `#include <utils/debug.h>` for pdebug access (or keep arena self-contained and use a callback for logging).

#### 3b: Async Event Object (`async_t`)

Create `src/libplctag/protocols/enip_tcp/async_sock.[ch]`.

The `async_t` object encapsulates the OS-specific event notification mechanism. It is created once per connection in `enip_tcp_conn_find_or_create()` and stored in `conn->async`. Functions access it through the connection pointer rather than receiving it as an explicit argument.

```c
/* Opaque async context — one per connection handler thread */
async_t *async_create(void);
void async_destroy(async_t *a);

/* Wake the async_wait from another thread (e.g., when a tag queues work) */
plctag_error_code_t async_wake(async_t *a);
```

**Platform-specific internals of `async_t`**:

| Field | Linux | macOS | Windows |
|-------|-------|-------|---------|
| Event loop fd | `int epoll_fd` (from `epoll_create1`) | `int kqueue_fd` (from `kqueue`) | `HANDLE wake_event` (from `CreateEvent`) |
| Wake mechanism | `int eventfd` (from `eventfd(0, EFD_NONBLOCK)`) registered with `epoll_fd` | `EVFILT_USER` kevent (no extra fd needed) | `HANDLE wake_event` (manual-reset event) |

Use `#if defined(__linux__)` / `#elif defined(__APPLE__)` / `#elif defined(_WIN32)` compile-time selection. All three implementations provide the same external API.

**`async_wake()`** implementation:
- **Linux**: `eventfd_write(a->eventfd, 1)` — the epoll loop sees this as readable.
- **macOS**: `kevent()` with `EVFILT_USER` + `NOTE_TRIGGER` on the kqueue — no separate fd needed.
- **Windows**: `SetEvent(a->wake_event)`.

#### 3c: Stream Socket Functions

```c
/* Create a non-blocking TCP socket, returns the raw socket handle */
socket_t async_stream_create(void);
/* POSIX: socket_t = int.  Windows: socket_t = SOCKET. */

/* Non-blocking connect with timeout, monitored via async_t */
plctag_error_code_t async_stream_connect(async_t *a, socket_t sock,
    const char *host, uint16_t port, size_t timeout_ms);

/* Blocking-style exact read: reads exactly read_buf.len bytes or times out */
plctag_error_code_t async_stream_read_exact(async_t *a, socket_t sock,
    Bytes read_buf, size_t timeout_ms);

/* Blocking-style exact write: writes exactly write_buf.len bytes or times out */
plctag_error_code_t async_stream_write_exact(async_t *a, socket_t sock,
    Bytes write_buf, size_t timeout_ms);

/* Close socket */
plctag_error_code_t async_stream_close(socket_t sock);
```

**`async_stream_create()`** must set these socket options (reference: existing [platform.c](src/platform/posix/platform.c#L1166-L1340)):

- `O_NONBLOCK` (POSIX) / `ioctlsocket(FIONBIO)` (Windows)
- `TCP_NODELAY` — disable Nagle's algorithm
- `SO_REUSEADDR` — allow quick rebind
- `SO_NOSIGPIPE` (BSD/macOS only) — prevent SIGPIPE on write to closed socket

Do **not** set `SO_LINGER(0)` — the existing AB code sets this for abort-on-close; evaluate whether the new implementation needs it.

**`async_stream_connect()`** implementation:

1. Resolve hostname via `getaddrinfo()` (POSIX) / same on Windows.  Try `inet_pton()` first for raw IPs.
2. Call `connect()` — expect `EINPROGRESS` (POSIX) / `WSAEWOULDBLOCK` (Windows).
3. Wait for writability:
   - **Linux**: `epoll_ctl(ADD, sock, EPOLLOUT)`, then `epoll_wait(timeout)`. After connect, `epoll_ctl(DEL, sock)`.
   - **macOS**: `kevent()` with `EVFILT_WRITE` + `EV_ADD | EV_ONESHOT`, then `kevent(timeout)`.
   - **Windows**: `WSAEventSelect(sock, event, FD_CONNECT)`, then `WaitForMultipleObjects({sock_event, wake_event}, timeout)`.
4. Check `SO_ERROR` via `getsockopt()` to confirm connection success.
5. Return `PLCTAG_STATUS_OK` on success, `PLCTAG_ERR_TIMEOUT` on timeout, `PLCTAG_ERR_OPEN` on failure.
6. If `async_wake()` was called during the wait (detected via the wake mechanism), return `PLCTAG_ERR_ABORT` so the caller can check `terminating`.

**`async_stream_read_exact()`** implementation:

1. Loop: attempt `recv()` / `read()` into `read_buf.data + bytes_read_so_far`.
2. If partial read (`EAGAIN`/`EWOULDBLOCK`), wait for readability:
   - **Linux**: `epoll_ctl(MOD, sock, EPOLLIN)`, `epoll_wait(remaining_timeout)`.
   - **macOS**: `kevent()` with `EVFILT_READ`, `kevent(remaining_timeout)`.
   - **Windows**: `WSAEventSelect(sock, event, FD_READ)`, `WaitForMultipleObjects(remaining_timeout)`.
3. Also monitor the wake mechanism in the same wait call. If woken, return `PLCTAG_ERR_ABORT`.
4. Subtract elapsed time from `timeout_ms` on each iteration.
5. Return `PLCTAG_STATUS_OK` when exactly `read_buf.len` bytes have been read.
6. Return `PLCTAG_ERR_TIMEOUT` if timeout expires, `PLCTAG_ERR_READ` on socket error, `PLCTAG_ERR_BAD_CONNECTION` if peer disconnects (recv returns 0).

**`async_stream_write_exact()`** — same pattern as read_exact but with `send()` / `write()` and `EPOLLOUT` / `EVFILT_WRITE` / `FD_WRITE`.

#### 3d: Connection Handler Thread (Blocking Loop)

Update `enip_tcp_conn.c` to replace the stub handler thread with a real blocking-style main loop:

```c
THREAD_FUNC(enip_tcp_conn_handler) {
    enip_tcp_conn_p conn = (enip_tcp_conn_p)arg;
    int retry_count = 0;

    while (!conn->terminating) {
        conn->sock = async_stream_create();
        if (conn->sock == INVALID_SOCKET) goto retry_wait;

        /* --- Stage 1: TCP Connect --- */
        int rc = async_stream_connect(conn->async, conn->sock, conn->host, (uint16_t)conn->port, 5000);
        if (rc != PLCTAG_STATUS_OK) goto close_socket;

        /* --- Stages 2-3: EIP Register + Forward Open (Phase 4+) --- */
        /* ... placeholder ... */

        /* --- Stage 4: Steady-State Loop --- */
        retry_count = 0;
        atomic_set_int32(&conn->connection_status, PLCTAG_CONN_STATUS_UP);

        while (!conn->terminating) {
            /* tickle_active_tags + send/receive (Phases 6+) */
            /* ... placeholder ... */

            /* Sleep until next event or timeout */
            /* For now, just sleep with wake support */
            /* ... */

            if (conn->terminating) break;
        }

        atomic_set_int32(&conn->connection_status, PLCTAG_CONN_STATUS_DOWN);

        /* --- Cleanup --- */
        /* Forward Close (Phase 9+) */
        /* EIP Unregister (Phase 4+) */

    close_socket:
        async_stream_close(conn->sock);
        conn->sock = INVALID_SOCKET;

    retry_wait:
        if (conn->terminating) break;
        /* Exponential backoff: 500ms * 2^retry_count, capped at 30s */
        int64_t wait_ms = 500 * (1 << (retry_count < 6 ? retry_count : 6));
        if (wait_ms > 30000) wait_ms = 30000;
        retry_count++;
        /* Sleep with wake support so we can exit promptly */
        cond_wait(conn->conn_wait_cond, (int)wait_ms);
    }

    /* conn->async and conn->arena are freed in the conn destructor */
    THREAD_RETURN(0);
}
```

`async_t`, `socket_t`, and `Arena` are stored in `enip_tcp_conn_t` (see Phase 2). The handler thread accesses them via `conn->async`, `conn->sock`, and `conn->arena`. The connection destructor calls `arena_free(&conn->arena)` and `async_destroy(conn->async)`.

**Files to create**:

- `src/libplctag/protocols/enip_tcp/arena.h` (copy from `~/Projects/data_table/arena.h`)
- `src/libplctag/protocols/enip_tcp/arena.c` (copy from `~/Projects/data_table/arena.c`, modify error handling)
- `src/libplctag/protocols/enip_tcp/bytes.h` (copy from `~/Projects/data_table/bytes.h`)
- `src/libplctag/protocols/enip_tcp/bytes.c` (copy from `~/Projects/data_table/bytes.c`)
- `src/libplctag/protocols/enip_tcp/async_sock.h`
- `src/libplctag/protocols/enip_tcp/async_sock.c`

**Files to modify**:

- `src/libplctag/protocols/enip_tcp/enip_tcp_conn.c` — real handler thread
- `src/libplctag/protocols/enip_tcp/enip_tcp_conn.h` — add `async_t` mention
- `CMakeLists.txt` — add new source files, platform-specific compile flags

**How to test**: Create a tag pointing at a running `ab_server`:

```bash
# Terminal 1: start simulator
./build/bin_dist/ab_server --plc=ControlLogix --path=1,0 --tag=TestDINT:DINT[10]

# Terminal 2: test connection
# (write a small test_enip_connect.c that creates a tag with protocol=enip-tcp)
```

With debug level `PLCTAG_DEBUG_DETAIL`, verify in log output:

- `async_stream_create()` succeeds.
- `async_stream_connect()` to `127.0.0.1:44818` succeeds.
- The handler thread enters the steady-state loop.
- On `plc_tag_destroy()`, the connection closes cleanly.
- If `ab_server` is not running, verify the retry loop with exponential backoff.
- Test on **all three platforms** (Linux, macOS, Windows) to validate the `epoll`/`kqueue`/`WaitForMultipleObjects` paths.

---

### Phase 4: EIP Session Register/Unregister

**Goal**: The handler loop registers an EIP session after TCP connect and unregisters on disconnect. Uses `arena` + `bytes_pack`/`bytes_unpack` for packet encoding/decoding.

**What to implement**:

1. **Copy CIP encoding/decoding** from `~/Projects/data_table/cip.[ch]` → `src/libplctag/protocols/enip_tcp/cip.[ch]`. For this phase, only the following functions are needed:
   - `eip_encode_header()` — builds the 24-byte EIP encapsulation header + payload.
   - `eip_parse_response()` — parses EIP response, extracts CIP response from CPF envelope.

   The rest of `cip.[ch]` will be used in later phases. Copy the complete files now so they're available.

2. **Implement `eip_register_session()`** in `enip_tcp_conn.c`:

   ```text
   Function: plctag_error_code_t eip_register_session(enip_tcp_conn_p conn, uint32_t *session_handle_out)

   1. arena_reset(&conn->arena)
   2. Build register payload:  bytes_pack(&conn->arena, "<HI", 1 /*protocol_version*/, 0 /*options*/)
   3. Build full packet:       eip_encode_header(&conn->arena, EIP_CMD_REGISTER_SESSION /*0x0065*/, 0 /*no session yet*/, payload)
   4. Send:                    async_stream_write_exact(conn->async, conn->sock, packet, 5000)
   5. Read EIP header:         async_stream_read_exact(conn->async, conn->sock, {recv_buf, 24}, 5000)
   6. Parse length from header: bytes_unpack({recv_buf, 24}, "<2xH", &payload_len)  [skip cmd, read length]
   7. Read payload:            async_stream_read_exact(a, sock, {recv_buf+24, payload_len}, 5000)
   8. Parse full response:     bytes_unpack to extract session_handle, verify status == 0
   9. *session_handle_out = parsed session_handle
   10. Return PLCTAG_STATUS_OK
   ```

   Reference: The EIP RegisterSession is command `0x0065`. The existing AB code does this in `session_register()` in [session.c](src/libplctag/protocols/ab/session.c). The `cip.c` function `eip_encode_header()` handles the 24-byte header construction with `bytes_pack(arena, "<HHII8xI", command, payload_len, session, 0, 0)`.

3. **Implement `eip_unregister_session()`** in `enip_tcp_conn.c`:

   ```text
   Function: plctag_error_code_t eip_unregister_session(enip_tcp_conn_p conn)

   1. arena_reset(&conn->arena)
   2. Build packet: eip_encode_header(&conn->arena, 0x0066 /*UnregisterSession*/, conn->session_handle, {NULL, 0} /*empty payload*/)
   3. Send: async_stream_write_exact(conn->async, conn->sock, packet, 5000)
   4. No response expected for UnregisterSession.
   5. Return PLCTAG_STATUS_OK
   ```

4. **Wire into handler loop** — update the handler in `enip_tcp_conn.c`:

   ```c
   /* After TCP connect succeeds: */

   /* --- Stage 2: EIP Register Session --- */
   rc = eip_register_session(conn, &conn->session_handle);
   if (rc != PLCTAG_STATUS_OK) goto close_socket;

   /* ... steady-state loop ... */

   /* --- Cleanup (before close_socket): --- */
   eip_unregister_session(conn);
   conn->session_handle = 0;
   ```

**Note on recv buffer management**: The handler thread needs a receive buffer. Options:

- Stack-allocated: `uint8_t recv_buf[4096]` on the handler thread stack. Simple, sufficient for most responses.
- Arena-allocated: Use a separate arena region for receive data. Slightly more complex but avoids stack overflow with large responses.

Recommend stack-allocated for the receive buffer (it's a bounded size — EIP packets max out at ~64KB but typical responses are <4KB). Use `Bytes recv = bytes_from_buf(recv_buf, sizeof(recv_buf))` to wrap it for the unpack functions.

**Files to create**:

- `src/libplctag/protocols/enip_tcp/cip.h` (copy from `~/Projects/data_table/cip.h`)
- `src/libplctag/protocols/enip_tcp/cip.c` (copy from `~/Projects/data_table/cip.c`, remove trend-specific functions)

**Files to modify**:

- `src/libplctag/protocols/enip_tcp/enip_tcp_conn.c` — add register/unregister, wire into handler loop
- `src/libplctag/protocols/enip_tcp/enip_tcp_conn.h` — add `session_handle` field (already in struct)
- `CMakeLists.txt` — add cip.c

**How to test**: Run `ab_server` and create a tag with `protocol=enip-tcp`. With `PLCTAG_DEBUG_DETAIL`, verify:

```text
TCP connected to 127.0.0.1:44818
EIP RegisterSession sent, session_handle=0x00000001
```

Then destroy the tag and verify:

```text
EIP UnregisterSession sent
TCP socket closed
```

Also test with wrong port (should fail to connect) and verify the retry loop with register failure.

---

### Phase 5: CPF Headers and Unconnected Messaging

**Goal**: Add the ability to send and receive unconnected CIP messages through the connection. This is the transport layer for tag reads/writes (Phase 6+).

**What to implement**:

1. **A general `send_unconnected_cip()` function** in `enip_tcp_conn.c`:

   ```text
   Function: plctag_error_code_t send_unconnected_cip(
       enip_tcp_conn_p conn,
       Bytes cip_payload,          /* the inner CIP service request */
       Bytes route,                /* port/slot path to target device */
       uint8_t *recv_buf, size_t recv_buf_size,
       CipResponse *response_out,
       size_t timeout_ms)

   1. arena_save(&conn->arena)   /* save arena position for cleanup */
   2. Wrap in Unconnected Send:  cip_encode_unconnected(&conn->arena, cip_payload, route)
   3. Wrap in CPF:               cpf_encode_unconnected(&conn->arena, unconnected_payload)
   4. Wrap in EIP:               eip_encode_header(&conn->arena, EIP_CMD_SEND_RR_DATA /*0x006F*/, conn->session_handle, cpf)
   5. Send:                      async_stream_write_exact(conn->async, conn->sock, packet, timeout_ms)
   6. arena_restore(&conn->arena, saved)  /* free send-side arena memory */
   7. Read EIP header (24 bytes): async_stream_read_exact(conn->async, conn->sock, {recv_buf, 24}, timeout_ms)
   8. Parse payload length from header
   9. Read payload:              async_stream_read_exact(a, sock, {recv_buf+24, payload_len}, timeout_ms)
   10. Parse response:           eip_parse_response(bytes_from_buf(recv_buf, 24+payload_len))
   11. *response_out = parsed CipResponse
   12. Return PLCTAG_STATUS_OK
   ```

   Reference: The CPF encoding uses `cpf_encode_unconnected()` from [cip.c](cip.c) which builds `Interface(4) Timeout(2) ItemCount(2) NullAddr(4) DataItem(0x00B2+len)`. The Unconnected Send wraps the CIP payload with service `0x52` to Connection Manager class `0x06` instance `0x01`, plus priority/timeout and the route.

2. **Build the CIP route from the connection's path string**. Parse the user-provided `path` attribute (e.g., `"1,0"` meaning backplane port 1, slot 0) into a CIP port/link segment using `cip_encode_port_segment()`. Store the encoded route in `conn->conn_path` / `conn->conn_path_size`. The existing AB code does this path parsing in [session.c](src/libplctag/protocols/ab/session.c); the simplest initial implementation handles the common case of `"port,slot"` format.

3. **Encode the tag name** into CIP ANSI Extended Symbol Segment format. Call `encode_tag_name()` from [cip.c](cip.c) which builds `[0x91][length][ASCII name][pad_if_odd]`.

**Files to modify**:

- `src/libplctag/protocols/enip_tcp/enip_tcp_conn.c` — add `send_unconnected_cip()`, path parsing
- `src/libplctag/protocols/enip_tcp/enip_tcp_conn.h` — declare the function

**How to test**: Write a test that sends a CIP "Get Attribute All" to the Identity Object (class `0x01`, instance `0x01`) — this is a simple request that every EIP device responds to:

```c
/* CIP Get Attribute All, Class 1, Instance 1 */
Bytes payload = bytes_pack(&arena, "");  /* no service data */
Bytes cip_req = cip_encode_object_service(&arena, 0x01 /*Get_Attr_All*/, 0x01 /*Identity*/, 0x01, payload);
```

Verify the response contains the device's identity information (vendor ID, product name, etc.). `ab_server` responds to identity requests. This validates the entire CPF + Unconnected Send + CIP encode/decode pipeline without needing tag read/write.

---

### Phase 6: Tag Read (Non-Fragmented)

**Goal**: Implement single-packet CIP Read Tag Service (`0x4C`) for standard ControlLogix tags. This is the first phase where `plc_tag_read()` actually works end-to-end.

**What to implement**:

1. **CIP Read Tag Service encoding**. Add to `cip.[ch]`:

   ```c
   /* CIP Read Tag Service (0x4C) — non-fragmented */
   Bytes cip_encode_read_tag(Arena *a, const char *tag_name, uint16_t elem_count);
   ```

   Implementation:

   ```text
   1. Encode tag name:  encode_tag_name(arena, tag_name)  → ANSI symbol segment
   2. Append element count: bytes_pack(arena, "<H", elem_count)
   3. Combine: service byte 0x4C + path_words + name_segment + elem_count
      Full: bytes_pack(arena, "<BB*H", 0x4C, path_size_words, &encoded_name, elem_count)
   ```

   Reference: The existing AB code builds this in `build_read_request_connected()` in [eip_cip.c](src/libplctag/protocols/ab/eip_cip.c). The CIP Read Tag service code is `0x4C`, the fragmented variant is `0x52` (Phase 8).

2. **CIP Read response parsing**. Add to `cip.[ch]`:

   ```c
   /* Parse CIP Read Tag response — returns data type and tag data */
   plctag_error_code_t cip_parse_read_tag_response(CipResponse resp, uint16_t *data_type_out, Bytes *data_out);
   ```

   Implementation: After calling `cip_get_response_data()` to skip extended status, the data starts with `uint16_t data_type` followed by the raw tag value bytes.

3. **Implement `enip_tcp_read()`** in `enip_tcp_tag.c`. This is the vtable `.read` function called by `plc_tag_read()`:

   ```text
   Function: int enip_tcp_read(plc_tag_p p_tag)

   1. Cast to enip_tcp_tag_p tag
   2. If tag->read_in_progress, return PLCTAG_STATUS_PENDING
   3. Set tag->op = ENIP_TAG_OP_READ_REQUEST
   4. Set tag->op_time = time_ms()  /* immediate */
   5. Lock conn->conn_mutex
   6. insert_tag_sorted(conn, tag)  /* or move_tag_sorted if already in list */
   7. Unlock conn->conn_mutex
   8. async_wake(conn->async)       /* wake the handler thread */
   9. Set tag->read_in_progress = 1
   10. tag->status = PLCTAG_STATUS_PENDING
   11. Return PLCTAG_STATUS_PENDING
   ```

4. **Process reads in the handler thread's steady-state loop**. In `enip_tcp_conn.c`, implement `tickle_active_tags()`:

   ```text
   Function: int64_t tickle_active_tags(enip_tcp_conn_p conn)
   /* Uses conn->async, conn->sock, conn->arena internally. recv_buf is a local: uint8_t recv_buf[4096] */

   1. int64_t now = time_ms()
   2. int64_t next_wake = now + 100  /* default 100ms if nothing scheduled */
   3. Lock conn->conn_mutex
   4. For each tag in conn->active_tags where tag->op_time <= now:
      a. If tag->op == ENIP_TAG_OP_READ_REQUEST:
         - Lock tag->api_mutex
         - arena_reset(&conn->arena)
         - Build CIP read request: cip_encode_read_tag(&conn->arena, tag->name, tag->elem_count)
         - Wrap in route:          cip_encode_unconnected(&conn->arena, cip_req, conn_route)
         - Wrap in CPF:            cpf_encode_unconnected(&conn->arena, unconn_req)
         - Wrap in EIP:            eip_encode_header(&conn->arena, 0x006F, conn->session_handle, cpf)
         - Unlock conn->conn_mutex  (must not hold during I/O)
         - Send:                   async_stream_write_exact(conn->async, conn->sock, packet, 5000)
         - Read response:          read EIP header + payload
         - Parse:                  eip_parse_response() → CipResponse
         - cip_parse_read_tag_response() → data_type, data_bytes
         - Copy data into tag->data buffer
         - Set tag->read_complete = 1
         - Set tag->status = PLCTAG_STATUS_OK (or error code)
         - tag->op = ENIP_TAG_OP_IDLE
         - Signal: cond_signal(tag->tag_cond_wait)
         - Fire event: tag_raise_event + plc_tag_generic_handle_event_callbacks
         - Unlock tag->api_mutex
         - Re-lock conn->conn_mutex
         - remove_tag_from_active(conn, tag) (or reschedule if auto_sync)
      b. Else if tag->op_time > now:
         - next_wake = MIN(next_wake, tag->op_time)
         - break  /* list is sorted, no more due tags */
   5. Unlock conn->conn_mutex
   6. Return next_wake - now  /* ms to sleep */
   ```

5. **Update the handler thread's steady-state loop**:

   ```c
   while (!conn->terminating) {
       int64_t wait_ms = tickle_active_tags(conn);

       if (conn->terminating) break;

       /* Sleep until next event, socket data, or wake */
       cond_wait(conn->conn_wait_cond, (int)wait_ms);
   }
   ```

6. **Implement `enip_tcp_status()`** — returns `tag->status`. If `read_in_progress` and the operation completed in the handler thread, this returns `PLCTAG_STATUS_OK`. The generic `plc_tag_read()` synchronous path in [lib.c](src/libplctag/lib/lib.c) polls `plc_tag_status()` in a `cond_wait` loop using `tag->tag_cond_wait`, which the handler thread signals on completion.

**Files to modify**:

- `src/libplctag/protocols/enip_tcp/cip.c` / `cip.h` — add `cip_encode_read_tag()`, `cip_parse_read_tag_response()`
- `src/libplctag/protocols/enip_tcp/enip_tcp_tag.c` — implement `enip_tcp_read()`, `enip_tcp_status()`
- `src/libplctag/protocols/enip_tcp/enip_tcp_conn.c` — implement `tickle_active_tags()`, update handler loop

**How to test**: Use `tag_rw2` (in [src/tools/tag_rw2/](src/tools/tag_rw2/)) against `ab_server`:

```bash
# Terminal 1:
./build/bin_dist/ab_server --plc=ControlLogix --path=1,0 --tag=TestDINT:DINT[10]

# Terminal 2:
./build/bin_dist/tag_rw2 -t uint32 -p "protocol=enip-tcp&gateway=127.0.0.1&path=1,0&name=TestDINT&elem_count=10&elem_size=4"
```

**Note**: `tag_rw2` hardcodes `protocol=ab-eip` parsing — either modify it to accept a protocol parameter or write a small test program. The key verification is that `plc_tag_read(tag, 5000)` returns `PLCTAG_STATUS_OK` and `plc_tag_get_uint32(tag, 0)` returns a valid value.

Also test against **real ControlLogix hardware** if available. The `ab_server` simulator may not catch all edge cases.

---

### Phase 7: Tag Write (Non-Fragmented)

**Goal**: Implement CIP Write Tag Service (`0x4D`) for single-packet writes.

**What to implement**:

1. **CIP Write Tag Service encoding** in `cip.[ch]`:

   ```c
   /* CIP Write Tag Service (0x4D) — non-fragmented */
   Bytes cip_encode_write_tag(Arena *a, const char *tag_name, uint16_t data_type, uint16_t elem_count, Bytes data);
   ```

   Format: `[0x4D][path_words][encoded_name][data_type:2][elem_count:2][data...]`

   Reference: Existing AB code in `build_write_request_connected()` in [eip_cip.c](src/libplctag/protocols/ab/eip_cip.c).

2. **Implement `enip_tcp_write()`** in `enip_tcp_tag.c` — same pattern as `enip_tcp_read()` but sets `op = ENIP_TAG_OP_WRITE_REQUEST`.

3. **Add write processing to `tickle_active_tags()`** — for `ENIP_TAG_OP_WRITE_REQUEST`:
   - Build CIP write request with tag's current `data` buffer contents.
   - Send, receive response.
   - Parse response (write response has no data payload, just status).
   - Set `tag->write_complete = 1`, signal `tag_cond_wait`, fire events.

4. **Implement `enip_tcp_tag_data_written()`** — the vtable `.tag_data_written` function. Called by the library when `auto_sync_write_ms > 0` and the user writes data to the tag. Schedules a write by inserting/moving the tag in `active_tags` with `op = WRITE_REQUEST` and `op_time = now + auto_sync_write_ms`.

**Files to modify**:

- `src/libplctag/protocols/enip_tcp/cip.c` / `cip.h` — add `cip_encode_write_tag()`
- `src/libplctag/protocols/enip_tcp/enip_tcp_tag.c` — implement `enip_tcp_write()`, `enip_tcp_tag_data_written()`
- `src/libplctag/protocols/enip_tcp/enip_tcp_conn.c` — add write case to `tickle_active_tags()`

**How to test**: Use `tag_rw2` or a custom test to write then read back:

```bash
# Write 42 to first element, read back to verify
./test_enip_write --write 42 --read --protocol=enip-tcp --gateway=127.0.0.1 --path=1,0 --name=TestDINT --type=DINT
```

Also run `test_simultaneous_rw_ab_server` adapted for `enip-tcp` — concurrent reader and writer threads.

---

### Phase 8: Fragmented Read/Write

**Goal**: Support tags larger than one packet using CIP Read Tag Fragmented (`0x52`) and Write Tag Fragmented (`0x53`) services.

**What to implement**:

1. **CIP Fragment services** in `cip.[ch]`:

   ```c
   Bytes cip_encode_read_tag_fragmented(Arena *a, const char *tag_name, uint16_t elem_count, uint32_t byte_offset);
   Bytes cip_encode_write_tag_fragmented(Arena *a, const char *tag_name, uint16_t data_type, uint16_t elem_count, uint32_t byte_offset, Bytes data_chunk);
   ```

   The `byte_offset` field is a 32-bit offset into the tag data. Each fragment request reads/writes from that offset. The fragment size is limited by the connection buffer (~470 bytes for standard, ~3900 bytes for Large Forward Open).

2. **Multi-packet loop in `tickle_active_tags()`**. For reads:
   - Initialize `tag->offset = 0` on first request.
   - Send `cip_encode_read_tag_fragmented(arena, name, count, offset)`.
   - Parse response. The response includes the data chunk. Check CIP status:
     - Status `0x00` = complete (last fragment).
     - Status `0x06` = partial transfer — more data available.
   - Copy received bytes into `tag->data + offset`.
   - Advance `tag->offset += received_bytes`.
   - If partial, keep `tag->op = READ_REQUEST` with `op_time = now` (immediate re-request). The tag stays at the front of the sorted list.
   - If complete, set `read_complete = 1` and finish.

3. **Automatic fragment vs. non-fragment selection**: If `tag->size <= max_payload_for_single_read`, use the non-fragmented service (`0x4C`). Otherwise use fragmented (`0x52`). The max payload depends on unconnected message size (~480 bytes) or connected message buffer (negotiated in Forward Open).

4. **Write fragmentation** — same loop pattern but with `cip_encode_write_tag_fragmented()` and sending `tag->data + offset` chunks.

**Files to modify**:

- `src/libplctag/protocols/enip_tcp/cip.c` / `cip.h`
- `src/libplctag/protocols/enip_tcp/enip_tcp_conn.c` — update `tickle_active_tags()` for multi-packet handling

**How to test**: Create a tag with a large element count:

```
protocol=enip-tcp&gateway=127.0.0.1&path=1,0&name=BigArray&elem_type=DINT&elem_count=500&elem_size=4
```

This is 2000 bytes, requiring ~4-5 fragments with standard unconnected messaging. Verify all data reads back correctly. Test with both `ab_server` and real hardware.

---

### Phase 9: Forward Open / Connected Messaging

**Goal**: Implement CIP Forward Open/Close for connected messaging. Connected messaging is optional for ControlLogix (required for Micro800) and enables larger buffer sizes and multi-request packing.

**What to implement**:

1. **Forward Open negotiation** — the `cip.[ch]` already has `cip_encode_forward_open_payload()` and `cip_parse_forward_open_response()`. Add the negotiation logic to `enip_tcp_conn.c`:

   ```text
   Function: plctag_error_code_t forward_open_blocking(enip_tcp_conn_p conn)

   1. Build ForwardOpenParams with initial parameters:
      - ot_connection_id = random
      - to_connection_id = random
      - connection_serial = conn->conn_serial_number++
      - vendor_id = VENDOR_ID
      - originator_serial = ORIGINATOR_SERIAL
      - ot_rpi = 2000000 (2 seconds)
      - to_rpi = 2000000
      - ot_params/to_params = connection size + flags
      - transport_trigger = 0xA3 (server transport, class 3)
   2. Build route: cip_encode_mr_route(arena, port, slot)
   3. Encode payload: cip_encode_forward_open_payload(arena, params, route)
   4. Wrap in CIP object service (class 0x06, instance 0x01, service 0x54/0x5B)
   5. Send as unconnected message via send_unconnected_cip()
   6. Parse response:
      - Status 0x00 → success: store ot_connection_id, to_connection_id
      - Status 0x01 + extended 0x0100 → duplicate connection: retry with new IDs
      - Status 0x01 + extended 0x0109 → connection size too large: retry with PLC's suggested size
      - Extended FO (0x5B) fails → retry with old FO (0x54)
   7. Store conn->targ_connection_id, conn->orig_connection_id, conn->max_payload_size
   ```

   Reference: The existing AB implementation is in `send_forward_open_request()` / `receive_forward_open_response()` in [session.c](src/libplctag/protocols/ab/session.c).

2. **Connected messaging send/receive** — add `send_connected_cip()`:

   ```text
   Function: plctag_error_code_t send_connected_cip(
       enip_tcp_conn_p conn,
       uint32_t conn_id, uint16_t *conn_seq_num,
       Bytes cip_payload,
       uint8_t *recv_buf, size_t recv_buf_size,
       CipResponse *response_out,
       size_t timeout_ms)

   1. (*conn_seq_num)++
   2. cpf_encode_connected(&conn->arena, conn_id, *conn_seq_num, cip_payload)
   3. eip_encode_header(&conn->arena, EIP_CMD_SEND_UNIT_DATA /*0x0070*/, conn->session_handle, cpf)
   4. Send + receive + parse (same pattern as unconnected)
   ```

3. **Forward Close** — add `forward_close_blocking()` using `cip_encode_forward_close_payload()`. Called during cleanup.

4. **Wire into handler loop** — after EIP register, optionally do Forward Open (controlled by a `conn->use_connected_msg` flag parsed from attributes).

5. **Update `tickle_active_tags()`** — when `conn->use_connected_msg` is true, use `send_connected_cip()` instead of `send_unconnected_cip()`. For connected messaging, the CIP request doesn't need the Unconnected Send wrapper — the payload goes directly inside the CPF connected data item.

**Files to modify**:

- `src/libplctag/protocols/enip_tcp/enip_tcp_conn.c` — Forward Open/Close, connected send/receive, handler loop update
- `src/libplctag/protocols/enip_tcp/enip_tcp_conn.h` — add connection ID fields, `use_connected_msg` flag

**How to test**: Test both paths:

- **Unconnected** (default, already working from Phase 6): `protocol=enip-tcp&gateway=127.0.0.1&path=1,0&name=TestDINT&elem_count=1&elem_size=4`
- **Connected**: `protocol=enip-tcp&gateway=127.0.0.1&path=1,0&name=TestDINT&elem_count=1&elem_size=4&use_connected_msg=1`

Verify Forward Open succeeds (check connection IDs in debug log), reads/writes work through connected path, and Forward Close fires on disconnect. Test the retry scenarios (over-sized connection buffer, duplicate IDs) using `ab_server` flags if supported.

---

### Phase 10: Multi-Request Packing

**Goal**: Pack multiple CIP requests into a single EIP packet for ControlLogix, reducing round-trips.

**What to implement**:

1. **CIP Multiple Service Packet encoding** in `cip.[ch]`:

   ```c
   /* Encode CIP Multiple Service Packet (service 0x0A to Message Router) */
   Bytes cip_encode_multi_request(Arena *a, Bytes *requests, int request_count);

   /* Parse CIP Multiple Service Packet response — returns array of individual CipResponses */
   int cip_parse_multi_response(Bytes response, CipResponse *responses_out, int max_responses);
   ```

   Format: `[0x0A][path_words][class 0x02 instance 0x01][service_count:2][offsets:2*N][request_1][request_2]...`

   Each offset is relative to the start of the service data area.

2. **Modify `tickle_active_tags()`** to batch multiple tags:

   Instead of sending one CIP request per tag, collect all due tags' CIP payloads into an array, then:
   - If only 1 tag: send as single request (no packing overhead).
   - If 2+ tags: pack with `cip_encode_multi_request()`.
   - Respect max packet size — stop adding requests when the next one would exceed the buffer.

3. **Route responses back to tags** — the multi-response parsing returns individual `CipResponse` items in the same order as the requests. Match by position.

**Files to modify**:

- `src/libplctag/protocols/enip_tcp/cip.c` / `cip.h`
- `src/libplctag/protocols/enip_tcp/enip_tcp_conn.c`

**How to test**: Create 5+ tags on the same connection, trigger simultaneous reads. Verify in debug log that requests are packed (1 EIP packet for N tags instead of N packets). Verify all tags receive correct data. Test edge cases: packed request exceeds buffer → split into multiple packets.

---

### Phase 11: Auto-Sync Scheduling

**Goal**: The connection handler thread manages auto-sync read/write scheduling using the sorted `active_tags` list, matching the Modbus pattern.

**What to implement**:

1. **Auto-sync read scheduling**: When a tag is created with `auto_sync_read_ms > 0`:
   - In `enip_tcp_tag_create()`, after connection assignment, insert the tag into `conn->active_tags` with `op = READ_REQUEST` and `op_time = now + random_jitter(auto_sync_read_ms)` to spread initial reads.
   - After each successful read, reschedule: `tag->op = READ_REQUEST`, `tag->op_time = tag->auto_sync_next_read` (advance by whole multiples of the period to avoid drift — same algorithm as Modbus [modbus.c](src/libplctag/protocols/mb/modbus.c) `mb_tickler()` and the current `plc_tag_generic_tickler()` in [lib.c](src/libplctag/lib/lib.c)).

2. **Auto-sync write scheduling**: Via `enip_tcp_tag_data_written()` (already stubbed in Phase 7). When data is dirtied:
   - If tag already has a write pending, move it (debounce by resetting `op_time`).
   - If tag is idle, insert with `op = WRITE_REQUEST`, `op_time = now + auto_sync_write_ms`.

3. **Adaptive sleep**: `tickle_active_tags()` already returns the next wake time. The handler thread uses this for precise `cond_wait()` timeout instead of a fixed 100ms.

4. **The tag sets `skip_tickler = 1`** in `enip_tcp_tag_create()`. Since the ENIP-TCP protocol handles all scheduling internally, the global `tag_tickler_func` thread should never process these tags. This was already mentioned in Phase 2; ensure it's implemented.

**Files to modify**:

- `src/libplctag/protocols/enip_tcp/enip_tcp_tag.c` — auto-sync init, rescheduling
- `src/libplctag/protocols/enip_tcp/enip_tcp_conn.c` — rescheduling after completion

**How to test**: Use `test_auto_sync`-equivalent test with `protocol=enip-tcp`:

```c
int32_t tag = plc_tag_create("protocol=enip-tcp&gateway=127.0.0.1&path=1,0&name=TestDINT"
                              "&elem_count=1&elem_size=4&auto_sync_read_ms=100", 5000);
/* Wait and check that reads happen every ~100ms */
```

Monitor with callbacks that auto-sync reads fire at correct intervals. Also test auto-sync writes — set `auto_sync_write_ms=200`, write data to the tag, verify it arrives at the PLC within 200ms.

---

### Phase 12: Callbacks and Events

**Goal**: Fire tag events (`READ_COMPLETED`, `WRITE_COMPLETED`, `ABORTED`, etc.) from the connection handler thread and dispatch user callbacks correctly.

**What to implement**:

1. **Event raising** — after completing a read or write in `tickle_active_tags()`:
   
   ```c
   /* After setting read_complete = 1 or write_complete = 1: */
   tag_raise_event(tag, PLCTAG_EVENT_READ_COMPLETED, tag->status);
   /* Drop api_mutex before callback dispatch: */
   mutex_unlock(tag->api_mutex);
   plc_tag_generic_handle_event_callbacks(tag);
   ```

   Reference: The existing callback dispatch functions are in [lib.c](src/libplctag/lib/lib.c). `tag_raise_event()` stages the event (sets `event_*_status` fields). `plc_tag_generic_handle_event_callbacks()` calls the user's callback function **outside** the tag mutex.

2. **`PLCTAG_EVENT_READ_STARTED` / `PLCTAG_EVENT_WRITE_STARTED`** — fire these when the request is first sent, before waiting for the response.

3. **`PLCTAG_EVENT_ABORTED`** — fire when `enip_tcp_abort()` is called. Remove the tag from `active_tags`, reset op state.

4. **`PLCTAG_EVENT_CREATED`** — fire at the end of `enip_tcp_tag_create()` after the tag is fully initialized.

5. **`PLCTAG_EVENT_DESTROYED`** — fire in the tag destructor.

**Files to modify**:

- `src/libplctag/protocols/enip_tcp/enip_tcp_tag.c` — event firing in create/destroy/abort
- `src/libplctag/protocols/enip_tcp/enip_tcp_conn.c` — event firing in tickle_active_tags

**How to test**: Run `test_callback` and `test_callback_ex` adapted for `enip-tcp`. Verify all event types fire at the correct times.

---

### Phase 13: Error Recovery, Reconnection, Idle Disconnect

**Goal**: Handle socket errors, PLC reboots, and idle connections gracefully.

**What to implement**:

1. **Socket error during send/receive**: If `async_stream_write_exact()` or `async_stream_read_exact()` returns an error:
   - Set all in-progress tags to `PLCTAG_ERR_BAD_CONNECTION`.
   - Fire `ABORTED` events.
   - Break out of steady-state loop → falls through to cleanup → close socket → retry.

2. **Idle disconnect**: If no tags have pending operations for `connection_inactivity_timeout_ms`:
   - Clean disconnect (Forward Close + unregister + close socket).
   - Handler thread sleeps until a tag queues work (`async_wake` from `enip_tcp_read()` / `enip_tcp_write()`).
   - On wake, reconnect and resume.
   - This matches the existing AB behavior in [session.c](src/libplctag/protocols/ab/session.c) `SESSION_WAIT_IDLE_RECONNECT`.

3. **Connection status reporting**: Set `conn->connection_status` atomics at each stage so `plc_tag_get_int_attribute(tag, "connection_status", 0)` returns accurate values per the `plc_tag_conn_status_t` enum in [libplctag.h](src/libplctag/lib/libplctag.h#L423-L429).

4. **Exponential backoff on connection failure**: Already stubbed in Phase 3 handler loop. Ensure retry_count resets on successful connection.

**Files to modify**:

- `src/libplctag/protocols/enip_tcp/enip_tcp_conn.c` — error handling, idle disconnect, connection status

**How to test**:

- `test_reconnect` adapted for `enip-tcp` — kill and restart `ab_server` mid-operation.
- `test_idle_disconnect` adapted — create tag, let it idle, verify disconnect, then read to trigger reconnect.
- `test_shutdown_cip` adapted — clean shutdown while connected.

---

### Phase 14: String Handling

**Goal**: Support reading and writing ControlLogix STRING type tags.

**What to implement**:

1. **String type detection** — when a tag is created with the `string` type, set appropriate byte order metadata in `tag->byte_order` so the generic string accessor functions (`plc_tag_get_string()`, `plc_tag_set_string()`, etc.) work correctly.

   ControlLogix strings have the format: `[length:4 bytes LE][data:82 bytes max][pad:2 bytes]` = 88 bytes total.

   Reference: The existing implementation sets this up in `set_tag_byte_order()` in [eip_cip.c](src/libplctag/protocols/ab/eip_cip.c). The byte order struct fields are documented in [tag.h](src/libplctag/lib/tag.h#L78-L99).

2. **No special encoding needed** — strings are read/written as regular tag data. The CIP Read/Write Tag services treat strings as opaque byte arrays. The interpretation happens in the accessor layer.

**Files to modify**:

- `src/libplctag/protocols/enip_tcp/enip_tcp_tag.c` — byte order setup for string tags

**How to test**: Use `test_string` adapted for `enip-tcp` or:

```bash
./tag_rw2 -t string -p "protocol=enip-tcp&gateway=127.0.0.1&path=1,0&name=TestString"
```

---

### Phase 15: Tag Listing

**Goal**: Support the ControlLogix tag listing tag type (discovery of all tags in a PLC).

**What to implement**:

1. **CIP Get Instance Attribute List** encoding/decoding — used to enumerate tags. Service `0x55` to Symbol Object class `0x6B`.

2. **Continuation handling** — the PLC returns tags in batches. Each response includes a `next_id` for the next batch. The tag stays in `READ_REQUEST` state with `op_time = now` until `next_id == 0` (all tags listed).

3. **Growing buffer** — the tag's data buffer grows as listing responses arrive. Use `mem_realloc()` or allocate a large initial buffer.

4. **Create a separate vtable** for listing tags (or use a flag in the common vtable) since listing tags have different read semantics (multi-step, growing buffer).

**Reference**: The existing implementation is in [eip_cip_special.c](src/libplctag/protocols/ab/eip_cip_special.c) — `listing_tag_tickler()`, `listing_tag_read_start()`.

**Files to create/modify**:

- `src/libplctag/protocols/enip_tcp/enip_tcp_listing.c` / `.h` — listing-specific logic
- `src/libplctag/protocols/enip_tcp/cip.c` — listing CIP encoding/decoding
- `src/libplctag/protocols/enip_tcp/enip_tcp_tag.c` — dispatch listing vs regular tag creation

**How to test**: Run `list_tags_logix` adapted for `enip-tcp` against `ab_server` and real hardware. Verify the output matches the existing `ab-eip` listing output.

---

### Phase 16: UDT Type Information

**Goal**: Support reading UDT (User Defined Type) metadata for ControlLogix.

**What to implement**:

1. **Two-phase read** — UDT tags first read metadata (template attributes), then read field definitions. This is a multi-step sequence similar to tag listing.

2. **CIP services** — Get Attributes of Template Object (class `0x6C`).

**Reference**: Existing implementation in [eip_cip_special.c](src/libplctag/protocols/ab/eip_cip_special.c) — `udt_tag_read_start()`, `udt_tag_tickler()`.

**Files to modify**:

- `src/libplctag/protocols/enip_tcp/enip_tcp_tag.c` — UDT tag type
- `src/libplctag/protocols/enip_tcp/cip.c` — UDT CIP services

**How to test**: Run `test_tag_type_attribute` adapted for `enip-tcp`.

---

### Phase 17: Raw CIP and Identity Tags

**Goal**: Support raw CIP passthrough and Get Identity tags.

**What to implement**:

1. **Raw CIP tags** — the user provides raw CIP request bytes, the library sends them and returns raw response bytes. Minimal encoding — just wrap in CPF + EIP.

2. **Identity tags** — send List Identity (EIP command `0x0063`) or Get Attribute All to Identity Object.

**Reference**: Existing implementations in [eip_cip_special.c](src/libplctag/protocols/ab/eip_cip_special.c).

**Files to modify**:

- `src/libplctag/protocols/enip_tcp/enip_tcp_tag.c` — raw and identity tag types

**How to test**: `test_raw_cip` and `get_identity` adapted for `enip-tcp`.

---

### Phase 18: Attribute Support and Compatibility

**Goal**: Implement all tag attributes (`get_int_attrib`, `set_int_attrib`, `get_byte_array_attrib`) to match the existing AB implementation's attribute interface.

**What to implement**:

1. `enip_tcp_get_int_attrib()` — handle `elem_size`, `elem_count`, `size`, `read_cache_ms`, `auto_sync_read_ms`, `auto_sync_write_ms`, `connection_group_id`, `connection_status`.

2. `enip_tcp_set_int_attrib()` — handle settable attributes.

**Reference**: Existing implementation in `ab_get_int_attrib()` / `ab_set_int_attrib()` in [ab_common.c](src/libplctag/protocols/ab/ab_common.c).

**Files to modify**:

- `src/libplctag/protocols/enip_tcp/enip_tcp_tag.c`

**How to test**: `test_tag_attributes` adapted for `enip-tcp`.

---

## Risk Analysis

| Phase | Risk | Mitigation |
|-------|------|------------|
| 1 | **Low** — registration plumbing only | Existing tests verify library still works |
| 2 | **Low** — data structures, no I/O | Verify struct layout, connection sharing |
| 3 | **High** — cross-platform async I/O from scratch | Test each platform independently; start with one platform, port after |
| 4 | **Low** — straightforward encode/send/receive | `ab_server` validates EIP register |
| 5 | **Medium** — CPF + Unconnected Send has precise byte layout | Hex-dump packets and compare with Wireshark captures |
| 6 | **Medium** — first end-to-end tag operation | Test with `ab_server` first, then real hardware |
| 7 | **Low** — mirrors Phase 6 pattern | |
| 8 | **Medium** — fragmentation offset math is tricky | Compare byte-for-byte with existing AB fragmented reads |
| 9 | **High** — Forward Open has complex retry/negotiation | Port logic from existing `session.c`; test all error paths |
| 10 | **Medium** — multi-request offset table encoding | Compare with existing `pack_requests()` output |
| 11 | **Low** — ports proven Modbus pattern | Compare auto-sync timing with existing AB auto-sync |
| 12 | **Low** — mirrors Modbus event pattern | Test with callback test suite |
| 13 | **Medium** — reconnection edge cases | Stress test with `ab_server` kill/restart |
| 14 | **Low** — string format is well-documented | Compare with existing string tests |
| 15 | **Medium** — multi-step listing protocol | Compare output with existing `list_tags_logix` |
| 16–18 | **Low** — follows established patterns | |

## Dependency Graph

```text
Phase 1 (registration) ──→ Phase 2 (structs)
                              │
                              ├──→ Phase 3 (async socket + arena + bytes)
                              │       │
                              │       ├──→ Phase 4 (EIP register)
                              │       │       │
                              │       │       ├──→ Phase 5 (CPF + unconnected CIP)
                              │       │       │       │
                              │       │       │       ├──→ Phase 6 (read)
                              │       │       │       │       │
                              │       │       │       │       ├──→ Phase 7 (write)
                              │       │       │       │       │       │
                              │       │       │       │       │       ├──→ Phase 8 (fragmented r/w)
                              │       │       │       │       │       │
                              │       │       │       │       │       ├──→ Phase 11 (auto-sync)
                              │       │       │       │       │       │
                              │       │       │       │       │       ├──→ Phase 12 (callbacks)
                              │       │       │       │       │
                              │       │       │       │       ├──→ Phase 9 (Forward Open)
                              │       │       │       │               │
                              │       │       │       │               ├──→ Phase 10 (multi-request)
                              │       │       │       │
                              │       │       │       ├──→ Phase 13 (error recovery)
                              │       │       │
                              │       │       ├──→ Phase 14 (strings) [after Phase 7]
                              │       │       │
                              │       │       ├──→ Phase 15 (listing) [after Phase 6]
                              │       │       │
                              │       │       ├──→ Phase 16 (UDT) [after Phase 6]
                              │       │       │
                              │       │       ├──→ Phase 17 (raw + identity) [after Phase 5]
                              │       │
                              │       ├──→ Phase 18 (attributes) [after Phase 2]
```

**Critical path**: Phases 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8 gets to a working tag read/write. Everything else branches off.

## File Layout Summary

```text
src/libplctag/protocols/enip_tcp/
├── enip_tcp.h              # Module init/teardown, tag_create entry point
├── enip_tcp.c              # Module init/teardown implementation
├── enip_tcp_tag.h          # Tag struct, vtable, op enum
├── enip_tcp_tag.c          # Tag lifecycle, read/write/abort/status, auto-sync, callbacks
├── enip_tcp_conn.h         # Connection struct, sorted list helpers
├── enip_tcp_conn.c         # Connection find/create, handler thread, tickle_active_tags
├── async_sock.h            # Cross-platform async socket API
├── async_sock.c            # epoll/kqueue/IOCP implementation
├── arena.h                 # Arena allocator (from data_table)
├── arena.c                 # Arena allocator (from data_table)
├── bytes.h                 # Bytes struct + pack/unpack (from data_table)
├── bytes.c                 # Bytes struct + pack/unpack (from data_table)
├── cip.h                   # EIP/CIP encoding/decoding (from data_table, extended)
├── cip.c                   # EIP/CIP encoding/decoding (from data_table, extended)
├── enip_tcp_listing.h      # Listing tag type (Phase 15)
└── enip_tcp_listing.c      # Listing tag type (Phase 15)
```

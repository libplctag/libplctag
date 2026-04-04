# Plan: fiber_net IO Library + modbus_server3 (Revised)

## Overview

Build two things in `src/poc/modbus_server3/`:

1. **fiber_net** — A reusable network I/O library using yafl stackful coroutines and `poll()`/`WSAPoll()`. Provides blocking-style socket API where each connection runs in its own fiber. All buffers use `Bytes` structs (from data_table) backed by arena allocation. No `buf_t` dependency.

2. **modbus_server3** — A Modbus TCP server using fiber_net, with packet encoding/decoding via `bytes_pack()`/`bytes_unpack()`. Modeled on modbus_server's structure.

**Key design decisions:**
- Copy `arena.[ch]` and `bytes.[ch]` from `~/Projects/data_table`, modifying arena to return errors via `util_err_t` instead of panicking
- All I/O uses `Bytes` structs — no bare `uint8_t*` or `void*` for buffers
- Fiber stacks are small; buffers >~32 bytes must be arena-allocated, not stack-allocated
- Minimize TLS usage — only yafl's internal `tls_current_fiber` (already exists); fiber_net passes context explicitly via yafl's `void*` data channel
- Not backward-compatible with existing socket/buf code; this is a clean prototype

## Architecture

```
┌──────────────────────────────────────────────────────┐
│                fiber_net_run()                        │
│                                                      │
│  1. Initial-resume new fibers                        │
│  2. Build poll() array from active wait requests     │
│  3. poll()/WSAPoll() with timeout                    │
│  4. Resume fibers whose sockets fired                │
│  5. Reap completed fibers                            │
└──────────────────┬───────────────────────────────────┘
                   │
     ┌─────────────┼─────────────┐
     │             │             │
  Listener      Client 1     Client N
   Fiber         Fiber         Fiber
     │             │             │
  fiber_socket_  fiber_socket_  fiber_socket_
  accept()      recv()         send()
     │             │             │
  (suspends     (suspends     (suspends
   with          with          with
   POLLIN)       POLLIN)       POLLOUT)
```

### Suspend/Resume Protocol (same as original plan)

Fiber ↔ event loop communication via yafl's `void*` channel:
1. Fiber calls IO helper → allocates `fiber_wait_t` on stack (small: fd + events, ~8 bytes) → calls `yafl_fiber_suspend(&wait_req)`
2. Event loop receives `fiber_wait_t*`, stores it, adds fd/events to poll set
3. When poll() fires, loop calls `yafl_fiber_resume(fiber, NULL)` — NULL = "your event fired"
4. On shutdown, resume with non-NULL = cancellation signal

### Memory Model

Each fiber gets its own `Arena` (allocated at fiber creation, passed via context). The arena is reset between request/response cycles. Buffers for recv/send are allocated from the arena, not on the fiber stack.

```
Per-client fiber context:
  ├── Arena (e.g. 8KB) — reset each request cycle
  │   ├── recv buffer (Bytes, 512B from arena)
  │   ├── send buffer (Bytes, 512B from arena)
  │   └── temp pack/unpack scratch
  ├── server_ctx_t* (shared, read-mostly)
  └── stats/timing (small, can be on fiber stack)
```

## Phase 1: Foundation Files (copied + modified)

### 1a. `src/poc/modbus_server3/arena.h` / `arena.c` — Copy from `~/Projects/data_table`

**Modifications to arena.c:**
- Change `arena_init()` to return `util_err_t` and take `Arena *out` parameter (instead of returning Arena by value and calling `exit(1)`)
- Change `arena_alloc()` to return `NULL` and set an error flag on overflow instead of `exit(1)`
- Add `arena_has_error()` check or return `NULL` on failure (callers check for NULL)
- Include `err.h` for `util_err_t`
- Remove `fprintf(stderr, ...)` stats printing from `arena_free()` (or guard behind a debug flag)

### 1b. `src/poc/modbus_server3/bytes.h` / `bytes.c` — Copy from `~/Projects/data_table`

**Modifications:**
- Add `Bytes bytes_alloc(Arena *a, size_t len)` — allocates `len` bytes from arena without zeroing (fast; recv buffers will be overwritten anyway)
- Add `void bytes_zero(Bytes b)` — zeroes all data spanned by the Bytes struct (`memset(b.data, 0, b.len)`). Used only when a cleared buffer is actually needed.
- Add `bytes_is_null(Bytes b)` inline helper: `return b.data == NULL;`
- `bytes_pack()` uses arena_alloc internally; if arena returns NULL, the packed result will have NULL data — callers must check

### 1c. Reuse from `src/poc/modbus_server/` (unchanged, referenced via include path or copied)

- `err.h/c` — error codes and `util_err_str()`
- `log.h/c` + `log_modules.def` — logging (add new module entries)
- `args.h/c` — argument parsing
- `utils.h` — timing, signals
- `register_storage.h/c` + `modbus_bitarray.h` — Modbus register model

## Phase 2: fiber_net Library

### 2a. `src/poc/modbus_server3/fiber_net.h` — Network I/O API

**Types:**
```c
typedef struct fiber_net fiber_net_t;   // opaque event loop handle

#ifdef _WIN32
typedef SOCKET fiber_socket_t;          // just a file descriptor
#else
typedef int fiber_socket_t;
#endif
#define FIBER_INVALID_SOCKET ((fiber_socket_t)-1)

typedef void *(*fiber_task_fn)(void *arg);  // fiber entry (matches yafl_fiber_fn)
```

**Event Loop API:**
- `util_err_t fiber_net_create(fiber_net_t **out, size_t max_tasks, size_t stack_size, yafl_stack_flags_t flags)`
- `util_err_t fiber_net_run(fiber_net_t *net, uint32_t tick_ms)` — blocks until stopped
- `util_err_t fiber_net_stop(fiber_net_t *net)` — signal event loop to exit
- `void fiber_net_destroy(fiber_net_t **net)`

**Task/Fiber Management:**
- `util_err_t fiber_net_add_task(fiber_net_t *net, fiber_task_fn fn, void *context)` — creates fiber, initial-resumes it on next tick

**Socket API (called from within a fiber — `fiber_net_t*` passed explicitly):**
- `util_err_t fiber_socket_listen(fiber_net_t *net, fiber_socket_t *out_sock, const char *bind_ip, uint16_t port, size_t backlog)` — creates TCP server socket, binds, listens. Stores fd in `*out_sock`.
- `util_err_t fiber_socket_accept(fiber_net_t *net, fiber_socket_t listener, fiber_socket_t *out_client, size_t timeout_ms)` — suspends on POLLIN until a client connects or timeout. Returns UTIL_OK, UTIL_ETIMEOUT, etc.
- `util_err_t fiber_socket_recv(fiber_net_t *net, fiber_socket_t sock, Bytes buf, size_t timeout_ms)` — reads exactly `buf.len` bytes into `buf.data`. Loops recv()+suspend until full or timeout/error.
- `util_err_t fiber_socket_send(fiber_net_t *net, fiber_socket_t sock, Bytes buf, size_t timeout_ms)` — sends all `buf.len` bytes. Loops send()+suspend until drained or timeout/error.
- `util_err_t fiber_socket_close(fiber_net_t *net, fiber_socket_t sock)` — close fd, remove from poll set.

**Design notes:**
- `fiber_socket_t` is just a file descriptor — no per-socket heap allocation, no back-pointers
- `fiber_net_t*` is threaded through explicitly; fibers receive it via their context struct
- Timeout is tracked by recording start time and computing remaining ms for each poll() iteration
- The low-level suspend helper `fiber_net_wait(fiber_net_t *net, fiber_socket_t fd, short events, size_t timeout_ms)` is internal to fiber_net.c

### 2b. `src/poc/modbus_server3/fiber_net.c` — Implementation

**fiber_net_run() main loop** (same pattern as original plan):
1. Initial-resume newly added fibers (they run until first IO call suspends them)
2. Build `pollfd[]` array from all fds that have pending waits
3. `poll()`/`WSAPoll()` with tick_ms timeout (or shortest remaining timeout across fibers)
4. Resume fibers whose fds fired
5. Reap completed fibers (check `yafl_fiber_status() == COMPLETE`)
6. Repeat until stopped

**Internal suspend/resume flow for `fiber_socket_recv()`:**
```
fiber_socket_recv(net, sock, buf, timeout_ms):
  total_read = 0
  while total_read < buf.len:
    rc = raw_recv(sock, buf.data + total_read, buf.len - total_read)
    if rc > 0: total_read += rc; continue
    if rc == 0: return UTIL_ECLOSED
    if EAGAIN: fiber_net_wait(net, sock, POLLIN, remaining_timeout)
               if timeout expired: return UTIL_ETIMEOUT
               if cancelled: return UTIL_ECANCELLED
    else: return translate_error()
  return UTIL_OK
```

**Platform abstraction:**
- `poll()` on POSIX, `WSAPoll()` on Windows (already pattern-matched from coro_net.h)
- Raw socket creation uses standard `socket()/bind()/listen()/accept()/recv()/send()` with non-blocking mode
- `fcntl(O_NONBLOCK)` on POSIX, `ioctlsocket(FIONBIO)` on Windows
- `SO_REUSEADDR`, `TCP_NODELAY`, SIGPIPE prevention (same patterns as existing socket.c)

**Single allocation for internal arrays** (following coro_net.c pattern): per-fiber state slots, pollfd[], task_map[] in one malloc.

**Wakeup mechanism:** Self-pipe or socket pair to interrupt poll() when new tasks are added or stop is requested.

## Phase 3: Modbus Server

### 3a. `src/poc/modbus_server3/modbus_server3.c`

**Server context:**
```
server_ctx_t:
  register_storage_t *storage
  fiber_net_t *net
  volatile int running
  int64_t start_time_us
  server_stats_t stats
```

**Listener fiber** — `void *listener_fiber(void *arg)`:
```
  ctx = (listener_ctx_t *)arg
  net = ctx->server->net
  fiber_socket_t listen_fd = FIBER_INVALID_SOCKET
  fiber_socket_listen(net, &listen_fd, ctx->bind_ip, ctx->port, 128)
  
  loop while running:
    fiber_socket_t client_fd = FIBER_INVALID_SOCKET
    rc = fiber_socket_accept(net, listen_fd, &client_fd, 1000)
    if rc == UTIL_ETIMEOUT: continue
    if rc != UTIL_OK: break
    
    // Allocate client context (malloc — outlives this stack frame)
    client_ctx = malloc(sizeof(modbus_client_ctx_t))
    client_ctx->sock = client_fd
    client_ctx->server = ctx->server
    fiber_net_add_task(net, client_fiber, client_ctx)
    stats->clients_connected++
  
  fiber_socket_close(net, listen_fd)
  return NULL
```

**Client fiber** — `void *client_fiber(void *arg)`:
```
  ctx = (modbus_client_ctx_t *)arg
  net = ctx->server->net
  sock = ctx->sock
  Arena arena
  arena_init(&arena, 8192)  // per-client arena for all buffers
  
  loop:
    arena_reset(&arena)
    
    // Phase 1: Receive MBAP header (7 bytes)
    Bytes hdr_buf = bytes_alloc(&arena, MBAP_HEADER_SIZE)
    rc = fiber_socket_recv(net, sock, hdr_buf, 30000)
    if rc != UTIL_OK: break
    
    // Phase 2: Decode MBAP header using bytes_unpack
    uint16_t txn_id, proto_id, length
    uint8_t unit_id
    bytes_unpack(hdr_buf, ">HHHb", &txn_id, &proto_id, &length, &unit_id)
    // validate proto_id == 0, length in range
    
    // Phase 3: Receive PDU body (length - 1 bytes, unit_id already counted)
    size_t pdu_len = length - 1
    Bytes pdu_buf = bytes_alloc(&arena, pdu_len)
    rc = fiber_socket_recv(net, sock, pdu_buf, 30000)
    if rc != UTIL_OK: break
    
    // Phase 4: Process request → build response using bytes_pack
    uint8_t fc = pdu_buf.data[0]
    Bytes pdu_body = bytes_slice(pdu_buf, 1, pdu_len - 1)
    Bytes response_pdu = modbus_process_request_bytes(&arena, fc, pdu_body, ctx->server->storage)
    
    // Phase 5: Build MBAP response header + PDU
    uint16_t resp_len = (uint16_t)(response_pdu.len + 1)  // +1 for unit_id
    Bytes resp_hdr = bytes_pack(&arena, ">HHHb", txn_id, (uint16_t)0, resp_len, unit_id)
    Bytes response = bytes_join(&arena, resp_hdr, response_pdu)
    
    // Phase 6: Send response
    rc = fiber_socket_send(net, sock, response, 30000)
    if rc != UTIL_OK: break
    
    // Update stats
  
  fiber_socket_close(net, sock)
  arena_free(&arena)
  free(ctx)
  return NULL
```

### 3b. `src/poc/modbus_server3/modbus_protocol3.h` / `modbus_protocol3.c` — Bytes-based Modbus processing

Rewrite of the protocol layer using `bytes_pack()`/`bytes_unpack()` instead of `buf_t`:

- `Bytes modbus_process_request_bytes(Arena *a, uint8_t fc, Bytes request_body, register_storage_t *storage)` — dispatches by function code, returns response PDU (including FC byte)
- Internal handlers for each FC (read coils, read holding registers, write single register, etc.) — each uses `bytes_unpack()` to parse the request and `bytes_pack()` to build the response
- Exception responses built with `bytes_pack(a, ">BB", fc | 0x80, exception_code)`

Reuses `register_storage_t` from modbus_server for actual register access.

### 3c. `src/poc/modbus_server3/main()` structure

Same overall flow as modbus_server.c:
1. Parse args (listen addresses, debug level, register counts) — reuse args.h/c
2. Create register storage — reuse register_storage.h/c
3. `fiber_net_create()`
4. For each listen address: allocate listener_ctx, `fiber_net_add_task(net, listener_fiber, ctx)`
5. `fiber_net_run(net, 50)` — blocks
6. Print statistics, cleanup

## Phase 4: Build System

### 4a. `src/poc/modbus_server3/CMakeLists.txt`

```
set(YAFL_DIR "$ENV{HOME}/Projects/yafl")
add_subdirectory(${YAFL_DIR} ${CMAKE_CURRENT_BINARY_DIR}/yafl)

set(MODBUS_SERVER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../modbus_server)

set(SOURCES
    modbus_server3.c
    modbus_protocol3.c
    fiber_net.c
    arena.c
    bytes.c
    ${MODBUS_SERVER_SRC}/register_storage.c
    ${MODBUS_SERVER_SRC}/err.c
    ${MODBUS_SERVER_SRC}/log.c
    ${MODBUS_SERVER_SRC}/args.c
    ${MODBUS_SERVER_SRC}/utils.c
)

add_executable(modbus_server3 ${SOURCES})
target_include_directories(modbus_server3 PRIVATE ${MODBUS_SERVER_SRC} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(modbus_server3 yafl)
# Platform: m, rt (Linux), ws2_32 (Windows)
```

### 4b. `src/poc/CMakeLists.txt` — Add `add_subdirectory("modbus_server3")`

### 4c. `src/poc/modbus_server/log_modules.def` — Add new entries:
```
LOG_MODULE_ENTRY(FIBER_NET,              14)
LOG_MODULE_ENTRY(MODBUS3_CLIENT,         15)
LOG_MODULE_ENTRY(MODBUS3_LISTENER,       16)
```

## Phase 5: Test Script

### 5a. `src/tests/scripts/run_modbus3_tests.sh`

Copy `src/tests/scripts/run_modbus2_tests.sh` and change `modbus_server2` → `modbus_server3` references.

## Files Summary

**New files to create (in `src/poc/modbus_server3/`):**
- `arena.h` — copied from data_table, modified for err.h
- `arena.c` — copied from data_table, modified to return errors
- `bytes.h` — copied from data_table, add `bytes_alloc()`, `bytes_zero()`, `bytes_is_null()`
- `bytes.c` — copied from data_table, implement `bytes_alloc()` and `bytes_zero()`
- `fiber_net.h` — new reusable fiber-based network API
- `fiber_net.c` — new event loop + socket implementation
- `modbus_protocol3.h` — Bytes-based Modbus protocol processing
- `modbus_protocol3.c` — Bytes-based Modbus protocol implementation
- `modbus_server3.c` — server main + fiber handlers
- `CMakeLists.txt` — build configuration

**Files to modify:**
- `src/poc/CMakeLists.txt` — add subdirectory
- `src/poc/modbus_server/log_modules.def` — add 3 module entries

**Files to copy + adapt:**
- `src/tests/scripts/run_modbus2_tests.sh` → `src/tests/scripts/run_modbus3_tests.sh` — change server executable name

**Existing files reused unchanged (via include path):**
- `src/poc/modbus_server/err.h/c`
- `src/poc/modbus_server/log.h/c`
- `src/poc/modbus_server/args.h/c`
- `src/poc/modbus_server/utils.h`
- `src/poc/modbus_server/register_storage.h/c`
- `src/poc/modbus_server/modbus_bitarray.h`
- `~/Projects/yafl` (via add_subdirectory)

## Implementation Order

1. Copy + modify `arena.[ch]` and `bytes.[ch]` (*parallel with step 2*)
2. Add log module entries to `log_modules.def`
3. Create `fiber_net.h` — define the API
4. Create `fiber_net.c` — implement event loop, socket ops (*depends on 1, 3*)
5. Create `modbus_protocol3.h/c` — Bytes-based protocol layer (*depends on 1*)
6. Create `modbus_server3.c` — main + fibers (*depends on 4, 5*)
7. Create `CMakeLists.txt`, update parent CMakeLists.txt (*depends on 6*)
8. Copy `src/tests/scripts/run_modbus2_tests.sh` → `src/tests/scripts/run_modbus3_tests.sh`, change `modbus_server2` → `modbus_server3` references

## Verification

1. Build: `cd build && cmake -DCMAKE_BUILD_TYPE=Debug .. && cmake --build . --target modbus_server3`
2. Manual smoke test: `./bin_dist/modbus_server3 --listen=127.0.0.1:5020 --debug=DETAIL` + Modbus TCP client
3. Run automated test suite: `bash src/tests/scripts/run_modbus3_tests.sh build/bin_dist`
4. Verify multiple simultaneous connections work
5. Verify graceful shutdown on Ctrl+C (statistics printed, all fibers cleaned up)
6. Compare behavior against modbus_server and modbus_server2

## Design Constraints

- **No buf_t** — all buffers are `Bytes` structs backed by arena allocation
- **No bare uint8_t*/void* for buffers** — only `Bytes`
- **No large stack allocations in fibers** — anything >~32 bytes must come from arena
- **Minimal TLS** — only yafl's internal `tls_current_fiber`; fiber_net passes all context explicitly via void* args and fiber_socket_t back-pointers
- **Arena errors via util_err_t** — no exit()/panic on OOM
- **Not backward-compatible** — clean prototype, no reuse of socket.h/c or buf.h/c

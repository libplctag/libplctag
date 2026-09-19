# Socket Layering Design

Status: design, not yet implemented. Scope limited to the three programs that ship:
the core library, `ab_server`, and `modbus_server` (the CI simulator, which currently
lives under `src/poc`). Other POC servers are out of scope.

Related: `docs/deferred_fixes.md` §2.4.

## 1. Problem

Three socket APIs exist, with `socket_read`/`socket_write`/`socket_close`/`socket_accept`
colliding by name across them with different signatures and return conventions.

| API | Handle | Buffer | Errors | Compiled into | Lines |
|---|---|---|---|---|---|
| `platform.c` | opaque `sock_p` `{fd, wake_read_fd, wake_write_fd, port}` | `uint8_t *` + len | `PLCTAG_ERR_*` | libplctag | ~1,100 × 2 platforms |
| `tools/ab_server/socket.[ch]` | bare `SOCKET` | `slice_s` | negative `err_t` | `ab_server` | 78 + 422 |
| `poc/utils/socket.[ch]` | `socket_t` (= fd) | `buf_t` | `util_err_t` | `modbus_server`, `scan_eip_network` | 335 + 1009 |

Duplicated three times over: byte-identical copies of the third API lived at
`src/tests/utils/socket.[ch]` (compiled by **nothing**) and `src/poc/modbus_server/socket.c`.
Both are gone as of step 1; the line counts above are as they were before that.

### What is actually shared

Only three things, and each is written two or three times:

1. **The EAGAIN + `select()` + retry loop** — `platform.c:651`/`782`,
   `ab_server/socket.c:324`/`382`, and `poc/utils/socket.c`'s `send_buf`/`recv_buf`.
2. **Socket setup boilerplate** — non-blocking, `SO_REUSEADDR`, `TCP_NODELAY`,
   `SO_NOSIGPIPE`, `SO_LINGER`, winsock init, `INVALID_SOCKET`.
3. **The self-connected-TCP wake pair** — `platform.c`'s Windows
   `sock_create_event_wakeup_channel()` (socket/bind/getsockname/listen/connect/accept)
   and `coro_net.c:80-130`, which build the same thing. POSIX `platform.c` uses
   `socketpair()` instead, so the two platform halves do not even agree with each other.

Everything else differs for real reasons: `slice_s` vs `buf_t` vs `uint8_t *`,
thread-per-connection vs coroutine loop, three unrelated error enums. Forcing those to
converge is what has always made this look larger than a consolidation. This design does
not converge them.

### What is not used

- **`socket_wait_event()` has three call sites (`modbus.c:1221`, `1379`, `1393`) and
  `socket_wake()` has two (`modbus.c:901`, `1463`).** All five are in `modbus.c`. The
  thirteen other `wake_plc_thread()` calls funnel into `1463`, and the `wake_plc` vtable
  slot is `NULL` in every AB and Omron vtable — only `modbus.c:400` sets it. AB
  `session.c` and Omron `conn.c` block in `socket_read`/`socket_write` with a timeout and
  sleep on their nap; they have no way to be woken out of a socket wait and nothing tries
  to.

  They still *pay* for the mechanism: `socket_create()` builds a wake pair for every
  socket unconditionally, so the library carries one wake pair per socket and all but the
  modbus ones are never used. The event mask and the wake channel are a modbus-only
  mechanism whose cost is charged to every connection in the library.
- **`ab_server`'s `socket_open_tcp_client()`** has no caller outside its own `socket.c`.
- **The UDP surface of `poc/utils/socket.c`** (`create_udp`, `create_udp_server`,
  `sendto_buf`, `sendtov_buf`, `recvfrom_buf`, `set_broadcast`) is used only by
  `scan_eip_network`, not by `modbus_server`. It stays where it is — see §5.

## 2. The future model changes the answer

The target is a few threads each driving many sockets, with per-connection state machines
(protothread-style coroutines or switch/case dispatch). Today everything except
`modbus_server` is one thread per socket.

Two consequences, and they decide the whole design:

**Waiting must move out of the transfer calls.** Every layer's `read`/`write` currently
hides a `select()` inside itself. That is the thread-per-socket model expressed as an API:
a call that sleeps is a call that owns its thread. In the target model transfer calls never
sleep, and all waiting happens once per loop iteration for the whole thread. The
EAGAIN+select core is therefore *not* the shared primitive to extract — it is the
compatibility shim, and it shrinks as callers migrate.

**The wake channel belongs to the poller, not to the socket.** `struct sock_t` carries its
own wake pair, so N connections cost 3N descriptors and N wake pairs whether or not they
are ever woken — today only the modbus ones are. One thread driving 200 sockets needs
exactly one wake pair.

## 3. L0 — `src/utils/socket_fd.[ch]`

Non-blocking operations on a bare handle. No buffer type, no timeouts, no opinions.

```c
/* handle + setup */
socket_fd_t socket_fd_open_tcp(void);
int32_t socket_fd_close(socket_fd_t fd);
int32_t socket_fd_set_nonblocking(socket_fd_t fd, bool on);   /* nodelay, reuseaddr, nosigpipe, linger alongside */

/* client */
int32_t socket_fd_connect_start(socket_fd_t fd, const char *host, int32_t port);
int32_t socket_fd_connect_check(socket_fd_t fd);              /* OK | WOULD_BLOCK | error */

/* server */
int32_t socket_fd_bind_listen(socket_fd_t fd, const char *host, int32_t port, int32_t backlog);
int32_t socket_fd_accept(socket_fd_t listener, socket_fd_t *out_client);   /* OK | WOULD_BLOCK */

/* transfer — never sleeps; a partial transfer is normal, not an error */
int32_t socket_fd_recv(socket_fd_t fd, uint8_t *buf, int32_t len, int32_t *out_count);
int32_t socket_fd_send(socket_fd_t fd, const uint8_t *buf, int32_t len, int32_t *out_count);

/* UDP, for the adapter that needs it */
int32_t socket_fd_recvfrom(socket_fd_t fd, uint8_t *buf, int32_t len, socket_fd_addr_t *from, int32_t *out_count);
int32_t socket_fd_sendto(socket_fd_t fd, const uint8_t *buf, int32_t len, const socket_fd_addr_t *to, int32_t *out_count);
```

No timeout parameter anywhere. Every call returns immediately with `PLCTAG_STATUS_OK`, a
would-block status, or an error.

This is the entire portability surface — winsock init, `INVALID_SOCKET`,
`EAGAIN`/`WSAEWOULDBLOCK`, `EINPROGRESS`/`WSAEINPROGRESS`, the socket options — and it is
the only file in the socket stack with a `#ifdef _WIN32` in it. Roughly 300 lines
replacing three copies.

## 4. L0b — `src/utils/poller.[ch]`

```c
typedef struct poller_t *poller_p;

int32_t poller_create(poller_p *p);          /* creates the thread's one wake pair */
int32_t poller_destroy(poller_p *p);

int32_t poller_add(poller_p p, socket_fd_t fd, int32_t events, void *context);
int32_t poller_modify(poller_p p, socket_fd_t fd, int32_t events);
int32_t poller_remove(poller_p p, socket_fd_t fd);

typedef struct {
    socket_fd_t fd;
    int32_t events;
    void *context;
} poller_event_t;

int32_t poller_wait(poller_p p, poller_event_t *out, int32_t max, int32_t timeout_ms, int32_t *out_count);
int32_t poller_wake(poller_p p);             /* safe to call from any thread */
```

`void *context` is the integration point with per-connection state machines: it is the
connection object. `poller_wait()` returns "these connections are ready, here they are";
the loop calls each one's step function. Protothreads, switch/case dispatch and coroutines
sit on top of this identically — the poller never learns which was chosen.

**Backend: `poll()` / `WSAPoll()`, not `select()`.** Both platforms have it, and
`select()`'s `FD_SETSIZE` ceiling of 1024 is a wall that would be hit precisely in the
many-sockets model this is for. `epoll`/`kqueue` can replace the backend later without
touching the interface. Do not build them now.

The existing event bits (`SOCK_EVENT_CAN_READ`, `CAN_WRITE`, `CONNECT`, `DISCONNECT`,
`ERROR`, `TIMEOUT`, `WAKE_UP`) carry over verbatim. They are a reasonable readiness
vocabulary and keeping them means `modbus.c` does not churn.

## 5. L1 — per-program adapters

Each program keeps its own buffer type and error enum. These are not defects to be
unified; they are three programs with different lifetimes making different choices.

| Program | Adapter | Keeps |
|---|---|---|
| library | `src/utils/socket.[ch]` — today's `sock_p` API | `uint8_t *`, `PLCTAG_ERR_*` |
| `ab_server` | `src/tools/ab_server/socket.[ch]` | `slice_s`, `err_t` |
| `modbus_server` | `src/poc/utils/socket.[ch]` | `buf_t`, `util_err_t`, **and the UDP functions** |

`src/poc/utils/socket.[ch]` keeps its full API including the UDP half, so
`scan_eip_network` continues to build unchanged. It loses only its platform code, which
moves to L0.

Each adapter also carries a **`_timeout` shim** — `recv(fd, buf, len, timeout_ms)`
implemented as a poller-of-one plus `socket_fd_recv()`. This is what lets today's
thread-per-socket callers compile with zero edits. It is explicitly transitional: every
caller that moves to a shared poller deletes one use of it, and the shim goes away when
the last one does.

## 6. L2 — concurrency models stay separate

`socket_wait_event` for the library, `coro_net` for `modbus_server`, threads for
`ab_server`. These are different concurrency models, not different spellings of one thing.
Nothing to share here.

### Where each program lands

**`ab_server`** — thread per connection, `select()` inside each read and write. Adopts the
timeout shim with no behavior change. If it is later multiplexed, one poller plus a
per-session state machine replaces the `do/while` in `tcp_server.c:191-260`.

**`modbus_server`** — already in the target shape. `coro_net` becomes an L2 over `poller`,
losing its own self-connected-TCP wake pair (`coro_net.c:80-130`) and its socket setup.
This is the one program the new API *fits* rather than tolerates, which makes it the
reference implementation for the other two.

**Library** — `session.c` and `conn.c` are pure thread-per-socket with timeout reads; they
take the shim and do not change. `modbus.c` is the only code already asking readiness
questions, and it asks them one socket at a time, so it is the natural first migration:
one poller per PLC thread, then one poller per group of PLCs, at which point
`plc_cleanup_nap` and the wake pipe merge into `poller_wake()`.

### `socket_wait_event()`

Do not carry it into `src/utils/` unchanged. It is "poll one socket plus its private wake
pipe" — the degenerate case of `poller_wait()`, with a per-socket wake pair the target
model does not want. Give it the poller shape, and keep a one-socket convenience wrapper
for `modbus.c` until it migrates.

## 7. Order

1. **Dead code — DONE 2026-09-10.** Deleted `src/tests/utils/socket.[ch]` and
   `coro_net.[ch]` (compiled by nothing) and `ab_server`'s `socket_open_tcp_client()`.
   `modbus_server` now builds against `../utils`. That last one had to take all seven
   duplicated file pairs, not just `socket.c`: the headers use `#pragma once`, so a
   translation unit reaching `err.h` through both directories redefines every enumerator.
   `modbus_server2`/`3` pick up the shared copies through a new `${MODBUS_UTILS_SRC}`. Clean
   build, simulator suite 181/181.
2. **Round 6 — DONE 2026-09-10.** The library's sockets moved to `src/utils/socket.[ch]`,
   zero call-site edits. *Not* the same recipe as the thread, atomic, spinlock, mutex and nap
   rounds: those folded both platforms into shared public functions over a thin static seam,
   which is wrong here because only ~56% of the two socket bodies match and the divergence
   runs through the middle of every function. `socket.c` carries both bodies whole under one
   top-level `#ifdef _WIN32`; the real merge is step 3's job, below. This does **not** empty
   `platform.h` — `sleep_ms`/`time_ms` and the packing macros remain. Sockets now log under their own
   `PLCTAG_MODULE_SOCKET`.
3. **L0 + poller — DONE 2026-09-19.** `src/utils/socket_fd.[ch]` and `src/utils/poller.[ch]`
   are in, built into the library, and covered by `src/tests/unit/test_poller.c`. The first
   consumer is `src/poc/modbus_server_poller`, a fork of `src/tools/modbus_server` rewritten
   onto them; the original is untouched and is still what the test suite runs. Forking rather
   than converting is what let the poller be shaped by a real caller without putting the
   suite's only Modbus server at risk.
4. **Adapters onto L0**, one program at a time, each keeping its own types.
5. **Migrate readiness**: `modbus.c` first, then whichever server is to be multiplexed.

Steps 1, 2 and 3 are done. Steps 4 and 5 only pay off if the many-sockets-per-thread model
actually happens. The condition this document put on step 3 — hold until a real caller exists
— was met by writing that caller as the fork rather than by waiting for one.

## 8. Expected result

~2,600 lines of socket code become ~300 shared (L0) + ~150 (poller) + ~350 of adapters,
with no consumer changing its call shape and one file holding every `#ifdef _WIN32` in the
socket stack.

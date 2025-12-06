# pt_net.c Implementation Plan

## Overview

Implement `src/tests/pt_net/pt_net.c` - a protothread-based network I/O library that replaces the existing reactor.c and socket.c with a simpler, stackless coroutine-based approach.

## Key Requirements

- Cross-platform: Linux, Windows, BSD, macOS
- Use poll() on POSIX, WSAPoll() on Windows
- Maximize code sharing between platforms
- Zero runtime allocation (static sizing)
- Integrate with existing utils: buf.h, err.h, utils.h
- Support the API defined in pt_net.h

## Thread State vs Exit Status

**Important distinction:**

Protothread functions return `util_err_t`, but this serves two purposes:

1. **UTIL_EAGAIN** = "I yielded, still running"
   - The thread is NOT done
   - The reactor checks `thread->state` to see WHY it yielded:
     - `PT_NET_THREAD_WAITING_IO` - waiting for socket I/O
     - `PT_NET_THREAD_WAITING_TIMER` - waiting for timer expiry
     - `PT_NET_THREAD_SUSPENDED` - explicitly suspended
   - The try functions (pt_net_try_recv_tcp, pt_net_try_sleep, etc.) set these states before returning UTIL_EAGAIN

2. **Any other util_err_t** = "I'm done, here's my exit status"
   - The thread completed
   - The reactor stores this in `thread->exit_status`
   - Common exit codes:
     - `UTIL_OK` - completed successfully
     - `UTIL_ECLOSED` - connection closed
     - `UTIL_ECANCELLED` - thread was cancelled
     - `UTIL_ETIMEOUT` - operation timed out
     - Any other error that caused early termination

**Example flow:**
```c
// Thread calls pt_net_recv_tcp() which calls pt_net_try_recv_tcp()
util_err_t pt_net_try_recv_tcp(buf_t *buf, pt_net_socket_t *sock, ...) {
    // Try to recv
    if (would_block) {
        // Set state BEFORE returning UTIL_EAGAIN
        this_pt->state = PT_NET_THREAD_WAITING_IO;
        this_pt->waiting_socket = sock;
        this_pt->wait_type = PT_NET_WAIT_READ;
        return UTIL_EAGAIN;  // Reactor will poll() and wake us later
    }

    if (recv_error) {
        return UTIL_EIO;  // Thread exits with error
    }

    return UTIL_OK;  // Success, continue execution
}
```

## Implementation Phases

### Phase 1: Core Data Structures and Platform Abstraction

**File**: `src/tests/pt_net/pt_net.c`

**Platform Detection**:
```c
// Reuse pattern from socket.c
#if defined(__APPLE__) || defined(__FreeBSD__) || ...
#define PT_NET_BSD
#endif

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET pt_net_socket_fd_t;
#else
  #include <poll.h>
  typedef int pt_net_socket_fd_t;
#endif
```

**Core Structures**:
```c
struct pt_net_core_s {
    // Socket pool
    pt_net_socket_s *sockets;
    size_t socket_count;
    size_t socket_capacity;

    // Protothread pool
    pt_net_thread_s *threads;
    size_t thread_count;
    size_t thread_capacity;

    // Poll arrays (rebuilt each iteration)
    struct pollfd *pollfds;
    size_t pollfd_count;
    size_t pollfd_capacity;

    pt_net_thread_t *timer_threads;

    // Control
    bool shutdown;
};

struct pt_net_socket_s {
    pt_net_socket_fd_t fd;
    pt_net_socket_type_t type;  // TCP_LISTENER, TCP_CLIENT, UDP

    pt_net_core_t *core;

    // Framer for TCP
    pt_net_framer_fn framer;
    void *framer_context;

    // Receive buffer (for framing)
    uint8_t *recv_buf_data;
    buf_t recv_buf;
    size_t recv_buf_capacity;

    // Wait queue - threads waiting on this socket
    pt_net_thread_s *waiting_threads;

    // Socket state
    bool nonblocking;
    bool connected;  // For TCP clients
    pt_net_addr_t local_addr;
    pt_net_addr_t remote_addr;
};

struct pt_net_thread_s {
    // Protothread state
    int line;  // Continuation point (__LINE__ from PT_NET_YIELD)
    bool suspended;
    bool cancelled;

    pt_net_core_t *core;

    // Function and context
    util_err_t (*func)(pt_net_thread_t *, void *);
    void *context;
    const char *name;

    // State
    pt_net_thread_state_t state;  // RUNNABLE, WAITING_IO, WAITING_TIMER, SUSPENDED, COMPLETED
    util_err_t exit_status;

    // What this thread is waiting on
    pt_net_socket_s *waiting_socket;
    pt_net_wait_type_t wait_type;  // READ, WRITE, ACCEPT

    // Timer (for pt_net_sleep or pt_net_with_timeout)
    uint64_t timer_expiry_ms;  // 0 if no timer

    // Linked list for wait queues
    pt_net_thread_s *next_waiting;
};
```

### Phase 2: Socket Operations (Non-blocking)

Implement all `pt_net_try_*` functions that perform actual syscalls:

**TCP Functions**:

- `pt_net_open_tcp_listener()` - socket(), bind(), listen(), set nonblocking
- `pt_net_try_accept_tcp()` - accept(), set nonblocking, return UTIL_EAGAIN if would block
- `pt_net_try_connect_tcp()` - socket(), connect (non-blocking), handle EINPROGRESS
- `pt_net_try_recv_tcp()` - recv() with framer logic
- `pt_net_try_send_tcp()` - send()
- `pt_net_close_tcp()` - close()

**UDP Functions**:
- `pt_net_open_udp()` - socket(), bind()
- `pt_net_try_recvfrom_udp()` - recvfrom()
- `pt_net_try_sendto_udp()` - sendto()
- `pt_net_udp_enable_broadcast()` - setsockopt(SO_BROADCAST)
- `pt_net_udp_join_multicast()` - setsockopt(IP_ADD_MEMBERSHIP)
- `pt_net_close_udp()` - close()

**Address Functions**:
- `pt_net_init_addr()` - parse IP string, set port
- `pt_net_get_local_socket_addr()` - getsockname()
- `pt_net_get_remote_socket_addr()` - getpeername()

**Cross-platform patterns** (from socket.c):
- SIGPIPE prevention (SO_NOSIGPIPE on BSD, MSG_NOSIGNAL on Linux)
- Error code conversion (util_err_from_errno, util_err_from_wsa)
- Nonblocking mode (fcntl O_NONBLOCK on POSIX, ioctlsocket on Windows)

### Phase 3: Protothread Management

**Thread Lifecycle**:
```c
util_err_t pt_net_spawn_thread(
    pt_net_thread_t **pt,
    pt_net_core_t *core,
    util_err_t (*func)(pt_net_thread_t *, void *),
    void *context,
    const char *name
) {
    // Find free slot in thread pool
    // Initialize thread structure
    // Set state = RUNNABLE
    // Call func() once to start execution
    return UTIL_OK;
}

/**
 * NOTE: The macros in pt_net.h should directly access thread fields instead
 * of calling helper functions. This avoids unnecessary function call overhead.
 *
 * PT_NET_FUNC_BODY_START should use:
 *     switch (this_pt->line) { case 0:
 *
 * PT_NET_YIELD should use:
 *     this_pt->line = __LINE__;
 *
 * pt_net_suspend_thread should use:
 *     this_pt->suspended = true;
 *     this_pt->state = PT_NET_THREAD_SUSPENDED;
 *
 * DO NOT implement these as separate functions:
 *   - pt_net_thread_get_line()
 *   - pt_net_thread_set_line()
 *   - pt_net_thread_set_suspended()
 */
```

**Thread Coordination**:

- `pt_net_wake_thread()` - set state to RUNNABLE
- `pt_net_cancel_thread()` - set cancelled flag
- `pt_net_is_cancelled()` - check flag
- `pt_net_try_join_thread()` - check if target completed, yield if not

### Phase 4: Reactor Loop (Core)

**Main loop** (based on reactor.c pattern):
```c

util_err_t pt_net_core_run(pt_net_core_t *core) {
    while (!core->shutdown) {
        // 1. Build pollfd array
        build_pollfds(core);

        // 2. Calculate poll timeout
        int timeout_ms = calculate_next_timeout(core);  // Based on timers

        // Clamp timeout to maximum 100ms to ensure responsiveness
        if (timeout_ms > 100 || timeout_ms < 0) {
            timeout_ms = 100;
        }

        // 3. Poll for events
        #ifdef _WIN32
        int ret = WSAPoll(core->pollfds, core->pollfd_count, timeout_ms);
        #else
        int ret = poll(core->pollfds, core->pollfd_count, timeout_ms);
        #endif

        // 4. Resume threads that got I/O events
        resume_io_waiters(core);

        // 5. Resume threads with expired timers
        resume_timer_waiters(core);

        // 6. Clear poll array (will rebuild next iteration)
        core->pollfd_count = 0;

        // 7. Run all runnable threads
        run_all_runnable(core);
    }
    return UTIL_OK;
}
```

**build_pollfds()** logic:

- Iterate through sockets
- For each socket, check if any threads are waiting on it
- If yes, add to pollfds array with appropriate events (POLLIN/POLLOUT)
- Track mapping from pollfd index back to socket

**resume_io_waiters()** logic:

- Iterate through pollfds array with revents != 0
- Find socket for each active pollfd
- Find threads waiting on that socket
- Mark threads as RUNNABLE
- Clear wait state

**run_all_runnable()** logic:
```c
static void run_all_runnable(pt_net_core_t *core) {
    for (size_t i = 0; i < core->thread_count; i++) {
        pt_net_thread_t *pt = &core->threads[i];

        if (pt->state != PT_NET_THREAD_RUNNABLE) {
            continue;
        }

        // Call the protothread function
        util_err_t err = pt->func(pt, pt->context);

        if (err == UTIL_EAGAIN) {
            // Thread yielded - it registered what it's waiting for
            continue;
        }

        // Thread completed (returned non-EAGAIN)
        pt->state = PT_NET_THREAD_COMPLETED;
        pt->exit_status = err;
    }
}
```

### Phase 5: Timer Support

**Timer Management**:

Instead of a separate timer heap, use a linked list of threads waiting on timers.
The `pt_net_core_t` already has `pt_net_thread_t *timer_threads` for this purpose.
Each thread has its own expiry time in the `timer_expiry_ms` field.

**Timer Functions**:

- `pt_net_try_sleep(pt_net_thread_t *this_pt, uint32_t ms)`
  - Calculate expiry time: `this_pt->timer_expiry_ms = util_get_time_ms() + ms`
  - Set thread state to WAITING_TIMER
  - Add thread to core->timer_threads linked list
  - Return UTIL_EAGAIN to trigger yield

**DO NOT implement these functions** (they were for a pt_net_with_timeout macro that we're not adding):
- `pt_net_timer_start()`
- `pt_net_timer_expired()`
- `pt_net_timer_cancel()`

**calculate_next_timeout()**:

```c
static int calculate_next_timeout(pt_net_core_t *core) {
    if (!core->timer_threads) {
        return INT_MAX;  // No timers, will be clamped to 100ms
    }

    uint64_t now = util_get_time_ms();
    int min_timeout = INT_MAX;

    // Walk the timer_threads list
    for (pt_net_thread_t *pt = core->timer_threads; pt != NULL; pt = pt->next_waiting) {
        if (pt->timer_expiry_ms <= now) {
            return 0;  // At least one timer already expired
        }

        int timeout = (int)(pt->timer_expiry_ms - now);
        if (timeout < min_timeout) {
            min_timeout = timeout;
        }
    }

    return min_timeout;
}
```

**resume_timer_waiters()**:

```c
static void resume_timer_waiters(pt_net_core_t *core) {
    uint64_t now = util_get_time_ms();
    pt_net_thread_t **pp = &core->timer_threads;

    while (*pp != NULL) {
        pt_net_thread_t *pt = *pp;

        if (pt->timer_expiry_ms <= now) {
            // Timer expired - remove from list and mark runnable
            *pp = pt->next_waiting;
            pt->next_waiting = NULL;
            pt->timer_expiry_ms = 0;
            pt->state = PT_NET_THREAD_RUNNABLE;
        } else {
            // Not expired yet, keep in list
            pp = &pt->next_waiting;
        }
    }
}
```

### Phase 6: Integration and Testing


**WSA Initialization**:
```c
util_err_t pt_net_core_create(pt_net_core_t **core, size_t max_socks, size_t max_pts) {
    #ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
    #endif

    // Allocate core structure and pools
    // Initialize all structures
}

void pt_net_core_shutdown(pt_net_core_t *core) {
    #ifdef _WIN32
    WSACleanup();
    #endif
    // Free resources
}
```

**CMakeLists.txt**:

```cmake
add_executable(pt_net_test
    pt_net_test.c
    pt_net.c
    ../utils/buf.c
    ../utils/err.c
    ../utils/utils.c
)

if(WIN32)
    target_link_libraries(pt_net_test ws2_32)
endif()
```

## Code Reuse from Existing Files

**From socket.c** (adapt, don't copy):

- Platform detection macros
- WSAStartup/WSACleanup pattern
- Setting nonblocking mode
- SIGPIPE prevention
- inet_pton usage for address parsing
- Error conversion (util_err_from_errno/wsa)

**From reactor.c** (adapt, don't copy):

- poll()/WSAPoll() call pattern
- pollfd array management
- EINTR handling on POSIX
- Poll event interpretation (POLLIN, POLLOUT, POLLERR, POLLHUP)

**From utils/** (use directly):

- buf.h/buf.c for all buffer operations
- err.h/err.c for error codes
- utils.h/utils.c for util_get_time_ms()

## Key Design Decisions

1. **Single-threaded**: No mutexes needed, simpler than reactor.c
2. **Cooperative multitasking**: Protothreads must yield explicitly
3. **Static allocation**: All pools pre-sized at core_create()
4. **Level-triggered polling**: Simpler than edge-triggered, poll() naturally level-triggered
5. **Per-socket receive buffers**: For framing support
6. **Timer linked list**: Threads waiting on timers are kept in a linked list (core->timer_threads)

## Critical Files

- **Create**: `src/tests/pt_net/pt_net.c` (~2000 lines estimated)
- **Update**: `src/tests/pt_net/pt_net.h` (remove timer helper functions, update macros to use direct field access)
- **Reference**: `src/tests/utils/socket.c` (cross-platform patterns)
- **Reference**: `src/tests/utils/reactor.c` (poll loop pattern)
- **Use**: `src/tests/utils/buf.h`, `err.h`, `utils.h`

## Required Changes to pt_net.h

**Fix type name in pt_net_open_tcp_listener** (line 280):
Change `pt_addr_t *addr` to `pt_net_addr_t *addr`

**Remove these function declarations** (lines 652-655):
```c
void pt_net_timer_start(pt_net_thread_t *pt, uint32_t ms);
bool pt_net_timer_expired(pt_net_thread_t *pt);
void pt_net_timer_cancel(pt_net_thread_t *pt);
```

**Remove these function declarations** (lines 636-638):
```c
int pt_net_thread_get_line(pt_net_thread_t *pt);
void pt_net_thread_set_line(pt_net_thread_t *pt, int line);
void pt_net_thread_set_suspended(pt_net_thread_t *pt, bool suspended);
```

**Update PT_NET_FUNC_BODY_START** (line 121):
```c
#define PT_NET_FUNC_BODY_START \
    switch (this_pt->line) { \
    case 0:
```

**Update PT_NET_YIELD** (line 144):
```c
#define PT_NET_YIELD() \
    do { \
        this_pt->line = __LINE__; \
        return UTIL_EAGAIN; \
        case __LINE__:; \
    } while (0)
```

**Update pt_net_suspend_thread** (line 576):
```c
#define pt_net_suspend_thread(this_pt) \
    do { \
        this_pt->suspended = true; \
        this_pt->state = PT_NET_THREAD_SUSPENDED; \
        PT_NET_YIELD(); \
    } while (0)
```

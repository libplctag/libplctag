# Thread-Like Fiber API Design

**Date:** 2026-01-21
**Goal:** Provide thread-like programming model using fibers (fcontext)

---

## Design Philosophy

Users should write code that **looks like threaded code**:

```c
void connection_handler(void *arg) {
    socket_t sock = (socket_t)(intptr_t)arg;

    // Just normal blocking I/O code!
    uint8_t buf[512];
    ssize_t n = ev_recv(sock, buf, sizeof(buf), 0);

    process_request(buf, n);

    ev_send(sock, response, response_len, 0);
}
```

**No manual yields, no scheduler awareness, no macros.**

Under the hood:
- Each handler runs on its own fiber (stack)
- `ev_recv()` automatically yields when would block
- Scheduler resumes when data available
- Looks synchronous, actually async

---

## Core API

### Fiber Management

```c
/**
 * @brief Fiber entry point signature.
 *
 * Identical to pthread_start_routine.
 */
typedef void (*ev_fiber_func_t)(void *arg);

/**
 * @brief Create a new fiber.
 *
 * Similar to pthread_create but non-preemptive.
 *
 * @param loop Event loop to run in
 * @param func Fiber entry point
 * @param arg Argument passed to func
 * @return UTIL_OK on success, error on failure
 */
util_err_t ev_fiber_create(ev_loop_t *loop,
                           ev_fiber_func_t func,
                           void *arg);

/**
 * @brief Exit current fiber.
 *
 * Similar to pthread_exit. Cleans up and never returns.
 */
void ev_fiber_exit(void) __attribute__((noreturn));

/**
 * @brief Yield to other fibers.
 *
 * Similar to sched_yield() - allows other ready fibers to run.
 */
void ev_yield(void);
```

### Blocking I/O (Thread-Like)

```c
/**
 * @brief Receive data (blocks fiber until data available).
 *
 * Identical semantics to POSIX recv(). Fiber yields if would block.
 *
 * @return Bytes received, 0 on EOF, -1 on error (errno set)
 */
ssize_t ev_recv(socket_t sockfd, void *buf, size_t len, int flags);

/**
 * @brief Send data (blocks fiber until can send).
 *
 * Identical semantics to POSIX send(). May return partial write.
 *
 * @return Bytes sent, -1 on error (errno set)
 */
ssize_t ev_send(socket_t sockfd, const void *buf, size_t len, int flags);

/**
 * @brief Receive exactly n bytes (blocks until complete or error).
 *
 * Helper that loops until all bytes received.
 *
 * @return UTIL_OK on success, error on failure
 */
util_err_t ev_recv_exact(socket_t sockfd, void *buf, size_t len);

/**
 * @brief Send all bytes (blocks until complete or error).
 *
 * Helper that loops until all bytes sent.
 *
 * @return UTIL_OK on success, error on failure
 */
util_err_t ev_send_all(socket_t sockfd, const void *buf, size_t len);

/**
 * @brief Accept connection (blocks until client connects).
 *
 * Identical semantics to POSIX accept().
 *
 * @return Client socket on success, -1 on error (errno set)
 */
socket_t ev_accept(socket_t sockfd, struct sockaddr *addr, socklen_t *addrlen);

/**
 * @brief Connect to remote host (blocks until connected).
 *
 * Identical semantics to POSIX connect().
 *
 * @return 0 on success, -1 on error (errno set)
 */
int ev_connect(socket_t sockfd, const struct sockaddr *addr, socklen_t addrlen);

/**
 * @brief Close socket.
 *
 * Identical to POSIX close(). Not async - closes immediately.
 */
int ev_close(socket_t sockfd);
```

### Timing

```c
/**
 * @brief Sleep (blocks fiber for specified duration).
 *
 * Similar to usleep() but in milliseconds.
 */
void ev_sleep(uint32_t milliseconds);
```

### Buffer Integration (Optional Convenience)

```c
/**
 * @brief Receive into read buffer (blocks until n bytes available).
 *
 * Convenience wrapper around ev_recv_exact + buffer append.
 *
 * @return UTIL_OK on success, error on failure
 */
util_err_t ev_recv_buf(socket_t sockfd, ev_read_buf_t *buf, size_t n);

/**
 * @brief Send from write buffer (blocks until all sent).
 *
 * Convenience wrapper around ev_send_all + buffer consumption.
 *
 * @return UTIL_OK on success, error on failure
 */
util_err_t ev_send_buf(socket_t sockfd, ev_write_buf_t *buf);

/**
 * @brief Receive until frame complete (blocks until checker says complete).
 *
 * @return UTIL_OK on success, error on failure
 */
util_err_t ev_recv_frame(socket_t sockfd,
                         ev_read_buf_t *buf,
                         ev_frame_checker_t checker,
                         void *checker_ctx);
```

---

## Implementation

### Internal Fiber Structure

```c
typedef struct ev_fiber_s {
    fcontext_t fctx;           // fcontext handle
    void *stack_base;          // Stack allocation
    size_t stack_size;

    enum {
        FIBER_READY,
        FIBER_RUNNING,
        FIBER_BLOCKED,
        FIBER_DEAD
    } state;

    ev_loop_t *loop;

    // Blocking state
    socket_t blocked_fd;
    short blocked_events;
    uint64_t blocked_until_ms;  // For ev_sleep

    // Error state
    int saved_errno;
} ev_fiber_t;
```

### Automatic Yielding in I/O Functions

```c
// ev_io.c

// Internal: get real recv function
#ifdef __APPLE__
    // On macOS, use weak symbols
    extern ssize_t recv(int, void *, size_t, int) __attribute__((weak));
    #define REAL_RECV recv
#else
    // On Linux, link with --wrap
    extern ssize_t __real_recv(int, void *, size_t, int);
    #define REAL_RECV __real_recv
#endif

ssize_t ev_recv(socket_t sockfd, void *buf, size_t len, int flags) {
    ev_fiber_t *fiber = ev_fiber_current();

    if (!fiber) {
        // Not in a fiber context - just call real recv
        return REAL_RECV(sockfd, buf, len, flags);
    }

    // Ensure socket is non-blocking
    ev_socket_set_nonblocking(sockfd);

    while (1) {
        // Try non-blocking recv
        ssize_t n = REAL_RECV(sockfd, buf, len, flags | MSG_DONTWAIT);

        if (n >= 0) {
            return n;  // Success
        }

        if (errno == EINTR) {
            continue;  // Interrupted, retry
        }

        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;  // Real error
        }

        // Would block - yield to scheduler
        fiber->state = FIBER_BLOCKED;
        fiber->blocked_fd = sockfd;
        fiber->blocked_events = POLLIN;
        fiber->saved_errno = 0;

        ev_fiber_yield_to_scheduler();

        // Resumed - check for error during block
        if (fiber->saved_errno != 0) {
            errno = fiber->saved_errno;
            return -1;
        }

        // Try again
    }
}

ssize_t ev_send(socket_t sockfd, const void *buf, size_t len, int flags) {
    ev_fiber_t *fiber = ev_fiber_current();

    if (!fiber) {
        return REAL_SEND(sockfd, buf, len, flags);
    }

    ev_socket_set_nonblocking(sockfd);

    while (1) {
        ssize_t n = REAL_SEND(sockfd, buf, len, flags | MSG_DONTWAIT);

        if (n >= 0) {
            return n;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }

        // Would block
        fiber->state = FIBER_BLOCKED;
        fiber->blocked_fd = sockfd;
        fiber->blocked_events = POLLOUT;

        ev_fiber_yield_to_scheduler();

        if (fiber->saved_errno != 0) {
            errno = fiber->saved_errno;
            return -1;
        }
    }
}

util_err_t ev_recv_exact(socket_t sockfd, void *buf, size_t len) {
    uint8_t *p = buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = ev_recv(sockfd, p, remaining, 0);

        if (n < 0) {
            return util_err_from_errno(errno);
        }

        if (n == 0) {
            return UTIL_ECLOSED;
        }

        p += n;
        remaining -= n;
    }

    return UTIL_OK;
}

util_err_t ev_send_all(socket_t sockfd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = ev_send(sockfd, p, remaining, 0);

        if (n < 0) {
            return util_err_from_errno(errno);
        }

        p += n;
        remaining -= n;
    }

    return UTIL_OK;
}

void ev_sleep(uint32_t milliseconds) {
    ev_fiber_t *fiber = ev_fiber_current();

    if (!fiber) {
        // Not in fiber, use real sleep
        usleep(milliseconds * 1000);
        return;
    }

    // Block until time expires
    fiber->state = FIBER_BLOCKED;
    fiber->blocked_fd = -1;  // Not waiting on socket
    fiber->blocked_until_ms = ev_time_ms() + milliseconds;

    ev_fiber_yield_to_scheduler();
}
```

---

## Example: Modbus TCP Handler

```c
#include <ev.h>

void modbus_connection(void *arg) {
    socket_t sock = (socket_t)(intptr_t)arg;

    uint8_t recv_data[512];
    uint8_t send_data[512];
    ev_read_buf_t recv_buf = ev_read_buf_init(recv_data, sizeof(recv_data));
    ev_write_buf_t send_buf = ev_write_buf_init(send_data, sizeof(send_data));

    while (1) {
        // Receive MBAP header (7 bytes)
        if (ev_recv_exact(sock, recv_data, 7) != UTIL_OK) {
            break;  // Connection closed or error
        }
        ev_read_buf_append_commit(&recv_buf, 7);

        // Parse header
        uint16_t length;
        ev_read_buf_decode_u16(&recv_buf, EV_BIG_ENDIAN, &length);
        // ... decode rest of header

        // Receive PDU
        if (ev_recv_exact(sock, recv_data + 7, length - 1) != UTIL_OK) {
            break;
        }
        ev_read_buf_append_commit(&recv_buf, length - 1);

        // Process request
        process_modbus_request(&recv_buf, &send_buf);

        // Send response
        util_err_t err = ev_send_buf(sock, &send_buf);
        if (err != UTIL_OK) {
            break;
        }

        // Reset for next request
        ev_read_buf_reset(&recv_buf);
        ev_write_buf_reset(&send_buf);
    }

    ev_close(sock);
}

void modbus_acceptor(void *arg) {
    socket_t listen_sock = (socket_t)(intptr_t)arg;
    ev_loop_t *loop = ev_loop_current();

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        // Block until client connects
        socket_t client_sock = ev_accept(listen_sock,
                                         (struct sockaddr *)&client_addr,
                                         &addr_len);

        if (client_sock < 0) {
            continue;  // Accept failed
        }

        // Spawn fiber for this connection
        ev_fiber_create(loop, modbus_connection, (void *)(intptr_t)client_sock);
    }
}

int main() {
    ev_loop_t *loop = ev_loop_create(1000);

    // Create listening socket (regular POSIX call)
    socket_t listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    // ... bind, listen

    // Start acceptor fiber
    ev_fiber_create(loop, modbus_acceptor, (void *)(intptr_t)listen_sock);

    // Run event loop
    ev_loop_run(loop, 100);

    return 0;
}
```

**This is indistinguishable from threaded code!**

---

## Comparison: Threads vs Fibers

### With Threads (pthread)

```c
void *connection_handler(void *arg) {
    socket_t sock = (socket_t)(intptr_t)arg;

    uint8_t buf[512];
    ssize_t n = recv(sock, buf, sizeof(buf), 0);  // Blocks thread

    process(buf, n);

    send(sock, response, response_len, 0);  // Blocks thread

    close(sock);
    return NULL;
}

pthread_create(&thread, NULL, connection_handler, (void *)(intptr_t)sock);
```

### With Our Fibers

```c
void connection_handler(void *arg) {
    socket_t sock = (socket_t)(intptr_t)arg;

    uint8_t buf[512];
    ssize_t n = ev_recv(sock, buf, sizeof(buf), 0);  // Blocks fiber

    process(buf, n);

    ev_send(sock, response, response_len, 0);  // Blocks fiber

    ev_close(sock);
}

ev_fiber_create(loop, connection_handler, (void *)(intptr_t)sock);
```

**Almost identical!** Just change `recv` → `ev_recv`, `send` → `ev_send`, `close` → `ev_close`.

---

## Advantages Over Symmetric Fibers

### Symmetric Fibers (raw fcontext)

```c
void handler(fcontext_transfer_t t) {
    // Must manually track context
    fcontext_t scheduler = t.prev_context;

    // Want to receive?
    recv_request_t req = { .sock = sock, .buf = buf, .len = 512 };
    t = jump_fcontext(scheduler, &req);

    // Manually extract result
    recv_result_t *result = t.data;
    ssize_t n = result->n;

    // Too low-level!
}
```

### Thread-Like API

```c
void handler(void *arg) {
    socket_t sock = (socket_t)(intptr_t)arg;

    uint8_t buf[512];
    ssize_t n = ev_recv(sock, buf, 512, 0);

    // Just works!
}
```

**Much cleaner and more intuitive.**

---

## Disadvantages vs Real Threads

| Feature | Threads | Fibers |
|---------|---------|--------|
| Preemption | Yes | No (cooperative) |
| Blocking system calls | Work | Must use ev_* versions |
| CPU-bound work | Parallelizes | Blocks event loop |
| Stack size | Large (MB) | Small (8KB) |
| Context switch | Slow (syscall) | Fast (just registers) |
| Max concurrent | 100s | 1000s |

**Trade-off:** Lose preemption, gain scalability and performance.

---

## Migration from Threaded Code

### Before (pthread)

```c
void *worker(void *arg) {
    while (1) {
        socket_t sock = accept(listen_sock, NULL, NULL);

        uint8_t buf[512];
        recv(sock, buf, sizeof(buf), 0);

        process(buf);

        send(sock, response, response_len, 0);
        close(sock);
    }
}

for (int i = 0; i < 10; i++) {
    pthread_create(&threads[i], NULL, worker, NULL);
}
```

### After (fibers)

```c
void worker(void *arg) {
    while (1) {
        socket_t sock = ev_accept(listen_sock, NULL, NULL);

        uint8_t buf[512];
        ev_recv(sock, buf, sizeof(buf), 0);

        process(buf);

        ev_send(sock, response, response_len, 0);
        ev_close(sock);
    }
}

ev_fiber_create(loop, worker, NULL);  // Just one fiber handles all!
```

**Minimal code changes, huge scalability improvement.**

---

## Recommendation

**Provide thread-like API (`ev_recv`, `ev_send`, etc.) wrapping fibers (fcontext).**

**Why:**
1. **Familiar** - looks like threaded code everyone knows
2. **Simple** - no manual context switching
3. **Scalable** - fibers are lightweight (8KB vs MB)
4. **Fast** - no syscalls for context switch
5. **Safe** - cooperative (no race conditions)

**Implementation:**
- Use fcontext for stack switching (~900 lines)
- Wrap in thread-like API (~500 lines)
- Total: ~1400 lines for complete system

This gives you the best of both worlds:
- Programming model of threads (familiar, simple)
- Performance of async I/O (scalable, fast)

# Stackful Coroutine Implementation Design

**Date:** 2026-01-21
**Based on:** Measured stack usage <4KB per coroutine

---

## Memory Analysis

### Measured Reality
- Actual stack usage: <4KB per coroutine
- Proposed allocation: 8KB stack + 4KB guard = 12KB virtual per coroutine
- Actual RSS (resident memory): ~4KB per active coroutine

### Scaling
| Connections | Virtual Alloc | Resident (RSS) | vs Stack Copying |
|-------------|---------------|----------------|------------------|
| 10          | 120KB         | ~40KB          | ~20KB saved      |
| 100         | 1.2MB         | ~400KB         | ~200KB saved     |
| 500         | 6MB           | ~2MB           | ~1MB saved       |
| 1000        | 12MB          | ~4MB           | ~2MB saved       |

**Conclusion:** Memory savings from stack copying are minimal compared to complexity cost.

---

## API Design

### Core Types

```c
/**
 * @brief Coroutine task.
 */
typedef struct ev_coro_s ev_coro_t;

/**
 * @brief Coroutine entry point.
 *
 * @param arg User-provided context
 */
typedef void (*ev_coro_func_t)(void *arg);

/**
 * @brief Coroutine state.
 */
typedef enum {
    EV_CORO_READY,      /* Ready to run */
    EV_CORO_RUNNING,    /* Currently executing */
    EV_CORO_BLOCKED,    /* Blocked on I/O */
    EV_CORO_DEAD        /* Finished execution */
} ev_coro_state_t;
```

### Lifecycle

```c
/**
 * @brief Create a new coroutine.
 *
 * @param loop Event loop to run in
 * @param func Coroutine entry point
 * @param arg User context
 * @param stack_size Stack size (0 for default 8KB)
 * @param out_coro Receives created coroutine
 * @return UTIL_OK on success, error code on failure
 */
util_err_t ev_coro_create(ev_loop_t *loop,
                          ev_coro_func_t func,
                          void *arg,
                          size_t stack_size,
                          ev_coro_t **out_coro);

/**
 * @brief Destroy a coroutine.
 *
 * Coroutine must be in DEAD state.
 *
 * @param coro Coroutine to destroy
 */
void ev_coro_destroy(ev_coro_t **coro);

/**
 * @brief Get coroutine state.
 */
ev_coro_state_t ev_coro_get_state(ev_coro_t *coro);

/**
 * @brief Get current coroutine.
 *
 * @return Current coroutine, or NULL if not in coroutine context
 */
ev_coro_t *ev_coro_current(void);
```

### I/O Operations (Blocking from Coroutine Perspective)

```c
/**
 * @brief Receive exactly n bytes.
 *
 * Blocks the coroutine until n bytes are received or error occurs.
 *
 * @param sock Socket to receive from
 * @param buf Buffer to receive into
 * @param n Number of bytes to receive
 * @return UTIL_OK on success, error code on failure
 */
util_err_t ev_coro_recv_exact(socket_t sock, ev_read_buf_t *buf, size_t n);

/**
 * @brief Receive until frame complete.
 *
 * @param sock Socket to receive from
 * @param buf Buffer to receive into
 * @param checker Frame checker callback
 * @param ctx Context for checker
 * @return UTIL_OK on success, error code on failure
 */
util_err_t ev_coro_recv_frame(socket_t sock,
                              ev_read_buf_t *buf,
                              ev_frame_checker_t checker,
                              void *ctx);

/**
 * @brief Send all bytes from buffer.
 *
 * Blocks until all bytes sent or error occurs.
 *
 * @param sock Socket to send to
 * @param buf Buffer to send from
 * @return UTIL_OK on success, error code on failure
 */
util_err_t ev_coro_send_all(socket_t sock, ev_write_buf_t *buf);

/**
 * @brief Accept a connection.
 *
 * Blocks until connection arrives.
 *
 * @param listen_sock Listening socket
 * @param out_sock Receives accepted socket
 * @param out_addr Receives client address (optional)
 * @return UTIL_OK on success, error code on failure
 */
util_err_t ev_coro_accept(socket_t listen_sock,
                          socket_t *out_sock,
                          struct sockaddr_in *out_addr);

/**
 * @brief Connect to remote host.
 *
 * Blocks until connected or timeout.
 *
 * @param sock Socket (must be non-blocking)
 * @param addr Address to connect to
 * @param timeout_ms Timeout in milliseconds
 * @return UTIL_OK on success, error code on failure
 */
util_err_t ev_coro_connect(socket_t sock,
                           const struct sockaddr_in *addr,
                           uint32_t timeout_ms);

/**
 * @brief Sleep for specified duration.
 *
 * @param ms Milliseconds to sleep
 */
void ev_coro_sleep(uint32_t ms);

/**
 * @brief Yield to other coroutines.
 *
 * Allows other ready coroutines to run before resuming.
 */
void ev_coro_yield(void);
```

---

## Usage Examples

### Example 1: Modbus TCP Server

```c
/* Per-connection coroutine - normal C code! */
void modbus_connection(void *arg) {
    socket_t sock = (socket_t)(intptr_t)arg;

    uint8_t recv_data[512];
    uint8_t send_data[512];
    ev_read_buf_t recv_buf = ev_read_buf_init(recv_data, sizeof(recv_data));
    ev_write_buf_t send_buf = ev_write_buf_init(send_data, sizeof(send_data));

    ev_frame_length_prefixed_cfg_t frame_cfg = {
        .length_offset = 4,
        .length_size = 2,
        .length_endian = EV_BIG_ENDIAN,
        .header_size = 6,
        .length_includes_header = false,
        .max_frame_size = 512
    };

    while (1) {
        /* Just normal blocking-style code! */
        util_err_t err = ev_coro_recv_frame(sock, &recv_buf,
                                            ev_frame_check_length_prefixed,
                                            &frame_cfg);
        if (err != UTIL_OK) {
            break;  /* Connection closed or error */
        }

        /* Decode header - normal function calls */
        modbus_header_t hdr;
        if (!decode_modbus_header(&recv_buf, &hdr)) {
            break;
        }

        /* Process request - normal function call */
        process_modbus_request(&hdr, &recv_buf, &send_buf);

        /* Send response - blocking */
        err = ev_coro_send_all(sock, &send_buf);
        if (err != UTIL_OK) {
            break;
        }

        /* Reset for next request */
        ev_read_buf_reset(&recv_buf);
        ev_write_buf_reset(&send_buf);
    }

    ev_socket_close(sock);
}

/* Accept coroutine */
void modbus_acceptor(void *arg) {
    socket_t listen_sock = (socket_t)(intptr_t)arg;
    ev_loop_t *loop = ev_loop_current();

    while (1) {
        socket_t client_sock;
        struct sockaddr_in client_addr;

        /* Block until connection */
        util_err_t err = ev_coro_accept(listen_sock, &client_sock, &client_addr);
        if (err != UTIL_OK) {
            continue;
        }

        /* Spawn coroutine for this connection */
        ev_coro_t *conn_coro;
        err = ev_coro_create(loop, modbus_connection,
                            (void *)(intptr_t)client_sock,
                            0,  /* default stack size */
                            &conn_coro);

        if (err != UTIL_OK) {
            ev_socket_close(client_sock);
        }
    }
}

int main(int argc, char *argv[]) {
    /* Create event loop */
    ev_loop_t *loop = ev_loop_create(1000);

    /* Create listening socket */
    socket_t listen_sock;
    ev_socket_opts_t opts = EV_SOCKET_OPTS_DEFAULT;
    ev_socket_create_tcp_server("0.0.0.0", 502, 10, &opts, &listen_sock);

    /* Start acceptor coroutine */
    ev_coro_t *acceptor;
    ev_coro_create(loop, modbus_acceptor,
                   (void *)(intptr_t)listen_sock,
                   0, &acceptor);

    /* Run event loop */
    ev_loop_run(loop, 100);

    return 0;
}
```

**Compare to protothreads:** This is ~40 lines of clean, readable code vs ~100 lines with manual state management.

### Example 2: Helper Functions That Just Work

```c
/* Helper function - can call blocking I/O and return values! */
eip_header_t read_eip_header(socket_t sock, ev_read_buf_t *buf) {
    /* Block until we have 24 bytes */
    ev_coro_recv_exact(sock, buf, 24);

    /* Decode header */
    eip_header_t hdr;
    ev_read_buf_decode_u16(buf, EV_LITTLE_ENDIAN, &hdr.command);
    ev_read_buf_decode_u16(buf, EV_LITTLE_ENDIAN, &hdr.length);
    ev_read_buf_decode_u32(buf, EV_LITTLE_ENDIAN, &hdr.session_handle);
    ev_read_buf_decode_u32(buf, EV_LITTLE_ENDIAN, &hdr.status);
    ev_read_buf_decode_u64(buf, EV_LITTLE_ENDIAN, &hdr.sender_context);
    ev_read_buf_decode_u32(buf, EV_LITTLE_ENDIAN, &hdr.options);

    return hdr;  /* Normal return! */
}

cpf_header_t read_cpf_header(socket_t sock, ev_read_buf_t *buf) {
    /* ... similar ... */
    return cpf;
}

void handle_enip_connection(void *arg) {
    socket_t sock = (socket_t)(intptr_t)arg;
    ev_read_buf_t buf = ...;

    /* Just call helper functions! */
    eip_header_t eip = read_eip_header(sock, &buf);
    cpf_header_t cpf = read_cpf_header(sock, &buf);

    /* Process based on headers */
    if (eip.command == ENIP_CMD_SEND_RR_DATA) {
        handle_rr_data(&eip, &cpf, sock, &buf);
    }
}
```

**This is the huge win** - normal function decomposition with normal returns!

---

## Platform-Specific Implementation

### Linux/macOS (POSIX)

```c
#include <ucontext.h>
#include <sys/mman.h>

struct ev_coro_s {
    ucontext_t context;
    void *stack_base;
    size_t stack_size;
    ev_coro_state_t state;
    ev_loop_t *loop;
    socket_t blocked_on_fd;
    short blocked_on_events;
    util_err_t error;
};

util_err_t ev_coro_create(ev_loop_t *loop,
                          ev_coro_func_t func,
                          void *arg,
                          size_t stack_size,
                          ev_coro_t **out_coro) {
    if (stack_size == 0) {
        stack_size = 8 * 1024;  /* 8KB default */
    }

    size_t guard_size = 4 * 1024;  /* 4KB guard */
    size_t total_size = stack_size + guard_size;

    ev_coro_t *coro = calloc(1, sizeof(ev_coro_t));
    if (!coro) {
        return UTIL_ENOMEM;
    }

    /* Allocate stack with guard page */
    coro->stack_base = mmap(NULL, total_size,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (coro->stack_base == MAP_FAILED) {
        free(coro);
        return UTIL_ENOMEM;
    }

    /* Protect guard page */
    if (mprotect(coro->stack_base, guard_size, PROT_NONE) != 0) {
        munmap(coro->stack_base, total_size);
        free(coro);
        return UTIL_EINTERNAL;
    }

    coro->stack_size = total_size;
    coro->state = EV_CORO_READY;
    coro->loop = loop;
    coro->blocked_on_fd = -1;

    /* Initialize context */
    if (getcontext(&coro->context) != 0) {
        munmap(coro->stack_base, total_size);
        free(coro);
        return UTIL_EINTERNAL;
    }

    coro->context.uc_stack.ss_sp = (char *)coro->stack_base + guard_size;
    coro->context.uc_stack.ss_size = stack_size;
    coro->context.uc_link = NULL;

    makecontext(&coro->context, (void (*)(void))coro_entry_wrapper, 2, func, arg);

    /* Add to event loop's ready queue */
    ev_loop_add_coro(loop, coro);

    *out_coro = coro;
    return UTIL_OK;
}

void ev_coro_destroy(ev_coro_t **coro) {
    if (!coro || !*coro) return;

    ev_coro_t *c = *coro;

    if (c->stack_base) {
        munmap(c->stack_base, c->stack_size);
    }

    free(c);
    *coro = NULL;
}

/* Internal: wrapper to handle coroutine completion */
static void coro_entry_wrapper(ev_coro_func_t func, void *arg) {
    ev_coro_t *current = ev_coro_current();

    /* Run user function */
    func(arg);

    /* Mark as dead */
    current->state = EV_CORO_DEAD;

    /* Yield back to scheduler (never returns) */
    ev_coro_yield_to_scheduler();
}

/* Blocking recv implementation */
util_err_t ev_coro_recv_exact(socket_t sock, ev_read_buf_t *buf, size_t n) {
    ev_coro_t *current = ev_coro_current();

    while (ev_read_buf_available(buf) < n) {
        /* Try non-blocking recv */
        ssize_t received = recv(sock,
                               ev_read_buf_append_ptr(buf),
                               ev_read_buf_space(buf),
                               MSG_DONTWAIT);

        if (received > 0) {
            ev_read_buf_append_commit(buf, received);
            continue;
        }

        if (received == 0) {
            return UTIL_ECLOSED;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* Block this coroutine on socket read */
            current->state = EV_CORO_BLOCKED;
            current->blocked_on_fd = sock;
            current->blocked_on_events = POLLIN;

            /* Yield to scheduler */
            ev_coro_yield_to_scheduler();

            /* When we resume, check for errors */
            if (current->error != UTIL_OK) {
                return current->error;
            }

            /* Try again */
            continue;
        }

        /* Other error */
        return util_err_from_errno(errno);
    }

    return UTIL_OK;
}

/* Internal: yield to scheduler */
void ev_coro_yield_to_scheduler(void) {
    ev_coro_t *current = ev_coro_current();
    ev_loop_t *loop = current->loop;

    /* Save current context and switch to scheduler */
    swapcontext(&current->context, &loop->scheduler_context);
}
```

### Windows

```c
#include <windows.h>

struct ev_coro_s {
    LPVOID fiber;
    ev_coro_state_t state;
    ev_loop_t *loop;
    /* ... same as POSIX ... */
};

util_err_t ev_coro_create(ev_loop_t *loop,
                          ev_coro_func_t func,
                          void *arg,
                          size_t stack_size,
                          ev_coro_t **out_coro) {
    if (stack_size == 0) {
        stack_size = 8 * 1024;
    }

    ev_coro_t *coro = calloc(1, sizeof(ev_coro_t));
    if (!coro) {
        return UTIL_ENOMEM;
    }

    /* Create fiber with stack */
    coro->fiber = CreateFiberEx(stack_size,  /* commit size */
                                stack_size,   /* reserve size */
                                0,            /* flags */
                                (LPFIBER_START_ROUTINE)coro_entry_wrapper,
                                coro);

    if (!coro->fiber) {
        free(coro);
        return UTIL_EINTERNAL;
    }

    coro->state = EV_CORO_READY;
    coro->loop = loop;

    ev_loop_add_coro(loop, coro);

    *out_coro = coro;
    return UTIL_OK;
}

void ev_coro_yield_to_scheduler(void) {
    ev_coro_t *current = ev_coro_current();
    ev_loop_t *loop = current->loop;

    SwitchToFiber(loop->scheduler_fiber);
}
```

### FreeRTOS

```c
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

struct ev_coro_s {
    TaskHandle_t task_handle;
    SemaphoreHandle_t resume_sem;
    ev_coro_state_t state;
    /* ... */
};

util_err_t ev_coro_create(ev_loop_t *loop,
                          ev_coro_func_t func,
                          void *arg,
                          size_t stack_size,
                          ev_coro_t **out_coro) {
    if (stack_size == 0) {
        stack_size = 2048;  /* FreeRTOS typically uses smaller stacks */
    }

    ev_coro_t *coro = pvPortMalloc(sizeof(ev_coro_t));
    if (!coro) {
        return UTIL_ENOMEM;
    }

    coro->resume_sem = xSemaphoreCreateBinary();
    if (!coro->resume_sem) {
        vPortFree(coro);
        return UTIL_ENOMEM;
    }

    BaseType_t result = xTaskCreate(
        coro_task_wrapper,
        "coro",
        stack_size / sizeof(StackType_t),
        coro,
        tskIDLE_PRIORITY + 1,
        &coro->task_handle
    );

    if (result != pdPASS) {
        vSemaphoreDelete(coro->resume_sem);
        vPortFree(coro);
        return UTIL_ENOMEM;
    }

    coro->state = EV_CORO_READY;
    coro->loop = loop;

    *out_coro = coro;
    return UTIL_OK;
}

void ev_coro_yield_to_scheduler(void) {
    ev_coro_t *current = ev_coro_current();

    /* Suspend until scheduler resumes us */
    xSemaphoreTake(current->resume_sem, portMAX_DELAY);
}
```

---

## Event Loop Integration

The event loop needs to:
1. Maintain queues of coroutines (ready, blocked)
2. Poll for I/O events
3. Resume coroutines when their events occur

```c
struct ev_loop_s {
    /* Scheduler context (POSIX only) */
    ucontext_t scheduler_context;

    /* Coroutine queues */
    ev_coro_t **ready_queue;
    size_t ready_count;
    size_t ready_capacity;

    ev_coro_t **blocked_queue;
    size_t blocked_count;
    size_t blocked_capacity;

    /* Poll array */
    struct pollfd *pollfds;
    ev_coro_t **poll_coros;  /* Maps pollfd index to coroutine */
    size_t poll_count;

    /* Currently running coroutine */
    ev_coro_t *current;

    /* Wake pipe (for ev_loop_stop, etc) */
    socket_t wake_fds[2];

    bool running;
};

util_err_t ev_loop_poll(ev_loop_t *loop, uint32_t timeout_ms) {
    /* Build pollfd array from blocked queue */
    loop->poll_count = 0;
    for (size_t i = 0; i < loop->blocked_count; i++) {
        ev_coro_t *coro = loop->blocked_queue[i];
        if (coro->blocked_on_fd != -1) {
            loop->pollfds[loop->poll_count].fd = coro->blocked_on_fd;
            loop->pollfds[loop->poll_count].events = coro->blocked_on_events;
            loop->pollfds[loop->poll_count].revents = 0;
            loop->poll_coros[loop->poll_count] = coro;
            loop->poll_count++;
        }
    }

    /* Add wake pipe */
    loop->pollfds[loop->poll_count].fd = loop->wake_fds[0];
    loop->pollfds[loop->poll_count].events = POLLIN;
    loop->poll_count++;

    /* Poll */
    int ready = poll(loop->pollfds, loop->poll_count, timeout_ms);

    if (ready > 0) {
        /* Move ready coroutines to ready queue */
        for (size_t i = 0; i < loop->poll_count - 1; i++) {
            if (loop->pollfds[i].revents) {
                ev_coro_t *coro = loop->poll_coros[i];
                coro->state = EV_CORO_READY;
                coro->blocked_on_fd = -1;
                ev_loop_move_to_ready(loop, coro);
            }
        }
    }

    /* Run ready coroutines */
    while (loop->ready_count > 0) {
        ev_coro_t *coro = loop->ready_queue[0];
        ev_loop_remove_from_ready(loop, 0);

        if (coro->state == EV_CORO_DEAD) {
            ev_coro_destroy(&coro);
            continue;
        }

        /* Resume coroutine */
        loop->current = coro;
        coro->state = EV_CORO_RUNNING;

#ifdef _WIN32
        SwitchToFiber(coro->fiber);
#else
        swapcontext(&loop->scheduler_context, &coro->context);
#endif

        loop->current = NULL;

        /* Coroutine yielded back - check state */
        if (coro->state == EV_CORO_BLOCKED) {
            ev_loop_add_to_blocked(loop, coro);
        } else if (coro->state == EV_CORO_DEAD) {
            ev_coro_destroy(&coro);
        }
    }

    return UTIL_OK;
}
```

---

## Stack Overflow Detection

With guard pages, stack overflow produces a clear segfault:

```
Program received signal SIGSEGV, Segmentation fault.
0x00007ffff7a12345 in modbus_connection () at modbus.c:42
42          uint8_t big_array[10000];  /* Oops, too big for stack */

(gdb) bt
#0  0x00007ffff7a12345 in modbus_connection () at modbus.c:42
#1  0x00007ffff7a10000 in coro_entry_wrapper ()
#2  0x0000000000000000 in ?? ()
```

The backtrace clearly shows which coroutine overflowed its stack.

To be extra safe, can also fill stacks with canary pattern and check:

```c
void ev_coro_check_stack_usage(ev_coro_t *coro) {
    uint8_t *stack = (uint8_t *)coro->stack_base + GUARD_SIZE;
    size_t unused = 0;

    for (size_t i = 0; i < coro->stack_size - GUARD_SIZE; i++) {
        if (stack[i] == 0xCC) {
            unused++;
        } else {
            break;
        }
    }

    size_t used = coro->stack_size - GUARD_SIZE - unused;
    size_t peak = (used * 100) / (coro->stack_size - GUARD_SIZE);

    if (peak > 75) {
        EV_LOG(EV_LOG_WARN, "Coroutine using %zu%% of stack (%zu/%zu bytes)",
               peak, used, coro->stack_size - GUARD_SIZE);
    }
}
```

---

## Code Size Estimate

| Component | Lines | Notes |
|-----------|-------|-------|
| Core API | 150 | Types, lifecycle, accessors |
| POSIX implementation | 300 | ucontext, mmap, swapcontext |
| Windows implementation | 250 | Fibers API |
| FreeRTOS implementation | 200 | Tasks + semaphores |
| Event loop integration | 200 | Scheduler, queues, poll |
| I/O helpers | 150 | recv_exact, send_all, etc |
| **Total per platform** | **~1050** | Choose one platform impl |

This is very reasonable for the functionality gain.

---

## Migration Path

### Phase 1: Implement POSIX version
- Linux/macOS support
- Test with Modbus server example
- Measure actual memory usage

### Phase 2: Add Windows support
- Fibers implementation
- Test on Windows

### Phase 3: Optional RTOS support
- FreeRTOS or Zephyr
- For embedded targets

### Phase 4: Deprecate protothreads
- Keep for backward compatibility
- New code uses stackful coroutines

---

## Recommendation

**Implement stackful coroutines with 8KB stacks + 4KB guard pages.**

**Rationale:**
- Your measurements show <4KB actual usage
- 8KB gives 2× safety margin
- Guard pages catch overflows immediately
- Resident memory is ~400KB for 100 connections
- Code quality improvement is enormous
- Normal C functions with normal returns
- Platform implementations are straightforward (~300 lines each)

**The ~200KB RAM cost vs stack copying is absolutely worth it** for:
- No signal handler race conditions
- No stack corruption bugs
- Simple, maintainable implementation
- Clear stack overflow detection
- Industry-standard approach

Start with POSIX implementation (Linux/macOS), validate memory usage, then add other platforms as needed.

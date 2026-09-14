#include <string.h>
#include <limits.h>
#include <inttypes.h>
#include "coro_net.h"
#include "socket.h"
#include "log.h"
#include "utils.h"

/* Platform detection for BSD-like systems (mirrors socket.c) */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__bsdi__) \
    || defined(__DragonFly__)
#    define UTIL_BSD_OS_TYPE
#endif

#ifndef MSG_NOSIGNAL
#    define MSG_NOSIGNAL 0
#endif

#define INVALID_TASK_INDEX (-1)

#ifdef _WIN32
#    include <winsock2.h>
#    include <ws2tcpip.h>
typedef ULONG nfds_t;
#endif

/**
 * @brief Task arrays (Struct-of-Arrays optimization)
 *
 * Separated by access frequency for cache efficiency:
 * - Hot arrays: fd, event (accessed in rebuild loop)
 * - Warm arrays: handler, context, line (accessed in dispatch)
 */

struct coro_net_t {
    // Configuration
    size_t max_tasks;
    size_t task_offset;  // Round-robin fairness offset

    size_t iteraction_count;  // Number of iterations in the current run loop

    // Hot arrays - rebuild loop (cache-critical)
    socket_t *task_fds;
    short *task_events;

    // Warm arrays - dispatch loop
    int *task_lines;
    void (**task_handlers)(coro_task_handle_t, socket_t, void *);
    void **task_contexts;

    // Poll arrays (rebuilt each iteration)
    coro_pollfd *pfds;
    uint8_t *task_map;

    // socket pair for wakeup
    socket_t wakeup_fds[2];

    // Runtime state
    bool running;          // Signals whether the loop should continue
    bool in_run_loop;      // True when we're inside coro_run() call stack
    bool destroy_on_exit;  // Set when coro_destroy() is called while in_run_loop

#ifdef _WIN32
    bool wsa_initialized;
#endif
};


static void coro_cleanup(coro_net_t **net);

#ifdef _WIN32
/* simulate POSIX socketpair()*/
static int socketpair(int domain, int type, int protocol, socket_t pair[2]) {
    socket_t listener = INVALID_SOCKET;
    socket_t client = INVALID_SOCKET;
    socket_t server = INVALID_SOCKET;
    socket_address_t addr;
    struct sockaddr_in sin;
    int len = sizeof(sin);

    /* we just ignore these for now. */
    (void)domain;
    (void)type;
    (void)protocol;

    // 1. Create a temporary listener socket on localhost:0 (ephemeral port)
    if(socket_address_init(&addr, "127.0.0.1", 0) != UTIL_OK) { return -1; }

    listener = socket_create_tcp_server(&addr, 1);
    if(listener == INVALID_SOCKET) { return -1; }

    // 2. Get the assigned ephemeral port
    // We need raw getsockname here because socket.h doesn't expose a wrapper for retrieving the bound port
    if(getsockname(listener, (struct sockaddr *)&sin, &len) == SOCKET_ERROR) {
        socket_close(listener);
        return -1;
    }

    // 3. Prepare address for client connection using the assigned port
    if(socket_address_init(&addr, "127.0.0.1", ntohs(sin.sin_port)) != UTIL_OK) {
        socket_close(listener);
        return -1;
    }

    // 4. Create client socket and connect
    client = socket_create_tcp();
    if(client == INVALID_SOCKET) {
        socket_close(listener);
        return -1;
    }

    if(socket_connect(client, &addr) != UTIL_OK) {
        socket_close(listener);
        socket_close(client);
        return -1;
    }

    // 5. Accept the connection
    if(socket_accept(listener, &server, NULL) != UTIL_OK) {
        socket_close(listener);
        socket_close(client);
        return -1;
    }

    // 6. Cleanup listener
    socket_close(listener);

    // 7. Disable Nagle's algorithm for low latency
    socket_set_nodelay(client, true);
    socket_set_nodelay(server, true);

    pair[0] = client;
    pair[1] = server;

    return 0;
}
#endif /* _WIN32  for socketpair() emulation */

static void coro_wake(coro_net_t *net) {
    if(!net) { return; }
    char c = 1;
    /* Prevent SIGPIPE when writing to wake socket:
     * - Linux: Use MSG_NOSIGNAL flag in send()
     * - BSD/macOS: SO_NOSIGPIPE socket option (set at socket creation)
     * - MSG_NOSIGNAL is 0 on BSD/macOS, so it's safe to always include
     */
    send(net->wakeup_fds[0], &c, 1, MSG_NOSIGNAL);
}

// Accessor functions for macros
int coro_get_line(coro_task_handle_t task) {
    if(!task.coro_net || task.index < 0 || task.index >= (int)task.coro_net->max_tasks) { return 0; }
    return task.coro_net->task_lines[task.index];
}

util_err_t coro_set_line(coro_task_handle_t task, int line) {
    if(!task.coro_net || task.index < 0 || task.index >= (int)task.coro_net->max_tasks) { return UTIL_EINVAL; }
    task.coro_net->task_lines[task.index] = line;
    return UTIL_OK;
}

util_err_t coro_set_task_event(coro_task_handle_t task, short event) {
    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_SPEW, "Setting task event: index=%d event=0x%04x", task.index, event);
    if(!task.coro_net || task.index < 0 || task.index >= (int)task.coro_net->max_tasks) { return UTIL_EINVAL; }
    task.coro_net->task_events[task.index] = event;
    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_SPEW, "Task event set successfully to 0x%04x", task.coro_net->task_events[task.index]);

    // Wake the loop to process the event change immediately
    // coro_wake(task.coro_net);

    return UTIL_OK;
}

socket_t coro_get_fd(coro_task_handle_t task) {
    if(!task.coro_net || task.index < 0 || task.index >= (int)task.coro_net->max_tasks) { return INVALID_SOCKET; }
    return task.coro_net->task_fds[task.index];
}

util_err_t coro_create(coro_net_t **out_coro_net, size_t max_tasks) {
    if(!out_coro_net) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_WARN, "Invalid argument: out_coro_net is NULL");
        return UTIL_ENULL;
    }

    if(max_tasks == 0 || max_tasks > 256) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_WARN, "Invalid argument: max_tasks must be between 1 and 256");
        return UTIL_EINVAL;  // uint8_t task_map limit
    }

    *out_coro_net = NULL;

    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Creating coro_net instance: max_tasks=%zu", max_tasks);

    // Calculate total memory needed for single allocation
    size_t total_size = sizeof(coro_net_t);

    // Calculate array sizes
    size_t task_fds_size = max_tasks * sizeof(socket_t);
    size_t task_events_size = max_tasks * sizeof(short);
    size_t task_lines_size = max_tasks * sizeof(int);
    size_t task_handlers_size = max_tasks * sizeof(void *);
    size_t task_contexts_size = max_tasks * sizeof(void *);

    // Allocate extra space in pfds and task_map for the wake socket
    size_t pfds_size = (max_tasks + 1) * sizeof(coro_pollfd);
    size_t task_map_size = (max_tasks + 1) * sizeof(uint8_t);

    // Add padding for alignment (8-byte alignment)
    size_t align = 8;
    total_size += task_fds_size + align;
    total_size += task_events_size + align;
    total_size += task_lines_size + align;
    total_size += task_handlers_size + align;
    total_size += task_contexts_size + align;
    total_size += pfds_size + align;
    total_size += task_map_size + align;

    // Allocate single contiguous block
    void *mem = calloc(1, total_size);
    if(!mem) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_ERROR, "Failed to allocate coro_net instance (%zu bytes)", total_size);
        return UTIL_ERESOURCE;
    }

    // Partition the memory block
    coro_net_t *net = (coro_net_t *)mem;
    uint8_t *ptr = (uint8_t *)mem + sizeof(coro_net_t);

// Macro to align pointer
#define ALIGN_PTR(p) ((uint8_t *)(((uintptr_t)(p) + align - 1) & ~(align - 1)))

    ptr = ALIGN_PTR(ptr);
    net->task_fds = (socket_t *)ptr;
    ptr += task_fds_size;

    ptr = ALIGN_PTR(ptr);
    net->task_events = (short *)ptr;
    ptr += task_events_size;

    ptr = ALIGN_PTR(ptr);
    net->task_lines = (int *)ptr;
    ptr += task_lines_size;

    ptr = ALIGN_PTR(ptr);
    net->task_handlers = (void (**)(coro_task_handle_t, socket_t, void *))ptr;
    ptr += task_handlers_size;

    ptr = ALIGN_PTR(ptr);
    net->task_contexts = (void **)ptr;
    ptr += task_contexts_size;

    ptr = ALIGN_PTR(ptr);
    net->pfds = (coro_pollfd *)ptr;
    ptr += pfds_size;

    ptr = ALIGN_PTR(ptr);
    net->task_map = (uint8_t *)ptr;
    ptr += task_map_size;

#undef ALIGN_PTR

    // Initialize configuration
    net->max_tasks = max_tasks;
    net->running = false;
    net->in_run_loop = false;
    net->destroy_on_exit = false;
    net->task_offset = 0;

    // Initialize task_fds to INVALID_SOCKET
    for(size_t i = 0; i < max_tasks; i++) { net->task_fds[i] = INVALID_SOCKET; }

    // Initialize task_events to CORO_EVENT_ALWAYS so tasks run immediately
    for(size_t i = 0; i < max_tasks; i++) { net->task_events[i] = CORO_EVENT_ALWAYS; }

    if(socketpair(AF_UNIX, SOCK_STREAM, 0, net->wakeup_fds) != 0) {
        util_err_t err = socket_get_err();
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_ERROR, "Failed to create wakeup socket pair.", util_err_str(err));

        free(mem);
        *out_coro_net = NULL;

        return err;
    }

#ifdef UTIL_BSD_OS_TYPE
    /* Suppress SIGPIPE on BSD/macOS when writing to closed sockets */
    int nosigpipe = 1;
    if(setsockopt(net->wakeup_fds[0], SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe)) != 0) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_WARN, "Failed to set SO_NOSIGPIPE on wakeup socket 0");
    }
    if(setsockopt(net->wakeup_fds[1], SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe)) != 0) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_WARN, "Failed to set SO_NOSIGPIPE on wakeup socket 1");
    }
#endif

    socket_set_nonblocking(net->wakeup_fds[0], true);
    socket_set_nonblocking(net->wakeup_fds[1], true);

    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Coro_net instance created successfully.");

    *out_coro_net = net;
    return UTIL_OK;
}

util_err_t coro_stop(coro_net_t *net) {
    if(!net) { return UTIL_EINVAL; }

    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Stopping coro_net instance.");
    net->running = false;
    coro_wake(net);  // Wake up the loop so it sees running=false immediately

    return UTIL_OK;
}

void coro_destroy(coro_net_t **net) {
    if(!net || !*net) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_WARN, "coro_destroy called with NULL pointer.");
        return;
    }

    // If called while inside coro_run() call stack, defer cleanup
    if((*net)->in_run_loop) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Deferring coro_net destruction until run loop exits.");
        (*net)->destroy_on_exit = true;
        (*net)->running = false;  // Signal to exit
        coro_wake(*net);          // Ensure loop wakes up
        return;
    }

    // Otherwise proceed with immediate cleanup
    coro_cleanup(net);
    *net = NULL;
}

util_err_t coro_add_task(coro_task_handle_t *task, coro_net_t *net, socket_t fd,
                         void (*handler)(coro_task_handle_t task, socket_t fd, void *context), void *context) {
    if(!task) { return UTIL_EINVAL; }
    if(!net) { return UTIL_EINVAL; }

    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_DETAIL, "Adding task with socket fd=%d", (int)fd);

    // Only set non-blocking and TCP_NODELAY if fd is valid (socket-less tasks have fd == CORO_NO_SOCKET)
    if(fd != CORO_NO_SOCKET && fd != INVALID_SOCKET) {
        socket_set_nonblocking(fd, true);
        socket_set_nodelay(fd, true);
    }

    for(size_t i = 0; i < net->max_tasks; i++) {
        if(net->task_fds[i] == INVALID_SOCKET) {
            net->task_fds[i] = fd;
            net->task_lines[i] = 0;
            net->task_handlers[i] = handler;
            net->task_contexts[i] = context;
            net->task_events[i] = CORO_EVENT_ALWAYS;

            // Construct task handle
            task->coro_net = net;
            task->index = (int)i;

            pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_SPEW, "Added task fd=%d with handle index=%d", (int)fd, (int)i);

            // Wake the loop to process the new task
            coro_wake(net);

            return UTIL_OK;
        }
    }

    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_WARN, "task list full");
    if(fd != CORO_NO_SOCKET) { socket_close(fd); }
    task->coro_net = NULL;
    task->index = INVALID_TASK_INDEX;
    return UTIL_ERESOURCE;
}


void coro_remove_task(coro_task_handle_t task) {
    if(!task.coro_net) { return; }
    if(task.index < 0 || task.index >= (int)task.coro_net->max_tasks) { return; }

    if(task.coro_net->task_fds[task.index] != INVALID_SOCKET && task.coro_net->task_fds[task.index] != CORO_NO_SOCKET) {
        socket_close(task.coro_net->task_fds[task.index]);
    }
    task.coro_net->task_fds[task.index] = INVALID_SOCKET;
    task.coro_net->task_events[task.index] = CORO_EVENT_NONE;
    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_DETAIL, "Removed task with handle index=%d", task.index);

    // Wake the loop to update the poll list
    coro_wake(task.coro_net);
}


util_err_t coro_run(coro_net_t **net_ptr, uint32_t tick_interval_ms) {
    coro_net_t *net;
    if(!net_ptr || !*net_ptr) { return UTIL_EINVAL; }

    net = *net_ptr;

    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Starting coroutine event loop.");
    net->running = true;
    net->in_run_loop = true;
    net->destroy_on_exit = false;

    while(net->running) {
        size_t nfds = 0;

        /* do it here before we continue elsewhere */
        net->iteraction_count++;

        int64_t rebuild_start = util_time_us();

        /*
         * We scan the entire task array.
         * - Tasks without a handler are skipped with a warning if allocated.
         * - For tasks marked as ALWAYS, we run them immediately.
         * - For tasks with a valid socket and a non-NONE event, we add them to the poll array.
         *
         * Note that we use a changing offset to ensure round-robin fairness.
         */
        for(size_t i = 0; i < net->max_tasks; i++) {
            size_t actual_index = (i + net->task_offset) % net->max_tasks;

            /* is there something to run? */
            if(net->task_fds[actual_index] != INVALID_SOCKET) { /* is this an allocated task? */
                if(net->task_handlers[actual_index] == NULL) {
                    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_WARN, "Task %zu has no handler set, skipping!", actual_index);
                    continue;
                }

                /* is it an ALWAYS task with a handler? */
                if(net->task_events[actual_index] == CORO_EVENT_ALWAYS) {
                    /* we will run these tasks immediately */
                    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_SPEW, "Dispatching ALWAYS task immediately: index=%zu", actual_index);
                    coro_task_handle_t task_handle = {.coro_net = net, .index = (int)actual_index};
                    net->task_handlers[actual_index](task_handle, net->task_fds[actual_index], net->task_contexts[actual_index]);
                }

                /* Add socket tasks to poll array if they have a valid socket and a poll event set */
                if(net->task_fds[actual_index] != CORO_NO_SOCKET && net->task_events[actual_index] != CORO_EVENT_NONE) {
                    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_SPEW, "Adding socket task to poll array: index=%zu fd=%d events=0x%04x",
                          actual_index, (int)net->task_fds[actual_index], net->task_events[actual_index]);
                    net->pfds[nfds].fd = net->task_fds[actual_index];
                    net->pfds[nfds].events = net->task_events[actual_index];
                    net->pfds[nfds].revents = 0;
                    net->task_map[nfds] = (uint8_t)actual_index;
                    nfds++;
                }
            }
        }

        /* update the task offset */
        net->task_offset = (net->task_offset + 1) % net->max_tasks;

        // Add wake socket to poll set
        size_t wake_pfd_index = nfds;
        net->pfds[nfds].fd = net->wakeup_fds[1];
        net->pfds[nfds].events = POLLIN;
        net->pfds[nfds].revents = 0;
        // No need to set task_map for wake socket as we handle it explicitly
        nfds++;

        int64_t rebuild_time = util_time_us() - rebuild_start;

        // No sockets to poll: sleep and continue
        // Note: We always have at least the wake socket now, so nfds >= 1
        if(nfds == 0) {
            util_sleep_ms((int)tick_interval_ms);
            continue;
        }

        // Poll with fixed timeout
        int64_t poll_start = util_time_us();
        int poll_result = coro_poll(net->pfds, (nfds_t)nfds, (int)tick_interval_ms);
        int64_t poll_time = util_time_us() - poll_start;

        if(poll_result < 0) {
            pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_DETAIL, "Poll error: %s", util_err_str(socket_get_err()));
            continue;
        }

        int64_t dispatch_start = util_time_us();

        // Dispatch socket events with round-robin fairness
        for(size_t i = 0; i < nfds; i++) {
            // Handle wake socket
            if(i == wake_pfd_index) {
                if(net->pfds[i].revents & POLLIN) {
                    // Drain the wake socket
                    char buf[32];
                    while(recv(net->wakeup_fds[1], buf, sizeof(buf), 0) > 0);
                    // No user handler to call
                }
                continue;
            }

            uint8_t idx = net->task_map[i];

            if(net->pfds[i].revents && net->task_handlers[idx] != NULL) {
                coro_task_handle_t task_handle = {.coro_net = net, .index = idx};
                net->task_handlers[idx](task_handle, net->task_fds[idx], net->task_contexts[idx]);
            }
        }

        int64_t dispatch_time = util_time_us() - dispatch_start;

        if(net->iteraction_count % 100 == 0) {
            // Log iteration statistics every 100 iterations
            pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_DETAIL,
                  "Iteration %zu: rebuild=%" PRId64 "us, poll=%" PRId64 "us, dispatch=%" PRId64 "us, nfds=%zu",
                  net->iteraction_count, (int64_t)rebuild_time, (int64_t)poll_time, (int64_t)dispatch_time, nfds);
        }
    }

    // Mark that we've exited the run loop
    net->in_run_loop = false;

    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Coroutine event loop exited.");

    // If destroy was called while we were running, perform cleanup now
    if(net->destroy_on_exit) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Performing deferred destruction.");

        coro_cleanup(net_ptr);

        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Deferred destruction completed.");
    }

    return UTIL_OK;
}


void coro_cleanup(coro_net_t **net) {
    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Destroying coro_net instance.");
    if(!net || !*net) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_WARN, "Coro_net instance is NULL, nothing to destroy.");
        return;
    }

    // Close wake sockets
    if((*net)->wakeup_fds[0] != INVALID_SOCKET) { socket_close((*net)->wakeup_fds[0]); }
    if((*net)->wakeup_fds[1] != INVALID_SOCKET) { socket_close((*net)->wakeup_fds[1]); }

    // Close all open sockets
    for(size_t i = 0; i < (*net)->max_tasks; i++) {
        if((*net)->task_fds[i] != INVALID_SOCKET && (*net)->task_fds[i] != CORO_NO_SOCKET) {
            socket_close((*net)->task_fds[i]);
            (*net)->task_fds[i] = INVALID_SOCKET;
        }
    }


    /* Free the entire block (single allocation) */
    free(*net);
    *net = NULL;

    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Coro_net instance destroyed.");
}

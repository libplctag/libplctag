#include "coro_net.h"
#include "log.h"
#include "utils.h"
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>

#define MAX_TASKS 64

static task_t tasks[MAX_TASKS];
static coro_pollfd pfds[MAX_TASKS];

static bool running = false;
static CSOCKET wake_read_fd = (CSOCKET)-1;
static CSOCKET wake_write_fd = (CSOCKET)-1;
static int task_count = 0;

// --- Platform Helpers ---

static void set_non_blocking(CSOCKET fd);
static void set_no_delay(CSOCKET fd);
static int coro_create_wakeup_pipe(void);
static void coro_destroy_wakeup_pipe(void);
static void coro_signal_wakeup(void);

void coro_init(void) {
    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Initializing coroutine network library.");
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_ERROR, "WSAStartup failed.");
        exit(EXIT_FAILURE);
    }
#endif
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].fd = (CSOCKET)-1;
    }

    if (coro_create_wakeup_pipe() != 0) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_ERROR, "Failed to create wakeup pipe.");
        exit(EXIT_FAILURE);
    }

    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Coroutine network library initialized.");
}



void coro_stop(void) {
    running = false;

    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Stopping coroutine network library.");
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].fd != (CSOCKET)-1) {
            CS_CLOSE(tasks[i].fd);
            tasks[i].fd = (CSOCKET)-1;
        }
    }
    coro_destroy_wakeup_pipe();
    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Coroutine network library stopped.");
}



void coro_add(CSOCKET fd, void (*handler)(task_t*), void *context) {

    set_non_blocking(fd);
    set_no_delay(fd);

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].fd == (CSOCKET)-1) {
            tasks[i].fd = fd;
            tasks[i].line = 0;
            tasks[i].handler = handler;
            tasks[i].context = context;
            tasks[i].events = POLLIN; // Default start state
            coro_signal_wakeup();  // Signal to wake up event loop immediately
            return;
        }
    }

    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_WARN, "task_t list full");
    CS_CLOSE(fd);
}

void coro_remove(CSOCKET fd) {
    for (size_t i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].fd == fd) {
            CS_CLOSE(tasks[i].fd);
            tasks[i].fd = (CSOCKET)-1;
            coro_signal_wakeup();  // Signal to wake up event loop immediately
            return;
        }
    }
}

// Sets the application-managed buffers for the task
void coro_set_buffer(task_t *t, buf_t *rx, buf_t *tx) {
    t->rx_buf = *rx;
    t->tx_buf = *tx;
}


void coro_run(void) {
    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Starting coroutine network loop.");
    running = true;

    while (running) {
        size_t nfds = 0;
        size_t task_map[MAX_TASKS];
        size_t wake_pipe_index = (size_t)-1;  // Track wake pipe position in poll array

        // 1. Rebuild Poll List
        int64_t rebuild_start = util_time_us();

        // Always include the wake pipe for instant wakeup
        if (wake_read_fd != (CSOCKET)-1) {
            pfds[nfds].fd = wake_read_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            wake_pipe_index = nfds;
            nfds++;
        }

        for (size_t i = 0; i < MAX_TASKS; i++) {
            if (tasks[i].fd != (CSOCKET)-1) {
                pfds[nfds].fd = tasks[i].fd;
                pfds[nfds].events = tasks[i].events;
                pfds[nfds].revents = 0;
                task_map[nfds] = i;
                nfds++;
            }
        }

        int64_t rebuild_time = util_time_us() - rebuild_start;

        //if (nfds == 0) break;

        /* If nothing to do, wait a little bit to allow something to happen.  This should be handled by a wake up pipe or similar */
        if(nfds == 0) {
            util_sleep_ms(10);
            continue;
        }

        int64_t poll_start = util_time_us();
        int poll_result = coro_poll(pfds, (nfds_t)nfds, 100);
        int64_t poll_time = util_time_us() - poll_start;

        if (poll_result < 0) {
#ifdef _WIN32
            // Check for interruption/error
            if (WSAGetLastError() != WSAEINTR) {
                fprintf(stderr, "Poll failed: %d\n", WSAGetLastError());
                break;
            }
#else
            if (errno == EINTR) continue;
            perror("Poll failed");
            break;
#endif
        }

        // 3. Dispatch
        int64_t dispatch_start = util_time_us();

        // Handle wake pipe first if it has events
        if (wake_pipe_index != (size_t)-1 && pfds[wake_pipe_index].revents) {
            // Drain any pending wakeup signals to prevent poll from hanging
            uint8_t buf[256];
            ssize_t n;
            while ((n = recv(wake_read_fd, (char *)buf, sizeof(buf), 0)) > 0) {
                // Just discard the bytes
            }
            pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_DETAIL, "Wakeup signal received and drained.");
        }

        // Handle regular tasks
        for (size_t i = 0; i < nfds; i++) {
            // Skip the wake pipe (already handled above)
            if (i == wake_pipe_index) continue;

            if (pfds[i].revents) {
                size_t ti = task_map[i];
                tasks[ti].handler(&tasks[ti]);
            }
        }
        int64_t dispatch_time = util_time_us() - dispatch_start;

        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_DETAIL, "Loop iteration: rebuild=%lldus, poll=%lldus, dispatch=%lldus, nfds=%u",
              (long long)rebuild_time, (long long)poll_time, (long long)dispatch_time, nfds);
    }
#ifdef _WIN32
    WSACleanup();
#endif
    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Coroutine network loop exited.");
}


/* Helpers */


static void set_non_blocking(CSOCKET fd) {
#ifdef _WIN32
    u_long iMode = 1;
    ioctlsocket(fd, FIONBIO, &iMode);
#else
    fcntl(fd, F_SETFL, O_NONBLOCK);
#endif
}


static void set_no_delay(CSOCKET fd) {
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
}


/**
 * Signal the wakeup pipe to interrupt the event loop.
 * This wakes up poll() instantly instead of waiting for the timeout.
 */
static void coro_signal_wakeup(void) {
    if (wake_write_fd != (CSOCKET)-1) {
        uint8_t byte = 0xFF;
        ssize_t sent = send(wake_write_fd, (char *)&byte, 1, 0);
        (void)sent;  /* Suppress unused variable warning */
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_DETAIL, "Wakeup signal sent.");
    }
}


/**
 * Destroy the wakeup pipe and clean up resources.
 */
static void coro_destroy_wakeup_pipe(void) {
    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Destroying wakeup pipe.");

    if (wake_read_fd != (CSOCKET)-1) {
        CS_CLOSE(wake_read_fd);
        wake_read_fd = (CSOCKET)-1;
    }

    if (wake_write_fd != (CSOCKET)-1) {
        CS_CLOSE(wake_write_fd);
        wake_write_fd = (CSOCKET)-1;
    }
}


/**
 * Create a wakeup pipe using socketpair (POSIX) or TCP sockets (Windows).
 * This allows the event loop to be interrupted instantly when:
 * - A new task is added via coro_add()
 * - The event mask changes for an existing task
 * - The loop needs to be stopped
 *
 * Returns 0 on success, -1 on failure.
 */

#ifdef _WIN32
/**
 * Windows implementation: Create a pair of connected TCP sockets on loopback.
 */
static int coro_create_wakeup_pipe(void) {
    SOCKET listener = INVALID_SOCKET;
    SOCKET accept_fd = INVALID_SOCKET;
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(struct sockaddr_in);
    u_long non_blocking = 1;

    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Creating wakeup pipe (Windows TCP socket pair).");

    /* Create listening socket */
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_ERROR, "Failed to create listener socket: %d", WSAGetLastError());
        return -1;
    }

    /* Bind to loopback on any port */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = 0;  /* Let OS choose port */
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_ERROR, "Failed to bind listener socket: %d", WSAGetLastError());
        closesocket(listener);
        return -1;
    }

    if (listen(listener, 1) == SOCKET_ERROR) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_ERROR, "Failed to listen on socket: %d", WSAGetLastError());
        closesocket(listener);
        return -1;
    }

    /* Get the bound port */
    addr_len = sizeof(addr);
    if (getsockname(listener, (struct sockaddr *)&addr, &addr_len) == SOCKET_ERROR) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_ERROR, "Failed to get listener socket name: %d", WSAGetLastError());
        closesocket(listener);
        return -1;
    }

    /* Connect to create the read side */
    wake_read_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (wake_read_fd == INVALID_SOCKET) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_ERROR, "Failed to create read socket: %d", WSAGetLastError());
        closesocket(listener);
        return -1;
    }

    if (connect(wake_read_fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_ERROR, "Failed to connect read socket: %d", WSAGetLastError());
        closesocket(listener);
        closesocket(wake_read_fd);
        wake_read_fd = INVALID_SOCKET;
        return -1;
    }

    /* Accept to create the write side */
    accept_fd = accept(listener, NULL, NULL);
    if (accept_fd == INVALID_SOCKET) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_ERROR, "Failed to accept connection: %d", WSAGetLastError());
        closesocket(listener);
        closesocket(wake_read_fd);
        wake_read_fd = INVALID_SOCKET;
        return -1;
    }

    wake_write_fd = accept_fd;

    /* Close listener, we don't need it anymore */
    closesocket(listener);

    /* Set both to non-blocking */
    if (ioctlsocket(wake_read_fd, FIONBIO, &non_blocking) == SOCKET_ERROR) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_ERROR, "Failed to set read fd non-blocking: %d", WSAGetLastError());
        closesocket(wake_read_fd);
        closesocket(wake_write_fd);
        wake_read_fd = INVALID_SOCKET;
        wake_write_fd = INVALID_SOCKET;
        return -1;
    }

    if (ioctlsocket(wake_write_fd, FIONBIO, &non_blocking) == SOCKET_ERROR) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_ERROR, "Failed to set write fd non-blocking: %d", WSAGetLastError());
        closesocket(wake_read_fd);
        closesocket(wake_write_fd);
        wake_read_fd = INVALID_SOCKET;
        wake_write_fd = INVALID_SOCKET;
        return -1;
    }

    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Wakeup pipe created successfully.");
    return 0;
}

#else

/**
 * POSIX implementation: Use socketpair to create a bidirectional socket pair.
 */
static int coro_create_wakeup_pipe(void) {
    int wake_fds[2] = {-1, -1};
    int flags = 0;

    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Creating wakeup pipe (POSIX socketpair).");

    if (socketpair(PF_LOCAL, SOCK_STREAM, 0, wake_fds) < 0) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_ERROR, "Failed to create socketpair: %s", strerror(errno));
        return -1;
    }

    wake_read_fd = wake_fds[0];
    wake_write_fd = wake_fds[1];

    /* Set read fd to non-blocking */
    if ((flags = fcntl(wake_read_fd, F_GETFL)) < 0) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_ERROR, "Failed to get flags on read fd: %s", strerror(errno));
        close(wake_read_fd);
        close(wake_write_fd);
        wake_read_fd = -1;
        wake_write_fd = -1;
        return -1;
    }

    flags |= O_NONBLOCK;
    if (fcntl(wake_read_fd, F_SETFL, flags) < 0) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_ERROR, "Failed to set read fd non-blocking: %s", strerror(errno));
        close(wake_read_fd);
        close(wake_write_fd);
        wake_read_fd = -1;
        wake_write_fd = -1;
        return -1;
    }

    /* Set write fd to non-blocking */
    if ((flags = fcntl(wake_write_fd, F_GETFL)) < 0) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_ERROR, "Failed to get flags on write fd: %s", strerror(errno));
        close(wake_read_fd);
        close(wake_write_fd);
        wake_read_fd = -1;
        wake_write_fd = -1;
        return -1;
    }

    flags |= O_NONBLOCK;
    if (fcntl(wake_write_fd, F_SETFL, flags) < 0) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_ERROR, "Failed to set write fd non-blocking: %s", strerror(errno));
        close(wake_read_fd);
        close(wake_write_fd);
        wake_read_fd = -1;
        wake_write_fd = -1;
        return -1;
    }

    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Wakeup pipe created successfully.");
    return 0;
}

#endif


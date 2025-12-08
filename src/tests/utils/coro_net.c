#include "coro_net.h"
#include "log.h"
#include "utils.h"
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <string.h>
#include <limits.h>

#define MAX_TASKS 64
#define MAX_TIMERS 64
#define INVALID_HANDLE -1
#define INET_ADDRSTRLEN 16

// --- socket_entry_t Structure (Private) ---
typedef struct socket_entry_t {
    CSOCKET fd;
    int line;
    short events;
    void (*handler)(coro_socket_handle_t handle, CSOCKET fd, void *context);
    void *context;
    char ip_addr[INET_ADDRSTRLEN];  // IPv4 address buffer
    uint16_t port;                   // Port number
} socket_entry_t;

typedef struct timer_t {
    bool used;
    int64_t deadline_us;
    void (*handler)(coro_timer_handle_t handle, void *context);
    void *context;
} timer_t;

static socket_entry_t sockets[MAX_TASKS];
static timer_t timers[MAX_TIMERS];
static coro_pollfd pfds[MAX_TASKS];
static bool running = false;

static util_err_t get_last_socket_error(void);
static void set_non_blocking(CSOCKET fd);
static void set_no_delay(CSOCKET fd);
static int64_t now_us(void);
static int64_t next_timer_timeout_ms(void);
static void timer_dispatch_due(void);

// Helper to validate handle
static inline bool is_valid_socket_handle(coro_socket_handle_t handle) {
    return (handle >= 0 && handle < MAX_TASKS && sockets[handle].fd != (CSOCKET)-1);
}

// Accessor functions for macros
int coro_get_line(coro_socket_handle_t handle) {
    if (handle < 0 || handle >= MAX_TASKS) return 0;
    return sockets[handle].line;
}

void coro_set_line(coro_socket_handle_t handle, int line) {
    if (handle < 0 || handle >= MAX_TASKS) return;
    sockets[handle].line = line;
}

void coro_set_events(coro_socket_handle_t handle, short events) {
    if (handle < 0 || handle >= MAX_TASKS) return;
    sockets[handle].events = events;
}

CSOCKET coro_get_fd(coro_socket_handle_t handle) {
    if (handle < 0 || handle >= MAX_TASKS) return (CSOCKET)-1;
    return sockets[handle].fd;
}

void coro_close_socket(coro_socket_handle_t handle) {
    if (handle < 0 || handle >= MAX_TASKS) return;
    if (sockets[handle].fd != (CSOCKET)-1) {
        CS_CLOSE(sockets[handle].fd);
        sockets[handle].fd = (CSOCKET)-1;
        sockets[handle].line = 0;
    }
}

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
        sockets[i].fd = (CSOCKET)-1;
    }

    for (int i = 0; i < MAX_TIMERS; i++) {
        timers[i].used = false;
    }

    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Coroutine network library initialized.");
}

void coro_stop(void) {
    running = false;

    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Stopping coroutine network library.");
    for (int i = 0; i < MAX_TASKS; i++) {
        if (sockets[i].fd != (CSOCKET)-1) {
            CS_CLOSE(sockets[i].fd);
            sockets[i].fd = (CSOCKET)-1;
        }
    }
    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Coroutine network library stopped.");
}

util_err_t coro_add_socket(coro_socket_handle_t *handle, 
                           CSOCKET fd, 
                           void (*handler)(coro_socket_handle_t handle, CSOCKET fd, void *context), 
                           void *context) {
    if (!handle) return UTIL_EINVAL;

    set_non_blocking(fd);
    set_no_delay(fd);

    for (int i = 0; i < MAX_TASKS; i++) {
        if (sockets[i].fd == (CSOCKET)-1) {
            sockets[i].fd = fd;
            sockets[i].line = 0;
            sockets[i].handler = handler;
            sockets[i].context = context;
            sockets[i].events = POLLIN;
            sockets[i].ip_addr[0] = '\0';
            sockets[i].port = 0;
            *handle = i;
            pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_DETAIL, "Added socket fd=%d with handle=%d", (int)fd, i);
            return UTIL_OK;
        }
    }

    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_WARN, "socket list full");
    CS_CLOSE(fd);
    *handle = INVALID_HANDLE;
    return UTIL_ERESOURCE;
}

void coro_remove_socket(coro_socket_handle_t handle) {
    if (handle < 0 || handle >= MAX_TASKS) return;
    
    if (sockets[handle].fd != (CSOCKET)-1) {
        CS_CLOSE(sockets[handle].fd);
        sockets[handle].fd = (CSOCKET)-1;
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_DETAIL, "Removed socket with handle=%d", handle);
    }
}

util_err_t coro_add_timer(coro_timer_handle_t *handle,
                          int64_t delay_ms,
                          void (*handler)(coro_timer_handle_t handle, void *context),
                          void *context) {
    if (!handle) return UTIL_EINVAL;

    int64_t deadline = now_us() + (delay_ms * 1000);
    
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i].used) {
            timers[i].used = true;
            timers[i].deadline_us = deadline;
            timers[i].handler = handler;
            timers[i].context = context;
            *handle = i;
            pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_DETAIL, "Timer added with handle=%d, delay=%lldms", i, (long long)delay_ms);
            return UTIL_OK;
        }
    }
    
    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_WARN, "timer list full");
    *handle = INVALID_HANDLE;
    return UTIL_ERESOURCE;
}

void coro_remove_timer(coro_timer_handle_t handle) {
    if (handle < 0 || handle >= MAX_TIMERS) return;
    
    timers[handle].used = false;
    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_DETAIL, "Removed timer with handle=%d", handle);
}

void coro_run(void) {
    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Starting coroutine network loop.");
    running = true;

    while (running) {
        size_t nfds = 0;
        coro_socket_handle_t task_map[MAX_TASKS];

        int64_t rebuild_start = util_time_us();

        for (coro_socket_handle_t i = 0; i < MAX_TASKS; i++) {
            if (sockets[i].fd != (CSOCKET)-1) {
                pfds[nfds].fd = sockets[i].fd;
                pfds[nfds].events = sockets[i].events;
                pfds[nfds].revents = 0;
                task_map[nfds] = i;
                nfds++;
            }
        }

        int64_t rebuild_time = util_time_us() - rebuild_start;

        if (nfds == 0) {
            int64_t sleep_ms = next_timer_timeout_ms();
            util_sleep_ms(sleep_ms);
            timer_dispatch_due();
            continue;
        }

        int64_t poll_start = util_time_us();
        int64_t timeout_ms = next_timer_timeout_ms();
        int poll_result = coro_poll(pfds, (nfds_t)nfds, (int)timeout_ms);
        int64_t poll_time = util_time_us() - poll_start;

        if (poll_result < 0) {
            pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_DETAIL, "Poll error: %s", util_err_to_string(get_last_socket_error()));
            continue;
        }

        int64_t dispatch_start = util_time_us();

        for (size_t i = 0; i < nfds; i++) {
            if (pfds[i].revents) {
                coro_socket_handle_t handle = task_map[i];
                sockets[handle].handler(handle, sockets[handle].fd, sockets[handle].context);
            }
        }

        timer_dispatch_due();

        int64_t dispatch_time = util_time_us() - dispatch_start;

        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_DETAIL, "Loop iteration: rebuild=%lldus, poll=%lldus, dispatch=%lldus, nfds=%zu",
              (long long)rebuild_time, (long long)poll_time, (long long)dispatch_time, nfds);
    }
#ifdef _WIN32
    WSACleanup();
#endif
    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Coroutine network loop exited.");
}

/* --- I/O Functions --- */

util_err_t cr_accept(coro_socket_handle_t listener_handle, CSOCKET *client) {
    if (!is_valid_socket_handle(listener_handle)) return UTIL_EINVAL;
    
    *client = accept(sockets[listener_handle].fd, NULL, NULL);
    if (*client == (CSOCKET)-1) {
        if (errno == CS_EAGAIN || errno == EWOULDBLOCK) {
            return UTIL_EAGAIN;
        } else {
            return util_err_from_errno(errno);
        }
    }
    return UTIL_OK;
}

util_err_t cr_connect(coro_socket_handle_t sock_handle, const char *ip_addr, uint16_t port) {
    if (!is_valid_socket_handle(sock_handle)) return UTIL_EINVAL;
    if (!ip_addr) return UTIL_EINVAL;
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, ip_addr, &addr.sin_addr) != 1) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_ERROR, "Invalid IP address: %s", ip_addr);
        return UTIL_EINVAL;
    }
    
    int res = connect(sockets[sock_handle].fd, (struct sockaddr *)&addr, sizeof(addr));
    if (res < 0) {
        if (errno == CS_EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS) {
            return UTIL_EAGAIN;
        } else {
            return util_err_from_errno(errno);
        }
    }
    return UTIL_OK;
}

util_err_t cr_read(coro_socket_handle_t sock_handle, buf_t *read_buf, util_err_t (*frame_func)(buf_t *buf, void *context), void *context) {
    if (!is_valid_socket_handle(sock_handle)) return UTIL_EINVAL;
    
    ssize_t r;
    util_err_t rc = UTIL_OK;
    do {
        r = recv(sockets[sock_handle].fd, buf_write_ptr(read_buf), buf_write_size(read_buf), 0);
        if (r < 0) {
            if (errno == CS_EAGAIN || errno == EWOULDBLOCK) {
                return UTIL_EAGAIN;
            } else {
                util_err_t err = util_err_from_errno(errno);
                buf_set_error(read_buf, err, "socket read");
                return err;
            }
        } else if (r == 0) {
            buf_set_error(read_buf, UTIL_ECLOSED, "socket closed");
            return UTIL_ECLOSED;
        }

        if ((size_t)r > buf_write_size(read_buf)) {
            buf_set_error(read_buf, UTIL_EBOUNDS, "buffer overflow");
            return UTIL_EBOUNDS;
        }

        buf_write_advance(read_buf, (size_t)r);
    } while (frame_func && (rc = frame_func(read_buf, context)) == UTIL_EAGAIN);

    return rc;
}

util_err_t cr_recvfrom(coro_socket_handle_t sock_handle, buf_t *read_buf, const char **ip_addr, uint16_t *port) {
    if (!is_valid_socket_handle(sock_handle)) return UTIL_EINVAL;
    if (!ip_addr || !port) return UTIL_EINVAL;
    
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    
    ssize_t r = recvfrom(sockets[sock_handle].fd, buf_write_ptr(read_buf), buf_write_size(read_buf), 0, 
                         (struct sockaddr *)&addr, &addrlen);
    if (r < 0) {
        if (errno == CS_EAGAIN || errno == EWOULDBLOCK) {
            return UTIL_EAGAIN;
        } else {
            util_err_t err = util_err_from_errno(errno);
            buf_set_error(read_buf, err, "socket recvfrom");
            return err;
        }
    } else if (r == 0) {
        buf_set_error(read_buf, UTIL_ECLOSED, "socket closed");
        return UTIL_ECLOSED;
    } else {
        if ((size_t)r > buf_write_size(read_buf)) {
            buf_set_error(read_buf, UTIL_EBOUNDS, "buffer overflow");
            return UTIL_EBOUNDS;
        }
        
        // Convert address to string and store in socket entry
        if (inet_ntop(AF_INET, &addr.sin_addr, sockets[sock_handle].ip_addr, INET_ADDRSTRLEN) == NULL) {
            pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_ERROR, "Failed to convert IP address");
            return UTIL_EINVAL;
        }
        sockets[sock_handle].port = ntohs(addr.sin_port);
        
        // Return pointers to the stored values
        *ip_addr = sockets[sock_handle].ip_addr;
        *port = sockets[sock_handle].port;
        
        buf_write_advance(read_buf, (size_t)r);
        return UTIL_OK;
    }
}

util_err_t cr_sendto(coro_socket_handle_t sock_handle, buf_t *write_buf, const char *ip_addr, uint16_t port) {
    if (!is_valid_socket_handle(sock_handle)) return UTIL_EINVAL;
    if (!ip_addr) return UTIL_EINVAL;
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, ip_addr, &addr.sin_addr) != 1) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_ERROR, "Invalid IP address: %s", ip_addr);
        return UTIL_EINVAL;
    }
    
    ssize_t r = sendto(sockets[sock_handle].fd, buf_read_ptr(write_buf), buf_read_size(write_buf), 0,
                       (struct sockaddr *)&addr, sizeof(addr));
    if (r < 0) {
        if (errno == CS_EAGAIN || errno == EWOULDBLOCK) {
            return UTIL_EAGAIN;
        } else {
            util_err_t err = util_err_from_errno(errno);
            buf_set_error(write_buf, err, "socket sendto");
            return err;
        }
    } else if (r == 0) {
        buf_set_error(write_buf, UTIL_ECLOSED, "socket closed");
        return UTIL_ECLOSED;
    } else {
        buf_read_advance(write_buf, (size_t)r);
        return UTIL_OK;
    }
}

util_err_t cr_write(coro_socket_handle_t sock_handle, buf_t *write_buf) {
    if (!is_valid_socket_handle(sock_handle)) return UTIL_EINVAL;
    
    ssize_t r;
    do {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_DETAIL, "Attempting to write %zu bytes to socket:", buf_read_size(write_buf));
        pdlog_bytes(LOG_MODULE_CORO_NET, LOG_LEVEL_DETAIL, write_buf);
        r = send(sockets[sock_handle].fd, buf_read_ptr(write_buf), buf_read_size(write_buf), 0);
        if (r < 0) {
            if (errno == CS_EAGAIN || errno == EWOULDBLOCK) {
                pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_DETAIL, "Socket write would block");
                return UTIL_EAGAIN;
            } else {
                util_err_t err = util_err_from_errno(errno);
                buf_set_error(write_buf, err, "socket write");
                return err;
            }
        } else {
            pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_DETAIL, "Wrote %zd bytes to socket", r);
            buf_read_advance(write_buf, (size_t)r);
        }
    } while (buf_read_size(write_buf));

    return UTIL_OK;
}

/* Helpers */

static util_err_t get_last_socket_error(void) {
#ifdef _WIN32
    return util_err_from_errno(WSAGetLastError());
#else
    return util_err_from_errno(errno);
#endif
}

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

/* --- Timer Functions --- */

static int64_t now_us(void) {
    return util_time_us();
}

static int64_t next_timer_timeout_ms(void) {
    int64_t next_deadline = INT64_MAX;
    int64_t now = now_us();

    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timers[i].used && timers[i].deadline_us < next_deadline) {
            next_deadline = timers[i].deadline_us;
        }
    }

    if (next_deadline == INT64_MAX) {
        return 100;
    }

    int64_t diff_us = next_deadline - now;
    if (diff_us < 0) {
        return 0;
    }

    return (diff_us + 999) / 1000;
}

static void timer_dispatch_due(void) {
    int64_t now = now_us();

    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timers[i].used && timers[i].deadline_us <= now) {
            timers[i].used = false;
            pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_DETAIL, "Timer expired, dispatching handler for handle=%d", i);
            timers[i].handler(i, timers[i].context);
        }
    }
}


#ifndef CORONET_H
#define CORONET_H

/**
 * @file coro_net.h
 * @brief Coroutine-based network I/O library
 *
 * IMPORTANT: This library is NOT thread-safe. All functions operating on a
 * coro_net_t instance must be called from the same thread, or the caller must
 * provide external synchronization. Typically, coro_run() is called in a
 * dedicated thread and all socket/timer operations happen within handlers
 * invoked by that thread.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include "buf.h"
#include "log.h"

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef SOCKET CSOCKET;
    #define CS_CLOSE(fd) closesocket(fd)
    #define CS_EAGAIN WSAEWOULDBLOCK
    #define coro_poll WSAPoll
    typedef WSAPOLLFD coro_pollfd;
#else
    #include <poll.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <sys/socket.h>
    typedef int CSOCKET;
    #define CS_CLOSE(fd) close(fd)
    #define CS_EAGAIN EAGAIN
    #define coro_poll poll
    typedef struct pollfd coro_pollfd;
#endif

/* opaque types */
typedef struct coro_net_t coro_net_t;
typedef struct coro_socket_t coro_socket_t;
typedef struct coro_timer_t coro_timer_t;

// Handle types

// --- Public API ---

/* note that the maximum for number of sockets and timers is 256 because the indexes are stored in one byte. */
util_err_t coro_create(coro_net_t **out_coro_net, size_t max_sockets, size_t max_timers);
void coro_destroy(coro_net_t *coro_net);
util_err_t coro_run(coro_net_t *coro_net);
util_err_t coro_stop(coro_net_t *coro_net);

typedef struct coro_socket_handle_t {
    coro_net_t *coro_net;
    int index;
} coro_socket_handle_t;

typedef struct coro_timer_handle_t {
    coro_net_t *coro_net;
    int index;
} coro_timer_handle_t;

/* the socket object maintains a pointer to its owning coro_net object*/
util_err_t coro_add_socket(coro_socket_handle_t *sock, 
                           coro_net_t *coro_net,
                           CSOCKET fd, 
                           void (*handler)(coro_socket_handle_t sock, CSOCKET fd, void *context), 
                           void *context);

void coro_remove_socket(coro_socket_handle_t sock_ptr);

/* the timer object maintains a pointer to its owning coro_net object */
util_err_t coro_add_timer(coro_timer_handle_t *timer_ptr,
                          coro_net_t *coro_net,
                          int64_t delay_ms,
                          void (*handler)(coro_timer_handle_t timer_ptr, void *context),
                          void *context);

void coro_remove_timer(coro_timer_handle_t timer_ptr);

/* non-blocking IO functions */
util_err_t cr_accept(coro_socket_handle_t listener_sock, CSOCKET *client_fd_ptr);
util_err_t cr_connect(coro_socket_handle_t sock, const char *ip_addr, uint16_t port);
util_err_t cr_read(coro_socket_handle_t sock, buf_t *read_buf, util_err_t (*frame_func)(buf_t *buf, void *context), void *context);
util_err_t cr_recvfrom(coro_socket_handle_t sock, buf_t *read_buf, const char **ip_addr, uint16_t *port);
util_err_t cr_sendto(coro_socket_handle_t sock, buf_t *write_buf, const char *ip_addr, uint16_t port);
util_err_t cr_write(coro_socket_handle_t sock, buf_t *write_buf);

/* internal accessors for macros */
int coro_get_line(coro_socket_handle_t sock);
util_err_t coro_set_line(coro_socket_handle_t sock, int line);
util_err_t coro_set_events(coro_socket_handle_t sock, short events);
CSOCKET coro_get_fd(coro_socket_handle_t sock);

/* FIXME - when would you use this instead of coro_remove_socket? */
util_err_t coro_close_socket(coro_socket_handle_t sock);


/* Macros for defining coroutines */
#define CR_START(sock_ptr) switch(coro_get_line(sock_ptr)) { case 0:
#define CR_YIELD(sock_ptr, ev) \
    do { coro_set_line((sock_ptr), __LINE__); coro_set_events((sock_ptr), (ev)); return; case __LINE__:; } while(0)
#define CR_END(sock_ptr) default: break; /* FIXME - should be smarter than this */} coro_close_socket(sock_ptr);

/* socket helper macros for common usage */
#define cr_yield_accept(sock_ptr, client_fd_ptr, err) \
    while(((err) = cr_accept((sock_ptr), (client_fd_ptr))) == UTIL_EAGAIN) { \
        CR_YIELD((sock_ptr), POLLIN); \
    }

#define cr_yield_connect(sock_ptr, ip, port, err) \
    while(((err) = cr_connect((sock_ptr), (ip), (port))) == UTIL_EAGAIN) { \
        CR_YIELD((sock_ptr), POLLOUT); \
    }

#define cr_yield_read(sock_ptr, buf, func, ctx, err) \
    while(((err) = cr_read((sock_ptr), (buf), (func), (ctx))) == UTIL_EAGAIN) { \
        CR_YIELD((sock_ptr), POLLIN); \
    }

#define cr_yield_recvfrom(sock_ptr, buf, ip, port, err) \
    while(((err) = cr_recvfrom((sock_ptr), (buf), (ip), (port))) == UTIL_EAGAIN) { \
        CR_YIELD((sock_ptr), POLLIN); \
    }

#define cr_yield_sendto(sock_ptr, buf, ip, port, err) \
    while(((err) = cr_sendto((sock_ptr), (buf), (ip), (port))) == UTIL_EAGAIN) { \
        CR_YIELD((sock_ptr), POLLOUT); \
    }

#define cr_yield_write(sock_ptr, buf, err) \
    while(((err) = cr_write((sock_ptr), (buf))) == UTIL_EAGAIN) { \
        CR_YIELD((sock_ptr), POLLOUT); \
    }

#endif



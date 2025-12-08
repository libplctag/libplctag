#ifndef CORONET_H
#define CORONET_H

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

// Handle types
typedef int32_t coro_socket_handle_t;
typedef int32_t coro_timer_handle_t;

// --- Public API ---
void coro_init(void);
void coro_run(void);
void coro_stop(void);

util_err_t coro_add_socket(coro_socket_handle_t *handle, 
                           CSOCKET fd, 
                           void (*handler)(coro_socket_handle_t handle, CSOCKET fd, void *context), 
                           void *context);

void coro_remove_socket(coro_socket_handle_t handle);

util_err_t coro_add_timer(coro_timer_handle_t *handle,
                          int64_t delay_ms,
                          void (*handler)(coro_timer_handle_t handle, void *context),
                          void *context);

void coro_remove_timer(coro_timer_handle_t handle);

// --- I/O Function Declarations ---
util_err_t cr_accept(coro_socket_handle_t listener_handle, CSOCKET *client);
util_err_t cr_connect(coro_socket_handle_t sock_handle, const char *ip_addr, uint16_t port);
util_err_t cr_read(coro_socket_handle_t sock_handle, buf_t *read_buf, util_err_t (*frame_func)(buf_t *buf, void *context), void *context);
util_err_t cr_recvfrom(coro_socket_handle_t sock_handle, buf_t *read_buf, const char **ip_addr, uint16_t *port);
util_err_t cr_sendto(coro_socket_handle_t sock_handle, buf_t *write_buf, const char *ip_addr, uint16_t port);
util_err_t cr_write(coro_socket_handle_t sock_handle, buf_t *write_buf);

// Internal accessors for macros
int coro_get_line(coro_socket_handle_t handle);
void coro_set_line(coro_socket_handle_t handle, int line);
void coro_set_events(coro_socket_handle_t handle, short events);
CSOCKET coro_get_fd(coro_socket_handle_t handle);
void coro_close_socket(coro_socket_handle_t handle);

// --- CORE COROUTINE MACROS ---
#define CR_START(handle) switch(coro_get_line(handle)) { case 0:
#define CR_YIELD(handle, ev) \
    do { coro_set_line((handle), __LINE__); coro_set_events((handle), (ev)); return; case __LINE__:; } while(0)
#define CR_END(handle) } coro_close_socket(handle);

// --- Helper Macros ---
#define cr_yield_accept(handle, client_fd, err) \
    while(((err) = cr_accept((handle), (client_fd))) == UTIL_EAGAIN) { \
        CR_YIELD((handle), POLLIN); \
    }

#define cr_yield_connect(handle, ip, port, err) \
    while(((err) = cr_connect((handle), (ip), (port))) == UTIL_EAGAIN) { \
        CR_YIELD((handle), POLLOUT); \
    }

#define cr_yield_read(handle, buf, func, ctx, err) \
    while(((err) = cr_read((handle), (buf), (func), (ctx))) == UTIL_EAGAIN) { \
        CR_YIELD((handle), POLLIN); \
    }

#define cr_yield_recvfrom(handle, buf, ip, port, err) \
    while(((err) = cr_recvfrom((handle), (buf), (ip), (port))) == UTIL_EAGAIN) { \
        CR_YIELD((handle), POLLIN); \
    }

#define cr_yield_sendto(handle, buf, ip, port, err) \
    while(((err) = cr_sendto((handle), (buf), (ip), (port))) == UTIL_EAGAIN) { \
        CR_YIELD((handle), POLLOUT); \
    }

#define cr_yield_write(handle, buf, err) \
    while(((err) = cr_write((handle), (buf))) == UTIL_EAGAIN) { \
        CR_YIELD((handle), POLLOUT); \
    }

#endif



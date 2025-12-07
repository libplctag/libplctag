#ifndef CORONET_H
#define CORONET_H

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "buf.h"


// --- Platform Includes and Defines ---

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
// Check both EAGAIN and EWOULDBLOCK for maximum POSIX compatibility
#define CS_EAGAIN EAGAIN
#define coro_poll poll
typedef struct pollfd coro_pollfd;
#endif

// --- Buffer Structure ---

// // Index-based buffer for managing network I/O state
// typedef struct {
//     char *data;         // Pointer to the start of the memory block (allocated by app)
//     size_t capacity;    // Total allocated size of the memory block
//     size_t read_pos;    // Index of the first unread byte (consumed by app or sent)
//     size_t write_pos;   // Index of the next available byte (written by socket)
// } buf_t;


// --- Task Structure (The Coroutine Context) ---

typedef struct Task {
    CSOCKET fd;
    int line;           // Coroutine "Instruction Pointer"
    short events;       // Used by poll/WSAPoll
    void (*handler)(struct Task*);
    void *context;      // Application-provided context pointer

    // I/O Buffers for simultaneous read/write
    buf_t rx_buf;
    buf_t tx_buf;

    // Fields for UDP/Connectionless Operations
    socklen_t addrlen;
    struct sockaddr_storage addr; // Source/Destination address storage
} Task;

// --- Public API ---
void coro_init(void);
void coro_add(CSOCKET fd, void (*handler)(Task*), void *context);
void coro_remove(CSOCKET fd);
void coro_run(void);
void coro_stop(void);
void coro_set_buffer(Task *t, buf_t *rx, buf_t *tx);


// --- CORE COROUTINE MACROS ---

#define CR_START(t) switch((t)->line) { case 0:
// Sets the poll mask and yields control
#define CR_YIELD(t, ev) \
    do { (t)->line = __LINE__; (t)->events = (ev); return; case __LINE__:; } while(0)
#define CR_END(t) } CS_CLOSE((t)->fd); (t)->fd = (CSOCKET)-1; (t)->line = 0;


// --- I/O functions and MACROS ---

/**
 * @brief Reads data into the provided buffer until a frame is reached.  
 * 
 * The frame_func is a function pointer that takes a buf_t* and returns
 * UTIL_OK when a complete frame is available, UTIL_EAGAIN if more data
 * 
 * 
 * 
 * @param t 
 * @param buf 
 * @param frame_func 
 */
static inline util_err_t cr_buf_read(Task *t, buf_t *buf, util_err_t (*frame_func)(buf_t *)) {
    ssize_t r;
    util_err_t rc = UTIL_OK;
    do {
        r = recv(t->fd, buf_write_ptr(buf), buf_write_size(buf), 0);
        if (r < 0) {
            if (errno == CS_EAGAIN || errno == EWOULDBLOCK) {
                return UTIL_EAGAIN;
            } else {
                util_err_t err = util_err_from_errno(errno);
                buf_set_error(buf, err, "socket read");
                return err;
            }
        } else if (r == 0) {
            buf_set_error(buf, UTIL_ECLOSED, "socket closed");
            return UTIL_ECLOSED;
        }

        if(r > buf_write_size(buf)) {
            buf_set_error(buf, UTIL_EBOUNDS, "buffer overflow");
            return UTIL_EBOUNDS;
        }

        buf_write_advance(buf, (size_t)r);
    } while (frame_func && (rc = frame_func(buf)) == UTIL_EAGAIN);

    return rc;
}


static inline util_err_t cr_buf_write(Task *t, buf_t *buf) {
    ssize_t r;
    do {
        r = send(t->fd, buf_read_ptr(buf), buf_read_size(buf), 0);
        if (r < 0) {
            if (errno == CS_EAGAIN || errno == EWOULDBLOCK) {
                return UTIL_EAGAIN;
            } else {
                util_err_t err = util_err_from_errno(errno);
                buf_set_error(buf, err, "socket write");
                return err;
            }
        } else {
            buf_read_advance(buf, (size_t)r);
        }
    } while(buf_read_size(buf));

    return UTIL_OK;
}


// Pseudo-blocking accept() call
static inline util_err_t cr_accept(Task *t, CSOCKET *new_fd) {
    *new_fd = accept(t->fd, NULL, NULL);
    if (*new_fd == (CSOCKET)-1) {
        if (errno == CS_EAGAIN || errno == EWOULDBLOCK) {
            return UTIL_EAGAIN;
        } else {
            return util_err_from_errno(errno);
        }
    }
    return UTIL_OK;
}

// UDP: Pseudo-blocking recvfrom()
static inline util_err_t cr_recvfrom(Task *t, buf_t *buf) {
    ssize_t r;
    t->addrlen = sizeof(t->addr);
    r = recvfrom(t->fd, buf_write_ptr(buf), buf_write_size(buf), 0,
                 (struct sockaddr *)&t->addr, &t->addrlen);
    if (r < 0) {
        if (errno == CS_EAGAIN || errno == EWOULDBLOCK) {
            return UTIL_EAGAIN;
        } else {
            util_err_t err = util_err_from_errno(errno);
            buf_set_error(buf, err, "socket recvfrom");
            return err;
        }
    } else if (r == 0) {
        buf_set_error(buf, UTIL_ECLOSED, "socket closed");
        return UTIL_ECLOSED;
    } else {
        if ((size_t)r > buf_write_size(buf)) {
            buf_set_error(buf, UTIL_EBOUNDS, "buffer overflow");
            return UTIL_EBOUNDS;
        }
        buf_write_advance(buf, (size_t)r);
        return UTIL_OK;  /* UDP is datagram-based; exit after one packet */
    }
}

// UDP: Pseudo-blocking sendto()
static inline util_err_t cr_sendto(Task *t, buf_t *buf) {
    ssize_t r;
    r = sendto(t->fd, buf_read_ptr(buf), buf_read_size(buf), 0,
               (struct sockaddr *)&t->addr, t->addrlen);
    if (r < 0) {
        if (errno == CS_EAGAIN || errno == EWOULDBLOCK) {
            return UTIL_EAGAIN;
        } else {
            util_err_t err = util_err_from_errno(errno);
            buf_set_error(buf, err, "socket sendto");
            return err;
        }
    } else if (r == 0) {
        buf_set_error(buf, UTIL_ECLOSED, "socket closed");
        return UTIL_ECLOSED;
    } else {
        buf_read_advance(buf, (size_t)r);
        return UTIL_OK;
    }
}

#endif



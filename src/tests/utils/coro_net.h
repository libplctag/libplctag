#ifndef CORONET_H
#define CORONET_H

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "buf.h"
#include "log.h"


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


// --- task_t Structure (The Coroutine Context) ---

typedef struct task_t {
    CSOCKET fd;
    int line;           // Coroutine "Instruction Pointer"
    short events;       // Used by poll/WSAPoll
    void (*handler)(struct task_t*);
    void *context;      // Application-provided context pointer

    // I/O Buffers for simultaneous read/write
    buf_t rx_buf;
    buf_t tx_buf;

    // Fields for UDP/Connectionless Operations
    socklen_t addrlen;
    struct sockaddr_storage addr; // Source/Destination address storage
} task_t;

// --- Public API ---
void coro_init(void);
void coro_add(CSOCKET fd, void (*handler)(task_t*), void *context);
void coro_remove(CSOCKET fd);
void coro_run(void);
void coro_stop(void);
void coro_set_buffer(task_t *t, buf_t *rx, buf_t *tx);


// --- CORE COROUTINE MACROS ---

#define CR_START(t) switch((t)->line) { case 0:
// Sets the poll mask and yields control
#define CR_YIELD(t, ev) \
    do { (t)->line = __LINE__; (t)->events = (ev); return; case __LINE__:; } while(0)
#define CR_END(t) } CS_CLOSE((t)->fd); (t)->fd = (CSOCKET)-1; (t)->line = 0;


// --- I/O Function Declarations ---

util_err_t cr_accept(task_t *t, CSOCKET *new_fd);
util_err_t cr_connect(task_t *t, const struct sockaddr *addr, socklen_t addrlen);
util_err_t cr_read(task_t *t, buf_t *buf, util_err_t (*frame_func)(buf_t *));
util_err_t cr_recvfrom(task_t *t, buf_t *buf);
util_err_t cr_sendto(task_t *t, buf_t *buf);
util_err_t cr_write(task_t *t, buf_t *buf);


/** 
 * @brief Simple accept loop: Call cr_accept() repeatedly until a connection is accepted or error.
 *
 * Usage:
 *   util_err_t err;
 *   CSOCKET new_fd;
 *   cr_yield_accept(t, &new_fd, err);
 *   if(err != UTIL_OK) { handle_error(err); }
 *
 * @param t        The task_t pointer (coroutine context)
 * @param new_fd   Pointer to CSOCKET variable to receive new connection fd
 * @param err      Variable to hold the error code (must be util_err_t)
 */
#define cr_yield_accept(t, new_fd, err) \
    while(((err) = cr_accept((t), (new_fd))) == UTIL_EAGAIN) { \
        CR_YIELD((t), POLLIN); \
    }


/**
 * @brief Simple connect loop: Call cr_connect() repeatedly until connected or error.
 * 
 * Usage:
 *  util_err_t err;
 *  struct sockaddr_in addr;
 * // (initialize addr here)
 * cr_yield_connect(t, (struct sockaddr *)&addr, sizeof(addr), err);
 * if(err != UTIL_OK) { handle_error(err); }
 * 
 * @param t        The task_t pointer (coroutine context)
 * @param addr     Pointer to sockaddr structure with target address
 * @param addrlen  Length of the sockaddr structure
 * @param err      Variable to hold the error code (must be util_err_t)
 */
#define cr_yield_connect(t, addr, addrlen, err) \
    while(((err) = cr_connect((t), (addr), (addrlen))) == UTIL_EAGAIN) { \
        CR_YIELD((t), POLLOUT); \
    }

/**
 * @brief Simple read loop: Call cr_read() repeatedly until frame complete or error.
 *
 * Usage:
 *   util_err_t err;
 *   cr_yield_read(t, &buf, frame_func, err);
 *   if(err != UTIL_OK) { handle_error(err); }
 *
 * @param t        The task_t pointer (coroutine context)
 * @param buf      The buf_t pointer to read into
 * @param func     Frame check function pointer
 * @param err      Variable to hold the error code (must be util_err_t)
 */
#define cr_yield_read(t, buf, func, err) \
    while(((err) = cr_read((t), (buf), (func))) == UTIL_EAGAIN) { \
        CR_YIELD((t), POLLIN); \
    }


/** 
 * @brief Simple recvfrom loop: Call cr_recvfrom() repeatedly until data received or error.
 *
 * Usage:
 *   util_err_t err;
 *   cr_yield_recvfrom(t, &buf, err);
 *   if(err != UTIL_OK) { handle_error(err); } 
 * 
 * @param t        The task_t pointer (coroutine context)
 * @param buf      The buf_t pointer to read into
 * @param err      Variable to hold the error code (must be util_err_t) 
 */
#define cr_yield_recvfrom(t, buf, err) \
    while(((err) = cr_recvfrom((t), (buf))) == UTIL_EAGAIN) { \
        CR_YIELD((t), POLLIN); \
    }



/** 
 * @brief Simple sendto loop: Call cr_sendto() repeatedly until data sent or error.
 *
 * Usage:
 *   util_err_t err;
 *   cr_yield_sendto(t, &buf, err);
 *   if(err != UTIL_OK) { handle_error(err); } 
 * 
 * @param t        The task_t pointer (coroutine context)
 * @param buf      The buf_t pointer to write from
 * @param err      Variable to hold the error code (must be util_err_t) 
 */
#define cr_yield_sendto(t, buf, err) \
    while(((err) = cr_sendto((t), (buf))) == UTIL_EAGAIN) { \
        CR_YIELD((t), POLLOUT); \
    }


/**
 * @brief Simple write loop: Call cr_write() repeatedly until all data sent or error.
 *
 * Usage:
 *   util_err_t err;
 *   cr_yield_write(t, &buf, err);
 *   if(err != UTIL_OK) { handle_error(err); }
 *
 * @param t        The task_t pointer (coroutine context)
 * @param buf      The buf_t pointer to write from
 * @param err      Variable to hold the error code (must be util_err_t)
 */
#define cr_yield_write(t, buf, err) \
    while(((err) = cr_write((t), (buf))) == UTIL_EAGAIN) { \
        CR_YIELD((t), POLLOUT); \
    }




#endif



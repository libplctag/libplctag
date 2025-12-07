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


// --- I/O MACROS ---

// Reads data into the provided buffer until capacity is reached.
#define CR_BUF_READ(t, buf) \
    do { \
        ssize_t r; \
        while ((buf)->write_pos < (buf)->capacity) { \
            size_t bytes_to_read = (buf)->capacity - (buf)->write_pos; \
            r = recv((t)->fd, (buf)->data + (buf)->write_pos, bytes_to_read, 0); \
            if (r < 0) { \
                if (errno == CS_EAGAIN || errno == EWOULDBLOCK) { CR_YIELD(t, POLLIN); } \
                else break; /* Error */ \
            } else if (r == 0) { \
                break; /* EOF/Disconnection */ \
            } \
            (buf)->write_pos += r; \
        } \
    } while(0)

// Sends data from the buffer until all data (up to write_pos) is sent.
#define CR_BUF_WRITE(t, buf) \
    do { \
        ssize_t r; \
        while ((buf)->read_pos < (buf)->write_pos) { \
            size_t bytes_to_send = (buf)->write_pos - (buf)->read_pos; \
            r = send((t)->fd, (buf)->data + (buf)->read_pos, bytes_to_send, 0); \
            if (r < 0) { \
                if (errno == CS_EAGAIN || errno == EWOULDBLOCK) { CR_YIELD(t, POLLOUT); } \
                else break; /* Error */ \
            } else if (r == 0) { \
                break; /* Error/Disconnection */ \
            } \
            (buf)->read_pos += r; \
        } \
    } while(0)

// Pseudo-blocking accept() call
#define CR_ACCEPT(t, new_fd) \
    do { \
        do { \
            (new_fd) = accept((t)->fd, NULL, NULL); \
            if ((new_fd) == (CSOCKET)-1 && (errno == CS_EAGAIN || errno == EWOULDBLOCK)) { \
                CR_YIELD(t, POLLIN); \
            } else break; \
        } while(1); \
    } while(0)

// UDP: Pseudo-blocking recvfrom()
#define CR_RECVFROM(t, buf) \
    do { \
        ssize_t r; \
        (t)->addrlen = sizeof((t)->addr); \
        while ((buf)->write_pos < (buf)->capacity) { \
            size_t bytes_to_read = (buf)->capacity - (buf)->write_pos; \
            r = recvfrom((t)->fd, (buf)->data + (buf)->write_pos, bytes_to_read, 0, \
                         (struct sockaddr *)&(t)->addr, &(t)->addrlen); \
            if (r < 0) { \
                if (errno == CS_EAGAIN || errno == EWOULDBLOCK) { CR_YIELD(t, POLLIN); } \
                else break; /* Error */ \
            } else { \
                (buf)->write_pos += r; \
                break; /* UDP is datagram-based; exit after one packet */ \
            } \
        } \
    } while(0)

// UDP: Pseudo-blocking sendto()
#define CR_SENDTO(t, buf) \
    do { \
        ssize_t r; \
        while ((buf)->read_pos < (buf)->write_pos) { \
            size_t bytes_to_send = (buf)->write_pos - (buf)->read_pos; \
            r = sendto((t)->fd, (buf)->data + (buf)->read_pos, bytes_to_send, 0, \
                       (struct sockaddr *)&(t)->addr, (t)->addrlen); \
            if (r < 0) { \
                if (errno == CS_EAGAIN || errno == EWOULDBLOCK) { CR_YIELD(t, POLLOUT); } \
                else break; /* Error */ \
            } else { \
                (buf)->read_pos += r; \
            } \
        } \
    } while(0)

#endif



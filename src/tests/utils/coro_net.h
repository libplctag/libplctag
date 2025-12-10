#ifndef CORONET_H
#define CORONET_H

/**
 * @file coro_net.h
 * @brief Coroutine-based network I/O library
 *
 * IMPORTANT: This library is NOT thread-safe. All functions operating on a
 * coro_net_t instance must be called from the same thread, or the caller must
 * provide external synchronization. Typically, coro_run() is called in a
 * dedicated thread and all task operations happen within handlers
 * invoked by that thread.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include "buf.h"
#include "log.h"
#include "socket.h"

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #define CS_CLOSE(fd) closesocket(fd)
    #define CS_EAGAIN WSAEWOULDBLOCK
    #define coro_poll WSAPoll
    typedef WSAPOLLFD coro_pollfd;
    #define INVALID_SOCKET_FD INVALID_SOCKET
#else
    #include <poll.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <sys/socket.h>
    #define CS_CLOSE(fd) close(fd)
    #define CS_EAGAIN EAGAIN
    #define coro_poll poll
    typedef struct pollfd coro_pollfd;
    #define INVALID_SOCKET_FD -1
#endif

/* opaque types */
typedef struct coro_net_t coro_net_t;

/* Task event constants */
#define CORO_NO_SOCKET 0
#define CORO_EVENT_ALWAYS ((short)-1) /* poll events are of type short */
#define CORO_EVENT_NONE 0
#define CORO_EVENT_READ POLLIN
#define CORO_EVENT_WRITE POLLOUT
#define CORO_EVENT_CONNECT POLLOUT
#define CORO_EVENT_ACCEPT POLLIN

/* Handle types */
typedef struct coro_task_handle_t {
    coro_net_t *coro_net;
    int index;
} coro_task_handle_t;

/* --- Public API --- */

/**
 * @brief Create a coroutine network event loop
 * 
 * @param out_coro_net Pointer to receive the created coro_net_t instance
 * @param max_tasks Maximum number of concurrent tasks (max 256)
 * @return UTIL_OK on success, error code otherwise
 */
util_err_t coro_create(coro_net_t **out_coro_net, size_t max_tasks);

/**
 * @brief a coroutine network event loop
 * 
 * @param coro_net The coro_net_t instance to destroy. pointer to pointer to 
 * allow NULLing out after destruction.
 */
void coro_destroy(coro_net_t **coro_net);

/**
 * @brief the coroutine event loop (blocking until coro_stop is called)
 * 
 * @param coro_net The coro_net_t instance. Pointer to pointer to allow NULLing out after destruction.
 * @param tick_interval_ms Polling tick interval in milliseconds
 * @return UTIL_OK on success, error code otherwise
 */
util_err_t coro_run(coro_net_t **coro_net, uint32_t tick_interval_ms);

/**
 * @brief Stop the coroutine event loop
 * 
 * @param coro_net The coro_net_t instance
 * @return UTIL_OK on success, error code otherwise
 */
util_err_t coro_stop(coro_net_t *coro_net);

/**
 * @brief a task to the event loop
 * 
 * All tasks are added to the event loop with an initial event of CORO_EVENT_ALWAYS.
 * This means the task's handler will be called every loop cycle until it changes its event.
 * It is done this way so that each handler can do its own setup and then set the appropriate event
 * to wait for (e.g., CORO_EVENT_READ for read, CORO_EVENT_WRITE for write).
 * 
 * @param task Output parameter for the task handle
 * @param coro_net The coro_net_t instance
 * @param fd Socket file descriptor (CORO_NO_SOCKET for socket-less tasks)
 * @param handler Function called when task event occurs or every loop cycle for ALWAYS tasks
 * @param context User-supplied context pointer passed to handler
 * @return UTIL_OK on success, error code otherwise
 */
util_err_t coro_add_task(coro_task_handle_t *task,
                         coro_net_t *coro_net,
                         socket_t fd,
                         void (*handler)(coro_task_handle_t task, socket_t fd, void *context),
                         void *context);

/**
 * @brief a task from the event loop
 * 
 * @param task The task handle to remove
 */
void coro_remove_task(coro_task_handle_t task);



/* Accessors for macros */

/**
 * @brief Set the event(s) this task is waiting for
 * 
 * This should generally not need to be called directly by user code, as the
 * coro_wait_for_event macro handles this automatically. However, it can be used for
 * advanced scenarios where the task needs to change its event outside of the
 * coroutine handler.
 * 
 * @param task The task handle
 * @param event Event mask (CORO_EVENT_ALWAYS, POLLIN, POLLOUT, POLLIN|POLLOUT, etc.)
 * @return UTIL_OK on success, error code otherwise
 */
util_err_t coro_set_task_event(coro_task_handle_t task, short event);


/**
 * Get the current line number for a task's coroutine
 * (Used by CORO_START/coro_wait_for_event macros for switch statement)
 */
int coro_get_line(coro_task_handle_t task);

/**
 * Set the line number for a task's coroutine
 * (Used by coro_wait_for_event macro)
 */
util_err_t coro_set_line(coro_task_handle_t task, int line);

/**
 * Get the socket FD for a task
 */
socket_t coro_get_fd(coro_task_handle_t task);

/* --- Coroutine Macros --- */

/**
 * Start a coroutine handler function
 * Usage:
 *   void my_handler(coro_task_handle_t task, socket_t fd, void *context) {
 *       CORO_START(task);
 *       // ... coroutine code
 *       CORO_END(task);
 *   }
 */
#define CORO_START(task) switch(coro_get_line(task)) { case 0:

/**
 * Yield execution until a task event occurs
 * Usage:
 *   coro_wait_for_event(task, POLLIN);        // Wait for socket read
 *   coro_wait_for_event(task, POLLOUT);       // Wait for socket write
 *   coro_wait_for_event(task, CORO_EVENT_ALWAYS);  // Run next loop cycle
 */
#define coro_wait_for_event(task, ev) \
    do { \
        coro_set_line((task), __LINE__); \
        coro_set_task_event((task), (ev)); \
        return; \
        case __LINE__:; \
    } while(0)

/**
 * End a coroutine handler function
 * This removes the task from the event loop
 */
#define CORO_END(task) default: break; } coro_remove_task(task);


/**
 * Accept a connection with automatic retry on EAGAIN
 * Usage:
 *   socket_t client_fd;
 *   socket_address_t client_addr;
 *   socket_accept_yield(task, &client_fd, &client_addr, err);
 */
#define socket_accept_yield(task, client_fd_ptr, client_addr, err) \
    do { \
        socket_t __listen_fd; \
        util_err_t __err; \
        do { \
            __listen_fd = coro_get_fd(task); \
            pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_DETAIL, "Attempting to accept connection on fd=%d", (int)__listen_fd); \
            __err = socket_accept(__listen_fd, (client_fd_ptr), (client_addr)); \
            pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_DETAIL, "Accept returned %s", util_err_str(__err)); \
            if (__err == UTIL_EAGAIN) { \
                pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_DETAIL, "Yielding on accept for fd=%d", (int)__listen_fd); \
                coro_wait_for_event((task), CORO_EVENT_ACCEPT); \
                __err = UTIL_EAGAIN; /* Re-initialize after yield for loop condition */ \
            } \
        } while (__err == UTIL_EAGAIN); \
        (err) = __err; \
    } while(0)

/**
 * Connect with automatic retry on EAGAIN
 * Usage:
 *   socket_address_t addr;
 *   socket_connect_yield(task, &addr, err);
 */
#define socket_connect_yield(task, address, err) \
    do { \
        socket_t __fd; \
        util_err_t __err; \
        do { \
            __fd = coro_get_fd(task); \
            __err = socket_connect(__fd, (address)); \
            if (__err == UTIL_EAGAIN) { \
                coro_wait_for_event((task), CORO_EVENT_CONNECT); \
                __err = UTIL_EAGAIN; /* Re-initialize after yield for loop condition */ \
            } \
        } while (__err == UTIL_EAGAIN); \
        (err) = __err; \
    } while(0)

/**
 * Read with automatic retry until frame complete
 * Usage:
 *   int64_t first_byte_ts = 0, complete_ts = 0;
 *   socket_read_yield(task, &buf, frame_check_func, ctx, &first_byte_ts, &complete_ts, err);
 *
 * @param first_byte_ts_ptr Optional pointer to timestamp (int64_t*) - set when first data arrives (can be NULL)
 * @param complete_ts_ptr Optional pointer to timestamp (int64_t*) - set when frame complete (can be NULL)
 */
#define socket_read_yield(task, buf, frame_func, ctx, first_byte_ts_ptr, complete_ts_ptr, err) \
    do { \
        socket_t __fd; \
        util_err_t __err; \
        do { \
            __fd = coro_get_fd(task); \
            __err = socket_recv_buf(__fd, (buf)); \
            if(__err == UTIL_OK) { \
                pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_SPEW, "Read %zu bytes from fd=%d", buf_read_size(buf), (int)__fd); \
                /* Capture first byte timestamp if requested and not already set */ \
                if ((first_byte_ts_ptr) && (*(first_byte_ts_ptr) == 0)) { \
                    *(first_byte_ts_ptr) = util_time_us(); \
                } \
                __err = (frame_func)((buf), (ctx)); \
                /* Capture frame complete timestamp if frame check succeeded */ \
                if (__err == UTIL_OK && (complete_ts_ptr)) { \
                    *(complete_ts_ptr) = util_time_us(); \
                } \
            } \
            if( __err == UTIL_EAGAIN) { \
                coro_wait_for_event((task), CORO_EVENT_READ); \
                __err = UTIL_EAGAIN; /* Re-initialize after yield for loop condition */ \
            } \
        } while (__err == UTIL_EAGAIN); \
        (err) = __err; \
    } while(0)

/**
 * Receive datagram with automatic retry on EAGAIN
 * Usage:
 *   socket_address_t from_addr;
 *   socket_recvfrom_yield(task, &buf, &from_addr, err);
 */
#define socket_recvfrom_yield(task, buf, from_addr, err) \
    do { \
        socket_t __fd; \
        util_err_t __err; \
        do { \
            __fd = coro_get_fd(task); \
            __err = socket_recvfrom_buf(__fd, (from_addr), (buf)); \
            if (__err == UTIL_EAGAIN) { \
                coro_wait_for_event((task), CORO_EVENT_READ); \
                __err = UTIL_EAGAIN; /* Re-initialize after yield for loop condition */ \
            } \
        } while (__err == UTIL_EAGAIN); \
        (err) = __err; \
    } while(0)

/**
 * Send datagram with automatic retry on EAGAIN
 * Usage:
 *   socket_address_t to_addr;
 *   socket_sendto_yield(task, &buf, &to_addr, err);
 */
#define socket_sendto_yield(task, buf, to_addr, err) \
    do { \
        socket_t __fd; \
        util_err_t __err; \
        do { \
            __fd = coro_get_fd(task); \
            __err = socket_sendto_buf(__fd, (to_addr), (buf)); \
            if (__err == UTIL_EAGAIN) { \
                coro_wait_for_event((task), CORO_EVENT_WRITE); \
                __err = UTIL_EAGAIN; /* Re-initialize after yield for loop condition */ \
            } \
        } while (__err == UTIL_EAGAIN); \
        (err) = __err; \
    } while(0)

/**
 * Write with automatic retry on EAGAIN
 * Usage:
 *   socket_write_yield(task, &buf, err);
 */
#define socket_write_yield(task, buf, err) \
    do { \
        socket_t __fd; \
        util_err_t __err; \
        __err = UTIL_OK; \
        while (buf_read_size((buf)) > 0) { \
            __fd = coro_get_fd(task); \
            __err = socket_send_buf(__fd, (buf)); \
            if (__err == UTIL_EAGAIN) { \
                coro_wait_for_event((task), CORO_EVENT_WRITE); \
                __err = UTIL_EAGAIN; /* Re-initialize after yield for loop condition */ \
            } else if (__err != UTIL_OK) { \
                break; \
            } \
        } \
        (err) = __err; \
    } while(0)

#endif



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
 *   coro_yield(task, CORO_EVENT_READ);        // Wait for socket read
 *   coro_yield(task, CORO_EVENT_WRITE);       // Wait for socket write
 *   coro_yield(task, CORO_EVENT_ALWAYS);      // Run next loop cycle
 */
#define coro_yield(task, event) \
    do { \
        coro_set_line((task), __LINE__); \
        coro_set_task_event((task), (event)); \
        return; \
        case __LINE__:; \
    } while(0)

/**
 * End a coroutine handler function
 * This removes the task from the event loop
 */
#define CORO_END(task) default: break; } coro_remove_task(task);



#endif



/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever     *
 * you choose.                                                             *
 *                                                                         *
 * MPL 2.0:                                                                *
 *                                                                         *
 *   This Source Code Form is subject to the terms of the Mozilla Public   *
 *   License, v. 2.0. If a copy of the MPL was not distributed with this   *
 *   file, You can obtain one at http://mozilla.org/MPL/2.0/.              *
 *                                                                         *
 *                                                                         *
 * LGPL 2:                                                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../utils/buf.h"
#include "../utils/err.h"


/*******************************************************************************
 *******************************************************************************
 ***
 *** Opaque Types
 ***
 *******************************************************************************
 ******************************************************************************/

/** Opaque core reactor type. */
typedef struct pt_net_core_s pt_net_core_t;

/** Opaque socket type. */
typedef struct pt_net_socket_s pt_net_socket_t;

/** Opaque protothread type. */
typedef struct pt_net_thread_s pt_net_thread_t;


/*******************************************************************************
 *******************************************************************************
 ***
 *** Public Types
 ***
 *******************************************************************************
 ******************************************************************************/

/**
 * @brief Socket address structure.
 *
 * Holds a hostname/IP string and port for addressing.
 */
typedef struct pt_net_addr_s {
    char host[64];
    uint16_t port;
} pt_net_addr_t;

/**
 * @brief Framer callback function type.
 *
 * A framer inspects the input buffer and determines if a complete
 * protocol frame is available.
 *
 * @param input   Buffer containing received data.
 * @param context User context.
 * @return UTIL_OK if complete frame available, UTIL_EBOUNDS if more data needed.
 */
typedef util_err_t (*pt_net_framer_fn)(buf_t *input, void *context);


/*******************************************************************************
 *******************************************************************************
 ***
 *** Protothread Macros
 ***
 *******************************************************************************
 ******************************************************************************/

/**
 * @brief Declares/defines a protothread function signature.
 *
 * Usage:
 *   static PT_NET_FUNC(my_handler);           // declaration
 *   PT_NET_FUNC(my_handler) { ... }           // definition
 */
#define PT_NET_FUNC(name) \
    util_err_t name(pt_net_thread_t *this_pt, void *context)

/**
 * @brief Marks the start of a protothread function body.
 *
 * Must appear at the beginning of the function, after local variable declarations.
 * Works with Duff's device or similar continuation mechanism.
 */
#define PT_NET_FUNC_BODY_START \
    switch (this_pt->line) { \
    case 0:

/**
 * @brief Marks the end of a protothread function body.
 */
#define PT_NET_FUNC_BODY_END \
    } \
    return UTIL_OK


/*******************************************************************************
 *******************************************************************************
 ***
 *** Internal Protothread Macros (used by other macros)
 ***
 *******************************************************************************
 ******************************************************************************/

/**
 * @brief Save continuation point and yield.
 */
#define PT_NET_YIELD() \
    do { \
        this_pt->line = __LINE__; \
        return UTIL_EAGAIN; \
        case __LINE__:; \
    } while (0)

/**
 * @brief Yield if operation returned EAGAIN, otherwise continue.
 */
#define PT_NET_YIELD_ON_EAGAIN(err) \
    do { \
        if ((err) == UTIL_EAGAIN) { \
            PT_NET_YIELD(); \
        } \
    } while (0)


/*******************************************************************************
 *******************************************************************************
 ***
 *** Core Lifecycle Functions
 ***
 *******************************************************************************
 ******************************************************************************/

/**
 * @brief Create a pt_net core reactor.
 *
 * @param core      Pointer to a pointer to the core reactor.
 * @param max_socks Maximum concurrent sockets (for static allocation).
 * @param max_pts   Maximum concurrent protothreads.
 * @return UTIL_OK on success.
 */
util_err_t pt_net_core_create(pt_net_core_t **core, size_t max_socks, size_t max_pts);

/**
 * @brief Run the reactor loop until all protothreads complete or error.
 *
 * @param core The core reactor.
 * @return UTIL_OK or error.
 */
util_err_t pt_net_core_run(pt_net_core_t *core);

/**
 * @brief Signal shutdown and clean up all resources.
 *
 * @param core The core reactor.
 */
void pt_net_core_shutdown(pt_net_core_t *core);


/*******************************************************************************
 *******************************************************************************
 ***
 *** Generic socket functions
 ***
 *******************************************************************************
 ******************************************************************************/


/**
 * @brief Close a socket.
 * 
 * Closes the passed socket and releases all associated resources.  Removes the
 * socket from the reactor it is registered with if any.
 * 
 * @param core The core reactor.
 * @param sock The socket to close.
 * @return util_err_t 
 */
util_err_t pt_net_close_socket(pt_net_socket_t *sock);


/*******************************************************************************
 *******************************************************************************
 ***
 *** Address Functions
 ***
 *******************************************************************************
 ******************************************************************************/

/**
 * @brief Initialize an address structure.
 * 
 * Note: host may only be an IP address string.  A NULL or empty string indicates
 * INADDR_ANY.  A zero port indicates any port.
 * 
 * @param addr IP address structure to initialize.
 * @param host IP address as a string.
 * @param port Port number.
 * @return util_err_t 
 */
util_err_t pt_net_init_addr(pt_net_addr_t *addr, const char *host, uint16_t port);

/**
 * @brief Get the local address of a socket.
 * 
 * @param addr pointer to output address structure.
 * @param sock pointer to the socket.
 * @return util_err_t 
 */
util_err_t pt_net_get_local_socket_addr(pt_net_addr_t *addr, pt_net_socket_t *sock);


/**
 * @brief Get the remote address of a socket.
 * 
 * @param addr pointer to output address structure.
 * @param sock pointer to the socket.
 * @return util_err_t 
 */
util_err_t pt_net_get_remote_socket_addr(pt_net_addr_t *addr, pt_net_socket_t *sock);


/*******************************************************************************
 *******************************************************************************
 ***
 *** TCP Server Functions
 ***
 *******************************************************************************
 ******************************************************************************/

/**
 * @brief Open a TCP listener socket.
 *
 * @param sock    Output socket handle.
 * @param core    The core reactor.
 * @param addr    pointer to a pt_addr_t or NULL for INADDR_ANY.
 * @param framer  Optional framer for accepted connections (may be NULL).
 * @param context User context passed to framer.
 * @return UTIL_OK on success.
 */
util_err_t pt_net_open_tcp_listener(
    pt_net_socket_t **sock,
    pt_net_core_t *core,
    pt_net_addr_t *addr,
    pt_net_framer_fn framer,
    void *context
);


/*******************************************************************************
 *******************************************************************************
 ***
 *** TCP Server Macros (suspend on block)
 ***
 *******************************************************************************
 ******************************************************************************/

/**
 * @brief Accept a TCP connection.
 *
 * Suspends the protothread if no connection is pending.
 *
 * @param client_sock   Output: accepted client socket.
 * @param listener_sock The listener socket.
 * @return UTIL_OK on success, error on failure.
 */
#define pt_net_accept_tcp_connection(client_sock, listener_sock) \
    do { \
        util_err_t _pt_err; \
        do { \
            _pt_err = pt_net_try_accept_tcp(client_sock, listener_sock); \
            PT_NET_YIELD_ON_EAGAIN(_pt_err); \
        } while (_pt_err == UTIL_EAGAIN); \
        if (_pt_err != UTIL_OK) return _pt_err; \
    } while (0)


/*******************************************************************************
 *******************************************************************************
 ***
 *** TCP Client Macros (suspend on block)
 ***
 *******************************************************************************
 ******************************************************************************/

/**
 * @brief Connect to a remote TCP server.
 *
 * Suspends until connected or error.
 *
 * @param sock    Output socket handle.
 * @param core    The core reactor.
 * @param addr    Pointer to pt_addr_t.
 * @return UTIL_OK on success, UTIL_ECONNREFUSED, UTIL_ETIMEOUT, etc.
 */
#define pt_net_connect_tcp(sock, core, addr) \
    do { \
        util_err_t _pt_err; \
        do { \
            _pt_err = pt_net_try_connect_tcp(sock, core, addr); \
            PT_NET_YIELD_ON_EAGAIN(_pt_err); \
        } while (_pt_err == UTIL_EAGAIN); \
        if (_pt_err != UTIL_OK) return _pt_err; \
    } while (0)


/*******************************************************************************
 *******************************************************************************
 ***
 *** TCP I/O Macros (suspend on block)
 ***
 *******************************************************************************
 ******************************************************************************/


/**
 * @brief Read a complete frame using the socket's framer.
 *
 * Suspends until framer returns UTIL_OK indicating complete frame.  If the framer
 * function pointer is NULL, reads until buffer is full or we get an EAGAIN error.
 * Advances the buffer write cursor.
 *
 * @param buf  Buffer to read into (framer checks for complete frame).
 * @param sock The socket.
 * @param framer The framer function to determine frame boundaries.
 * @param context User context passed to framer.
 * @return UTIL_OK when complete frame available.
 */
#define pt_net_recv_tcp(buf, sock, framer, context) \
    do { \
        util_err_t _pt_err; \
        do { \
            _pt_err = pt_net_try_recv_tcp(buf, sock, framer, context); \
            PT_NET_YIELD_ON_EAGAIN(_pt_err); \
        } while (_pt_err == UTIL_EAGAIN); \
        if (_pt_err != UTIL_OK) return _pt_err; \
    } while (0)

/**
 * @brief Write data to a TCP socket.
 *
 * Writes from buf_read_ptr, up to buf_read_size bytes. Advances buf read cursor.
 * Suspends if socket buffer full. Returns when all data written.
 *
 * @param buf           Buffer to write from.
 * @param sock          The socket.
 * @return UTIL_OK on success.
 */
#define pt_net_send_tcp(buf, sock) \
    do { \
        util_err_t _pt_err; \
        do { \
            _pt_err = pt_net_try_send_tcp(buf, sock); \
            PT_NET_YIELD_ON_EAGAIN(_pt_err); \
        } while (_pt_err == UTIL_EAGAIN); \
        if (_pt_err != UTIL_OK) return _pt_err; \
    } while (0)


/*******************************************************************************
 *******************************************************************************
 ***
 *** UDP Functions
 ***
 *******************************************************************************
 ******************************************************************************/

/**
 * @brief Open a UDP socket.
 *
 * @param sock      Output socket handle.
 * @param core      The core reactor.
 * @param bind_addr Local address to bind (NULL for any).
 * @return UTIL_OK on success.
 */
util_err_t pt_net_open_udp(
    pt_net_socket_t **sock,
    pt_net_core_t *core,
    pt_net_addr_t *bind_addr
);

/**
 * @brief Enable broadcast on a UDP socket.
 *
 * Must be called before sending to broadcast addresses.
 *
 * @param sock The UDP socket.
 * @return UTIL_OK on success.
 */
util_err_t pt_net_udp_enable_broadcast(pt_net_socket_t *sock);

/**
 * @brief Join a multicast group.
 *
 * @param sock           The UDP socket.
 * @param multicast_addr Multicast group address (e.g., "239.1.2.3").
 * @param interface_addr Local interface address (NULL for default).
 * @return UTIL_OK on success.
 */
util_err_t pt_net_udp_join_multicast(
    pt_net_socket_t *sock,
    pt_net_addr_t *multicast_addr,
    pt_net_addr_t *interface_addr
);

/**
 * @brief Leave a multicast group.
 *
 * @param sock           The UDP socket.
 * @param multicast_addr Multicast group address.
 * @param interface_addr Local interface address (NULL for default).
 * @return UTIL_OK on success.
 */
util_err_t pt_net_udp_leave_multicast(
    pt_net_socket_t *sock,
    pt_net_addr_t *multicast_addr,
    pt_net_addr_t *interface_addr
);


/*******************************************************************************
 *******************************************************************************
 ***
 *** UDP I/O Macros (suspend on block)
 ***
 *******************************************************************************
 ******************************************************************************/

/**
 * @brief Receive a UDP datagram.
 *
 * Suspends if no datagram available.  Reads the whole datagram up to buffer size.
 * If there is more data than fits in the buffer, then return UTIL_EBOUNDS and
 * discard the excess data.
 *
 * @param buf        Buffer to receive into (write cursor advanced).
 * @param from_addr  Output: source address (may be NULL).
 * @param sock       The UDP socket.
 * @return UTIL_OK on success.
 */
#define pt_net_recvfrom_udp(buf, from_addr, sock) \
    do { \
        util_err_t _pt_err; \
        do { \
            _pt_err = pt_net_try_recvfrom_udp(buf, from_addr, sock); \
            PT_NET_YIELD_ON_EAGAIN(_pt_err); \
        } while (_pt_err == UTIL_EAGAIN); \
        if (_pt_err != UTIL_OK) return _pt_err; \
    } while (0)

/**
 * @brief Send a UDP datagram.
 *
 * Suspends if socket buffer full.  Will send the entire contents of the buffer,
 * or return an error.
 *
 * @param buf        Buffer containing datagram (read cursor advanced).
 * @param sock       The UDP socket.
 * @param to_addr    Destination address.
 * @return UTIL_OK on success.
 */
#define pt_net_sendto_udp(buf, sock, to_addr) \
    do { \
        util_err_t _pt_err; \
        do { \
            _pt_err = pt_net_try_sendto_udp(buf, sock, to_addr); \
            PT_NET_YIELD_ON_EAGAIN(_pt_err); \
        } while (_pt_err == UTIL_EAGAIN); \
        if (_pt_err != UTIL_OK) return _pt_err; \
    } while (0)



/*******************************************************************************
 *******************************************************************************
 ***
 *** Protothread Control Functions
 ***
 *******************************************************************************
 ******************************************************************************/

/**
 * @brief Spawn a new protothread.
 *
 * @param pt      Output thread handle (may be NULL if not needed).
 * @param core    The core reactor.
 * @param func    The protothread function.
 * @param context User context passed to thread function.
 * @param name    Can be null.  If not NULL, takes ownership of the string.
 * @return UTIL_OK on success, UTIL_ERESOURCE if max threads reached.
 */
util_err_t pt_net_spawn_thread(
    pt_net_thread_t **pt,
    pt_net_core_t *core,
    util_err_t (*func)(pt_net_thread_t *, void *),
    void *context,
    const char *name
);

/**
 * @brief Wake a suspended protothread.
 *
 * @param pt   The thread to wake.
 * @return UTIL_OK on success.
 */
util_err_t pt_net_wake_thread(pt_net_thread_t *pt);
/**
 * @brief Request a protothread to terminate.
 *
 * Sets a cancellation flag. The thread must check with pt_net_is_cancelled()
 * and handle cancellation appropriately.
 *
 * @param pt   The thread to cancel.
 * @return UTIL_OK on success.
 */
util_err_t pt_net_cancel_thread( pt_net_thread_t *pt);

/**
 * @brief Check if cancellation was requested.
 *
 * @param this_pt The current protothread.
 * @return true if cancellation requested.
 */
bool pt_net_is_cancelled(pt_net_thread_t *this_pt);


/*******************************************************************************
 *******************************************************************************
 ***
 *** Protothread Control Macros (suspend on block)
 ***
 *******************************************************************************
 ******************************************************************************/

/**
 * @brief Suspend current protothread until explicitly woken.
 *
 * @param this_pt The current protothread.
 */
#define pt_net_suspend_thread(this_pt) \
    do { \
        (this_pt)->suspended = true; \
        (this_pt)->state = PT_NET_THREAD_SUSPENDED; \
        PT_NET_YIELD(); \
    } while (0)

/**
 * @brief Wait for a protothread to complete.
 *
 * Suspends until the target thread exits.
 *
 * @param pt   The thread to wait for.
 * @return Exit status of target thread.
 */
#define pt_net_join_thread(pt) \
    do { \
        util_err_t _pt_err; \
        do { \
            _pt_err = pt_net_try_join_thread( pt); \
            PT_NET_YIELD_ON_EAGAIN(_pt_err); \
        } while (_pt_err == UTIL_EAGAIN); \
        if (_pt_err != UTIL_OK) return _pt_err; \
    } while (0)


/*******************************************************************************
 *******************************************************************************
 ***
 *** Timer Macros (suspend on block)
 ***
 *******************************************************************************
 ******************************************************************************/

/**
 * @brief Sleep for a duration.
 *
 * Suspends the protothread for the specified time.
 *
 * @param ms   Milliseconds to sleep.
 */
#define pt_net_sleep( ms) \
    do { \
        util_err_t _pt_err; \
        do { \
            _pt_err = pt_net_try_sleep(this_pt, ms); \
            PT_NET_YIELD_ON_EAGAIN(_pt_err); \
        } while (_pt_err == UTIL_EAGAIN); \
    } while (0)



/*******************************************************************************
 *******************************************************************************
 ***
 *** Internal Functions (Try functions - return UTIL_EAGAIN if would block)
 ***
 *******************************************************************************
 ******************************************************************************/

/* Try functions - return UTIL_EAGAIN if would block */
util_err_t pt_net_try_accept_tcp(pt_net_socket_t **client_sock, pt_net_socket_t *listener_sock);
util_err_t pt_net_try_connect_tcp(pt_net_socket_t **sock, pt_net_core_t *core, pt_net_addr_t *addr);
util_err_t pt_net_try_recv_tcp(buf_t *buf, pt_net_socket_t *sock, pt_net_framer_fn framer, void *context);
util_err_t pt_net_try_send_tcp(buf_t *buf, pt_net_socket_t *sock);

util_err_t pt_net_try_recvfrom_udp(buf_t *buf, pt_net_addr_t *from_addr, pt_net_socket_t *sock);
util_err_t pt_net_try_sendto_udp(buf_t *buf, pt_net_socket_t *sock, const pt_net_addr_t *to_addr);

util_err_t pt_net_try_join_thread(pt_net_thread_t *pt);
util_err_t pt_net_try_sleep(pt_net_thread_t *this_pt, uint32_t ms);


#ifdef __cplusplus
}
#endif

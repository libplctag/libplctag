#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "err.h"
#include "buf.h"

/* Cross-platform socket type */
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET socket_t;
  #ifndef INVALID_SOCKET
  #define INVALID_SOCKET ((SOCKET)(~0))
  #endif
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  typedef int socket_t;
  #ifndef INVALID_SOCKET
  #define INVALID_SOCKET ((socket_t)-1)
  #endif
#endif

/**
 * @brief Socket module functions.
 * 
 * All socket operations will never generate signals (e.g. SIGPIPE on Unix).
 * 
 * When socket operations are called on non-blocking sockets, they may return
 * UTIL_EAGAIN to indicate the operation would block.  The caller should retry the operation
 * when the socket is ready (e.g. using an event loop or reactor).
 */


/**
 * @brief Initialize the socket module.
 *
 * This function must be called before using any other socket functions.
 *
 * @return util_err_t
 */
util_err_t socket_init(void);

/**
 * @brief Cleanup the socket module.
 *
 * This function should be called when socket operations are no longer needed,
 * to release any resources allocated by the module.
 */
void socket_cleanup(void);

/**
 * @brief Get the last socket error and translate it to util_err_t.
 *
 * On Windows, gets WSAGetLastError() and translates it.
 * On POSIX systems, gets errno and translates it.
 *
 * @return util_err_t - The translated error code
 */
util_err_t socket_get_err(void);

/* Address manipulation */

typedef struct {
    struct sockaddr_storage addr;
    socklen_t               addr_len;
} socket_address_t;

/**
 * @brief Convert a string representation of an IP address and port to a socket_address_t.
 * 
 * @param out_addr - Pointer to the socket_address_t to populate
 * @param address_str - String representation of the IP address (dotted-quad or hostname)
 * @param port - Port number
 * @return util_err_t - UTIL_OK on success, error code on failure.
 */
util_err_t socket_address_init(socket_address_t *out_addr, const char *address_str, uint16_t port);

/**
 * @brief Convert a socket_address_t to a string representation.
 * 
 * @param addr - Pointer to the socket_address_t to convert
 * @param out_str - Buffer to store the string representation
 * @param out_str_cap - Capacity of the output buffer
 * @return util_err_t - UTIL_OK on success, error code on failure.
 */
util_err_t socket_address_get_addr_str(const socket_address_t *addr, char *out_str, size_t out_str_cap);

/**
 * @brief Get the port number from a socket_address_t.
 * 
 * @param addr - Pointer to the socket_address_t
 * @return uint16_t - Port number in host byte order
 */
uint16_t socket_address_get_port(const socket_address_t *addr);

/**
 * @brief Create a TCP socket.
 *
 * The socket is created in blocking mode. To use with a reactor, call
 * socket_set_nonblocking(sock, true) or add the socket to a reactor via
 * reactor_add_socket(), which automatically sets it to non-blocking.
 *
 * @return socket_t - Created socket, or INVALID_SOCKET on failure.
 */
socket_t socket_create_tcp(void);

/**
 * @brief Create a UDP socket.
 *
 * The socket is created in blocking mode. To use with a reactor, call
 * socket_set_nonblocking(sock, true) or add the socket to a reactor via
 * reactor_add_socket(), which automatically sets it to non-blocking.
 *
 * @return socket_t - Created socket, or INVALID_SOCKET on failure.
 */
socket_t socket_create_udp(void);

/**
 * @brief Create a TCP server socket.
 * 
 * @param address - IP address to bind to (NULL for INADDR_ANY)
 * @param port - Port number to bind to, 0 for ephemeral port
 * @param backlog - Maximum pending connections
 * @return socket_t - Created socket, or INVALID_SOCKET on failure.
 */
socket_t socket_create_tcp_server(socket_address_t *address, int backlog);

/**
 * @brief Create a UDP server socket.
 * 
 * @param address - socket address to bind to (NULL for INADDR_ANY)
 * @return socket_t - Created socket, or INVALID_SOCKET on failure.
 */
socket_t socket_create_udp_server(socket_address_t *address);

/**
 * @brief Close a socket.
 * 
 * @param sock - Socket to close
 * @return util_err_t - UTIL_OK on success, error code on failure.
 */
util_err_t socket_close(socket_t sock);


/**
 * @brief Set a socket to non-blocking mode.
 * 
 * @param sock - Socket to modify
 * @param nonblocking - true to set non-blocking, false for blocking
 * @return util_err_t - UTIL_OK on success, error code on failure.
 */
util_err_t socket_set_nonblocking(socket_t sock, bool nonblocking);

/**
 * @brief Set a socket option to allow reuse of the address.
 * 
 * @param sock - Socket to modify
 * @param reuse - true to enable reuse, false to disable
 * @return util_err_t - UTIL_OK on success, error code on failure.
 */
util_err_t socket_set_reuseaddr(socket_t sock, bool reuse);

/**
 * @brief Set a socket option to disable Nagle's algorithm.
 * 
 * @param sock - Socket to modify
 * @param nodelay - true to disable Nagle's algorithm, false to enable
 * @return util_err_t - UTIL_OK on success, error code on failure.
 */
util_err_t socket_set_nodelay(socket_t sock, bool nodelay);

/**
 * @brief Set a socket option to enable broadcast.
 * 
 * @param sock - Socket to modify
 * @param broadcast - true to enable broadcast, false to disable
 * @return util_err_t - UTIL_OK on success, error code on failure.
 */
util_err_t socket_set_broadcast(socket_t sock, bool broadcast);


/**
 * @brief Accept a new client connection on a server socket.
 * 
 * The socket must be a valid listening server socket and is set up
 * to not generate any signals (e.g. SIGPIPE on Unix).
 *
 * @param server - Server socket to accept connections on
 * @param out_client - Pointer to store the accepted client socket
 * @param out_client_addr - Pointer to store the address of the accepted client.
 *               Can be NULL if not needed.
 * @return util_err_t - UTIL_OK on success, error code on failure.
 */
util_err_t socket_accept(socket_t server, socket_t *out_client, socket_address_t *out_client_addr);

/**
 * @brief Connect a socket to a remote address.
 * 
 * @param sock - Socket to connect
 * @param address - Remote address to connect to. Must not be NULL.
 * @return util_err_t - UTIL_OK if in progress or completed, error code on failure.
 */
util_err_t socket_connect(socket_t sock, socket_address_t *address);

/**
 * @brief Send data on a socket.
 * 
 * The buffer is modified to reflect the amount of data sent by moving
 * the start index forward.  The end index is not modified.
 * 
 * @param sock - Socket to send data on
 * @param out - Buffer containing the data to send
 * @return util_err_t - UTIL_OK on success, error code on failure.
 */
util_err_t socket_send_buf(socket_t sock, buf_t *out);

/**
 * @brief Send data from multiple buffers using scatter/gather I/O.
 * 
 * Internally uses writev/WSASend where available, falls back to sequential
 * send() otherwise. Each buffer's start index is advanced as bytes are consumed.
 * 
 * On UTIL_EAGAIN, partial progress is already reflected in the buffers;
 * caller re-arms CAN_WRITE and retries with the same array.
 * 
 * @param sock - Socket to send data on
 * @param segments - Array of buffer pointers
 * @param segment_count - Number of buffers in the array
 * @return util_err_t - UTIL_OK when all buffers drained, UTIL_EAGAIN on would-block, error otherwise
 */
util_err_t socket_sendv_buf(socket_t sock, buf_t **segments, size_t segment_count);

/**
 * @brief Receive data on a socket.
 * 
 * Data is received into the provided buffer.  The passed
 * buffer must have sufficient capacity to hold the incoming data or the
 * data will be truncated.  The end index of the buffer is updated to reflect
 * the amount of data received.
 * 
 * Any existing data in the buffer is preserved; new data is appended.
 * 
 * @param sock - Socket to receive data on
 * @param in - Buffer to store the received data
 * @return util_err_t - UTIL_OK on success, error code on failure.
 */
util_err_t socket_recv_buf(socket_t sock, buf_t *in);



/* Datagram helpers */

/**
 * @brief Send data on a socket.
 * 
 * The buffer is modified to reflect the amount of data sent.  Data is sent starting from the
 * current start index and the start index is updated to reflect the amount of data sent.
 * 
 * @param sock - Socket to send data on
 * @param addr - Remote address to send to
 * @param out - Buffer containing the data to send
 * @return util_err_t - UTIL_OK on success, error code on failure.
 */
util_err_t socket_sendto_buf(socket_t sock, socket_address_t *addr, buf_t *out);

/**
 * @brief Send datagram data from multiple buffers using scatter/gather I/O.
 * 
 * Internally uses sendmsg/WSASendTo where available, falls back to sequential
 * sendto() otherwise. Each buffer's start index is advanced as bytes are consumed.
 * 
 * Note: For datagrams, either all data is sent or none is sent (atomic operation).
 * 
 * @param sock - Socket to send data on
 * @param addr - Remote address to send to
 * @param segments - Array of buffer pointers
 * @param segment_count - Number of buffers in the array
 * @return util_err_t - UTIL_OK on success, UTIL_EAGAIN on would-block, error otherwise
 */
util_err_t socket_sendtov_buf(socket_t sock, socket_address_t *addr, buf_t **segments, size_t segment_count);

/**
 * @brief Receive data on a socket.
 * 
 * This modifies the passed buffer end index to reflect the amount of data received.
 * 
 * The buffer does not need to be reset before calling this function.  Data is received
 * into the buffer starting at the current end index.
 * 
 * @param sock - Socket to receive data on
 * @param from_addr - Pointer to store the address of the sender
 * @param in - Buffer to store the received data
 * @return util_err_t - UTIL_OK on success, error code on failure.
 */
util_err_t socket_recvfrom_buf(socket_t sock, socket_address_t *from_addr, buf_t *in);

#ifdef __cplusplus
}
#endif

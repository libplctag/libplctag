#include "socket.h"
#include "log.h"
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stddef.h>

/* Platform detection for BSD-like systems (mirrors production code) */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__) || defined(__bsdi__) || defined(__DragonFly__)
#    define UTIL_BSD_OS_TYPE
#endif

#ifndef _WIN32
  #include <unistd.h>       // For close()
  #include <fcntl.h>        // For fcntl() and O_NONBLOCK
  #include <netdb.h>        // For getaddrinfo()
  #include <netinet/tcp.h>  // For TCP_NODELAY
  #include <sys/uio.h>      // For writev() and struct iovec
#else
  #include <ws2tcpip.h>     // For getaddrinfo() on Windows
#endif

/*
 * SIGPIPE Prevention Strategy:
 * - BSD/macOS: Use SO_NOSIGPIPE socket option (set at creation time)
 * - Linux: Use MSG_NOSIGNAL flag on send operations
 * - Other POSIX: Define MSG_NOSIGNAL as 0 if unavailable (application must ignore SIGPIPE)
 * - Windows: No SIGPIPE, not applicable
 */
#if !defined(_WIN32) && !defined(UTIL_BSD_OS_TYPE) && !defined(MSG_NOSIGNAL)
  #define MSG_NOSIGNAL 0
  #warning "MSG_NOSIGNAL not available on this platform. Application should ignore SIGPIPE signal."
#endif

/* Maximum number of IO vectors/buffers for scatter-gather operations */
#define SOCKET_MAX_IOVECS 16

/* Maximum payload size for single datagram send in fallback case */
#define SOCKET_MAX_PAYLOAD 4096

/* ================================================================
 * Initialization
 * ================================================================ */

util_err_t socket_init(void) {
#ifdef _WIN32
    WSADATA wsa_data;
    int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (result != 0) {
        return util_err_from_wsa(result);
    }
#endif
    return UTIL_OK;
}

void socket_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

util_err_t socket_get_err(void) {
#ifdef _WIN32
    return util_err_from_wsa(WSAGetLastError());
#else
    return util_err_from_errno(errno);
#endif
}

/* ================================================================
 * Address manipulation
 * ================================================================ */

util_err_t socket_address_init(socket_address_t *out_addr,
                                      const char *address_str, uint16_t port) {
    if (out_addr == NULL || address_str == NULL) {
        return UTIL_EINVAL;
    }

    memset(out_addr, 0, sizeof(*out_addr));

    /* Try to parse as IPv4 address */
    struct in_addr ipv4;
    if (inet_pton(AF_INET, address_str, &ipv4) == 1) {
        /* Valid IPv4 address */
        struct sockaddr_in *sin = (struct sockaddr_in *)&out_addr->addr;
        sin->sin_family = AF_INET;
        sin->sin_addr = ipv4;
        sin->sin_port = htons(port);
        out_addr->addr_len = sizeof(struct sockaddr_in);
        return UTIL_OK;
    }

    /* Try to parse as IPv6 address */
    struct in6_addr ipv6;
    if (inet_pton(AF_INET6, address_str, &ipv6) == 1) {
        /* Valid IPv6 address */
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&out_addr->addr;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_addr = ipv6;
        sin6->sin6_port = htons(port);
        out_addr->addr_len = sizeof(struct sockaddr_in6);
        return UTIL_OK;
    }

    /* Try to resolve as hostname */
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%u", port);

    int ret = getaddrinfo(address_str, port_str, &hints, &result);
    if (ret != 0) {
        return UTIL_ERESOLVE;
    }

    /* Use the first result */
    if (result != NULL) {
        memcpy(&out_addr->addr, result->ai_addr, result->ai_addrlen);
        out_addr->addr_len = (socklen_t)result->ai_addrlen;
        freeaddrinfo(result);
        return UTIL_OK;
    }

    return UTIL_ERESOLVE;
}

util_err_t socket_address_get_addr_str(const socket_address_t *addr,
                                    char *out_str, size_t out_str_cap) {
    if (addr == NULL || out_str == NULL || out_str_cap == 0) {
        return UTIL_EINVAL;
    }

    const struct sockaddr *sa = (const struct sockaddr *)&addr->addr;

    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
        const char *ptr = inet_ntop(AF_INET, &sin->sin_addr, out_str, (socklen_t)out_str_cap);
        if (ptr == NULL) {
            return UTIL_EINTERNAL;
        }
        return UTIL_OK;
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
        const char *ptr = inet_ntop(AF_INET6, &sin6->sin6_addr, out_str, (socklen_t)out_str_cap);
        if (ptr == NULL) {
            return UTIL_EINTERNAL;
        }
        return UTIL_OK;
    }

    return UTIL_EINVAL;
}


/* Changed function signature and implementation */
uint16_t socket_address_get_port(const socket_address_t *addr) {
    if (addr == NULL) {
        return 0;
    }

    const struct sockaddr *sa = (const struct sockaddr *)&addr->addr;

    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
        return ntohs(sin->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
        return ntohs(sin6->sin6_port);
    }

    return 0;
}


/* ================================================================
 * Socket creation
 * ================================================================ */

socket_t socket_create_tcp(void) {
    socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
#ifdef _WIN32
        return INVALID_SOCKET;
#else
        return -1;
#endif
    }

#ifdef UTIL_BSD_OS_TYPE
    /* Suppress SIGPIPE on BSD/macOS when writing to closed sockets */
    int nosigpipe = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe)) != 0) {
        close(sock);
        return -1;
    }
#endif

    /* Socket created in blocking mode by default */
    return sock;
}

socket_t socket_create_udp(void) {
    socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
#ifdef _WIN32
        return INVALID_SOCKET;
#else
        return -1;
#endif
    }

#ifdef UTIL_BSD_OS_TYPE
    /* Suppress SIGPIPE on BSD/macOS when writing to closed sockets */
    int nosigpipe = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe)) != 0) {
        close(sock);
        return -1;
    }
#endif

    /* Socket created in blocking mode by default */
    return sock;
}

socket_t socket_create_tcp_server(socket_address_t *address, int backlog) {
    if (address == NULL || backlog <= 0) {
        return INVALID_SOCKET;
    }

    socket_t sock = socket(((struct sockaddr *)&address->addr)->sa_family,
                          SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

    /* Enable SO_REUSEADDR to allow quick restart */
    int reuse = 1;
#ifdef _WIN32
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse)) != 0) {
        closesocket(sock);
        return INVALID_SOCKET;
    }
#else
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        close(sock);
        return -1;
    }
#endif

#ifdef UTIL_BSD_OS_TYPE
    /* Suppress SIGPIPE on BSD/macOS when writing to closed sockets */
    int nosigpipe = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe)) != 0) {
        close(sock);
        return -1;
    }
#endif

    /* Bind to address */
    if (bind(sock, (struct sockaddr *)&address->addr, address->addr_len) != 0) {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return INVALID_SOCKET;
    }

    /* Listen for connections */
    if (listen(sock, backlog) != 0) {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return INVALID_SOCKET;
    }

    return sock;
}

socket_t socket_create_udp_server(socket_address_t *address) {
    if (address == NULL) {
        return INVALID_SOCKET;
    }

    socket_t sock = socket(((struct sockaddr *)&address->addr)->sa_family,
                          SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

    /* Enable SO_REUSEADDR */
    int reuse = 1;
#ifdef _WIN32
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse)) != 0) {
        closesocket(sock);
        return INVALID_SOCKET;
    }
#else
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        close(sock);
        return -1;
    }
#endif

#ifdef UTIL_BSD_OS_TYPE
    /* Suppress SIGPIPE on BSD/macOS when writing to closed sockets */
    int nosigpipe = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe)) != 0) {
        close(sock);
        return -1;
    }
#endif

    /* Bind to address */
    if (bind(sock, (struct sockaddr *)&address->addr, address->addr_len) != 0) {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return INVALID_SOCKET;
    }

    return sock;
}

util_err_t socket_close(socket_t sock) {
    if (sock == INVALID_SOCKET) {
        return UTIL_EINVAL;
    }

#ifdef _WIN32
    if (closesocket(sock) != 0) {
        return util_err_from_wsa(WSAGetLastError());
    }
#else
    if (close(sock) != 0) {
        return util_err_from_errno(errno);
    }
#endif

    return UTIL_OK;
}

/* ================================================================
 * Socket options
 * ================================================================ */

util_err_t socket_set_nonblocking(socket_t sock, bool nonblocking) {
    if (sock == INVALID_SOCKET) {
        return UTIL_EINVAL;
    }

#ifdef _WIN32
    u_long mode = nonblocking ? 1 : 0;
    if (ioctlsocket(sock, FIONBIO, &mode) != 0) {
        return util_err_from_wsa(WSAGetLastError());
    }
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1) {
        return util_err_from_errno(errno);
    }
    if (nonblocking) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    if (fcntl(sock, F_SETFL, flags) == -1) {
        return util_err_from_errno(errno);
    }
#endif

    return UTIL_OK;
}

util_err_t socket_set_reuseaddr(socket_t sock, bool reuse) {
    if (sock == INVALID_SOCKET) {
        return UTIL_EINVAL;
    }

    int opt = reuse ? 1 : 0;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt)) != 0) {
        return socket_get_err();
    }

    return UTIL_OK;
}

util_err_t socket_set_nodelay(socket_t sock, bool nodelay) {
    if (sock == INVALID_SOCKET) {
        return UTIL_EINVAL;
    }

    int opt = nodelay ? 1 : 0;
    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&opt, sizeof(opt)) != 0) {
        return socket_get_err();
    }

    return UTIL_OK;
}

util_err_t socket_set_broadcast(socket_t sock, bool broadcast) {
    if (sock == INVALID_SOCKET) {
        return UTIL_EINVAL;
    }

#ifdef SO_BROADCAST
    int opt = broadcast ? 1 : 0;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char *)&opt, sizeof(opt)) != 0) {
        return socket_get_err();
    }
    return UTIL_OK;
#else
    /* Platform does not support SO_BROADCAST */
    (void)broadcast;  /* Avoid unused parameter warning */
    return UTIL_ENOTSUPPORTED;
#endif
}

/* ================================================================
 * Connection
 * ================================================================ */

util_err_t socket_accept(socket_t server, socket_t *out_client,
                        socket_address_t *out_client_addr) {
    if (server == INVALID_SOCKET || out_client == NULL) {
        return UTIL_EINVAL;
    }

    struct sockaddr_storage client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    socket_t client = accept(server, (struct sockaddr *)&client_addr, &client_addr_len);
    if (client == INVALID_SOCKET) {
#ifdef _WIN32
        int err = WSAGetLastError();
        /* In non-blocking mode, no connection ready returns WSAEWOULDBLOCK */
        if (err == WSAEWOULDBLOCK) {
            return UTIL_EAGAIN;
        }
        return util_err_from_wsa(err);
#else
        /* In non-blocking mode, no connection ready returns EAGAIN or EWOULDBLOCK */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return UTIL_EAGAIN;
        }
        /* EINTR can happen if interrupted by signal - treat as transient */
        if (errno == EINTR) {
            return UTIL_EAGAIN;
        }
        return util_err_from_errno(errno);
#endif
    }

#ifdef UTIL_BSD_OS_TYPE
    /* Suppress SIGPIPE on BSD/macOS when writing to closed sockets */
    int nosigpipe = 1;
    if (setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe)) != 0) {
        /* Log warning but don't fail - socket is already accepted */
        pdlog(LOG_MODULE_SOCKET, LOG_LEVEL_WARN, "Failed to set SO_NOSIGPIPE on accepted socket");
    }
#endif

    /* Accepted socket starts in blocking mode by default */
    *out_client = client;

    if (out_client_addr != NULL) {
        memcpy(&out_client_addr->addr, &client_addr, client_addr_len);
        out_client_addr->addr_len = client_addr_len;
    }

    return UTIL_OK;
}

util_err_t socket_connect(socket_t sock, socket_address_t *address) {
    if (sock == INVALID_SOCKET || address == NULL) {
        return UTIL_EINVAL;
    }

    int result = connect(sock, (struct sockaddr *)&address->addr, address->addr_len);
    if (result == 0) {
        return UTIL_OK;
    }

#ifdef _WIN32
    int err = WSAGetLastError();
    if (err == WSAEINPROGRESS || err == WSAEWOULDBLOCK) {
        return UTIL_EAGAIN;  /* Connection in progress */
    }
    return util_err_from_wsa(err);
#else
    int err = errno;
    if (err == EINPROGRESS) {
        return UTIL_EAGAIN;  /* Connection in progress */
    }
    return util_err_from_errno(err);
#endif
}

/* ================================================================
 * Data transfer (Stream)
 * ================================================================ */

util_err_t socket_send_buf(socket_t sock, buf_t *out) {
    if (sock == INVALID_SOCKET || out == NULL) {
        return UTIL_EINVAL;
    }

    if (!buf_ok(out)) {
        return UTIL_EINVAL;
    }

    size_t to_send = buf_read_size(out);
    if (to_send == 0) {
        return UTIL_OK;  /* Nothing to send */
    }

    const uint8_t *data = buf_read_ptr(out);

    pdlog(LOG_MODULE_SOCKET, LOG_LEVEL_SPEW, "socket_send_buf: Attempting to send %zu bytes", to_send);
    pdlog_bytes(LOG_MODULE_SOCKET, LOG_LEVEL_SPEW, out);

#ifdef _WIN32
    int sent = send(sock, (const char *)data, (int)to_send, 0);
    if (sent == SOCKET_ERROR) {
        return util_err_from_wsa(WSAGetLastError());
    }
#else
    /* Use MSG_NOSIGNAL to prevent SIGPIPE on Unix */
    ssize_t sent = send(sock, data, to_send, MSG_NOSIGNAL);
    if (sent < 0) {
        pdlog(LOG_MODULE_SOCKET, LOG_LEVEL_ERROR, "socket_send_buf: send() failed with errno %d", errno);
        return util_err_from_errno(errno);
    }
#endif

    pdlog(LOG_MODULE_SOCKET, LOG_LEVEL_SPEW, "socket_send_buf: Successfully sent %zd bytes out of %zu", sent, to_send);

    /* Advance read cursor by amount actually sent */
    buf_read_advance(out, (size_t)sent);

    return UTIL_OK;
}

util_err_t socket_sendv_buf(socket_t sock, buf_t **segments, size_t segment_count) {
    if (!segments || segment_count == 0) {
        return UTIL_EABORT;
    }

    /* Check if segment count exceeds maximum */
    if (segment_count > SOCKET_MAX_IOVECS) {
        return UTIL_EABORT;
    }

    /* Check if all segments are empty */
    bool all_empty = true;
    for (size_t i = 0; i < segment_count; i++) {
        if (buf_read_size(segments[i]) > 0) {
            all_empty = false;
            break;
        }
    }
    if (all_empty) {
        return UTIL_OK;
    }

#if defined(_WIN32)
    /* Windows: use WSASend with WSABUF array */
    WSABUF bufs[SOCKET_MAX_IOVECS];
    DWORD buf_count = 0;

    for (size_t i = 0; i < segment_count; i++) {
        size_t sz = buf_read_size(segments[i]);
        if (sz > 0) {
            bufs[buf_count].buf = (char*)buf_read_ptr(segments[i]);
            bufs[buf_count].len = (ULONG)sz;
            buf_count++;
        }
    }

    DWORD bytes_sent = 0;
    int result = WSASend(sock, bufs, buf_count, &bytes_sent, 0, NULL, NULL);

    if (result == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            return UTIL_EAGAIN;
        }
        return util_err_from_wsa(err);
    }

    /* Advance buffers by bytes_sent */
    size_t remaining = bytes_sent;
    for (size_t i = 0; i < segment_count && remaining > 0; i++) {
        size_t sz = buf_read_size(segments[i]);
        size_t consumed = (sz < remaining) ? sz : remaining;
        buf_read_advance(segments[i], consumed);
        remaining -= consumed;
    }

    /* Check if more data remains */
    bool has_remaining = false;
    for (size_t i = 0; i < segment_count; i++) {
        if (buf_read_size(segments[i]) > 0) {
            has_remaining = true;
            break;
        }
    }

    return has_remaining ? UTIL_EAGAIN : UTIL_OK;
    
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    /* Unix: use sendmsg with iovec array for signal safety */
    struct iovec vecs[SOCKET_MAX_IOVECS];
    size_t vec_count = 0;

    for (size_t i = 0; i < segment_count; i++) {
        size_t sz = buf_read_size(segments[i]);
        if (sz > 0) {
            vecs[vec_count].iov_base = (void*)buf_read_ptr(segments[i]);
            vecs[vec_count].iov_len = sz;
            vec_count++;
        }
    }

#ifdef UTIL_BSD_OS_TYPE
    /* On BSD/macOS, SO_NOSIGPIPE was set at socket creation, use writev() */
    ssize_t sent = writev(sock, vecs, (int)vec_count);
#else
    /* On Linux, use sendmsg() with MSG_NOSIGNAL to prevent SIGPIPE */
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = vecs;
    msg.msg_iovlen = (size_t)vec_count;
    /* MSG_NOSIGNAL is Linux-specific */
    ssize_t sent = sendmsg(sock, &msg, MSG_NOSIGNAL);
#endif

    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return UTIL_EAGAIN;
        }
        return util_err_from_errno(errno);
    }

    /* Advance buffers by sent bytes */
    size_t remaining = (size_t)sent;
    for (size_t i = 0; i < segment_count && remaining > 0; i++) {
        size_t sz = buf_read_size(segments[i]);
        size_t consumed = (sz < remaining) ? sz : remaining;
        buf_read_advance(segments[i], consumed);
        remaining -= consumed;
    }

    /* Check if more data remains */
    bool has_remaining = false;
    for (size_t i = 0; i < segment_count; i++) {
        if (buf_read_size(segments[i]) > 0) {
            has_remaining = true;
            break;
        }
    }

    return has_remaining ? UTIL_EAGAIN : UTIL_OK;
    
#else
    /* Fallback: sequential send() */
    for (size_t i = 0; i < segment_count; i++) {
        while (buf_read_size(segments[i]) > 0) {
            util_err_t err = socket_send_buf(sock, segments[i]);
            if (err != UTIL_OK) {
                return err;
            }
        }
    }
    
    return UTIL_OK;
#endif
}

/* ================================================================
 * Data transfer (Datagram)
 * ================================================================ */

util_err_t socket_sendto_buf(socket_t sock, socket_address_t *addr, buf_t *out) {
    if (sock == INVALID_SOCKET || addr == NULL || out == NULL) {
        return UTIL_EINVAL;
    }

    if (!buf_ok(out)) {
        return UTIL_EINVAL;
    }

    size_t to_send = buf_read_size(out);
    if (to_send == 0) {
        return UTIL_OK;  /* Nothing to send */
    }

    const uint8_t *data = buf_read_ptr(out);

#ifdef _WIN32
    int sent = sendto(sock, (const char *)data, (int)to_send, 0,
                     (struct sockaddr *)&addr->addr, addr->addr_len);
    if (sent == SOCKET_ERROR) {
        return util_err_from_wsa(WSAGetLastError());
    }
#else
    /* Use MSG_NOSIGNAL to prevent SIGPIPE on Unix */
    ssize_t sent = sendto(sock, data, to_send, MSG_NOSIGNAL,
                         (struct sockaddr *)&addr->addr, addr->addr_len);
    if (sent < 0) {
        return util_err_from_errno(errno);
    }
#endif

    /* Advance read cursor by amount actually sent */
    buf_read_advance(out, (size_t)sent);

    return UTIL_OK;
}

util_err_t socket_sendtov_buf(socket_t sock, socket_address_t *addr, buf_t **segments, size_t segment_count) {
    if (!addr || !segments || segment_count == 0) {
        return UTIL_EABORT;
    }

    /* Check if segment count exceeds maximum */
    if (segment_count > SOCKET_MAX_IOVECS) {
        return UTIL_EABORT;
    }

    /* Check if all segments are empty */
    bool all_empty = true;
    size_t total_size = 0;
    for (size_t i = 0; i < segment_count; i++) {
        size_t sz = buf_read_size(segments[i]);
        total_size += sz;
        if (sz > 0) {
            all_empty = false;
        }
    }
    if (all_empty) {
        return UTIL_OK;
    }

#if defined(_WIN32)
    /* Windows: use WSASendTo with WSABUF array */
    WSABUF bufs[SOCKET_MAX_IOVECS];
    DWORD buf_count = 0;

    for (size_t i = 0; i < segment_count; i++) {
        size_t sz = buf_read_size(segments[i]);
        if (sz > 0) {
            bufs[buf_count].buf = (char*)buf_read_ptr(segments[i]);
            bufs[buf_count].len = (ULONG)sz;
            buf_count++;
        }
    }

    DWORD bytes_sent = 0;
    int result = WSASendTo(sock, bufs, buf_count, &bytes_sent, 0,
                          (struct sockaddr*)&addr->addr, addr->addr_len, NULL, NULL);

    if (result == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            return UTIL_EAGAIN;
        }
        return util_err_from_wsa(err);
    }

    /* For datagrams, either all is sent or none */
    if (bytes_sent == total_size) {
        for (size_t i = 0; i < segment_count; i++) {
            size_t sz = buf_read_size(segments[i]);
            buf_read_advance(segments[i], sz);
        }
        return UTIL_OK;
    }

    return UTIL_EABORT;
    
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    /* Unix: use sendmsg with iovec array */
    struct iovec vecs[SOCKET_MAX_IOVECS];  /* Fixed-size array instead of alloca() */
    size_t vec_count = 0;
    
    for (size_t i = 0; i < segment_count; i++) {
        size_t sz = buf_read_size(segments[i]);
        if (sz > 0) {
            vecs[vec_count].iov_base = (void*)buf_read_ptr(segments[i]);
            vecs[vec_count].iov_len = sz;
            vec_count++;
        }
    }
    
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = (void*)&addr->addr;
    msg.msg_namelen = addr->addr_len;
    msg.msg_iov = vecs;
    msg.msg_iovlen = (int)vec_count;

#ifdef UTIL_BSD_OS_TYPE
    /* On BSD/macOS, SO_NOSIGPIPE was set at socket creation */
    ssize_t sent = sendmsg(sock, &msg, 0);
#else
    /* On Linux, MSG_NOSIGNAL prevents SIGPIPE when writing to closed sockets */
    ssize_t sent = sendmsg(sock, &msg, MSG_NOSIGNAL);
#endif
    
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return UTIL_EAGAIN;
        }
        return util_err_from_errno(errno);
    }
    
    /* For datagrams, either all is sent or none */
    if ((size_t)sent == total_size) {
        for (size_t i = 0; i < segment_count; i++) {
            size_t sz = buf_read_size(segments[i]);
            buf_read_advance(segments[i], sz);
        }
        return UTIL_OK;
    }
    
    return UTIL_EABORT;
    
#else
    /* Fallback: concatenate and use single sendto() */
    /* Check that total payload fits in fixed buffer */
    if (total_size > SOCKET_MAX_PAYLOAD) {
        return UTIL_EABORT;  /* Payload too large for fallback implementation */
    }

    uint8_t temp_buf[SOCKET_MAX_PAYLOAD];  /* Fixed-size buffer instead of alloca() */
    size_t offset = 0;

    for (size_t i = 0; i < segment_count; i++) {
        size_t sz = buf_read_size(segments[i]);
        if (sz > 0) {
            memcpy(temp_buf + offset, buf_read_ptr(segments[i]), sz);
            offset += sz;
        }
    }

#ifdef _WIN32
    ssize_t sent = sendto(sock, temp_buf, total_size, 0,
                         (struct sockaddr*)&addr->addr, addr->addr_len);
#elif defined(UTIL_BSD_OS_TYPE)
    /* On BSD/macOS, SO_NOSIGPIPE was set at socket creation */
    ssize_t sent = sendto(sock, temp_buf, total_size, 0,
                         (struct sockaddr*)&addr->addr, addr->addr_len);
#else
    /* On Linux, use MSG_NOSIGNAL to prevent SIGPIPE */
    ssize_t sent = sendto(sock, temp_buf, total_size, MSG_NOSIGNAL,
                         (struct sockaddr*)&addr->addr, addr->addr_len);
#endif
    
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return UTIL_EAGAIN;
        }
        return util_err_from_errno(errno);
    }
    
    /* Advance buffers by sent bytes */
    size_t remaining = (size_t)sent;
    for (size_t i = 0; i < segment_count && remaining > 0; i++) {
        size_t sz = buf_read_size(segments[i]);
        size_t consumed = (sz < remaining) ? sz : remaining;
        buf_read_advance(segments[i], consumed);
        remaining -= consumed;
    }
    
    /* Check if more data remains */
    for (size_t i = 0; i < segment_count; i++) {
        if (buf_read_size(segments[i]) > 0) {
            return UTIL_EAGAIN;
        }
    }
    
    return UTIL_OK;
#endif
}

util_err_t socket_recv_buf(socket_t sock, buf_t *in) {
    if (sock == INVALID_SOCKET || in == NULL) {
        return UTIL_EINVAL;
    }

    if (!buf_ok(in)) {
        return UTIL_EINVAL;
    }

    size_t capacity = buf_write_size(in);
    if (capacity == 0) {
        return UTIL_ERESOURCE;  /* No space in buffer */
    }

    uint8_t *write_ptr = buf_write_ptr(in);

#ifdef _WIN32
    int received = recv(sock, (char *)write_ptr, (int)capacity, 0);
    if (received == SOCKET_ERROR) {
        return util_err_from_wsa(WSAGetLastError());
    }
    if (received == 0) {
        return UTIL_ECLOSED;  /* Connection closed */
    }
#else
    ssize_t received = recv(sock, write_ptr, capacity, 0);
    if (received < 0) {
        return util_err_from_errno(errno);
    }
    if (received == 0) {
        return UTIL_ECLOSED;  /* Connection closed */
    }
#endif

    /* Advance write cursor by amount received */
    buf_write_advance(in, (size_t)received);

    return UTIL_OK;
}

util_err_t socket_recv_frame(socket_t sock, buf_t *buf,
                             socket_frame_check_fn frame_check, void *context) {
    util_err_t err;

    if (sock == INVALID_SOCKET || buf == NULL || frame_check == NULL) {
        return UTIL_EINVAL;
    }

    if (!buf_ok(buf)) {
        return UTIL_EINVAL;
    }

    /* First check if we already have a complete frame from previous data */
    err = frame_check(buf, context);
    if (err == UTIL_OK) {
        return UTIL_OK;  /* Frame already complete */
    }
    if (err != UTIL_EAGAIN) {
        return err;  /* Frame check found an error */
    }

    /* Try to receive more data */
    err = socket_recv_buf(sock, buf);
    if (err != UTIL_OK) {
        return err;  /* EAGAIN, ECLOSED, or error */
    }

    /* Check again after receiving new data */
    return frame_check(buf, context);
}

util_err_t socket_recvfrom_buf(socket_t sock, socket_address_t *from_addr, buf_t *in) {
    if (sock == INVALID_SOCKET || from_addr == NULL || in == NULL) {
        return UTIL_EINVAL;
    }

    if (!buf_ok(in)) {
        return UTIL_EINVAL;
    }

    size_t capacity = buf_write_size(in);
    if (capacity == 0) {
        return UTIL_ERESOURCE;  /* No space in buffer */
    }

    uint8_t *write_ptr = buf_write_ptr(in);
    
    struct sockaddr_storage sender_addr;
    socklen_t sender_addr_len = sizeof(sender_addr);

#ifdef _WIN32
    int received = recvfrom(sock, (char *)write_ptr, (int)capacity, 0,
                           (struct sockaddr *)&sender_addr, &sender_addr_len);
    if (received == SOCKET_ERROR) {
        return util_err_from_wsa(WSAGetLastError());
    }
#else
    ssize_t received = recvfrom(sock, write_ptr, capacity, 0,
                               (struct sockaddr *)&sender_addr, &sender_addr_len);
    if (received < 0) {
        return util_err_from_errno(errno);
    }
#endif

    /* Store sender address */
    memcpy(&from_addr->addr, &sender_addr, sender_addr_len);
    from_addr->addr_len = sender_addr_len;

    /* Advance write cursor by amount received */
    buf_write_advance(in, (size_t)received);

    return UTIL_OK;
}

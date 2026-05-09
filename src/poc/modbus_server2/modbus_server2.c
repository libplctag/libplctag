/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
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

/*
 * modbus_server2.c
 *
 * Modbus TCP server using:
 *   - BSD sockets directly (no platform abstraction layer)
 *   - Blocking sockets with SO_REUSEADDR and TCP_NODELAY
 *   - select() with a 20ms timeout on every socket (no SO_RCVTIMEO)
 *   - One thread per listening socket
 *   - One thread per accepted client connection
 *
 * Threading model:
 *   main()           - parses args, starts listener threads, waits for SIGINT
 *   listener_thread  - loops in accept(); on timeout checks g_terminate;
 *                      on termination joins all client threads then exits
 *   client_thread    - loops in recv() with select(); on timeout checks
 *                      g_terminate; closes socket and signals done on exit
 */

/* ============================================================================
 * Platform portability — sockets
 * ============================================================================ */

#ifdef _WIN32
#    include <winsock2.h>
#    include <ws2tcpip.h>
typedef SOCKET sock_t;
#    define INVALID_SOCK INVALID_SOCKET
#    define close_socket(s) closesocket(s)
static int sock_error(void) { return (int)WSAGetLastError(); }
#else
#    include <sys/socket.h>
#    include <sys/select.h>
#    include <sys/time.h>
#    include <netinet/in.h>
#    include <netinet/tcp.h>
#    include <arpa/inet.h>
#    include <unistd.h>
#    include <errno.h>
typedef int sock_t;
#    define INVALID_SOCK (-1)
#    define close_socket(s) close(s)
static int sock_error(void) { return errno; }
#endif

/* ============================================================================
 * Platform portability — threading
 * ============================================================================ */

#ifdef _WIN32
#    include <windows.h>
typedef HANDLE thread_t;
typedef CRITICAL_SECTION mutex_t;

#    define THREAD_FUNC(fn) static DWORD WINAPI fn(LPVOID arg)
#    define THREAD_RETURN(v) return (DWORD)(uintptr_t)(v)

static int thread_start(thread_t *t, LPTHREAD_START_ROUTINE fn, void *arg) {
    *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    return (*t != NULL) ? 0 : -1;
}
static void thread_join(thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
static void mutex_init(mutex_t *m) { InitializeCriticalSection(m); }
static void mutex_lock(mutex_t *m) { EnterCriticalSection(m); }
static void mutex_unlock(mutex_t *m) { LeaveCriticalSection(m); }
static void mutex_destroy(mutex_t *m) { DeleteCriticalSection(m); }

#else
#    include <pthread.h>
typedef pthread_t thread_t;
typedef pthread_mutex_t mutex_t;

#    define THREAD_FUNC(fn) static void *fn(void *arg)
#    define THREAD_RETURN(v) return (void *)(intptr_t)(v)

static int thread_start(thread_t *t, void *(*fn)(void *), void *arg) { return pthread_create(t, NULL, fn, arg); }
static void thread_join(thread_t t) { pthread_join(t, NULL); }
static void mutex_init(mutex_t *m) { pthread_mutex_init(m, NULL); }
static void mutex_lock(mutex_t *m) { pthread_mutex_lock(m); }
static void mutex_unlock(mutex_t *m) { pthread_mutex_unlock(m); }
static void mutex_destroy(mutex_t *m) { pthread_mutex_destroy(m); }
#endif

/* ============================================================================
 * Standard includes
 * ============================================================================ */

#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "buf.h"
#include "err.h"
#include "log.h"
#include "modbus_protocol.h"
#include "register_storage.h"
#include "utils.h"

/* ============================================================================
 * Constants
 * ============================================================================ */

#define SELECT_TIMEOUT_MS 20
#define LISTEN_BACKLOG 128

/* Buffer sizes — slightly larger than the Modbus ADU max (267 bytes) */
#define MODBUS_RECV_BUF_SIZE (MODBUS_MAX_ADU_SIZE + 16)
#define MODBUS_SEND_BUF_SIZE (MODBUS_MAX_ADU_SIZE + 16)

/* Histogram bucket boundaries (microseconds) */
#define HIST_BUCKET_COUNT 8
static const int64_t hist_boundaries[HIST_BUCKET_COUNT] = {100, 500, 1000, 2000, 5000, 10000, 50000, INT64_MAX};

/* ============================================================================
 * Types — forward declarations
 * ============================================================================ */

struct server_s;

/* ============================================================================
 * Statistics
 * ============================================================================ */

typedef struct {
    int64_t total_requests;
    int64_t total_response_time_us;
    int64_t total_response_time_sq_us;
    int64_t min_response_time_us;
    int64_t max_response_time_us;
    int64_t total_recv_time_us;
    int64_t total_process_time_us;
    int64_t total_send_time_us;
    int64_t clients_connected;
    int64_t clients_disconnected;
    int64_t hist_buckets[HIST_BUCKET_COUNT];
} server_stats_t;

/* ============================================================================
 * Client entry — one per accepted connection, owned by the listener thread
 * ============================================================================ */

typedef struct {
    thread_t thread;
    sock_t fd;
    volatile int done; /* set to 1 by client_thread before it returns */
    struct server_s *server;
} client_entry_t;

/* ============================================================================
 * Listener context — one per listening address/port
 * ============================================================================ */

typedef struct {
    thread_t thread;
    sock_t listen_fd;
    char host[256];
    uint16_t port;
    volatile int started; /* 0 = pending, 1 = running, -1 = failed */
    struct server_s *server;

    /* Client thread list — accessed only from the listener thread */
    client_entry_t **clients;
    int num_clients;
    int clients_cap;
} listener_ctx_t;

/* ============================================================================
 * Server context
 * ============================================================================ */

typedef struct server_s {
    register_storage_t *storage;
    int64_t start_time_us;
    listener_ctx_t **listeners;
    int num_listeners;
    server_stats_t stats;
    mutex_t stats_mutex;
    mutex_t storage_mutex; /* serialises all register bank access */
} server_t;

/* ============================================================================
 * Global state
 * ============================================================================ */

static volatile sig_atomic_t g_terminate = 0;
static server_t *g_server = NULL;

/* ============================================================================
 * Signal handling
 * ============================================================================ */

static void signal_handler(void) { g_terminate = 1; }

/* ============================================================================
 * Socket helpers
 * ============================================================================ */

/*
 * wait_readable — select() on fd for up to timeout_ms milliseconds.
 * Returns: >0 = fd is readable, 0 = timeout, <0 = error.
 */
static int wait_readable(sock_t fd, int timeout_ms) {
    fd_set rfds;
    struct timeval tv;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return select((int)(fd + 1), &rfds, NULL, NULL, &tv);
}

/*
 * wait_writable — select() on fd for up to timeout_ms milliseconds.
 * Returns: >0 = fd is writable, 0 = timeout, <0 = error.
 */
static int wait_writable(sock_t fd, int timeout_ms) {
    fd_set wfds;
    struct timeval tv;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return select((int)(fd + 1), NULL, &wfds, NULL, &tv);
}

/*
 * apply_socket_options — set SO_REUSEADDR and TCP_NODELAY on a socket.
 */
static void apply_socket_options(sock_t fd, log_module_t log_module) {
    int opt = 1;
    if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt)) < 0) {
        pdlog(log_module, LOG_LEVEL_WARN, "SO_REUSEADDR failed: %d", sock_error());
    }
    opt = 1;
    if(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&opt, sizeof(opt)) < 0) {
        pdlog(log_module, LOG_LEVEL_WARN, "TCP_NODELAY failed: %d", sock_error());
    }
}

/*
 * create_server_socket — create a blocking TCP listen socket bound to host:port.
 * Returns INVALID_SOCK on error.
 */
static sock_t create_server_socket(const char *host, uint16_t port) {
    struct sockaddr_in addr;

    sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd == INVALID_SOCK) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "socket() failed: %d", sock_error());
        return INVALID_SOCK;
    }

    apply_socket_options(fd, LOG_MODULE_MODBUS_SERVER);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if(inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Invalid bind address: %s", host);
        close_socket(fd);
        return INVALID_SOCK;
    }

    if(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "bind() failed on %s:%u: %d", host, (unsigned)port, sock_error());
        close_socket(fd);
        return INVALID_SOCK;
    }

    if(listen(fd, LISTEN_BACKLOG) < 0) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "listen() failed: %d", sock_error());
        close_socket(fd);
        return INVALID_SOCK;
    }

    return fd;
}

/*
 * send_all — send exactly len bytes using select() before each send() call.
 * Returns len on success, -1 on error or if g_terminate is set.
 */
static int send_all(sock_t fd, const uint8_t *data, int len) {
    int sent = 0;
    while(sent < len) {
        if(g_terminate) { return -1; }

        int n = wait_writable(fd, SELECT_TIMEOUT_MS);
        if(n < 0) { return -1; }
        if(n == 0) { continue; } /* timeout — recheck g_terminate */

        int r = (int)send(fd, (const char *)(data + sent), (size_t)(len - sent), 0);
        if(r <= 0) { return -1; }
        sent += r;
    }
    return sent;
}

/* ============================================================================
 * Modbus frame receive
 * ============================================================================ */

/*
 * recv_modbus_frame — receive one complete Modbus TCP ADU into buf.
 *
 * A Modbus TCP ADU layout:
 *   bytes [0..1]  transaction ID
 *   bytes [2..3]  protocol ID (must be 0)
 *   bytes [4..5]  length (big-endian): covers unit_id + PDU
 *   bytes [6..]   unit_id + PDU  (exactly <length> bytes)
 *
 * Total frame = 6 + length_field bytes.
 *
 * Uses select() with SELECT_TIMEOUT_MS before every recv() so the loop
 * can check g_terminate on each timeout.
 *
 * Returns: total byte count on success, 0 on clean disconnect/terminate,
 *          -1 on protocol or socket error.
 */
static int recv_modbus_frame(sock_t fd, uint8_t *buf, int buf_size) {
    int received = 0;
    int total_needed = -1; /* unknown until we have the first 6 bytes */

    while(1) {
        if(g_terminate) { return 0; }

        /* How many bytes to request in the next recv() */
        int want;
        if(total_needed < 0) {
            want = 6 - received; /* collect enough to read the length field */
        } else {
            want = total_needed - received;
        }

        if(want <= 0) { break; } /* frame is complete */

        if(received + want > buf_size) {
            pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_ERROR, "Frame too large: need %d bytes, buffer holds %d",
                  received + want, buf_size);
            return -1;
        }

        int n = wait_readable(fd, SELECT_TIMEOUT_MS);
        if(n < 0) { return -1; }
        if(n == 0) { continue; } /* timeout */

        int r = (int)recv(fd, (char *)(buf + received), (size_t)want, 0);
        if(r == 0) { return 0; } /* clean disconnect */
        if(r < 0) { return -1; } /* error */
        received += r;

        /* Once we have the first 6 bytes, decode the total frame length */
        if(total_needed < 0 && received >= 6) {
            uint16_t mbap_len = (uint16_t)(((uint16_t)buf[4] << 8) | (uint16_t)buf[5]);

            /* Validate: must cover at least unit_id(1) + FC(1), at most PDU_max+1 */
            if(mbap_len < 2 || mbap_len > (uint16_t)(MODBUS_MAX_PDU_SIZE + 1)) {
                pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_WARN, "Invalid MBAP length field: %u", (unsigned)mbap_len);
                return -1;
            }

            total_needed = 6 + (int)mbap_len;
            if(total_needed > buf_size) {
                pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_ERROR, "Frame size %d exceeds buffer %d", total_needed, buf_size);
                return -1;
            }
        }
    }

    return received;
}

/* ============================================================================
 * Client thread
 * ============================================================================ */

THREAD_FUNC(client_thread) {
    client_entry_t *entry = (client_entry_t *)arg;
    server_t *server = entry->server;
    sock_t fd = entry->fd;

    uint8_t recv_raw[MODBUS_RECV_BUF_SIZE];
    uint8_t send_raw[MODBUS_SEND_BUF_SIZE];

    pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_INFO, "Client thread started (fd %d)", (int)fd);

    mutex_lock(&server->stats_mutex);
    server->stats.clients_connected++;
    mutex_unlock(&server->stats_mutex);

    while(!g_terminate) {
        /* ---- Receive a complete Modbus frame ---- */
        int64_t recv_start = util_time_us();

        int frame_len = recv_modbus_frame(fd, recv_raw, MODBUS_RECV_BUF_SIZE);
        if(frame_len <= 0) { break; } /* disconnect, error, or g_terminate */
        if(g_terminate) { break; }

        int64_t recv_done = util_time_us();

        /* ---- Wrap the received bytes in a buf_t for the protocol layer ---- */
        buf_t recv_b = buf_init(recv_raw, (size_t)frame_len);
        recv_b.write = (size_t)frame_len; /* mark all bytes as available to read */

        /* ---- Parse MBAP header ---- */
        mbap_header_t header;
        if(modbus_parse_mbap_header(&recv_b, &header) != UTIL_OK) {
            pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_WARN, "MBAP header parse failed");
            break;
        }

        /* ---- Extract function code ---- */
        uint8_t function_code = 0;
        if(!buf_read_u8(&recv_b, "function_code", &function_code)) {
            pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_WARN, "Failed to read function code");
            break;
        }

        pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_DETAIL, "Request: TxID=%u FC=0x%02X", (unsigned)header.transaction_id,
              (unsigned)function_code);

        /* ---- Process request ---- */
        buf_t send_b = buf_init(send_raw, MODBUS_SEND_BUF_SIZE);

        int64_t proc_start = util_time_us();
        mutex_lock(&server->storage_mutex);
        util_err_t err = modbus_process_request(function_code, &recv_b, &send_b, &header, server->storage);
        mutex_unlock(&server->storage_mutex);
        if(err != UTIL_OK) { modbus_build_exception_response(&send_b, &header, function_code, err); }
        int64_t proc_done = util_time_us();

        /* ---- Send response ---- */
        int send_len = (int)send_b.write;

        pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_DETAIL, "Sending %d-byte response", send_len);

        int64_t send_start = util_time_us();
        if(send_all(fd, send_raw, send_len) < 0) {
            pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_WARN, "Send failed");
            break;
        }
        int64_t send_done = util_time_us();

        /* ---- Update statistics (mutex-protected) ---- */
        int64_t total_time = send_done - recv_start;
        int64_t recv_time = recv_done - recv_start;
        int64_t proc_time = proc_done - proc_start;
        int64_t send_time = send_done - send_start;

        mutex_lock(&server->stats_mutex);
        server_stats_t *s = &server->stats;
        s->total_requests++;
        s->total_response_time_us += total_time;
        s->total_response_time_sq_us += total_time * total_time;
        s->total_recv_time_us += recv_time;
        s->total_process_time_us += proc_time;
        s->total_send_time_us += send_time;
        if(s->min_response_time_us == 0 || total_time < s->min_response_time_us) { s->min_response_time_us = total_time; }
        if(total_time > s->max_response_time_us) { s->max_response_time_us = total_time; }
        for(int i = 0; i < HIST_BUCKET_COUNT; i++) {
            if(total_time <= hist_boundaries[i]) {
                s->hist_buckets[i]++;
                break;
            }
        }
        mutex_unlock(&server->stats_mutex);
    }

    pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_INFO, "Client thread exiting (fd %d)", (int)fd);

    close_socket(fd);

    mutex_lock(&server->stats_mutex);
    server->stats.clients_disconnected++;
    mutex_unlock(&server->stats_mutex);

    entry->done = 1;
    THREAD_RETURN(0);
}

/* ============================================================================
 * Listener thread helpers
 * ============================================================================ */

/*
 * listener_add_client — append a client entry to the listener's tracking list.
 * The list is grown with realloc() as needed.
 * Called only from the listener thread; no locking required.
 */
static int listener_add_client(listener_ctx_t *ctx, client_entry_t *entry) {
    if(ctx->num_clients >= ctx->clients_cap) {
        int new_cap = ctx->clients_cap ? ctx->clients_cap * 2 : 64;
        client_entry_t **tmp = (client_entry_t **)realloc(ctx->clients, (size_t)new_cap * sizeof(*tmp));
        if(!tmp) { return -1; }
        ctx->clients = tmp;
        ctx->clients_cap = new_cap;
    }
    ctx->clients[ctx->num_clients++] = entry;
    return 0;
}

/*
 * listener_reap_clients — join any client threads that have set entry->done.
 * Compacts the clients array in-place after each join.
 * Called only from the listener thread; no locking required.
 */
static void listener_reap_clients(listener_ctx_t *ctx) {
    int i = 0;
    while(i < ctx->num_clients) {
        client_entry_t *e = ctx->clients[i];
        if(e->done) {
            thread_join(e->thread);
            free(e);
            /* Fill the gap with the last entry */
            ctx->clients[i] = ctx->clients[--ctx->num_clients];
        } else {
            i++;
        }
    }
}

/*
 * listener_join_all_clients — join every remaining client thread.
 * Called once g_terminate is set and the listener is winding down.
 */
static void listener_join_all_clients(listener_ctx_t *ctx) {
    for(int i = 0; i < ctx->num_clients; i++) {
        thread_join(ctx->clients[i]->thread);
        free(ctx->clients[i]);
        ctx->clients[i] = NULL;
    }
    ctx->num_clients = 0;
}

/* ============================================================================
 * Listener thread
 * ============================================================================ */

THREAD_FUNC(listener_thread) {
    listener_ctx_t *ctx = (listener_ctx_t *)arg;

    /* ---- Create the listening socket ---- */
    ctx->listen_fd = create_server_socket(ctx->host, ctx->port);
    if(ctx->listen_fd == INVALID_SOCK) {
        pdlog(LOG_MODULE_MODBUS_CORO_LISTENER, LOG_LEVEL_ERROR, "Failed to create server socket on %s:%u", ctx->host,
              (unsigned)ctx->port);
        ctx->started = -1;
        THREAD_RETURN(-1);
    }

    pdlog(LOG_MODULE_MODBUS_CORO_LISTENER, LOG_LEVEL_INFO, "Listening on %s:%u", ctx->host, (unsigned)ctx->port);
    ctx->started = 1;

    /* ---- Accept loop ---- */
    while(!g_terminate) {
        /* Opportunistically reap finished client threads */
        listener_reap_clients(ctx);

        /* Wait for an incoming connection */
        int n = wait_readable(ctx->listen_fd, SELECT_TIMEOUT_MS);
        if(n < 0) {
            pdlog(LOG_MODULE_MODBUS_CORO_LISTENER, LOG_LEVEL_ERROR, "select() error on listen socket: %d", sock_error());
            break;
        }
        if(n == 0) { continue; } /* timeout — loop back and check g_terminate */

        /* Accept the connection */
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        sock_t client_fd = accept(ctx->listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if(client_fd == INVALID_SOCK) {
            /* Can happen if the connection was reset between select() and accept() */
            pdlog(LOG_MODULE_MODBUS_CORO_LISTENER, LOG_LEVEL_WARN, "accept() failed: %d", sock_error());
            continue;
        }

        apply_socket_options(client_fd, LOG_MODULE_MODBUS_CORO_LISTENER);

        char client_ip[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        pdlog(LOG_MODULE_MODBUS_CORO_LISTENER, LOG_LEVEL_INFO, "Accepted connection from %s:%u (fd %d)", client_ip,
              (unsigned)ntohs(client_addr.sin_port), (int)client_fd);

        /* Allocate client entry */
        client_entry_t *entry = (client_entry_t *)calloc(1, sizeof(client_entry_t));
        if(!entry) {
            pdlog(LOG_MODULE_MODBUS_CORO_LISTENER, LOG_LEVEL_ERROR, "Out of memory for client entry");
            close_socket(client_fd);
            continue;
        }
        entry->fd = client_fd;
        entry->server = ctx->server;
        entry->done = 0;

        /* Register before starting the thread so done=1 is never missed */
        if(listener_add_client(ctx, entry) < 0) {
            pdlog(LOG_MODULE_MODBUS_CORO_LISTENER, LOG_LEVEL_ERROR, "Out of memory growing client list");
            close_socket(client_fd);
            free(entry);
            continue;
        }

        if(thread_start(&entry->thread, client_thread, entry) != 0) {
            pdlog(LOG_MODULE_MODBUS_CORO_LISTENER, LOG_LEVEL_ERROR, "Failed to start client thread");
            close_socket(client_fd);
            ctx->num_clients--; /* undo the add */
            free(entry);
            continue;
        }
    }

    /* ---- Shutdown: join all client threads, then close listen socket ---- */
    pdlog(LOG_MODULE_MODBUS_CORO_LISTENER, LOG_LEVEL_INFO, "Listener on %s:%u shutting down — joining %d client thread(s)",
          ctx->host, (unsigned)ctx->port, ctx->num_clients);

    listener_join_all_clients(ctx);
    close_socket(ctx->listen_fd);
    ctx->listen_fd = INVALID_SOCK;

    THREAD_RETURN(0);
}

/* ============================================================================
 * Statistics output
 * ============================================================================ */

static const char *hist_bucket_label(int b) {
    switch(b) {
        case 0: return "0-100us";
        case 1: return "100-500us";
        case 2: return "500us-1ms";
        case 3: return "1-2ms";
        case 4: return "2-5ms";
        case 5: return "5-10ms";
        case 6: return "10-50ms";
        case 7: return ">50ms";
        default: return "unknown";
    }
}

static void print_statistics(server_t *server) {
    if(!server) { return; }

    int64_t end_us = util_time_us();
    double runtime = (double)(end_us - server->start_time_us) / 1.0e6;

    server_stats_t *s = &server->stats;
    int64_t total = s->total_requests;

    fflush(stderr);
    fprintf(stderr, "\n");
    fprintf(stderr, "╔══════════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║         MODBUS SERVER (THREADED) PERFORMANCE STATISTICS          ║\n");
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║ Runtime: %.2f seconds\n", runtime);
    fprintf(stderr, "║ Total requests: %" PRId64 "\n", total);
    if(runtime > 0) { fprintf(stderr, "║ Throughput: %.2f requests/sec\n", (double)total / runtime); }
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║                       CLIENT STATISTICS                          ║\n");
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║ Clients connected:    %" PRId64 "\n", s->clients_connected);
    fprintf(stderr, "║ Clients disconnected: %" PRId64 "\n", s->clients_disconnected);
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║                     RESPONSE TIME SUMMARY                        ║\n");
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");

    if(total > 0) {
        double mean = (double)s->total_response_time_us / (double)total;
        double var = ((double)s->total_response_time_sq_us / (double)total) - mean * mean;
        double sd = var > 0.0 ? sqrt(var) : 0.0;

        fprintf(stderr, "║ Average:  %8.2f us\n", mean);
        fprintf(stderr, "║ Std Dev:  %8.2f us\n", sd);
        fprintf(stderr, "║ Minimum:  %8" PRId64 " us\n", s->min_response_time_us);
        fprintf(stderr, "║ Maximum:  %8" PRId64 " us\n", s->max_response_time_us);

        fprintf(stderr, "╠══════════════════════════════════════════════════════════════╣\n");
        fprintf(stderr, "║                   LATENCY BREAKDOWN (avg)                    ║\n");
        fprintf(stderr, "╠══════════════════════════════════════════════════════════════╣\n");

        double avg_recv = (double)s->total_recv_time_us / (double)total;
        double avg_process = (double)s->total_process_time_us / (double)total;
        double avg_send = (double)s->total_send_time_us / (double)total;
        double avg_total = avg_recv + avg_process + avg_send;

        fprintf(stderr, "║  Recv (socket):   %8.2f us (%5.1f%%)\n", avg_recv, avg_total > 0 ? avg_recv / avg_total * 100 : 0);
        fprintf(stderr, "║  Process (modbus):%8.2f us (%5.1f%%)\n", avg_process,
                avg_total > 0 ? avg_process / avg_total * 100 : 0);
        fprintf(stderr, "║  Send (socket):   %8.2f us (%5.1f%%)\n", avg_send, avg_total > 0 ? avg_send / avg_total * 100 : 0);
    }

    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║                  RESPONSE TIME HISTOGRAM                         ║\n");
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");

    int64_t max_bucket = 0;
    for(int i = 0; i < HIST_BUCKET_COUNT; i++) {
        if(s->hist_buckets[i] > max_bucket) { max_bucket = s->hist_buckets[i]; }
    }

    for(int i = 0; i < HIST_BUCKET_COUNT; i++) {
        int64_t cnt = s->hist_buckets[i];
        double pct = total > 0 ? (double)cnt / (double)total * 100.0 : 0.0;
        int bar = max_bucket > 0 ? (int)((double)cnt / (double)max_bucket * 30) : 0;
        fprintf(stderr, "║  %-12s │", hist_bucket_label(i));
        for(int j = 0; j < bar; j++) { fprintf(stderr, "█"); }
        for(int j = bar; j < 30; j++) { fprintf(stderr, " "); }
        fprintf(stderr, "│ %6" PRId64 " (%5.1f%%)\n", cnt, pct);
    }

    fprintf(stderr, "╚══════════════════════════════════════════════════════════════════╝\n");
    fflush(stderr);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char *argv[]) {
    server_t server = {0};
    g_server = &server;

#ifdef _WIN32
    WSADATA wsa_data;
    if(WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return EXIT_FAILURE;
    }
#endif

    mutex_init(&server.stats_mutex);
    mutex_init(&server.storage_mutex);
    server.start_time_us = util_time_us();

    /* ---- Argument parsing ---- */
    args_flag_def_t flags[] = {
        {"listen",
         ARGS_TYPE_STRING,
         ARGS_OPTIONAL,
         ARGS_MULTIPLE,
         "server.listen",
         "Address and port to listen on (address:port)",
         {.has_default = false}},
        {"debug",
         ARGS_TYPE_STRING,
         ARGS_OPTIONAL,
         ARGS_ONCE,
         "logging.debug",
         "Debug level (ERROR, WARN, INFO, DETAIL, SPEW)",
         {.has_default = true, .value.string_val = "INFO"}},
        {"coils",
         ARGS_TYPE_INT,
         ARGS_OPTIONAL,
         ARGS_ONCE,
         "modbus.coils",
         "Number of coils",
         {.has_default = true, .value.int_val = 1000}},
        {"discrete-inputs",
         ARGS_TYPE_INT,
         ARGS_OPTIONAL,
         ARGS_ONCE,
         "modbus.discrete_inputs",
         "Number of discrete inputs",
         {.has_default = true, .value.int_val = 1000}},
        {"holding-registers",
         ARGS_TYPE_INT,
         ARGS_OPTIONAL,
         ARGS_ONCE,
         "modbus.holding_registers",
         "Number of holding registers",
         {.has_default = true, .value.int_val = 1000}},
        {"input-registers",
         ARGS_TYPE_INT,
         ARGS_OPTIONAL,
         ARGS_ONCE,
         "modbus.input_registers",
         "Number of input registers",
         {.has_default = true, .value.int_val = 1000}},
        {"help",
         ARGS_TYPE_BOOL,
         ARGS_OPTIONAL,
         ARGS_ONCE,
         "general.help",
         "Show this help message",
         {.has_default = true, .value.bool_val = false}},
    };
    const size_t num_flags = sizeof(flags) / sizeof(flags[0]);

    args_result_t args_result = {0};
    util_err_t parse_rc = args_parse(argc, (const char **)argv, flags, num_flags, &args_result);
    if(parse_rc != UTIL_OK) {
        fprintf(stderr, "Argument parse error: %s\n", args_get_error_detail(&args_result));
        args_print_help(argv[0], flags, num_flags);
        args_free(&args_result);
        return EXIT_FAILURE;
    }

    if(args_get_bool(&args_result, "help")) {
        args_print_help(argv[0], flags, num_flags);
        args_free(&args_result);
        return EXIT_SUCCESS;
    }

    /* ---- Logging level ---- */
    const char *debug_str = args_get_string(&args_result, "debug");
    log_level_t log_level = LOG_LEVEL_INFO;
    if(debug_str) {
        if(strcmp(debug_str, "ERROR") == 0) {
            log_level = LOG_LEVEL_ERROR;
        } else if(strcmp(debug_str, "WARN") == 0) {
            log_level = LOG_LEVEL_WARN;
        } else if(strcmp(debug_str, "INFO") == 0) {
            log_level = LOG_LEVEL_INFO;
        } else if(strcmp(debug_str, "DETAIL") == 0) {
            log_level = LOG_LEVEL_DETAIL;
        } else if(strcmp(debug_str, "SPEW") == 0) {
            log_level = LOG_LEVEL_SPEW;
        }
    }
    log_set_all_modules(log_level);

    /* ---- Register storage ---- */
    int64_t coils_val = args_get_int(&args_result, "coils");
    int64_t di_val = args_get_int(&args_result, "discrete-inputs");
    int64_t hr_val = args_get_int(&args_result, "holding-registers");
    int64_t ir_val = args_get_int(&args_result, "input-registers");

    if(coils_val < 0 || coils_val > 65535 || di_val < 0 || di_val > 65535 || hr_val < 0 || hr_val > 65535 || ir_val < 0
       || ir_val > 65535) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Register counts must be 0-65535");
        args_free(&args_result);
        return EXIT_FAILURE;
    }

    server.storage = register_storage_create((size_t)coils_val, (size_t)di_val, (size_t)hr_val, (size_t)ir_val);
    if(!server.storage) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Failed to create register storage");
        args_free(&args_result);
        return EXIT_FAILURE;
    }

    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "Modbus server (threaded) starting");
    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "  Coils:             %zu", (size_t)coils_val);
    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "  Discrete Inputs:   %zu", (size_t)di_val);
    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "  Holding Registers: %zu", (size_t)hr_val);
    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "  Input Registers:   %zu", (size_t)ir_val);

    /* ---- Signal handling ---- */
    util_set_interrupt_handler(signal_handler);

    /* ---- Start listener threads ---- */
    size_t listen_count = args_get_count(&args_result, "listen");
    if(listen_count == 0) { listen_count = 1; }

    server.listeners = (listener_ctx_t **)calloc(listen_count, sizeof(listener_ctx_t *));
    if(!server.listeners) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Out of memory");
        register_storage_destroy(server.storage);
        args_free(&args_result);
        return EXIT_FAILURE;
    }

    for(size_t i = 0; i < listen_count; i++) {
        const char *listen_addr = NULL;

        args_value_t val = args_get_at(&args_result, "listen", i);
        if(val.present) { listen_addr = val.value.string_val; }
        if(!listen_addr) { listen_addr = "127.0.0.1:502"; }

        /* Parse "host:port" — use strrchr so IPv6 addresses are handled */
        char addr_copy[256];
        strncpy(addr_copy, listen_addr, sizeof(addr_copy) - 1);
        addr_copy[sizeof(addr_copy) - 1] = '\0';

        char *colon = strrchr(addr_copy, ':');
        if(!colon) {
            pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Invalid listen address (expected host:port): %s", listen_addr);
            continue;
        }
        *colon = '\0';
        uint16_t port = (uint16_t)atoi(colon + 1);
        const char *host = addr_copy;

        listener_ctx_t *lctx = (listener_ctx_t *)calloc(1, sizeof(listener_ctx_t));
        if(!lctx) {
            pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Out of memory for listener context");
            continue;
        }
        strncpy(lctx->host, host, sizeof(lctx->host) - 1);
        lctx->port = port;
        lctx->server = &server;
        lctx->started = 0;

        if(thread_start(&lctx->thread, listener_thread, lctx) != 0) {
            pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Failed to start listener thread for %s:%u", lctx->host,
                  (unsigned)port);
            free(lctx);
            continue;
        }

        server.listeners[server.num_listeners++] = lctx;

        /* Wait up to 500ms for the listener to bind before printing status */
        for(int w = 0; w < 50 && lctx->started == 0; w++) { util_sleep_ms(10); }
        if(lctx->started < 0) {
            pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Listener failed to start on %s:%u", lctx->host, (unsigned)port);
        }
    }

    if(server.num_listeners == 0) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "No listeners started — exiting");
        register_storage_destroy(server.storage);
        free(server.listeners);
        args_free(&args_result);
        mutex_destroy(&server.stats_mutex);
        mutex_destroy(&server.storage_mutex);
        return EXIT_FAILURE;
    }

    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "Server running with %d listener(s).  Press Ctrl+C to stop.",
          server.num_listeners);

    /* ---- Main loop: wait for termination ---- */
    while(!g_terminate) { util_sleep_ms(100); }

    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "Shutting down...");

    /* ---- Join listener threads (each joins its own client threads first) ---- */
    for(int i = 0; i < server.num_listeners; i++) {
        thread_join(server.listeners[i]->thread);
        free(server.listeners[i]->clients);
        free(server.listeners[i]);
    }
    free(server.listeners);

    /* ---- Print final statistics ---- */
    print_statistics(&server);

    /* ---- Clean up ---- */
    register_storage_destroy(server.storage);
    args_free(&args_result);
    mutex_destroy(&server.stats_mutex);
    mutex_destroy(&server.storage_mutex);

#ifdef _WIN32
    WSACleanup();
#endif

    return EXIT_SUCCESS;
}

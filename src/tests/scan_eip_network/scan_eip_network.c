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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#    include <winsock2.h>
#    include <iphlpapi.h>
#    include <ws2ipdef.h>
#    pragma comment(lib, "ws2_32.lib")
#    pragma comment(lib, "iphlpapi.lib")
#else
#    include <arpa/inet.h>
#    include <ifaddrs.h>
#    include <net/if.h>
#endif

#include "tests/utils/socket.h"
#include "tests/utils/buf.h"
#include "tests/utils/args.h"
#include "tests/utils/log.h"
#include "tests/utils/utils.h"
#include "tests/utils/coro_net.h"
#include "tests/utils/err.h"


/* EtherNet/IP List Identity constants */
#define EIP_LIST_IDENTITY_CMD ((uint16_t)0x0063)
#define EIP_HEADER_SIZE (24)
#define EIP_LIST_IDENTITY_ITEM_TYPE ((uint16_t)0x000C)
#define EIP_BROADCAST_PORT (44818)

/* CIDR Network representation */
typedef struct {
    uint32_t network_base; /* Network address (network & mask) */
    uint32_t broadcast;    /* Broadcast address (network | ~mask) */
    uint32_t netmask;      /* Network mask (/prefix converted) */
    uint8_t prefix_len;    /* Prefix length (0-32) */
} cidr_net_t;

/* Device deduplication entry */
typedef struct {
    uint32_t sender_ip;     /* Sender's IPv4 address */
    uint32_t serial_number; /* Device serial number */
} dedup_key_t;

/* Dedup table - dynamic array */
typedef struct {
    dedup_key_t *entries; /* Array of keys seen so far */
    size_t count;         /* Number of entries */
    size_t capacity;      /* Allocated capacity */
} dedup_table_t;

/* Receiver coroutine context */
typedef struct {
    socket_t udp_sock;         /* UDP socket */
    socket_address_t src_addr; /* Source address for recvfrom */
    buf_t recv_buf;            /* Receive buffer */
    dedup_table_t dedup;       /* Deduplication table */
} receiver_ctx_t;

/* Timer coroutine context */
typedef struct {
    int64_t start_time_us; /* When timer started */
    uint32_t timeout_ms;   /* Timeout duration in milliseconds */
} timer_ctx_t;

/* Global flag for running state */
static volatile int g_running = 1;

/* ========================================================================== */
/* CIDR Parsing and Network Utilities                                        */
/* ========================================================================== */

static util_err_t parse_cidr(const char *cidr_str, cidr_net_t *out) {
    char ip_part[16];
    const char *slash;
    unsigned long prefix_val;
    int prefix_len;
    struct in_addr addr;

    if(!cidr_str || !out) { return UTIL_EINVAL; }

    /* Find the '/' separator */
    slash = strchr(cidr_str, '/');
    if(!slash) {
        pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_ERROR, "CIDR must contain '/' separator");
        return UTIL_EINVAL;
    }

    /* Extract and validate IP part */
    size_t ip_len = (size_t)(slash - cidr_str);
    if(ip_len >= sizeof(ip_part)) {
        pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_ERROR, "IP address too long");
        return UTIL_EINVAL;
    }
    memcpy(ip_part, cidr_str, ip_len);
    ip_part[ip_len] = '\0';

    /* Parse IP address */
    if(inet_pton(AF_INET, ip_part, &addr) != 1) {
        pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_ERROR, "Invalid IPv4 address: %s", ip_part);
        return UTIL_EINVAL;
    }

    /* Parse prefix length */
    prefix_val = strtoul(slash + 1, NULL, 10);
    if(prefix_val > 32) {
        pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_ERROR, "Prefix length must be 0-32, got %lu", prefix_val);
        return UTIL_EINVAL;
    }
    prefix_len = (int)prefix_val;

    /* Calculate network parameters */
    uint32_t ip_host_order = ntohl(addr.s_addr);
    uint32_t netmask = (prefix_len == 0) ? 0 : (0xFFFFFFFFU << (32 - prefix_len));
    uint32_t network_base = ip_host_order & netmask;
    uint32_t broadcast = network_base | (~netmask);

    out->network_base = network_base;
    out->broadcast = broadcast;
    out->netmask = netmask;
    out->prefix_len = (uint8_t)prefix_len;

    pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_INFO, "CIDR parsed: base=0x%08x broadcast=0x%08x mask=0x%08x prefix=%d",
          network_base, broadcast, netmask, prefix_len);

    return UTIL_OK;
}

/* ========================================================================== */
/* Interface Enumeration (OS-Specific)                                       */
/* ========================================================================== */

#ifdef _WIN32

static util_err_t find_interface_windows(const cidr_net_t *net, struct in_addr *out_addr) {
    PIP_ADAPTER_ADDRESSES adapters = NULL;
    PIP_ADAPTER_ADDRESSES adapter = NULL;
    DWORD ret_val = 0;
    ULONG out_buf_len = 0;

    if(!net || !out_addr) { return UTIL_EINVAL; }

    /* Get all adapters */
    out_buf_len = sizeof(IP_ADAPTER_ADDRESSES);
    adapters = (IP_ADAPTER_ADDRESSES *)malloc(out_buf_len);
    if(!adapters) {
        pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_ERROR, "Memory allocation failed");
        return UTIL_ERESOURCE;
    }

    ret_val = GetAdaptersAddresses(AF_INET, 0, NULL, adapters, &out_buf_len);
    if(ret_val == ERROR_BUFFER_OVERFLOW) {
        free(adapters);
        adapters = (IP_ADAPTER_ADDRESSES *)malloc(out_buf_len);
        if(!adapters) {
            pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_ERROR, "Memory allocation failed");
            return UTIL_ERESOURCE;
        }
        ret_val = GetAdaptersAddresses(AF_INET, 0, NULL, adapters, &out_buf_len);
    }

    if(ret_val != NO_ERROR) {
        pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_ERROR, "GetAdaptersAddresses failed: %ld", ret_val);
        free(adapters);
        return UTIL_EINTERNAL;
    }

    /* Find matching adapter */
    for(adapter = adapters; adapter != NULL; adapter = adapter->Next) {
        if(adapter->FirstUnicastAddress == NULL) { continue; }

        for(PIP_ADAPTER_UNICAST_ADDRESS unicast = adapter->FirstUnicastAddress; unicast != NULL; unicast = unicast->Next) {
            if(unicast->Address.lpSockaddr->sa_family == AF_INET) {
                struct sockaddr_in *sa = (struct sockaddr_in *)unicast->Address.lpSockaddr;
                uint32_t adapter_ip = ntohl(sa->sin_addr.s_addr);

                if((adapter_ip & net->netmask) == net->network_base) {
                    *out_addr = sa->sin_addr;
                    pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_INFO, "Found matching interface: %s", inet_ntoa(sa->sin_addr));
                    free(adapters);
                    return UTIL_OK;
                }
            }
        }
    }

    pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_ERROR, "No matching network interface found");
    free(adapters);
    return UTIL_ENOTFOUND;
}

#else

static util_err_t find_interface_posix(const cidr_net_t *net, struct in_addr *out_addr) {
    struct ifaddrs *ifaddr_list = NULL;
    struct ifaddrs *ifa = NULL;

    if(!net || !out_addr) { return UTIL_EINVAL; }

    if(getifaddrs(&ifaddr_list) == -1) {
        pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_ERROR, "getifaddrs failed");
        return UTIL_EINTERNAL;
    }

    /* Iterate through interfaces */
    for(ifa = ifaddr_list; ifa != NULL; ifa = ifa->ifa_next) {
        if(ifa->ifa_addr == NULL) { continue; }

        if(ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            uint32_t iface_ip = ntohl(sa->sin_addr.s_addr);

            if((iface_ip & net->netmask) == net->network_base) {
                *out_addr = sa->sin_addr;
                pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_INFO, "Found matching interface %s: %s", ifa->ifa_name,
                      inet_ntoa(sa->sin_addr));
                freeifaddrs(ifaddr_list);
                return UTIL_OK;
            }
        }
    }

    pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_ERROR, "No matching network interface found");
    freeifaddrs(ifaddr_list);
    return UTIL_ENOTFOUND;
}

#endif

static util_err_t find_interface(const cidr_net_t *net, struct in_addr *out_addr) {
#ifdef _WIN32
    return find_interface_windows(net, out_addr);
#else
    return find_interface_posix(net, out_addr);
#endif
}

/* ========================================================================== */
/* Socket Configuration                                                      */
/* ========================================================================== */

static util_err_t setup_broadcast_socket(socket_t *out_sock, const struct in_addr *local_addr) {
    util_err_t err;
    socket_t sock;
    socket_address_t sock_addr;
    char addr_str[INET_ADDRSTRLEN];

    if(!out_sock || !local_addr) { return UTIL_EINVAL; }

    /* Convert address to string for socket_address_init */
    inet_ntop(AF_INET, local_addr, addr_str, sizeof(addr_str));

    /* Bind to local address on ephemeral port using socket_create_udp_server */
    err = socket_address_init(&sock_addr, addr_str, 0);
    if(err != UTIL_OK) {
        pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_ERROR, "Failed to init socket address");
        return err;
    }

    sock = socket_create_udp_server(&sock_addr);
    if(sock == INVALID_SOCKET) {
        pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_ERROR, "Failed to create UDP server socket");
        return UTIL_EBIND;
    }

    /* Enable reuse address */
    err = socket_set_reuseaddr(sock, true);
    if(err != UTIL_OK) {
        pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_WARN, "Failed to set SO_REUSEADDR");
        socket_close(sock);
        return err;
    }

    /* Enable broadcast */
    err = socket_set_broadcast(sock, true);
    if(err != UTIL_OK) {
        pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_ERROR, "Failed to set SO_BROADCAST");
        socket_close(sock);
        return err;
    }

    /* Set non-blocking */
    err = socket_set_nonblocking(sock, true);
    if(err != UTIL_OK) {
        pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_ERROR, "Failed to set non-blocking");
        socket_close(sock);
        return err;
    }

    *out_sock = sock;
    pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_INFO, "Broadcast socket configured successfully");
    return UTIL_OK;
}

/* ========================================================================== */
/* List Identity Request Builder                                             */
/* ========================================================================== */

static util_err_t build_list_identity_request(buf_t *out_buf) {
    if(!buf_ok(out_buf)) { return UTIL_EINVAL; }

    /* Reset buffer */
    buf_reset(out_buf);

    /* Build EIP header (24 bytes, little-endian) */
    buf_write_u16_le(out_buf, "command", EIP_LIST_IDENTITY_CMD);
    buf_write_u16_le(out_buf, "length", 0);
    buf_write_u32_le(out_buf, "session_handle", 0);
    buf_write_u32_le(out_buf, "status", 0);
    buf_write_u64_le(out_buf, "sender_context", 0);
    buf_write_u32_le(out_buf, "options", 0);

    if(!buf_ok(out_buf)) { return buf_get_error(out_buf); }

    pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_SPEW, "Built List Identity request (24 bytes)");
    return UTIL_OK;
}

/* ========================================================================== */
/* Deduplication Table                                                       */
/* ========================================================================== */

static void dedup_table_init(dedup_table_t *tbl) {
    if(tbl) {
        tbl->entries = NULL;
        tbl->count = 0;
        tbl->capacity = 0;
    }
}

static void dedup_table_cleanup(dedup_table_t *tbl) {
    if(tbl && tbl->entries) {
        free(tbl->entries);
        tbl->entries = NULL;
        tbl->count = 0;
        tbl->capacity = 0;
    }
}

static bool dedup_table_has(const dedup_table_t *tbl, uint32_t ip, uint32_t serial) {
    if(!tbl) { return false; }

    for(size_t i = 0; i < tbl->count; i++) {
        if(tbl->entries[i].sender_ip == ip && tbl->entries[i].serial_number == serial) { return true; }
    }
    return false;
}

static util_err_t dedup_table_add(dedup_table_t *tbl, uint32_t ip, uint32_t serial) {
    if(!tbl) { return UTIL_EINVAL; }

    /* Check if already exists */
    if(dedup_table_has(tbl, ip, serial)) { return UTIL_OK; }

    /* Grow table if needed */
    if(tbl->count >= tbl->capacity) {
        size_t new_capacity = (tbl->capacity == 0) ? 16 : (tbl->capacity * 2);
        dedup_key_t *new_entries = (dedup_key_t *)realloc(tbl->entries, new_capacity * sizeof(dedup_key_t));
        if(!new_entries) {
            pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_ERROR, "Failed to allocate dedup table");
            return UTIL_ERESOURCE;
        }
        tbl->entries = new_entries;
        tbl->capacity = new_capacity;
    }

    /* Add new entry */
    tbl->entries[tbl->count].sender_ip = ip;
    tbl->entries[tbl->count].serial_number = serial;
    tbl->count++;

    return UTIL_OK;
}

/* ========================================================================== */
/* List Identity Item Parser                                                 */
/* ========================================================================== */

static util_err_t parse_list_identity_item(buf_t *item_buf, uint32_t sender_ip, dedup_table_t *dedup) {
    char product_name_buf[256];
    uint16_t port_net;
    uint32_t ipv4_addr_net;
    uint16_t vendor_id;
    uint16_t device_type;
    uint16_t product_code;
    uint8_t revision_major, revision_minor;
    uint16_t status_word;
    uint32_t serial_number;
    uint8_t product_name_len;
    uint8_t state = 0;

    if(!buf_ok(item_buf)) { return buf_get_error(item_buf); }

    /* Parse fixed fields (port and address are big-endian, others little-endian) */
    buf_read_advance(item_buf, 2); /* skip protocol_version */
    buf_read_advance(item_buf, 2); /* skip sin_family */

    if(!buf_read_u16_be(item_buf, "port", &port_net) || !buf_read_u32_be(item_buf, "ipv4_addr", &ipv4_addr_net)) {
        return buf_get_error(item_buf);
    }

    buf_read_advance(item_buf, 8); /* skip reserved */

    if(!buf_read_u16_le(item_buf, "vendor_id", &vendor_id) || !buf_read_u16_le(item_buf, "device_type", &device_type)
       || !buf_read_u16_le(item_buf, "product_code", &product_code) || !buf_read_u8(item_buf, "revision_major", &revision_major)
       || !buf_read_u8(item_buf, "revision_minor", &revision_minor) || !buf_read_u16_le(item_buf, "status_word", &status_word)
       || !buf_read_u32_le(item_buf, "serial_number", &serial_number)
       || !buf_read_u8(item_buf, "product_name_len", &product_name_len)) {
        return buf_get_error(item_buf);
    }

    /* Extract product name (not null-terminated) */
    memset(product_name_buf, 0, sizeof(product_name_buf));
    size_t copy_len = (product_name_len < sizeof(product_name_buf) - 1) ? product_name_len : sizeof(product_name_buf) - 1;
    if(!buf_read_bytes(item_buf, "product_name", (uint8_t *)product_name_buf, copy_len)) { return buf_get_error(item_buf); }
    product_name_buf[copy_len] = '\0';

    /* Extract state if present */
    if(buf_read_size(item_buf) > 0) { buf_read_u8(item_buf, "state", &state); }

    /* Check deduplication */
    if(dedup_table_has(dedup, sender_ip, serial_number)) {
        pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_SPEW, "Duplicate device: IP=0x%08x Serial=%u", sender_ip, serial_number);
        return UTIL_OK;
    }

    /* Add to dedup table */
    dedup_table_add(dedup, sender_ip, serial_number);

    /* buf_read_u32_be() returns host-order, but inet_ntop() expects network order */
    uint16_t port_host = ntohs(port_net); /* convert from network to host order */
    struct in_addr addr;
    addr.s_addr = htonl(ipv4_addr_net); /* convert from host back to network order for inet_ntop */
    char addr_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, addr_str, sizeof(addr_str));

    /* Print device line */
    printf("%s\t%u\t%u\t%u\t%u\t%u.%u\t%u\t%u\t%s\t%u\n", addr_str, port_host, vendor_id, device_type, product_code,
           revision_major, revision_minor, status_word, serial_number, product_name_buf, state);

    return UTIL_OK;
}

/* ========================================================================== */
/* Response Parser (EIP + CPF)                                               */
/* ========================================================================== */

static util_err_t parse_eip_list_identity_response(const buf_t *recv_buf, const socket_address_t *src_addr,
                                                   dedup_table_t *dedup) {
    buf_t parse_buf;
    uint16_t command;
    uint16_t length;
    uint16_t item_count;
    uint16_t item_type;
    uint16_t item_length;

    if(!buf_ok(recv_buf) || !src_addr || !dedup) { return UTIL_EINVAL; }

    /* Create a working copy for parsing */
    parse_buf = *recv_buf;

    /* Parse EIP encapsulation header (little-endian) */
    if(!buf_read_u16_le(&parse_buf, "command", &command)) {
        pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_WARN, "Failed to read EIP command");
        return buf_get_error(&parse_buf);
    }

    if(!buf_read_u16_le(&parse_buf, "length", &length)) {
        pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_WARN, "Failed to read EIP length");
        return buf_get_error(&parse_buf);
    }

    /* Validate command */
    if(command != EIP_LIST_IDENTITY_CMD) {
        pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_WARN, "Unexpected command: 0x%04x (expected 0x%04x)", command,
              EIP_LIST_IDENTITY_CMD);
        return UTIL_EINTERNAL;
    }

    /* Skip rest of header (session, status, context, options) */
    buf_read_advance(&parse_buf, 4 + 4 + 8 + 4); /* session, status, context, options */

    /* Parse CPF item count (little-endian) */
    if(!buf_read_u16_le(&parse_buf, "item_count", &item_count)) {
        pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_WARN, "Failed to read CPF item count");
        return buf_get_error(&parse_buf);
    }

    /* Extract sender IP from socket address */
    uint32_t sender_ip = ntohl(*(uint32_t *)&((struct sockaddr_in *)&src_addr->addr)->sin_addr);

    /* Process each CPF item */
    for(uint16_t i = 0; i < item_count; i++) {
        if(!buf_read_u16_le(&parse_buf, "item_type", &item_type) || !buf_read_u16_le(&parse_buf, "item_length", &item_length)) {
            break;
        }

        /* Create a buffer view for just this item */
        if(buf_read_size(&parse_buf) >= (size_t)item_length) {
            buf_t item_buf = parse_buf;
            item_buf.write = item_buf.read + item_length;

            /* Process List Identity item */
            if(item_type == EIP_LIST_IDENTITY_ITEM_TYPE) { parse_list_identity_item(&item_buf, sender_ip, dedup); }

            /* Advance past the item */
            buf_read_advance(&parse_buf, item_length);
        } else {
            break;
        }
    }

    return UTIL_OK;
}

/* ========================================================================== */
/* Receiver Coroutine Task                                                   */
/* ========================================================================== */

static void receiver_handler(coro_task_handle_t handle, socket_t fd, void *context) {
    receiver_ctx_t *ctx = (receiver_ctx_t *)context;
    util_err_t err;

    CORO_START(handle);

    pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_INFO, "Receiver task started");

    while(g_running) {
        /* Yield until data arrives */
        while((err = socket_recvfrom_buf(fd, &ctx->src_addr, &ctx->recv_buf)) == UTIL_EAGAIN) {
            coro_yield(handle, CORO_EVENT_READ);
        }

        if(err != UTIL_OK) {
            if(err != UTIL_ECLOSED) {
                pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_WARN, "Receive error: %s", util_err_str(err));
            }
            break;
        }

        /* Parse response */
        parse_eip_list_identity_response(&ctx->recv_buf, &ctx->src_addr, &ctx->dedup);

        /* Reset buffer for next receive */
        buf_reset(&ctx->recv_buf);
    }

    pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_INFO, "Receiver task ended");
    CORO_END(handle);
}

/* ========================================================================== */
/* Timer Coroutine Task                                                      */
/* ========================================================================== */

static void timer_handler(coro_task_handle_t handle, socket_t unused_fd, void *context) {
    timer_ctx_t *ctx = (timer_ctx_t *)context;
    uint64_t elapsed_ms;

    (void)unused_fd;

    CORO_START(handle);

    pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_INFO, "Timer task started (timeout=%u ms)", ctx->timeout_ms);

    while(g_running) {
        elapsed_ms = (util_time_us() - ctx->start_time_us) / 1000;

        if(elapsed_ms >= (int64_t)ctx->timeout_ms) {
            pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_INFO, "Timeout reached (%ld ms)", elapsed_ms);
            g_running = 0;
            coro_stop(handle.coro_net);
            break;
        }

        coro_yield(handle, CORO_EVENT_ALWAYS);
    }

    pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_INFO, "Timer task ended");
    CORO_END(handle);
}

/* ========================================================================== */
/* Main Function                                                             */
/* ========================================================================== */

int main(int argc, char *argv[]) {
    args_result_t args_result = {0};
    args_flag_def_t flags[] = {
        {"delay-max-ms",
         ARGS_TYPE_INT,
         ARGS_REQUIRED,
         ARGS_ONCE,
         "scan.delay_max_ms",
         "Maximum delay in milliseconds (100-2000)",
         {.has_default = false}},
        {"network",
         ARGS_TYPE_STRING,
         ARGS_REQUIRED,
         ARGS_ONCE,
         "scan.network",
         "Network in CIDR notation (e.g., 192.168.1.0/24)",
         {.has_default = false}},
    };
    const char *network_str = NULL;
    int64_t delay_ms_val = 0;
    uint32_t delay_ms = 0;
    cidr_net_t network = {0};
    struct in_addr local_addr = {0};
    socket_t broadcast_sock = INVALID_SOCKET;
    coro_net_t *coro_net = NULL;
    coro_task_handle_t receiver_task = {0};
    coro_task_handle_t timer_task = {0};
    receiver_ctx_t receiver_ctx = {0};
    timer_ctx_t timer_ctx = {0};
    buf_t request_buf = {0};
    uint8_t request_data[256];
    uint8_t recv_buf_data[4096];
    socket_address_t broadcast_addr = {0};
    util_err_t err;
    int exit_code = EXIT_FAILURE;

    /* Initialize socket layer */
    socket_init();

    /* Parse arguments */
    err = args_parse(argc, (const char **)argv, flags, sizeof(flags) / sizeof(flags[0]), &args_result);
    if(err != UTIL_OK) {
        fprintf(stderr, "ERROR: Argument parsing failed: %s\n", args_get_error_detail(&args_result));
        fprintf(stderr, "USAGE: %s --delay-max-ms=<100-2000> --network=<w.x.y.z/p>\n", argv[0]);
        goto cleanup;
    }

    /* Extract parsed values */
    delay_ms_val = args_get_int(&args_result, "delay-max-ms");
    network_str = args_get_string(&args_result, "network");

    if(!network_str) {
        fprintf(stderr, "ERROR: Missing required --network argument\n");
        fprintf(stderr, "USAGE: %s --delay-max-ms=<100-2000> --network=<w.x.y.z/p>\n", argv[0]);
        goto cleanup;
    }

    /* Validate delay */
    delay_ms = (uint32_t)delay_ms_val;
    if(delay_ms < 100 || delay_ms > 2000) {
        fprintf(stderr, "ERROR: delay-max-ms must be 100-2000, got %u\n", delay_ms);
        goto cleanup;
    }

    pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_INFO, "Starting scan_eip_network");
    pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_INFO, "Delay: %u ms, Network: %s", delay_ms, network_str);

    /* Parse CIDR network */
    err = parse_cidr(network_str, &network);
    if(err != UTIL_OK) { goto cleanup; }

    /* Find matching interface */
    err = find_interface(&network, &local_addr);
    if(err != UTIL_OK) { goto cleanup; }

    /* Setup broadcast socket */
    err = setup_broadcast_socket(&broadcast_sock, &local_addr);
    if(err != UTIL_OK) { goto cleanup; }

    /* Initialize receive buffer */
    request_buf = buf_init(request_data, sizeof(request_data));

    /* Build and send List Identity request */
    err = build_list_identity_request(&request_buf);
    if(err != UTIL_OK) { goto cleanup; }

    /* Setup broadcast address */
    struct in_addr broadcast_in_addr;
    broadcast_in_addr.s_addr = htonl(network.broadcast);
    char broadcast_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &broadcast_in_addr, broadcast_str, sizeof(broadcast_str));
    socket_address_init(&broadcast_addr, broadcast_str, EIP_BROADCAST_PORT);

    /* Send request */
    err = socket_sendto_buf(broadcast_sock, &broadcast_addr, &request_buf);
    if(err != UTIL_OK) {
        pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_ERROR, "Failed to send broadcast: %s", util_err_str(err));
        goto cleanup;
    }

    pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_INFO, "List Identity request sent to broadcast");

    /* Create coro_net */
    err = coro_create(&coro_net, 10);
    if(err != UTIL_OK) {
        pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_ERROR, "Failed to create coro_net");
        goto cleanup;
    }

    /* Initialize receiver context */
    dedup_table_init(&receiver_ctx.dedup);
    receiver_ctx.udp_sock = broadcast_sock;
    receiver_ctx.recv_buf = buf_init(recv_buf_data, sizeof(recv_buf_data));

    /* Create receiver task */
    err = coro_add_task(&receiver_task, coro_net, broadcast_sock, receiver_handler, &receiver_ctx);
    if(err != UTIL_OK) {
        pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_ERROR, "Failed to create receiver task");
        goto cleanup;
    }

    /* Initialize timer context */
    timer_ctx.start_time_us = util_time_us();
    timer_ctx.timeout_ms = delay_ms;

    /* Create timer task */
    err = coro_add_task(&timer_task, coro_net, CORO_NO_SOCKET, timer_handler, &timer_ctx);
    if(err != UTIL_OK) {
        pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_ERROR, "Failed to create timer task");
        goto cleanup;
    }

    /* Print header */
    printf("IP_Address\tPort\tVendor\tDevice_Type\tProduct_Code\tRevision\tStatus\tSerial\tProduct_Name\tState\n");

    /* Run event loop */
    pdlog(LOG_MODULE_SCAN_EIP_NETWORK, LOG_LEVEL_INFO, "Starting event loop");
    coro_run(&coro_net, 15); /* 15ms tick interval */

    /* Print results */
    printf("\nScan complete. Found %zu unique devices.\n", receiver_ctx.dedup.count);

    exit_code = EXIT_SUCCESS;

cleanup:
    /* Cleanup tasks */
    if(receiver_task.coro_net) { coro_remove_task(receiver_task); }
    if(timer_task.coro_net) { coro_remove_task(timer_task); }

    /* Cleanup coro_net */
    coro_destroy(&coro_net);

    /* Cleanup dedup table */
    dedup_table_cleanup(&receiver_ctx.dedup);

    /* Cleanup socket */
    if(broadcast_sock != INVALID_SOCKET) { socket_close(broadcast_sock); }

    /* Cleanup args */
    args_free(&args_result);

    /* Cleanup socket layer */
    socket_cleanup();

    return exit_code;
}

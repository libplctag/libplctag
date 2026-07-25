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
 * Verifies the ab_server's EtherNet/IP ListIdentity (0x63) responder.
 *
 * ListIdentity is an unconnected encapsulation command: send the 24-byte EIP
 * header with command 0x63 (no session, no payload) and the server replies with
 * a CPF CIP Identity item. This test checks that the emulator advertises the
 * fixed, generic identity it is supposed to - in particular vendor id 0
 * (unspecified; the emulator must not impersonate a real vendor) and the product
 * name "libplctag ab_server".
 *
 * Usage: test_list_identity [host] [port]   (defaults: 127.0.0.1 44818)
 * Exit code 0 on success, 1 on any failure.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    include <windows.h>
typedef SOCKET sock_t;
#    define SOCK_INVALID INVALID_SOCKET
#    define close_sock closesocket
#else
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <sys/time.h>
#    include <time.h>
#    include <unistd.h>
typedef int sock_t;
#    define SOCK_INVALID (-1)
#    define close_sock close
#endif

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT (44818)
#define EIP_CMD_LIST_IDENTITY (0x0063)
#define EIP_HEADER_SIZE (24)
#define EXPECTED_VENDOR_ID (0)
#define EXPECTED_PRODUCT "libplctag ab_server"
#define RECV_TIMEOUT_MS (5000)

/* CPF identity field offsets within the reply (header(24) + CPF item header(6) + body). */
#define OFF_ITEM_COUNT (EIP_HEADER_SIZE + 0) /* 24 */
#define OFF_ITEM_TYPE (EIP_HEADER_SIZE + 2)  /* 26 */
#define OFF_VENDOR_ID (EIP_HEADER_SIZE + 24) /* 48: item header(6) + encap-ver(2) + sockaddr(16) */
#define OFF_PRODUCT_CODE (EIP_HEADER_SIZE + 28)   /* 52 */
#define OFF_REVISION_MAJOR (EIP_HEADER_SIZE + 30) /* 54 */
#define OFF_REVISION_MINOR (EIP_HEADER_SIZE + 31) /* 55 */
#define OFF_SERIAL (EIP_HEADER_SIZE + 34)    /* 58 */
#define OFF_NAME_LEN (EIP_HEADER_SIZE + 38)  /* 62 */
#define OFF_NAME (EIP_HEADER_SIZE + 39)      /* 63 */
#define CPF_ITEM_CIP_IDENTITY (0x000C)

static uint16_t rd_u16le(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int set_recv_timeout(sock_t fd) {
#ifdef _WIN32
    DWORD ms = RECV_TIMEOUT_MS;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec = RECV_TIMEOUT_MS / 1000;
    tv.tv_usec = (RECV_TIMEOUT_MS % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

int main(int argc, char **argv) {
    const char *host = (argc > 1) ? argv[1] : DEFAULT_HOST;
    int port = (argc > 2) ? atoi(argv[2]) : DEFAULT_PORT;
    sock_t fd = SOCK_INVALID;
    int rc = 1;

#ifdef _WIN32
    WSADATA wsa;
    if(WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup() failed\n");
        return 1;
    }
#endif

    do {
        struct sockaddr_in addr;
        uint8_t req[EIP_HEADER_SIZE];
        uint8_t resp[256];
        size_t got = 0;
        uint16_t vendor = 0;
        uint8_t name_len = 0;
        char name[64];

        /* Build the 24-byte ListIdentity request: command 0x63, everything else zero
         * (length 0, no session, no payload). */
        memset(req, 0, sizeof(req));
        req[0] = (uint8_t)(EIP_CMD_LIST_IDENTITY & 0xFF);
        req[1] = (uint8_t)((EIP_CMD_LIST_IDENTITY >> 8) & 0xFF);

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((unsigned short)port);
        if(inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
            fprintf(stderr, "Invalid host address: %s\n", host);
            break;
        }

        /* The server may still be coming up; retry the connection briefly. */
        {
            int connected = 0;
            int attempt;
            for(attempt = 0; attempt < 25; attempt++) {
                fd = socket(AF_INET, SOCK_STREAM, 0);
                if(fd == SOCK_INVALID) {
                    fprintf(stderr, "socket() failed\n");
                    break;
                }
                (void)set_recv_timeout(fd);
                if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                    connected = 1;
                    break;
                }
                close_sock(fd);
                fd = SOCK_INVALID;
                sleep_ms(200);
            }
            if(!connected) {
                fprintf(stderr, "connect() to %s:%d failed\n", host, port);
                break;
            }
        }

        if(send(fd, (const char *)req, (int)sizeof(req), 0) != (int)sizeof(req)) {
            fprintf(stderr, "send() of ListIdentity request failed\n");
            break;
        }

        /* Read until we have the full identity (through the product name + state byte)
         * or the peer/timeout stops us. */
        while(got < sizeof(resp)) {
            size_t remaining = sizeof(resp) - got;
#ifdef _WIN32
            int n = recv(fd, (char *)(resp + got), (int)remaining, 0);
#else
            ssize_t n = recv(fd, (char *)(resp + got), remaining, 0);
#endif
            if(n <= 0) { break; }
            got += (size_t)n;
            if(got >= (size_t)(OFF_NAME + 1)) { break; }
        }

        if(got < (size_t)(OFF_VENDOR_ID + 2)) {
            fprintf(stderr, "Short ListIdentity reply: %u bytes\n", (unsigned)got);
            break;
        }

        if(rd_u16le(resp + 0) != EIP_CMD_LIST_IDENTITY) {
            fprintf(stderr, "Reply command is 0x%04x, expected 0x%04x\n", rd_u16le(resp + 0), EIP_CMD_LIST_IDENTITY);
            break;
        }
        if(rd_u16le(resp + OFF_ITEM_COUNT) != 1) {
            fprintf(stderr, "Expected 1 CPF item, got %u\n", rd_u16le(resp + OFF_ITEM_COUNT));
            break;
        }
        if(rd_u16le(resp + OFF_ITEM_TYPE) != CPF_ITEM_CIP_IDENTITY) {
            fprintf(stderr, "Expected CIP Identity item 0x%04x, got 0x%04x\n", CPF_ITEM_CIP_IDENTITY,
                    rd_u16le(resp + OFF_ITEM_TYPE));
            break;
        }

        vendor = rd_u16le(resp + OFF_VENDOR_ID);
        if(vendor != EXPECTED_VENDOR_ID) {
            fprintf(stderr, "Expected vendor id %d (unspecified), got %u - the emulator must not impersonate a vendor\n",
                    EXPECTED_VENDOR_ID, vendor);
            break;
        }

        name_len = resp[OFF_NAME_LEN];
        if((size_t)(OFF_NAME + name_len) > got) {
            fprintf(stderr, "Product name (len %u) runs past the %u-byte reply\n", name_len, (unsigned)got);
            break;
        }
        if(name_len >= sizeof(name)) {
            fprintf(stderr, "Product name too long: %u\n", name_len);
            break;
        }
        memcpy(name, resp + OFF_NAME, name_len);
        name[name_len] = '\0';
        if(strcmp(name, EXPECTED_PRODUCT) != 0) {
            fprintf(stderr, "Expected product name %s, got %s\n", EXPECTED_PRODUCT, name);
            break;
        }

        printf("ListIdentity OK: vendor id %u, product code %u, product name %s, revision %u.%u, serial 0x%08x\n",
               vendor, rd_u16le(resp + OFF_PRODUCT_CODE), name, resp[OFF_REVISION_MAJOR], resp[OFF_REVISION_MINOR],
               (unsigned)rd_u32le(resp + OFF_SERIAL));
        rc = 0;
    } while(0);

    if(fd != SOCK_INVALID) { close_sock(fd); }
#ifdef _WIN32
    WSACleanup();
#endif
    return rc;
}

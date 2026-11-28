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
 * Request framing in modbus_server's TCP loop, over a raw socket so the client library's own
 * framing is not in the way.  A request that arrives in pieces stays in the server's buffer
 * until the rest comes in with another read, and is processed once it is whole:
 *
 *   - a write split inside the MBAP header, then inside the PDU, is applied and answered.
 *   - a read split the same way returns the value the split write stored.
 *   - the connection stays usable for a whole request afterwards.
 *
 * Usage: test_modbus_server_framing <port>, against an already running modbus_server.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#    include <winsock2.h>
#    include <ws2tcpip.h>
typedef SOCKET sock_t;
typedef int io_len_t;
#    define sock_close closesocket
#    define sleep_ms(ms) Sleep(ms)
#else
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <sys/time.h>
#    include <time.h>
#    include <unistd.h>
typedef int sock_t;
typedef size_t io_len_t;
#    define INVALID_SOCKET (-1)
#    define sock_close close
/* usleep is gone from POSIX 2008, which the build asks for. */
#    define sleep_ms(ms) nanosleep(&(struct timespec){.tv_sec = (ms) / 1000, .tv_nsec = ((ms) % 1000) * 1000000L}, NULL)
#endif

#define MB_READ_HOLDING ((uint8_t)0x03)
#define MB_WRITE_SINGLE ((uint8_t)0x06)
#define MB_REQUEST_SIZE (12) /* MBAP (7) + function, address, quantity or value (5) */
#define MB_READ_ONE_REPLY_SIZE (11) /* MBAP (7) + function, byte count, one register (4) */
#define MB_WRITE_SINGLE_REPLY_SIZE (12) /* the request echoed */
#define TEST_REGISTER ((uint16_t)200)
#define TEST_VALUE ((uint16_t)0x1234)


static sock_t connect_to(uint16_t port);
static bool send_all(sock_t sock, const uint8_t *data, size_t len);
static bool send_in_pieces(sock_t sock, const uint8_t *data, size_t len);
static bool read_exact(sock_t sock, uint8_t *buf, size_t len);
static void make_request(uint8_t *buf, uint16_t tid, uint8_t function, uint16_t address, uint16_t word);
static bool check_reply(const uint8_t *reply, uint16_t tid, uint8_t function, const char *what);


int main(int argc, char **argv) {
    uint8_t request[MB_REQUEST_SIZE];
    uint8_t reply[MB_WRITE_SINGLE_REPLY_SIZE];
    uint16_t port = 0;
    sock_t sock = INVALID_SOCKET;
    bool ok = true;

    if(argc != 2 || (port = (uint16_t)atoi(argv[1])) == 0) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

#ifdef _WIN32
    {
        WSADATA wsa;
        if(WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { return 1; }
    }
#endif

    sock = connect_to(port);
    ok = (sock != INVALID_SOCKET);

    /* a write in pieces: it must be applied, not dropped, once the last piece arrives. */
    make_request(request, 1, MB_WRITE_SINGLE, TEST_REGISTER, TEST_VALUE);
    ok = ok && send_in_pieces(sock, request, sizeof(request));
    ok = ok && read_exact(sock, reply, MB_WRITE_SINGLE_REPLY_SIZE);
    ok = ok && check_reply(reply, 1, MB_WRITE_SINGLE, "split write");
    if(ok && memcmp(reply + 7, request + 7, 5) != 0) {
        fprintf(stderr, "split write: the reply does not echo the request\n");
        ok = false;
    }

    /* a read in pieces returns what the write stored. */
    make_request(request, 2, MB_READ_HOLDING, TEST_REGISTER, 1);
    ok = ok && send_in_pieces(sock, request, sizeof(request));
    ok = ok && read_exact(sock, reply, MB_READ_ONE_REPLY_SIZE);
    ok = ok && check_reply(reply, 2, MB_READ_HOLDING, "split read");
    if(ok && (reply[8] != 2 || reply[9] != (uint8_t)(TEST_VALUE >> 8) || reply[10] != (uint8_t)(TEST_VALUE & 0xFF))) {
        fprintf(stderr, "split read: got byte count %u value 0x%02x%02x, expected 2 and 0x%04x\n", (unsigned)reply[8],
                (unsigned)reply[9], (unsigned)reply[10], (unsigned)TEST_VALUE);
        ok = false;
    }

    /* the connection is still in step: a whole request is answered. */
    make_request(request, 3, MB_READ_HOLDING, TEST_REGISTER, 1);
    ok = ok && send_all(sock, request, sizeof(request));
    ok = ok && read_exact(sock, reply, MB_READ_ONE_REPLY_SIZE);
    ok = ok && check_reply(reply, 3, MB_READ_HOLDING, "whole read after the split ones");

    if(sock != INVALID_SOCKET) { sock_close(sock); }

    printf("%s\n", ok ? "PASS" : "FAIL");

    return ok ? 0 : 1;
}


/* three writes with a pause after each: part of the MBAP header, the rest of it and part of the PDU, the rest. */
static bool send_in_pieces(sock_t sock, const uint8_t *data, size_t len) {
    bool ok = send_all(sock, data, 3);

    sleep_ms(200);
    ok = ok && send_all(sock, data + 3, 6);
    sleep_ms(200);
    ok = ok && send_all(sock, data + 9, len - 9);

    return ok;
}


static void make_request(uint8_t *buf, uint16_t tid, uint8_t function, uint16_t address, uint16_t word) {
    buf[0] = (uint8_t)(tid >> 8);
    buf[1] = (uint8_t)(tid & 0xFF);
    buf[2] = 0; /* protocol ID */
    buf[3] = 0;
    buf[4] = 0; /* length: unit ID and PDU */
    buf[5] = 6;
    buf[6] = 1; /* unit ID */
    buf[7] = function;
    buf[8] = (uint8_t)(address >> 8);
    buf[9] = (uint8_t)(address & 0xFF);
    buf[10] = (uint8_t)(word >> 8);
    buf[11] = (uint8_t)(word & 0xFF);
}


static bool check_reply(const uint8_t *reply, uint16_t tid, uint8_t function, const char *what) {
    uint16_t got_tid = (uint16_t)((reply[0] << 8) | reply[1]);

    if(got_tid != tid || reply[7] != function) {
        fprintf(stderr, "%s: reply has transaction %u function 0x%02x, expected %u and 0x%02x\n", what, (unsigned)got_tid,
                (unsigned)reply[7], (unsigned)tid, (unsigned)function);
        return false;
    }

    printf("%s: ok\n", what);

    return true;
}


static sock_t connect_to(uint16_t port) {
    struct sockaddr_in addr;
    sock_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
    DWORD timeout = 5000;
#else
    struct timeval timeout = {5, 0};
#endif

    if(sock == INVALID_SOCKET) { return INVALID_SOCKET; }

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if(connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "connect to port %u failed\n", (unsigned)port);
        sock_close(sock);
        return INVALID_SOCKET;
    }

    return sock;
}


static bool send_all(sock_t sock, const uint8_t *data, size_t len) {
    while(len > 0) {
        int32_t rc = (int32_t)send(sock, (const char *)data, (io_len_t)len, 0);

        if(rc <= 0) {
            fprintf(stderr, "send failed\n");
            return false;
        }

        data += rc;
        len -= (size_t)rc;
    }

    return true;
}


static bool read_exact(sock_t sock, uint8_t *buf, size_t len) {
    while(len > 0) {
        int32_t rc = (int32_t)recv(sock, (char *)buf, (io_len_t)len, 0);

        if(rc <= 0) {
            fprintf(stderr, "recv failed or connection closed\n");
            return false;
        }

        buf += rc;
        len -= (size_t)rc;
    }

    return true;
}

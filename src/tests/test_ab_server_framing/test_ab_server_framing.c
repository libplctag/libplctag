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
 * Encapsulation framing in ab_server's TCP loop, over a raw socket so the client library's
 * own framing is not in the way:
 *
 *   - a request split across two writes is answered, and the connection stays usable.
 *   - two requests in one write are both answered.
 *   - an unknown command gets EIP status 0x0001, not an internal code.
 *   - CIP requests are routed by class, then service: Read Tag Fragmented (0x52) sent straight
 *     in SendRRData reads the tag instead of being taken for Unconnected Send, an unknown class
 *     gets 0x05 and an unknown service on a known class gets 0x08.
 *   - Multiple Service answers each request in place and gets 0x1E when one fails; on any
 *     Message Router instance but 1 it gets 0x05.
 *   - A logical path with any segment after the class and instance gets 0x04.
 *   - A tag path whose index segment is cut short gets 0x03; a read, write or RMW payload cut short gets 0x13.
 *   - Execute PCCC echoes the requestor ID it was sent.
 *
 * The server must have a tag named TestReal.
 * Usage: test_ab_server_framing <port>, against an already running ab_server.
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
#    include <unistd.h>
typedef int sock_t;
typedef size_t io_len_t;
#    define INVALID_SOCKET (-1)
#    define sock_close close
#    define sleep_ms(ms) usleep((useconds_t)(ms) * 1000)
#endif

#define EIP_HEADER_SIZE (24)
#define EIP_LIST_IDENTITY ((uint16_t)0x0063)
#define EIP_REGISTER_SESSION ((uint16_t)0x0065)
#define EIP_BOGUS_COMMAND ((uint16_t)0x0099)
#define EIP_STATUS_INVALID_COMMAND ((uint32_t)0x0001)
#define EIP_SEND_RR_DATA ((uint16_t)0x006F)
#define CPF_UCONN_HEADER_SIZE (16)

#define CIP_OK ((uint8_t)0x00)
#define CIP_ERR_INVALID_PARAM ((uint8_t)0x03)
#define CIP_ERR_PATH_SEGMENT ((uint8_t)0x04)
#define CIP_ERR_PATH_DEST_UNKNOWN ((uint8_t)0x05)
#define CIP_ERR_UNSUPPORTED ((uint8_t)0x08)
#define CIP_ERR_INSUFFICIENT_DATA ((uint8_t)0x13)
#define CIP_ERR_EMBEDDED_SERVICE ((uint8_t)0x1E)

#define REGISTER_SESSION_SIZE (EIP_HEADER_SIZE + 4)


static sock_t connect_to(uint16_t port);
static bool send_all(sock_t sock, const uint8_t *data, size_t len);
static bool read_exact(sock_t sock, uint8_t *buf, size_t len);
static bool read_reply(sock_t sock, uint16_t command, uint32_t *status);
static uint32_t last_session;        /* session handle of the last reply read */
static uint8_t last_payload[600];    /* payload of the last reply read */
static size_t make_request(uint8_t *buf, uint16_t command);
static bool test_split_request(uint16_t port);
static bool test_pipelined(uint16_t port);
static bool test_bad_command(uint16_t port);
static bool test_cip_routing(uint16_t port);
static bool send_rr_data(sock_t sock, uint32_t session, const uint8_t *cip, size_t cip_len, uint8_t *cip_status);


int main(int argc, char **argv) {
    uint16_t port = 0;
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

    ok = test_split_request(port) && ok;
    ok = test_pipelined(port) && ok;
    ok = test_bad_command(port) && ok;
    ok = test_cip_routing(port) && ok;

    printf("%s\n", ok ? "PASS" : "FAIL");

    return ok ? 0 : 1;
}


static bool test_split_request(uint16_t port) {
    uint8_t buf[64];
    size_t len = make_request(buf, EIP_REGISTER_SESSION);
    uint32_t status = 0xFFFFFFFF;
    sock_t sock = connect_to(port);
    bool ok = (sock != INVALID_SOCKET);

    /* the server must read twice for the first request */
    ok = ok && send_all(sock, buf, 10);
    sleep_ms(200);
    ok = ok && send_all(sock, buf + 10, len - 10);
    ok = ok && read_reply(sock, EIP_REGISTER_SESSION, &status) && status == 0;

    /* the RegisterSession reply is where the client learns its session handle */
    if(ok && last_session == 0) {
        fprintf(stderr, "RegisterSession reply carries session handle 0\n");
        ok = false;
    }

    len = make_request(buf, EIP_LIST_IDENTITY);
    ok = ok && send_all(sock, buf, len);
    ok = ok && read_reply(sock, EIP_LIST_IDENTITY, &status) && status == 0;

    if(sock != INVALID_SOCKET) { sock_close(sock); }

    printf("test_ab_server_split_request: %s\n", ok ? "ok" : "FAILED");

    return ok;
}


static bool test_pipelined(uint16_t port) {
    uint8_t buf[128];
    size_t len = make_request(buf, EIP_REGISTER_SESSION);
    uint32_t status = 0xFFFFFFFF;
    sock_t sock = connect_to(port);
    bool ok = (sock != INVALID_SOCKET);

    len += make_request(buf + len, EIP_LIST_IDENTITY);

    ok = ok && send_all(sock, buf, len);
    ok = ok && read_reply(sock, EIP_REGISTER_SESSION, &status) && status == 0;
    ok = ok && read_reply(sock, EIP_LIST_IDENTITY, &status) && status == 0;

    if(sock != INVALID_SOCKET) { sock_close(sock); }

    printf("test_ab_server_pipelined: %s\n", ok ? "ok" : "FAILED");

    return ok;
}


static bool test_bad_command(uint16_t port) {
    uint8_t buf[64];
    size_t len = make_request(buf, EIP_BOGUS_COMMAND);
    uint32_t status = 0;
    sock_t sock = connect_to(port);
    bool ok = (sock != INVALID_SOCKET);

    ok = ok && send_all(sock, buf, len);
    ok = ok && read_reply(sock, EIP_BOGUS_COMMAND, &status);

    if(ok && status != EIP_STATUS_INVALID_COMMAND) {
        fprintf(stderr, "bad command: status 0x%x, expected 0x%x\n", (unsigned)status, (unsigned)EIP_STATUS_INVALID_COMMAND);
        ok = false;
    }

    /* the connection survives the error reply */
    len = make_request(buf, EIP_LIST_IDENTITY);
    ok = ok && send_all(sock, buf, len);
    ok = ok && read_reply(sock, EIP_LIST_IDENTITY, &status) && status == 0;

    if(sock != INVALID_SOCKET) { sock_close(sock); }

    printf("test_ab_server_bad_command: %s\n", ok ? "ok" : "FAILED");

    return ok;
}


static bool test_cip_routing(uint16_t port) {
    /* Read Tag Fragmented, one element at offset 0, of the tag TestReal */
    static const uint8_t read_frag[] = {0x52, 0x05, 0x91, 0x08, 'T', 'e', 's', 't', 'R', 'e', 'a', 'l', 0x01, 0x00, 0, 0, 0, 0};
    /* Get_Attribute_Single on class 0x99 instance 1 */
    static const uint8_t no_class[] = {0x0E, 0x02, 0x20, 0x99, 0x24, 0x01};
    /* service 0x33 on the Identity object */
    static const uint8_t no_service[] = {0x33, 0x02, 0x20, 0x01, 0x24, 0x01};
    /* Identity Get Attributes All with an attribute segment after the instance */
    static const uint8_t extra_segment[] = {0x01, 0x03, 0x20, 0x01, 0x24, 0x01, 0x30, 0x01};
    /* Multiple Service of read_frag then no_class; offsets count from the count word */
    static const uint8_t multi[] = {0x0A, 0x02, 0x20, 0x02, 0x24, 0x01, 2, 0, 6, 0, 24, 0,
                                    0x52, 0x05, 0x91, 0x08, 'T', 'e', 's', 't', 'R', 'e', 'a', 'l', 0x01, 0x00, 0, 0, 0, 0,
                                    0x0E, 0x02, 0x20, 0x99, 0x24, 0x01};
    /* Read Tag Fragmented of TestReal with a 16-bit index segment cut off after its pad byte */
    static const uint8_t truncated_index[] = {0x52, 0x06, 0x91, 0x08, 'T', 'e', 's', 't', 'R', 'e', 'a', 'l', 0x29, 0x00, 0x01, 0x00, 0, 0, 0, 0};
    /* Read Tag Fragmented of TestReal with only the element count; the byte offset is missing */
    static const uint8_t short_read[] = {0x52, 0x05, 0x91, 0x08, 'T', 'e', 's', 't', 'R', 'e', 'a', 'l', 0x01, 0x00};
    /* Write Tag of TestReal with the REAL type and an element count but no data */
    static const uint8_t short_write[] = {0x4D, 0x05, 0x91, 0x08, 'T', 'e', 's', 't', 'R', 'e', 'a', 'l', 0xCA, 0x00, 0x01, 0x00};
    /* Read-Modify-Write of TestReal with 4 byte masks but only one byte of OR mask */
    static const uint8_t short_rmw[] = {0x4E, 0x05, 0x91, 0x08, 'T', 'e', 's', 't', 'R', 'e', 'a', 'l', 0x04, 0x00, 0xFF};
    /*
     * Execute PCCC with a foreign requestor ID and an SLC read, which a ControlLogix does not
     * serve: the reply echoes the requestor ID, then 0x4F, 0xF0, the TNS and status 0x0E.
     */
    static const uint8_t pccc[] = {0x4B, 0x02, 0x20, 0x67, 0x24, 0x01, 0x07, 0x11, 0x22,
                                   0x33, 0x44, 0x55, 0x66, 0x0F, 0x00, 0x34, 0x12, 0xA2};
    /* Multiple Service of no_class on Message Router instance 2 */
    static const uint8_t multi_bad_instance[] = {0x0A, 0x02, 0x20, 0x02, 0x24, 0x02, 1, 0, 4, 0, 0x0E, 0x02, 0x20, 0x99, 0x24, 0x01};
    const uint8_t *reply = last_payload + CPF_UCONN_HEADER_SIZE;
    uint8_t buf[64];
    size_t len = make_request(buf, EIP_REGISTER_SESSION);
    uint32_t status = 0xFFFFFFFF;
    uint8_t cip_status = 0xFF;
    sock_t sock = connect_to(port);
    bool ok = (sock != INVALID_SOCKET);

    ok = ok && send_all(sock, buf, len);
    ok = ok && read_reply(sock, EIP_REGISTER_SESSION, &status) && status == 0;

    if(ok && !(send_rr_data(sock, last_session, read_frag, sizeof(read_frag), &cip_status) && cip_status == CIP_OK)) {
        fprintf(stderr, "Read Tag Fragmented in SendRRData: CIP status 0x%02x, expected 0\n", cip_status);
        ok = false;
    }

    if(ok && !(send_rr_data(sock, last_session, no_class, sizeof(no_class), &cip_status) && cip_status == CIP_ERR_PATH_DEST_UNKNOWN)) {
        fprintf(stderr, "unknown class: CIP status 0x%02x, expected 0x05\n", cip_status);
        ok = false;
    }

    if(ok && !(send_rr_data(sock, last_session, no_service, sizeof(no_service), &cip_status) && cip_status == CIP_ERR_UNSUPPORTED)) {
        fprintf(stderr, "unknown service: CIP status 0x%02x, expected 0x08\n", cip_status);
        ok = false;
    }

    if(ok && !(send_rr_data(sock, last_session, extra_segment, sizeof(extra_segment), &cip_status)
               && cip_status == CIP_ERR_PATH_SEGMENT)) {
        fprintf(stderr, "segment after the instance: CIP status 0x%02x, expected 0x04\n", cip_status);
        ok = false;
    }

    if(ok && !(send_rr_data(sock, last_session, truncated_index, sizeof(truncated_index), &cip_status)
               && cip_status == CIP_ERR_INVALID_PARAM)) {
        fprintf(stderr, "truncated index segment: CIP status 0x%02x, expected 0x03\n", cip_status);
        ok = false;
    }

    if(ok && !(send_rr_data(sock, last_session, short_read, sizeof(short_read), &cip_status)
               && cip_status == CIP_ERR_INSUFFICIENT_DATA)) {
        fprintf(stderr, "short read payload: CIP status 0x%02x, expected 0x13\n", cip_status);
        ok = false;
    }

    if(ok && !(send_rr_data(sock, last_session, short_write, sizeof(short_write), &cip_status)
               && cip_status == CIP_ERR_INSUFFICIENT_DATA)) {
        fprintf(stderr, "short write payload: CIP status 0x%02x, expected 0x13\n", cip_status);
        ok = false;
    }

    if(ok && !(send_rr_data(sock, last_session, short_rmw, sizeof(short_rmw), &cip_status)
               && cip_status == CIP_ERR_INSUFFICIENT_DATA)) {
        fprintf(stderr, "short RMW payload: CIP status 0x%02x, expected 0x13\n", cip_status);
        ok = false;
    }

    if(ok && !(send_rr_data(sock, last_session, pccc, sizeof(pccc), &cip_status) && cip_status == CIP_OK
               && memcmp(reply + 4, pccc + 6, 7) == 0 && reply[11] == 0x4F && reply[12] == 0xF0 && reply[13] == 0x34
               && reply[14] == 0x12 && reply[15] == 0x0E)) {
        fprintf(stderr, "Execute PCCC: CIP status 0x%02x, expected the requestor ID echoed and PCCC status 0x0E\n", cip_status);
        ok = false;
    }

    /* replies: Read Tag Fragmented is 10 bytes (header, type, REAL), so the second is at 16 */
    if(ok && !(send_rr_data(sock, last_session, multi, sizeof(multi), &cip_status) && cip_status == CIP_ERR_EMBEDDED_SERVICE
               && reply[6] == 6 && reply[8] == 16 && reply[4 + 6 + 2] == CIP_OK && reply[4 + 16 + 2] == CIP_ERR_PATH_DEST_UNKNOWN)) {
        fprintf(stderr, "Multiple Service: CIP status 0x%02x, expected 0x1E with sub-replies 0 and 0x05\n", cip_status);
        ok = false;
    }

    if(ok && !(send_rr_data(sock, last_session, multi_bad_instance, sizeof(multi_bad_instance), &cip_status)
               && cip_status == CIP_ERR_PATH_DEST_UNKNOWN)) {
        fprintf(stderr, "Multiple Service on instance 2: CIP status 0x%02x, expected 0x05\n", cip_status);
        ok = false;
    }

    if(sock != INVALID_SOCKET) { sock_close(sock); }

    printf("test_ab_server_cip_routing: %s\n", ok ? "ok" : "FAILED");

    return ok;
}


/* one unconnected CIP request in SendRRData; *cip_status is the general status of the reply. */
static bool send_rr_data(sock_t sock, uint32_t session, const uint8_t *cip, size_t cip_len, uint8_t *cip_status) {
    uint8_t buf[128] = {0};
    size_t len = EIP_HEADER_SIZE + CPF_UCONN_HEADER_SIZE + cip_len;
    size_t payload_len = len - EIP_HEADER_SIZE;
    uint32_t status = 0;

    buf[0] = (uint8_t)(EIP_SEND_RR_DATA & 0xFF);
    buf[2] = (uint8_t)(payload_len & 0xFF);
    buf[3] = (uint8_t)(payload_len >> 8);
    buf[4] = (uint8_t)(session & 0xFF);
    buf[5] = (uint8_t)((session >> 8) & 0xFF);
    buf[6] = (uint8_t)((session >> 16) & 0xFF);
    buf[7] = (uint8_t)(session >> 24);

    /* CPF: interface 0, timeout 0, two items: null address, unconnected data */
    buf[EIP_HEADER_SIZE + 6] = 2;
    buf[EIP_HEADER_SIZE + 12] = 0xB2;
    buf[EIP_HEADER_SIZE + 14] = (uint8_t)cip_len;
    memcpy(buf + EIP_HEADER_SIZE + CPF_UCONN_HEADER_SIZE, cip, cip_len);

    if(!send_all(sock, buf, len) || !read_reply(sock, EIP_SEND_RR_DATA, &status) || status != 0) { return false; }

    *cip_status = last_payload[CPF_UCONN_HEADER_SIZE + 2];

    return true;
}


/* RegisterSession carries version 1 and zero flags; the other commands carry nothing. */
static size_t make_request(uint8_t *buf, uint16_t command) {
    uint16_t payload_len = (command == EIP_REGISTER_SESSION) ? 4 : 0;

    memset(buf, 0, (size_t)(EIP_HEADER_SIZE + payload_len));
    buf[0] = (uint8_t)(command & 0xFF);
    buf[1] = (uint8_t)(command >> 8);
    buf[2] = (uint8_t)payload_len;
    if(payload_len) { buf[EIP_HEADER_SIZE] = 1; }

    return (size_t)(EIP_HEADER_SIZE + payload_len);
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


/* reads one whole reply and checks it answers command */
static bool read_reply(sock_t sock, uint16_t command, uint32_t *status) {
    uint8_t buf[600];
    uint16_t reply_command = 0;
    uint16_t payload_len = 0;

    if(!read_exact(sock, buf, EIP_HEADER_SIZE)) { return false; }

    reply_command = (uint16_t)(buf[0] | (buf[1] << 8));
    payload_len = (uint16_t)(buf[2] | (buf[3] << 8));
    last_session = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    *status = (uint32_t)buf[8] | ((uint32_t)buf[9] << 8) | ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);

    if(payload_len > sizeof(buf) - EIP_HEADER_SIZE || !read_exact(sock, buf + EIP_HEADER_SIZE, payload_len)) { return false; }

    memcpy(last_payload, buf + EIP_HEADER_SIZE, payload_len);

    if(reply_command != command) {
        fprintf(stderr, "reply command 0x%x, expected 0x%x\n", (unsigned)reply_command, (unsigned)command);
        return false;
    }

    return true;
}

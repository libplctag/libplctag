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
 * The EIP encapsulation header is the only thing that says an incoming packet answers the
 * request we sent.  The command tells the handlers which CPF layout to expect and the session
 * handle says the packet belongs to this conversation at all, so a PLC that changes either one
 * moves every field the response handlers read.
 *
 * These tests stand up a hostile server on the loopback interface that completes the session
 * registration honestly and then answers the Forward Open with one field altered.  The library
 * has to reject the reply and drop the connection rather than parse it.
 *
 * The oracle is the server side, not the return code: after writing the bad reply the server
 * waits on the socket.  A library that rejected the packet closes the connection, so the read
 * returns end-of-file.  A library that accepted it would carry on and send the next request,
 * and one that neither noticed nor gave up would leave us waiting until the read times out.
 * Those three outcomes are distinguishable here and a bare "tag creation failed" is not --
 * creation also fails when the library simply hangs.
 *
 * POSIX sockets are used directly rather than the platform shim because the shim is internal
 * to the library.  The unit test CMakeLists.txt leaves this test out on Windows for that
 * reason.
 */

#include "mini_mock.h"

#include <arpa/inet.h>
#include <errno.h>
#include <libplctag/lib/libplctag.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define EIP_HEADER_SIZE (24)
#define EIP_REGISTER_SESSION (0x0065)
#define EIP_SEND_RR_DATA (0x006F)
#define EIP_SEND_UNIT_DATA (0x0070)

/* how the client behaved after we sent it the bad reply. */
typedef enum {
    CLIENT_RESULT_NONE = 0,     /* the exchange never got that far. */
    CLIENT_RESULT_DISCONNECTED, /* rejected the reply and hung up -- what we want. */
    CLIENT_RESULT_KEPT_GOING,   /* accepted the reply and sent another request. */
    CLIENT_RESULT_STALLED,      /* neither -- the library is sitting on a packet it did not check. */
} client_result_t;

/* which field of the Forward Open reply to corrupt. */
typedef enum {
    CORRUPT_NOTHING = 0, /* the control: a reply the library should accept. */
    CORRUPT_COMMAND,
    CORRUPT_SESSION_HANDLE,
} corruption_t;

typedef struct {
    int listen_fd;
    int port;
    corruption_t corruption;
    client_result_t result;
} server_state_t;


static void put_u16(uint8_t *buf, uint16_t val) {
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
}


static uint16_t get_u16(const uint8_t *buf) { return (uint16_t)((uint16_t)buf[0] | (uint16_t)((uint16_t)buf[1] << 8)); }


static void put_u32(uint8_t *buf, uint32_t val) {
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
}


/* read exactly count bytes, or fail.  Returns the number read, which is short only on EOF. */
static int read_exactly(int fd, uint8_t *buf, int count) {
    int total = 0;

    while(total < count) {
        ssize_t rc = read(fd, buf + total, (size_t)(count - total));

        if(rc <= 0) { return total; }

        total += (int)rc;
    }

    return total;
}


/* read one whole encapsulated packet into buf.  Returns the total size, or zero at EOF. */
static int read_packet(int fd, uint8_t *buf, int capacity) {
    int payload_size = 0;

    if(read_exactly(fd, buf, EIP_HEADER_SIZE) != EIP_HEADER_SIZE) { return 0; }

    payload_size = (int)get_u16(buf + 2);

    if(payload_size < 0 || payload_size > capacity - EIP_HEADER_SIZE) { return 0; }

    if(read_exactly(fd, buf + EIP_HEADER_SIZE, payload_size) != payload_size) { return 0; }

    return EIP_HEADER_SIZE + payload_size;
}


/*
 * The hostile server.
 *
 * Registration is answered honestly -- the library will not get as far as the Forward Open
 * otherwise, and the Forward Open reply is the one we want to corrupt.
 */
static void *server_thread(void *arg) {
    server_state_t *state = (server_state_t *)arg;
    uint8_t request[2048];
    uint8_t reply[128];
    uint32_t session_handle = 0x12345678;
    int conn_fd = -1;
    int request_size = 0;
    struct timeval timeout;

    conn_fd = accept(state->listen_fd, NULL, NULL);
    if(conn_fd < 0) { return NULL; }

    /* the library must not be able to stall this thread forever. */
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    /* first exchange: register the session, honestly. */
    request_size = read_packet(conn_fd, request, (int)sizeof(request));
    if(request_size == 0 || get_u16(request) != EIP_REGISTER_SESSION) {
        close(conn_fd);
        return NULL;
    }

    memset(reply, 0, sizeof(reply));
    put_u16(reply + 0, EIP_REGISTER_SESSION);
    put_u16(reply + 2, 4);               /* payload is the version and options words. */
    put_u32(reply + 4, session_handle);  /* session handle. */
    put_u32(reply + 8, 0);               /* status: success. */
    memcpy(reply + 12, request + 12, 8); /* echo the sender context. */
    put_u32(reply + 20, 0);              /* options. */
    put_u16(reply + 24, 1);              /* protocol version. */
    put_u16(reply + 26, 0);              /* options flags. */

    if(write(conn_fd, reply, EIP_HEADER_SIZE + 4) != (ssize_t)(EIP_HEADER_SIZE + 4)) {
        close(conn_fd);
        return NULL;
    }

    /* second exchange: the Forward Open.  This is the one we corrupt. */
    request_size = read_packet(conn_fd, request, (int)sizeof(request));
    if(request_size == 0) {
        close(conn_fd);
        return NULL;
    }

    /*
     * Build a fully successful Forward Open reply, so that the only thing wrong with it is the
     * one field we are corrupting.  This is what makes the test discriminating: on a reply the
     * library accepts it goes on to open the connection and send a read, so "the client hung
     * up" can only mean the corrupted field is what stopped it.  A reply that also carried a
     * CIP error would end the exchange either way and prove nothing.
     */
    memset(reply, 0, sizeof(reply));
    put_u16(reply + 0, (state->corruption == CORRUPT_COMMAND) ? EIP_SEND_UNIT_DATA : EIP_SEND_RR_DATA);
    put_u16(reply + 2, 46); /* payload size: the 70 byte reply less the 24 byte header. */
    put_u32(reply + 4, (state->corruption == CORRUPT_SESSION_HANDLE) ? session_handle + 1 : session_handle);
    put_u32(reply + 8, 0);               /* status: success at the encapsulation layer. */
    memcpy(reply + 12, request + 12, 8); /* echo the sender context. */
    put_u32(reply + 20, 0);              /* options. */

    put_u32(reply + 24, 0); /* interface handle. */
    put_u16(reply + 28, 0); /* router timeout. */
    put_u16(reply + 30, 2); /* CPF item count. */
    put_u16(reply + 32, 0); /* null address item. */
    put_u16(reply + 34, 0); /* ...of zero length. */
    put_u16(reply + 36, 0x00B2);
    put_u16(reply + 38, 30); /* unconnected data item: everything from the reply service on. */

    reply[40] = (uint8_t)(request[40] | 0x80); /* echo the request's service code as a reply. */
    reply[41] = 0;
    reply[42] = 0; /* general status: success. */
    reply[43] = 0; /* no extended status. */

    put_u32(reply + 44, 0x0A0A0A0A);     /* originator to target connection ID. */
    memcpy(reply + 48, request + 32, 4); /* target to originator ID: echo what the client asked for. */
    put_u16(reply + 52, 1);              /* connection serial number. */
    put_u16(reply + 54, 0x00F6);         /* originator vendor ID. */
    put_u32(reply + 56, 1);              /* originator serial number. */
    put_u32(reply + 60, 1000000);        /* originator to target actual packet interval. */
    put_u32(reply + 64, 1000000);        /* target to originator actual packet interval. */
    reply[68] = 0;                       /* application reply data size. */
    reply[69] = 0;                       /* reserved. */

    if(write(conn_fd, reply, 70) != 70) {
        close(conn_fd);
        return NULL;
    }

    /* now see what the client does with a reply it should have refused. */
    {
        ssize_t rc = read(conn_fd, request, sizeof(request));

        if(rc == 0) {
            state->result = CLIENT_RESULT_DISCONNECTED;
        } else if(rc > 0) {
            state->result = CLIENT_RESULT_KEPT_GOING;
        } else {
            state->result = (errno == EAGAIN || errno == EWOULDBLOCK) ? CLIENT_RESULT_STALLED : CLIENT_RESULT_DISCONNECTED;
        }
    }

    close(conn_fd);

    return NULL;
}


/* bring up a listener on an arbitrary free loopback port. */
static int start_server(server_state_t *state, corruption_t corruption, pthread_t *thread) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int enable = 1;

    memset(state, 0, sizeof(*state));
    state->corruption = corruption;
    state->result = CLIENT_RESULT_NONE;

    state->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(state->listen_fd < 0) { return -1; }

    setsockopt(state->listen_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* let the kernel pick, so concurrent runs do not collide. */

    if(bind(state->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(state->listen_fd, 1) < 0
       || getsockname(state->listen_fd, (struct sockaddr *)&addr, &addr_len) < 0) {
        close(state->listen_fd);
        return -1;
    }

    state->port = (int)ntohs(addr.sin_port);

    if(pthread_create(thread, NULL, server_thread, state) != 0) {
        close(state->listen_fd);
        return -1;
    }

    return 0;
}


/* point a tag at the hostile server and let the handshake run to its conclusion. */
static client_result_t run_against_hostile_server(corruption_t corruption) {
    server_state_t state;
    pthread_t thread;
    char attribs[512];
    int32_t tag_id = 0;

    if(start_server(&state, corruption, &thread) != 0) { return CLIENT_RESULT_NONE; }

    // NOLINTNEXTLINE
    snprintf(attribs, sizeof(attribs),
             "protocol=ab-eip&gateway=127.0.0.1:%d&path=1,0&plc=ControlLogix&elem_count=1&name=DummyTag", state.port);

    /* the create is expected to fail; we care about how the server saw the client behave. */
    tag_id = plc_tag_create(attribs, 3000);

    if(tag_id > 0) { plc_tag_destroy(tag_id); }

    pthread_join(thread, NULL);
    close(state.listen_fd);

    return state.result;
}


/*
 * The control.  An uncorrupted reply must be accepted, otherwise the two tests below would pass
 * for any reason at all -- including the library rejecting everything this server ever sends.
 */
static void test_valid_response_accepted(void **state) {
    (void)state;

    assert_int_equal(run_against_hostile_server(CORRUPT_NOTHING), CLIENT_RESULT_KEPT_GOING);
}


/* A reply that answers SendRRData with SendUnitData must not be parsed as a Forward Open. */
static void test_wrong_encap_command_rejected(void **state) {
    (void)state;

    assert_int_equal(run_against_hostile_server(CORRUPT_COMMAND), CLIENT_RESULT_DISCONNECTED);
}


/* A reply carrying somebody else's session handle is not ours to act on. */
static void test_wrong_session_handle_rejected(void **state) {
    (void)state;

    assert_int_equal(run_against_hostile_server(CORRUPT_SESSION_HANDLE), CLIENT_RESULT_DISCONNECTED);
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_valid_response_accepted),
        cmocka_unit_test(test_wrong_encap_command_rejected),
        cmocka_unit_test(test_wrong_session_handle_rejected),
    };

    int rc = cmocka_run_group_tests(tests, NULL, NULL);

    plc_tag_shutdown();

    return rc;
}

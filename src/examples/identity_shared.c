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
 * Reads @identity in a process that already has a tag open to the same PLC (issue 690).
 *
 *   identity_shared --tag=<tag string> --identity=<@identity tag string>
 *                   --expect-sessions=N --expect-connected=0|1
 *
 * --tag is created and read first, then --identity.  The test counts the sessions the library
 * creates (one means the @identity tag shared the first tag's) and checks the @identity tag's
 * use_connected_msg.
 */

#include <libplctag/lib/libplctag.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TIMEOUT_MS (5000)

static void logger(int32_t tag_id, int debug_level, const char *message);
static const char *arg_value(const char *arg, const char *prefix);

/* plc_tag_create() makes sessions on the calling thread, so only main() touches this. */
static int32_t sessions_created = 0;


static void logger(int32_t tag_id, int debug_level, const char *message) {
    const char *at = strstr(message, " INFO session_create_unsafe:");

    (void)tag_id;
    (void)debug_level;

    /* session_create_unsafe() logs "Starting" at INFO, which release builds keep.  Match the emitting
     * function: other lines name it too ("called from session_create_unsafe:N"). */
    if(at) {
        at += strlen(" INFO session_create_unsafe:");
        while(*at >= '0' && *at <= '9') { at++; }
        if(strncmp(at, " Starting", 9) == 0) { sessions_created++; }
    }

    fputs(message, stderr);
}


static const char *arg_value(const char *arg, const char *prefix) {
    size_t len = strlen(prefix);

    return (strncmp(arg, prefix, len) == 0) ? arg + len : NULL;
}


int main(int argc, char *argv[]) {
    const char *tag_str = NULL;
    const char *identity_str = NULL;
    int32_t expect_sessions = -1;
    int32_t expect_connected = -1;
    int32_t tag = PLCTAG_ERR_NOT_FOUND;
    int32_t identity = PLCTAG_ERR_NOT_FOUND;
    int32_t rc = PLCTAG_STATUS_OK;
    int32_t connected = -1;
    int32_t size = 0;
    bool ok = true;

    for(int32_t i = 1; i < argc; i++) {
        const char *v = NULL;

        if((v = arg_value(argv[i], "--tag="))) {
            tag_str = v;
        } else if((v = arg_value(argv[i], "--identity="))) {
            identity_str = v;
        } else if((v = arg_value(argv[i], "--expect-sessions="))) {
            expect_sessions = (int32_t)atoi(v);
        } else if((v = arg_value(argv[i], "--expect-connected="))) {
            expect_connected = (int32_t)atoi(v);
        }
    }

    if(!tag_str || !identity_str || expect_sessions < 0 || expect_connected < 0) {
        fprintf(stderr, "USAGE: %s --tag=<tag string> --identity=<@identity tag string> --expect-sessions=N --expect-connected=0|1\n",
                argv[0]);
        return 1;
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_INFO);
    plc_tag_register_logger(logger);

    tag = plc_tag_create(tag_str, TIMEOUT_MS);
    rc = (tag < 0) ? tag : plc_tag_read(tag, TIMEOUT_MS);
    printf("first tag: create %" PRId32 ", read %s.\n", tag, plc_tag_decode_error(rc));
    if(rc != PLCTAG_STATUS_OK) { ok = false; }

    if(ok) {
        identity = plc_tag_create(identity_str, TIMEOUT_MS);
        rc = (identity < 0) ? identity : plc_tag_read(identity, TIMEOUT_MS);
        printf("@identity: create %" PRId32 ", read %s.\n", identity, plc_tag_decode_error(rc));
        if(rc != PLCTAG_STATUS_OK) { ok = false; }
    }

    if(ok) {
        /* vendor, device type, product code, revision, status and serial: 14 bytes before the name. */
        size = (int32_t)plc_tag_get_size(identity);
        if(size < 14) {
            printf("FAIL: @identity returned %" PRId32 " bytes, fewer than 14.\n", size);
            ok = false;
        }

        connected = (int32_t)plc_tag_get_int_attribute(identity, "use_connected_msg", -1);
        if(connected != expect_connected) {
            printf("FAIL: @identity use_connected_msg is %" PRId32 ", expected %" PRId32 ".\n", connected, expect_connected);
            ok = false;
        }

        if(sessions_created != expect_sessions) {
            printf("FAIL: %" PRId32 " sessions created, expected %" PRId32 ".\n", sessions_created, expect_sessions);
            ok = false;
        }
    }

    if(identity >= 0) { plc_tag_destroy(identity); }
    if(tag >= 0) { plc_tag_destroy(tag); }

    plc_tag_unregister_logger();

    printf("%s\n", ok ? "PASS" : "FAIL");

    return ok ? 0 : 1;
}

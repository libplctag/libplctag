/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever    *
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

#include "compat_utils.h"
#include <libplctag/lib/libplctag.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 6, 16
#define DEFAULT_TIMEOUT_MS 5000
#define POLL_SLEEP_MS 10

static const char *src_tag_attribs = NULL;
static const char *clone_tag_attribs = NULL;
static const char *device_tag_attribs = NULL;
static int timeout_ms = DEFAULT_TIMEOUT_MS;

static void usage(const char *prog) {
    fprintf(
        stderr,
        "Usage: %s --src-tag=<full attribs> --clone-attrib=<partial attribs> --device-tag=<@device attribs> [--timeout=ms] [--debug=N]\n",
        prog);
    fprintf(stderr, "Example (AB):\n");
    fprintf(stderr,
            "  %s --src-tag=\"protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=TestDINT\" \\\n"
            "     --clone-attrib=\"name=OtherDINT&elem_count=1\" \\\n"
            "     --device-tag=\"protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=@device\"\n",
            prog);
}

static int wait_for_tag_ready(int32_t tag, int timeout) {
    int64_t end_time = compat_time_ms() + timeout;
    int rc = PLCTAG_STATUS_PENDING;

    do {
        rc = plc_tag_status(tag);
        if(rc == PLCTAG_STATUS_OK) { return rc; }

        if(rc != PLCTAG_STATUS_PENDING) { return rc; }

        compat_sleep_ms(POLL_SLEEP_MS, NULL);
    } while(compat_time_ms() < end_time);

    return PLCTAG_ERR_TIMEOUT;
}

static int test_invalid_source_id(void) {
    int32_t rc = plc_tag_create_from_tag(2147483647, "name=Whatever", NULL, NULL, 0);

    fprintf(stderr, "test_invalid_source_id: rc=%s (%d)\n", plc_tag_decode_error((int)rc), (int)rc);

    if(rc != PLCTAG_ERR_NOT_FOUND) {
        fprintf(stderr, "ERROR: expected PLCTAG_ERR_NOT_FOUND for invalid source ID.\n");
        return PLCTAG_ERR_BAD_STATUS;
    }

    return PLCTAG_STATUS_OK;
}

static int test_device_source_not_allowed(void) {
    int32_t device_tag = 0;
    int32_t clone_tag = 0;

    device_tag = plc_tag_create(device_tag_attribs, timeout_ms);
    if(device_tag < 0) {
        fprintf(stderr, "ERROR: could not create device source tag: %s\n", plc_tag_decode_error((int)device_tag));
        return (int)device_tag;
    }

    clone_tag = plc_tag_create_from_tag(device_tag, "name=IgnoredForDevice", NULL, NULL, timeout_ms);
    fprintf(stderr, "test_device_source_not_allowed: rc=%s (%d)\n", plc_tag_decode_error((int)clone_tag), (int)clone_tag);

    plc_tag_destroy(device_tag);

    if(clone_tag > 0) {
        plc_tag_destroy(clone_tag);
        fprintf(stderr, "ERROR: expected PLCTAG_ERR_NOT_ALLOWED from @device source tag.\n");
        return PLCTAG_ERR_BAD_STATUS;
    }

    if(clone_tag != PLCTAG_ERR_NOT_ALLOWED) {
        fprintf(stderr, "ERROR: expected PLCTAG_ERR_NOT_ALLOWED from @device source tag.\n");
        return PLCTAG_ERR_BAD_STATUS;
    }

    return PLCTAG_STATUS_OK;
}

static int test_successful_clone_path(void) {
    int32_t src_tag = 0;
    int32_t clone_tag = 0;
    int rc = PLCTAG_STATUS_OK;

    src_tag = plc_tag_create(src_tag_attribs, timeout_ms);
    if(src_tag < 0) {
        fprintf(stderr, "ERROR: could not create source tag: %s\n", plc_tag_decode_error((int)src_tag));
        return (int)src_tag;
    }

    clone_tag = plc_tag_create_from_tag(src_tag, clone_tag_attribs, NULL, NULL, timeout_ms);
    if(clone_tag < 0) {
        fprintf(stderr, "ERROR: could not create clone tag from source: %s\n", plc_tag_decode_error((int)clone_tag));
        plc_tag_destroy(src_tag);
        return (int)clone_tag;
    }

    rc = wait_for_tag_ready(clone_tag, timeout_ms);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "ERROR: clone tag did not become ready: %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(clone_tag);
        plc_tag_destroy(src_tag);
        return rc;
    }

    plc_tag_destroy(src_tag);

    rc = plc_tag_read(clone_tag, timeout_ms);
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        fprintf(stderr, "ERROR: clone tag read failed after source destroy: %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(clone_tag);
        return rc;
    }

    if(rc == PLCTAG_STATUS_PENDING) {
        rc = wait_for_tag_ready(clone_tag, timeout_ms);
        if(rc != PLCTAG_STATUS_OK) {
            fprintf(stderr, "ERROR: clone tag did not recover from pending read: %s\n", plc_tag_decode_error(rc));
            plc_tag_destroy(clone_tag);
            return rc;
        }
    }

    plc_tag_destroy(clone_tag);

    return PLCTAG_STATUS_OK;
}

static int parse_args(int argc, char **argv) {
    int i = 0;

    for(i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--src-tag=", 10) == 0) {
            src_tag_attribs = &argv[i][10];
        } else if(strncmp(argv[i], "--clone-attrib=", 15) == 0) {
            clone_tag_attribs = &argv[i][15];
        } else if(strncmp(argv[i], "--device-tag=", 13) == 0) {
            device_tag_attribs = &argv[i][13];
        } else if(strncmp(argv[i], "--timeout=", 10) == 0) {
            timeout_ms = atoi(&argv[i][10]);
        } else if(strncmp(argv[i], "--debug=", 8) == 0) {
            plc_tag_set_debug_level(atoi(&argv[i][8]));
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return PLCTAG_ERR_BAD_PARAM;
        }
    }

    if(!src_tag_attribs || !clone_tag_attribs || !device_tag_attribs) { return PLCTAG_ERR_BAD_PARAM; }

    if(timeout_ms <= 0) { timeout_ms = DEFAULT_TIMEOUT_MS; }

    return PLCTAG_STATUS_OK;
}

int main(int argc, char **argv) {
    int rc = PLCTAG_STATUS_OK;

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required library version %d.%d.%d not available.\n", REQUIRED_VERSION);
        return 1;
    }

    rc = parse_args(argc, argv);
    if(rc != PLCTAG_STATUS_OK) {
        usage(argv[0]);
        return 1;
    }

    rc = test_invalid_source_id();
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "RESULT: FAIL (invalid source ID case)\n");
        return 1;
    }

    rc = test_device_source_not_allowed();
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "RESULT: FAIL (@device source case)\n");
        return 1;
    }

    rc = test_successful_clone_path();
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "RESULT: FAIL (successful clone case)\n");
        return 1;
    }

    fprintf(stderr, "RESULT: PASS\n");
    return 0;
}

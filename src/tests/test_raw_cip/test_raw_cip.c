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


#include "compat_utils.h"
#include <libplctag/api/libplctag.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 4, 0

#define DEFAULT_TAG_STRING "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&name=@raw"
#define DATA_TIMEOUT 5000

#define CIP_SRV_LIST_TAG_INSTANCES ((uint8_t)0x55)
#define CIP_SRV_READ_NAMED_TAG ((uint8_t)0x4c)
#define CIP_SRV_REPLY_FLAG ((uint8_t)0x80)

/* the library builds the EIP and CPF framing, so a raw tag carries the CIP request alone. */
#define MAX_RAW_PAYLOAD (256)

/* enough raw tags to make the connection bundle them into one packet. */
#define MAX_TAGS (8)


/*
 * CIP List Tag Instances against class 0x6b.  Only a real PLC answers this one;
 * the simulator does not implement the service.
 */
static const uint8_t list_tags_payload[] = {CIP_SRV_LIST_TAG_INSTANCES,
                                            0x03, 0x20, 0x6b, 0x25, 0x00, 0x00, 0x00, 0x04, 0x00,
                                            0x02, 0x00, 0x07, 0x00, 0x08, 0x00, 0x01, 0x00};


/*
 * CIP Read Named Tag for a single element.  The tag name goes in as an ANSI
 * extended symbolic segment, padded to an even number of bytes.
 */
static int build_read_named_tag(const char *name, uint8_t *payload, int payload_capacity) {
    int name_len = (int)strlen(name);
    int path_len = 2 + name_len + (name_len & 1); /* segment type, length, name, pad */
    int index = 0;

    if(name_len <= 0 || name_len > 255) {
        printf("ERROR: tag name of %d characters cannot be encoded!\n", name_len);
        return -1;
    }

    if(2 + path_len + 2 > payload_capacity) {
        printf("ERROR: tag name of %d characters does not fit the payload buffer!\n", name_len);
        return -1;
    }

    payload[index++] = CIP_SRV_READ_NAMED_TAG;
    payload[index++] = (uint8_t)(path_len / 2); /* path size in 16-bit words */
    payload[index++] = 0x91;                    /* ANSI extended symbolic segment */
    payload[index++] = (uint8_t)name_len;

    memcpy(&payload[index], name, (size_t)name_len);
    index += name_len;

    if(name_len & 1) { payload[index++] = 0; } /* pad to an even number of bytes */

    payload[index++] = 0x01; /* element count, little endian */
    payload[index++] = 0x00;

    return index;
}


/* check one finished raw tag: the reply is the CIP response and nothing else. */
static int check_response(int32_t tag, uint8_t expected_service, bool verbose) {
    int size = plc_tag_get_size(tag);
    uint8_t reply_service = 0;
    uint8_t general_status = 0;

    if(size <= 0) {
        printf("ERROR: Unable to get the data size!\n");
        return 1;
    }

    if(verbose) {
        for(int i = 0; i < size; i++) {
            uint8_t data = plc_tag_get_uint8(tag, i);
            printf("data[%d]=%u (%x)\n", i, (unsigned int)data, (unsigned int)data);
        }
    }

    if(size < 4) {
        printf("ERROR: Response of %d bytes is too short to be a CIP response!\n", size);
        return 1;
    }

    reply_service = plc_tag_get_uint8(tag, 0);
    general_status = plc_tag_get_uint8(tag, 2);

    if(reply_service != (uint8_t)(expected_service | CIP_SRV_REPLY_FLAG)) {
        printf("ERROR: Reply service is %02x, expected %02x!\n", (unsigned int)reply_service,
               (unsigned int)(expected_service | CIP_SRV_REPLY_FLAG));
        return 1;
    }

    /*
     * 0x06 is "partial transfer", which List Tag Instances returns whenever there
     * are more instances than fit in one reply.  That is a success for this test.
     */
    if(general_status != 0x00 && general_status != 0x06) {
        printf("ERROR: CIP general status is %02x!\n", (unsigned int)general_status);
        return 1;
    }

    printf("Reply service %02x, general status %02x, %d bytes of CIP response.\n", (unsigned int)reply_service,
           (unsigned int)general_status, size);

    return 0;
}


static void usage(void) {
    printf("Usage: test_raw_cip [--tag=<attribute string>] [--read=<tag name>] [--count=<N>]\n");
    printf("  --tag=   the @raw tag to create.  Defaults to a ControlLogix on the test bench.\n");
    printf("           Add &use_connected_msg=0 to exercise the unconnected path.\n");
    printf("  --read=  send a CIP Read Named Tag for <tag name> instead of List Tag Instances.\n");
    printf("           List Tag Instances needs a real PLC; a named read works against the simulator.\n");
    printf("  --count= run N raw tags at once on one connection so their requests are bundled\n");
    printf("           into a single Multiple Service Packet.  Defaults to 1.\n");
}


int main(int argc, char **argv) {
    int32_t tags[MAX_TAGS];
    int tag_count = 1;
    int result = 0;
    int rc = PLCTAG_STATUS_OK;
    int version_major = plc_tag_get_int_attribute(0, "version_major", 0);
    int version_minor = plc_tag_get_int_attribute(0, "version_minor", 0);
    int version_patch = plc_tag_get_int_attribute(0, "version_patch", 0);
    const char *tag_string = DEFAULT_TAG_STRING;
    const char *read_name = NULL;
    uint8_t raw_payload[MAX_RAW_PAYLOAD];
    int raw_payload_size = 0;
    uint8_t expected_service = 0;

    for(int i = 0; i < MAX_TAGS; i++) { tags[i] = 0; }

    for(int i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--tag=", 6) == 0) {
            tag_string = argv[i] + 6;
        } else if(strncmp(argv[i], "--read=", 7) == 0) {
            read_name = argv[i] + 7;
        } else if(strncmp(argv[i], "--count=", 8) == 0) {
            tag_count = atoi(argv[i] + 8);
            if(tag_count < 1 || tag_count > MAX_TAGS) {
                printf("ERROR: --count must be between 1 and %d!\n", MAX_TAGS);
                return 1;
            }
        } else {
            printf("ERROR: unknown argument \"%s\"!\n", argv[i]);
            usage();
            return 1;
        }
    }

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available, found %d.%d.%d!\n", REQUIRED_VERSION, version_major,
               version_minor, version_patch);
        return 1;
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    printf("Starting with library version %d.%d.%d.\n", version_major, version_minor, version_patch);
    printf("Tag string: %s\n", tag_string);
    printf("Running %d raw tag(s) on one connection.\n", tag_count);

    /* build the CIP request the raw tag will carry. */
    if(read_name) {
        raw_payload_size = build_read_named_tag(read_name, raw_payload, (int)(unsigned int)sizeof(raw_payload));
        if(raw_payload_size < 0) { return 1; }
        expected_service = CIP_SRV_READ_NAMED_TAG;
        printf("Sending CIP Read Named Tag for \"%s\", %d bytes.\n", read_name, raw_payload_size);
    } else {
        raw_payload_size = (int)(unsigned int)sizeof(list_tags_payload);
        memcpy(raw_payload, list_tags_payload, (size_t)raw_payload_size);
        expected_service = CIP_SRV_LIST_TAG_INSTANCES;
        printf("Sending CIP List Tag Instances, %d bytes.\n", raw_payload_size);
    }

    /*
     * Run every tag on one connection.  With more than one in flight the connection
     * bundles their CIP messages into a single Multiple Service Packet, which is a
     * different assembly path from a lone request.
     */
    for(int i = 0; i < tag_count; i++) {
        tags[i] = plc_tag_create(tag_string, DATA_TIMEOUT);
        if(tags[i] < 0) {
            printf("ERROR %s: Could not create tag %d!\n", plc_tag_decode_error(tags[i]), i);
            result = 1;
            goto done;
        }

        /*
         * Set the tag buffer size so that we can write the request.
         * Note that this returns the old size (if any) or a negative error.
         */
        rc = plc_tag_set_size(tags[i], raw_payload_size);
        if(rc < 0) {
            printf("Unable to set the payload size on tag %d, %s!\n", i, plc_tag_decode_error(rc));
            result = 1;
            goto done;
        }

        /* set up the raw data */
        for(int j = 0; j < raw_payload_size && rc == PLCTAG_STATUS_OK; j++) { rc = plc_tag_set_uint8(tags[i], j, raw_payload[j]); }

        if(rc != PLCTAG_STATUS_OK) {
            printf("Unable to set the payload data in tag %d, %s!\n", i, plc_tag_decode_error(rc));
            result = 1;
            goto done;
        }
    }

    /* start every write before waiting on any of them, so they queue together. */
    for(int i = 0; i < tag_count; i++) {
        rc = plc_tag_write(tags[i], 0);
        if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
            printf("ERROR: Unable to start the raw request on tag %d, %s!\n", i, plc_tag_decode_error(rc));
            result = 1;
            goto done;
        }
    }

    for(int i = 0; i < tag_count; i++) {
        int64_t deadline = compat_time_ms() + DATA_TIMEOUT;

        do {
            rc = plc_tag_status(tags[i]);
            if(rc == PLCTAG_STATUS_PENDING) { compat_sleep_ms(10, NULL); }
        } while(rc == PLCTAG_STATUS_PENDING && compat_time_ms() < deadline);

        if(rc != PLCTAG_STATUS_OK) {
            printf("ERROR: Raw request on tag %d failed, %s!\n", i, plc_tag_decode_error(rc));
            result = 1;
            goto done;
        }
    }

    for(int i = 0; i < tag_count; i++) {
        printf("--- tag %d ---\n", i);
        if(check_response(tags[i], expected_service, tag_count == 1) != 0) {
            result = 1;
            goto done;
        }
    }

done:
    for(int i = 0; i < tag_count; i++) {
        if(tags[i] > 0) { plc_tag_destroy(tags[i]); }
    }

    if(result == 0) { printf("SUCCESS!\n"); }

    return result;
}

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


#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>


/*
 * This is a partial example of how we might do a lighter weight retrieval of tag
 * information.   Most of the tag data comes in on demand rather than all at once.
 */

#define REQUIRED_VERSION 2, 4, 0

#define TAG_STRING "protocol=ab-eip&gateway=10.206.1.40&path=1,0&plc=ControlLogix&name=@raw"
#define DATA_TIMEOUT 5000

enum {
    Get_Attributes_All = 0x01,
    Get_Attribute_List = 0x03,
    Get_Attribute_Single = 0x0E,
    Find_Next_Object_Instance = 0x11,
    Symbolic_Translation = 0x4B,
    Get_All_Tags_Attributes = 0x55,
} cip_commands;


static int set_tag_data(int32_t tag, uint8_t *data, size_t raw_data_size) {
    int rc = PLCTAG_STATUS_OK;
    int data_size = (int)(unsigned int)raw_data_size;

    rc = plc_tag_set_size(tag, data_size);
    if(rc < 0) {
        printf("\nERROR: Unable to set the payload size on the tag %s!\n", plc_tag_decode_error(rc));
        return rc;
    }

    rc = PLCTAG_STATUS_OK;

    for(int i = 0; i < data_size && rc == PLCTAG_STATUS_OK; i++) {
        // printf("*** Setting index %d to %x.\n", i, (int)(unsigned int)get_attribute_list[i]);
        rc = plc_tag_set_uint8(tag, i, data[i]);

        if(rc != PLCTAG_STATUS_OK) {
            printf("\nERROR: %s (%d) Unable to set the payload data in the tag at location %d!\n", plc_tag_decode_error(rc), rc,
                   i);
        }
    }

    return rc;
}

int print_tag_data(int32_t tag) {
    int size = plc_tag_get_size(tag);
    if(size < 0) {
        printf("\nERROR: Unable to get the payload size on the tag %s!\n", plc_tag_decode_error(size));
        return size;
    }

    /* print out the data */
    for(int i = 0; i < size; i++) {
        uint8_t data = plc_tag_get_uint8(tag, i);
        printf(" %02x", (unsigned int)data);
    }

    return PLCTAG_STATUS_OK;
}


int send_tag_data(int32_t tag, uint8_t *data, size_t data_size) {
    int rc = PLCTAG_STATUS_OK;

    do {
        /* copy data into the tag buffer */
        rc = set_tag_data(tag, data, data_size);
        if(rc != PLCTAG_STATUS_OK) {
            printf("\nERROR: Unable to set the request data! Got error code %d: %s\n", rc, plc_tag_decode_error(rc));
            break;
        }

        printf("\n\tSending data: ");

        print_tag_data(tag);

        /* get the data, Write is the only action supported. */
        rc = plc_tag_write(tag, DATA_TIMEOUT);
        if(rc != PLCTAG_STATUS_OK) {
            printf("\nERROR: Unable to send the raw request! Got error code %d: %s\n", rc, plc_tag_decode_error(rc));
            break;
        }

        printf("\n\tReceived response data: ");

        rc = print_tag_data(tag);
        if(rc != PLCTAG_STATUS_OK) {
            printf("\nERROR: Unable to print the response! Got error code %d: %s\n", rc, plc_tag_decode_error(rc));
            break;
        }
    } while(0);

    return rc;
}


int32_t get_tag_instance_counts(int32_t tag, uint16_t *num_instances, uint16_t *max_id) {
    int32_t rc = PLCTAG_STATUS_OK;
    uint8_t request[] = {
        (uint8_t)Get_Attribute_List,
        0x02,
        0x20,
        0x6B,  // Tag Manager class
        0x24,
        0x00,  // no instance, point to the class itself.
        0x02,
        0x00,  // ask for two attributes
        0x02,
        0x00,  // Attr: max instance ID
        0x03,
        0x00,  // Attr: number of instances
    };

    do {
        rc = send_tag_data(tag, request, sizeof(request));
        if(rc != PLCTAG_STATUS_OK) { break; }

        /* did we get enough data? */
        if(plc_tag_get_size(tag) >= 4) {
            uint8_t cip_status = plc_tag_get_uint8(tag, 2);

            if(cip_status != 0) {
                printf("ERROR: CIP command failed on remote PLC with error code %x!", (unsigned int)cip_status);
                rc = PLCTAG_ERR_REMOTE_ERR;
                break;
            }
        } else {
            printf("ERROR: Insufficient data returned in CIP response to get full CIP header!");
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        if(plc_tag_get_size(tag) >= 22) {
            uint16_t attrib_count = plc_tag_get_uint16(tag, 4);
            uint16_t attrib_max_instance_id = plc_tag_get_uint16(tag, 6);
            uint16_t attrib_max_instance_status = plc_tag_get_uint16(tag, 8);
            uint32_t attrib_max_instance_val = plc_tag_get_uint32(tag, 10);
            uint16_t attrib_num_instances_id = plc_tag_get_uint16(tag, 14);
            uint16_t attrib_num_instances_status = plc_tag_get_uint16(tag, 16);
            uint32_t attrib_num_instances_val = plc_tag_get_uint32(tag, 18);

            (void)attrib_count;
            (void)attrib_max_instance_id;
            (void)attrib_max_instance_status;
            (void)attrib_num_instances_id;
            (void)attrib_num_instances_status;

            /*
             * note that if we had any failure of an attribute retrieval, it
             * would result in a status of partial failure at the command
             * level with a status of 0x1e.   Then each field that failed would
             * have a status such as 0x14 (not found/does not exist).
             */

            *num_instances = (uint16_t)attrib_num_instances_val;
            *max_id = (uint16_t)attrib_max_instance_val;
        } else {
            printf("ERROR: Insufficient data returned in CIP response to get all attribute values!");
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }
    } while(0);

    return rc;
}


/**
 * @brief Get the remaining tag info for this specific tag instance.
 *
 * We have the ID and the tag name string.  We just need the rest of the
 * type information.
 *
 * @param tag
 * @param tag_instance_id
 * @param tag_name
 * @param tag_type
 * @return int32_t
 */
int32_t get_tag_full_info(int32_t tag, uint16_t tag_instance_id, const char **tag_name, uint16_t *tag_type) {
    int32_t rc = PLCTAG_STATUS_OK;
    uint8_t request[] = {
        (uint8_t)Get_Attribute_List,
        0x03,
        0x20,
        0x6B,  // Tag Manager class
        0x25,
        0x00,
        0x00,
        0x00,  // instance ID.
        0x03,
        0x00,  // ask for several attributes
        0x02,
        0x00,  // Attr: UINT - tag type information
        0x08,
        0x00,  // Attr: 3xDINT - tag array dimensions
        0x07,
        0x00,  // Attr: DINT Element size in bytes
    };

    (void)tag_name;
    (void)tag_type;


    do {
        /* plug in the instance ID */
        request[6] = (uint8_t)(tag_instance_id & 0x00FF);
        request[7] = (uint8_t)((tag_instance_id & 0xFF00) >> 8);

        rc = send_tag_data(tag, request, sizeof(request));
        if(rc != PLCTAG_STATUS_OK) { break; }

        /* did we get enough data? */
        if(plc_tag_get_size(tag) >= 4) {
            uint8_t cip_status = plc_tag_get_uint8(tag, 2);

            if(cip_status != 0) {
                printf("\nERROR: CIP command failed on remote PLC with error code %x!", (unsigned int)cip_status);
                rc = PLCTAG_ERR_REMOTE_ERR;
                break;
            }
        } else {
            printf("ERROR: Insufficient data returned in CIP response to get full CIP header!");
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        /* TODO - add decoding for the other fields */
    } while(0);

    return rc;
}


/**
 * @brief Get minimal tag information on all tags
 *
 * This just gets the ID and the tag name string, for all tags in the PLC.
 *
 * @param tag
 * @param tag_instance_id
 * @return int32_t
 */
int32_t get_all_tags(int32_t tag, uint16_t tag_instance_id) {
    int32_t rc = PLCTAG_STATUS_OK;
    uint8_t request[] = {
        (uint8_t)Get_All_Tags_Attributes,
        0x03,
        0x20,
        0x6B,  // Tag Manager class
        0x25,
        0x00,
        0x01,
        0x00,
        0x01,
        0x00,  // ask for several attributes
        0x01,
        0x00,  // Attr: INT counted string, Tag name
    };

    do {
        /* plug in the instance ID */
        request[6] = (uint8_t)(tag_instance_id & 0x00FF);
        request[7] = (uint8_t)((tag_instance_id & 0xFF00) >> 8);

        rc = send_tag_data(tag, request, sizeof(request));
        if(rc != PLCTAG_STATUS_OK) { break; }

        /* did we get enough data? */
        if(plc_tag_get_size(tag) >= 4) {
            uint8_t cip_status = plc_tag_get_uint8(tag, 2);

            if(cip_status != 0) {
                printf("\nERROR: CIP command failed on remote PLC with error code %x!", (unsigned int)cip_status);
                rc = PLCTAG_ERR_REMOTE_ERR;
                break;
            }
        } else {
            printf("ERROR: Insufficient data returned in CIP response to get full CIP header!");
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }
    } while(0);

    return rc;
}


int main(void) {
    int32_t tag = 0;
    int rc = PLCTAG_STATUS_OK;
    int version_major = plc_tag_get_int_attribute(0, "version_major", 0);
    int version_minor = plc_tag_get_int_attribute(0, "version_minor", 0);
    int version_patch = plc_tag_get_int_attribute(0, "version_patch", 0);
    uint16_t num_instances = 0;
    uint16_t max_id = 0;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available, found %d.%d.%d!\n", REQUIRED_VERSION, version_major,
               version_minor, version_patch);
        return 1;
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_NONE);

    printf("Starting with library version %d.%d.%d.\n", version_major, version_minor, version_patch);

    do {
        /* create the tag */
        tag = plc_tag_create(TAG_STRING, DATA_TIMEOUT);
        if(tag < 0) {
            printf("ERROR %s: Could not create tag!\n", plc_tag_decode_error(tag));
            break;
        }

        rc = get_tag_instance_counts(tag, &num_instances, &max_id);
        if(rc < 0) {
            printf("ERROR %s: Could not get the number of tag instances!\n", plc_tag_decode_error(rc));
            break;
        }

        printf("\nThe PLC has %" PRIu16 " tags and a maximum tag ID of %" PRIu16 ".\n", num_instances, max_id);

        rc = get_all_tags(tag, (uint16_t)1);
        if(rc < 0) {
            printf("ERROR %s: Could not get the number of tag instances!\n", plc_tag_decode_error(rc));
            break;
        }

        /* TODO - do something with this data */


    } while(0);

    if(tag > 0) { plc_tag_destroy(tag); }

    printf("\n\n********* Complete *********!\n");

    return 0;
}

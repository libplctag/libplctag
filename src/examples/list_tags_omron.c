/***************************************************************************
 *   Copyright (C) 2024 by Kyle Hayes                                      *
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


#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include "../lib/libplctag.h"
#include "utils.h"


/*
 * This is a prototype of how we might list tags on Omron.  It just gets the tag
 * names and instances so far.
 */

#define REQUIRED_VERSION 2,6,0

#define TAG_STRING_TEMPLATE "protocol=ab-eip&gateway=%s&path=1,0&plc=omron-njnx&name=@raw"
#define TAG_STRING_SIZE (512)
#define DATA_TIMEOUT 5000

enum {
    Get_Attributes_All = 0x01,
    Get_Attribute_List = 0x03,
    Get_Attribute_Single = 0x0E,
    Find_Next_Object_Instance = 0x11,
    Symbolic_Translation = 0x4B,
    Read_Tag_Fragmented = 0x52,
    Get_All_Tags_Attributes = 0x55,
    Omron_Get_All_Instances = 0x5F,
} cip_commands;

/*

Sample read command: Read Test

Read Tag
52 0a 91 12 54 65 73 74 4d 61 6e 79 42 4f 4f 4c 46 69 65 6c 64 73 01 00 00 00 00 00

Get_Attributes_All
01 0a 91 12 54 65 73 74 4d 61 6e 79 42 4f 4f 4c 46 69 65 6c 64 73

Get_Attribute_List
03 0a 91 12 54 65 73 74 4d 61 6e 79 42 4f 4f 4c 46 69 65 6c 64 73 01 00 01 00

Get_Attribute_Single
0e 0a 91 12 54 65 73 74 4d 61 6e 79 42 4f 4f 4c 46 69 65 6c 64 73 30 01

Find_Next_Object_Instance
11 0a 91 12 54 65 73 74 4d 61 6e 79 42 4f 4f 4c 46 69 65 6c 64 73

*/


static int set_tag_data(int32_t tag, uint8_t *data, size_t raw_data_size)
{
    int rc = PLCTAG_STATUS_OK;
    int data_size = (int)(unsigned int)raw_data_size;

    rc = plc_tag_set_size(tag, data_size);
    if(rc < 0) {
        printf( "\nERROR: Unable to set the payload size on the tag %s!\n", plc_tag_decode_error(rc));
        return rc;
    }

    rc = PLCTAG_STATUS_OK;

    for(int i=0; i < data_size && rc == PLCTAG_STATUS_OK; i++) {
        // printf("*** Setting index %d to %x.\n", i, (int)(unsigned int)get_attribute_list[i]);
        rc = plc_tag_set_uint8(tag, i, data[i]);

        if(rc != PLCTAG_STATUS_OK) {
            printf( "\nERROR: %s (%d) Unable to set the payload data in the tag at location %d!\n", plc_tag_decode_error(rc), rc, i);
        }
    }

    return rc;
}


int print_tag_data(int32_t tag)
{
    int size = plc_tag_get_size(tag);
    if(size < 0) {
        printf( "\nERROR: Unable to get the payload size on the tag %s!\n", plc_tag_decode_error(size));
        return size;
    }

    /* print out the data */
    for(int i=0; i < size; i++) {
        uint8_t data = plc_tag_get_uint8(tag, i);
        printf(" %02x", (unsigned int)data);
    }

    return PLCTAG_STATUS_OK;
}



int send_tag_data(int32_t tag, uint8_t *data, size_t data_size)
{
    int rc = PLCTAG_STATUS_OK;

    do {
        /* copy data into the tag buffer */
        rc = set_tag_data(tag, data, data_size);
        if(rc != PLCTAG_STATUS_OK)  {
            printf("\nERROR: Unable to set the request data! Got error code %d: %s\n", rc, plc_tag_decode_error(rc));
            break;
        }

        printf("\n\tSending data: ");

        print_tag_data(tag);

        /* get the data, Write is the only action supported. */
        rc = plc_tag_write(tag, DATA_TIMEOUT);
        if(rc != PLCTAG_STATUS_OK) {
            printf("\nERROR: Unable to send the raw request! Got error code %d: %s\n",rc, plc_tag_decode_error(rc));
            break;
        }

        printf("\n\tReceived response data: ");

        rc = print_tag_data(tag);
        if(rc != PLCTAG_STATUS_OK) {
            printf("\n\nERROR: Unable to print the response! Got error code %d: %s\n",rc, plc_tag_decode_error(rc));
            break;
        }
    } while(0);

    return rc;
}


int32_t get_tag_instance_counts(int32_t tag, uint16_t *num_instances, uint16_t *max_id)
{
    int32_t rc = PLCTAG_STATUS_OK;
    uint8_t get_inst_count_and_max_id[] = {
                                            (uint8_t)Get_Attributes_All,
                                            0x03,                       /* 3 words in path */
                                            0x20, 0x6a,                 /* Class 6A */
                                            0x25, 0x00, 0x00, 0x00      /* instance = 0, so hitting the class */
                                          };


    do {
            rc = send_tag_data(tag, get_inst_count_and_max_id, sizeof(get_inst_count_and_max_id));
            if(rc != PLCTAG_STATUS_OK) break;

            /* did we get enough data? */
            if(plc_tag_get_size(tag) >= 4) {
                uint8_t cip_status = plc_tag_get_uint8(tag, 2);

                if(cip_status != 0) {
                    printf("\nERROR: CIP command failed on remote PLC with error code %x!", (unsigned int)cip_status);
                    rc = PLCTAG_ERR_REMOTE_ERR;
                    break;
                }
            } else {
                printf("\nERROR: Insufficient data returned in CIP response to get full CIP header!");
                rc = PLCTAG_ERR_TOO_SMALL;
                break;
            }

            if(plc_tag_get_size(tag) >= 6) {
                /* these might be reversed */
                *num_instances = plc_tag_get_uint16(tag, 6);
                *max_id = plc_tag_get_uint16(tag, 8);

                printf("\nINFO: the number of instances is %"PRIu16" and the max instance ID is %"PRIu16".\n", *num_instances, *max_id);
            } else {
                printf("\nERROR: Insufficient data returned in CIP response to get all attribute values!");
                rc = PLCTAG_ERR_TOO_SMALL;
                break;
            }
    } while(0);

    return rc;
}


/**
 * @brief Get the remaining tag info for this specific tag instance.
 *
 * We have the ID and we want the tag name.
 *
 * @param tag
 * @param tag_instance_id
 * @param tag_name - out param
 *
 * @return int32_t
 */
int32_t get_tag_info(int32_t tag, uint16_t tag_instance_id, char *tag_name, int tag_name_buf_size)
{
    int32_t rc = PLCTAG_STATUS_OK;
    uint8_t request[] = {
                         (uint8_t)Get_Attributes_All,
                         0x03,                       /* 3 words in path */
                         0x20, 0x6a,                 /* Class 6A */
                         0x25, 0x00, 0x00, 0x00      /* replace instance*/
                        };
    int i = 0;
    uint32_t cursor = 0;

    do {
        /* plug in the instance ID */
        request[6] = (uint8_t)(tag_instance_id & 0x00FF);
        request[7] = (uint8_t)((tag_instance_id & 0xFF00) >> 8);

        rc = send_tag_data(tag, request, sizeof(request));
        if(rc != PLCTAG_STATUS_OK) break;

        /* did we get enough data? */
        if(plc_tag_get_size(tag) < 20) {
            printf("\nERROR:: Insufficient data returned in CIP response!");
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        /* check the command status */
        uint8_t cip_status = plc_tag_get_uint8(tag, 2);

        /* IDs are not necessarily continguous, so report anything missing but don't stop. */
        if(cip_status != 0) {
            printf("\nERROR: CIP command failed on remote PLC with error code %x!", (unsigned int)cip_status);
            return PLCTAG_ERR_NOT_FOUND;
        }

        /*
            Parse out the attribute data.

            Attribute data seem to be:
                0000 81 00 00 00- CIP header
                0004 02 00 00 00 - size of UDT or instance for an array
                0008 a3 - Array
                0009 c1 - element type is BOOL
                0010 01 - one dimension
                0011 00 - padding to align to 4 bytes
                0012 09 00 00 00 - 9 BOOLs in the array
                0016 00 00 00 00 00 00 00 00
                0024 (20 + 1*4) 00 - bit number
                0025 04 02 00 - ?
                0028 00 00 00 00 - element type instance ID?
                0032 00 00 00 00 - start index of array dimension 0.
            We need a minimum of 20 bytes
        */

        uint8_t name_len = plc_tag_get_uint8(tag, 8);

        printf("\nINFO: Tag name is %u characters long.\n", (unsigned int)name_len);

        /* FIXME should check for sanity here. */
        if(plc_tag_get_size(tag) < (9 + name_len + 4)) {
            printf("\nERROR: Insufficient space in response!  Expected %d bytes, but got %d bytes", (int)(unsigned int)(9 + name_len + 4), plc_tag_get_size(tag));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        for(i=0; i < (int)name_len && i < tag_name_buf_size; i++) {
            tag_name[i] = (char)plc_tag_get_uint8(tag, i + 9);
        }

        /* zero out the rest of the string buffer. */
        for( ; i < tag_name_buf_size; i++) {
            tag_name[i] = 0;
        }
    } while(0);

    return rc;
}



int get_tag_attributes(int32_t tag, const char *tag_name)
{
    int rc = PLCTAG_STATUS_OK;
    uint8_t request[130] = {0};
    int req_index = 0;
    uint8_t *path_word_count =  NULL;
    uint8_t *string_byte_len = NULL;
    uint8_t cip_header_size = 4;
    int cursor = 0;

    do {
        /* build the request */
        req_index = 0;

        /* service */
        request[req_index] = Get_Attributes_All;
        req_index++;

        /* path length in words, will need to backfill */
        request[req_index] = 0;
        path_word_count = &(request[req_index]);
        req_index++;

        /* this will be an extended symbolic segment */
        request[req_index] = 0x91;
        req_index++;

        /* string length in bytes. */
        string_byte_len = &request[req_index];
        *string_byte_len = strlen(tag_name);
        req_index++;

        /* copy the string */
        strcpy((char *)(&request[req_index]), tag_name);
        req_index += *string_byte_len;

        if(*string_byte_len & 0x01) {
            /* need pad */
            request[req_index] = 0;
            req_index++;
        }

        /* fix up the word count */
        *path_word_count = (req_index-2)/2;

        rc = send_tag_data(tag, request, (size_t)(unsigned int)req_index);
        if(rc != PLCTAG_STATUS_OK) {
            printf("\nERROR:: Error getting \"%s\" attribute data!\n", tag_name);
            break;
        }

        cursor = cip_header_size;
        uint32_t tag_type_len = plc_tag_get_uint32(tag, cursor);
        cursor += 4;

        uint8_t tag_data_type = plc_tag_get_uint8(tag, cursor);
        cursor += 1;

        /* only valid for arrays */
        uint8_t tag_data_element_type = plc_tag_get_uint8(tag, cursor);
        cursor += 1;

        uint8_t num_array_dimensions = plc_tag_get_uint8(tag, cursor);
        cursor += 1;

        /* skip a padding byte */
        cursor += 1;

        uint32_t dimension_element_counts[10];

        for(uint8_t dim=0; dim < num_array_dimensions && dim < 10; dim++) {
            dimension_element_counts[dim] = plc_tag_get_uint32(tag, cursor);
            cursor += 4;
        }

        uint32_t tag_element_type_id = plc_tag_get_uint32(tag, cursor);
        cursor += 4;


        printf("\tTag type %02x\n", (unsigned int)tag_data_type);

        if(tag_data_type == 0xa0 || tag_data_type == 0xa2) {
            /* struct/UDT */

            printf("\tTag size %"PRIu32"\n", tag_type_len);
            printf("\tTag data type instance ID: %04"PRIx32"\n", tag_element_type_id);
        } else if(tag_data_type == 0xa1 || tag_data_type == 0xa3) {
            /* array */

            /* skip unknown field */
            cursor += 4;

            uint8_t bit_number = plc_tag_get_uint8(tag, cursor);
            cursor += 1;

            /* skip 3 unknown bytes */
            uint32_t alternate_element_type_id = plc_tag_get_uint32(tag, cursor);
            cursor += 4;

            /* get the start index for each array dimension */
            uint32_t array_start_indexes[10];
            for(uint8_t dim=0; dim < num_array_dimensions && dim < 10; dim++) {
                array_start_indexes[dim] = plc_tag_get_uint32(tag, cursor);
                cursor += 4;
            }

            printf("\t\tTag array element type 0x%02x\n", (unsigned int)tag_data_element_type);
            printf("\t\tTag array element type ID %04"PRIx32" or maybe %04"PRIx32"\n", tag_element_type_id, alternate_element_type_id);
            printf("\t\tTag array dimensions: [");
            for(uint8_t i=0; i < num_array_dimensions && i < 10; i++) {
                if(i != 0) printf(", ");
                printf("%"PRIu32, dimension_element_counts[i]);
            }
            printf("]\n");
            printf("\t\tTag array start indexes: [");
            for(uint8_t i=0; i < num_array_dimensions && i < 10; i++) {
                if(i != 0) printf(", ");
                printf("%"PRIu32, array_start_indexes[i]);
            }
            printf("]\n");
        }
    } while(0);

    return rc;
}


struct tag_entry_t {
        uint32_t instance_id;
        char tag_name[48];
        bool used;
};

typedef struct tag_entry_t tag_entry_t;
typedef tag_entry_t *tag_entry_p;

int32_t process_instance_data(int32_t tag, tag_entry_p *tags, uint16_t num_instances, uint16_t current_tag_entry_index)
{
    int32_t rc = PLCTAG_STATUS_OK;
    uint32_t cursor = 0;
    uint32_t check_cursor = 0;
    int32_t processed_instance_count = 0;

    do {
        /* process the data */
        cursor = 4; /* skip past the CIP header */
        uint32_t batch_size = plc_tag_get_uint32(tag, cursor);
        cursor += 4;

        /* if there are no entries, then we are done */
        if(batch_size == 0) {
            printf("\nINFO: Got no instance entries back.  Done.\n");
            rc = batch_size;
            break;
        }

        printf("\nINFO: processing %"PRIu32" instances starting at instance index %"PRIu16".\n", batch_size, current_tag_entry_index);

        for(int instance_index = current_tag_entry_index;
                instance_index < batch_size && instance_index < num_instances;
                instance_index++) {
            uint32_t instance_id = plc_tag_get_uint32(tag, cursor);
            cursor += 4;

            tags[instance_index]->instance_id = instance_id;

            uint16_t instance_data_len = plc_tag_get_uint16(tag, cursor);
            cursor += 2;

            /* calculate a check */
            check_cursor = cursor + instance_data_len;

            /* skip the class? */
            cursor += 2;

            /* skip the instance, again */
            cursor += 4;

            /* read the tag name */
            uint8_t name_length = plc_tag_get_uint8(tag, cursor);
            cursor += 1;

            uint8_t char_index = 0;
            for(char_index = 0; char_index < name_length && char_index < (sizeof((*tags)[instance_index].tag_name) - 1); char_index++) {
                (*tags)[instance_index].tag_name[char_index] = (char)plc_tag_get_uint8(tag, cursor + char_index);
            }

            /* zero out the rest of the buffer */
            for(; char_index < sizeof((*tags)[instance_index].tag_name); char_index++) {
                (*tags)[instance_index].tag_name[char_index] = 0;
            }

            /* bump the cursor past to the next one but take into account the padding */
            cursor += name_length + (name_length & 0x01 ? 1 : 0);

            if(check_cursor != cursor) {
                printf("ERROR: check for cursor position failed!  Expected %"PRIu32" but got %"PRIu32".\n", check_cursor, cursor);
                rc = PLCTAG_ERR_BAD_STATUS;
                break;
            }
        }

        rc = (int32_t)batch_size;
    } while(0);

    return rc;
}


int32_t get_instance_data_fast(int32_t tag, tag_entry_p tags, uint16_t num_instances, int16_t next_instance_id, int16_t current_tag_entry_index)
{
    int32_t rc = PLCTAG_STATUS_OK;
    int cursor = 0;
    int16_t batch_size = 0;

    uint8_t request[] = {
                         (uint8_t)Omron_Get_All_Instances,
                         0x03,                       /* 3 words in path */
                         0x20, 0x6a,                 /* Class 6A */
                         0x25, 0x00, 0x00, 0x00,     /* replace instance*/
                         0x00, 0x00, 0x00, 0x00,     /* starting instance ID */
                         0x64, 0x00, 0x00, 0x00,     /* number of instances to get */
                         0x02, 0x00                  /* 1 = system tags, 2 = user tags */
                        };

    do {
        /* patch up the next instance ID */
        request[7] = (next_instance_id & 0xFF);
        request[8] = ((next_instance_id >> 8) & 0xFF);

        rc = send_tag_data(tag, request, sizeof(request));
        if(rc != PLCTAG_STATUS_OK) break;

        /* did we get enough data? */
        if(plc_tag_get_size(tag) < 10) {
            printf("\nERROR:: Insufficient data returned in CIP response!");
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        /* a zero batch size indicates that we are done */
        batch_size = process_instance_data(tag, &tags, num_instances, current_tag_entry_index);
        rc = batch_size;
    } while(0);

    return rc;
}





void usage()
{
    printf("Usage: list_tags_omron <PLC IP> [--debug]"
           "\n\t\tExample: list_tags_omron 10.1.2.3"
           "\nAdding the optional --debug flag will turn on some debugging in the library.\n"
          );
    exit(1);
}



char *setup_tag_string(int argc, char **argv)
{
    char tag_string[TAG_STRING_SIZE+1] = {0};
    const char *gateway = NULL;
    const char *path = NULL;

    if(argc < 2) {
        usage();
    }

    if(!argv[1] || strlen(argv[1]) == 0) {
        fprintf(stderr, "Hostname or IP address must not be zero length!\n");
        usage();
    }

    if(argc == 3) {

        printf("INFO: argv[0] = \"%s\".\n", argv[0]);
        printf("INFO: argv[1] = \"%s\".\n", argv[1]);
        printf("INFO: argv[2] = \"%s\".\n", argv[2]);

        /* two args */
        if(strcasecmp("--debug", argv[1]) == 0) {
            printf("INFO: debug arg is in argv[1]=\"%s\"\n", argv[1]);
            printf("INFO: host arg is in argv[2]=\"%s\"\n", argv[2]);

            gateway = argv[2];
            plc_tag_set_debug_level(PLCTAG_DEBUG_INFO);
        } else if(strcasecmp("--debug", argv[2]) == 0) {
            printf("INFO: debug arg is in argv[2]=\"%s\"\n", argv[2]);
            printf("INFO: host arg is in argv[1]=\"%s\"\n", argv[1]);

            gateway = argv[1];
            plc_tag_set_debug_level(PLCTAG_DEBUG_INFO);
        } else {
            usage();
        }
    } else if(argc == 2) {
        gateway = argv[1];
        plc_tag_set_debug_level(PLCTAG_DEBUG_ERROR);
    } else {
        usage();
    }

    /* build the tag string. */
    snprintf(tag_string, TAG_STRING_SIZE, TAG_STRING_TEMPLATE, gateway);

    /* FIXME - check size! */
    printf("Using tag string \"%s\".\n", tag_string);

    return strdup(tag_string);
}






int main(int argc, char **argv)
{
    int32_t tag = 0;
    int rc = PLCTAG_STATUS_OK;
    char *tag_string;
    int size = 0;
    int version_major = plc_tag_get_int_attribute(0, "version_major", 0);
    int version_minor = plc_tag_get_int_attribute(0, "version_minor", 0);
    int version_patch = plc_tag_get_int_attribute(0, "version_patch", 0);
    uint16_t num_instances = 0;
    uint16_t max_id = 0;
    tag_entry_p tags;
    uint16_t current_tag_entry_index = 0;
    uint16_t next_instance_id = (uint16_t)1;

    printf("WARNING: This code is not complete and still very EXPERIMENTAL!\n");

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available, found %d.%d.%d!\n", REQUIRED_VERSION, version_major, version_minor, version_patch);
        return 1;
    }

    printf("Starting with library version %d.%d.%d.\n", version_major, version_minor, version_patch);

    // plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    tag_string = setup_tag_string(argc, argv);
    if(!tag_string) {
        printf("\nERROR:: unable to create tag string!\n");
        usage();
    }

    do {
        /* create the tag */
        tag = plc_tag_create(tag_string, DATA_TIMEOUT);
        if(tag < 0) {
            printf("\nERROR: %s: Could not create tag!\n", plc_tag_decode_error(tag));
            rc = tag;
            break;
        }

        rc = get_tag_instance_counts(tag, &num_instances, &max_id);
        if(rc < 0) {
            printf("\nERROR: %s: Could not run Get_Attribute_All on class 6A!\n", plc_tag_decode_error(rc));
            break;
        }

        /* allocate the tags array to store the instance data */
        tags = calloc(num_instances, sizeof(struct tag_entry_t));
        if(!tags) {
            printf("ERROR: unable to allocate memory for tags!");
            rc = PLCTAG_ERR_NO_MEM;
            break;
        }

        do {
            int32_t num_instances_processed = get_instance_data_fast(tag, tags, num_instances, next_instance_id, current_tag_entry_index);
            if(num_instances_processed < 0) {
                printf("\nERROR: %s: Could not run Omron get instances on class 6A!\n", plc_tag_decode_error(num_instances_processed));
            } else {
                current_tag_entry_index += num_instances_processed;
                next_instance_id = tags[current_tag_entry_index - 1].instance_id + 1;
            }

            rc = num_instances_processed;
        } while(rc > 0); /* zero entries or a negative terminate the loop */

        for(int32_t instance_index=0; instance_index < current_tag_entry_index; instance_index++) {
            printf("\n\nTag %s (%04"PRIx32"):\n", tags[instance_index].tag_name, tags[instance_index].instance_id);
            rc = get_tag_attributes(tag, tags[instance_index].tag_name);
            if(rc != PLCTAG_STATUS_OK)
            break;
        }

        // for(uint16_t id = 1; id <= max_id; id++) {
        //     char tag_name[128];

        //     /* no need to zero out the buffer as get_tag_name will zero out anything it does not overwrite */
        //     rc = get_tag_info(tag, id, tag_name, sizeof(tag_name));
        //     if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_NOT_FOUND) {
        //         printf("\nERROR: %s: Could not run Get_Attribute_All on tag instance!\n", plc_tag_decode_error(rc));
        //         break;
        //     }

        //     if(rc == PLCTAG_STATUS_OK) {
        //         printf("Instance 0x%02x: \"%s\".\n", id, tag_name);
        //     } else {
        //         printf("Instance 0x%02x seems to be unused.\n", id);
        //     }

        //     /* get the tag attributes from the name */
        //     if(rc == PLCTAG_STATUS_OK) {
        //         rc = get_tag_attributes(tag, tag_name);
        //     }
        // }
    } while(0);

    if(tag > 0) {
        plc_tag_destroy(tag);
    } else {
        usage();
    }

    printf("\n\n********* Complete *********!\n");

    return 0;
}

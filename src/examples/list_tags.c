/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
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
#include <stdint.h>
#include <string.h>
#include "../lib/libplctag.h"
#include "utils.h"

#define REQUIRED_VERSION 2,2,0

#define TAG_STRING_SIZE (200)
#define TIMEOUT_MS (5000)

#define TAG_IS_SYSTEM ((uint16_t)0x1000)
#define TAG_DIM_MASK ((uint16_t)0x6000)

struct program_entry_s {
    struct program_entry_s *next;
    char *program_name;
};

struct tag_entry_s {
    struct tag_entry_s *next;
    char *name;
    uint16_t type;
    uint16_t elem_size;
    uint16_t elem_count;
    uint16_t num_dimensions;
    uint16_t dimensions[3];
};

void usage()
{
    printf("Usage: list_tags <PLC IP> <PLC path>\nExample: list_tags 10.1.2.3 1,0\n");
    exit(1);
}

int32_t setup_tag(char *plc_ip, char *path, char *program)
{
    int32_t tag = PLCTAG_ERR_CREATE;
    char tag_string[TAG_STRING_SIZE] = {0,};

    if(!program || strlen(program) == 0) {
        snprintf(tag_string, TAG_STRING_SIZE-1,"protocol=ab-eip2&gateway=%s&path=%s&plc=ControlLogix&name=@tags", plc_ip, path);
    } else {
        snprintf(tag_string, TAG_STRING_SIZE-1,"protocol=ab-eip2&gateway=%s&path=%s&plc=ControlLogix&name=%s.@tags", plc_ip, path, program);
    }

    //printf("Using tag string: %s\n", tag_string);

    tag = plc_tag_create(tag_string, TIMEOUT_MS);
    if(tag < 0) {
        printf("Unable to open tag!  Return code %s\n", plc_tag_decode_error(tag));
        usage();
    }

    return tag;
}


void get_list(int32_t tag, char *prefix, struct tag_entry_s **tag_list, struct program_entry_s **prog_list)
{
    int rc = PLCTAG_STATUS_OK;
    int offset = 0;
    int prefix_size = 0;

    rc = plc_tag_read(tag, TIMEOUT_MS);
    if(rc != PLCTAG_STATUS_OK) {
        printf("Unable to read tag!  Return code %s\n",plc_tag_decode_error(tag));
        usage();
    }

    /* get the prefix length */
    if(prefix && strlen(prefix) > 0) {
        prefix_size = (int)(unsigned int)strlen(prefix) + 1; /* +1 for the dot */
    } else {
        prefix_size = 0;
    }

    /* process each tag entry. */
    do {
        /* uint32_t tag_instance_id = 0; */
        uint16_t tag_type = 0;
        uint16_t element_length = 0;
        uint16_t array_dims[3] = {0,};
        int tag_name_len = 0;
        char *tag_name = NULL;

        /* each entry looks like this:
            uint32_t instance_id    monotonically increasing but not contiguous
            uint16_t symbol_type    type of the symbol.
            uint16_t element_length length of one array element in bytes.
            uint32_t array_dims[3]  array dimensions.
            uint16_t string_len     string length count.
            uint8_t string_data[]   string bytes (string_len of them)
        */

        /* we do not actually need this.
        tag_instance_id = plc_tag_get_uint32(tag, offset);
         */
        offset += 4;

        tag_type = plc_tag_get_uint16(tag, offset);
        offset += 2;

        element_length = plc_tag_get_uint16(tag, offset);
        offset += 2;

        array_dims[0] = (uint16_t)plc_tag_get_uint32(tag, offset);
        offset += 4;
        array_dims[1] = (uint16_t)plc_tag_get_uint32(tag, offset);
        offset += 4;
        array_dims[2] = (uint16_t)plc_tag_get_uint32(tag, offset);
        offset += 4;

        /* use library support for strings. Offset points to the start of the string. */
        tag_name_len = plc_tag_get_string_length(tag, offset) + 1; /* add +1 for the zero byte. */

        /* allocate space for the prefix plus the tag name. */
        tag_name = malloc((size_t)(unsigned int)(tag_name_len + prefix_size));
        if(!tag_name) {
            fprintf(stderr, "Unable to allocate memory for the tag name!\n");
            usage();
        }

        /* copy the prefix string. */
        if(prefix_size > 0) {
            snprintf(tag_name, (size_t)(unsigned int)(tag_name_len + prefix_size), "%s.", prefix);
        }

        rc = plc_tag_get_string(tag, offset, tag_name + prefix_size, tag_name_len);
        if(rc != PLCTAG_STATUS_OK) {
            fprintf(stderr, "Unable to get the tag name string, got error %s!\n", plc_tag_decode_error(rc));
            usage();
        }

        /* fprintf(stderr, "Tag %s, string length: %d.\n", tag_name, (int)(unsigned int)strlen(tag_name)); */

        /* skip past the string. */
        offset += plc_tag_get_string_total_length(tag, offset);

        /* if the tag is actually a program, put it on the program list for later. */
        if(prog_list && strncmp(tag_name, "Program:", strlen("Program:")) == 0) {
            struct program_entry_s *entry = malloc(sizeof(*entry));

            if(!entry) {
                fprintf(stderr,"Unable to allocate memory for program entry!\n");
                usage();
            }

            //printf("index %d: Found program: %s\n", index, tag_name);

            entry->next = *prog_list;
            entry->program_name = tag_name;

            *prog_list = entry;
        } else if(!(tag_type & TAG_IS_SYSTEM)) {
            struct tag_entry_s *tag_entry = calloc(1, sizeof(*tag_entry));

            if(!tag_entry) {
                fprintf(stderr, "Unable to allocate memory for tag entry!\n");
                usage();
            }

            tag_entry->elem_count = 1;

            //printf("index %d: Found tag name=%s, tag instance ID=%x, tag type=%x, element length (in bytes) = %d, array dimensions = (%d, %d, %d)\n", index, tag_name, tag_instance_id, tag_type, (int)element_length, (int)array_dims[0], (int)array_dims[1], (int)array_dims[2]);

            /* fill in the fields. */
            tag_entry->name = tag_name;
            tag_entry->type = tag_type;
            tag_entry->elem_size = element_length;
            tag_entry->num_dimensions = (uint16_t)((tag_type & TAG_DIM_MASK) >> 13);
            tag_entry->dimensions[0] = array_dims[0];
            tag_entry->dimensions[1] = array_dims[1];
            tag_entry->dimensions[2] = array_dims[2];

            for(uint16_t i=0; i < tag_entry->num_dimensions; i++) {
                tag_entry->elem_count = (uint16_t)((uint16_t)tag_entry->elem_count * (uint16_t)(tag_entry->dimensions[i]));
            }

            /* link it up to the list */
            tag_entry->next = *tag_list;
            *tag_list = tag_entry;
        } else {
            //printf("index %d: Found system tag name=%s, tag instance ID=%x, tag type=%x, element length (in bytes) = %d, array dimensions = (%d, %d, %d)\n", index, tag_name, tag_instance_id, tag_type, (int)element_length, (int)array_dims[0], (int)array_dims[1], (int)array_dims[2]);
            free(tag_name);
        }
    } while(rc == PLCTAG_STATUS_OK && offset < plc_tag_get_size(tag));

    plc_tag_destroy(tag);
}




int main(int argc, char **argv)
{
    char *host = NULL;
    char *path = NULL;
    int32_t tag;
    struct program_entry_s *programs = NULL;
    struct tag_entry_s *tags = NULL;

    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    if(argc < 3) {
        usage();
    }

    if(!argv[1] || strlen(argv[1]) == 0) {
        printf("Hostname or IP address must not be zero length!\n");
        usage();
    }

    host = argv[1];

    if(!argv[2] || strlen(argv[2]) == 0) {
        printf("PLC path must not be zero length!\n");
        usage();
    }

    path = argv[2];

    /* get the controller tags first. */

    fprintf(stderr, "Getting controller tags.\n");

    tag = setup_tag(host, path, NULL);

    get_list(tag, NULL, &tags, &programs);

    while(programs) {
        struct program_entry_s *program = programs;

        fprintf(stderr, "Getting tags for program: %s.\n", program->program_name);
        tag = setup_tag(argv[1], argv[2], program->program_name);
        get_list(tag, program->program_name, &tags, NULL);

        /* go to the next one */
        programs = programs->next;

        /* now clean up */
        free(program->program_name);
        free(program);
    }

    /* loop over the tags and output their connection strings. */
    while(tags) {
        struct tag_entry_s *tag = tags;

        printf("Tag \"%s", tag->name);
        switch(tag->num_dimensions) {
            case 1:
                printf("[%d]", tag->dimensions[0]);
                break;

            case 2:
                printf("[%d,%d]", tag->dimensions[0], tag->dimensions[1]);
                break;

            case 3:
                printf("[%d,%d,%d]", tag->dimensions[0], tag->dimensions[1], tag->dimensions[2]);
                break;

            default:
                break;
        }

        printf("\" (%04x): protocol=ab-eip&gateway=%s&path=%s&plc=ControlLogix&elem_size=%u&elem_count=%u&name=%s\n", tag->type, host, path, tag->elem_size, tag->elem_count, tag->name);

        tags = tag->next;

        free(tag->name);
        free(tag);
    }

    return 0;
}

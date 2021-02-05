/***************************************************************************
 *   Copyright (C) 2021 by Kyle Hayes                                      *
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/libplctag.h"
#include "utils.h"

#define REQUIRED_VERSION 2,2,1

#define TAG_STRING_SIZE (200)
#define TAG_STRING_TEMPLATE "protocol=ab-eip2&gateway=%s&path=%s&plc=ControlLogix&name=@raw"
#define TIMEOUT_MS 5000


#define TYPE_IS_STRUCT ((uint16_t)0x8000)
#define TYPE_IS_SYSTEM ((uint16_t)0x1000)
#define TYPE_DIM_MASK ((uint16_t)0x6000)
#define TYPE_UDT_ID_MASK ((uint16_t)0x0FFF)
#define TAG_DIM_MASK ((uint16_t)0x6000)


#define MAX_UDTS (1 << 12)





struct program_entry_s {
    struct program_entry_s *next;
    char *program_name;
};

struct tag_entry_s {
    struct tag_entry_s *next;
    char *name;
    struct tag_entry_s *parent;
    uint16_t type;
    uint16_t elem_size;
    uint16_t elem_count;
    uint16_t num_dimensions;
    uint16_t dimensions[3];
};


struct udt_field_entry_s {
    char *name;
    uint16_t type;
    uint16_t metadata;
    uint32_t size;
    uint32_t offset;
};


struct udt_entry_s {
    char *name;
    uint16_t id;
    uint16_t num_fields;
    uint32_t instance_size;
    struct udt_field_entry_s fields[];
};




static void usage(void);
static int open_raw_tag(int argc, char **argv);
static int get_tag_list(int32_t tag_id, struct tag_entry_s **tag_list, struct tag_entry_s *parent);
static void print_element_type(uint16_t element_type);
static int process_tag_entry(int32_t tag, int *offset, uint16_t *last_tag_id, struct tag_entry_s **tag_list, struct tag_entry_s *parent);
static int get_udt_definition(int32_t tag, uint16_t udt_id);
static int encode_request_prefix(const char *name, uint8_t *buffer, int *encoded_size);
static int get_template_info(int32_t tag, uint16_t udt_id, uint16_t *num_members, uint32_t *udt_definition_dwords, uint32_t *udt_instance_size);
static int get_template_field_info(int32_t tag, uint16_t udt_id, uint32_t template_definition_size_dwords);


/* a local cache of all found UDT definitions. */
static struct udt_entry_s *udts[MAX_UDTS] = { NULL };


int main(int argc, char **argv)
{
    int rc = PLCTAG_STATUS_OK;
    char *host = NULL;
    char *path = NULL;
    int32_t tag = 0;
    struct tag_entry_s *tag_list = NULL;
    int version_major = plc_tag_get_int_attribute(0, "version_major", 0);
    int version_minor = plc_tag_get_int_attribute(0, "version_minor", 0);
    int version_patch = plc_tag_get_int_attribute(0, "version_patch", 0);

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available, found %d.%d.%d!\n", REQUIRED_VERSION, version_major, version_minor, version_patch);
        return 1;
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_WARN);

    printf("Starting with library version %d.%d.%d.\n", version_major, version_minor, version_patch);

    /* clear the UDTs. */
    for(int index = 0; index < MAX_UDTS; index++) {
        udts[index] = NULL;
    }

    /* set up the tag. */
    tag = open_raw_tag(argc, argv);
    if(tag <= 0) {
        printf("Unable to create raw tag, error %s!\n", plc_tag_decode_error(tag));
        usage();
    }

    /* get the list of controller tags. */
    rc = get_tag_list(tag, &tag_list, NULL);
    if(rc != PLCTAG_STATUS_OK) {
        printf("Unable to get tag list or no tags visible in the target PLC, error %s!\n", plc_tag_decode_error(rc));
        usage();
    }

    /*
     * now loop through the tags and get the list for the program tags.
     *
     * This is safe because we push the new tags on the front of the list and
     * so do not change any existing tag in the list.
     */
    for(struct tag_entry_s *entry = tag_list; entry; entry = entry->next) {
        if(strncmp(entry->name, "Program:", strlen("Program:")) == 0) {
            /* this is a program tag, check for its tags. */
            printf("Getting tags for program: \"%s\".\n", entry->name);

            rc = get_tag_list(tag, &tag_list, entry);
            if(rc != PLCTAG_STATUS_OK) {
                printf("Unable to get program tag list or no tags visible in the target PLC, error %s!\n", plc_tag_decode_error(rc));
                usage();
            }
        }
    }

    /* loop through the tags and get the UDT information. */
    for(struct tag_entry_s *entry = tag_list; entry; entry = entry->next) {
        /* check the type of the tag's element type. */
        uint16_t element_type = entry->type;

        /* if this is a structure, make sure we have the definition. */
        if((element_type & TYPE_IS_STRUCT) && !(element_type & TYPE_IS_SYSTEM)) {
            rc = get_udt_definition(tag, element_type & TYPE_UDT_ID_MASK);
            if(rc != PLCTAG_STATUS_OK) {
                printf("Unable to get UDT template ID %u, error %s!\n", (unsigned int)(element_type & TYPE_UDT_ID_MASK), plc_tag_decode_error(rc));
                usage();
            }
        }
    }

    /* output all the tags. */
    for(struct tag_entry_s *tag = tag_list; tag; tag = tag->next) {
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

        /* handle the type. */
        printf("\" ");
        print_element_type(tag->type);
        printf(".  ");

        /* print the tag string */
        printf("tag string = \"protocol=ab-eip&gateway=%s&path=%s&plc=ControlLogix&elem_size=%u&elem_count=%u&name=%s\"\n", host, path, tag->elem_size, tag->elem_count, tag->name);
    }

    printf("UDTs:\n");

    /* print out all the UDTs */
    for(int index=0; index < MAX_UDTS; index++) {
        struct udt_entry_s *udt = udts[index];

        if(udt) {
            printf(" UDT %s (%d bytes):\n", udt->name, (int)(unsigned int)udt->instance_size); /* FIXME */

            for(int field_index = 0; field_index < udt->num_fields; field_index++) {
                printf("    Field %d: %s, offset %d, ", field_index, udt->fields[field_index].name, udt->fields[field_index].offset);
                if(udt->fields[field_index].type & 0x2000) { /* MAGIC */
                    printf("array [%d] of type ", (int)(unsigned int)(udt->fields[field_index].metadata));
                } else {
                    printf(" type ");
                }
                print_element_type(udt->fields[field_index].type);
                if(udt->fields[field_index].type == 0xC1) {
                    /* bit type, the metadata is the bit number. */
                    printf(" bit %d", (int)(unsigned int)(udt->fields[field_index].metadata));
                }
                if(udt->fields[field_index].type )
                printf(".\n");
            }
        }
    }

    /* clean up memory */
    while(tag_list) {
        struct tag_entry_s *tag = tag_list;

        /* unlink */
        tag_list = tag_list->next;

        if(tag->name) {
            free(tag->name);
            tag->name = NULL;
        }

        free(tag);
    }

    for(int index=0; index < MAX_UDTS; index++) {
        struct udt_entry_s *udt = udts[index];

        if(udt) {
            if(udt->name) {
                free(udt->name);
            }

            for(int field_index=0; field_index < udt->num_fields; field_index++) {
                struct udt_field_entry_s *field = &(udt->fields[field_index]);

                if(field->name) {
                    free(field->name);
                }
            }

            free(udt);

            udts[index] = NULL;
        }
    }

    plc_tag_destroy(tag);

    printf("SUCCESS!\n");

    return 0;
}


void usage()
{
    printf("Usage: list_tags <PLC IP> <PLC path>\nExample: list_tags 10.1.2.3 1,0\n");
    exit(1);
}



int open_raw_tag(int argc, char **argv)
{
    int32_t tag = PLCTAG_ERR_CREATE;
    char tag_string[TAG_STRING_SIZE] = {0,};
    const char *gateway = NULL;
    const char *path = NULL;

    if(argc < 3) {
        usage();
    }

    if(!argv[1] || strlen(argv[1]) == 0) {
        printf("Hostname or IP address must not be zero length!\n");
        usage();
    }

    gateway = argv[1];

    if(!argv[2] || strlen(argv[2]) == 0) {
        printf("PLC path must not be zero length!\n");
        usage();
    }

    path = argv[2];

    /* build the tag string. */
    snprintf(tag_string, TAG_STRING_SIZE-1,TAG_STRING_TEMPLATE, gateway, path);

    /* FIXME - check size! */

    tag = plc_tag_create(tag_string, TIMEOUT_MS);
    if(tag < 0) {
        printf("Unable to open tag!  Return code %s\n", plc_tag_decode_error(tag));
        usage();
    }

    return tag;
}


int get_tag_list(int32_t tag, struct tag_entry_s **tag_list, struct tag_entry_s *parent)
{
    int rc = PLCTAG_STATUS_OK;
    int done = 1;
    uint8_t raw_payload[] = { 0x20,
                              0x6b,
                              0x25,
                              0x00,
                              0x00, /* tag entry ID byte 0 */
                              0x00, /* tag entry ID byte 1 */
                              0x04, /* get 4 attributes */
                              0x00,
                              0x02, /* symbol type */
                              0x00,
                              0x07, /* element length */
                              0x00,
                              0x08, /* array dimensions */
                              0x00,
                              0x01, /* symbol name string */
                              0x00 };
    int raw_payload_size = 0;
    uint16_t last_tag_entry_id = 0;
    int payload_size = 0;
    uint8_t encoded_prefix[256 + 3] = {0};
    int encoded_prefix_size = 0;

    do {
        uint8_t status_code = 0;
        int offset = 0;
        int tag_id_index = 0;

        raw_payload_size = (int)(unsigned int)sizeof(raw_payload);

        /* determine if there is a prefix on the request. */
        if(!parent) {
            encoded_prefix_size = 0;
        } else {
            if(strlen(parent->name) < 256) {
                rc = encode_request_prefix(parent->name, &encoded_prefix[0], &encoded_prefix_size);
                if(rc != PLCTAG_STATUS_OK) {
                    printf("Unable to encode program prefix %s, error %s!\n", parent->name, plc_tag_decode_error(rc));
                    usage();
                }
            } else {
                printf("Program \"%s\" name is too long!\n", parent->name);
                usage();
            }
        }

        /* set up the payload size */
        payload_size = raw_payload_size + encoded_prefix_size + 2;
        rc = plc_tag_set_int_attribute(tag, "payload_size", payload_size);
        if(rc != PLCTAG_STATUS_OK) {
            printf("Unable to set the payload size, got error %s!\n", plc_tag_decode_error(rc));
            usage();
        }

        /* set up the core request type. */
        rc = plc_tag_set_uint8(tag, offset, 0x55); /* get multiple instance attributes. */
        if(rc != PLCTAG_STATUS_OK) {
            printf("Error %s while filling in the raw CIP request!\n", plc_tag_decode_error(rc));
            usage();
        }
        offset++;

        /* set the length */
        rc = plc_tag_set_uint8(tag, offset, (uint8_t)(unsigned int)((6 + encoded_prefix_size)/2));
        if(rc != PLCTAG_STATUS_OK) {
            printf("Error %s while setting the path size in the raw CIP request!\n", plc_tag_decode_error(rc));
            usage();
        }
        offset++;

        /* copy the encoded prefix */
        for(int index=0; index < encoded_prefix_size  && rc == PLCTAG_STATUS_OK; index++) {
            rc = plc_tag_set_uint8(tag, offset + index, encoded_prefix[index]);
        }
        if(rc != PLCTAG_STATUS_OK) {
            printf("Error %s while copying the encoded prefix into the request!\n", plc_tag_decode_error(rc));
            usage();
        }
        offset += encoded_prefix_size;

        /* copy in the request. */
        for(int index=0; index < raw_payload_size && rc == PLCTAG_STATUS_OK; index++) {
            if(index == 4) {
                tag_id_index = offset + index;
            }
            rc = plc_tag_set_uint8(tag, offset+index, raw_payload[index]);
        }
        if(rc != PLCTAG_STATUS_OK) {
            printf("Error %s while filling in the raw CIP request!\n", plc_tag_decode_error(rc));
            usage();
        }
        offset += raw_payload_size;

        /* update the tag entry ID */
        rc = plc_tag_set_uint16(tag, tag_id_index, last_tag_entry_id);
        if(rc != PLCTAG_STATUS_OK) {
            printf("Error %s while back filling the last tag entry id into the raw CIP request!\n", plc_tag_decode_error(rc));
            usage();
        }

        /* go get it. */
        rc = plc_tag_read(tag, TIMEOUT_MS);
        if(rc != PLCTAG_STATUS_OK) {
            printf("Error %s trying to send CIP request!\n", plc_tag_decode_error(rc));
            usage();
        }

        /* process the raw data. */
        payload_size = plc_tag_get_int_attribute(tag, "payload_size", -1);
        if(payload_size < 0) {
            printf("Error getting payload size!\n");
            usage();
        }

        /* check the payload size. */
        if(payload_size < 4) {
            printf("Unexpectedly small payload size %d!\n", payload_size);
            usage();
        }

        /* get the CIP response header status */
        status_code = plc_tag_get_uint8(tag, 2);

        /* step past the CIP response header. */
        offset = 4;

        if(status_code == 6) {
            /* need to keep going */
            done = 0;
        } else {
            if(status_code != 0) {
                printf("CIP error in tag listing response, %u!\n", status_code);
                usage();
            }

            done = 1;
        }

        /* process each entry */
        do {
            rc = process_tag_entry(tag, &offset, &last_tag_entry_id, tag_list, parent);
        } while(rc == PLCTAG_STATUS_OK && offset < payload_size);

        /* increment last_tag_entry_id so that we do not process the same one twice. */
        last_tag_entry_id++;
    } while(!done);

    return PLCTAG_STATUS_OK;
}




int process_tag_entry(int32_t tag, int *offset, uint16_t *last_tag_id, struct tag_entry_s **tag_list, struct tag_entry_s *parent)
{
    uint16_t tag_type = 0;
    uint16_t element_length = 0;
    uint16_t array_dims[3] = {0,};
    uint16_t tag_name_len = 0;
    char *tag_name = NULL;
    struct tag_entry_s *tag_entry = NULL;

    /* each entry looks like this:
        uint32_t instance_id    monotonically increasing but not contiguous
        uint16_t symbol_type    type of the symbol.
        uint16_t element_length length of one array element in bytes.
        uint32_t array_dims[3]  array dimensions.
        uint16_t string_len     string length count.
        uint8_t string_data[]   string bytes (string_len of them)
    */

    *last_tag_id = (uint16_t)plc_tag_get_uint32(tag, *offset);
    *offset += 4;

    tag_type = plc_tag_get_uint16(tag, *offset);
    *offset += 2;

    element_length = plc_tag_get_uint16(tag, *offset);
    *offset += 2;

    array_dims[0] = (uint16_t)plc_tag_get_uint32(tag, *offset);
    *offset += 4;
    array_dims[1] = (uint16_t)plc_tag_get_uint32(tag, *offset);
    *offset += 4;
    array_dims[2] = (uint16_t)plc_tag_get_uint32(tag, *offset);
    *offset += 4;

    /* get the tag name length. */
    tag_name_len = plc_tag_get_uint16(tag, *offset);
    *offset += 2;

    /* allocate space for the the tag name.  Add one for the zero termination. */
    tag_name = calloc((size_t)(unsigned int)(tag_name_len + 1), 1);
    if(!tag_name) {
        fprintf(stderr, "Unable to allocate memory for the tag name!\n");
        return PLCTAG_ERR_NO_MEM;
    }

    /* copy the string data. */
    for(int index=0; index < (int)(unsigned int)tag_name_len; index++) {
        tag_name[index] = (char)plc_tag_get_uint8(tag, (*offset) + index);
    }

    /* terminate the string. */
    tag_name[(size_t)tag_name_len] = 0;

    /* bump the offset past the string. */
    *offset += (int)(unsigned int)tag_name_len;

    fprintf(stderr, "Tag %s, string length: %d.\n", tag_name, (int)(unsigned int)strlen(tag_name));

    /* allocate the new tag entry. */
    tag_entry = calloc(1, sizeof(*tag_entry));

    if(!tag_entry) {
        fprintf(stderr, "Unable to allocate memory for tag entry!\n");
        return PLCTAG_ERR_NO_MEM;
    }

    printf("Found tag name=%s, tag instance ID=%x, tag type=%x, element length (in bytes) = %d, array dimensions = (%d, %d, %d)\n", tag_name, *last_tag_id, tag_type, (int)element_length, (int)array_dims[0], (int)array_dims[1], (int)array_dims[2]);

    /* fill in the fields. */
    tag_entry->name = tag_name;
    tag_entry->parent = parent;
    tag_entry->type = tag_type;
    tag_entry->elem_size = element_length;
    tag_entry->num_dimensions = (uint16_t)((tag_type & TAG_DIM_MASK) >> 13);
    tag_entry->dimensions[0] = array_dims[0];
    tag_entry->dimensions[1] = array_dims[1];
    tag_entry->dimensions[2] = array_dims[2];

    /* calculate the element count. */
    tag_entry->elem_count = 1;
    for(uint16_t i=0; i < tag_entry->num_dimensions; i++) {
        tag_entry->elem_count = (uint16_t)((uint16_t)tag_entry->elem_count * (uint16_t)(tag_entry->dimensions[i]));
    }

    /* link it up to the list */
    tag_entry->next = *tag_list;
    *tag_list = tag_entry;

    return PLCTAG_STATUS_OK;
}



void print_element_type(uint16_t element_type)
{
    if(element_type & TYPE_IS_SYSTEM) {
        printf("element type SYSTEM %04x", (unsigned int)(element_type));
    } else if(element_type & TYPE_IS_STRUCT) {
        printf("element type UDT (0x%04x) %s", (unsigned int)(element_type), udts[(size_t)(unsigned int)(element_type & TYPE_UDT_ID_MASK)]->name);
    } else {
        uint16_t atomic_type = element_type & 0xFF; /* MAGIC */
        const char *type = NULL;

        switch(atomic_type) {
            case 0xC1: type = "BOOL: Boolean value"; break;
            case 0xC2: type = "SINT: Signed 8–bit integer value"; break;
            case 0xC3: type = "INT: Signed 16–bit integer value"; break;
            case 0xC4: type = "DINT: Signed 32–bit integer value"; break;
            case 0xC5: type = "LINT: Signed 64–bit integer value"; break;
            case 0xC6: type = "USINT: Unsigned 8–bit integer value"; break;
            case 0xC7: type = "UINT: Unsigned 16–bit integer value"; break;
            case 0xC8: type = "UDINT: Unsigned 32–bit integer value"; break;
            case 0xC9: type = "ULINT: Unsigned 64–bit integer value"; break;
            case 0xCA: type = "32–bit floating point value, IEEE format"; break;
            case 0xCB: type = "64–bit floating point value, IEEE format"; break;
            case 0xCC: type = "Synchronous time value"; break;
            case 0xCD: type = "Date value"; break;
            case 0xCE: type = "Time of day value"; break;
            case 0xCF: type = "Date and time of day value"; break;
            case 0xD0: type = "Character string, 1 byte per character"; break;
            case 0xD1: type = "8-bit bit string"; break;
            case 0xD2: type = "16-bit bit string"; break;
            case 0xD3: type = "32-bit bit string"; break;
            case 0xD4: type = "64-bit bit string"; break;
            case 0xD5: type = "Wide char character string, 2 bytes per character"; break;
            case 0xD6: type = "High resolution duration value"; break;
            case 0xD7: type = "Medium resolution duration value"; break;
            case 0xD8: type = "Low resolution duration value"; break;
            case 0xD9: type = "N-byte per char character string"; break;
            case 0xDA: type = "Counted character sting with 1 byte per character and 1 byte length indicator"; break;
            case 0xDB: type = "Duration in milliseconds"; break;
            case 0xDC: type = "CIP path segment(s)"; break;
            case 0xDD: type = "Engineering units"; break;
            case 0xDE: type = "International character string (encoding?)"; break;
        }

        if(type) {
            printf("(%04x) %s", (unsigned int)element_type, type);
        } else {
            printf("UNKNOWN TYPE %04x", (unsigned int)element_type);
        }
    }
}


int encode_request_prefix(const char *name, uint8_t *buffer, int *encoded_size)
{
    int symbol_length_index = 0;
    /* start with the symbolic type identifier */
    *encoded_size = 0;
    buffer[*encoded_size] = (uint8_t)(unsigned int)0x91; (*encoded_size)++;

    /* dummy value for the encoded length */
    symbol_length_index = *encoded_size;
    buffer[*encoded_size] = (uint8_t)(unsigned int)0; (*encoded_size)++;

    /* copy the string. */
    for(int index=0; index < (int)(unsigned int)strlen(name); index++) {
        buffer[*encoded_size] = (uint8_t)name[index];
        (*encoded_size)++;
    }

    /* backfill the encoded size */
    buffer[symbol_length_index] = (uint8_t)(unsigned int)(strlen(name));

    /* make sure we have an even number of bytes. */
    if((*encoded_size) & 0x01) {
        buffer[*encoded_size] = 0;
        (*encoded_size)++;
    }

    return PLCTAG_STATUS_OK;
}


int get_udt_definition(int32_t tag, uint16_t udt_id)
{
    int rc = PLCTAG_STATUS_OK;
    uint16_t num_members = 0;
    uint32_t udt_definition_dwords = 0;
    uint32_t udt_instance_size = 0;

    /* check to see if we have this type already. */
    if(udts[udt_id]) {
        return PLCTAG_STATUS_OK;
    }

    rc = get_template_info(tag, udt_id, &num_members, &udt_definition_dwords, &udt_instance_size);
    if(rc != PLCTAG_STATUS_OK) {
        printf("Unable to get template information for UDT %04x, error %s!\n", (unsigned int)udt_id, plc_tag_decode_error(rc));
        usage();
    }

    /* allocate a UDT struct with this info. */
    udts[(size_t)udt_id] = calloc(1, sizeof(struct udt_entry_s) + (sizeof(struct udt_field_entry_s) * num_members));
    if(!udts[(size_t)udt_id]) {
        printf("Unable to allocate a new UDT definition structure!\n");
        usage();
    }

    udts[(size_t)udt_id]->id = udt_id;
    udts[(size_t)udt_id]->num_fields = num_members;
    udts[(size_t)udt_id]->instance_size = udt_instance_size;

    rc = get_template_field_info(tag, udt_id, udt_definition_dwords);
    if(rc != PLCTAG_STATUS_OK) {
        printf("Unable to get template %04x's field information, error %s!\n", (unsigned int)udt_id, plc_tag_decode_error(rc));
        usage();
    }

    return PLCTAG_STATUS_OK;
}



int get_template_info(int32_t tag, uint16_t udt_id, uint16_t *num_members, uint32_t *udt_definition_dwords, uint32_t *udt_instance_size)
{
    int rc = PLCTAG_STATUS_OK;
    int offset = 0;
    uint8_t raw_payload[] = { 0x03,  /* Get Attributes List Service */
                              0x03,  /* path is 3 words, 6 bytes. */
                              0x20,  /* class, 8-bit */
                              0x6C,  /* Template class */
                              0x25,  /* 16-bit instance ID next. */
                              0x00,  /* padding */
                              0x00, 0x00,  /* instance ID bytes */
                              0x03, 0x00,  /* get 3 attributes. */
                              0x02, 0x00,  /* Attribute 2: Template Member Count: number of fields in the template/UDT. */
                              0x04, 0x00,  /* Attribute 4: Template Object Definition Size, in 32-bit words. */
                              0x05, 0x00   /* Attribute 5: Template Structure Size, in bytes.   Size of the object. */
                            };
    int raw_payload_size = (int)(unsigned int)sizeof(raw_payload);
    uint8_t status_code = 0;
    int response_size = 0;
    uint16_t result_count = 0;

    /* Set the payload length */
    rc = plc_tag_set_int_attribute(tag, "payload_size", raw_payload_size);

    /* copy the data into the tag. */
    for(int payload_index=0; payload_index < raw_payload_size && rc == PLCTAG_STATUS_OK; payload_index++) {
        rc = plc_tag_set_uint8(tag, payload_index, raw_payload[payload_index]);
    }

    if(rc != PLCTAG_STATUS_OK) {
        printf("Unable to copy default template info request payload into the tag buffer, error %s!\n", plc_tag_decode_error(rc));
        usage();
    }

    /* back fill the UDT ID */
    rc = plc_tag_set_uint16(tag, 6, udt_id); /* MAGIC */

    /* go get it. */
    rc = plc_tag_read(tag, TIMEOUT_MS);
    if(rc != PLCTAG_STATUS_OK) {
        printf("Error %s trying to send CIP request!\n", plc_tag_decode_error(rc));
        usage();
    }

    /* process the raw data. */
    response_size = plc_tag_get_int_attribute(tag, "payload_size", -1);
    if(response_size < 0) {
        printf("Error getting payload size!\n");
        usage();
    }

    /* check the payload size. */
    if(response_size < 4) {
        printf("Unexpectedly small response size %d!\n", response_size);
        usage();
    }

    /* get the CIP response header. 4 bytes. */
    status_code = plc_tag_get_uint8(tag, 2);

    /* step past the CIP response header. */
    offset = 4;

    if(status_code == 6) {
        /* need to keep going */
        printf("Getting more than a packet of template info results is unsupported!");
        usage();
    } else {
        if(status_code != 0) {
            printf("CIP error in template info response, %u!\n", status_code);
            usage();
        }
    }

    result_count = plc_tag_get_uint16(tag, offset);
    offset += 2;

    if(result_count != 3) {
        printf("Unexpected number of attribute results, %d!", (int)(unsigned int)result_count);
        usage();
    }

    /* read all 3 attributes, not clear that they will come in order. */
    for(int attrib_num=0; attrib_num < 3; attrib_num++) {
        uint16_t attrib_id = plc_tag_get_uint16(tag, offset);
        uint16_t attrib_status = plc_tag_get_uint16(tag, offset +2);
        // uint16_t struct_handle = 0;

        offset += 4;

        if(attrib_status != 0) {
            printf("Unable to get attribute ID %x, got error %x!\n", attrib_id, attrib_status);
            usage();
        }

        switch(attrib_id) {
            // case 0x01:
            //     /* CRC of the fields, used as a struct handle. */
            //     struct_handle = plc_tag_get_uint16(tag, offset);
            //     offset += 2;
            //     break;

            case 0x02:
                /* number of members in the template */
                *num_members = plc_tag_get_uint16(tag, offset);
                offset += 2;
                break;

            /* 3? */

            case 0x04:
                /* template definitions size in DINTs */
                *udt_definition_dwords = plc_tag_get_uint32(tag, offset);
                offset += 4;
                break;

            case 0x05:
                /* template instance size in bytes */
                *udt_instance_size = plc_tag_get_uint32(tag, offset);
                printf("UDT intance size %d bytes.\n", (int)(unsigned int)(*udt_instance_size));
                offset += 4;
                break;

            default:
                printf("Unexpected attribute %x found!\n", attrib_id);
                usage();
                break;
        }
    }

    return PLCTAG_STATUS_OK;
}




int get_template_field_info(int32_t tag, uint16_t udt_id, uint32_t template_definition_size_dwords)
{
    int rc = PLCTAG_STATUS_OK;
    int offset = 0;
    uint8_t raw_payload[] = { 0x4C,  /* Read Template Service */
                              0x03,  /* path is 3 words, 6 bytes. */
                              0x20,  /* class, 8-bit */
                              0x6C,  /* Template class */
                              0x25,  /* 16-bit instance ID next. */
                              0x00,  /* padding */
                              0x00, 0x00,  /* template/UDT instance ID bytes */
                              0x00, 0x00, 0x00, 0x00, /* byte offset. */
                              0x00, 0x00   /* Total bytes to transfer. */
                            };
    int raw_payload_size = (int)(unsigned int)sizeof(raw_payload);
    // uint8_t service_type = 0;
    uint8_t status_code = 0;
    int response_size = 0;
    uint8_t *buffer = NULL;
    uint32_t buffer_capacity = (template_definition_size_dwords * 4) - 23;
    uint32_t buffer_offset = 0;
    uint16_t field_metadata = 0;
    uint16_t field_element_type = 0;
    uint32_t field_offset = 0;

    /* make a buffer for this data. */
    buffer = calloc(template_definition_size_dwords, 4); /* allocates a little too much, but that is OK */
    if(!buffer) {
        printf("Unable to allocate template field info buffer!");
        usage();
    }

    /* loop until we get all the data. */
    do {
        /* Set the payload length */
        rc = plc_tag_set_int_attribute(tag, "payload_size", raw_payload_size);

        /* copy the data into the tag. */
        for(int payload_index=0; payload_index < raw_payload_size && rc == PLCTAG_STATUS_OK; payload_index++) {
            rc = plc_tag_set_uint8(tag, payload_index, raw_payload[payload_index]);
        }

        if(rc != PLCTAG_STATUS_OK) {
            printf("Unable to copy default template info request payload into the tag buffer, error %s!\n", plc_tag_decode_error(rc));
            usage();
        }

        /* back fill the UDT ID */
        rc = plc_tag_set_uint16(tag, 6, udt_id); /* MAGIC */
        if(rc != PLCTAG_STATUS_OK) {
            printf("Error %s trying to send CIP request!\n", plc_tag_decode_error(rc));
            usage();
        }

        /* back fill the offset */
        rc = plc_tag_set_uint32(tag, 8, buffer_offset); /* MAGIC */
        if(rc != PLCTAG_STATUS_OK) {
            printf("Error %s trying to backfill offset in CIP request!\n", plc_tag_decode_error(rc));
            usage();
        }

        /* back fill the total transfer size */
        rc = plc_tag_set_uint16(tag, 12, (uint16_t)buffer_capacity); /* MAGIC */
        if(rc != PLCTAG_STATUS_OK) {
            printf("Error %s trying to backfill total transfer size request!\n", plc_tag_decode_error(rc));
            usage();
        }

        /* go get it. */
        rc = plc_tag_read(tag, TIMEOUT_MS);
        if(rc != PLCTAG_STATUS_OK) {
            printf("Error %s trying to send CIP request!\n", plc_tag_decode_error(rc));
            usage();
        }

        /* process the raw data. */
        response_size = plc_tag_get_int_attribute(tag, "payload_size", -1);
        if(response_size < 0) {
            printf("Error getting template field info response size!\n");
            usage();
        }

        /* check the payload size. */
        if(response_size < 4) {
            printf("Unexpectedly small template field response size %d!\n", response_size);
            usage();
        }

        /* get the CIP response header. 4 bytes. */
        status_code = plc_tag_get_uint8(tag, 2);

        /* step past the CIP response header. */
        offset = 4;

        /* copy the data into the buffer. */
        for(int data_index=offset; data_index < response_size; data_index++) {
            buffer[(uint32_t)(unsigned int)(data_index - offset) + buffer_offset] = plc_tag_get_uint8(tag, data_index);
        }

        buffer_offset += (uint32_t)(unsigned int)(response_size - offset);
    } while(status_code == 6);

    if(status_code != 0) {
        printf("Got unexpected error status, %d, while reading template field info!", (int)(unsigned int)status_code);
        usage();
    }

    if(buffer_offset != buffer_capacity) {
        printf("Got unexpected amount of data!  Got %u bytes but expected %u bytes!\n", buffer_offset, buffer_capacity);
        usage();
    }

    /* process the data. */
    buffer_offset = 0;

    /* first section is the field type and size info for all fields. */
    for(int field_index=0; field_index < udts[udt_id]->num_fields; field_index++) {
        field_metadata = (uint16_t)(buffer[buffer_offset]) + (uint16_t)((uint16_t)(buffer[buffer_offset+1]) << 8);
        buffer_offset += 2;

        field_element_type = (uint16_t)(buffer[buffer_offset]) + (uint16_t)((uint16_t)(buffer[buffer_offset+1]) << 8);
        buffer_offset += 2;

        field_offset = (uint32_t)(buffer[buffer_offset])
                     + (uint32_t)((uint32_t)(buffer[buffer_offset+1]) << 8)
                     + (uint32_t)((uint32_t)(buffer[buffer_offset+2]) << 16)
                     + (uint32_t)((uint32_t)(buffer[buffer_offset+3]) << 24);
        buffer_offset += 4;

        udts[udt_id]->fields[field_index].type = field_element_type;
        udts[udt_id]->fields[field_index].metadata = field_metadata;
        udts[udt_id]->fields[field_index].offset = field_offset;

        /* make sure that we have all the Template/UDT info */
        if((field_element_type & TYPE_IS_STRUCT) && !(field_element_type & TYPE_IS_SYSTEM)) {
            rc = get_udt_definition(tag, field_element_type & TYPE_UDT_ID_MASK);
        }
    }

    /*
     * then get the template/UDT name.   This is weird.
     * Scan until we see a 0x3B, semicolon, byte.   That is the end of the
     * template name.   Actually we should look for ";n" but the semicolon
     * seems to be enough for now.
     */

    int name_index_start = (int)(unsigned int)buffer_offset;

    while(buffer[buffer_offset] != 0x3B && buffer[buffer_offset] != 0) {
        buffer_offset++;
    }

    int name_index_end = (int)(unsigned int)buffer_offset;

    udts[udt_id]->name = calloc((size_t)(unsigned int)((name_index_end - name_index_start) + 1), (size_t)1);
    if(!udts[udt_id]->name) {
        printf("Unable to allocate name for Template/UDT!\n");
        usage();
    }

    /* copy the data, the name is already zero terminated by calloc */
    for(int name_index=0; name_index < (name_index_end - name_index_start); name_index++) {
        udts[udt_id]->name[name_index] = (char)buffer[name_index_start + name_index];
    }

    /* skip to the end of the string.  Looks zero-terminated. */
    while(buffer[buffer_offset] != 0) {
        buffer_offset++;
    }

    /* step past the zero. */
    buffer_offset++;

    /*
     * This is the second section of the data, the field names.   They appear
     * to be zero terminated.
     */

    /* loop over all fields and get name strings.  They are zero terminated. */
    for(int field_index=0; field_index < udts[udt_id]->num_fields && buffer_offset < buffer_capacity; field_index++) {
        int name_size = 0;

        name_index_start = (int)(unsigned int)buffer_offset;

        while(buffer[buffer_offset] != 0) {
            buffer_offset++;
        }

        name_index_end = (int)(unsigned int)buffer_offset;

        name_size = name_index_end - name_index_start;

        /* debug */
        printf("Field %d name is %d chars long.\n", field_index, name_size);

        udts[udt_id]->fields[field_index].name = calloc((size_t)(unsigned int)(name_size + 1), (size_t)1);
        if(!(udts[udt_id]->fields[field_index].name)) {
            printf("Unable to allocate field name!");
            usage();
        }

        /* copy the name. */
        for(int name_index=0; name_index < name_size; name_index++) {
            udts[udt_id]->fields[field_index].name[name_index] = (char)buffer[name_index_start + name_index];

            printf("Field %d name character %d is '%c'.\n", field_index, name_index, (char)buffer[name_index_start + name_index]);
        }

        /* zero terminated by calloc */

        /* step past the zero. */
        buffer_offset++;
    }

    /* sanity check */
    if(buffer_offset != buffer_capacity - 1) {
        printf("Did not process whole buffer.  Processed %d bytes out of %d bytes.", buffer_offset, buffer_capacity);
    }

    free(buffer);


    return PLCTAG_STATUS_OK;
}



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


#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>
#include <lib/libplctag.h>
#include "utils.h"

typedef enum {
    TYPE_BIT, TYPE_I8, TYPE_U8, TYPE_I16, TYPE_U16, TYPE_I32, TYPE_U32, TYPE_I64, TYPE_U64,
    TYPE_F32, TYPE_F64, TYPE_STRING
} element_type_t;

struct run_args {
    char *tag_string;
    int32_t tag;
    int timeout;
    int debug;
    int element_type;
    int write_val_count;
    union {
        int8_t *i8;
        uint8_t *u8;
        int16_t *i16;
        uint16_t *u16;
        int32_t *i32;
        uint32_t *u32;
        int64_t *i64;
        uint64_t *u64;
        float *f32;
        double *f64;
        char **string;
        void *dummy;
    } write_vals;
};

#define REQUIRED_VERSION 2,2,1
#define DEFAULT_TIMEOUT (5000)


static void usage(void);
static void parse_args(int argc, char **argv, struct run_args *args);
static void parse_type(char *type_str, struct run_args *args);
static void parse_write_vals(char *write_vals, struct run_args *args);
static void dump_values(struct run_args *args);
static void cleanup(struct run_args *args);
static void update_values(struct run_args *args);


int main(int argc, char **argv)
{
    int rc = PLCTAG_STATUS_OK;
    struct run_args args;

    /* zero out all the bytes of args. */
    memset(&args, 0, sizeof(args));

    /* make sure we have the required library version */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Library version %d.%d.%d requested, but linked library version is not compatible!\n", REQUIRED_VERSION);
        exit(1);
    }

    /* output the version we are using. */
    printf("Library version %d.%d.%d.\n",
                            plc_tag_get_int_attribute(0, "version_major", 0),
                            plc_tag_get_int_attribute(0, "version_minor", 0),
                            plc_tag_get_int_attribute(0, "version_patch", 0));

    /* parse the argument list. */
    parse_args(argc, argv, &args);

    /* set the debug level if it was set in the command line parameters. */
    if(args.debug > 0) {
        plc_tag_set_debug_level(args.debug);
    }

    /* set up a scope to fake local exceptions. */
    do {
        args.tag = plc_tag_create(args.tag_string, args.timeout);
        if(plc_tag_status(args.tag) != PLCTAG_STATUS_OK) {
            printf("ERROR: Error creating tag %s!\n", plc_tag_decode_error(args.tag));
            rc = args.tag;
            break;
        }

        /* start with a read. */
        rc = plc_tag_read(args.tag, args.timeout);
        if(rc != PLCTAG_STATUS_OK) {
            printf("ERROR: Error returned while trying to read tag %s!\n", plc_tag_decode_error(rc));
            break;
        }

        /* dump out the tag values. */
        dump_values(&args);

        /* is there something to write? */
        if(args.write_val_count > 0) {
            update_values(&args);

            rc = plc_tag_write(args.tag, args.timeout);
            if(rc != PLCTAG_STATUS_OK) {
                printf("ERROR: Error returned while trying to write tag %s!\n", plc_tag_decode_error(rc));
                break;
            }

            printf("New values written to tag.\n");

            /* read it back in and dump the values. */
            rc = plc_tag_read(args.tag, args.timeout);
            if(rc != PLCTAG_STATUS_OK) {
                printf("ERROR: Error returned while trying to read tag %s!\n", plc_tag_decode_error(rc));
                break;
            }

            /* dump out the tag values. */
            dump_values(&args);
        }
    } while(0);

    cleanup(&args);

    return rc;
}



void usage(void)
{
    printf( "Usage:\n "
            "tag_rw2 --type=<type> --tag=<tag string> [--write=<vals>] [--timeout=<timeout>] [--debug=<debug>] \n"
            "\n"
            "  <type>    - type is one of 'bit', 'uint8', 'sint8', 'uint16', 'sint16', \n "
            "              'uint32', 'sint32', 'real32', 'real64', or 'string'.  The type is \n"
            "              the type of the data to be read/written to the named tag.  The\n"
            "              types starting with 'u' are unsigned and with 's' are signed.\n"
            "              For floating point, use 'real32' or 'real64'.  \n"
            "\n"
            "  <tag string> - The path to the device containing the named data.  This value may need to\n"
            "              be quoted.   Use double quotes on Windows and single quotes on Unix-like systems.\n"
            "\n"
			"  <vals>    - The value(s) to write.  Must be formatted appropriately\n"
			"              for the data type.  Multiple values are comma separated. Optional.\n"
            "\n"
            "  <timeout> - Set the timeout to this number of milliseconds.  Default is 5000.  Optional.\n"
            "\n"
			"  <debug>   - Set the debug level.   Values 1-5.\n"
			"              1 - output debug info only on fatal errors.\n"
			"              2 - output debug info for warnings and errors.\n"
			"              3 - output debug info for informative messages, warnings and errors.\n"
			"              4 - output debug info for detailed status messages, informative messages, warnings and errors.\n"
			"              5 - turn on all debugging output.  Not recommended.\n"
			"              This field is optional.\n"
			"\n"
            "Example: tag_rw2 --type=uint32 '--tag=protocol=ab_eip&gateway=10.206.1.39&path=1,0&cpu=ControlLogix&elem_count=2&name=pcomm_test_dint_array' --debug=4 --write=12,34 --timeout=1000\n"
            "Note: Use double quotes \"\" for the attribute string in Windows.\n");

    exit(1);
}


void parse_args(int argc, char **argv, struct run_args *args)
{
    int i = 0;
    bool has_type = false;
    bool has_tag = false;
    bool has_debug = false;
    bool has_timeout = false;
    bool has_write_vals = false;
    char *write_vals = NULL;

    /* argv[0] is the tag_rw2 command itself. */
    for(i = 1; i < argc; i++) {

        /* DEBUG */
        printf("Processing argument %d \"%s\".\n", i, argv[i]);

        if(strncmp(argv[i],"--type=", 7) == 0) {
            /* type argument. */

            if(has_type) {
                printf("ERROR: The type argument can only appear once!\n");
                cleanup(args);
                usage();
            }

            has_type = true;

            parse_type(&(argv[i][7]), args);
        } else if(strncmp(argv[i],"--tag=", 6) == 0) {
            if(has_tag) {
                printf("ERROR: Only one tag argument may be present!\n");
                cleanup(args);
                usage();
            }

            args->tag_string = &(argv[i][6]);

            has_tag = true;
        } else if(strncmp(argv[i],"--debug=", 8) == 0) {
            if(has_debug) {
                printf("ERROR: Only one debug argument may be present!\n");
                cleanup(args);
                usage();
            }

            args->debug = atoi(&(argv[i][8]));

            has_debug = true;
        } else if(strncmp(argv[i],"--timeout=", 10) == 0) {
            if(has_timeout) {
                printf("ERROR: Only one timeout argument may be present!\n");
                cleanup(args);
                usage();
            }

            args->timeout = atoi(&(argv[i][10]));

            if(args->timeout <= 0) {
                printf("ERROR: timeout value must be greater than zero and is in milliseconds!\n");
                cleanup(args);
                usage();
            }

            has_timeout = true;
        } else if(strncmp(argv[i],"--write=", 8) == 0) {
            if(has_write_vals) {
                printf("ERROR: Only one write value(s) argument may be present!\n");
                cleanup(args);
                usage();
            }

            write_vals = &(argv[i][8]);

            has_write_vals = true;
        } else {
            printf("Warning: Unknown argument \"%s\"!\n", argv[i]);
        }
    }

    /* check the args we got. */
    if(!has_tag) {
        printf("ERROR: you must have a tag argument!\n");
        cleanup(args);
        usage();
    }

    if(!has_type) {
        printf("ERROR: you must have a type argument!\n");
        cleanup(args);
        usage();
    }

    if(!has_debug) {
        args->debug = 0;
    }

    if(!has_timeout) {
        args->timeout = DEFAULT_TIMEOUT;
    }

    /* handle any write arguments */
    if(has_write_vals) {
        parse_write_vals(write_vals, args);
    } else {
        args->write_val_count = 0;
    }

    /* double check the types.  Bits can _not_ be arrays. */
    if(args->element_type == TYPE_BIT && args->write_val_count > 1) {
        printf("ERROR: You may not treat a bit tag as an array!\n");
        cleanup(args);
        usage();
    }
}



void parse_type(char *type_str, struct run_args *args)
{
    if(strcasecmp(type_str, "bit") == 0) {
        args->element_type = TYPE_BIT;
    } else if(strcasecmp(type_str, "sint8") == 0) {
        args->element_type = TYPE_I8;
    } else if(strcasecmp(type_str, "uint8") == 0) {
        args->element_type = TYPE_U8;
    } else if(strcasecmp(type_str, "sint16") == 0) {
        args->element_type = TYPE_I16;
    } else if(strcasecmp(type_str, "uint16") == 0) {
        args->element_type = TYPE_U16;
    } else if(strcasecmp(type_str, "sint32") == 0) {
        args->element_type = TYPE_I32;
    } else if(strcasecmp(type_str, "uint32") == 0) {
        args->element_type = TYPE_U32;
    } else if(strcasecmp(type_str, "sint64") == 0) {
        args->element_type = TYPE_I64;
    } else if(strcasecmp(type_str, "uint64") == 0) {
        args->element_type = TYPE_U64;
    } else if(strcasecmp(type_str, "real32") == 0) {
        args->element_type = TYPE_F32;
    } else if(strcasecmp(type_str, "real64") == 0) {
        args->element_type = TYPE_F64;
    } else if(strcasecmp(type_str, "string") == 0) {

        /* DEBUG */
        printf("Setting type to TYPE_STRING.\n");

        args->element_type = TYPE_STRING;
    } else {
        printf("ERROR: Unknown type %s!\n", type_str);
        cleanup(args);
        usage();
    }
}


void parse_write_vals(char *write_vals, struct run_args *args)
{
    int num_vals = 0;
    int len = 0;
    int val_start = -1;
    int elem_index = 0;
    char *tmp_vals = NULL;

    /* check the value string */
    if(!write_vals || strlen(write_vals) == 0) {
        printf("ERROR: String of values to write must not be zero-length!\n");
        cleanup(args);
        usage();
    }

    tmp_vals = strdup(write_vals);
    if(!tmp_vals) {
        printf("ERROR: Unable to copy write value(s) string!\n");
        cleanup(args);
        exit(1);
    }

    /*
     * count the number of elements. At the same time, zero terminate
     * each value string.
     */
    len = (int)(unsigned int)strlen(tmp_vals);
    num_vals = 1; /* there is one to start, then a comma before all the others. */
    val_start = -1;
    for(int i = 0; i < len; i++) {
        if(tmp_vals[i] == ',') {
            num_vals++;
            tmp_vals[i] = 0; /* terminate that value string */
        } else {
            /* see if we have noted the first part of the string. */
            if(val_start == -1) {
                val_start = i;
            }
        }
    }

    /* DEBUG */
    printf("Number of write args %d.\n", num_vals);

    args->write_val_count = num_vals;

    /* now that we know the number of arguments, we can process them. */
    switch(args->element_type) {
        case TYPE_BIT:
        case TYPE_U8:
            args->write_vals.u8 = calloc((size_t)(unsigned int)args->write_val_count, sizeof(uint8_t));
            if(!args->write_vals.u8) {
                printf("ERROR: Unable to allocate value array for write values!");
                cleanup(args);
                exit(1);
            }

            val_start = -1;
            elem_index = 0;
            for(int i=0; i < len; i++) {
                if(val_start == -1 && tmp_vals[i] != 0) {
                    val_start = i;

                    if(sscanf_platform(&(tmp_vals[val_start]), "%" SCNu8 "", &(args->write_vals.u8[elem_index])) != 1) {
                        printf("ERROR: bad format for unsigned 8-bit integer for write value.\n");
                        cleanup(args);
                        usage();
                    }

                    elem_index++;
                }

                if(tmp_vals[i] == 0) {
                    val_start = -1;
                }
            }

            break;

        case TYPE_I8:
            args->write_vals.i8 = calloc((size_t)(unsigned int)args->write_val_count, sizeof(int8_t));
            if(!args->write_vals.i8) {
                printf("ERROR: Unable to allocate value array for write values!");
                cleanup(args);
                exit(1);
            }

            val_start = -1;
            elem_index = 0;
            for(int i=0; i < len; i++) {
                if(val_start == -1 && tmp_vals[i] != 0) {
                    val_start = i;

                    if(sscanf_platform(&tmp_vals[val_start], "%" SCNd8 "", &(args->write_vals.i8[elem_index])) != 1) {
                        printf("ERROR: bad format for signed 8-bit integer for write value.\n");
                        cleanup(args);
                        usage();
                    }

                    elem_index++;
                }

                if(tmp_vals[i] == 0) {
                    val_start = -1;
                }
            }

            break;

        case TYPE_U16:
            args->write_vals.u16 = calloc((size_t)(unsigned int)args->write_val_count, sizeof(uint16_t));
            if(!args->write_vals.u16) {
                printf("ERROR: Unable to allocate value array for write values!");
                cleanup(args);
                exit(1);
            }

            val_start = -1;
            elem_index = 0;
            for(int i=0; i < len; i++) {
                if(val_start == -1 && tmp_vals[i] != 0) {
                    val_start = i;

                    if(sscanf_platform(&(tmp_vals[val_start]), "%" SCNu16 "", &(args->write_vals.u16[elem_index])) != 1) {
                        printf("ERROR: bad format for unsigned 16-bit integer for write value.\n");
                        cleanup(args);
                        usage();
                    }

                    elem_index++;
                }

                if(tmp_vals[i] == 0) {
                    val_start = -1;
                }
            }

            break;

        case TYPE_I16:
            args->write_vals.i16 = calloc((size_t)(unsigned int)args->write_val_count, sizeof(int16_t));
            if(!args->write_vals.i16) {
                printf("ERROR: Unable to allocate value array for write values!");
                cleanup(args);
                exit(1);
            }

            val_start = -1;
            elem_index = 0;
            for(int i=0; i < len; i++) {
                if(val_start == -1 && tmp_vals[i] != 0) {
                    val_start = i;

                    if(sscanf_platform(&tmp_vals[val_start], "%" SCNd16 "", &(args->write_vals.i16[elem_index])) != 1) {
                        printf("ERROR: bad format for signed 16-bit integer for write value.\n");
                        cleanup(args);
                        usage();
                    }

                    elem_index++;
                }

                if(tmp_vals[i] == 0) {
                    val_start = -1;
                }
            }

            break;

        case TYPE_U32:
            args->write_vals.u32 = calloc((size_t)(unsigned int)args->write_val_count, sizeof(uint32_t));
            if(!args->write_vals.u32) {
                printf("ERROR: Unable to allocate value array for write values!");
                cleanup(args);
                exit(1);
            }

            val_start = -1;
            elem_index = 0;
            for(int i=0; i < len; i++) {
                if(val_start == -1 && tmp_vals[i] != 0) {
                    val_start = i;

                    if(sscanf_platform(&(tmp_vals[val_start]), "%" SCNu32 "", &(args->write_vals.u32[elem_index])) != 1) {
                        printf("ERROR: bad format for unsigned 32-bit integer for write value.\n");
                        cleanup(args);
                        usage();
                    }

                    elem_index++;
                }

                if(tmp_vals[i] == 0) {
                    val_start = -1;
                }
            }

            break;

        case TYPE_I32:
            args->write_vals.i32 = calloc((size_t)(unsigned int)args->write_val_count, sizeof(int32_t));
            if(!args->write_vals.i32) {
                printf("ERROR: Unable to allocate value array for write values!");
                cleanup(args);
                exit(1);
            }

            val_start = -1;
            elem_index = 0;
            for(int i=0; i < len; i++) {
                if(val_start == -1 && tmp_vals[i] != 0) {
                    val_start = i;

                    if(sscanf_platform(&tmp_vals[val_start], "%" SCNd32 "", &(args->write_vals.i32[elem_index])) != 1) {
                        printf("ERROR: bad format for signed 32-bit integer for write value.\n");
                        cleanup(args);
                        usage();
                    }

                    elem_index++;
                }

                if(tmp_vals[i] == 0) {
                    val_start = -1;
                }
            }

            break;

        case TYPE_U64:
            args->write_vals.u64 = calloc((size_t)(unsigned int)args->write_val_count, sizeof(uint64_t));
            if(!args->write_vals.u64) {
                printf("ERROR: Unable to allocate value array for write values!");
                cleanup(args);
                exit(1);
            }

            val_start = -1;
            elem_index = 0;
            for(int i=0; i < len; i++) {
                if(val_start == -1 && tmp_vals[i] != 0) {
                    val_start = i;

                    if(sscanf_platform(&(tmp_vals[val_start]), "%" SCNu64 "", &(args->write_vals.u64[elem_index])) != 1) {
                        printf("ERROR: bad format for unsigned 64-bit integer for write value.\n");
                        cleanup(args);
                        usage();
                    }

                    elem_index++;
                }

                if(tmp_vals[i] == 0) {
                    val_start = -1;
                }
            }

            break;

        case TYPE_I64:
            args->write_vals.i64 = calloc((size_t)(unsigned int)args->write_val_count, sizeof(int64_t));
            if(!args->write_vals.i64) {
                printf("ERROR: Unable to allocate value array for write values!");
                cleanup(args);
                exit(1);
            }

            val_start = -1;
            elem_index = 0;
            for(int i=0; i < len; i++) {
                if(val_start == -1 && tmp_vals[i] != 0) {
                    val_start = i;

                    if(sscanf_platform(&tmp_vals[val_start], "%" SCNd64 "", &(args->write_vals.i64[elem_index])) != 1) {
                        printf("ERROR: bad format for signed 64-bit integer for write value.\n");
                        cleanup(args);
                        usage();
                    }

                    elem_index++;
                }

                if(tmp_vals[i] == 0) {
                    val_start = -1;
                }
            }

            break;

        case TYPE_F32:
            args->write_vals.f32 = calloc((size_t)(unsigned int)args->write_val_count, sizeof(float));
            if(!args->write_vals.f32) {
                printf("ERROR: Unable to allocate value array for write values!");
                cleanup(args);
                exit(1);
            }

            val_start = -1;
            elem_index = 0;
            for(int i=0; i < len; i++) {
                if(val_start == -1 && tmp_vals[i] != 0) {
                    val_start = i;

                    if(sscanf_platform(&(tmp_vals[val_start]), "%f", &(args->write_vals.f32[elem_index])) != 1) {
                        printf("ERROR: bad format for 32-bit floating point value.\n");
                        cleanup(args);
                        usage();
                    }

                    elem_index++;
                }

                if(tmp_vals[i] == 0) {
                    val_start = -1;
                }
            }

            break;

        case TYPE_F64:
            args->write_vals.f64 = calloc((size_t)(unsigned int)args->write_val_count, sizeof(double));
            if(!args->write_vals.f64) {
                printf("ERROR: Unable to allocate value array for write values!");
                cleanup(args);
                exit(1);
            }

            val_start = -1;
            elem_index = 0;
            for(int i=0; i < len; i++) {
                if(val_start == -1 && tmp_vals[i] != 0) {
                    val_start = i;

                    if(sscanf_platform(&tmp_vals[val_start], "%lf", &(args->write_vals.f64[elem_index])) != 1) {
                        printf("ERROR: bad format for 64-bit floating point value.\n");
                        cleanup(args);
                        usage();
                    }

                    elem_index++;
                }

                if(tmp_vals[i] == 0) {
                    val_start = -1;
                }
            }

            break;

        case TYPE_STRING:
            args->write_vals.string = calloc((size_t)(unsigned int)args->write_val_count, sizeof(char *));
            if(!args->write_vals.string) {
                printf("ERROR: Unable to allocate value array for write values!");
                cleanup(args);
                exit(1);
            }

            /* go through the strings. */
            val_start = -1;
            elem_index = 0;
            for(int i=0; i < len; i++) {
                if(val_start == -1 && tmp_vals[i] != 0) {
                    val_start = i;

                    if((args->write_vals.string[elem_index] = strdup(&tmp_vals[val_start])) == NULL) {
                        printf("ERROR: Unable to allocate string copy for write argument %d!\n", elem_index);
                        cleanup(args);
                        usage();
                    }

                    elem_index++;
                }

                if(tmp_vals[i] == 0) {
                    val_start = -1;
                }
            }

            break;

        default:
            printf("ERROR: Unknown data type (%d)!\n", args->element_type);
            cleanup(args);
            usage();
            break;
    }

    if(tmp_vals) {
        free(tmp_vals);
    }
}



void dump_values(struct run_args *args)
{
    int item_index = 0;
    int offset = 0;
    int32_t tag = args->tag;

    /* display the data */
    if(args->element_type == TYPE_BIT) {
        int rc = plc_tag_get_bit(tag, 0);
        int bit_num = plc_tag_get_int_attribute(tag, "bit_num", -1);

        if(rc < 0) {
            printf("Error received trying to read bit tag: %s!\n", plc_tag_decode_error(rc));
            cleanup(args);
            exit(1);
        } else {
<<<<<<< HEAD
            printf("bit %d = %d\n", bit_num, rc);
=======
                    break;

                case TYPE_U32:
                    printf("data[%d]=%" PRIu32 " (%" PRIx32 ")\n",item_index, plc_tag_get_uint32(tag, offset), plc_tag_get_uint32(tag, offset));
                    offset += 4;
                    break;

                case TYPE_U64:
                    printf("data[%d]=%" PRIu64 " (%" PRIx64 ")\n", item_index, plc_tag_get_uint64(tag, offset),plc_tag_get_uint64(tag, offset));
                    offset += 8;
                    break;

                case TYPE_I8:
                    printf("data[%d]=%" PRId8 " (%" PRIx8 ")\n", item_index, plc_tag_get_int8(tag, offset),plc_tag_get_int8(tag, offset));
                    offset += 1;
                    break;

                case TYPE_I16:
                    printf("data[%d]=%" PRId16 " (%" PRIx16 ")\n", item_index, plc_tag_get_int16(tag, offset),plc_tag_get_int16(tag, offset));
                    offset += 2;
                    break;

                case TYPE_I32:
                    printf("data[%d]=%" PRId32 " (%" PRIx32 ")\n", item_index, plc_tag_get_int32(tag, offset),plc_tag_get_int32(tag, offset));
                    offset += 4;
                    break;

                case TYPE_I64:
                    printf("data[%d]=%" PRId64 " (%" PRIx64 ")\n", item_index, plc_tag_get_int64(tag, offset),plc_tag_get_int64(tag, offset));
                    offset += 8;
                    break;

                case TYPE_F32:
                    printf("data[%d]=%f\n", item_index, plc_tag_get_float32(tag, offset));
                    offset += 4;
                    break;

                case TYPE_F64:
                    printf("data[%d]=%lf\n", item_index, plc_tag_get_float64(tag, offset));
                    offset += 8;
                    break;

                case TYPE_STRING:
                    {
                        int str_len = plc_tag_get_string_length(tag, offset);
                        char *str = NULL;
                        int rc = PLCTAG_STATUS_OK;

                        if(str_len > 0) {
                            str = calloc((size_t)(unsigned int)(str_len+1), sizeof(char));
                            if(!str) {
                                printf("ERROR: Unable to allocate temporary buffer to output string!\n");
                                cleanup(args);
                                exit(1);
                            }

                            rc = plc_tag_get_string(tag, offset, str, str_len);
                            if(rc != PLCTAG_STATUS_OK) {
                                printf("ERROR: Unable to get string %d, error: %s!\n", item_index, plc_tag_decode_error(rc));
                                cleanup(args);
                                exit(1);
                            }

                            printf("data[%d]=\"%s\"\n", item_index, str);

                            free(str);
                        } else if(str_len == 0) {
                            printf("data[%d]=\"\"\n", item_index);
                        } else {
                            printf("Error getting string length for item %d!  Got error value %s!", item_index, plc_tag_decode_error(str_len));
                        }
                    }

                    offset += plc_tag_get_string_total_length(tag, offset);

                    break;

                default:
                    printf("ERROR: Unsupported tag type %d!\n", args->element_type);
                    cleanup(args);
                    exit(1);
                    break;
            }

            item_index++;
        }
    }
}


void cleanup(struct run_args *args)
{
    plc_tag_destroy(args->tag);
    args->tag = 0;

    if(args->write_val_count > 0) {
        if(args->element_type == TYPE_STRING && args->write_vals.string) {
            for(int i=0; i < args->write_val_count; i++) {
                free(args->write_vals.string[i]);
                args->write_vals.string[i] = NULL;
            }

            free(args->write_vals.string);
            args->write_vals.string = NULL;
        } else {
            if(args->write_vals.dummy) {
                free(args->write_vals.dummy);
                args->write_vals.dummy = NULL;
            }
        }

        args->write_val_count = 0;
    }
}



void update_values(struct run_args *args)
{
    int item_index = 0;
    int offset = 0;
    int rc = PLCTAG_STATUS_OK;
    int32_t tag = args->tag;

    /* display the data */
    if(args->element_type == TYPE_BIT) {
        rc = plc_tag_set_bit(tag, offset, (int)(unsigned int)args->write_vals.u8[0]);

        if(rc < 0) {
            printf("Error received trying to write bit tag: %s!\n", plc_tag_decode_error(rc));
            cleanup(args);
            exit(1);
        }
    } else {
        int len = plc_tag_get_size(tag);

        item_index = 0;
        offset = 0;
        while(offset < len && item_index < args->write_val_count) {
            switch(args->element_type) {
                case TYPE_U8:
                    rc = plc_tag_set_uint8(tag, offset, args->write_vals.u8[item_index]);
                    if(rc != PLCTAG_STATUS_OK) {
                        printf("Error returned while trying to write unsigned 8-bit value of entry %d!\n", item_index);
                        cleanup(args);
                        exit(1);
                    }
                    offset += 1;
                    break;

                case TYPE_U16:
                    rc = plc_tag_set_uint16(tag, offset, args->write_vals.u16[item_index]);
                    if(rc != PLCTAG_STATUS_OK) {
                        printf("Error returned while trying to write unsigned 16-bit value of entry %d!\n", item_index);
                        cleanup(args);
                        exit(1);
                    }
                    offset += 2;
                    break;

                case TYPE_U32:
                    rc = plc_tag_set_uint32(tag, offset, args->write_vals.u32[item_index]);
                    if(rc != PLCTAG_STATUS_OK) {
                        printf("Error returned while trying to write unsigned 32-bit value of entry %d!\n", item_index);
                        cleanup(args);
                        exit(1);
                    }
                    offset += 4;
                    break;

                case TYPE_U64:
                    rc = plc_tag_set_uint64(tag, offset, args->write_vals.u64[item_index]);
                    if(rc != PLCTAG_STATUS_OK) {
                        printf("Error returned while trying to write unsigned 64-bit value of entry %d!\n", item_index);
                        cleanup(args);
                        exit(1);
                    }
                    offset += 8;
                    break;

                case TYPE_I8:
                    rc = plc_tag_set_int8(tag, offset, args->write_vals.i8[item_index]);
                    if(rc != PLCTAG_STATUS_OK) {
                        printf("Error returned while trying to write signed 8-bit value of entry %d!\n", item_index);
                        cleanup(args);
                        exit(1);
                    }
                    offset += 1;
                    break;

                case TYPE_I16:
                    rc = plc_tag_set_int16(tag, offset, args->write_vals.i16[item_index]);
                    if(rc != PLCTAG_STATUS_OK) {
                        printf("Error returned while trying to write signed 16-bit value of entry %d!\n", item_index);
                        cleanup(args);
                        exit(1);
                    }
                    offset += 2;
                    break;

                case TYPE_I32:
                    rc = plc_tag_set_int32(tag, offset, args->write_vals.i32[item_index]);
                    if(rc != PLCTAG_STATUS_OK) {
                        printf("Error returned while trying to write signed 32-bit value of entry %d!\n", item_index);
                        cleanup(args);
                        exit(1);
                    }
                    offset += 4;
                    break;

                case TYPE_I64:
                    rc = plc_tag_set_int64(tag, offset, args->write_vals.i64[item_index]);
                    if(rc != PLCTAG_STATUS_OK) {
                        printf("Error returned while trying to write signed 64-bit value of entry %d!\n", item_index);
                        cleanup(args);
                        exit(1);
                    }
                    offset += 8;
                    break;

                case TYPE_F32:
                    rc = plc_tag_set_float32(tag, offset, args->write_vals.f32[item_index]);
                    if(rc != PLCTAG_STATUS_OK) {
                        printf("Error returned while trying to write 32-bit floating point value of entry %d!\n", item_index);
                        cleanup(args);
                        exit(1);
                    }
                    offset += 4;
                    break;

                case TYPE_F64:
                    rc = plc_tag_set_float64(tag, offset, args->write_vals.f64[item_index]);
                    if(rc != PLCTAG_STATUS_OK) {
                        printf("Error returned while trying to write 64-bit floating point value of entry %d!\n", item_index);
                        cleanup(args);
                        exit(1);
                    }
                    offset += 8;
                    break;

                case TYPE_STRING:
                    {
                        int str_len = (int)(unsigned int)strlen(args->write_vals.string[item_index]);
                        int str_capacity = plc_tag_get_string_capacity(tag, offset);

                        /* clamp the length. */
                        if(str_len > str_capacity) {
                            printf("Warning: truncating string %d, \"%s\", to fit fixed string capacity!\n", item_index, args->write_vals.string[item_index]);
                            str_len = str_capacity;

                            /* zero terminate it at the new shorter length */
                            args->write_vals.string[item_index][str_len] = 0;
                        }

                        /* set the string. */
                        rc = plc_tag_set_string(tag, offset, args->write_vals.string[item_index]);
                        if(rc != PLCTAG_STATUS_OK) {
                            printf("Error while setting the string %d, error: %s!\n", item_index, plc_tag_decode_error(rc));
                            cleanup(args);
                            exit(1);
                        }
                    }

                    offset += plc_tag_get_string_total_length(tag, offset);

                    break;

                default:
                    printf("ERROR: Unsupported tag type %d!\n", args->element_type);
                    cleanup(args);
                    exit(1);
                    break;
            }

            item_index++;
        }
    }
}


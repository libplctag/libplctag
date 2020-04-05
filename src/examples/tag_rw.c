/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
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


/* need this for strdup */
#define POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>
#include <lib/libplctag.h>
#include <lib/version.h>
#include "utils.h"


#define REQUIRED_VERSION 2,1,0


#define PLC_LIB_BIT     (0x101)
#define PLC_LIB_UINT8   (0x108)
#define PLC_LIB_SINT8   (0x208)
#define PLC_LIB_UINT16  (0x110)
#define PLC_LIB_SINT16  (0x210)
#define PLC_LIB_UINT32  (0x120)
#define PLC_LIB_SINT32  (0x220)
#define PLC_LIB_UINT64  (0x140)
#define PLC_LIB_SINT64  (0x240)
#define PLC_LIB_REAL32  (0x320)
#define PLC_LIB_REAL64  (0x340)

#define DATA_TIMEOUT 5000


void usage(void)
{
    printf( "Usage:\n "
            "tag_rw -t <type> -p <path> [-w <val>] [-d <lvl>] \n"
            "  -t <type> - type is one of 'uint8', 'sint8', 'uint16', 'sint16', \n "
            "              'uint32', 'sint32', or 'real32'.  The type is the type\n"
            "              of the data to be read/written to the named tag.  The\n"
            "              types starting with 'u' are unsigned and with 's' are signed.\n"
            "              For floating point, use 'real32'.  \n"
            "  -p <path> - The path to the device containing the named data.\n"
			"  -w <val>  - The value to write.  Must be formatted appropriately\n"
			"              for the data type.  Optional.\n"
			"  -d <lvl>  - Set the debug level.   Values 1-5.\n"
			"              1 - output debug info only on fatal errors.\n"
			"              2 - output debug info for warnings and errors.\n"
			"              3 - output debug info for informative messages, warnings and errors.\n"
			"              4 - output debug info for detailed status messages, informative messages, warnings and errors.\n"
			"              5 - turn on all debugging output.  Not recommended.\n"
			"              This field is optional.\n"
			"\n"
            "Example: tag_rw -t uint32 -p 'protocol=ab_eip&gateway=10.206.1.27&path=1,0&cpu=LGX&elem_size=4&elem_count=200&name=pcomm_test_dint_array'\n"
            "Note: Use double quotes \"\" for the attribute string in Windows.\n");
}




static int data_type = 0;
static char *write_str = NULL;
static char *path = NULL;
static int debug_level = PLCTAG_DEBUG_NONE;


void check_version(void)
{
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Library version %d.%d.%d requested, but linked library version is not compatible!\n", REQUIRED_VERSION);
        exit(1);
    }
}


void print_lib_version(void)
{
    int32_t tag = 0;
    int i, size = 0;
    char ver[16] = {0,};

    tag = plc_tag_create("make=system&family=library&name=version", DATA_TIMEOUT);
    if(tag < 0) {
        fprintf(stderr,"ERROR %s: Could not create tag!\n", plc_tag_decode_error(tag));
        return;
    }

    size = plc_tag_get_size(tag);

    plc_tag_read(tag, 0);

    for(i=0; i < size && i < (int)sizeof(ver); i++) {
        ver[i] = (char)plc_tag_get_uint8(tag,i);
    }

    printf("Library version %s.\n", ver);

    plc_tag_destroy(tag);
}


void parse_args(int argc, char **argv)
{
    int i;

    for (i = 0; i < argc; i++) {
        printf("Arg[%d]=%s\n", i, argv[i]);
    }

    i = 1;

    while(i < argc) {
        if(!strcmp(argv[i],"-t")) {
            i++; /* get the arg next */
            if(i < argc) {
                if(!strcasecmp("bit",argv[i])) {
                    data_type = PLC_LIB_BIT;
                } else if(!strcasecmp("uint8",argv[i])) {
                    data_type = PLC_LIB_UINT8;
                } else if(!strcasecmp("sint8",argv[i])) {
                    data_type = PLC_LIB_SINT8;
                } else if(!strcasecmp("uint16",argv[i])) {
                    data_type = PLC_LIB_UINT16;
                } else if(!strcasecmp("sint16",argv[i])) {
                    data_type = PLC_LIB_SINT16;
                } else if(!strcasecmp("uint32",argv[i])) {
                    data_type = PLC_LIB_UINT32;
                } else if(!strcasecmp("sint32",argv[i])) {
                    data_type = PLC_LIB_SINT32;
                } else if(!strcasecmp("uint64",argv[i])) {
                    data_type = PLC_LIB_UINT64;
                } else if(!strcasecmp("sint64",argv[i])) {
                    data_type = PLC_LIB_SINT64;
                } else if(!strcasecmp("real32",argv[i])) {
                    data_type = PLC_LIB_REAL32;
                } else if(!strcasecmp("real64",argv[i])) {
                    data_type = PLC_LIB_REAL64;
                } else {
                    printf("ERROR: unknown data type: %s\n",argv[i]);
                    usage();
                    exit(1);
                }
            } else {
                printf("ERROR: you must have a value to write after -t\n");
                usage();
                exit(1);
            }
        } else if(!strcmp(argv[i],"-w")) {
            i++;
            if(i < argc) {
                write_str = strdup(argv[i]);
            } else {
                printf("ERROR: you must have a value to write after -w\n");
                usage();
                exit(1);
            }
        } else if(!strcmp(argv[i],"-p")) {
            i++;
            if(i < argc) {
                path = strdup(argv[i]);
            } else {
                printf("ERROR: you must have a tag string after -p\n");
                usage();
                exit(1);
            }
		} else if (!strcmp(argv[i], "-d")) {
			i++;
			if (i < argc) {
				debug_level = atoi(argv[i]);

				if (debug_level <= PLCTAG_DEBUG_NONE || debug_level > PLCTAG_DEBUG_SPEW) {
					printf("ERROR: Debug level must be between 1 and 5, inclusive.\n");
					usage();
					exit(1);
				}

				plc_tag_set_debug_level(debug_level);
			} else {
				printf("ERROR: you must have a debug level after -d\n");
				usage();
				exit(1);
			}
		} else {
            /* something unexpected */
            usage();
            exit(1);
        }

        i++;
    }
}


int main(int argc, char **argv)
{
    int32_t tag = 0;
    int is_write = 0;
    uint64_t u_val64;
    uint32_t u_val32;
    uint16_t u_val16;
    uint8_t u_val8;
    int64_t i_val64;
    int32_t i_val32;
    int16_t i_val16;
    int8_t i_val8;
    double f_val64;
    float f_val32;
    int i;
    int rc;

    check_version();

    print_lib_version();

    parse_args(argc, argv);

    /* check arguments */
    if(!path || !data_type) {
        usage();
        exit(0);
    }

    /* convert any write values */
    if(write_str && strlen(write_str)) {
        is_write = 1;

        switch(data_type) {
        case PLC_LIB_BIT:
            /* fall through */
        case PLC_LIB_UINT8:
            if(sscanf_platform(write_str,"%" SCNu8 "", &u_val8) != 1) {
                printf("ERROR: bad format for unsigned 8-bit integer for write value.\n");
                usage();
                exit(1);
            }

            break;

        case PLC_LIB_UINT16:
            if(sscanf_platform(write_str,"%" SCNu16 "",&u_val16) != 1) {
                printf("ERROR: bad format for unsigned 16-bit integer for write value.\n");
                usage();
                exit(1);
            }

            break;

        case PLC_LIB_UINT32:
            if(sscanf_platform(write_str,"%" SCNu32 "", &u_val32) != 1) {
                printf("ERROR: bad format for unsigned 32-bit integer for write value.\n");
                usage();
                exit(1);
            }

            break;

        case PLC_LIB_UINT64:
            if(sscanf_platform(write_str, "%" SCNu64 "", &u_val64) != 1) {
                printf("ERROR: bad format for unsigned 64-bit integer for write value.\n");
                usage();
                exit(1);
            }

            break;

        case PLC_LIB_SINT8:
            if(sscanf_platform(write_str,"%" SCNd8 "", &i_val8) != 1) {
                printf("ERROR: bad format for signed 8-bit integer for write value.\n");
                usage();
                exit(1);
            }

            break;

        case PLC_LIB_SINT16:
            if(sscanf_platform(write_str,"%" SCNd16 "", &i_val16) != 1) {
                printf("ERROR: bad format for signed 16-bit integer for write value.\n");
                usage();
                exit(1);
            }

            break;

        case PLC_LIB_SINT32:
            if(sscanf_platform(write_str,"%" SCNd32 "", &i_val32) != 1) {
                printf("ERROR: bad format for signed 32-bit integer for write value.\n");
                usage();
                exit(1);
            }

            break;

        case PLC_LIB_SINT64:
            if(sscanf_platform(write_str, "%" SCNd64 "", &i_val64) != 1) {
                printf("ERROR: bad format for signed 64-bit integer for write value.\n");
                usage();
                exit(1);
            }

            break;

        case PLC_LIB_REAL32:
            if(sscanf_platform(write_str,"%f", &f_val32) != 1) {
                printf("ERROR: bad format for 32-bit floating point for write value.\n");
                usage();
                exit(1);
            }

            break;

        case PLC_LIB_REAL64:
            if(sscanf_platform(write_str, "%lf", &f_val64) != 1) {
                printf("ERROR: bad format for 64-bit floating point for write value.\n");
                usage();
                exit(1);
            }

            break;

        default:
            printf("ERROR: bad data type!");
            usage();
            exit(1);
            break;
        }
    } else {
        is_write = 0;
    }

    /* create the tag */
    tag = plc_tag_create(path, DATA_TIMEOUT);
    if(tag < 0) {
        printf("ERROR %s: error creating tag!\n", plc_tag_decode_error(tag));
        if(path) free(path);
        if(write_str) free(write_str);
        return 0;
    }

    if((rc = plc_tag_status(tag)) != PLCTAG_STATUS_OK) {
        printf("ERROR: tag creation error, tag status: %s\n",plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        if(path) free(path);
        if(write_str) free(write_str);
        return 0;
    }

    do {
        if(!is_write) {
            int index = 0;

            rc = plc_tag_read(tag, DATA_TIMEOUT);
            if(rc != PLCTAG_STATUS_OK) {
                printf("ERROR: tag read error, tag status: %s\n",plc_tag_decode_error(rc));
                break;
            }

            /* display the data */
            if(data_type == PLC_LIB_BIT) {
                int rc = plc_tag_get_bit(tag, 0);

                if(rc < 0) {
                    printf("Error received trying to read bit tag: %s!\n", plc_tag_decode_error(rc));
                } else {
                    printf("data=%d\n", rc);
                }
            } else {
                index = 0;
                for(i=0; index < plc_tag_get_size(tag); i++) {
                    switch(data_type) {
                    case PLC_LIB_UINT8:
                        printf("data[%d]=%" PRIu8 " (%" PRIx8 ")\n", i, plc_tag_get_uint8(tag,index),plc_tag_get_uint8(tag,index));
                        index += 1;
                        break;

                    case PLC_LIB_UINT16:
                        printf("data[%d]=%" PRIu16 " (%" PRIx16 ")\n", i, plc_tag_get_uint16(tag,index),plc_tag_get_uint16(tag,index));
                        index += 2;
                        break;

                    case PLC_LIB_UINT32:
                        printf("data[%d]=%" PRIu32 " (%" PRIx32 ")\n",i, plc_tag_get_uint32(tag,index), plc_tag_get_uint32(tag,index));
                        index += 4;
                        break;

                    case PLC_LIB_UINT64:
                        printf("data[%d]=%" PRIu64 " (%" PRIx64 ")\n", i, plc_tag_get_uint64(tag,index),plc_tag_get_uint64(tag,index));
                        index += 8;
                        break;

                    case PLC_LIB_SINT8:
                        printf("data[%d]=%" PRId8 " (%" PRIx8 ")\n", i, plc_tag_get_int8(tag,index),plc_tag_get_int8(tag,index));
                        index += 1;
                        break;

                    case PLC_LIB_SINT16:
                        printf("data[%d]=%" PRId16 " (%" PRIx16 ")\n", i, plc_tag_get_int16(tag,index),plc_tag_get_int16(tag,index));
                        index += 2;
                        break;

                    case PLC_LIB_SINT32:
                        printf("data[%d]=%" PRId32 " (%" PRIx32 ")\n", i, plc_tag_get_int32(tag,index),plc_tag_get_int32(tag,index));
                        index += 4;
                        break;

                    case PLC_LIB_SINT64:
                        printf("data[%d]=%" PRId64 " (%" PRIx64 ")\n", i, plc_tag_get_int64(tag,index),plc_tag_get_int64(tag,index));
                        index += 8;
                        break;

                    case PLC_LIB_REAL32:
                        printf("data[%d]=%f\n",i,plc_tag_get_float32(tag,index));
                        index += 4;
                        break;

                    case PLC_LIB_REAL64:
                        printf("data[%d]=%lf\n", i, plc_tag_get_float64(tag,index));
                        index += 8;
                        break;
                    }
                }
            }
        } else {
            switch(data_type) {
            case PLC_LIB_BIT:
                rc = plc_tag_set_bit(tag, 0, (int)u_val8);
                break;

            case PLC_LIB_UINT8:
                rc = plc_tag_set_uint8(tag,0, u_val8);
                break;

            case PLC_LIB_UINT16:
                rc = plc_tag_set_uint16(tag,0, u_val16);
                break;

            case PLC_LIB_UINT32:
                rc = plc_tag_set_uint32(tag,0, u_val32);
                break;

            case PLC_LIB_UINT64:
                rc = plc_tag_set_uint64(tag,0, u_val64);
                break;

            case PLC_LIB_SINT8:
                rc = plc_tag_set_int8(tag,0, i_val8);
                break;

            case PLC_LIB_SINT16:
                rc = plc_tag_set_int16(tag,0, i_val16);
                break;

            case PLC_LIB_SINT32:
                rc = plc_tag_set_int32(tag,0, i_val32);
                break;

            case PLC_LIB_SINT64:
                rc = plc_tag_set_int64(tag,0, i_val64);
                break;

            case PLC_LIB_REAL32:
                rc = plc_tag_set_float32(tag, 0, f_val32);
                break;

            case PLC_LIB_REAL64:
                rc = plc_tag_set_float64(tag, 0, f_val64);
                break;
            }

            /* write the data */
            rc = plc_tag_write(tag, DATA_TIMEOUT);
            if(rc != PLCTAG_STATUS_OK) {
                printf("ERROR: error writing data: %s!\n",plc_tag_decode_error(rc));
            } else {
                printf("Wrote %s\n",write_str);
            }
        }
    } while(0);

    if(write_str) {
        free(write_str);
    }

    if(path) {
        free(path);
    }

    if(tag) {
        plc_tag_destroy(tag);
    }

    printf("Done\n");

    return 0;
}

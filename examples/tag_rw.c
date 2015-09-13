/***************************************************************************
 *   Copyright (C) 2015 by OmanTek                                         *
 *   Author Kyle Hayes  kylehayes@omantek.com                              *
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


/**************************************************************************
 * CHANGE LOG                                                             *
 *                                                                        *
 * 2012-03-14  KRH - Created file.                                        *
 *                                                                        *
 * 2012-07-18  KRH - Updated code for changed library API.				   *
 **************************************************************************/


#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>
#include <libplctag.h>


#define PLC_LIB_BOOL    (0x101)
#define PLC_LIB_UINT8   (0x108)
#define PLC_LIB_SINT8   (0x208)
#define PLC_LIB_UINT16  (0x110)
#define PLC_LIB_SINT16  (0x210)
#define PLC_LIB_UINT32  (0x120)
#define PLC_LIB_SINT32  (0x220)
#define PLC_LIB_REAL32  (0x320)


#define DATA_TIMEOUT 5000


#ifdef WIN32
#define strcasecmp stricmp
#include <pgetopt.h>
#define getopt pgetopt
#define optarg poptarg
#else
#include <unistd.h>
#endif

void usage(void)
{
	printf( "Usage:\n "
	        "tag_rw [-l] -t <type> -p <path> [-w <val>] \n"
	        "  -t <type> - type is one of 'uint8', 'sint8', 'uint16', 'sint16', \n "
	        "              'uint32', 'sint32', or 'real32'.  The type is the type\n"
	        "              of the data to be read/written to the named tag.  The\n"
	        "              types starting with 'u' are unsigned and with 's' are signed.\n"
	        "              For floating point, use 'real32'.  \n"
	        "  -p <path> - The path to the device containing the named data.\n"
	        "  -w <val>  - The value to write.  Must be formatted appropriately\n"
	        "              for the data type.\n"
	        "\n"
	        "Example: tag_rw -t uint32 -p 'protocol=ab_eip&gateway=10.206.1.27&path=1,0&cpu=LGX&elem_size=4&elem_count=200&name=pcomm_test_dint_array'\n");
}


int main(int argc, char **argv)
{
	plc_tag tag = PLC_TAG_NULL;
	char *write_str=NULL;
	int is_write = 0;
	char *path = NULL;
	int data_type=0;
	uint32_t u_val;
	int32_t i_val;
	real32_t f_val;
	int i;
	int c;
	int rc;
	int timeout;

	while ((c=getopt(argc,argv,"t:w:p:?h"))!=EOF) {
		switch(c) {
			case 't':
				if(!strcasecmp("uint8",optarg)) {
					data_type = PLC_LIB_UINT8;
				} else if(!strcasecmp("sint8",optarg)) {
					data_type = PLC_LIB_SINT8;
				} else if(!strcasecmp("uint16",optarg)) {
					data_type = PLC_LIB_UINT16;
				} else if(!strcasecmp("sint16",optarg)) {
					data_type = PLC_LIB_SINT16;
				} else if(!strcasecmp("uint32",optarg)) {
					data_type = PLC_LIB_UINT32;
				} else if(!strcasecmp("sint32",optarg)) {
					data_type = PLC_LIB_SINT32;
				} else if(!strcasecmp("real32",optarg)) {
					data_type = PLC_LIB_REAL32;
				} else {
					printf("ERROR: unknown data type: %s\n",optarg);
					usage();
					exit(1);
				}

				break;

			case 'w':
				write_str = strdup(optarg);
				break;

			case 'p':
				path = strdup(optarg);
				break;

			case 'h':
			case '?':
				usage();
				exit(0);
				break;

			default:
				printf("ERROR: unknown option.\n");
				usage();
				exit(1);
				break;
		}
	}

	/* check arguments */
	if(!path || !data_type) {
		usage();
		exit(0);
	}

	/* convert any write values */
	if(write_str && strlen(write_str)) {
		is_write = 1;

		switch(data_type) {
			case PLC_LIB_UINT8:
			case PLC_LIB_UINT16:
			case PLC_LIB_UINT32:
				if(sscanf(write_str,"%u",&u_val) != 1) {
					printf("ERROR: bad format for unsigned integer for write value.\n");
					usage();
					exit(1);
				}

				break;

			case PLC_LIB_SINT8:
			case PLC_LIB_SINT16:
			case PLC_LIB_SINT32:
				if(sscanf(write_str,"%d",&i_val) != 1) {
					printf("ERROR: bad format for signed integer for write value.\n");
					usage();
					exit(1);
				}

				break;

			case PLC_LIB_REAL32:
				if(sscanf(write_str,"%f",&f_val) != 1) {
					printf("ERROR: bad format for 32-bit floating point for write value.\n");
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
	tag = plc_tag_create(path);

	if(!tag) {
		printf("ERROR: error creating tag!\n");

		return 0;
	}

	timeout = 5;

	while(timeout-- && plc_tag_status(tag) == PLCTAG_STATUS_PENDING) {
		sleep(1);
	}

	rc = plc_tag_status(tag);

	if(rc != PLCTAG_STATUS_OK) {
		printf("ERROR: tag creation error, tag status: %d\n",rc);
		plc_tag_destroy(tag);

		return 0;
	}

	do {
		if(!is_write) {
			int index = 0;

			rc = plc_tag_read(tag, DATA_TIMEOUT);

			if(rc != PLCTAG_STATUS_OK) {
				printf("ERROR: tag read error, tag status: %d\n",rc);
				plc_tag_destroy(tag);

				return 0;
			}

			/* display the data */
			for(i=0; index < plc_tag_get_size(tag); i++) {
				switch(data_type) {
					case PLC_LIB_UINT8:
						printf("data[%d]=%u (%x)\n",i,plc_tag_get_uint8(tag,index),plc_tag_get_uint8(tag,index));
						index += 1;
						break;

					case PLC_LIB_UINT16:
						printf("data[%d]=%u (%x)\n",i,plc_tag_get_uint16(tag,index),plc_tag_get_uint16(tag,index));
						index += 2;
						break;

					case PLC_LIB_UINT32:
						printf("data[%d]=%u (%x)\n",i,plc_tag_get_uint32(tag,index),plc_tag_get_uint32(tag,index));
						index += 4;
						break;

					case PLC_LIB_SINT8:
						printf("data[%d]=%d (%x)\n",i,plc_tag_get_int8(tag,index),plc_tag_get_int8(tag,index));
						index += 1;
						break;

					case PLC_LIB_SINT16:
						printf("data[%d]=%d (%x)\n",i,plc_tag_get_int16(tag,index),plc_tag_get_int16(tag,index));
						index += 2;
						break;

					case PLC_LIB_SINT32:
						printf("data[%d]=%d (%x)\n",i,plc_tag_get_int32(tag,index),plc_tag_get_int32(tag,index));
						index += 4;
						break;

					case PLC_LIB_REAL32:
						printf("data[%d]=%f\n",i,plc_tag_get_float32(tag,index));
						index += 4;
						break;
				}
			}
		} else {
			switch(data_type) {
				case PLC_LIB_UINT8:
					rc = plc_tag_set_uint8(tag,0,u_val);
					break;

				case PLC_LIB_UINT16:
					rc = plc_tag_set_uint16(tag,0,u_val);
					break;

				case PLC_LIB_UINT32:
					rc = plc_tag_set_uint32(tag,0,u_val);
					break;

				case PLC_LIB_SINT8:
					rc = plc_tag_set_int8(tag,0,i_val);
					break;

				case PLC_LIB_SINT16:
					rc = plc_tag_set_int16(tag,0,i_val);
					break;

				case PLC_LIB_SINT32:
					rc = plc_tag_set_int32(tag,0,i_val);
					break;

				case PLC_LIB_REAL32:
					rc = plc_tag_set_float32(tag,0,f_val);
					break;
			}

			/* write the data */
			rc = plc_tag_write(tag, DATA_TIMEOUT);

			if(rc != PLCTAG_STATUS_OK) {
				printf("ERROR: error writing data: %d!\n",rc);
			} else {
				printf("Wrote %s\n",write_str);
			}
		}
	} while(0);

	if(write_str)
		free(write_str);

	if(path)
		free(path);

	if(tag)
		plc_tag_destroy(tag);

	printf("Done\n");

	return 0;
}

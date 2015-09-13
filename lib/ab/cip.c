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
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

/**************************************************************************
 * CHANGE LOG                                                             *
 *                                                                        *
 * 2013-11-19  KRH - Created file.                                        *
 **************************************************************************/


#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <libplctag.h>
#include <platform.h>
#include <ab/ab_common.h>
#include <ab/cip.h>
#include <ab/tag.h>
#include <ab/eip.h>





/*
 * cip_encode_path()
 *
 * This function takes a path string of comma separated components that are numbers or
 * colon-separated triples that designate a DHP connection.  It converts the path
 * into a path segment in the passed ag.
 *
 * If the protocol type is for a PLC5 series and the last hop in the path is
 * DH+, then we need to set up a different message routing path.
 */

int cip_encode_path(ab_tag_p tag, const char *path)
{
	int ioi_size=0;
	int link_index=0;
	int last_is_dhp=0;
	int has_dhp=0;
	char dhp_channel;
	int src_addr=0, dest_addr=0;
	int tmp=0;
	char **links=NULL;
	char *link=NULL;
	uint8_t *data = tag->conn_path;

	/* split the path */
	links = str_split(path,",");

	if(links == NULL) {
		return PLCTAG_ERR_BAD_PARAM;
	}

	/* work along each string. */
	link = links[link_index];

	while(link && ioi_size < (MAX_CONN_PATH-2)) {   /* MAGIC -2 to allow for padding */
		if(sscanf(link,"%c:%d:%d",&dhp_channel,&src_addr,&dest_addr) == 3) {
			/* DHP link */
			switch(dhp_channel) {
				case 'a':
				case 'A':
				case '2':
					dhp_channel = 1;
					break;

				case 'b':
				case 'B':
				case '3':
					dhp_channel = 2;
					break;

				default:

					/* unknown port! */
					if(links) mem_free(links);

					return PLCTAG_ERR_BAD_PARAM;
					break;
			}

			last_is_dhp = 1;
			has_dhp = 1;
		} else {
			last_is_dhp = 0;

			if(str_to_int(link, &tmp) != 0) {
				if(links) mem_free(links);

				return PLCTAG_ERR_BAD_PARAM;
			}

			*data = tmp;

			/*printf("convert_links() link(%d)=%s (%d)\n",i,*links,tmp);*/

			data++;
			ioi_size++;
		}

		/* FIXME - handle case where IP address is in path */

		link_index++;
		link = links[link_index];
	}

	/* we do not need the split string anymore. */
	if(links) {
		mem_free(links);
		links = NULL;
	}


	/* Add to the path based on the protocol type and
	  * whether the last part is DH+.  Only some combinations of
	  * DH+ and PLC type work.
	  */
	if(last_is_dhp && tag->protocol_type == AB_PROTOCOL_PLC) {
		/* We have to make the difference from the more
		 * generic case.
		 */
		//tag->routing_path[0] = 0x20; /* class */
		//tag->routing_path[1] = 0xA6; /* DH+ */
		//tag->routing_path[2] = 0x24; /* instance */
		//tag->routing_path[3] = dhp_channel;  /* 1 = Channel A, 2 = Channel B */
		//tag->routing_path[4] = 0x2C; /* ? */
		//tag->routing_path[5] = 0x01; /* ? */
		//tag->routing_path_size = 6;

		/* try adding this onto the end of the routing path */
		*data = 0x20;
		data++;
		*data = 0xA6;
		data++;
		*data = 0x24;
		data++;
		*data = dhp_channel;
		data++;
		*data = 0x2C;
		data++;
		*data = 0x01;
		data++;
		ioi_size += 6;

		tag->dhp_src  = src_addr;
		tag->dhp_dest = dest_addr;
		tag->use_dhp_direct = 1;
	} else if(!has_dhp) {
		/* we can do a generic path to the router
		 * object in the PLC.
		 */
		tag->routing_path[0] = 0x20;
		tag->routing_path[1] = 0x02; /* router ? */
		tag->routing_path[2] = 0x24;
		tag->routing_path[3] = 0x01;
		tag->routing_path_size = 4;

		tag->dhp_src  = 0;
		tag->dhp_dest = 0;
		tag->use_dhp_direct = 0;
	} else {
		/* we had the special DH+ format and it was
		 * either not last or not a PLC5/SLC.  That
		 * is an error.
		 */

		tag->dhp_src  = 0;
		tag->dhp_dest = 0;
		tag->use_dhp_direct = 0;

		return PLCTAG_ERR_BAD_PARAM;
	}

	/*
	 * zero out the last byte if we need to.
	 * This pads out the path to a multiple of 16-bit
	 * words.
	 */
	if(ioi_size & 0x01) {
		*data = 0;
		ioi_size++;
	}

	/* set the connection path size */
	tag->conn_path_size = ioi_size;

	return PLCTAG_STATUS_OK;
}







char *cip_decode_status(int status)
{
	switch(status) {
		case 0x04:
			return "Bad or indecipherable IOI!";
			break;

		case 0x05:
			return "Unknown tag or item!";
			break;

		case 0x06:
			return "Response too large, partial data transfered!";
			break;

		case 0x0A:
			return "Error processing attributes!";
			break;

		case 0x13:
			return "Insufficient data/params to process request!";
			break;

		case 0x1C:
			return "Insufficient attributes to process request!";
			break;

		case 0x26:
			return "IOI word length does not match IOI length processed!";
			break;

		case 0xFF:

			/* extended status */

		default:
			return "Unknown error status.";
			break;
	}

	return "Unknown error status.";
}










#ifdef START
#undef START
#endif
#define START 1

#ifdef ARRAY
#undef ARRAY
#endif
#define ARRAY 2

#ifdef DOT
#undef DOT
#endif
#define DOT 3

#ifdef NAME
#undef NAME
#endif
#define NAME 4

/*
 * cip_encode_tag_name()
 *
 * This takes a LGX-style tag name like foo[14].blah and
 * turns it into an IOI path/string.
 */

int cip_encode_tag_name(ab_tag_p tag,const char *name)
{
	uint8_t *data = tag->encoded_name;
	const char *p = name;
	uint8_t *word_count = NULL;
	uint8_t *dp = NULL;
	uint8_t *name_len;
	int state;

	/* reserve room for word count for IOI string. */
	word_count = data;
	dp = data + 1;

	state = START;

	while(*p) {
		switch(state) {
			case START:

				/* must start with an alpha character or _ or :. */
				if(isalpha(*p) || *p == '_' || *p == ':') {
					state = NAME;
				} else if(*p == '.') {
					state = DOT;
				} else if(*p == '[') {
					state = ARRAY;
				} else {
					return 0;
				}

				break;

			case NAME:
				*dp = 0x91; /* start of ASCII name */
				dp++;
				name_len = dp;
				*name_len = 0;
				dp++;

				while(isalnum(*p) || *p == '_' || *p == ':') {
					*dp = *p;
					dp++;
					p++;
					(*name_len)++;
				}

				/* must pad the name to a multiple of two bytes */
				if(*name_len & 0x01) {
					*dp = 0;
					dp++;
				}

				state = START;

				break;

			case ARRAY:
				/* move the pointer past the [ character */
				p++;

				do {
					uint32_t val;
					char *np = NULL;
					val = (uint32_t)strtol(p,&np,0);

					if(np == p) {
						/* we must have a number */
						return 0;
					}

					p = np;

					if(val > 0xFFFF) {
						*dp = 0x2A;
						dp++;  /* 4-byte value */
						*dp = 0;
						dp++;     /* padding */

						/* copy the value in little-endian order */
						*dp = val & 0xFF;
						dp++;
						*dp = (val >> 8) & 0xFF;
						dp++;
						*dp = (val >> 16) & 0xFF;
						dp++;
						*dp = (val >> 24) & 0xFF;
						dp++;
					} else if(val > 0xFF) {
						*dp = 0x29;
						dp++;  /* 2-byte value */
						*dp = 0;
						dp++;     /* padding */

						/* copy the value in little-endian order */
						*dp = val & 0xFF;
						dp++;
						*dp = (val >> 8) & 0xFF;
						dp++;
					} else {
						*dp = 0x28;
						dp++;  /* 1-byte value */
						*dp = val;
						dp++;     /* value */
					}

					/* eat up whitespace */
					while(isspace(*p)) p++;
				} while(*p == ',');

				if(*p != ']')
					return 0;

				p++;

				state = START;

				break;

			case DOT:
				p++;
				state = START;
				break;

			default:
				/* this should never happen */
				return 0;

				break;
		}
	}

	/* word_count is in units of 16-bit integers, do not
	 * count the word_count value itself.
	 */
	*word_count = ((dp - data)-1)/2;

	/* store the size of the whole result */
	tag->encoded_name_size = dp - data;

	return 1;
}

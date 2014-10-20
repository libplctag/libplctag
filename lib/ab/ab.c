/***************************************************************************
 *   Copyright (C) 2012 by Process Control Engineers                       *
 *   Author Kyle Hayes  kylehayes@processcontrolengineers.com              *
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

/*#ifdef __cplusplus
extern "C"
{
#endif
*/



/*#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
*/
#include <platform.h>
#include <libplctag.h>
#include <libplctag_tag.h>
#include <ab/ab.h>
#include <ab/ab_defs.h>
#include <ab/common.h>
#include <ab/pccc.h>
#include <ab/cip.h>
#include <ab/eip_cip.h>
#include <ab/eip_pccc.h>
#include <ab/eip_dhp_pccc.h>
#include <util/attr.h>


/*
 * Generic Rockwell/Allen-Bradley protocol functions.
 *
 * These are the primary entry points into the AB protocol
 * stack.
 */


/* vtables for different kinds of tags */
struct tag_vtable_t default_vtable /*= { NULL, ab_tag_destroy, NULL, NULL }*/;
struct tag_vtable_t cip_vtable /*= { ab_tag_abort, ab_tag_destroy, eip_cip_tag_read_start, eip_cip_tag_status, eip_cip_tag_write_start }*/;
struct tag_vtable_t plc_vtable /*= { ab_tag_abort, ab_tag_destroy, eip_pccc_tag_read_start, eip_pccc_tag_status, eip_pccc_tag_write_start }*/;
struct tag_vtable_t plc_dhp_vtable /*= { ab_tag_abort, ab_tag_destroy, eip_dhp_pccc_tag_read_start, eip_dhp_pccc_tag_status, eip_dhp_pccc_tag_write_start}*/;


tag_vtable_p set_tag_vtable(ab_tag_p tag);


plc_tag ab_tag_create(attr attribs)
{
    ab_tag_p tag = AB_TAG_NULL;
    const char *path;
	int rc;
	int debug = attr_get_int(attribs,"debug",0);

    pdebug(debug,"Starting.");


    /*
     * allocate memory for the new tag.  Do this first so that
     * we have a vehicle for returning status.
     */

    tag = (ab_tag_p)mem_alloc(sizeof(struct ab_tag_t));

    if(!tag) {
    	pdebug(debug,"Unable to allocate memory for AB EIP tag!");
    	return PLC_TAG_NULL;
    }

    /* store the debug status */
    tag->debug = debug;

    /*
     * check the CPU type.
     *
     * This determines the protocol type.
     */
    if(check_cpu(tag, attribs) != PLCTAG_STATUS_OK) {
        tag->status = PLCTAG_ERR_BAD_DEVICE;
        return (plc_tag)tag;
    }

    /* AB PLCs are little endian. */
    tag->endian = PLCTAG_DATA_LITTLE_ENDIAN;

    /* allocate memory for the data */
    tag->elem_count = attr_get_int(attribs,"elem_count",1);
    tag->elem_size = attr_get_int(attribs,"elem_size",0);
    tag->size = (tag->elem_count) * (tag->elem_size);

    if(tag->size == 0) {
    	/* failure! Need data_size! */
    	tag->status = PLCTAG_ERR_BAD_PARAM;
    	return (plc_tag)tag;
    }

    tag->data = (uint8_t*)mem_alloc(tag->size);

    if(tag->data == NULL) {
    	tag->status = PLCTAG_ERR_NO_MEM;
    	return (plc_tag)tag;
    }

    /* get the connection path, punt if there is not one and we have a Logix-class PLC. */
    path = attr_get_str(attribs,"path",NULL);

    if(path == NULL && tag->protocol_type == AB_PROTOCOL_LGX) {
    	tag->status = PLCTAG_ERR_BAD_PARAM;
    	return (plc_tag)tag;
    }

    /* make sure the global mutex is set up */
    rc = check_mutex(tag->debug);
    if(rc != PLCTAG_STATUS_OK) {
    	tag->status = rc;
    	return (plc_tag)tag;
    }

    tag->first_read = 1;

	/*
	 * now we start the part that might conflict with other threads.
	 *
	 * The rest of this is inside a locked block.
	 */
	pdebug(debug,"Locking mutex");
	critical_block(io_thread_mutex) {
		/*
		 * set up tag vtable.  This is protocol specific
		 */
		tag->vtable = set_tag_vtable(tag);

		if(!tag->vtable) {
			pdebug(debug,"Unable to set tag vtable!");
			tag->status = PLCTAG_ERR_BAD_PARAM;
			break;
		}

		/*
		 * Check the request IO handler thread.
		 */
		if(!io_handler_thread) {
			rc = thread_create((thread_p*)&io_handler_thread,request_handler_func, 32*1024, NULL);
			if(rc != PLCTAG_STATUS_OK) {
				pdebug(debug,"Unable to create request handler thread!");
				tag->status = rc;
				break;
			}
		}

		/*
		 * Find or create a session.
		 */
		if(find_or_create_session(tag, attribs) != PLCTAG_STATUS_OK) {
			pdebug(debug,"Unable to create session!");
			tag->status = PLCTAG_ERR_BAD_GATEWAY;
			break;
		}

		/*
		 * parse the link path into the tag.  Note that it must
		 * pad the byte string to a multiple of 16-bit words. The function
		 * also adds the protocol/PLC specific routing information to the
		 * links specified.  This fills in fields in the connection about
		 * any DH+ special data.
		 * 
		 * Skip this if we don't have a path.
		 */
		if(path && cip_encode_path(tag,path) != PLCTAG_STATUS_OK) {
			pdebug(debug,"Unable to convert links strings to binary path!");
			tag->status = PLCTAG_ERR_BAD_PARAM;
			break;
		}


		/*
		 * check the tag name, this is protocol specific.
		 */

		if(check_tag_name(tag, attr_get_str(attribs,"name","NONE")) != PLCTAG_STATUS_OK) {
			pdebug(debug,"Bad tag name!");
			tag->status = PLCTAG_ERR_BAD_PARAM;
			break;
		}

		/*
		 * add the tag to the session's list.
		 */
		if(session_add_tag_unsafe(tag, tag->session) != PLCTAG_STATUS_OK) {
			pdebug(debug,"unable to add new tag to connection!");

			tag->status = PLCTAG_ERR_CREATE;
			break;
		}
    }

    pdebug(debug,"Done.");

    return (plc_tag)tag;
}





/*
 * set_tag_vtable
 *
 * Use various bits of information about the tag to determine
 * just what flavor of the protocol we will be using for this
 * tag.
 */
tag_vtable_p set_tag_vtable(ab_tag_p tag)
{
	switch(tag->protocol_type) {
		case AB_PROTOCOL_PLC:
			if(tag->use_dhp_direct) {
				if(!plc_dhp_vtable.abort) {
					plc_dhp_vtable.abort     = (tag_abort_func)ab_tag_abort;
					plc_dhp_vtable.destroy   = (tag_destroy_func)ab_tag_destroy;
					plc_dhp_vtable.read      = (tag_read_func)eip_dhp_pccc_tag_read_start;
					plc_dhp_vtable.status    = (tag_status_func)eip_dhp_pccc_tag_status;
					plc_dhp_vtable.write     = (tag_write_func)eip_dhp_pccc_tag_write_start;
				}
				return &plc_dhp_vtable;
			} else {
				if(!plc_vtable.abort) {
					plc_vtable.abort     = (tag_abort_func)ab_tag_abort;
					plc_vtable.destroy   = (tag_destroy_func)ab_tag_destroy;
					plc_vtable.read      = (tag_read_func)eip_pccc_tag_read_start;
					plc_vtable.status    = (tag_status_func)eip_pccc_tag_status;
					plc_vtable.write     = (tag_write_func)eip_pccc_tag_write_start;
				}
				return &plc_vtable;
			}
			break;

		case AB_PROTOCOL_MLGX:
			if(!plc_vtable.abort) {
				plc_vtable.abort     = (tag_abort_func)ab_tag_abort;
				plc_vtable.destroy   = (tag_destroy_func)ab_tag_destroy;
				plc_vtable.read      = (tag_read_func)eip_pccc_tag_read_start;
				plc_vtable.status    = (tag_status_func)eip_pccc_tag_status;
				plc_vtable.write     = (tag_write_func)eip_pccc_tag_write_start;
			}
			return &plc_vtable;

			break;

		case AB_PROTOCOL_LGX:
			if(!cip_vtable.abort) {
				cip_vtable.abort     = (tag_abort_func)ab_tag_abort;
				cip_vtable.destroy   = (tag_destroy_func)ab_tag_destroy;
				cip_vtable.read      = (tag_read_func)eip_cip_tag_read_start;
				cip_vtable.status    = (tag_status_func)eip_cip_tag_status;
				cip_vtable.write     = (tag_write_func)eip_cip_tag_write_start;
			}
			return &cip_vtable;

			break;

		default:
			return NULL;
			break;
	}

	return NULL;
}









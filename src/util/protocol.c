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

#include <ctype.h>
#include <stdlib.h>
#include <lib/libplctag.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/mem.h>
#include <util/mutex.h>
#include <util/protocol.h>
#include <util/string.h>


static protocol_p protocol_list = NULL;
static mutex_p protocol_list_mutex = NULL;



int protocol_get(const char *stack_type, attr attribs, protocol_p *protocol, int (*constructor)(attr attribs, protocol_p *protocol))
{
    int rc = PLCTAG_STATUS_OK;
    const char *host = NULL;
    const char *path = NULL;
    protocol_p result = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    /* get the host and path. */
    host = attr_get_str(attribs, "gateway", NULL);
    path = attr_get_str(attribs, "path", "");

    /* try to find a protocol that matches. */
    critical_block(protocol_list_mutex) {
        result = protocol_list;

        while(result && ((str_cmp_i(stack_type, result->stack_type) != 0) || (str_cmp_i(host, result->host) != 0) || (str_cmp_i(path, result->path) != 0))) {
            result = result->next;
        }

        /* if found, increment the reference to prevent dropping it before we return it. */
        if(result) {
            /*
             * if the existing object is in the process of being destroyed,
             * then this will result in a null pointer and we will create a
             * new one.
             */

            result = rc_inc(result);
        }

        if(!result) {
            /* not found, so make a new one. */
            rc = constructor(attribs, &result);
            if(rc == PLCTAG_STATUS_OK) {
                result->next = NULL;

                result->stack_type = str_dup(stack_type);
                if(!result->stack_type) {
                    pdebug(DEBUG_WARN, "Unable to allocate stack type copy!");
                    result = rc_dec(result);
                    break;
                }

                result->host = str_dup(host);
                if(!result->host) {
                    pdebug(DEBUG_WARN, "Unable to allocate host name copy!");
                    result = rc_dec(result);
                    break;
                }

                result->path = str_dup(path);
                if(!result->path) {
                    pdebug(DEBUG_WARN, "Unable to allocate path copy!");
                    result = rc_dec(result);
                    break;
                }

                rc = mutex_create(&(result->mutex));
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Unable to create PLC mutex, got error %s!", plc_tag_decode_error(rc));
                    result = rc_dec(result);
                    break;
                }

                /* get it on the list before we leave the mutex. */
                result->next = protocol_list;
                protocol_list = result;

                *protocol = result;
            } else {
                pdebug(DEBUG_WARN, "Unable to allocate new protocol, error %s!", plc_tag_decode_error(rc));
                break;
            }
        } /* else we found an existing one. */
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


int protocol_cleanup(protocol_p protocol)
{
    pdebug(DEBUG_INFO, "Starting.");

    if(!protocol) {
        pdebug(DEBUG_WARN, "Destructor function called with null pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* remove the PLC from the list. */
    critical_block(protocol_list_mutex) {
        protocol_p *walker = &protocol_list;

        while(*walker && *walker != protocol) {
            walker = &((*walker)->next);
        }

        if(*walker && *walker == protocol) {
            *walker = protocol->next;
            protocol->next = NULL;
        } /* else not on the list.  Weird. */
    }

    if(protocol->path) {
        mem_free(protocol->path);
        protocol->path = NULL;
    }

    if(protocol->host) {
        mem_free(protocol->host);
        protocol->host = NULL;
    }

    if(protocol->mutex) {
        mutex_destroy(&(protocol->mutex));
        protocol->mutex = NULL;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}



int protocol_request_init(protocol_p protocol, protocol_request_p req)
{
    pdebug(DEBUG_DETAIL, "Starting.");

    if(protocol) {
        pdebug(DEBUG_WARN, "PLC pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(req) {
        pdebug(DEBUG_WARN, "Request pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    mem_set(req, 0, sizeof(*req));

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


int protocol_request_start(protocol_p protocol, protocol_request_p req, void *tag,
                           slice_t (*build_request_callback)(slice_t output_buffer, void *tag),
                           int (*handle_response_callback)(slice_t input_buffer, void *tag))
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(protocol) {
        pdebug(DEBUG_WARN, "PLC pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(req) {
        pdebug(DEBUG_WARN, "Request pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* set up the request and put it on the list. */
    critical_block(protocol->mutex) {
        /* find the request, or the end of the list. */
        protocol_request_p *walker = &(protocol->request_list);

        while(*walker && *walker != req) {
            walker = &((*walker)->next);
        }

        if(! *walker) {
            /* ran off the end of the list. */
            req->next = NULL;
            req->protocol = protocol;
            req->tag = tag;
            req->build_request_callback = build_request_callback;
            req->handle_response_callback = handle_response_callback;

            /* add the request to the end of the list. */
            *walker = req;
        } else {
            pdebug(DEBUG_WARN, "Request is already in use!");
            rc = PLCTAG_ERR_BUSY;
            break;
        }
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int protocol_request_abort(protocol_p protocol, protocol_request_p req)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(protocol) {
        pdebug(DEBUG_WARN, "PLC pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(req) {
        pdebug(DEBUG_WARN, "Request pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* remove the request from the list. */
    critical_block(protocol->mutex) {
        protocol_request_p *walker = &(protocol->request_list);

        while(*walker && *walker != req) {
            walker = &((*walker)->next);
        }

        if(*walker && *walker == req) {
            *walker = req->next;
            req->next = NULL;
        } else {
            rc = PLCTAG_ERR_NOT_FOUND;
        }
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}



int protocol_init(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    rc = mutex_create(&protocol_list_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create PCCC PLC mutex!");
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


void protocol_teardown(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    rc = mutex_destroy(&protocol_list_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create PCCC PLC mutex!");
    }

    protocol_list = NULL;

    pdebug(DEBUG_INFO, "Done.");
}


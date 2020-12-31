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



int protocol_get(const char *protocol_key, attr attribs, protocol_p *protocol, int (*constructor)(attr attribs, protocol_p *protocol))
{
    int rc = PLCTAG_STATUS_OK;
    const char *host = NULL;
    const char *path = NULL;
    protocol_p result = NULL;

    pdebug(DEBUG_INFO, "Starting for protocol layer %s.", protocol_key);

    /* try to find a protocol that matches. */
    critical_block(protocol_list_mutex) {
        result = protocol_list;

        while(result && (str_cmp_i(protocol_key, result->protocol_key) != 0)) {
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
                *protocol = result;
            } else {
                pdebug(DEBUG_WARN, "Unable to allocate new protocol, error %s!", plc_tag_decode_error(rc));
                break;
            }
        } /* else we found an existing one. */
    }

    pdebug(DEBUG_DETAIL, "Starting for protocol layer %s.", protocol_key);

    return rc;
}



int protocol_init(protocol_p protocol,
                  const char *protocol_key,
                  int (*handle_new_request_callback)(protocol_p protocol, protocol_request_p request),
                  int (*handle_cleanup_request_callback)(protocol_p protocol, protocol_request_p request))
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting for protocol layer %s.", protocol->protocol_key);

    do {
        protocol->next = NULL;

        protocol->protocol_key = str_dup(protocol_key);
        if(!protocol->protocol_key) {
            pdebug(DEBUG_WARN, "Unable to allocate protocol key copy!");
            break;
        }

        rc = mutex_create(&(protocol->mutex));
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to create PLC mutex, got error %s!", plc_tag_decode_error(rc));
            break;
        }

        protocol->handle_new_request_callback = handle_new_request_callback;
        protocol->handle_cleanup_request_callback = handle_cleanup_request_callback;

        /* get it on the list before we leave the mutex. */
        protocol->next = protocol_list;
        protocol_list = protocol;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to initialize protocol, error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_DETAIL, "Done for protocol layer %s.", protocol->protocol_key);

    return rc;
}


int protocol_cleanup(protocol_p protocol)
{
    if(!protocol) {
        pdebug(DEBUG_WARN, "Destructor function called with null pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_DETAIL, "Starting for protocol layer %s.", protocol->protocol_key);

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

    if(protocol->protocol_key) {
        mem_free(protocol->protocol_key);
        protocol->protocol_key = NULL;
    }

    if(protocol->mutex) {
        mutex_destroy(&(protocol->mutex));
        protocol->mutex = NULL;
    }

    pdebug(DEBUG_DETAIL, "Done for protocol layer %s.", protocol->protocol_key);

    return PLCTAG_STATUS_OK;
}



int protocol_request_init(protocol_p protocol, protocol_request_p req)
{
    if(!protocol) {
        pdebug(DEBUG_WARN, "PLC pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_DETAIL, "Starting for protocol layer %s.", protocol->protocol_key);

    if(!req) {
        pdebug(DEBUG_WARN, "Request pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    mem_set(req, 0, sizeof(*req));

    pdebug(DEBUG_DETAIL, "Done for protocol layer %s.", protocol->protocol_key);

    return PLCTAG_STATUS_OK;
}



int protocol_start_request(protocol_p protocol,
                           protocol_request_p request,
                           void *client,
                           int (*build_request_callback)(protocol_p protocol, void *client, slice_t output_buffer, slice_t *used_buffer),
                           int (*process_response_callback)(protocol_p protocol, void *client, slice_t input_buffer, slice_t *used_buffer))
{
    int rc = PLCTAG_STATUS_OK;

    if(!protocol) {
        pdebug(DEBUG_WARN, "PLC pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_DETAIL, "Starting for protocol layer %s.", protocol->protocol_key);

    if(!request) {
        pdebug(DEBUG_WARN, "Request pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* set up the request and put it on the list. */
    critical_block(protocol->mutex) {
        rc = protocol->handle_new_request_callback(protocol, request);
        if(rc == PLCTAG_STATUS_OK) {
            /* find the request, or the end of the list. */
            protocol_request_p *walker = &(protocol->request_list);

            while(*walker && *walker != request) {
                walker = &((*walker)->next);
            }

            if(! *walker) {
                /* ran off the end of the list. */
                request->next = NULL;
                request->client = client;
                request->build_request_callback = build_request_callback;
                request->process_response_callback = process_response_callback;

                /* add the request to the end of the list. */
                *walker = request;
            } else {
                pdebug(DEBUG_WARN, "Request is already in use!");
                rc = PLCTAG_ERR_BUSY;
                break;
            }
        } /* else we refused the new request. */
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO, "Refused new request with error %s!", plc_tag_decode_error(rc));
    }

    pdebug(DEBUG_DETAIL, "Done for protocol layer %s.", protocol->protocol_key);

    return rc;
}


int protocol_stop_request(protocol_p protocol, protocol_request_p req)
{
    int rc = PLCTAG_STATUS_OK;

    if(!protocol) {
        pdebug(DEBUG_WARN, "PLC pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_DETAIL, "Starting for protocol layer %s.", protocol->protocol_key);

    if(!req) {
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
            rc = protocol->handle_cleanup_request_callback(protocol, req);
        } else {
            rc = PLCTAG_ERR_NOT_FOUND;
        }
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO, "Error, %s, found while trying to stop request!", plc_tag_decode_error(rc));
    }

    pdebug(DEBUG_DETAIL, "Done for protocol layer %s.", protocol->protocol_key);

    return rc;
}




int protocol_build_request(protocol_p protocol,
                           slice_t original_output_buffer,
                           slice_t *new_output_buffer,
                           int (*build_request_result_callback)(protocol_p protocol, protocol_request_p request, int status, slice_t used_slice, slice_t *new_output_slice))
{
    int rc = PLCTAG_STATUS_OK;
    slice_t output_buffer = original_output_buffer;
    slice_t used_buffer = slice_make(NULL, 0);
    int total_byte_count = 0;

    pdebug(DEBUG_DETAIL, "Starting for protocol layer %s.", protocol->protocol_key);

    /* iterate over the list of outstanding requests. */
    critical_block(protocol->mutex) {
        protocol_request_p *request_walker = &(protocol->request_list);

        while(*request_walker && slice_len(output_buffer) > 0) {
            /* build the request in the passed output buffer and return the used space. */
            rc = (*request_walker)->build_request_callback(protocol, (*request_walker)->client, output_buffer, &used_buffer);

            /* check the result and possibly change the result. Do any post-processing. */
            if(build_request_result_callback) {
                rc = build_request_result_callback(protocol, *request_walker, rc, used_buffer, &output_buffer);
            } else {
                /* no special handling, just trim off the part we used. */
                output_buffer = slice_from_slice(output_buffer, slice_len(used_buffer), slice_len(output_buffer));
            }

            if(rc != PLCTAG_STATUS_PENDING) {
                /* unlink the request. */
                protocol_request_p tmp_req = *request_walker;
                *request_walker = tmp_req->next;

                protocol->handle_cleanup_request_callback(protocol, tmp_req);

                break;
            } else {
                total_byte_count += slice_len(used_buffer);
            }

            /* go to the next one if we are not at the end of the list. */
            if(*request_walker) {
                request_walker = &((*request_walker)->next);
            }
        }
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, encountered while building requests!", plc_tag_decode_error(rc));
        *new_output_buffer = slice_make_err(rc);
        return rc;
    } else {
        *new_output_buffer = slice_from_slice(original_output_buffer, 0, total_byte_count);

        pdebug(DEBUG_DETAIL, "Completed request packet:");
        pdebug_dump_bytes(DEBUG_DETAIL, slice_data(*new_output_buffer), slice_len(*new_output_buffer));
    }

    pdebug(DEBUG_DETAIL, "Done for protocol layer %s.", protocol->protocol_key);

    return rc;
}



int protocol_process_response(protocol_p protocol,
                              slice_t original_input_buffer,
                              slice_t *remaining_input_buffer,
                              int (*process_response_result_callback)(protocol_p protocol, protocol_request_p request, int status, slice_t used_slice, slice_t *new_output_slice))
{
    int rc = PLCTAG_STATUS_OK;
    slice_t input_buffer = original_input_buffer;
    slice_t used_buffer = slice_make(NULL, 0);
    int total_bytes_processed = 0;

    pdebug(DEBUG_DETAIL, "Starting for protocol layer %s.", protocol->protocol_key);

    /* iterate over the list of outstanding requests. */
    critical_block(protocol->mutex) {
        protocol_request_p *request_walker = &(protocol->request_list);

        while(*request_walker && slice_len(input_buffer) > 0) {
            /* build the request in the passed output buffer and return the used space. */
            rc = (*request_walker)->process_response_callback(protocol, (*request_walker)->client, input_buffer, &used_buffer);

            /* check the result and possibly change the result. Do any post-processing. */
            if(process_response_result_callback) {
                rc = process_response_result_callback(protocol, *request_walker, rc, used_buffer, &input_buffer);
            } else {
                /* no special handling, just trim off the part we used. */
                input_buffer = slice_from_slice(input_buffer, slice_len(used_buffer), slice_len(input_buffer));
            }

            if(rc != PLCTAG_STATUS_PENDING) {
                /* unlink the request. */
                protocol_request_p tmp_req = *request_walker;
                *request_walker = tmp_req->next;

                protocol->handle_cleanup_request_callback(protocol, tmp_req);

                break;
            } else {
                total_bytes_processed += slice_len(used_buffer);
            }

            /* go to the next one if we are not at the end of the list. */
            if(*request_walker) {
                request_walker = &((*request_walker)->next);
            }
        }
    }

    if(rc == PLCTAG_STATUS_OK) {
        *remaining_input_buffer = slice_from_slice(original_input_buffer, total_bytes_processed, slice_len(original_input_buffer));

        pdebug(DEBUG_DETAIL, "Data bytes remaining:");
        pdebug_dump_bytes(DEBUG_DETAIL, slice_data(*remaining_input_buffer), slice_len(*remaining_input_buffer));
    } else {
        pdebug(DEBUG_WARN, "Error, %s, found while processing input packet!", plc_tag_decode_error(rc));
        *remaining_input_buffer = slice_make_err(rc);
        return rc;
    }

    pdebug(DEBUG_DETAIL, "Done for protocol layer %s.", protocol->protocol_key);

    return rc;
}







int protocol_module_init(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    rc = mutex_create(&protocol_list_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create PCCC PLC mutex!");
        return rc;
    }

    protocol_list = NULL;

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


void protocol_module_teardown(void)
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


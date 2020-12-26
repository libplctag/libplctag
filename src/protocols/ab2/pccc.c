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

#include <lib/libplctag.h>
#include <ab2/pccc.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/mem.h>
#include <util/mutex.h>
#include <util/string.h>

struct pccc_plc_s {
    struct pccc_plc_s *next;

    mutex_p mutex;
    pccc_plc_request_p request_list;

    char *host;
    char *path;
};


static pccc_plc_p pccc_plc_list = NULL;
static mutex_p pccc_plc_list_mutex = NULL;


static void pccc_plc_rc_destroy(void *plc_arg);


pccc_plc_p pccc_plc_get(attr attribs)
{
    int rc = PLCTAG_STATUS_OK;
    const char *host = NULL;
    const char *path = NULL;
    pccc_plc_p result = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    /* get the host and path. */
    host = attr_get_str(attribs, "gateway", NULL);
    path = attr_get_str(attribs, "path", "");

    critical_block(pccc_plc_list_mutex) {
        /* try to find a PLC that matches. */
        result = pccc_plc_list;

        while(result && (string_cmp_i(host, result->host) != 0) && (string_cmp_i(path, result->path) != 0)) {
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
            result = (pccc_plc_p)rc_alloc(sizeof(*result), pccc_plc_rc_destroy);
            if(result) {
                result->next = NULL;

                result->host = string_dup(host);
                if(!result->host) {
                    pdebug(DEBUG_WARN, "Unable to allocate host name copy!");
                    result = rc_dec(result);
                    break;
                }

                result->path = string_dup(path);
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

                /* TODO get the next protocol layer */

                /* get it on the list before we leave the mutex. */
                result->next = pccc_plc_list;
                pccc_plc_list = result;
            } else {
                pdebug(DEBUG_WARN, "Unable to allocate new PCCC PLC!");
                break;
            }
        } /* else we found an existing one. */
    }

    pdebug(DEBUG_INFO, "Done.");

    return result;
}


int pccc_plc_request_init(pccc_plc_p pccc_plc, pccc_plc_request_p req)
{
    pdebug(DEBUG_DETAIL, "Starting.");

    if(pccc_plc) {
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


int pccc_plc_request_start(pccc_plc_p plc, pccc_plc_request_p req, void *tag,
                                  int (*build_request_callback)(slice_t output_buffer, pccc_plc_p plc, void *tag),
                                  int (*handle_response_callback)(slice_t input_buffer, pccc_plc_p plc, void *tag))
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(plc) {
        pdebug(DEBUG_WARN, "PLC pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(req) {
        pdebug(DEBUG_WARN, "Request pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* set up the request and put it on the list. */
    critical_block(plc->mutex) {
        /* find the request, or the end of the list. */
        pccc_plc_request_p *walker = &(plc->request_list);

        while(*walker && *walker != req) {
            walker = &((*walker)->next);
        }

        if(! *walker) {
            /* ran off the end of the list. */
            req->next = NULL;
            req->plc = plc;
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


int pccc_plc_request_abort(pccc_plc_p plc, pccc_plc_request_p req)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(plc) {
        pdebug(DEBUG_WARN, "PLC pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(req) {
        pdebug(DEBUG_WARN, "Request pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* remove the request from the list. */
    critical_block(plc->mutex) {
        pccc_plc_request_p *walker = &(plc->request_list);

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


int pccc_plc_init(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    rc = mutex_create(&pccc_plc_list_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create PCCC PLC mutex!");
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


int pccc_plc_teardown(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    rc = mutex_destroy(&pccc_plc_list_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create PCCC PLC mutex!");
    }

    pccc_plc_list = NULL;

    pdebug(DEBUG_INFO, "Done.");
}


void pccc_plc_rc_destroy(void *plc_arg)
{
    pccc_plc_p plc = (pccc_plc_p)plc_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!plc) {
        pdebug(DEBUG_WARN, "Destructor function called with null pointer!");
        return;
    }

    /* remove the PLC from the list. */
    critical_block(pccc_plc_list_mutex) {
        pccc_plc_p *walker = &pccc_plc_init;

        while(*walker && *walker != plc) {
            walker = &((*walker)->next);
        }

        if(*walker && *walker == plc) {
            *walker = plc->next;
            plc->next = NULL;
        } /* else not on the list.  Weird. */
    }

    if(plc->path) {
        mem_free(plc->path);
        plc->path = NULL;
    }

    if(plc->host) {
        mem_free(plc->host);
        plc->host = NULL;
    }

    if(plc->mutex) {
        mutex_destroy(&(plc->mutex));
        plc->mutex = NULL;
    }

    pdebug(DEBUG_INFO, "Done.");
}



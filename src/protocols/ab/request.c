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
 * 2015-09-12  KRH - Created file.                                        *
 *                                                                        *
 **************************************************************************/


#include <ab/ab_common.h>
#include <ab/request.h>
#include <platform.h>
#include <ab/session.h>
#include <util/debug.h>
#include <util/rc.h>

void request_destroy(void *request_arg);

/*
 * request_create
 *
 * This does not do much for now other than allocate memory.  In the future
 * it may be desired to keep a pool of request buffers instead.  This shim
 * is here so that such a change can be done without major code changes
 * elsewhere.
 */

int request_create(ab_request_p *req, int max_payload_size, ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    ab_request_p res;
    int request_capacity = EIP_CIP_PREFIX_SIZE + max_payload_size;

    pdebug(DEBUG_DETAIL,"Starting.");

    res = (ab_request_p)rc_alloc(sizeof(struct ab_request_t) + request_capacity, request_destroy);

    if (!res) {
        *req = NULL;
        rc = PLCTAG_ERR_NO_MEM;
    } else {
        res->request_capacity = request_capacity;

        rc = mutex_create(&(res->request_mutex));
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN,"Unable to create request mutex!");
            *req = NULL;
            rc_dec(req);
            return rc;
        }

        /* we need to be careful as this sets up a reference cycle! */
        res->tag = rc_inc(tag);

        *req = res;
    }

    pdebug(DEBUG_DETAIL,"Done.");

    return rc;
}


int request_abort(ab_request_p req)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL,"Starting.");

    if(!req) {
        pdebug(DEBUG_WARN,"request pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    critical_block(req->request_mutex) {
        req->_abort_request = !0;
        req->tag = rc_dec(req->tag);
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int request_check_abort(ab_request_p req)
{
    int result = 0;

    if(!req) {
        return !0;
    }

    critical_block(req->request_mutex) {
        result = req->_abort_request;
    }

    return result;
}



ab_tag_p request_get_tag(ab_request_p req)
{
    ab_tag_p result = NULL;

    if(!req) {
        return result;
    }

    critical_block(req->request_mutex) {
        result = rc_inc(req->tag);
    }

    return result;
}


int request_allow_packing(ab_request_p req)
{
    int result = PLCTAG_STATUS_OK;

    if(!req) {
        return PLCTAG_ERR_NULL_PTR;
    }

    critical_block(req->request_mutex) {
        req->allow_packing = 1;
        result = PLCTAG_STATUS_OK;
    }

    return result;
}


int request_check_packing(ab_request_p req)
{
    int result = 0;

    if(!req) {
        return result;
    }

    critical_block(req->request_mutex) {
        result = req->allow_packing;
    }

    return result;
}




int request_get_packing_num(ab_request_p req)
{
    int result = 0;

    if(!req) {
        return result;
    }

    critical_block(req->request_mutex) {
        result = req->allow_packing;
    }

    return result;
}


int request_set_packing_num(ab_request_p req, int packing_num)
{
    int result = PLCTAG_STATUS_OK;

    if(!req) {
        return PLCTAG_ERR_NULL_PTR;
    }

    critical_block(req->request_mutex) {
        req->packing_num = packing_num;
        result = PLCTAG_STATUS_OK;
    }

    return result;
}




/*
 * request_destroy
 *
 * The request must be removed from any lists before this!
 */
//int request_destroy_unsafe(ab_request_p* req_pp)
void request_destroy(void *req_arg)
{
    ab_request_p req = req_arg;

    pdebug(DEBUG_DETAIL, "Starting.");

    request_abort(req);

    mutex_destroy(&(req->request_mutex));

    pdebug(DEBUG_DETAIL, "Done.");
}



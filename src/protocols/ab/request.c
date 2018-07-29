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
int request_create(ab_request_p* req, int max_payload_size)
{
    int rc = PLCTAG_STATUS_OK;
    ab_request_p res;
    int request_capacity = EIP_CIP_PREFIX_SIZE + max_payload_size;

    res = (ab_request_p)rc_alloc(sizeof(struct ab_request_t) + request_capacity, request_destroy);

    if (!res) {
        *req = NULL;
        rc = PLCTAG_ERR_NO_MEM;
    } else {
        res->request_capacity = request_capacity;

        //res->rc = refcount_init(1, res, request_destroy);

        res->num_retries_left = 5; /* MAGIC */
        res->retry_interval = 900; /* MAGIC */

        *req = res;
    }

    return rc;
}



/*
 * request_destroy
 *
 * The request must be removed from any lists before this!
 */
//int request_destroy_unsafe(ab_request_p* req_pp)
void request_destroy(void *req_arg)
{
    (void)req_arg;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* nothing to do. */

    pdebug(DEBUG_DETAIL, "Done.");
}



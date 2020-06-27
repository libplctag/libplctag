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
#include <mb/modbus.h>
#include <mb/mb_common.h>
#include <mb/plc.h>
#include <mb/tag.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/rc.h>

/* Modbus module globals. */
mutex_p mb_mutex = NULL;

volatile int library_terminating = 0;



/****** main entry point *******/

plc_tag_p mb_tag_create(attr attribs)
{
    (void)attribs;

    pdebug(DEBUG_INFO, "Starting.");


    pdebug(DEBUG_INFO, "Done.");

    return NULL;
}



/****** Library level functions. *******/

void mb_teardown(void)
{
    pdebug(DEBUG_INFO, "Starting.");

    library_terminating = 1;

    pdebug(DEBUG_DETAIL, "Destroying Modbus mutex.");
    if(mb_mutex) {
        mutex_destroy(&mb_mutex);
        mb_mutex = NULL;
    }

    pdebug(DEBUG_INFO, "Done.");
}



int mb_init()
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    pdebug(DEBUG_DETAIL, "Setting up mutex.");
    if(!mb_mutex) {
        rc = mutex_create(&mb_mutex);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error %s creating mutex!", plc_tag_decode_error(rc));
            return rc;
        }
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


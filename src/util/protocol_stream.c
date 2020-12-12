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


#include <platform.h>
#include <lib/libplctag.h>
#include <util/debug.h>
#include <util/protocol_stream.h>

int protocol_stream_init(protocol_stream_p stream)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug (DEBUG_DETAIL, "Starting");

    rc = mutex_create(&(stream->mutex));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"Unable to initialize mutex!");
        return rc;
    }

    stream->callbacks = NULL;

    pdebug(DEBUG_DETAIL, "Done");

    return rc;
}

int protocol_stream_destroy(protocol_stream_p stream)
{
    int rc = PLCTAG_STATUS_OK;
    protocol_stream_callback_p callback = NULL;

    pdebug(DEBUG_DETAIL, "Starting");

    /* loop through the callbacks and call them. */
    do {
        critical_block(stream->mutex) {
            if(stream->callbacks) {
                callback = stream->callbacks;
                stream->callbacks = callback->next;
            }
        }

        if(callback) {
            callback->callback_handler(PROTOCOL_STREAM_CLOSING, callback->context, slice_make(NULL, 0));
        }

    } while(callback);

    stream->callbacks = NULL;

    /* notify the stream event handler that the stream is being destroyed. */
    stream->event_handler(PROTOCOL_STREAM_CLOSING, stream->context);

    rc = mutex_destroy(&(stream->mutex));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"Unable to destroy mutex!");
        return rc;
    }

    pdebug(DEBUG_DETAIL, "Done");

    return rc;
}

int protocol_stream_register_callback(protocol_stream_p stream, protocol_stream_callback_p callback)
{
    pdebug(DEBUG_SPEW, "Starting");

    if(!stream) {
        pdebug(DEBUG_WARN, "Called with null stream!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!callback) {
        pdebug(DEBUG_WARN, "Called with null callback!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* put the new callback at the end of the list. */
    critical_block(stream->mutex) {
        protocol_stream_callback_p *callback_walker = &(stream->callbacks);

        while(*callback_walker) {
            callback_walker = &((*callback_walker)->next);
        }

        *callback_walker = callback;
        callback->next = NULL;
    }

    stream->event_handler(PROTOCOL_STREAM_CALLBACK_ADDED, stream->context);

    pdebug(DEBUG_SPEW, "Done");

    return PLCTAG_STATUS_OK;
}


int protocol_stream_remove_callback(protocol_stream_p stream, protocol_stream_callback_p callback)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting");

    if(!stream) {
        pdebug(DEBUG_WARN, "Called with null stream!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!callback) {
        pdebug(DEBUG_WARN, "Called with null callback!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* Find the callback in the list. */
    critical_block(stream->mutex) {
        protocol_stream_callback_p *callback_walker = &(stream->callbacks);

        while(*callback_walker && *callback_walker != callback) {
            callback_walker = &((*callback_walker)->next);
        }

        if(*callback_walker == callback) {
            *callback_walker = callback->next;
            callback->next = NULL;
        } else {
            /* not found. */
            rc = PLCTAG_ERR_NOT_FOUND;
        }
    }

    stream->event_handler(PROTOCOL_STREAM_CALLBACK_REMOVED, stream->context);

    pdebug(DEBUG_SPEW, "Done");

    return rc;
}


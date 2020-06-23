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
#include <platform.h>
#include <mb/mb_common.h>
#include <mb/plc.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/rc.h>

#define PLC_CONNECT_ERR_DELAY (5000) /* wait five seconds. */


static mb_plc_p plcs = NULL;

static void mb_plc_destroy(void *plc_arg);


mb_plc_p find_or_create_plc(attr attribs)
{
    const char *host = attr_get_str(attribs, "server", NULL);
    int port = attr_get_int(attribs, "port", 502);
    mb_plc_p plc = NULL;
    int is_old_plc = 0;

    pdebug(DEBUG_INFO, "Starting.");

    if(!host || str_length(host)) {
        pdebug(DEBUG_WARN, "Modbus server cannot be empty or missing!");
        return NULL;
    }

    if(port < 0 || port > 65535) {
        pdebug(DEBUG_WARN, "Port must be a legal TCP port, between 0 and 65535!");
        return NULL;
    }

    /* try to find an existing PLC */
    critical_block(mb_mutex) {
        mb_plc_p *walker = &plcs;

        while(*walker && !plc) {
            if(str_cmp_i((*walker)->host, host) == 0 && (*walker)->port == (uint16_t)port) {
                pdebug(DEBUG_DETAIL, "Using existing PLC.");
                plc = rc_inc(*walker);
            }
        }

        if(!plc) {
            plc = (mb_plc_p)rc_alloc((int)(unsigned int)sizeof(struct mb_plc_t), mb_plc_destroy);
        }
    }

    if(is_old_plc) {
        /* found existing PLC. */
        return plc;
    }



    pdebug(DEBUG_INFO, "Done.");

    return NULL;
}


int plc_add_tag(mb_plc_p plc, mb_tag_p tag)
{
    pdebug(DEBUG_INFO, "Starting.");

    if(!plc) {
        pdebug(DEBUG_WARN, "Cannot add tag to NULL plc pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    critical_block(plc->mutex) {
        tag->next = plc->tags;
        plc->tags = tag;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}



int plc_remove_tag(mb_plc_p plc, mb_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    if(!plc) {
        pdebug(DEBUG_WARN, "Cannot add tag to NULL plc pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    critical_block(plc->mutex) {
        mb_tag_p *walker = &plc->tags;

        while(*walker && *walker != tag) {
            walker = &((*walker)->next);
        }

        if(*walker == tag) {
            *walker = tag->next;
        } else {
            rc = PLCTAG_ERR_NOT_FOUND;
        }
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


void mb_plc_destroy(void *plc_arg)
{
    mb_plc_p plc = (mb_plc_p)plc_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!plc) {
        pdebug(DEBUG_WARN, "PLC destructor called with NULL pointer!");
        return;
    }

    if(plc->tags) {
        pdebug(DEBUG_WARN, "PLC destructor called while it still has tags!");

        /* better to leak memory here than crash? */
        return;
    }

    /* stop the thread first. */
    plc->terminate = 1;
    if(plc->thread) {
        thread_join(plc->thread);
        thread_destroy(&plc->thread);
        plc->thread = NULL;
    }

    critical_block(mb_mutex) {
        /* remove the plc from the list. */
        mb_plc_p *walker = &plcs;

        while(*walker && *walker != plc) {
            walker = &((*walker)->next);
        }

        if(*walker == plc) {
            *walker = plc->next;
        } else {
            pdebug(DEBUG_WARN, "Destructor called on PLC not in the global list!");
        }
    }

    pdebug(DEBUG_DETAIL, "Destroying PLC mutex.");
    if(plc->mutex) {
        mutex_destroy(&(plc->mutex));
        plc->mutex = NULL;
    }

    pdebug(DEBUG_DETAIL, "Closing socket.");
    if(plc->sock) {
        socket_close(plc->sock);
        plc->sock = NULL;
    }

    pdebug(DEBUG_INFO, "Done.");
}



THREAD_FUNC(plc_handler)
{
    mb_plc_p plc = (mb_plc_p)arg;
    int rc = PLCTAG_STATUS_OK;
    int64_t error_delay = (int64_t)0;
    int work_to_do = 0;
    
    plc->flags = NOTHING_TO_DO;

    pdebug(DEBUG_INFO, "Starting.");

    while(! plc->terminate) {
        /* clear our flag for sleeping. */
        work_to_do = 0;

        /* first make sure we have a socket to read and write from. */
        if(error_delay < time_ms()) {
            if(!plc->sock) {
                int rc = socket_create(&plc->sock);
                if(rc == PLCTAG_STATUS_OK) {
                    rc = socket_connect_tcp(plc->sock, plc->host, plc->port);
                    if(rc != PLCTAG_STATUS_OK) {
                        pdebug(DEBUG_INFO, "Unable to connect to host %s on port %d!", plc->host, plc->port);
                        socket_destroy(&plc->sock);
                        plc->sock = NULL;
                        error_delay = time_ms() + PLC_CONNECT_ERR_DELAY;
                    }
                } else {
                    pdebug(DEBUG_WARN, "Unable to create socket, error %s!", plc_tag_decode_error(rc));
                    error_delay = time_ms() + PLC_CONNECT_ERR_DELAY;
                }
            } else {
                /* fake exceptions */
                do {
                    rc = read_modbus_packet(plc);
                    if(rc != PLCTAG_STATUS_OK) {
                        pdebug(DEBUG_INFO, "Unable to read socket, error %s!", plc_tag_decode_error(rc));
                        break;
                    }

                    if(plc->flags) {
                        work_to_do = 1;
                    }

                    /* loop over all tags tickling them to see what to do next. */
                    critical_block(plc->mutex) {
                        mb_tag_p new_tag_list = NULL;

                        /*
                        * We want to ensure some fairness.  Reverse the list order as we traverse
                        * it.   This is not perfect.   We may need to revisit this.
                        */
                        while(plc->tags) {
                            mb_tag_p tag = plc->tags;
                            plc->tags = tag->next;

                            plc->flags = tag->handler(tag, plc->flags, &plc->out_buf[0], sizeof(plc->out_buf), &plc->in_buf[0], sizeof(plc->in_buf));

                            /* put it on the new list. */
                            tag->next = new_tag_list;
                            new_tag_list = tag;                    
                        }

                        plc->tags = new_tag_list;
                    }

                    if(plc->flags) {
                        work_to_do = 1;
                    }

                    rc = write_modbus_packet(plc);
                    if(rc != PLCTAG_STATUS_OK) {
                        pdebug(DEBUG_INFO, "Unable to write socket, error %s!", plc_tag_decode_error(rc));
                        break;
                    }

                } while(0);

                if(rc != PLCTAG_STATUS_OK) {
                    socket_destroy(&plc->sock);
                    plc->sock = NULL;
                    error_delay = time_ms() + PLC_CONNECT_ERR_DELAY;
                }
            }
        }

        /* don't sleep if there is still something to do. */
        if(! work_to_do) {
            sleep_ms(1);
        }
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return NULL;
}

#define MODBUS_TCP_MBAP_SIZE (6)

int read_modbus_packet(mb_plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    int bytes_to_read = MODBUS_TCP_MBAP_SIZE;

    pdebug(DEBUG_SPEW, "Starting.");

    if(!plc) {
        pdebug(DEBUG_WARN, "Called with NULL plc pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!plc->sock) {
        pdebug(DEBUG_WARN, "Called with NULL socket pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* do we have enough bytes to determine the correct length? */
    if(plc->input_bytes_read >= MODBUS_TCP_MBAP_SIZE) {
        /* big endian! */
        uint16_t pdu_size = (uint16_t)(plc->in_buf[5]) + (((uint16_t)(plc->in_buf[4])) << (uint16_t)8);

        bytes_to_read = MODBUS_TCP_MBAP_SIZE + (int)(unsigned int)pdu_size;
    }

    if(bytes_to_read > (int)(unsigned int)sizeof(plc->in_buf)) {
        pdebug(DEBUG_WARN, "Packet larger than buffer!");
        return PLCTAG_ERR_TOO_LARGE;
    }

    space_remaining = (int)(unsigned int)sizeof(plc->in_buf) - plc->input_bytes_read;
    rc = socket_read(plc->sock, &(plc->in_buf[0]) + plc->input_bytes_read, space_remaining);
    if(rc < 0) {
        pdebug(DEBUG_WARN, "Error reading socket!");
        return rc;
    }




    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}
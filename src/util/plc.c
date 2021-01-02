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


#include <stdint.h>
#include <lib/libplctag.h>
#include <util/atomic_int.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/mem.h>
#include <util/mutex.h>
#include <util/plc.h>
#include <util/sleep.h>
#include <util/string.h>
#include <util/socket.h>
#include <util/time.h>


#define DESTROY_DISCONNECT_TIMEOUT_MS (500)

struct plc_layer_s {
    void *context;
    int (*reset)(void *context);
    int (*connect)(void *context, uint8_t *buffer, int buffer_capacity, int *buffer_offset);
    int (*disconnect)(void *context, uint8_t *buffer, int buffer_capacity, int *buffer_offset);
    int (*prepare_request)(void *context, uint8_t *buffer, int buffer_capacity, int *buffer_offset);
    int (*build_request)(void *context, uint8_t *buffer, int buffer_capacity, int *buffer_offset);
    int (*process_response)(void *context, uint8_t *buffer, int buffer_capacity, int *buffer_offset);
    int (*destroy_layer)(void *context);
};



struct plc_s {
    struct plc_s *next;

    const char *key;

    mutex_p plc_mutex;

    bool is_terminating;

    struct plc_request_s *request_list;
    bool request_in_flight;

    int num_layers;
    struct plc_layer_s *layers;

    bool connect_in_flight;
    bool disconnect_in_flight;
    int current_layer;

    const char *host;
    int port;
    sock_p socket;
    bool is_connected;

    uint8_t *data;
    int data_capacity;
    int data_size;
    int data_offset;
};


static mutex_p plc_list_mutex = NULL;
static plc_p plc_list = NULL;



static void plc_rc_destroy(void *plc_arg);
static int start_connecting(plc_p plc);
static void write_connect_request(sock_p sock, void *plc_arg);
static void process_connect_response(sock_p sock, void *plc_arg);
static int start_disconnecting(plc_p plc);
static void write_disconnect_request(sock_p sock, void *plc_arg);
static void process_disconnect_response(sock_p sock, void *plc_arg);

int plc_get(const char *plc_type, attr attribs, plc_p *plc, int (*constructor)(plc_p plc, attr attribs))
{
    int rc = PLCTAG_STATUS_OK;
    const char *gateway = NULL;
    const char *path = NULL;
    const char *plc_key = NULL;

    pdebug(DEBUG_INFO, "Starting for PLC type %s.", plc_type);

    /* build the search key. */
    gateway = attr_get_str(attribs, "gateway", NULL);
    if(!gateway || str_length(gateway) == 0) {
        pdebug(DEBUG_WARN, "Gateway host missing or zero length!");
        return PLCTAG_ERR_BAD_GATEWAY;
    }

    path = attr_get_str(attribs, "path", "NO_PATH");

    plc_key = str_concat(plc_type, "/", gateway, "/", path);
    if(!plc_key) {
        pdebug(DEBUG_WARN, "Unable to allocate plc_key!");
        return PLCTAG_ERR_NO_MEM;
    }

    critical_block(plc_list_mutex) {
        plc_p plc = NULL;

        /* try to find the PLC */
        for(plc=plc_list; plc; plc = plc->next) {
            if(str_cmp_i(plc_key, plc->key) != 0) {
                /* found it. */
                break;
            }
        }

        /* try to get a reference if we found one. */
        plc = rc_inc(plc);

        /* now check if we got a valid reference. */
        if(!plc) {
            /* need to make one. */
            plc = rc_alloc(sizeof(*plc), plc_rc_destroy);
            if(!plc) {
                pdebug(DEBUG_WARN, "Unable to allocate memory for new PLC %s!", plc_key);
                mem_free(plc_key);
                *plc = NULL;
                rc = PLCTAG_ERR_NO_MEM;
                break;
            }

            plc->key = plc_key;

            /* build the layers */
            rc = constructor(*plc, attribs);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unable to construct PLC %s layers, error %s!", plc_key, plc_tag_decode_error(rc));
                break;
            }
        }
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to get or create PLC!");
        return rc;
    }


    pdebug(DEBUG_INFO, "Done for PLC type %s.", plc_type);

    return rc;
}



int plc_init(plc_p plc, int num_layers)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    /* allocate the layer array. */
    plc->layers = mem_alloc(sizeof(struct plc_layer_s) * num_layers);
    if(!plc->layers) {
        pdebug(DEBUG_WARN, "Unable to allocate the layer array memory!");
        return PLCTAG_ERR_NO_MEM;
    }

    plc->num_layers = num_layers;

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return rc;
}


int plc_set_layer(plc_p plc,
                   int layer_index,
                   void *context,
                   int (*reset)(void *context),
                   int (*connect)(void *context, uint8_t *buffer, int buffer_capacity, int *buffer_offset),
                   int (*disconnect)(void *context, uint8_t *buffer, int buffer_capacity, int *buffer_offset),
                   int (*prepare_request)(void *context, uint8_t *buffer, int buffer_capacity, int *buffer_offset),
                   int (*build_request)(void *context, uint8_t *buffer, int buffer_capacity, int *buffer_offset),
                   int (*process_response)(void *context, uint8_t *buffer, int buffer_capacity, int *buffer_offset),
                   int (*destroy_layer)(void *context)
                  )
{
    pdebug(DEBUG_INFO, "Starting.");

    if(!plc) {
        pdebug(DEBUG_WARN, "Layer pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(layer_index < 0 || layer_index >= plc->num_layers) {
        pdebug(DEBUG_WARN, "Layer index out of bounds!");
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    plc->layers[layer_index].context = context;
    plc->layers[layer_index].reset = reset;
    plc->layers[layer_index].connect = connect;
    plc->layers[layer_index].disconnect = disconnect;
    plc->layers[layer_index].prepare_request = prepare_request;
    plc->layers[layer_index].build_request = build_request;
    plc->layers[layer_index].process_response = process_response;
    plc->layers[layer_index].destroy_layer = destroy_layer;

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}



int plc_get_idle_timeout(plc_p plc);
int plc_set_idle_timeout(plc_p plc, int timeout_ms);
int plc_get_buffer_size(plc_p plc);
int plc_set_buffer_size(plc_p plc, int buffer_size);
int plc_start_request(plc_p plc,
                             plc_request_p request,
                             void *client,
                             int (build_request_callback)(void *client, uint8_t *buffer, int buffer_capacity, int *buffer_offset),
                             int (*process_response_callback)(void *client, uint8_t *buffer, int buffer_capacity, int *buffer_offset));
int plc_stop_request(plc_p plc, plc_request_p request);



void plc_rc_destroy(void *plc_arg)
{
    int rc = PLCTAG_STATUS_OK;
    plc_p plc = (plc_p)plc_arg;

    pdebug(DEBUG_INFO, "Starting.");

    /* remove it from the list. */
    critical_block(plc_list_mutex) {
        plc_p *walker = &plc_list;

        while(*walker && *walker != plc) {
            walker = &((*walker)->next);
        }
    }

    if(atomic_bool_get(&(plc->is_connected)) == TRUE) {
        pdebug(DEBUG_INFO, "PLC is still connected.");

        /* kick off disconnect. */
        rc = start_disconnecting(plc);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while starting disconnect process!");
        } else {
            int64_t timeout = time_ms() + DESTROY_DISCONNECT_TIMEOUT_MS;
            while(atomic_bool_get(&(plc->is_connected)) == TRUE && timeout > time_ms()) {
                sleep_ms(10);
            }

            if(atomic_bool_get(&(plc->is_connected)) == TRUE) {
                pdebug(DEBUG_WARN, "PLC is still connected, closing socket anyway!");
            }
        }

        socket_close(plc->socket);

        socket_destroy(&(plc->socket));
        plc->socket = NULL;
    }

    rc = mutex_destroy(&(plc->request_mutex));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, destroying PLC mutex!", plc_tag_decode_error(rc));
    }
    plc->request_mutex = NULL;

    if(plc->request_list) {
        pdebug(DEBUG_WARN, "Request list is not empty!");
    }
    plc->request_list = NULL;

    if(plc->data) {
        mem_free(plc->data);
        plc->data = NULL;
    }

    if(plc->key) {
        mem_free(plc->key);
        plc->key = NULL;
    }

    pdebug(DEBUG_INFO, "Done.");
}


/* must be called from the mutex. */
int start_disconnecting(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting to disconnect PLC %s.", plc->key);

    if(!plc->is_connected) {
        pdebug(DEBUG_INFO, "PLC already disconnected.");
        return PLCTAG_STATUS_OK;
    }

    if(plc->disconnect_in_flight) {
        pdebug(DEBUG_INFO, "PLC disconnect already in flight.");
        return PLCTAG_STATUS_OK;
    }

    if(!plc->socket) {
        pdebug(DEBUG_WARN, "PLC has no socket!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* set up disconnect state. */
    plc->disconnect_in_flight = TRUE;
    plc->current_layer = plc->num_layers -1;

    if(!plc->request_in_flight) {
        /* nothing is happening, so kick off the disconnect now. */
        plc->data_size = 0;
        plc->data_offset = 0;

        rc = socket_callback_when_write_ready(plc->socket, write_disconnect_request, plc);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while setting up disconnect callback!", plc_tag_decode_error(rc));
            return rc;
        }
    } /* else when the request comes back, we will kick off the disconnect. */

    pdebug(DEBUG_INFO, "Done starting disconnect PLC %s.", plc->key);

    return rc;
}


int build_disconnect_request(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting for plc %s.", plc->key);

    do {
        int bytes_written = 0;

        /* prepare the layers from lowest to highest.  This reserves space in the buffer. */
        for(int index = 0; index < plc->current_layer && index >= 0 && rc == PLCTAG_STATUS_OK; index++) {
            pdebug(DEBUG_DETAIL, "Preparing layer %d for disconnect.", index);
            rc = plc->layers[index].prepare_request(plc->layers[index].context, plc->data, plc->data_capacity, &(plc->data_size));
        }

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while preparing layers!");
            break;
        }

        /* set up the top level disconnect packet. */
        rc = plc->layers[plc->current_layer].disconnect(plc->layers[plc->current_layer].context, plc->data, plc->data_capacity, &(plc->data_size));
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while preparing layer %d for disconnect!", plc_tag_decode_error(rc), plc->current_layer);
            break;
        }

        /* backfill the layers of the packet.  Top to bottom. */
        for(int index = plc->current_layer-1; index >= 0; index--) {
            pdebug(DEBUG_DETAIL, "Building layer %d for disconnect.", index);
            rc = plc->layers[index].build_request(plc->layers[index].context, plc->data, plc->data_capacity, &(plc->data_size));
        }

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while building layers!");
            break;
        }
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error trying to build disconnect packet!");

        /* TODO - figure out what to do here to clean up. */

        /* punt */
        plc->data_size = 0;
        plc->data_offset = 0;
        plc->current_layer = plc->num_layers;
    }

    pdebug(DEBUG_INFO, "Starting for plc %s.", plc->key);

    return rc;
}





void write_disconnect_request(sock_p socket, void *plc_arg)
{
    int rc = PLCTAG_STATUS_OK;
    plc_p plc = (plc_p)plc_arg;

    pdebug(DEBUG_INFO, "Starting for plc %s.", plc->key);

    critical_block(plc->plc_mutex) {
        bool done = TRUE;

        /* multiple things to check that can change. */
        do {
            done = TRUE;

            do {
                /* are we done going through the layers? */
                if(plc->current_layer < 0) {
                    /* we are done. */
                    pdebug(DEBUG_DETAIL, "Finished disconnecting layers, now close socket.");
                    break;
                }

                /* does the current layer support disconnect? */
                if(!plc->layers[plc->current_layer].disconnect) {
                    /* no it does not, skip to the next layer. */
                    plc->current_layer--;
                    done = FALSE;
                    break;
                }

                /* do we have a packet to send? */
                if(plc->data_size == 0) {
                    /* no we do not. make one. */
                    rc = build_disconnect_request(plc);
                    if(rc != PLCTAG_STATUS_OK) {
                        pdebug(DEBUG_WARN, "Error, %s, building disconnect request for layer %d!", plc_tag_decode_error(rc), plc->current_layer);
                        break;
                    }
                }

                /* do we have data to send? */
                if(plc->data_offset < plc->data_size) {
                    int bytes_written = 0;

                    /* We have data to write. */
                    bytes_written = socket_tcp_write(plc->socket, plc->data + plc->data_offset, plc->data_size - plc->data_offset);
                    if(bytes_written < 0) {
                        /* error sending data! */
                        rc = bytes_written;

                        pdebug(DEBUG_WARN, "Error, %s, sending data!", plc_tag_decode_error(rc));
                        break;
                    }

                    plc->data_offset += bytes_written;

                    /* did we write all the data? */
                    if(plc->data_offset < plc->data_size) {
                        /* no, so queue up another write. */
                        rc = socket_callback_when_write_ready(plc->socket, write_disconnect_request, plc);
                        if(rc != PLCTAG_STATUS_OK) {
                            pdebug(DEBUG_WARN, "Error, %s, setting up callback for socket write!", plc_tag_decode_error(rc));
                            break;
                        }
                    }
                }

                /* did we write it all? */
                if(plc->data_offset >= plc->data_size) {
                    /* yes, so wait for the response. */
                    plc->data_offset = 0;
                    plc->data_size = 0;

                    rc = socket_callback_when_read_ready(plc->socket, process_disconnect_response, plc);
                    if(rc != PLCTAG_STATUS_OK) {
                        pdebug(DEBUG_WARN, "Error, %s, setting up callback for socket write!", plc_tag_decode_error(rc));
                        break;
                    }
                }
            } while(0);
        } while(!done);

        if(rc != PLCTAG_STATUS_OK || plc->current_layer >= plc->num_layers) {
            /* either we errored out or we walked through all layers. */

            /* final "layer" which is the socket. */
            if(plc->socket) {
                socket_close(plc->socket);
            }

            /* reset the layers */
            for(int index = 0; index < plc->num_layers; index++) {
                rc = plc->layers[index].reset(plc->layers[index].context);
            }

            plc->disconnect_in_flight = FALSE;
            plc->current_layer = -1;

            /* if we are not terminating, then see if we need to retrigger the connection process. */
            if(!plc->is_terminating) {
                if(plc->request_list) {
                    start_connecting(plc);
                }
            }
        }
    }

    pdebug(DEBUG_INFO, "Done for plc %s.", plc->key);
}


void process_disconnect_response(sock_p socket, void *plc_arg)
{
    int rc = PLCTAG_STATUS_OK;
    plc_p plc = (plc_p)plc_arg;

    pdebug(DEBUG_INFO, "Starting for plc %s.", plc->key);

    critical_block(plc->plc_mutex) {
        int bytes_read = 0;
        /* we have data to read. */
        bytes_read = socket_tcp_read(plc->socket, plc->data + plc->data_size, plc->data_capacity - plc->data_size);
        if(bytes_read < 0) {
            rc = bytes_read;
            pdebug(DEBUG_WARN, "Error, %s, while trying to read socket!", plc_tag_decode_error(rc));
            break;
        }

        plc->data_size += bytes_read;

        /* do we have enough data? */
        for(int index = 0; index <= plc->current_layer && rc == PLCTAG_STATUS_OK; index++) {
            rc = plc->layers[index].process_response(plc->layers[index].context, plc->data, plc->data_size, &(plc->data_offset));
        }

        if(rc == PLCTAG_ERR_PARTIAL) {
            /* not an error, we just need more data. */
            rc = socket_callback_when_read_ready(plc->socket, process_disconnect_response, plc);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error, %s, while setting up read callback!", plc_tag_decode_error(rc));
            }

            /* we are not done, so bail out of the critical block. */
            break;
        } else if(rc != PLCTAG_STATUS_OK) {
            /* some other error.  Punt. */
            pdebug(DEBUG_WARN, "Error, %s, while trying to process response!", plc_tag_decode_error(rc));
            break;
        }

        /* we are all good. Do the next layer. */
        plc->current_layer--;

        plc->data_size = 0;
        plc->data_offset = 0;

        rc = socket_callback_when_write_ready(plc->socket, write_disconnect_request, plc);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while setting up write callback!", plc_tag_decode_error(rc));
            break;
        }
    }

    if(rc != PLCTAG_STATUS_OK) {
        /* force a shutdown. */

        pdebug(DEBUG_WARN, "Error trying to process disconnect response.  Force reset the stack.");

        if(plc->socket) {
            socket_close(plc->socket);
        }

        /* reset the layers */
        for(int index = 0; index < plc->num_layers; index++) {
            rc = plc->layers[index].reset(plc->layers[index].context);
        }

        plc->disconnect_in_flight = FALSE;
        plc->current_layer = 0;

        /* if we are not terminating, then see if we need to retrigger the connection process. */
        if(!plc->is_terminating) {
            if(plc->request_list) {
                start_connecting(plc);
            }
        }

        return;
    }

    pdebug(DEBUG_INFO, "Done for plc %s.", plc->key);
}




/* must be called in the mutex. */
int start_connecting(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting to connect PLC %s.", plc->key);

    if(plc->is_connected) {
        pdebug(DEBUG_INFO, "PLC already connected.");
        return PLCTAG_STATUS_OK;
    }

    /* if we have a connect and a disconnect at the same time, the disconnect wins. */
    if(plc->disconnect_in_flight) {
        pdebug(DEBUG_INFO, "PLC disconnect in flight.");
        return PLCTAG_STATUS_OK;
    }

    if(!plc->socket) {
        pdebug(DEBUG_WARN, "PLC has no socket!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* set up connect state. */
    plc->connect_in_flight = TRUE;
    plc->current_layer = 0;
    plc->data_size = 0;
    plc->data_offset = 0;

    /* open socket */

    /* TODO - spawn a thread and do this in parallel. */

    rc = socket_tcp_connect(plc->socket, plc->host, plc->port);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, while trying to connect TCP socket!", plc_tag_decode_error(rc));
        return rc;
    }

    /* start setting up the connections of the layers. */
    rc = socket_callback_when_write_ready(plc->socket, write_connect_request, plc);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, while setting up disconnect callback!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_INFO, "Done starting disconnect PLC %s.", plc->key);

    return rc;
}




void write_connect_request(sock_p socket, void *plc_arg)
{
    int rc = PLCTAG_STATUS_OK;
    plc_p plc = (plc_p)plc_arg;

    pdebug(DEBUG_INFO, "Starting for plc %s.", plc->key);

    critical_block(plc->plc_mutex) {
        bool done = TRUE;

        /* multiple things to check that can change. */
        do {
            done = TRUE;

            do {
                /* are we done going through the layers? */
                if(plc->current_layer >= plc->num_layers) {
                    /* we are done. */
                    pdebug(DEBUG_DETAIL, "Finished connecting layers.");
                    break;
                }

                /* does the current layer support connect? */
                if(!plc->layers[plc->current_layer].connect) {
                    /* no it does not, skip to the next layer. */
                    plc->current_layer++;
                    done = FALSE;
                    break;
                }

                /* do we have a packet to send? */
                if(plc->data_size == 0) {
                    /* no we do not. make one. */
                    rc = build_connect_request(plc);
                    if(rc != PLCTAG_STATUS_OK) {
                        pdebug(DEBUG_WARN, "Error, %s, building connect request for layer %d!", plc_tag_decode_error(rc), plc->current_layer);
                        break;
                    }
                }

                /* do we have data to send? */
                if(plc->data_offset < plc->data_size) {
                    int bytes_written = 0;

                    /* We have data to write. */
                    bytes_written = socket_tcp_write(plc->socket, plc->data + plc->data_offset, plc->data_size - plc->data_offset);
                    if(bytes_written < 0) {
                        /* error sending data! */
                        rc = bytes_written;

                        pdebug(DEBUG_WARN, "Error, %s, sending data!", plc_tag_decode_error(rc));
                        break;
                    }

                    plc->data_offset += bytes_written;

                    /* did we write all the data? */
                    if(plc->data_offset < plc->data_size) {
                        /* no, so queue up another write. */
                        rc = socket_callback_when_write_ready(plc->socket, write_connect_request, plc);
                        if(rc != PLCTAG_STATUS_OK) {
                            pdebug(DEBUG_WARN, "Error, %s, setting up callback for socket write!", plc_tag_decode_error(rc));
                            break;
                        }
                    }
                }

                /* did we write it all? */
                if(plc->data_offset >= plc->data_size) {
                    /* yes, so wait for the response. */
                    plc->data_offset = 0;
                    plc->data_size = 0;

                    rc = socket_callback_when_read_ready(plc->socket, process_connect_response, plc);
                    if(rc != PLCTAG_STATUS_OK) {
                        pdebug(DEBUG_WARN, "Error, %s, setting up callback for socket write!", plc_tag_decode_error(rc));
                        break;
                    }
                }
            } while(0);
        } while(!done);

        if(rc != PLCTAG_STATUS_OK) {
            /* either we errored out or we walked through all layers. */

            /* final "layer" which is the socket. */
            if(plc->socket) {
                socket_close(plc->socket);
            }

            /* reset the layers */
            for(int index = 0; index < plc->num_layers; index++) {
                rc = plc->layers[index].reset(plc->layers[index].context);
            }

            plc->disconnect_in_flight = FALSE;
            plc->current_layer = -1;

            /* if we are not terminating, then see if we need to retrigger the connection process. */
            if(!plc->is_terminating) {
                if(plc->request_list) {
                    start_connecting(plc);
                }
            }
        }
    }

    pdebug(DEBUG_INFO, "Done for plc %s.", plc->key);
}




int build_connect_request(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting for plc %s.", plc->key);

    do {
        int bytes_written = 0;

        /* prepare the layers from lowest to highest.  This reserves space in the buffer. */
        for(int index = 0; index < plc->current_layer && index >= 0 && rc == PLCTAG_STATUS_OK; index++) {
            pdebug(DEBUG_DETAIL, "Preparing layer %d for connect.", index);
            rc = plc->layers[index].prepare_request(plc->layers[index].context, plc->data, plc->data_capacity, &(plc->data_size));
        }

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while preparing layers!");
            break;
        }

        /* set up the top level connect packet. */
        rc = plc->layers[plc->current_layer].connect(plc->layers[plc->current_layer].context, plc->data, plc->data_capacity, &(plc->data_size));
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while preparing layer %d for connect!", plc_tag_decode_error(rc), plc->current_layer);
            break;
        }

        /* backfill the layers of the packet.  Top to bottom. */
        for(int index = plc->current_layer-1; index >= 0; index--) {
            pdebug(DEBUG_DETAIL, "Building layer %d for connect.", index);
            rc = plc->layers[index].build_request(plc->layers[index].context, plc->data, plc->data_capacity, &(plc->data_size));
        }

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while building layers!");
            break;
        }
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error trying to build disconnect packet!");

        /* TODO - figure out what to do here to clean up. */

        /* punt */
        plc->data_size = 0;
        plc->data_offset = 0;
        plc->current_layer = plc->num_layers;
    }

    pdebug(DEBUG_INFO, "Starting for plc %s.", plc->key);

    return rc;
}



void process_connect_response(sock_p socket, void *plc_arg)
{
    int rc = PLCTAG_STATUS_OK;
    plc_p plc = (plc_p)plc_arg;

    pdebug(DEBUG_INFO, "Starting for plc %s.", plc->key);

    critical_block(plc->plc_mutex) {
        int bytes_read = 0;
        /* we have data to read. */
        bytes_read = socket_tcp_read(plc->socket, plc->data + plc->data_size, plc->data_capacity - plc->data_size);
        if(bytes_read < 0) {
            rc = bytes_read;
            pdebug(DEBUG_WARN, "Error, %s, while trying to read socket!", plc_tag_decode_error(rc));
            break;
        }

        plc->data_size += bytes_read;

        /* do we have enough data? */
        for(int index = 0; index <= plc->current_layer && rc == PLCTAG_STATUS_OK; index++) {
            rc = plc->layers[index].process_response(plc->layers[index].context, plc->data, plc->data_size, &(plc->data_offset));
        }

        if(rc == PLCTAG_ERR_PARTIAL) {
            /* not an error, we just need more data. */
            rc = socket_callback_when_read_ready(plc->socket, process_connect_response, plc);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error, %s, while setting up read callback!", plc_tag_decode_error(rc));
            }

            /* we are not done, so bail out of the critical block. */
            break;
        } else if(rc != PLCTAG_STATUS_OK) {
            /* some other error.  Punt. */
            pdebug(DEBUG_WARN, "Error, %s, while trying to process response!", plc_tag_decode_error(rc));
            break;
        }

        /* we are all good. Do the next layer. */
        plc->current_layer--;

        plc->data_size = 0;
        plc->data_offset = 0;

        rc = socket_callback_when_write_ready(plc->socket, write_connect_request, plc);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while setting up write callback!", plc_tag_decode_error(rc));
            break;
        }
    }

    if(rc != PLCTAG_STATUS_OK) {
        /* force a shutdown. */

        pdebug(DEBUG_WARN, "Error trying to process connect response.  Force reset the stack.");

        if(plc->socket) {
            socket_close(plc->socket);
        }

        /* reset the layers */
        for(int index = 0; index < plc->num_layers; index++) {
            rc = plc->layers[index].reset(plc->layers[index].context);
        }

        plc->connect_in_flight = FALSE;
        plc->current_layer = 0;

        /* if we are not terminating, then see if we need to retrigger the connection process. */
        if(!plc->is_terminating) {
            if(plc->request_list) {

                /* TODO - add error backoff */

                start_connecting(plc);
            }
        }

        return;
    }

    pdebug(DEBUG_INFO, "Done for plc %s.", plc->key);
}






int plc_module_init(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    plc_list = NULL;

    rc = mutex_create(&plc_list_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, creating plc list mutex!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


void plc_module_teardown(void)
{
    pdebug(DEBUG_INFO, "Starting.");

    if(plc_list != NULL) {
        pdebug(DEBUG_WARN, "PLC list not empty!");
    }

    plc_list = NULL;

    rc = mutex_destroy(&plc_list_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, destroying plc list mutex!", plc_tag_decode_error(rc));
        return;
    }

    pdebug(DEBUG_INFO, "Done.");
}

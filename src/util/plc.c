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


#include <inttypes.h>
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
    int (*initialize)(void *context);
    int (*connect)(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end);
    int (*disconnect)(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end);
    int (*prepare_for_request)(void *context);
    int (*build_request)(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_num);
    int (*prepare_for_response)(void *context);
    int (*process_response)(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_num);
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

    int idle_timeout_ms;

    uint8_t *data;
    int data_capacity;
    int data_size;
    int data_offset;

    /* model-specific data. */
    void *context;
    void (*context_destructor)(plc_p plc, void *context);
};


static mutex_p plc_list_mutex = NULL;
static plc_p plc_list = NULL;



static void plc_rc_destroy(void *plc_arg);
static void dispatch_plc_request_unsafe(plc_p plc);
static int start_connecting_unsafe(plc_p plc);
static void write_connect_request(sock_p sock, void *plc_arg);
static void process_connect_response(sock_p sock, void *plc_arg);
static int start_disconnecting_unsafe(plc_p plc);
static void write_disconnect_request(sock_p sock, void *plc_arg);
static void process_disconnect_response(sock_p sock, void *plc_arg);
static void build_plc_request_callback(sock_p sock, void *plc_arg);
static void write_plc_request_callback(sock_p sock, void *plc_arg);

int plc_get(const char *plc_type, attr attribs, plc_p *plc_return, int (*constructor)(plc_p plc, attr attribs))
{
    int rc = PLCTAG_STATUS_OK;
    const char *gateway = NULL;
    const char *path = NULL;
    const char *plc_key = NULL;

    pdebug(DEBUG_INFO, "Starting for PLC type %s.", plc_type);

    *plc_return = NULL;

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
            const char *host_args = NULL;
            char **host_segments = NULL;
            int port = 0;

            /* need to make one. */
            plc = rc_alloc(sizeof(*plc), plc_rc_destroy);
            if(!plc) {
                pdebug(DEBUG_WARN, "Unable to allocate memory for new PLC %s!", plc_key);
                mem_free(plc_key);
                plc = NULL;
                rc = PLCTAG_ERR_NO_MEM;
                break;
            }

            plc->key = plc_key;
            plc_key = NULL;

            /* build the layers */
            rc = constructor(plc, attribs);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unable to construct PLC %s layers, error %s!", plc_key, plc_tag_decode_error(rc));
                rc_dec(plc);
                break;
            }

            /* handle the host and port. Do this after the plc-specific constructor ran as it will set a default port. */
            host_args = attr_get_str(attribs, "gateway", NULL);
            port = attr_get_int(attribs, "default_port", 0);

            if(!host_args || str_length(host_args) == 0) {
                pdebug(DEBUG_WARN, "Host/gateway not provided!");
                rc_dec(plc);
                rc = PLCTAG_ERR_BAD_GATEWAY;
                break;
            }

            host_segments = str_split(host_args, ":");

            if(!host_segments) {
                pdebug(DEBUG_WARN, "Unable to split gateway string!");
                rc_dec(plc);
                rc = PLCTAG_ERR_BAD_GATEWAY;
                break;
            }

            if(!host_segments[0] || str_length(host_segments[0]) == 0) {
                pdebug(DEBUG_WARN, "Host/gateway not provided!");
                mem_free(host_segments);
                rc_dec(plc);
                return PLCTAG_ERR_BAD_GATEWAY;
            }

            plc->host = str_dup(host_segments[0]);

            if(host_segments[1] && str_length(host_segments[1])) {
                rc = str_to_int(host_segments[1], &port);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Unable to parse port number, error %s!", plc_tag_decode_error(rc));
                    mem_free(host_segments);
                    rc_dec(plc);
                    rc = PLCTAG_ERR_BAD_GATEWAY;
                    break;
                }

                if(port <= 0 || port > 65535) {
                    pdebug(DEBUG_WARN, "Port value (%d) must be between 0 and 65535!", port);
                    mem_free(host_segments);
                    rc_dec(plc);
                    rc = PLCTAG_ERR_BAD_GATEWAY;
                    break;
                }
            }

            mem_free(host_segments);

            plc->port = port;
        }

        *plc_return = plc;
    }

    mem_free(plc_key);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to get or create PLC!");
        return rc;
    }

    pdebug(DEBUG_INFO, "Done for PLC type %s.", plc_type);

    return rc;
}



int plc_init(plc_p plc, int num_layers, void *context, void (*context_destructor)(plc_p plc, void *context))
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    /* allocate the layer array. */
    plc->layers = mem_alloc((int)(unsigned int)sizeof(struct plc_layer_s) * num_layers);
    if(!plc->layers) {
        pdebug(DEBUG_WARN, "Unable to allocate the layer array memory!");
        return PLCTAG_ERR_NO_MEM;
    }

    plc->num_layers = num_layers;
    plc->context = context;
    plc->context_destructor = context_destructor;

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return rc;
}



int plc_set_layer(plc_p plc,
                  int layer_index,
                  void *context,
                  int (*initialize)(void *context),
                  int (*connect)(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end),
                  int (*disconnect)(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end),
                  int (*prepare_for_request)(void *context),
                  int (*build_request)(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_num),
                  int (*prepare_for_response)(void *context),
                  int (*process_response)(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_num),
                  int (*destroy_layer)(void *context)
                 )
{
    pdebug(DEBUG_INFO, "Starting.");

    if(!plc) {
        pdebug(DEBUG_WARN, "PLC pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(layer_index < 0 || layer_index >= plc->num_layers) {
        pdebug(DEBUG_WARN, "Layer index out of bounds!");
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    plc->layers[layer_index].context = context;
    plc->layers[layer_index].initialize = initialize;
    plc->layers[layer_index].connect = connect;
    plc->layers[layer_index].disconnect = disconnect;
    plc->layers[layer_index].prepare_for_request = prepare_for_request;
    plc->layers[layer_index].build_request = build_request;
    plc->layers[layer_index].prepare_for_response = prepare_for_response;
    plc->layers[layer_index].process_response = process_response;
    plc->layers[layer_index].destroy_layer = destroy_layer;

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


void *plc_get_context(plc_p plc)
{
    void *res = NULL;

    if(!plc) {
        pdebug(DEBUG_WARN, "PLC pointer is null!");
        return NULL;
    }

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    critical_block(plc->plc_mutex) {
        res = plc->context;
    }

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return res;
}


int plc_get_idle_timeout(plc_p plc)
{
    int res = 0;

    if(!plc) {
        pdebug(DEBUG_WARN, "PLC pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    critical_block(plc->plc_mutex) {
        res = plc->idle_timeout_ms;
    }

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return res;
}



int plc_set_idle_timeout(plc_p plc, int timeout_ms)
{
    int res = 0;

    if(!plc) {
        pdebug(DEBUG_WARN, "PLC pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(timeout_ms < 0 || timeout_ms > 5000) {
        pdebug(DEBUG_WARN, "Illegal timeout value %d!", timeout_ms);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    critical_block(plc->plc_mutex) {
        plc->idle_timeout_ms = timeout_ms;
    }

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return PLCTAG_STATUS_OK;
}



int plc_get_buffer_size(plc_p plc)
{
    int res = 0;

    if(!plc) {
        pdebug(DEBUG_WARN, "PLC pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    critical_block(plc->plc_mutex) {
        res = plc->data_size;
    }

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return res;
}



int plc_set_buffer_size(plc_p plc, int buffer_size)
{
    int rc = 0;

    if(!plc) {
        pdebug(DEBUG_WARN, "PLC pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(buffer_size <= 0 || buffer_size > 65536) {
        pdebug(DEBUG_WARN, "Illegal buffer size value %d!", buffer_size);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    critical_block(plc->plc_mutex) {
        plc->data = mem_realloc(plc->data, buffer_size);
        if(!plc->data) {
            pdebug(DEBUG_WARN, "Unable to reallocate memory for data buffer!");
            rc = PLCTAG_ERR_NO_MEM;
            break;
        }

        plc->data_capacity = buffer_size;
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error setting buffer size!");
        return rc;
    }

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return PLCTAG_STATUS_OK;
}




int plc_start_request(plc_p plc,
                      plc_request_p request,
                      void *client,
                      int (*build_request_callback)(void *client, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id req_num),
                      int (*process_response_callback)(void *client, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id req_num))
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    critical_block(plc->plc_mutex) {
        /* is the request already on the list?  If not, add it at the end. */
        plc_request_p *walker = &(plc->request_list);

        while(*walker && *walker != request) {
            walker = &((*walker)->next);
        }

        if(*walker) {
            /* we found the request, already busy. */
            pdebug(DEBUG_WARN, "Request is already queued!");
            rc = PLCTAG_ERR_BUSY;
            break;
        }

        /* did not find, it, so add it and continue processing. */
        request->next = NULL;
        request->req_id = -1;
        request->context = client;
        request->build_request = build_request_callback;
        request->process_response = process_response_callback;

        *walker = request;

        /* should we start up the packet sending cycle? */
        if(plc->is_connected) {
            if(!plc->request_in_flight) {
                plc->request_in_flight = TRUE;
                rc = socket_callback_when_write_ready(plc->socket, build_plc_request_callback, plc);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Error, %s, attempting to start socket read ready callback!", plc_tag_decode_error(rc));
                    break;
                }
            }
        } else {
            rc = start_connecting_unsafe(plc);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error, %s, trying to start connecting to the PLC!", plc_tag_decode_error(rc));
                break;
            }
        }
    }

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return PLCTAG_STATUS_OK;
}





int plc_stop_request(plc_p plc, plc_request_p request)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    critical_block(plc->plc_mutex) {
        /* is the request already on the list?  If not, add it at the end. */
        plc_request_p *walker = &(plc->request_list);

        while(*walker && *walker != request) {
            walker = &((*walker)->next);
        }

        if(*walker) {
            *walker = request->next;
        } else {
            pdebug(DEBUG_WARN, "Request not on the PLC's list.");
        }
    }

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return PLCTAG_STATUS_OK;
}



void plc_rc_destroy(void *plc_arg)
{
    int rc = PLCTAG_STATUS_OK;
    plc_p plc = (plc_p)plc_arg;
    int64_t timeout = 0;
    bool is_connected = TRUE;

    pdebug(DEBUG_INFO, "Starting.");

    /* remove it from the list. */
    critical_block(plc_list_mutex) {
        plc_p *walker = &plc_list;

        while(*walker && *walker != plc) {
            walker = &((*walker)->next);
        }

        if(*walker == plc) {
            *walker = plc->next;
            plc->next = NULL;
        } else {
            pdebug(DEBUG_WARN, "PLC not found on the list!");
        }
    }

    /* lock the PLC mutex for some of these operations. */
    critical_block(plc->plc_mutex) {
        plc->is_terminating = TRUE;

        if(plc->is_connected) {
            is_connected = TRUE;
            start_disconnecting_unsafe(plc);
        }
    }

    /* loop until we timeout or we are disconnected. */
    timeout = time_ms() + DESTROY_DISCONNECT_TIMEOUT_MS;

    while(is_connected && timeout > time_ms()) {
        critical_block(plc->plc_mutex) {
            is_connected = plc->is_connected;
        }

        if(is_connected) {
            sleep_ms(10);
        }
    }

    /* if there is local context, destroy it. */
    if(plc->context) {
        if(plc->context_destructor) {
            plc->context_destructor(plc, plc->context);
        } else {
            mem_free(plc->context);
        }

        plc->context = NULL;
    }

    critical_block(plc->plc_mutex) {
        /* regardless of why we are past the timeout loop, we close everything up. */
        socket_close(plc->socket);
    }

    socket_destroy(&(plc->socket));
    plc->socket = NULL;

    rc = mutex_destroy(&(plc->plc_mutex));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, destroying PLC mutex!", plc_tag_decode_error(rc));
    }
    plc->plc_mutex = NULL;

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


/* must be called within the PLC's mutex. */
void dispatch_plc_request_unsafe(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting for PLC %s.", plc->key);

    if(!plc->request_list) {
        pdebug(DEBUG_DETAIL, "PLC has no outstanding requests.");
        return;
    }

    if(plc->is_terminating) {
        pdebug(DEBUG_DETAIL, "PLC is terminating.");
        return;
    }

    if(plc->disconnect_in_flight) {
        pdebug(DEBUG_DETAIL, "Disconnect is in progress.");
        return;
    }

    if(plc->request_in_flight) {
        pdebug(DEBUG_DETAIL, "Request is in flight.");
        return;
    }

    if(plc->connect_in_flight) {
        pdebug(DEBUG_DETAIL, "Connection is in progress.");
        return;
    }

    if(plc->is_connected) {
        /* kick off a request. */
        rc = socket_callback_when_write_ready(plc->socket, build_plc_request_callback, plc);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to register callback with socket, error %s!", plc_tag_decode_error(rc));

            /* teardown the connection. */
            start_disconnecting_unsafe(plc);
            return;
        }
    } else {
        pdebug(DEBUG_DETAIL, "Starting PLC connect process.");
        start_connecting_unsafe(plc);
    }

    pdebug(DEBUG_DETAIL, "Done for PLC %s.", plc->key);
}


/* must be called from the mutex. */
int start_disconnecting_unsafe(plc_p plc)
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
    plc->current_layer = 0;

    /* kill any request in flight */
    if(plc->request_in_flight) {
        rc = socket_callback_when_read_ready(plc->socket, NULL, NULL);
        rc = socket_callback_when_write_ready(plc->socket, NULL, NULL);

        plc->request_in_flight = FALSE;
    }

    plc->data_size = 0;
    plc->data_offset = 0;

    /* ready the layers to write a request. */
    for(int index = 0; index < plc->num_layers; index++) {
        rc = plc->layers[index].prepare_for_request(plc->layers[index].context);
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, setting up layers for response!", plc_tag_decode_error(rc));
    }

    rc = socket_callback_when_write_ready(plc->socket, write_disconnect_request, plc);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, while setting up disconnect callback!", plc_tag_decode_error(rc));

        /* TODO - force close the socket here.   Everything is broken.  Start over. */

        return rc;
    }

    pdebug(DEBUG_INFO, "Done starting disconnect PLC %s.", plc->key);

    return rc;
}




void write_disconnect_request(sock_p socket, void *plc_arg)
{
    int rc = PLCTAG_STATUS_OK;
    plc_p plc = (plc_p)plc_arg;

    pdebug(DEBUG_INFO, "Starting for plc %s.", plc->key);

    critical_block(plc->plc_mutex) {
        do {
            /* does the current layer support disconnect? */
            while(plc->current_layer < plc->num_layers && !plc->layers[plc->current_layer].disconnect) {
                /* no it does not, skip to the next layer. */
                plc->current_layer++;
            }

            /* are we done going through the layers? */
            if(plc->current_layer >= plc->num_layers) {
                /* we are done. */
                pdebug(DEBUG_DETAIL, "Finished disconnecting layers.");
                break;
            }

            /* do we have a packet to send? */
            if(plc->data_size == 0) {
                /* no we do not. make one. */
                rc = build_disconnect_request_unsafe(plc);
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

        if(rc != PLCTAG_STATUS_OK || plc->current_layer >= plc->num_layers) {
            /* either we errored out or we walked through all layers. */

            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error, %s, trying to build disconnect request!", plc_tag_decode_error(rc));
            }

            /* final "layer" which is the socket. */
            if(plc->socket) {
                socket_close(plc->socket);
            }

            /* reset the layers */
            for(int index = 0; index < plc->num_layers; index++) {
                rc = plc->layers[index].initialize(plc->layers[index].context);
            }

            plc->disconnect_in_flight = FALSE;
            plc->current_layer = 0;

            dispatch_plc_request_unsafe(plc);
        }
    }

    pdebug(DEBUG_INFO, "Done for plc %s.", plc->key);
}


int build_disconnect_request_unsafe(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    int data_start = 0;
    int data_end = 0;
    plc_request_id req_id = INVALID_REQUEST_ID;

    pdebug(DEBUG_INFO, "Starting for plc %s.", plc->key);

    do {
        int bytes_written = 0;

        /* prepare the layers from lowest to highest.  This reserves space in the buffer. */
        for(int index = 0; index < plc->current_layer && index >= 0 && rc == PLCTAG_STATUS_OK; index++) {
            pdebug(DEBUG_DETAIL, "Preparing layer %d for disconnect.", index);
            rc = plc->layers[index].build_request(plc->layers[index].context, plc->data, plc->data_capacity, &data_start, &data_end, &req_id);
        }

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while preparing layers!");
            break;
        }

        /* set up the top level disconnect packet. */
        rc = plc->layers[plc->current_layer].disconnect(plc->layers[plc->current_layer].context, plc->data, plc->data_capacity, &data_start, &data_end);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while preparing layer %d for disconnect!", plc_tag_decode_error(rc), plc->current_layer);
            break;
        }

        /* backfill the layers of the packet.  Top to bottom. */
        for(int index = plc->current_layer-1; index >= 0; index--) {
            pdebug(DEBUG_DETAIL, "Building layer %d for disconnect.", index);
            rc = plc->layers[index].build_request(plc->layers[index].context, plc->data, plc->data_capacity, &data_start, &data_end, &req_id);
        }

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while building layers!");
            break;
        }

        pdebug(DEBUG_INFO, "Layer %d disconnect packet:", plc->current_layer);
        pdebug_dump_bytes(DEBUG_INFO, plc->data, data_end);

        plc->data_size = data_end;
        plc->data_offset = 0;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error trying to build disconnect packet!");

        /* TODO - figure out what to do here to clean up. */

        /* punt */
        plc->data_size = 0;
        plc->data_offset = 0;
        plc->current_layer = 0;
    }

    pdebug(DEBUG_INFO, "Done for plc %s.", plc->key);

    return rc;
}





void process_disconnect_response(sock_p socket, void *plc_arg)
{
    int rc = PLCTAG_STATUS_OK;
    plc_p plc = (plc_p)plc_arg;
    int data_start = 0;
    int data_end = 0;
    plc_request_id req_id = INVALID_REQUEST_ID;

    pdebug(DEBUG_INFO, "Starting for plc %s.", plc->key);

    critical_block(plc->plc_mutex) {
        int bytes_read = 0;
        do {
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
                rc = plc->layers[index].process_response(plc->layers[index].context, plc->data, plc->data_size, &data_start, &data_end, &req_id);
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
            plc->current_layer++;

            plc->data_size = 0;
            plc->data_offset = 0;

            if(plc->current_layer >= plc->num_layers) {
                pdebug(DEBUG_INFO, "Done disconnecting all layers.");

                socket_close(plc->socket);

                plc->is_connected = FALSE;
                plc->disconnect_in_flight = FALSE;

                /* reset the layers */
                for(int index = 0; index < plc->num_layers && rc == PLCTAG_STATUS_OK; index++) {
                    rc = plc->layers[index].initialize(plc->layers[index].context);
                }

                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Error, %s, while resetting layers!", plc_tag_decode_error(rc));
                    break;
                }
            } else {
                /* still layers to go. */
                rc = socket_callback_when_write_ready(plc->socket, write_disconnect_request, plc);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Error, %s, while setting up write callback!", plc_tag_decode_error(rc));
                    break;
                }
            }
        } while(0);

        if(rc != PLCTAG_STATUS_OK) {
            /* force a shutdown. */

            pdebug(DEBUG_WARN, "Error trying to process disconnect response.  Force reset the stack.");

            socket_close(plc->socket);

            /* reset the layers */
            for(int index = 0; index < plc->num_layers; index++) {
                rc = plc->layers[index].initialize(plc->layers[index].context);
            }

            plc->disconnect_in_flight = FALSE;
            plc->current_layer = 0;

            dispatch_plc_request_unsafe(plc);

            break;
        }
    }

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO, "Done for plc %s.", plc->key);
    } else {
        pdebug(DEBUG_WARN, "Error, %s, while processing disconnect response!", plc_tag_decode_error(rc));
    }
}




/* must be called in the mutex. */
int start_connecting_unsafe(plc_p plc)
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
        return PLCTAG_ERR_BUSY;
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
        do {
            /* skip layers that do not have a connect function. */
            while(plc->current_layer < plc->num_layers && !plc->layers[plc->current_layer].connect) {
                plc->current_layer++;
            }

            /* are we done going through the layers? */
            if(plc->current_layer >= plc->num_layers) {
                /* we are done. */
                pdebug(DEBUG_DETAIL, "Finished connecting layers.");
                break;
            }

            /* do we have a packet to send? */
            if(plc->data_size == 0) {
                /* no we do not. make one. */
                rc = build_connect_request_unsafe(plc);
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

                for(int index=0; index < plc->num_layers && rc == PLCTAG_STATUS_OK; index++) {
                    rc = plc->layers[index].prepare_for_response(plc->layers[index].context);
                }

                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Error, %s, preparing layers for a response!", plc_tag_decode_error(rc));
                    break;
                }

                rc = socket_callback_when_read_ready(plc->socket, process_connect_response, plc);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Error, %s, setting up callback for socket read!", plc_tag_decode_error(rc));
                    break;
                }
            }
        } while(0);

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while trying to write the connect request for layer %d!", plc_tag_decode_error(rc), plc->current_layer);
            start_disconnecting_unsafe(plc);
        }
    }

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO, "Done for plc %s.", plc->key);
    } else {
        pdebug(DEBUG_INFO, "Error, %s, while writing connect request for plc %s.", plc_tag_decode_error(rc), plc->key);
    }
}




int build_connect_request_unsafe(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    int data_start = 0;
    int data_end = plc->data_capacity;
    plc_request_id req_id = INVALID_REQUEST_ID;

    pdebug(DEBUG_INFO, "Starting for plc %s.", plc->key);

    do {
        int bytes_written = 0;

        /* reset the layers from lowest to highest. */
        for(int index = 0; index < plc->current_layer && index >= 0 && rc == PLCTAG_STATUS_OK; index++) {
            pdebug(DEBUG_DETAIL, "Preparing layer %d for connect.", index);
            rc = plc->layers[index].prepare_for_request(plc->layers[index].context);
        }

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while preparing layers!");
            break;
        }

        /* prepare the layers from lowest to highest.  Set what we can. */
        for(int index = 0; index < plc->current_layer && index >= 0 && rc == PLCTAG_STATUS_OK; index++) {
            pdebug(DEBUG_DETAIL, "Initial build layer %d for connect.", index);
            rc = plc->layers[index].build_request(plc->layers[index].context, plc->data, plc->data_capacity, &data_start, &data_end, &req_id);
        }

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while performing initial layer build!", plc_tag_decode_error(rc));
            break;
        }

        /* set up the top level connect packet. */
        rc = plc->layers[plc->current_layer].connect(plc->layers[plc->current_layer].context, plc->data, plc->data_capacity, &data_start, &data_end);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while preparing layer %d for connect!", plc_tag_decode_error(rc), plc->current_layer);
            break;
        }

        /* now backfill the lower layers. */
        for(int index = 0; index < plc->current_layer && index >= 0 && rc == PLCTAG_STATUS_OK; index++) {
            pdebug(DEBUG_DETAIL, "Final build layer %d for connect.", index);
            rc = plc->layers[index].build_request(plc->layers[index].context, plc->data, plc->data_capacity, &data_start, &data_end, &req_id);
        }

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while performing final layer build!", plc_tag_decode_error(rc));
            break;
        }

        /* set up the state for the next part. */
        plc->data_offset = 0;
        plc->data_size = data_end;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error trying to build disconnect packet!");

        /* TODO - figure out what to do here to clean up. */

        /* punt */
        plc->data_size = 0;
        plc->data_offset = 0;
        plc->current_layer = plc->num_layers;
    }

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO, "Done for plc %s.", plc->key);
    } else {
        pdebug(DEBUG_INFO, "Error, %s, while building connect request for plc %s.", plc_tag_decode_error(rc), plc->key);
    }

    return rc;
}



void process_connect_response(sock_p socket, void *plc_arg)
{
    int rc = PLCTAG_STATUS_OK;
    plc_p plc = (plc_p)plc_arg;
    int data_start = 0;
    int data_end = 0;
    plc_request_id req_id = INVALID_REQUEST_ID;

    pdebug(DEBUG_INFO, "Starting for plc %s.", plc->key);

    critical_block(plc->plc_mutex) {
        int bytes_read = 0;

        do {
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
                rc = plc->layers[index].process_response(plc->layers[index].context, plc->data, plc->data_size, &data_start, &data_end, &req_id);
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
            plc->current_layer++;

            /* get us ready for the next round. */
            plc->data_size = 0;
            plc->data_offset = 0;

            if(plc->current_layer >= plc->num_layers) {
                /* done! */
                pdebug(DEBUG_INFO, "Connection of all layers complete!");

                plc->is_connected = TRUE;

                /* try to dispatch new requests */
                dispatch_plc_request_unsafe(plc);
            } else {
                pdebug(DEBUG_INFO, "More layers to go.");

                rc = socket_callback_when_write_ready(plc->socket, write_connect_request, plc);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Error, %s, while setting up write callback!", plc_tag_decode_error(rc));
                    break;
                }
            }

        } while(0);

        if(rc != PLCTAG_STATUS_OK) {
            /* force a shutdown. */

            pdebug(DEBUG_WARN, "Error trying to process connect response.  Force disconnect and try again.");

            /* TODO - set up delay timer. */

            plc->connect_in_flight = FALSE;
            plc->current_layer = 0;

            start_disconnecting_unsafe(plc);

            break;
        }
    }

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO, "Done for plc %s.", plc->key);
    } else {
        pdebug(DEBUG_INFO, "Done with error %s for plc %s.", plc_tag_decode_error(rc), plc->key);
    }
}




void build_plc_request_callback(sock_p sock, void *plc_arg)
{
    int rc = PLCTAG_STATUS_OK;
    plc_p plc = (plc_p)plc_arg;

    pdebug(DEBUG_DETAIL, "Starting for PLC %s.", plc->key);

    critical_block(plc->plc_mutex) {
        plc_request_p request = plc->request_list;
        int data_start = 0;
        int data_end = 0;
        plc_request_id req_num = 0;

        /* are we connected? */
        if(!plc->is_connected) {
            pdebug(DEBUG_INFO, "PLC is not connected.");
            rc = PLCTAG_STATUS_OK;
            break;
        }

        /* do we have anything to do? */
        if(!request) {
            pdebug(DEBUG_INFO, "Request list is empty.");
            rc = PLCTAG_STATUS_OK;
            break;
        }

        /* if we have a disconnect at the same time, the disconnect wins. */
        if(plc->disconnect_in_flight) {
            pdebug(DEBUG_INFO, "PLC disconnect in flight.");
            rc = PLCTAG_STATUS_OK;
            break;
        }

        /* mark that there is a request in flight in case there is no marker. */
        plc->request_in_flight = TRUE;

        /* reset the state of the stack. */
        for(int index = 0; index < plc->num_layers && rc == PLCTAG_STATUS_OK; index++) {
            rc = plc->layers[index].prepare_for_request(plc->layers[index].context);
        }

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while preparing PLC protocol stack!", plc_tag_decode_error(rc));
            break;
        }

        do {
            /* build the packet from the bottom up */
            for(int index = 0; index < plc->num_layers && rc == PLCTAG_STATUS_OK; index++) {
                rc = plc->layers[index].build_request(plc->layers[index].context, plc->data, plc->data_capacity, &data_start, &data_end, &req_num);
            }

            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error, %s, while preparing PLC protocol stack!", plc_tag_decode_error(rc));
                break;
            }

            if(data_start == data_end) {
                pdebug(DEBUG_DETAIL, "No space left for processing packets.  Starting to send next.");
                break;
            }

            pdebug(DEBUG_INFO, "Preparing request %" PRId64 ".", req_num);

            /* add the request in. */
            request->req_id = req_num;
            rc = request->build_request(request->context, plc->data, plc->data_capacity, &data_start, &data_end, req_num);

            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error, %s, while preparing PLC protocol stack!", plc_tag_decode_error(rc));
                break;
            }

            pdebug(DEBUG_DETAIL, "Request packet:");
            pdebug_dump_bytes(DEBUG_DETAIL, plc->data, data_end);

            request = request->next;
        } while(request);

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while preparing PLC request!", plc_tag_decode_error(rc));
            break;
        }

        /* reset the stack to prepare for receiving a response. */
        for(int index = 0; index < plc->num_layers && rc == PLCTAG_STATUS_OK; index++) {
            rc = plc->layers[index].prepare_for_response(plc->layers[index].context);
        }

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while preparing PLC protocol stack!", plc_tag_decode_error(rc));
        }

        if(rc == PLCTAG_STATUS_OK) {
            /* queue the send. */
            plc->data_size = data_end;
            plc->data_offset = 0;

            rc = socket_callback_when_write_ready(plc->socket, write_plc_request_callback, plc);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error, %s, while setting up write callback!", plc_tag_decode_error(rc));
            }
        }

        /* check the result. */
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, found!  Starting disconnect process.", plc_tag_decode_error(rc));

            plc->request_in_flight = FALSE;

            /* clean up the whole stack at this point. */
            rc = start_disconnecting_unsafe(plc);
            break;
        }
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, while building request!", plc_tag_decode_error(rc));
    } else {
        pdebug(DEBUG_DETAIL, "Done for PLC %s.", plc->key);
    }
}



void write_plc_request_callback(sock_p socket, void *plc_arg)
{
    int rc = PLCTAG_STATUS_OK;
    plc_p plc = (plc_p)plc_arg;

    pdebug(DEBUG_INFO, "Starting for plc %s.", plc->key);

    critical_block(plc->plc_mutex) {

        /* multiple things to check that can change. */
        do {
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
                    rc = socket_callback_when_write_ready(plc->socket, write_plc_request_callback, plc);
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

                /* ready the layers to accept a response. */
                for(int index = 0; index < plc->num_layers && rc == PLCTAG_STATUS_OK; index++) {
                    rc = plc->layers[index].prepare_for_response(plc->layers[index].context);
                }

                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Error, %s, setting up layers for response!", plc_tag_decode_error(rc));
                    break;
                }

                rc = socket_callback_when_read_ready(plc->socket, process_plc_request_response, plc);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Error, %s, setting up callback for socket write!", plc_tag_decode_error(rc));
                    break;
                }
            }
        } while(0);

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while trying to write out request packet!", plc_tag_decode_error(rc));

            plc->request_in_flight = FALSE;

            /* tear down the stack. */
            start_disconnecting_unsafe(plc);

            break;
        }
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, while trying to write out request packet for PLC %s!", plc_tag_decode_error(rc), plc->key);
    } else {
        pdebug(DEBUG_INFO, "Done for plc %s.", plc->key);
    }
}




void process_plc_request_response(sock_p socket, void *plc_arg)
{
    int rc = PLCTAG_STATUS_OK;
    plc_p plc = (plc_p)plc_arg;

    pdebug(DEBUG_INFO, "Starting for plc %s.", plc->key);

    critical_block(plc->plc_mutex) {
        int bytes_read = 0;
        int data_start = 0;
        int data_end = 0;
        plc_request_id req_id = INVALID_REQUEST_ID;
        plc_request_p *request_walker = &(plc->request_list);

        /* we have data to read. */
        bytes_read = socket_tcp_read(plc->socket, plc->data + plc->data_size, plc->data_capacity - plc->data_size);
        if(bytes_read < 0) {
            rc = bytes_read;
            pdebug(DEBUG_WARN, "Error, %s, while trying to read socket!", plc_tag_decode_error(rc));
            break;
        }

        plc->data_size += bytes_read;

        /* possibly loop over the requests. */
        do {
            data_start = 0;
            data_end = plc->data_size;
            plc_request_p request = *request_walker;

            /* do we have enough data? */
            for(int index = 0; index <= plc->current_layer && rc == PLCTAG_STATUS_OK; index++) {
                rc = plc->layers[index].process_response(plc->layers[index].context, plc->data, plc->data_capacity, &data_start, &data_end, &req_id);
            }

            if(rc == PLCTAG_ERR_PARTIAL) {
                /* not an error, we just need more data. */
                rc = socket_callback_when_read_ready(plc->socket, process_plc_request_response, plc);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Error, %s, while setting up read callback!", plc_tag_decode_error(rc));
                }

                /* we are not done, so bail out of the block. */
                break;
            } else if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
                /* some other error.  Punt. */
                pdebug(DEBUG_WARN, "Error, %s, while trying to process response!", plc_tag_decode_error(rc));
                break;
            }

            /* check the request number. */
            if(request->req_id == req_id) {
                pdebug(DEBUG_DETAIL, "Processing request ID %" PRId64 ":", req_id);
                pdebug_dump_bytes(DEBUG_DETAIL, plc->data + data_start, data_end - data_start);

                rc = request->process_response(request->context, plc->data, plc->data_capacity, &data_start, &data_end, req_id);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Error, %s, processing response %" PRId64 ".", req_id);
                    break;
                }

                /* remove the request from the queue. */
                *request_walker = request->next;

                /* clean up the request. */
                request->next = NULL;
                request->req_id = INVALID_REQUEST_ID;
            } else {
                pdebug(DEBUG_DETAIL, "Skipping non-matching request %" PRId64 " vs. response %" PRId64 ".", request->req_id, req_id);
            }

            request_walker = &(request->next);
        } while(*request_walker);

        if(rc == PLCTAG_STATUS_OK) {
            pdebug(DEBUG_INFO, "Processed entire response packet.");

            dispatch_plc_request_unsafe(plc);
        } else {
            pdebug(DEBUG_WARN, "Error, %s, while trying to write out request packet!", plc_tag_decode_error(rc));

            plc->request_in_flight = FALSE;

            /* tear down the stack. */
            start_disconnecting_unsafe(plc);

            break;
        }
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, while trying to write out request packet for PLC %s!", plc_tag_decode_error(rc), plc->key);
    } else {
        pdebug(DEBUG_INFO, "Done for plc %s.", plc->key);
    }
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
    int rc = PLCTAG_STATUS_OK;

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

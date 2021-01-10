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





//FIXME - make sure connect goes bottom up and disconnect goes top down.




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
#include <util/timer_event.h>


#define DESTROY_DISCONNECT_TIMEOUT_MS (500)
#define DEFAULT_IDLE_TIMEOUT_MS (5000)
#define PLC_HEARTBEAT_INTERVAL_MS (200)


/* very common code pattern. */
#define RETRY_ON_ERROR(CONDITION, ...)  \
if(CONDITION) { \
    pdebug(DEBUG_WARN, __VA_ARGS__ ); \
    plc->state_func = state_error_retry; \
    rc = PLCTAG_STATUS_PENDING; \
    break; \
}




struct plc_layer_s {
    void *context;
    int (*initialize)(void *context);
    int (*connect)(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end);
    int (*disconnect)(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end);
    int (*prepare)(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_num);
    int (*build_request)(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_num);
    int (*process_response)(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_num);
    int (*destroy_layer)(void *context);
};



struct plc_s {
    struct plc_s *next;

    /* the unique identifying key for this PLC instance. */
    const char *key;

    mutex_p plc_mutex;

    bool is_terminating;

    /* where are we connected/connecting? */
    const char *host;
    int port;
    sock_p socket;

    /* our requests. */
    struct plc_request_s *request_list;
    struct plc_request_s *current_request;
    plc_request_id current_response_id;
    bool last_payload;

    int num_layers;
    struct plc_layer_s *layers;
    int current_layer;

    /* state engine */
    int (*state_func)(plc_p plc);

    /* data buffer. */
    uint8_t *data;
    int data_capacity;
    int data_actual_capacity;
    int data_size;
    bool response_complete;
    int payload_end;
    int payload_start;


    /* model-specific data. */
    void *context;
    void (*context_destructor)(plc_p plc, void *context);

    /* heartbeat heartbeat_timer */
    timer_p heartbeat_timer;

    /* error handling */
    int64_t next_retry_time;

    /* idle handling. */
    int idle_timeout_ms;
    int64_t next_idle_timeout;


    // bool connect_in_flight;
    // bool disconnect_in_flight;

    bool is_connected;

};


static mutex_p plc_list_mutex = NULL;
static plc_p plc_list = NULL;


static void start_idle_timeout_unsafe(plc_p plc);
static void plc_idle_timeout_callback(int64_t wake_time, int64_t current_time, void *plc_arg);
static void reset_plc(plc_p plc);
static void plc_rc_destroy(void *plc_arg);
static void dispatch_plc_request_unsafe(plc_p plc);
static int start_connecting_unsafe(plc_p plc);
static void write_connect_request(void *plc_arg);
static int build_connect_request_unsafe(plc_p plc);
static void process_connect_response(void *plc_arg);
static int start_disconnecting_unsafe(plc_p plc);
static void write_disconnect_request(void *plc_arg);
static int build_disconnect_request_unsafe(plc_p plc);
static void process_disconnect_response(void *plc_arg);
static void build_plc_request_callback(void *plc_arg);
static void write_plc_request_callback(void *plc_arg);
static void process_plc_request_response(void *plc_arg);

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
                rc = PLCTAG_ERR_BAD_GATEWAY;
                break;
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

            rc = timer_event_create(&(plc->heartbeat_timer));
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unable to create heartbeat_timer, error %s!", plc_tag_decode_error(rc));
                rc_dec(plc);
                break;
            }

            plc->idle_timeout_ms = DEFAULT_IDLE_TIMEOUT_MS;

            /* start the idle heartbeat_timer */
            start_idle_timeout_unsafe(plc);

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



int plc_initialize(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;

    if(!plc) {
        pdebug(DEBUG_WARN, "PLC pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    critical_block(plc->plc_mutex) {
        reset_plc(plc);
    }

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return rc;
}




int plc_set_number_of_layers(plc_p plc, int num_layers)
{
    int rc = PLCTAG_STATUS_OK;

    if(!plc) {
        pdebug(DEBUG_WARN, "PLC pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    /* clean up anything that already existed. */
    if(plc->layers) {
        for(int index = 0; index < plc->num_layers; index++) {
            plc->layers[index].destroy_layer(plc->layers[index].context);
        }

        mem_free(plc->layers);
    }

    plc->layers = mem_alloc((int)(unsigned int)sizeof(struct plc_layer_s) * num_layers);
    if(!plc->layers) {
        pdebug(DEBUG_WARN, "Unable to allocate the layer array memory!");
        plc->num_layers = 0;
        return PLCTAG_ERR_NO_MEM;
    }

    plc->num_layers = num_layers;

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return rc;
}



int plc_set_layer(plc_p plc,
                  int layer_index,
                  void *context,
                  int (*initialize)(void *context),
                  int (*connect)(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end),
                  int (*disconnect)(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end),
                  int (*prepare)(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_num),
                  int (*build_request)(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_num),
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
    plc->layers[layer_index].prepare = prepare;
    plc->layers[layer_index].build_request = build_request;
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


int plc_set_context(plc_p plc, void *context, void (*context_destructor)(plc_p plc, void *context))
{
    if(!plc) {
        pdebug(DEBUG_WARN, "PLC pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    critical_block(plc->plc_mutex) {
        plc->context = context;
        plc->context_destructor = context_destructor;
    }

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return PLCTAG_STATUS_OK;
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

    if(timeout_ms < 0 || timeout_ms > 5000) { /* FIXME - MAGIC constant */
        pdebug(DEBUG_WARN, "Illegal timeout value %d!", timeout_ms);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    critical_block(plc->plc_mutex) {
        res = plc->idle_timeout_ms;
        plc->idle_timeout_ms = timeout_ms;
    }

    /* new timeout period takes effect after the next heartbeat_timer wake up. */

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return res;
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
        res = plc->payload_end;
    }

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return res;
}



int plc_set_buffer_size(plc_p plc, int buffer_size, int buffer_overflow)
{
    int rc = 0;
    int actual_capacity = 0;

    if(!plc) {
        pdebug(DEBUG_WARN, "PLC pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    if(buffer_size <= 0 || buffer_size > 65536) {
        pdebug(DEBUG_WARN, "Illegal buffer size value %d!", buffer_size);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    if(buffer_overflow < 0 || buffer_overflow > 65536) {
        pdebug(DEBUG_WARN, "Illegal buffer overflow size value %d!", buffer_overflow);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    actual_capacity = buffer_size + buffer_overflow;

    critical_block(plc->plc_mutex) {
        plc->data = mem_realloc(plc->data, actual_capacity);
        if(!plc->data) {
            pdebug(DEBUG_WARN, "Unable to reallocate memory for data buffer!");
            rc = PLCTAG_ERR_NO_MEM;
            break;
        }

        plc->data_capacity = buffer_size;
        plc->data_actual_capacity = actual_capacity;
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

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);
    } else {
        pdebug(DEBUG_INFO, "Done with error %s for PLC %s.", plc_tag_decode_error(rc), plc->key);
    }

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
            rc = PLCTAG_ERR_NOT_FOUND;
        }
    }

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);
    } else {
        pdebug(DEBUG_INFO, "Done with error %s for PLC %s.", plc_tag_decode_error(rc), plc->key);
    }

    return rc;
}

void start_idle_timeout_unsafe(plc_p plc)
{
    int64_t next_timeout = time_ms() + plc->idle_timeout_ms;

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    /* remove it from the queue. */
    timer_event_snooze(plc->heartbeat_timer);

    timer_event_wake_at(plc->heartbeat_timer, next_timeout, plc_idle_timeout_callback, plc);

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);
}



void plc_idle_timeout_callback(int64_t wake_time, int64_t current_time, void *plc_arg)
{
    plc_p plc = (plc_p)plc_arg;

    (void)wake_time;

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    /* we want to take the mutex here.  Protect the PLC state while we see if we are going to timeout */
    critical_block(plc->plc_mutex) {
        if(plc->is_connected) {
            if(plc->next_idle_timeout < current_time) {
                pdebug(DEBUG_INFO, "Idle timeout triggered.  Starting to disconnect.");
                start_disconnecting_unsafe(plc);
            }
        }
    }

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);
}


void reset_plc(plc_p plc)
{
    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    if(plc->socket) {
        /*
         * this is safe because socket_close() acquires the socket mutex.
         * And that prevents the event loop thread from calling a callback at the
         * same time.   Once this is done, the even loop will not trigger any
         * callbacks against this PLC.
         */
        socket_close(plc->socket);
    }

    plc->is_connected = FALSE;
    plc->connect_in_flight = FALSE;
    plc->disconnect_in_flight = FALSE;
    plc->current_layer = 0;

    /* reset the layers */
    for(int index = 0; index < plc->num_layers; index++) {
        plc->layers[index].initialize(plc->layers[index].context);
    }

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);
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

    if(plc->heartbeat_timer) {
        timer_event_snooze(plc->heartbeat_timer);
        timer_destroy(&(plc->heartbeat_timer));
        plc->heartbeat_timer = NULL;
    }

    /* reset the PLC to clean up more things. */
    reset_plc(plc);

    if(plc->socket) {
        socket_destroy(&(plc->socket));
        plc->socket = NULL;
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



static int plc_state_runner(plc_p plc);
static void plc_heartbeat(plc_p plc);








/* new implementation */

int plc_state_runner(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    critical_block(plc->plc_mutex) {
        /* loop until we end up waiting for something. */
        do {
            rc = plc->state_func(plc);
        } while(rc == PLCTAG_STATUS_PENDING);
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}



void plc_heartbeat(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    int64_t now = event_loop_time();

    pdebug(DEBUG_DETAIL, "Starting.");

    /* we do not want the PLC changing from another thread */
    critical_block(plc->plc_mutex) {
        /* Only dispatch if we can. */
        if(plc->state_func == state_dispatch_requests) {
            plc_state_runner(plc);
        }
    }

    rc = timer_event_wake_at(plc->heartbeat_timer, now + PLC_HEARTBEAT_INTERVAL_MS, plc_heartbeat, plc);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to set up heartbeat_timer wake event.  Got error %s!", plc_tag_decode_error(rc));

        /* TODO - what do we do now?   Everything is broken... */
    }

    pdebug(DEBUG_DETAIL, "Done.");
}





/* main dispatch states. */
static int state_dispatch_requests(plc_p plc);
static int state_prepare_protocol_layers_for_tag_request(plc_p plc);
static int state_build_tag_request(plc_p plc);
static int state_fix_up_tag_request_layers(plc_p plc);
static int state_tag_request_sent(plc_p plc);
static int state_tag_response_ready(plc_p plc);
static int state_process_tag_response(plc_p plc);

/* connection states. */
static int state_start_connect(plc_p plc);
static int state_start_open_socket(plc_p plc);
static int state_start_connect_layer(plc_p plc);
static int state_build_connect_request(plc_p plc);
static int state_send_connect_request(plc_p plc);
static int state_wait_for_connect_response(plc_p plc);
static int state_process_connect_response(plc_p plc);

/* disconnect states. */
static int state_start_disconnect(plc_p plc);
static int state_start_disconnect_layer(plc_p plc);
static int state_build_disconnect_request(plc_p plc);
static int state_send_disconnect_request(plc_p plc);
static int state_wait_for_disconnect_response(plc_p plc);
static int state_process_disconnect_response(plc_p plc);
static int state_close_socket(plc_p plc);

/* terminal or error states */
static int state_error_retry(plc_p plc);
static int state_terminate(plc_p plc);


/* dispatch states. */


int state_dispatch_requests(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    int64_t now = event_loop_time();

    pdebug(DEBUG_DETAIL, "Starting.");

    do {
        /* if we are terminating, then close down. */
        if(plc->is_terminating) {
            if(plc->is_connected == TRUE) {
                pdebug(DEBUG_INFO, "PLC terminating, starting disconnect.");
                plc->state_func = state_start_disconnect;
                rc = PLCTAG_STATUS_PENDING;
                break;
            } else {
                plc->state_func = state_terminate;
                rc = PLCTAG_STATUS_OK;
                break;
            }
        }

        /* check idle time. */
        if(plc->next_idle_timeout < now) {
            pdebug(DEBUG_INFO, "Starting idle disconnect.");
            plc->state_func = state_start_disconnect;
            rc = PLCTAG_STATUS_PENDING;
            break;
        }

        /* are there requests queued? */
        if(plc->request_list) {
            /* are we connected? */
            if(plc->is_connected == FALSE) {
                plc->state_func = state_start_connect;
                rc = PLCTAG_STATUS_PENDING;
                break;
            }

            /* we have requests and we are connected, dispatch! */
            plc->state_func = state_prepare_protocol_layers_for_tag_request;
            rc = PLCTAG_STATUS_PENDING;
            break;
        }
    } while(0);

    if(rc == PLCTAG_STATUS_OK || rc == PLCTAG_STATUS_PENDING) {
        pdebug(DEBUG_DETAIL, "Done dispatching for PLC %s.", plc->key);
        return rc;
    } else {
        pdebug(DEBUG_WARN, "Error %s while trying to dispatch for PLC %s!", plc_tag_decode_error(rc), plc->key);
        return rc;
    }

    /* TODO - handle errors. */
}


int state_prepare_protocol_layers_for_tag_request(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    int data_start = 0;
    int data_end = 0;
    plc_request_id req_id = 0;

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    do {
        /* prepare the layers for a request. */
        for(int index = 0; index < plc->num_layers && rc == PLCTAG_STATUS_OK; index++) {
            rc = plc->layers[index].prepare(plc->layers[index].context, plc->data, plc->data_capacity, &data_start, &data_end, &req_id);
        }

        RETRY_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s while preparing layers for tag request for PLC %s!", plc_tag_decode_error(rc), plc->key);

        /* payload boundaries. */
        plc->payload_start = data_start;
        plc->payload_end = data_end;

        /* set up request */
        plc->current_request = plc->request_list;
        plc->current_request->req_id = req_id;

        plc->state_func = state_build_tag_request;
        rc = PLCTAG_STATUS_PENDING;
        break;
    } while(0);

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return rc;
}


int state_build_tag_request(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    int data_start = plc->payload_start;
    int data_end = plc->payload_end;

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    do {
        /* build the tag request on top of the prepared layers. */
        rc = plc->current_request->build_request(plc->current_request->context, plc->data, plc->data_actual_capacity, &data_start, &data_end, plc->current_request->req_id);

        RETRY_ON_ERROR(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_TOO_SMALL, "Error %s building tag request for PLC %s!", plc_tag_decode_error(rc), plc->key);

        /* was there enough space? */
        if(rc == PLCTAG_ERR_TOO_SMALL) {
            pdebug(DEBUG_INFO, "Insufficient space to build tag request for PLC %s.", plc->key);

            /* signal that we are done. */
            plc->last_payload = TRUE;
            plc->payload_end = plc->payload_start;
        } else {
            pdebug(DEBUG_INFO, "Build tag request, going to next state to fill in layers for PLC %s.", plc->key);

            plc->payload_end = data_end;
            plc->last_payload = FALSE;
        }

        /* fix up the layers. */
        plc->state_func = state_fix_up_tag_request_layers;
        rc = PLCTAG_STATUS_PENDING;
    } while(0);

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return rc;
}


int state_fix_up_tag_request_layers(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    int data_start = 0;
    int data_end = plc->payload_end;
    plc_request_id req_id = 0;

    pdebug(DEBUG_DETAIL, "Starting for PLC %s.", plc->key);

    do {
        for(int index = 0; index < plc->num_layers && rc == PLCTAG_STATUS_OK; index++) {
            rc = plc->layers[index].build_request(plc->layers[index].context, plc->data, plc->data_capacity, &data_start, &data_end, &req_id);
            if(rc == PLCTAG_STATUS_PENDING) {
                /* this layer can handle multiple requests. */
                plc->last_payload = FALSE;
                rc = PLCTAG_STATUS_OK;
            }
        }

        RETRY_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s while building request layers for PLC %s!", plc_tag_decode_error(rc), plc->key);

        /* we built the layers.   Do we have an option to add more requests? */
        if(plc->last_payload == FALSE && plc->current_request->next) {
            plc->current_request = plc->current_request->next;
            plc->current_request->req_id = req_id;

            /* point to the possible data range in the buffer. */
            plc->payload_start = data_start;
            plc->payload_end = data_end;

            plc->state_func = state_build_tag_request;
            rc = PLCTAG_STATUS_PENDING;
            break;
        }

        /* we built the full packet.  Send it. */
        plc->data_size = data_end;
        plc->state_func = state_tag_request_sent;
        rc = socket_callback_when_write_done(plc->socket, plc_state_runner, plc, plc->data, &(plc->data_size));

        RETRY_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s while setting up write callback for PLC %s!", plc_tag_decode_error(rc), plc->key);

        /* we are done and need to wait for the IO layer to send the packet. */
        rc = PLCTAG_STATUS_OK;
    } while(0);

    pdebug(DEBUG_DETAIL, "Done for PLC %s.", plc->key);

    return rc;
}


int state_tag_request_sent(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    int data_start = 0;
    int data_end = 0;
    plc_request_id req_id = 0;
    int bytes_written = 0;

    pdebug(DEBUG_DETAIL, "Starting for PLC %s.", plc->key);

    do {
        /* find out why we got called. */
        rc = socket_status(plc->socket);

        RETRY_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s when trying to write socket in PLC %s!", plc_tag_decode_error(rc), plc->key)

        /* all good!  We send the request.  Prepare the layers for receiving the response. */
        for(int index=0; index < plc->num_layers && rc == PLCTAG_STATUS_OK; index++) {
            rc = plc->layers[index].prepare(plc->layers[index].context, plc->data, plc->data_actual_capacity, &data_start, &data_end, &req_id);
        }

        RETRY_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s when trying to prepare layers for response in PLC %s!", plc_tag_decode_error(rc), plc->key)

        /* ready to wait for the response. */
        plc->state_func = state_tag_response_ready;

        /* clear out the payload. */
        plc->payload_end = plc->payload_start = plc->data_size = 0;

        rc = socket_callback_when_read_done(plc->socket, plc_state_runner, plc, plc->data, plc->data_actual_capacity, &(plc->data_size));

        RETRY_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s when trying to set up socket response read in PLC %s!", plc_tag_decode_error(rc), plc->key)

        /* wait for response. */
        rc = PLCTAG_STATUS_OK;
        break;
    } while(0);

    pdebug(DEBUG_DETAIL, "Done for PLC %s.", plc->key);

    return rc;
}



int state_tag_response_ready(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    int bytes_read = 0;
    int data_start = 0;
    int data_end = 0;
    plc_request_id req_id = 0;

    pdebug(DEBUG_DETAIL, "Starting for PLC %s.", plc->key);

    do {
        /* check socket status.  Might be an error */
        rc = socket_status(plc->socket);

        RETRY_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s from socket read for PLC %s!", plc_tag_decode_error(rc), plc->key);

        pdebug(DEBUG_DETAIL, "Got possible response:");
        pdebug_dump_bytes(DEBUG_DETAIL, plc->data, plc->data_size);

        /*
         * If a layer returns PLCTAG_STATUS_PENDING, then we know that there are multiple
         * sub-packets remaining within it and we need to continue.
         */
        plc->last_payload = TRUE;

        for(int index=0; index < plc->num_layers && rc == PLCTAG_STATUS_OK; index++) {
            rc = plc->layers[index].process_response(plc->layers[index].context, plc->data, plc->data_actual_capacity, &data_start, &data_end, &req_id);
            if(rc == PLCTAG_STATUS_PENDING) {
                plc->last_payload = FALSE;
                rc = PLCTAG_STATUS_OK;
            }
        }

        RETRY_ON_ERROR(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_PARTIAL, "Got error %s processing layers, restarting stack for PLC %s!", plc_tag_decode_error(rc), plc->key)

        /* if we get PLCTAG_ERR_PARTIAL, then we need to keep reading. */
        if(rc == PLCTAG_ERR_PARTIAL) {
            pdebug(DEBUG_INFO, "PLC %s did not get all the data yet, need to get more.", plc->key);

            /* come back to this routine. */
            plc->state_func = state_tag_response_ready;
            rc = socket_callback_when_read_done(plc->socket, plc_state_runner, plc, plc->data, plc->data_actual_capacity, &(plc->data_size));

            RETRY_ON_ERROR(rc != PLCTAG_STATUS_OK, "Unexpected error %s setting socket read callback for PLC %s!", plc_tag_decode_error(rc), plc->key);

            /* We know we need to wait, so there is nothing for the state runner to do. */
            rc = PLCTAG_STATUS_OK;
            break;
        }

        /* if we got here, then we have a valid response.
         * The data_start and data_end bracket the payload for the tag.
         *
         * req_id is the internal ID for the request we want to match.
         */

        // plc->response_complete = TRUE;
        // plc->last_payload = (rc == PLCTAG_STATUS_PENDING);

        plc->payload_start = data_start;
        plc->payload_end = data_end;
        plc->current_response_id = req_id;

        plc->state_func = state_process_tag_response;
        rc = PLCTAG_STATUS_PENDING;
        break;
    } while(0);

    pdebug(DEBUG_DETAIL, "Done for PLC %s.", plc->key);

    return rc;
}


int state_process_tag_response(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    int data_start = 0;
    int data_end = 0;

    pdebug(DEBUG_DETAIL, "Starting for PLC %s.", plc->key);

    do {
        /* is there any data left? */
        /* is this response for us? */
        if(plc->request_list && plc->current_response_id == plc->request_list->req_id) {
            plc_request_p request = plc->request_list;

            /* remove the request from the queue. We have a response. */
            plc->request_list = plc->request_list->next;
            request->next = NULL;

            data_start = plc->payload_start;
            data_end = plc->payload_end;

            pdebug(DEBUG_DETAIL, "Attempting to process request %" REQ_ID_FMT " for PLC %s.", request->req_id, plc->key);

            rc = request->process_response(request->context, plc->data, plc->data_actual_capacity, &data_start, &data_end, request->req_id);

            RETRY_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s processing request for tag for PLC %s!", plc_tag_decode_error(rc), plc->key);
        } else {
            /* nope */
            pdebug(DEBUG_INFO, "Response for a skipped or aborted request %" REQ_ID_FMT " for PLC %s.", plc->current_response_id, plc->key);
        }

        /* do we have more to process? */
        if(plc->last_payload == FALSE) {
            pdebug(DEBUG_INFO, "More response sub-packets to process for PLC %s.", plc->key);
            plc->state_func = state_tag_response_ready;
            rc = PLCTAG_STATUS_PENDING;
            break;
        } else {
            /* last one, go to the dispatcher. */
            pdebug(DEBUG_INFO, "No more response sub-packets to process for PLC %s, triggering dispatcher.", plc->key);
            plc->state_func = state_dispatch_requests;
            rc = PLCTAG_STATUS_PENDING;
            break;
        }
    } while(0);

    pdebug(DEBUG_DETAIL, "Done for PLC %s.", plc->key);

    return rc;
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
    plc->current_layer = plc->num_layers - 1;

    /* kill any request in flight */
    if(plc->request_in_flight) {
        rc = socket_callback_when_read_ready(plc->socket, NULL, NULL);
        rc = socket_callback_when_write_ready(plc->socket, NULL, NULL);

        plc->request_in_flight = FALSE;
    }

    plc->payload_end = 0;
    plc->payload_start = 0;

    rc = socket_callback_when_write_ready(plc->socket, write_disconnect_request, plc);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, while setting up disconnect callback!", plc_tag_decode_error(rc));

        /* clear up the world.  Something is really wrong. */
        reset_plc(plc);

        /* restart. */
        dispatch_plc_request_unsafe(plc);

        return rc;
    }

    pdebug(DEBUG_INFO, "Done starting disconnect PLC %s.", plc->key);

    return rc;
}




void write_disconnect_request(void *plc_arg)
{
    int rc = PLCTAG_STATUS_OK;
    plc_p plc = (plc_p)plc_arg;

    pdebug(DEBUG_INFO, "Starting for plc %s.", plc->key);

    critical_block(plc->plc_mutex) {
        do {
            /* does the current layer support disconnect? */
            while(plc->current_layer >= 0 && !plc->layers[plc->current_layer].disconnect) {
                /* no it does not, skip down to the next layer. */
                plc->current_layer--;
            }

            /* are we done going through the layers? */
            if(plc->current_layer < 0) {
                /* we are done. */
                pdebug(DEBUG_DETAIL, "Finished disconnecting layers.");
                break;
            }

            /* do we have a packet to send? */
            if(plc->payload_end == 0) {
                /* no we do not. make one. */
                rc = build_disconnect_request_unsafe(plc);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Error, %s, building disconnect request for layer %d!", plc_tag_decode_error(rc), plc->current_layer);
                    break;
                }
            }

            /* do we have data to send? */
            if(plc->payload_start < plc->payload_end) {
                int bytes_written = 0;

                /* We have data to write. */
                bytes_written = socket_tcp_write(plc->socket, plc->data + plc->payload_start, plc->payload_end - plc->payload_start);
                if(bytes_written < 0) {
                    /* error sending data! */
                    rc = bytes_written;

                    pdebug(DEBUG_WARN, "Error, %s, sending data!", plc_tag_decode_error(rc));
                    break;
                }

                plc->payload_start += bytes_written;

                /* did we write all the data? */
                if(plc->payload_start < plc->payload_end) {
                    /* no, so queue up another write. */
                    rc = socket_callback_when_write_ready(plc->socket, write_disconnect_request, plc);
                    if(rc != PLCTAG_STATUS_OK) {
                        pdebug(DEBUG_WARN, "Error, %s, setting up callback for socket write!", plc_tag_decode_error(rc));
                        break;
                    }
                }
            }

            /* did we write it all? */
            if(plc->payload_start >= plc->payload_end) {
                /* yes, so wait for the response. */
                plc->payload_start = 0;
                plc->payload_end = 0;

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

            /* clean up the world. */
            reset_plc(plc);

            /* restart. */
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
        /* prepare the layers from lowest to highest.  This reserves space in the buffer. */
        for(int index = 0; index < plc->current_layer && index >= 0 && rc == PLCTAG_STATUS_OK; index++) {
            pdebug(DEBUG_DETAIL, "Preparing layer %d for disconnect.", index);
            rc = plc->layers[index].prepare(plc->layers[index].context, plc->data, plc->data_capacity, &data_start, &data_end, &req_id);
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

        /* backfill the layers of the packet.  bottom to top again. */
        for(int index = 0; index < plc->current_layer && index >= 0 && rc == PLCTAG_STATUS_OK; index++) {
            pdebug(DEBUG_DETAIL, "Building layer %d for disconnect.", index);
            rc = plc->layers[index].build_request(plc->layers[index].context, plc->data, plc->data_capacity, &data_start, &data_end, &req_id);
        }

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while building layers!");
            break;
        }

        pdebug(DEBUG_INFO, "Layer %d disconnect packet:", plc->current_layer);
        pdebug_dump_bytes(DEBUG_INFO, plc->data, data_end);

        plc->payload_end = data_end;
        plc->payload_start = 0;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error trying to build disconnect packet!");

        /* punt */
        plc->payload_end = 0;
        plc->payload_start = 0;
        plc->current_layer = 0;

        /* let the caller deal with it. */
        return rc;
    }

    pdebug(DEBUG_INFO, "Done for plc %s.", plc->key);

    return rc;
}





void process_disconnect_response(void *plc_arg)
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
            bytes_read = socket_tcp_read(plc->socket, plc->data + plc->payload_end, plc->data_capacity - plc->payload_end);
            if(bytes_read < 0) {
                rc = bytes_read;
                pdebug(DEBUG_WARN, "Error, %s, while trying to read socket!", plc_tag_decode_error(rc));
                break;
            }

            plc->payload_end += bytes_read;

            /* do we have enough data? */
            for(int index = 0; index <= plc->current_layer && rc == PLCTAG_STATUS_OK; index++) {
                rc = plc->layers[index].process_response(plc->layers[index].context, plc->data, plc->payload_end, &data_start, &data_end, &req_id);
            }

            if(rc == PLCTAG_ERR_PARTIAL) {
                /* not an error, we just need more data. */
                rc = socket_callback_when_read_ready(plc->socket, process_disconnect_response, plc);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Error, %s, while setting up read callback!", plc_tag_decode_error(rc));
                } else {
                    /* we are not done, make sure the error is understood. */
                    rc = PLCTAG_ERR_PARTIAL;
                }

                break;
            } else if(rc != PLCTAG_STATUS_OK) {
                /* some other error.  Punt. */
                pdebug(DEBUG_WARN, "Error, %s, while trying to process response!", plc_tag_decode_error(rc));
                break;
            }

            /*
             * we are all good. Do the next layer.
             * Note we start at higher levels of the stack
             * and work down toward the lowest level. */
            plc->current_layer--;

            plc->payload_end = 0;
            plc->payload_start = 0;

            if(plc->current_layer < 0) {
                pdebug(DEBUG_INFO, "Done disconnecting all layers.");

                socket_close(plc->socket);

                plc->is_connected = FALSE;
                plc->disconnect_in_flight = FALSE;
                plc->current_layer = 0;

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

        if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_PARTIAL) {
            /* force a shutdown. */

            pdebug(DEBUG_WARN, "Error trying to process disconnect response.  Force reset the stack.");

            /* clean up the world. */
            reset_plc(plc);

            /* restart everything. */
            dispatch_plc_request_unsafe(plc);

            break;
        }
    }

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO, "Done for plc %s.", plc->key);
    } else {
        pdebug(DEBUG_WARN, "Done with error %s while processing disconnect response!", plc_tag_decode_error(rc));
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

    /* force the PLC into a good state */
    reset_plc(plc);

    /* set up connect state. */
    plc->connect_in_flight = TRUE;
    plc->current_layer = 0;
    plc->payload_end = 0;
    plc->payload_start = 0;

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




void write_connect_request(void *plc_arg)
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
            if(plc->payload_end == 0) {
                /* no we do not. make one. */
                rc = build_connect_request_unsafe(plc);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Error, %s, building connect request for layer %d!", plc_tag_decode_error(rc), plc->current_layer);
                    break;
                }
            }

            /* do we have data to send? */
            if(plc->payload_start < plc->payload_end) {
                int bytes_written = 0;

                /* We have data to write. */
                bytes_written = socket_tcp_write(plc->socket, plc->data + plc->payload_start, plc->payload_end - plc->payload_start);
                if(bytes_written < 0) {
                    /* error sending data! */
                    rc = bytes_written;

                    pdebug(DEBUG_WARN, "Error, %s, sending data!", plc_tag_decode_error(rc));
                    break;
                }

                plc->payload_start += bytes_written;

                /* did we write all the data? */
                if(plc->payload_start < plc->payload_end) {
                    /* no, so queue up another write. */
                    rc = socket_callback_when_write_ready(plc->socket, write_connect_request, plc);
                    if(rc != PLCTAG_STATUS_OK) {
                        pdebug(DEBUG_WARN, "Error, %s, setting up callback for socket write!", plc_tag_decode_error(rc));
                        break;
                    }
                }
            }

            /* did we write it all? */
            if(plc->payload_start >= plc->payload_end) {
                /* yes, so wait for the response. */
                plc->payload_start = 0;
                plc->payload_end = 0;

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
        /* prepare the layers from lowest to highest.  This reserves space in the buffer. */
        for(int index = 0; index < plc->current_layer && index >= 0 && rc == PLCTAG_STATUS_OK; index++) {
            pdebug(DEBUG_DETAIL, "Preparing layer %d for connect attempt.", index);
            rc = plc->layers[index].prepare(plc->layers[index].context, plc->data, plc->data_capacity, &data_start, &data_end, &req_id);
        }

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while preparing layers!");
            break;
        }

        /* set up the top level disconnect packet. */
        rc = plc->layers[plc->current_layer].connect(plc->layers[plc->current_layer].context, plc->data, plc->data_capacity, &data_start, &data_end);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while preparing layer %d for connect!", plc_tag_decode_error(rc), plc->current_layer);
            break;
        }

        /* backfill the layers of the packet.  bottom to top again. */
        for(int index = 0; index < plc->current_layer && index >= 0 && rc == PLCTAG_STATUS_OK; index++) {
            pdebug(DEBUG_DETAIL, "Building layer %d for connect attempt.", index);
            rc = plc->layers[index].build_request(plc->layers[index].context, plc->data, plc->data_capacity, &data_start, &data_end, &req_id);
        }

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while building layers!");
            break;
        }

        pdebug(DEBUG_INFO, "Layer %d disconnect packet:", plc->current_layer);
        pdebug_dump_bytes(DEBUG_INFO, plc->data, data_end);

        plc->payload_end = data_end;
        plc->payload_start = 0;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error trying to build disconnect packet!");

        /* TODO - figure out what to do here to clean up. */

        /* punt */
        plc->payload_end = 0;
        plc->payload_start = 0;
        plc->current_layer = plc->num_layers;
    }

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO, "Done for plc %s.", plc->key);
    } else {
        pdebug(DEBUG_INFO, "Error, %s, while building connect request for plc %s.", plc_tag_decode_error(rc), plc->key);
    }

    return rc;
}



void process_connect_response(void *plc_arg)
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
            bytes_read = socket_tcp_read(plc->socket, plc->data + plc->payload_end, plc->data_capacity - plc->payload_end);
            if(bytes_read < 0) {
                rc = bytes_read;
                pdebug(DEBUG_WARN, "Error, %s, while trying to read socket!", plc_tag_decode_error(rc));
                break;
            }

            plc->payload_end += bytes_read;

            /* do we have enough data? */
            for(int index = 0; index <= plc->current_layer && rc == PLCTAG_STATUS_OK; index++) {
                rc = plc->layers[index].process_response(plc->layers[index].context, plc->data, plc->payload_end, &data_start, &data_end, &req_id);
            }

            if(rc == PLCTAG_ERR_PARTIAL) {
                /* not an error, we just need more data. */
                rc = socket_callback_when_read_ready(plc->socket, process_connect_response, plc);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Error, %s, while setting up read callback!", plc_tag_decode_error(rc));
                } else {
                    /* we are not done, make sure the error is understood. */
                    rc = PLCTAG_ERR_PARTIAL;
                }

                break;
            } else if(rc != PLCTAG_STATUS_OK) {
                /* some other error.  Punt. */
                pdebug(DEBUG_WARN, "Error, %s, while trying to process response!", plc_tag_decode_error(rc));
                break;
            }

            /* we are all good. Do the next layer. */
            plc->current_layer++;

            /* get us ready for the next round. */
            plc->payload_end = 0;
            plc->payload_start = 0;

            if(plc->current_layer >= plc->num_layers) {
                /* done! */
                pdebug(DEBUG_INFO, "Connection of all layers complete!");

                plc->is_connected = TRUE;

                /* start the idle timeout heartbeat_timer */
                start_idle_timeout_unsafe(plc);

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

        if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_PARTIAL) {
            /* force a shutdown. */

            pdebug(DEBUG_WARN, "Error trying to process connect response.  Force disconnect and try again.");

            /* TODO - set up delay heartbeat_timer. */

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




void build_plc_request_callback(void *plc_arg)
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

        do {
            /* This allocates space in the buffer and may do some other housekeeping.*/
            for(int index = 0; index < plc->num_layers && rc == PLCTAG_STATUS_OK; index++) {
                rc = plc->layers[index].prepare(plc->layers[index].context, plc->data, plc->data_capacity, &data_start, &data_end, &req_num);
            }

            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error, %s, while preparing PLC protocol stack!", plc_tag_decode_error(rc));
                break;
            }

            /*
            * The data start points to the start where the request can be built.  The data end points
            * to the maximum possible place in the buffer.
            *
            * if data_end and data_start are the same, then we have no more buffer space to use and quit.
            *
            * We also quit if we run out of requests to process.
            *
            * req_num gives us the current request number we will store in the request to match later.
            */

            request = plc->request_list;

            do {
                /* add the request in. */
                request->req_id = req_num;
                rc = request->build_request(request->context, plc->data, plc->data_capacity, &data_start, &data_end, req_num);

                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Error, %s, while preparing PLC protocol stack!", plc_tag_decode_error(rc));
                    break;
                }

                pdebug(DEBUG_DETAIL, "Request packet before fix up:");
                pdebug_dump_bytes(DEBUG_DETAIL, plc->data, data_end);

                /* fix up/build the rest of the layers. */

                /* build the packet from the bottom up */
                for(int index = 0; index < plc->num_layers && rc == PLCTAG_STATUS_OK; index++) {
                    rc = plc->layers[index].build_request(plc->layers[index].context, plc->data, plc->data_capacity, &data_start, &data_end, &req_num);
                }

                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Error, %s, while preparing PLC protocol stack!", plc_tag_decode_error(rc));
                    break;
                }

                pdebug(DEBUG_DETAIL, "Request packet after fix up:");
                pdebug_dump_bytes(DEBUG_DETAIL, plc->data, data_start);

                request = request->next;
            } while((data_start != data_end) && request);

            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error, %s, while preparing PLC request!", plc_tag_decode_error(rc));
                break;
            }

            /* queue the send. */
            plc->payload_end = data_end;
            plc->payload_start = 0;

            rc = socket_callback_when_write_ready(plc->socket, write_plc_request_callback, plc);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error, %s, while setting up write callback!", plc_tag_decode_error(rc));
                break;
            }
        } while(0);

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



void write_plc_request_callback(void *plc_arg)
{
    int rc = PLCTAG_STATUS_OK;
    plc_p plc = (plc_p)plc_arg;

    pdebug(DEBUG_INFO, "Starting for plc %s.", plc->key);

    critical_block(plc->plc_mutex) {

        /* multiple things to check that can change. */
        do {
            /* do we have data to send? */
            if(plc->payload_start < plc->payload_end) {
                int bytes_written = 0;

                /* We have data to write. */
                bytes_written = socket_tcp_write(plc->socket, plc->data + plc->payload_start, plc->payload_end - plc->payload_start);
                if(bytes_written < 0) {
                    /* error sending data! */
                    rc = bytes_written;

                    pdebug(DEBUG_WARN, "Error, %s, sending data!", plc_tag_decode_error(rc));
                    break;
                }

                plc->payload_start += bytes_written;

                /* did we write all the data? */
                if(plc->payload_start < plc->payload_end) {
                    /* no, so queue up another write. */
                    rc = socket_callback_when_write_ready(plc->socket, write_plc_request_callback, plc);
                    if(rc != PLCTAG_STATUS_OK) {
                        pdebug(DEBUG_WARN, "Error, %s, setting up callback for socket write!", plc_tag_decode_error(rc));
                        break;
                    }
                }
            }

            /* did we write it all? */
            if(plc->payload_start >= plc->payload_end) {
                /* yes, so wait for the response. */
                plc->payload_start = 0;
                plc->payload_end = 0;

                /* set up the idle timeout */
                start_idle_timeout_unsafe(plc);

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




void process_plc_request_response(void *plc_arg)
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
        bytes_read = socket_tcp_read(plc->socket, plc->data + plc->payload_end, plc->data_capacity - plc->payload_end);
        if(bytes_read < 0) {
            rc = bytes_read;
            pdebug(DEBUG_WARN, "Error, %s, while trying to read socket!", plc_tag_decode_error(rc));
            break;
        }

        plc->payload_end += bytes_read;

        /* possibly loop over the requests. */
        do {
            data_start = 0;
            data_end = plc->payload_end;
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
                } else {
                    /* we are not done, make sure the error is understood. */
                    rc = PLCTAG_ERR_PARTIAL;
                }

                break;
            } else if(rc != PLCTAG_STATUS_OK) {
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
        } else if(rc == PLCTAG_ERR_PARTIAL) {
            pdebug(DEBUG_INFO, "Got incomplete response packet, waiting for more data.");
        } else {
            pdebug(DEBUG_WARN, "Error, %s, while trying to process response packet!", plc_tag_decode_error(rc));

            plc->request_in_flight = FALSE;

            /* tear down the stack. */
            start_disconnecting_unsafe(plc);

            break;
        }
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Done with error %s while trying to process response packet for PLC %s!", plc_tag_decode_error(rc), plc->key);
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

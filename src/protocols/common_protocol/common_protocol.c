/***************************************************************************
 *   Copyright (C) 2022 by Kyle Hayes                                      *
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
#include <lib/tag.h>
#include <common_protocol/common_protocol.h>
#include <util/atomic_int.h>


typedef enum {
    COMMON_DEVICE_STATE_CONNECT = 0,
    COMMON_DEVICE_STATE_READY,
    COMMON_DEVICE_STATE_BUILD_REQUEST,
    COMMON_DEVICE_STATE_SEND_REQUEST,
    COMMON_DEVICE_STATE_RECEIVE_RESPONSE,
    COMMON_DEVICE_STATE_DISCONNECT,
    COMMON_DEVICE_STATE_ERR_WAIT
} common_plc_state_t;


/* module-level data */
atomic_int library_terminating = {0};
mutex_p common_protocol_mutex = NULL;
common_protocol_device_p devices = NULL;


/* local functions */
static THREAD_FUNC(common_device_handler);
static int tickle_tag_unsafe(common_device_p device, common_tag_p tag);
static int tickle_all_tags(common_device_p device);
static int remove_tag(common_tag_list_p list, common_tag_p tag);
static struct common_tag_list_t merge_lists(common_tag_list_p first, common_tag_list_p last);
static void push_tag(common_tag_list_p list, common_tag_p tag);
static common_tag_p pop_tag(common_tag_list_p list);



/* Module entry points */
int common_protocol_init(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    pdebug(DEBUG_DETAIL, "Setting up mutex.");
    if(!common_protocol_mutex) {
        rc = mutex_create(&common_protocol_mutex);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error %s creating mutex!", plc_tag_decode_error(rc));
            return rc;
        }
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}



void common_protocol_teardown(void)
{
    pdebug(DEBUG_INFO, "Starting.");

    /* flag library termination */
    atomic_set(&library_terminating, 1);

    /* start the shutdown process for all active devices/threads */
    if(devices) {
        if(common_protocol_mutex) {
            critical_block(common_protocol_mutex) {
                for(common_device_p device = devices; device; device = device->next) {
                    atomic_set(&(device->terminate), 1);
                }
            }

            /* wait for all to stop. */
            while(devices) {
                sleep_ms(10);
            }
        } else {
            pdebug(DEBUG_WARN, "Devices list is not empty, but mutex is null!");
        }
    }


    if(common_device_mutex) {
        pdebug(DEBUG_DETAIL, "Destroying common device module mutex.");
        mutex_destroy(&common_device_mutex);
        common_device_mutex = NULL;
    }
    pdebug(DEBUG_DETAIL, "Common device module mutex destroyed.");

    /* allow things to restart later. */
    atomic_set(&library_terminating, 0);

    pdebug(DEBUG_INFO, "Done.");
}




int common_protocol_get_device(const char *server_name, int connection_group_id, common_device_p **device, common_device_p (*create_device)(void *arg), void *create_arg)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    if(atomic_get(&library_terminating)) {
        pdebug(DEBUG_WARN, "Library is terminating!");
        return PLCTAG_ERR_NOT_ALLOWED;
    }

    /* see if we can find a matching server. */
    critical_block(common_protocol_mutex) {
        common_device_p *walker = &devices;

        while(*walker && (*walker)->connection_group_id != connection_group_id && str_cmp_i(server_name, (*walker)->server_name) != 0) {
            walker = &((*walker)->next);
        }

        /* did we find one? */
        if(*walker && (*walker)->connection_group_id == connection_group_id && str_cmp_i(server_name, (*walker)->server_name) == 0) {
            pdebug(DEBUG_DETAIL, "Using existing PLC connection.");
            *device = rc_inc(*walker);
            is_new = 0;
        } else {
            /* nope, make a new one.  Do as little as possible in the mutex. */

            pdebug(DEBUG_DETAIL, "Creating new PLC connection.");

            is_new = 1;

            /* call the passed function to create a new device */
            *device = create_device(create_arg);
            if(*device) {
                pdebug(DEBUG_DETAIL, "Setting connection_group_id to %d.", connection_group_id);
                (*device)->connection_group_id = connection_group_id;

                /* copy the server string so that we can find this again. */
                pdebug(DEBUG_DETAIL, "Setting server name to %s.", server_name);
                (*device)->server_name = str_dup(server_name);
                if(! ((*device)->server_name)) {
                    pdebug(DEBUG_WARN, "Unable to allocate Modbus PLC server string!");
                    rc = PLCTAG_ERR_NO_MEM;
                } else {
                    (*device)->tag_list.head = NULL;
                    (*device)->tag_list.tail = NULL;

                    /* create the PLC mutex to protect the tag list. */
                    rc = mutex_create(&((*device)->mutex));
                    if(rc != PLCTAG_STATUS_OK) {
                        pdebug(DEBUG_WARN, "Unable to create new mutex, error %s!", plc_tag_decode_error(rc));
                        break;
                    }

                    /* link up the the PLC into the global list. */
                    (*device)->next = plcs;
                    plcs = *device;

                    /* now the PLC can be found and the tag list is ready for use. */
                }
            } else {
                pdebug(DEBUG_WARN, "Unable to allocate Modbus PLC object!");
                rc = PLCTAG_ERR_NO_MEM;
            }
        }
    }

    /* if everything went well and it is new, set up the new PLC. */
    if(rc == PLCTAG_STATUS_OK) {
        if(is_new) {
            pdebug(DEBUG_INFO, "Creating new PLC.");

            do {
                /* set up the PLC state */
                (*device)->state = COMMON_DEVICE_STATE_CONNECT;

                rc = thread_create(&((*device)->handler_thread), common_device_handler, 32768, (void *)(*device));
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Unable to create new handler thread, error %s!", plc_tag_decode_error(rc));
                    break;
                }

                pdebug(DEBUG_DETAIL, "Created thread %p.", (*device)->handler_thread);
            } while(0);
        }
    }

    if(rc != PLCTAG_STATUS_OK && *device) {
        pdebug(DEBUG_WARN, "PLC lookup and/or creation failed!");

        /* clean up. */
        *device = rc_dec(*device);
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}



/* never enter this from within the handler thread itself! */
void common_device_destroy(common_device_p device)
{
    pdebug(DEBUG_INFO, "Starting.");

    if(!device) {
        pdebug(DEBUG_WARN, "Destructor called with null pointer!");
        return;
    }

    /* remove the device from the list. */
    critical_block(common_protocol_mutex) {
        common_device_p *walker = &devices;

        while(*walker && *walker != device) {
            walker = &((*walker)->next);
        }

        if(*walker) {
            /* unlink the list. */
            *walker = device->next;
            device->next = NULL;
        } else {
            pdebug(DEBUG_WARN, "Device not found in the list!");
        }
    }

    /* shut down the thread. */
    if(device->handler_thread) {
        pdebug(DEBUG_DETAIL, "Terminating common device handler thread %p.", device->handler_thread);

        /* set the flag to cause the thread to terminate. */
        atomic_set(&(device->terminate), 1);

        /* signal the device to wake the thread. */
        device->vtable->wake_device(device);

        /* wait for the thread to terminate and destroy it. */
        thread_join(device->handler_thread);
        thread_destroy(&device->handler_thread);

        device->handler_thread = NULL;
    }

    if(device->mutex) {
        mutex_destroy(&device->mutex);
        device->mutex = NULL;
    }

    if(device->server_name) {
        mem_free(device->server_name);
        device->server = NULL;
    }

    /* check to make sure we have no tags left. */
    if(device->tag_list.head) {
        pdebug(DEBUG_WARN, "There are tags still remaining in the tag list, memory leak possible!");
    }

    pdebug(DEBUG_INFO, "Done.");
}



int common_tag_init(common_tag_p tag, attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata), void *userdata)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    /* fill in the core tag info. */
    rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to initialize generic tag parts!");
        rc_dec(tag);
        return (plc_tag_p)NULL;
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}



void common_tag_destructor(common_tag_p tag)
{
    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Destructor called with null pointer!");
        return;
    }

    /* abort everything. */
    common_tag_abort((plc_tag_p)tag);

    if(tag->device) {
        /* unlink the tag from the PLC. */
        critical_block(tag->device->mutex) {
            int rc = remove_tag(&(tag->device->tag_list), tag);
            if(rc == PLCTAG_STATUS_OK) {
                pdebug(DEBUG_DETAIL, "Tag removed from the PLC successfully.");
            } else if(rc == PLCTAG_ERR_NOT_FOUND) {
                pdebug(DEBUG_WARN, "Tag not found in the PLC's list for the tag's operation %d.", tag->op);
            } else {
                pdebug(DEBUG_WARN, "Error %s while trying to remove the tag from the PLC's list!", plc_tag_decode_error(rc));
            }
        }

        pdebug(DEBUG_DETAIL, "Releasing the reference to the PLC.");
        tag->device = rc_dec(tag->device);
    }

    if(tag->api_mutex) {
        mutex_destroy(&(tag->api_mutex));
        tag->api_mutex = NULL;
    }

    if(tag->ext_mutex) {
        mutex_destroy(&(tag->ext_mutex));
        tag->ext_mutex = NULL;
    }

    if(tag->tag_cond_wait) {
        cond_destroy(&(tag->tag_cond_wait));
        tag->tag_cond_wait = NULL;
    }

    if(tag->byte_order && tag->byte_order->is_allocated) {
        mem_free(tag->byte_order);
        tag->byte_order = NULL;
    }

    pdebug(DEBUG_INFO, "Done.");
}



/*************************************/
/* internal implementation functions */
/*************************************/


THREAD_FUNC(common_device_handler)
{
    int rc = PLCTAG_STATUS_OK;
    common_device_p device = (common_device_p)arg;
    int error_retries = 0;
    int failed_attempts = 0;

    pdebug(DEBUG_INFO, "Starting.");

    if(!device) {
        pdebug(DEBUG_WARN, "Null device pointer passed!");
        THREAD_RETURN(0);
    }

    while(! atomic_get(&(device->terminate))) {
        rc = tickle_all_tags(device);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error %s tickling tags!", plc_tag_decode_error(rc));
            /* FIXME - what should we do here? */
        }

        // /* if there is still a response marked ready, clean it up. */
        // if(device->flags.response_ready) {
        //     pdebug(DEBUG_DETAIL, "Orphan response found.");
        //     device->flags.response_ready = 0;
        //     device->read_data_len = 0;
        // }

        switch(device->state) {
        case COMMON_DEVICE_STATE_CONNECT:
            pdebug(DEBUG_DETAIL, "in COMMON_DEVICE_STATE_CONNECT state.");

            /* connect to the device.  Will likely block for a while. */
            rc = device->vtable->connect_to_device(device);
            if(rc == PLCTAG_STATUS_PENDING) {
                pdebug(DEBUG_DETAIL, "Staying in state COMMON_DEVICE_STATE_CONNECT to finish connection process.");
                device->state = COMMON_DEVICE_STATE_CONNECT;
            } else if(rc == PLCTAG_STATUS_OK) {
                pdebug(DEBUG_DETAIL, "Successfully connected to the device.  Going to COMMON_DEVICE_STATE_READY state.");

                /* reset err_delay */
                err_delay = PLC_SOCKET_ERR_START_DELAY;

                failed_attempts = 0;

                device->state = COMMON_DEVICE_STATE_READY;
            } else {
                pdebug(DEBUG_WARN, "Error %s received while starting socket connection.", plc_tag_decode_error(rc));

                socket_destroy(&(device->sock));

                /* exponential increase with jitter. */
                UPDATE_ERR_DELAY();

                pdebug(DEBUG_WARN, "Unable to connect to the PLC, will retry later! Going to COMMON_DEVICE_STATE_ERR_WAIT state to wait %"PRId64"ms.", err_delay);

                error_retries = failed_attempts;
                if(++failed_attempts > 10) {
                    failed_attempts = 10;
                }

                pdebug(DEBUG_DETAIL, "Current failed attempt count: %d.", failed_attempts);

                device->state = COMMON_DEVICE_STATE_ERR_WAIT;
            }
            break;

        case COMMON_DEVICE_STATE_READY:
            pdebug(DEBUG_DETAIL, "in COMMON_DEVICE_STATE_READY state.");

            // /* calculate what events we should be waiting for. */
            // waitable_events = SOCK_EVENT_DEFAULT_MASK | COMMON_DEVICE_EVENT_READ_READY;

            // /* if there is a request queued for sending, send it. */
            // if(device->flags.request_ready) {
            //     waitable_events |= COMMON_DEVICE_EVENT_WRITE_READY;
            // }

            /* this will wait if nothing wakes it up or until it times out. */
            device_events = device->vtable->wait_for_events(device, COMMON_DEVICE_IDLE_WAIT_TIMEOUT);

            /* check for socket errors or disconnects. */
            if((device_events & COMMON_DEVICE_EVENT_ERROR) || (device_events & COMMON_DEVICE_EVENT_DISCONNECT)) {
                if(device_events & COMMON_DEVICE_EVENT_DISCONNECT) {
                    pdebug(DEBUG_WARN, "Unexepected device disconnect!");
                } else {
                    pdebug(DEBUG_WARN, "Unexpected device error!");
                }

                pdebug(DEBUG_WARN, "Going to state COMMON_DEVICE_STATE_DISCONNECT");

                device->state = COMMON_DEVICE_STATE_DISCONNECT;
                break;
            }

            /* preference pushing requests to the PLC */
            if(device_events & COMMON_DEVICE_EVENT_WRITE_READY) {
                pdebug(DEBUG_DETAIL, "There is a request ready to send and we can send, going to state COMMON_DEVICE_STATE_SEND_REQUEST.");
                device->state = COMMON_DEVICE_STATE_SEND_REQUEST;
                break;
            }

            if(device_events & COMMON_DEVICE_EVENT_READ_READY) {
                pdebug(DEBUG_DETAIL, "We can receive a response going to state COMMON_DEVICE_STATE_RECEIVE_RESPONSE.");
                device->state = COMMON_DEVICE_STATE_RECEIVE_RESPONSE;
                break;
            }

            if(device_events & COMMON_DEVICE_EVENT_TIMEOUT) {
                pdebug(DEBUG_DETAIL, "Timed out waiting for something to happen.");
                /* just stay here in this state */
            }

            if(device_events & COMMON_DEVICE_EVENT_WAKEUP) {
                pdebug(DEBUG_DETAIL, "Someone woke us up.");
                /* stay in this state. */
            }

            break;

        case COMMON_DEVICE_STATE_SEND_REQUEST:
            pdebug(DEBUG_DETAIL, "in COMMON_DEVICE_STATE_SEND_REQUEST state.");

            /* send a PDU, this may block */
            rc = device->vtable->send_request(device);
            if(rc == PLCTAG_STATUS_OK) {
                pdebug(DEBUG_DETAIL, "Request sent, going to back to state COMMON_DEVICE_STATE_READY.");

                device->state = COMMON_DEVICE_STATE_READY;
            } else if(rc == PLCTAG_STATUS_PENDING) {
                pdebug(DEBUG_DETAIL, "Not all data written, will try again.");
                /* if we did not send all the packet, we stay in this state and keep trying. */
            } else {
                pdebug(DEBUG_WARN, "Closing device connection due to write error %s.", plc_tag_decode_error(rc));

                /* try to disconnect. */
                device->state = COMMON_DEVICE_STATE_DISCONNECT;
            }

            break;


        case COMMON_DEVICE_STATE_RECEIVE_RESPONSE:
            pdebug(DEBUG_DETAIL, "in COMMON_DEVICE_STATE_RECEIVE_RESPONSE state.");

            /* get a PDU, this may block */
            rc = device->vtable->receive_response(device);
            if(rc == PLCTAG_STATUS_OK) {
                pdebug(DEBUG_DETAIL, "Response ready, going back to COMMON_DEVICE_STATE_READY state.");
                device->state = COMMON_DEVICE_STATE_READY;
            } else if(rc == PLCTAG_STATUS_PENDING) {
                pdebug(DEBUG_DETAIL, "Response not complete, continue reading data.");
            } else {
                pdebug(DEBUG_WARN, "Closing device due to read error %s.", plc_tag_decode_error(rc));

                /* try to disconnect. */
                device->state = COMMON_DEVICE_STATE_DISCONNECT;
            }

            break;


        case COMMON_DEVICE_STATE_DISCONNECT:
            pdebug(DEBUG_DETAIL, "in COMMON_DEVICE_STATE_DISCONNECT state.");

            rc = device->vtable->disconnect_from_device(device);
            if(rc == PLCTAG_STATUS_OK) {
                pdebug(DEBUG_DETAIL, "Disconnect complete, going to COMMON_DEVICE_STATE_CONNECT state.");
                device->state = COMMON_DEVICE_STATE_CONNECT;
            } else if(rc == PLCTAG_STATUS_PENDING) {
                pdebug(DEBUG_DETAIL, "Disconnect not complete, continue trying.");
            } else {
                pdebug(DEBUG_WARN, "Disconnect failed due to error %s.", plc_tag_decode_error(rc));

                /* try to disconnect. */
                device->state = COMMON_DEVICE_STATE_ERR_WAIT;
            }

            break;

        case COMMON_DEVICE_STATE_ERR_WAIT:
            pdebug(DEBUG_DETAIL, "in COMMON_DEVICE_STATE_ERR_WAIT state.");

            if(err_retries > 0) {
                pdebug(DEBUG_DETAIL, "Error wait retry %d.", err_retries);
                sleep_ms(COMMON_DEVICE_ERR_WAIT_INCREMENT_MS);  
                error_retries--;           
            } else {
                pdebug(DEBUG_DETAIL, "Done with error wait, retrying device connection.");
                device->state = COMMON_DEVICE_STATE_CONNECT;
            }

            break;

        default:
            pdebug(DEBUG_WARN, "Unknown state %d!", device->state);
            device->state = COMMON_DEVICE_STATE_DISCONNECT;
            break;
        }

        /* wait if needed, could be signalled already. */
        //cond_wait(device->wait_cond, COMMON_DEVICE_IDLE_WAIT_TIMEOUT);
    }

    pdebug(DEBUG_INFO, "Done.");

    THREAD_RETURN(0);
}


int tickle_all_tags(common_device_p device)
{
    int rc = PLCTAG_STATUS_OK;
    struct common_tag_list_t idle_list = { NULL, NULL };
    struct common_tag_list_t active_list = { NULL, NULL };
    common_tag_p tag = NULL;

    pdebug(DEBUG_DETAIL, "Starting.");

    /*
     * The mutex prevents the list from changing, the PLC
     * from being freed, and the tags from being freed.
     */
    critical_block(device->mutex) {
        while((tag = pop_tag(&(device->tag_list)))) {
            int tag_pushed = 0;

            debug_set_tag_id(tag->tag_id);

            /* make sure nothing else can modify the tag while we are */
            critical_block(tag->api_mutex) {
                rc = tickle_tag_unsafe(device, tag);
                if(rc == PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_SPEW, "Pushing tag onto idle list.");
                    push_tag(&idle_list, tag);
                    tag_pushed = 1;
                } else if(rc == PLCTAG_STATUS_PENDING) {
                    pdebug(DEBUG_SPEW, "Pushing tag onto active list.");
                    push_tag(&active_list, tag);
                    tag_pushed = 1;
                } else {
                    pdebug(DEBUG_WARN, "Error %s tickling tag! Pushing tag onto idle list.", plc_tag_decode_error(rc));
                    push_tag(&idle_list, tag);
                    tag_pushed = 1;
                }
            }

            if(tag_pushed) {
                /* call the callbacks outside the API mutex. */
                plc_tag_generic_handle_event_callbacks((plc_tag_p)tag);
            } else {
                pdebug(DEBUG_WARN, "Tag mutex not taken!  Doing emergency push of tag onto idle list.");
                push_tag(&idle_list, tag);
            }

            rc = PLCTAG_STATUS_OK;

            debug_set_tag_id(0);
        }

        /* merge the lists and replace the old list. */
        pdebug(DEBUG_SPEW, "Merging active and idle lists.");
        device->tag_list = merge_lists(&active_list, &idle_list);
    }

    pdebug(DEBUG_DETAIL, "Done: %s", plc_tag_decode_error(rc));

    return rc;
}



int tickle_tag_unsafe(common_device_p device, common_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    int op = (int)(tag->op);
    int raise_event = 0;
    int event;
    int event_status;

    pdebug(DEBUG_SPEW, "Starting.");

    switch(op) {
        case TAG_OP_IDLE:
            pdebug(DEBUG_SPEW, "Tag is idle.");
            rc = PLCTAG_STATUS_OK;
            break;

        case TAG_OP_READ_REQUEST:
            rc = tag->common_vtable->create_read_request(tag);
            if(rc == PLCTAG_STATUS_OK) {
                pdebug(DEBUG_DETAIL, "Read request created.");

                /* the tag should wait for the response. */
                tag->op = TAG_OP_READ_RESPONSE;

                rc = PLCTAG_STATUS_PENDING;
            } else if(rc == PLCTAG_ERR_NO_RESOURCES) {
                pdebug(DEBUG_SPEW, "Insufficient resources to build tag request.");
                rc = PLCTAG_STATUS_PENDING;
            } else {
                pdebug(DEBUG_WARN, "Error %s creating read request!", plc_tag_decode_error(rc));

                tag->op = TAG_OP_IDLE;
                tag->read_complete = 1;
                tag->read_in_flight = 0;
                tag->status = (int8_t)rc;

                raise_event = 1;
                event = PLCTAG_EVENT_READ_COMPLETED;
                event_status = rc;

                plc_tag_generic_wake_tag((plc_tag_p)tag);
                rc = PLCTAG_STATUS_OK;
            }

            break;

        case TAG_OP_READ_RESPONSE:
            /* cross check the state. */
            if(device->state != COMMON_DEVICE_STATE_RECEIVE_RESPONSE) {
                pdebug(DEBUG_WARN, "Device changed state, restarting request.");
                tag->op = TAG_OP_READ_REQUEST;
                break;
            }

            rc = tag->common_vtable->check_read_response(tag);
            switch(rc) {
                case PLCTAG_ERR_PARTIAL:
                    /* partial response, keep going */
                    pdebug(DEBUG_DETAIL, "Found our response, but we are not done.");

                    tag->op = TAG_OP_READ_REQUEST;

                    rc = PLCTAG_STATUS_OK;
                    break;

                case PLCTAG_ERR_NO_MATCH:
                    pdebug(DEBUG_SPEW, "Not our response.");
                    rc = PLCTAG_STATUS_PENDING;
                    break;

                case PLCTAG_STATUS_OK:
                    /* fall through */
                default:
                    /* set the status before we might change it. */
                    tag->status = (int8_t)rc;

                    if(rc == PLCTAG_STATUS_OK) {
                        pdebug(DEBUG_DETAIL, "Found our response.");
                    } else {
                        pdebug(DEBUG_WARN, "Error %s checking read response!", plc_tag_decode_error(rc));
                        rc = PLCTAG_STATUS_OK;
                    }

                    tag->op = TAG_OP_IDLE;
                    tag->read_in_flight = 0;
                    tag->read_complete = 1;
                    tag->status = (int8_t)rc;

                    raise_event = 1;
                    event = PLCTAG_EVENT_READ_COMPLETED;
                    event_status = rc;

                    /* tell the world we are done. */
                    plc_tag_generic_wake_tag((plc_tag_p)tag);

                    break;
            }

            break;

        case TAG_OP_WRITE_REQUEST:
            rc = tag->common_vtable->create_write_request(tag);
            if(rc == PLCTAG_STATUS_OK) {
                pdebug(DEBUG_DETAIL, "Write request created.");

                tag->op = TAG_OP_WRITE_RESPONSE;

                rc = PLCTAG_STATUS_PENDING;
            } else if(rc == PLCTAG_ERR_NO_RESOURCES) {
                pdebug(DEBUG_SPEW, "Insufficient resources for new request.")
                rc = PLCTAG_STATUS_PENDING;
            } else {
                pdebug(DEBUG_WARN, "Error %s creating write request!", plc_tag_decode_error(rc));

                tag->op = TAG_OP_IDLE;
                tag->write_complete = 1;
                tag->write_in_flight = 0;
                tag->status = (int8_t)rc;

                raise_event = 1;
                event = PLCTAG_EVENT_WRITE_COMPLETED;
                event_status = rc;

                //plc_tag_tickler_wake();
                plc_tag_generic_wake_tag((plc_tag_p)tag);

                rc = PLCTAG_STATUS_OK;
            }

            break;

        case TAG_OP_WRITE_RESPONSE:
            /* check the state. */
            if(device->state != COMMON_DEVICE_STATE_RECEIVE_RESPONSE) {
                pdebug(DEBUG_WARN, "Device changed state, restarting request.");
                tag->op = TAG_OP_WRITE_REQUEST;
                break;
            }

            rc = tag->common_vtable->check_write_response(device, tag);
            if(rc == PLCTAG_STATUS_OK) {
                pdebug(DEBUG_DETAIL, "Found our response.");

                tag->op = TAG_OP_IDLE;
                tag->write_complete = 1;
                tag->write_in_flight = 0;
                tag->status = (int8_t)rc;

                raise_event = 1;
                event = PLCTAG_EVENT_WRITE_COMPLETED;
                event_status = rc;

                /* tell the world we are done. */
                plc_tag_generic_wake_tag((plc_tag_p)tag);

                rc = PLCTAG_STATUS_OK;
            } else if(rc == PLCTAG_ERR_PARTIAL) {
                pdebug(DEBUG_DETAIL, "Found our response, but we are not done.");

                tag->op = TAG_OP_WRITE_REQUEST;

                rc = PLCTAG_STATUS_OK;
            } else if(rc == PLCTAG_ERR_NO_MATCH) {
                pdebug(DEBUG_SPEW, "Not our response.");
                rc = PLCTAG_STATUS_PENDING;
            } else {
                pdebug(DEBUG_WARN, "Error %s checking write response!", plc_tag_decode_error(rc));

                tag->op = TAG_OP_IDLE;
                tag->write_complete = 1;
                tag->write_in_flight = 0;
                tag->status = (int8_t)rc;

                raise_event = 1;
                event = PLCTAG_EVENT_WRITE_COMPLETED;
                event_status = rc;

                /* tell the world we are done. */
                plc_tag_generic_wake_tag((plc_tag_p)tag);

                rc = PLCTAG_STATUS_OK;
            }

            break;

        default:
            pdebug(DEBUG_WARN, "Unknown tag operation %d!", op);

            tag->op = TAG_OP_IDLE;
            tag->status = (int8_t)PLCTAG_ERR_NOT_IMPLEMENTED;

            /* tell the world we are done. */
            plc_tag_generic_wake_tag((plc_tag_p)tag);

            rc = PLCTAG_STATUS_OK;
            break;
    }

    if(raise_event) {
        tag_raise_event((plc_tag_p)tag, event, (int8_t)event_status);
        plc_tag_generic_handle_event_callbacks((plc_tag_p)tag);
    }
    
    /*
     * Call the generic tag tickler function to handle auto read/write and set
     * up events.
     */
    plc_tag_generic_tickler((plc_tag_p)tag);

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}



common_tag_p pop_tag(common_tag_list_p list)
{
    common_tag_p tmp = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    tmp = list->head;

    /* unlink */
    if(tmp) {
        list->head = tmp->next;
        tmp->next = NULL;
    }

    /* if we removed the last element, then set the tail to NULL. */
    if(!list->head) {
        list->tail = NULL;
    }

    pdebug(DEBUG_SPEW, "Done.");

    return tmp;
}


void push_tag(common_tag_list_p list, common_tag_p tag)
{
    pdebug(DEBUG_SPEW, "Starting.");

    tag->next = NULL;

    if(list->tail) {
        list->tail->next = tag;
        list->tail = tag;
    } else {
        /* nothing in the list. */
        list->head = tag;
        list->tail = tag;
    }

    pdebug(DEBUG_SPEW, "Done.");
}


struct common_tag_list_t merge_lists(common_tag_list_p first, common_tag_list_p last)
{
    struct common_tag_list_t result;

    pdebug(DEBUG_SPEW, "Starting.");

    if(first->head) {
        common_tag_p tmp = first->head;

        pdebug(DEBUG_SPEW, "First list:");
        while(tmp) {
            pdebug(DEBUG_SPEW, "  tag %d", tmp->tag_id);
            tmp = tmp->next;
        }
    } else {
        pdebug(DEBUG_SPEW, "First list: empty.");
    }

    if(last->head) {
        common_tag_p tmp = last->head;

        pdebug(DEBUG_SPEW, "Second list:");
        while(tmp) {
            pdebug(DEBUG_SPEW, "  tag %d", tmp->tag_id);
            tmp = tmp->next;
        }
    } else {
        pdebug(DEBUG_SPEW, "Second list: empty.");
    }

    /* set up the head of the new list. */
    result.head = (first->head ? first->head : last->head);

    /* stitch up the tail of the first list. */
    if(first->tail) {
        first->tail->next = last->head;
    }

    /* set up the tail of the new list */
    result.tail = (last->tail ? last->tail : first->tail);

    /* make sure the old lists do not reference the tags. */
    first->head = NULL;
    first->tail = NULL;
    last->head = NULL;
    last->tail = NULL;

    if(result.head) {
        common_tag_p tmp = result.head;

        pdebug(DEBUG_SPEW, "Result list:");
        while(tmp) {
            pdebug(DEBUG_SPEW, "  tag %d", tmp->tag_id);
            tmp = tmp->next;
        }
    } else {
        pdebug(DEBUG_SPEW, "Result list: empty.");
    }

    pdebug(DEBUG_SPEW, "Done.");

    return result;
}



int remove_tag(common_tag_list_p list, common_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    common_tag_p cur = list->head;
    common_tag_p prev = NULL;

    pdebug(DEBUG_DETAIL, "Starting.");

    while(cur && cur != tag) {
        prev = cur;
        cur = cur->next;
    }

    if(cur == tag) {
        /* found it. */
        if(prev) {
            prev->next = tag->next;
        } else {
            /* at the head of the list. */
            list->head = tag->next;
        }

        if(list->tail == tag) {
            list->tail = prev;
        }

        rc = PLCTAG_STATUS_OK;
    } else {
        rc = PLCTAG_STATUS_PENDING;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


/****** Tag Control Functions ******/

/* These must all be called with the API mutex on the tag held. */
int common_tag_abort(plc_tag_p p_tag)
{
    common_tag_p tag = (common_tag_p)p_tag;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    tag->status = (int8_t)PLCTAG_STATUS_OK;
    tag->op = TAG_OP_IDLE;

    /* wake the device. */
    tag->device->vtable->wake(device);

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}



int common_tag_read_start(plc_tag_p p_tag)
{
    common_tag_p tag = (common_tag_p)p_tag;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(tag->op == TAG_OP_IDLE) {
        tag->op = TAG_OP_READ_REQUEST;
    } else {
        pdebug(DEBUG_WARN, "Operation in progress!");
        return PLCTAG_ERR_BUSY;
    }

    /* wake the PLC loop if we need to. */
    tag->device->vtable->wake(device);

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_PENDING;
}



int common_tag_status(plc_tag_p p_tag)
{
    common_tag_p tag = (common_tag_p)p_tag;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(tag->status != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Status not OK, returning %s.", plc_tag_decode_error(tag->status));
        return tag->status;
    }

    if(tag->op != TAG_OP_IDLE) {
        pdebug(DEBUG_DETAIL, "Operation in progress, returning PLCTAG_STATUS_PENDING.");
        return PLCTAG_STATUS_PENDING;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}



/* not used. */
int common_tag_tickler(plc_tag_p p_tag)
{
    (void)p_tag;

    return PLCTAG_STATUS_OK;
}




int common_tag_write_start(plc_tag_p p_tag)
{
    common_tag_p tag = (common_tag_p)p_tag;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(tag->op == TAG_OP_IDLE) {
        tag->op = TAG_OP_WRITE_REQUEST;
    } else {
        pdebug(DEBUG_WARN, "Operation in progress!");
        return PLCTAG_ERR_BUSY;
    }

    /* wake the device thread. */
    tag->device->vtable->wake(device);

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_PENDING;
}



int common_tag_wake_device(plc_tag_p p_tag)
{
    common_tag_p tag = (common_tag_p)p_tag;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* wake the device thread. */
    tag->device->vtable->wake(device);

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_PENDING;
}

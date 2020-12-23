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
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <pthread.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <time.h>

#include <lib/libplctag.h>
#include <util/atomic_int.h>
#include <util/debug.h>
#include <util/rc.h>



#if defined(__APPLE__) || defined(__FreeBSD__) ||  defined(__NetBSD__) || defined(__OpenBSD__) || defined(__bsdi__) || defined(__DragonFly__)
    #define BSD_OS_TYPE
    #if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
        #define _DARWIN_C_SOURCE _POSIX_C_SOURCE
    #endif
#endif



/***************************************************************************
 ******************************* Globals ***********************************
 **************************************************************************/


#define TICKLER_MAX_WAIT_TIME_MS (50)

static thread_p platform_thread = NULL;
static atomic_int library_shutdown = ATOMIC_INT_STATIC_INIT(0);

static mutex_p timer_mutex = NULL;
static timer_p timer_list = NULL;

static mutex_p socket_mutex = NULL;
static sock_p socket_list = NULL;
static sock_p *socket_list_iter = NULL;
static fd_set global_read_fds;
static fd_set global_write_fds;
static int max_socket_fd = -1;
static atomic_int need_recalculate_fd_sets = ATOMIC_INT_STATIC_INIT(1);

static THREAD_FUNC(platform_thread_func);

int pipe_fds[2];

/* used to wake the timer/socket thread. */
static void wake_up_event_loop(void);


/***************************************************************************
 ******************************* Memory ************************************
 **************************************************************************/




/***************************************************************************
 ******************************* Strings ***********************************
 **************************************************************************/



/***************************************************************************
 ******************************* Mutexes ***********************************
 **************************************************************************/


// /***************************************************************************
//  *************************** Condition Variables ***************************
//  **************************************************************************/

// struct condition_var_s {
//     pthread_mutex_t p_mutex;
//     pthread_cond_t p_cond;
//     int initialized;
// };



// int condition_var_create(condition_var_p *var)
// {
//     pdebug(DEBUG_DETAIL, "Starting.");

//     if(*var) {
//         pdebug(DEBUG_WARN, "Called with non-NULL pointer!");
//     }

//     *var = (struct condition_var_s *)mem_alloc(sizeof(struct condition_var_s));

//     if(! *var) {
//         pdebug(DEBUG_ERROR,"null mutex pointer.");
//         return PLCTAG_ERR_NULL_PTR;
//     }

//     if(pthread_mutex_init(&((*var)->p_mutex),NULL)) {
//         mem_free(*var);
//         *var = NULL;
//         pdebug(DEBUG_ERROR,"Error initializing pthread mutex.");
//         return PLCTAG_ERR_CREATE;
//     }

//     if(pthread_cond_init(&((*var)->p_cond), NULL)) {
//         pthread_mutex_destroy(&((*var)->p_mutex));
//         mem_free(*var);
//         *var = NULL;
//         pdebug(DEBUG_ERROR,"Error initializing pthread condition variable.");
//         return PLCTAG_ERR_CREATE;
//     }

//     (*var)->initialized = 1;

//     pdebug(DEBUG_DETAIL, "Done creating condition variable %p.", *var);

//     return PLCTAG_STATUS_OK;
// }



// int condition_var_destroy(condition_var_p *var)
// {
//     pdebug(DEBUG_DETAIL, "Starting to destroy condition variable %p.", var);

//     if(!var || !*var) {
//         pdebug(DEBUG_WARN, "null condition variable pointer.");
//         return PLCTAG_ERR_NULL_PTR;
//     }

//     if(pthread_mutex_destroy(&((*var)->p_mutex))) {
//         pdebug(DEBUG_WARN, "error while attempting to destroy pthread mutex.");
//         return PLCTAG_ERR_MUTEX_DESTROY;
//     }

//     if(pthread_cond_destroy(&((*var)->p_cond))) {
//         pdebug(DEBUG_WARN, "error while attempting to destroy pthread condition variable.");
//         return PLCTAG_ERR_MUTEX_DESTROY;
//     }

//     mem_free(*var);

//     *var = NULL;

//     pdebug(DEBUG_DETAIL, "Done.");

//     return PLCTAG_STATUS_OK;
// }


// int condition_var_wait_impl(const char *func, int line, condition_var_p var, int64_t timeout_wake_time)
// {
//     int rc = PLCTAG_STATUS_OK;
//     struct timespec abstime;

//     pdebug(DEBUG_SPEW,"Waiting on condition var %p, called from %s:%d.", var, func, line);

//     if(!var) {
//         pdebug(DEBUG_WARN, "Null condition var pointer!");
//         return PLCTAG_ERR_NULL_PTR;
//     }

//     if(!var->initialized) {
//         return PLCTAG_ERR_BAD_STATUS;
//     }

//     if(timeout_wake_time <= 0) {
//         pdebug(DEBUG_WARN, "Illegal wake up time.  Time must be positive!");
//         return PLCTAG_ERR_BAD_PARAM;
//     }

//     abstime.tv_sec = timeout_wake_time / 1000;
//     abstime.tv_nsec = (timeout_wake_time % 1000) * (int64_t)1000000;

//     /* first lock the mutex */
//     if(pthread_mutex_lock(&(var->p_mutex))) {
//         pdebug(DEBUG_WARN, "error locking mutex.");
//         return PLCTAG_ERR_MUTEX_LOCK;
//     }

//     /* now wait on the condition variable. */
//     rc = pthread_cond_timedwait(&(var->p_cond), &(var->p_mutex), &abstime);
//     if(rc) {
//         /* either timeout or another error. */
//         if(rc == ETIMEDOUT) {
//             /* timeout, translate the error. */
//             rc = PLCTAG_ERR_TIMEOUT;
//         } else {
//             pdebug(DEBUG_WARN, "Unable to wait on condition variable!");
//             rc = PLCTAG_ERR_MUTEX_LOCK; /* should have a better error. */
//         }
//     } else {
//         /* no error, the condition was signaled.  Perhaps. */
//         rc = PLCTAG_STATUS_OK;
//     }

//     /* try to unlock the mutex. */
//     pthread_mutex_unlock(&(var->p_mutex));

//     pdebug(DEBUG_SPEW,"Done.");

//     return rc;
// }


// int condition_var_signal_impl(const char *func, int line, condition_var_p var)
// {
//     pdebug(DEBUG_SPEW,"trying to signal condition variable %p, called from %s:%d.", var, func, line);

//     if(!var) {
//         pdebug(DEBUG_WARN, "null condition var pointer.");
//         return PLCTAG_ERR_NULL_PTR;
//     }

//     if(!var->initialized) {
//         return PLCTAG_ERR_BAD_STATUS;
//     }

//     if(pthread_cond_signal(&(var->p_cond))) {
//         pdebug(DEBUG_SPEW, "error signaling condition var!");
//         return PLCTAG_ERR_BAD_REPLY;
//     }

//     pdebug(DEBUG_SPEW,"Done.");

//     return PLCTAG_STATUS_OK;
// }



/***************************************************************************
 ******************************* Threads ***********************************
 **************************************************************************/



/***************************************************************************
 ******************************* Atomic Ops ********************************
 **************************************************************************/



/***************************************************************************
 ********************************* Timers **********************************
 **************************************************************************/




/***************************************************************************
 ***************************** Miscellaneous *******************************
 **************************************************************************/







/***************************************************************************
 ******************************* Platform **********************************
 ***************************************************************************/


static int64_t run_timers(int64_t current_time);
static int recalculate_fd_sets(void);
static sock_p socket_list_iter_start(void);
static sock_p socket_list_iter_next(void);
static void socket_list_iter_done(void);
static void socket_list_iter_unlink(void);

/*
 * platform_tickler()
 *
 * This function normally gets called by the platform thread
 * but this can be turned off and this tickler called directly
 * from the client application.   The latter would be used
 * primarily from embedded systems.
 */

void platform_tickler(void)
{
    int recalc_count = 0;
    int64_t wait_time_ms = 0;
    struct timeval timeval_wait;
    int max_fd = 0;
    int num_signaled_fds = 0;
    int64_t current_time = time_ms();
    int64_t next_wake_time = 0;
    fd_set tmp_read_fds;
    fd_set tmp_write_fds;

    pdebug(DEBUG_SPEW, "Starting.");

    /* call the timers, this gets the next wake up time. */
    next_wake_time = run_timers(current_time);

    /* do we need to recalculate the fd set for the sockets? */
    recalc_count = atomic_int_get(&need_recalculate_fd_sets);
    if(recalc_count > 0) {
        max_fd = recalculate_fd_sets();

        /*
         * subtract the number we had before.   Other threads may have
         * incremented the counter since we started this if statement.
         * If so, then we will trigger again.
         *
         * This handles the race condition that the counter changes
         * after we recalculate and before we reset the counter.
         */
        atomic_int_add(&need_recalculate_fd_sets, -recalc_count);
    }

    /* determine the amount of time to wait. */
    wait_time_ms = next_wake_time - time_ms();
    if(wait_time_ms < 0) {
        wait_time_ms = 0;
    }

    if(wait_time_ms > TICKLER_MAX_WAIT_TIME_MS) {
        wait_time_ms = TICKLER_MAX_WAIT_TIME_MS;
    }

    timeval_wait.tv_sec = 0;
    timeval_wait.tv_usec = wait_time_ms * 1000;

    /* copy the fd sets as select is destructive. */
    critical_block(socket_mutex) {
        tmp_read_fds = global_read_fds;
        tmp_write_fds = global_write_fds;
    }

    FD_SET(pipe_fds[0], &tmp_read_fds);

    /* make sure that the maximum fd is at least the pipe fd */
    max_fd = (max_fd < pipe_fds[0] ? pipe_fds[0] : max_fd);

    /* select on the open fds. */
    num_signaled_fds = select(max_fd + 1, &tmp_read_fds, &tmp_write_fds, NULL, &timeval_wait);
    if(num_signaled_fds > 0) {
        pdebug(DEBUG_DETAIL, "Starting to run socket callbacks.");

        /* catch the case that the wake up pipe has data. */
        if(FD_ISSET(pipe_fds[0], &tmp_read_fds)) {
            uint8_t dummy;

            while(read(pipe_fds[0], &dummy, 1) > 0) { }

            num_signaled_fds--;
        }

        if(num_signaled_fds > 0) {
            for(sock_p sock = socket_list_iter_start(); sock; rc_dec(sock), sock = socket_list_iter_next()) {
                if(sock->event_wanted == SOCKET_EVENT_READ && FD_ISSET(sock->fd, &tmp_read_fds) && sock->callback) {
                    sock->callback(sock, sock->event_wanted, sock->context);
                    socket_list_iter_unlink();
                } else if(sock->event_wanted == SOCKET_EVENT_WRITE && FD_ISSET(sock->fd, &tmp_write_fds) && sock->callback) {
                    sock->callback(sock, sock->event_wanted, sock->context);
                    socket_list_iter_unlink();
                }
            }

            socket_list_iter_done();
        }
    }
}



THREAD_FUNC(platform_thread_func)
{
    (void)arg;

    pdebug(DEBUG_INFO, "Starting.");

    while(! atomic_int_get(&library_shutdown)) {
        platform_tickler();
    }

    pdebug(DEBUG_INFO, "Done.");

    THREAD_RETURN(0);
}



int recalculate_fd_sets(void)
{
    pdebug(DEBUG_DETAIL, "Starting.");

    critical_block(socket_mutex) {
        max_socket_fd = -1;

        /* clear out the old fd sets */
        FD_ZERO(&global_read_fds);
        FD_ZERO(&global_write_fds);

        for(sock_p sock = socket_list; sock; sock = sock->next) {
            if(sock->event_wanted == SOCKET_EVENT_READ) {
                FD_SET(sock->fd, &global_read_fds);
                max_socket_fd = (max_socket_fd < sock->fd ? sock->fd : max_socket_fd);
            } else if(sock->event_wanted == SOCKET_EVENT_WRITE) {
                FD_SET(sock->fd, &global_write_fds);
                max_socket_fd = (max_socket_fd < sock->fd ? sock->fd : max_socket_fd);
            }
        }
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return max_socket_fd;
}


sock_p socket_list_iter_start(void)
{
    sock_p result = NULL;

    pdebug(DEBUG_DETAIL, "Starting.");

    critical_block(socket_mutex) {
        socket_list_iter = &socket_list;
        result = rc_inc(*socket_list_iter);
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return result;
}

sock_p socket_list_iter_next(void)
{
    sock_p result = NULL;

    pdebug(DEBUG_DETAIL, "Starting.");

    critical_block(socket_mutex) {
        if(socket_list_iter && *socket_list_iter) {
            socket_list_iter = &((*socket_list_iter)->next);
            result = rc_inc(*socket_list_iter);
        }
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return result;
}


void socket_list_iter_done(void)
{
    pdebug(DEBUG_DETAIL, "Starting.");

    critical_block(socket_mutex) {
        socket_list_iter = NULL;
    }

    pdebug(DEBUG_DETAIL, "Done.");
}


void socket_list_iter_unlink(void)
{
    pdebug(DEBUG_DETAIL, "Starting.");

    critical_block(socket_mutex) {
        if(socket_list_iter && *socket_list_iter) {
            *socket_list_iter = (*socket_list_iter)->next;
        }
    }

    pdebug(DEBUG_DETAIL, "Done.");
}


int platform_init(void)
{
    int rc = PLCTAG_STATUS_OK;
    int flags = 0;

    pdebug(DEBUG_INFO, "Starting.");

    /* open the pipe for waking the select wait. */
    if(pipe(pipe_fds)) {
        pdebug(DEBUG_WARN, "Unable to open waker pipe!");
        return PLCTAG_ERR_BAD_REPLY;
    }

    /* make the read pipe fd non-blocking. */
    if((flags = fcntl(pipe_fds[0], F_GETFL)) < 0) {
        pdebug(DEBUG_WARN, "Unable to get flags of read pipe fd!");
        return PLCTAG_ERR_BAD_REPLY;
    }

    /* set read fd non-blocking */
    flags |= O_NONBLOCK;

    if(fcntl(pipe_fds[0], F_SETFL, flags) < 0) {
        pdebug(DEBUG_WARN, "Unable to set flags of read pipe fd!");
        return PLCTAG_ERR_BAD_REPLY;
    }

    /* make the write pipe fd non-blocking. */
    if((flags = fcntl(pipe_fds[1], F_GETFL)) < 0) {
        pdebug(DEBUG_WARN, "Unable to get flags of write pipe fd!");
        return PLCTAG_ERR_BAD_REPLY;
    }

    /* set write fd non-blocking */
    flags |= O_NONBLOCK;

    if(fcntl(pipe_fds[1], F_SETFL, flags) < 0) {
        pdebug(DEBUG_WARN, "Unable to set flags of write pipe fd!");
        return PLCTAG_ERR_BAD_REPLY;
    }

    /* create the socket mutex. */
    rc = mutex_create(&socket_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to create socket mutex!");
        return rc;
    }

    /* create the timer mutex. */
    rc = mutex_create(&timer_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to create timer mutex!");
        return rc;
    }

    /* create the platform thread. */
    rc = thread_create(&platform_thread, platform_thread_func, 32768, NULL);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to create platform thread!");
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}



void platform_teardown(void)
{
}

/*
 * writing to the pipe causes the select to immediately
 * succeed.
 */
void wake_up_event_loop(void)
{
    uint8_t dummy = 0;
    write(pipe_fds[1], &dummy, 1);
}
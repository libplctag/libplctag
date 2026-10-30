/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
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

#ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <platform.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <libplctag/api/libplctag.h>
#include <utils/atomic_utils.h>
#include <utils/debug.h>


#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__bsdi__) \
    || defined(__DragonFly__)
#    define BSD_OS_TYPE
#    if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#        define _DARWIN_C_SOURCE _POSIX_C_SOURCE
#    endif
#endif


/***************************************************************************
 ******************************* Mutexes ***********************************
 **************************************************************************/

struct mutex_t {
    pthread_mutex_t p_mutex;
    int initialized;
};

int mutex_create(mutex_p *m) {
    pthread_mutexattr_t mutex_attribs;

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Starting.");

    if(*m) { pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Called with non-NULL pointer!"); }

    *m = (struct mutex_t *)mem_alloc(sizeof(struct mutex_t));

    if(!*m) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_ERROR, 0, "null mutex pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* set up for recursive locking. */
    pthread_mutexattr_init(&mutex_attribs);
    pthread_mutexattr_settype(&mutex_attribs, PTHREAD_MUTEX_RECURSIVE);

    if(pthread_mutex_init(&((*m)->p_mutex), &mutex_attribs)) {
        pthread_mutexattr_destroy(&mutex_attribs);
        mem_free(*m);
        *m = NULL;
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_ERROR, 0, "Error initializing mutex.");
        return PLCTAG_ERR_MUTEX_INIT;
    }

    (*m)->initialized = 1;

    pthread_mutexattr_destroy(&mutex_attribs);

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Done creating mutex %p.", *m);

    return PLCTAG_STATUS_OK;
}


int mutex_lock_impl(const char *func, int line, mutex_p m) {
    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "locking mutex %p, called from %s:%d.", m, func, line);

    if(!m) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "null mutex pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!m->initialized) { return PLCTAG_ERR_MUTEX_INIT; }

    if(pthread_mutex_lock(&(m->p_mutex))) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "error locking mutex.");
        return PLCTAG_ERR_MUTEX_LOCK;
    }

    return PLCTAG_STATUS_OK;
}


int mutex_try_lock_impl(const char *func, int line, mutex_p m) {
    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "trying to lock mutex %p, called from %s:%d.", m, func, line);

    if(!m) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "null mutex pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!m->initialized) { return PLCTAG_ERR_MUTEX_INIT; }

    if(pthread_mutex_trylock(&(m->p_mutex))) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "error locking mutex.");
        return PLCTAG_ERR_MUTEX_LOCK;
    }

    /*pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0,"Done.");*/

    return PLCTAG_STATUS_OK;
}


int mutex_unlock_impl(const char *func, int line, mutex_p m) {
    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "unlocking mutex %p, called from %s:%d.", m, func, line);

    if(!m) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "null mutex pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!m->initialized) { return PLCTAG_ERR_MUTEX_INIT; }

    if(pthread_mutex_unlock(&(m->p_mutex))) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "error unlocking mutex.");
        return PLCTAG_ERR_MUTEX_UNLOCK;
    }

    // pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0,"Done.");

    return PLCTAG_STATUS_OK;
}


int mutex_destroy(mutex_p *m) {
    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Starting to destroy mutex %p.", m);

    if(!m || !*m) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "null mutex pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(pthread_mutex_destroy(&((*m)->p_mutex))) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "error while attempting to destroy mutex.");
        return PLCTAG_ERR_MUTEX_DESTROY;
    }

    mem_free(*m);

    *m = NULL;

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/***************************************************************************
 ******************************* Threads ***********************************
 **************************************************************************/

struct thread_t {
    pthread_t p_thread;
    int initialized;
};

/*
 * thread_create()
 *
 * Start up a new thread.  This allocates the thread_t structure and starts
 * the passed function.  The arg argument is passed to the function.
 *
 * TODO - use the stacksize!
 */

extern int thread_create(thread_p *t, thread_func_t func, int stacksize, void *arg) {
    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Starting.");

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Warning: ignoring stacksize (%d) parameter.", stacksize);

    if(!t) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "null thread pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    *t = (thread_p)mem_alloc(sizeof(struct thread_t));

    if(!*t) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_ERROR, 0, "Failed to allocate memory for thread.");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* create a pthread.  0 means success. */
    if(pthread_create(&((*t)->p_thread), NULL, func, arg)) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_ERROR, 0, "error creating thread.");
        return PLCTAG_ERR_THREAD_CREATE;
    }

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * platform_thread_stop()
 *
 * Stop the current thread.  Does not take arguments.  Note: the thread
 * ends completely and this function does not return!
 */
void thread_stop(void) { pthread_exit((void *)0); }


/*
 * thread_kill()
 *
 * Kill another thread.
 */

void thread_kill(thread_p t) {
    if(t) {
#ifdef __ANDROID__
        pthread_kill(t->p_thread, 0);
#else
        pthread_cancel(t->p_thread);
#endif /* __ANDROID__ */
    }
}

/*
 * thread_join()
 *
 * Wait for the argument thread to stop and then continue.
 */

int thread_join(thread_p t) {
    void *unused;
    int64_t join_start_time = 0;
    int join_result = 0;

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Starting.");

    if(!t) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "null thread pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "About to join thread %p.", (void *)t->p_thread);
    join_start_time = time_ms();

    join_result = pthread_join(t->p_thread, &unused);

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "Thread join completed after %" PRId64 "ms. Result: %d",
           (time_ms() - join_start_time), join_result);

    if(join_result) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_ERROR, 0, "Error joining thread.");
        return PLCTAG_ERR_THREAD_JOIN;
    }

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * thread_detach
 *
 * Detach the thread.  You cannot call thread_join on a detached thread!
 */

extern int thread_detach(void) {
    pthread_detach(pthread_self());

    return PLCTAG_STATUS_OK;
}


/*
 * thread_destroy
 *
 * This gets rid of the resources of a thread struct.  The thread in
 * question must be dead first!
 */
extern int thread_destroy(thread_p *t) {
    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Starting.");

    if(!t || !*t) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "null thread pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    mem_free(*t);

    *t = NULL;

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/***************************************************************************
 ******************************* Atomic Ops ********************************
 **************************************************************************/

/*
 * lock_acquire
 *
 * Tries to write a non-zero value into the lock atomically.
 *
 * Returns non-zero on success.
 *
 * Warning: do not pass null pointers!
 */

#define ATOMIC_LOCK_VAL (1)

extern int lock_acquire_try(lock_t *lock) {
    int rc = __sync_lock_test_and_set((int *)lock, ATOMIC_LOCK_VAL);

    if(rc != ATOMIC_LOCK_VAL) {
        return 1;
    } else {
        return 0;
    }
}

/*
 * Spin briefly, then start yielding.
 *
 * A bare spin only pays off when the holder is running on another core and will
 * release within a few cycles. If it is descheduled, or blocked in a syscall, or
 * the process is being serialized onto one core -- an oversubscribed machine, or
 * running under Valgrind, which lets exactly one thread run at a time -- then
 * spinning burns the waiter's whole timeslice and actively delays the holder it is
 * waiting on. Yielding hands the CPU to the holder instead, so the worst case
 * degrades to "slow" rather than to a livelock that scales with the thread count.
 */
int lock_acquire(lock_t *lock) {
    int spins = 0;

    while(!lock_acquire_try(lock)) {
        if(++spins >= 100) { /* MAGIC */
            sched_yield();
            spins = 0;
        }
    }

    return 1;
}


extern void lock_release(lock_t *lock) {
    __sync_lock_release((int *)lock);
    /*pdebug("released lock");*/
}


/***************************************************************************
 ************************* Condition Variables *****************************
 ***************************************************************************/

struct cond_t {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int flag;
};

int cond_create(cond_p *c) {
    int rc = PLCTAG_STATUS_OK;
    cond_p tmp_cond = NULL;

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Starting.");

    if(!c) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Null pointer to condition var pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(*c) { pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Condition var pointer is not null, was it not deleted first?"); }

    /* clear the output first. */
    *c = NULL;

    tmp_cond = mem_alloc((int)(unsigned int)sizeof(*tmp_cond));
    if(!tmp_cond) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Unable to allocate new condition var!");
        return PLCTAG_ERR_NO_MEM;
    }

    if(pthread_mutex_init(&(tmp_cond->mutex), NULL)) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Unable to initialize pthread mutex!");
        mem_free(tmp_cond);
        return PLCTAG_ERR_CREATE;
    }

    if(pthread_cond_init(&(tmp_cond->cond), NULL)) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Unable to initialize pthread condition var!");
        pthread_mutex_destroy(&(tmp_cond->mutex));
        mem_free(tmp_cond);
        return PLCTAG_ERR_CREATE;
    }

    tmp_cond->flag = 0;

    *c = tmp_cond;

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Done.");

    return rc;
}


int cond_wait_impl(const char *func, int line_num, cond_p c, int timeout_ms) {
    int rc = PLCTAG_STATUS_OK;
    int64_t start_time = time_ms();
    int64_t end_time = 0;
    struct timespec timeout;
    struct timespec wall;

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Starting. Called from %s:%d.", func, line_num);

    if(!c) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Condition var pointer is null in call from %s:%d!", func, line_num);
        return PLCTAG_ERR_NULL_PTR;
    }

    /* FIXME - should this be a BAD_PARAM error instead as it was? */
    if(timeout_ms <= 0) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Timeout must be a positive value but was %d in call from %s:%d!",
               timeout_ms, func, line_num);
        return PLCTAG_ERR_TIMEOUT;
    }

    if(pthread_mutex_lock(&(c->mutex))) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Unable to lock mutex!");
        return PLCTAG_ERR_MUTEX_LOCK;
    }

    /*
     * set up timeout.
     *
     * NOTE: the time is _ABSOLUTE_ on CLOCK_REALTIME, the clock pthread_cond_timedwait() uses
     * by default.  time_ms() is monotonic, so it cannot be used here.
     */
    clock_gettime(CLOCK_REALTIME, &wall);
    end_time = ((int64_t)wall.tv_sec * 1000) + ((int64_t)wall.tv_nsec / 1000000) + timeout_ms;

    timeout.tv_sec = (time_t)(end_time / 1000);
    timeout.tv_nsec = (long)1000000 * (long)(end_time % 1000);

    while(!c->flag) {
        int64_t time_left = (int64_t)timeout_ms - (time_ms() - start_time);

        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Waiting for %" PRId64 "ms.", time_left);

        if(time_left > 0) {
            int wait_rc = 0;

            wait_rc = pthread_cond_timedwait(&(c->cond), &(c->mutex), &timeout);
            if(wait_rc == ETIMEDOUT) {
                pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Timeout response from condition var wait.");
                rc = PLCTAG_ERR_TIMEOUT;
                break;
            } else if(wait_rc != 0) {
                /*
                 * pthread_cond_timedwait() returns the error directly and never touches
                 * errno, so wait_rc is the only meaningful value to report here.
                 */
                pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Error %d waiting on condition variable!", wait_rc);
                rc = PLCTAG_ERR_BAD_STATUS;
                break;
            } else {
                /* we might need to wait again. could be a spurious wake up. */
                pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Condition var wait returned.");
                rc = PLCTAG_STATUS_OK;
            }
        } else {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Timed out.");
            rc = PLCTAG_ERR_TIMEOUT;
            break;
        }
    }

    if(c->flag) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Condition var signaled for call at %s:%d.", func, line_num);

        /* clear the flag now that we've responded. */
        c->flag = 0;
    } else {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Condition wait terminated due to error or timeout for call at %s:%d.", func,
               line_num);
    }

    if(pthread_mutex_unlock(&(c->mutex))) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Unable to unlock mutex!");
        return PLCTAG_ERR_MUTEX_UNLOCK;
    }

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Done for call at %s:%d.", func, line_num);

    return rc;
}


int cond_signal_impl(const char *func, int line_num, cond_p c) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Starting.  Called from %s:%d.", func, line_num);

    if(!c) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Condition var pointer is null in call at %s:%d!", func, line_num);
        return PLCTAG_ERR_NULL_PTR;
    }

    if(pthread_mutex_lock(&(c->mutex))) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Unable to lock mutex!");
        return PLCTAG_ERR_MUTEX_LOCK;
    }

    c->flag = 1;

    /*
     * pthread_cond_signal() returns the error directly and never touches errno, so
     * capture the return value rather than reporting an unrelated stale errno.
     */
    int signal_rc = pthread_cond_signal(&(c->cond));

    if(signal_rc) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Signal of condition var returned error %d in call at %s:%d!", signal_rc,
               func, line_num);
        rc = PLCTAG_ERR_BAD_STATUS;
    }

    if(pthread_mutex_unlock(&(c->mutex))) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Unable to unlock mutex!");
        return PLCTAG_ERR_MUTEX_UNLOCK;
    }

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Done. Called from %s:%d.", func, line_num);

    return rc;
}


int cond_clear_impl(const char *func, int line_num, cond_p c) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Starting.  Called from %s:%d.", func, line_num);

    if(!c) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Condition var pointer is null in call at %s:%d!", func, line_num);
        return PLCTAG_ERR_NULL_PTR;
    }

    if(pthread_mutex_lock(&(c->mutex))) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Unable to lock mutex!");
        return PLCTAG_ERR_MUTEX_LOCK;
    }

    c->flag = 0;

    if(pthread_mutex_unlock(&(c->mutex))) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Unable to unlock mutex!");
        return PLCTAG_ERR_MUTEX_UNLOCK;
    }

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Done. Called from %s:%d.", func, line_num);

    return rc;
}


int cond_destroy(cond_p *c) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Starting.");

    if(!c || !*c) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Condition var pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pthread_cond_destroy(&((*c)->cond));
    pthread_mutex_destroy(&((*c)->mutex));

    mem_free(*c);

    *c = NULL;

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Done.");

    return rc;
}


/***************************************************************************
 ******************************* Sockets ***********************************
 **************************************************************************/


#ifndef INVALID_SOCKET
#    define INVALID_SOCKET (-1)
#endif

struct sock_t {
    int fd;
    int wake_read_fd;
    int wake_write_fd;
    int port;
    atomic_bool wake_pending; /* see socket_wake() */
};


static int sock_create_event_wakeup_channel(sock_p sock);
static void socket_set_cloexec(int fd);
static int32_t wait_on_fds(struct pollfd *fds, nfds_t num_fds, int timeout_ms);

#define MAX_IPS (8)


/*
 * Keep socket descriptors out of child processes.
 *
 * A descriptor without FD_CLOEXEC survives fork()/exec(), so a child process
 * holds the connection open after this process closes it.  SOCK_CLOEXEC sets
 * the flag atomically where it exists; macOS has no SOCK_CLOEXEC, so there
 * the flag is set right after creation and a fork() from another thread in
 * between still leaks the descriptor.
 */
static void socket_set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD, 0);

    if(flags < 0) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Unable to get descriptor flags, errno: %d", errno);
        return;
    }

    if(fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Unable to set FD_CLOEXEC, errno: %d", errno);
    }
}

extern int socket_create(sock_p *s) {
    int32_t rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Starting.");

    if(!s) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "null socket pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    *s = (sock_p)mem_alloc(sizeof(struct sock_t));

    if(!*s) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_ERROR, 0, "Failed to allocate memory for socket.");
        return PLCTAG_ERR_NO_MEM;
    }

    (*s)->fd = INVALID_SOCKET;
    (*s)->wake_read_fd = INVALID_SOCKET;
    (*s)->wake_write_fd = INVALID_SOCKET;
    atomic_set_bool(&(*s)->wake_pending, false);

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Setting up wake pipe.");
    rc = sock_create_event_wakeup_channel((*s));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Unable to create wake pipe, error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


int socket_connect_tcp_start(sock_p s, const char *host, int port) {
    int rc = PLCTAG_STATUS_OK;
    struct in_addr ips[MAX_IPS];
    int num_ips = 0;
    struct sockaddr_in gw_addr;
    int sock_opt = 1;
    int i = 0;
    int done = 0;
    int fd;
    int flags;

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Starting.");

    /* Open a socket for communication with the gateway. */
#ifdef SOCK_CLOEXEC
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
#else
    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "socket() created fd=%d", fd);

    /* check for errors */
    if(fd < 0) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_ERROR, 0, "Socket creation failed, errno: %d", errno);
        return PLCTAG_ERR_OPEN;
    }

#ifndef SOCK_CLOEXEC
    socket_set_cloexec(fd);
#endif

    /* set up our socket to allow reuse if we crash suddenly. */
    sock_opt = 1;

    if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&sock_opt, sizeof(sock_opt))) {
        /* report before close(), which sets errno of its own on failure. */
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_ERROR, 0, "Error setting socket reuse option, errno: %d", errno);
        close(fd);
        return PLCTAG_ERR_OPEN;
    }

#ifdef BSD_OS_TYPE
    /* The *BSD family has a different way to suppress SIGPIPE on sockets. */
    if(setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (char *)&sock_opt, sizeof(sock_opt))) {
        /* report before close(), which sets errno of its own on failure. */
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_ERROR, 0, "Error setting socket SIGPIPE suppression option, errno: %d", errno);
        close(fd);
        return PLCTAG_ERR_OPEN;
    }
#endif

    /* NOTE: We do NOT set SO_RCVTIMEO or SO_SNDTIMEO here because:
     * 1. We use non-blocking mode with poll() for timeout handling
     * 2. SO_RCVTIMEO + non-blocking mode can cause poll() to not report readability correctly
     * 3. poll() provides finer control over timeout behavior than SO_RCVTIMEO/SO_SNDTIMEO
     * Instead, timeout handling is done via poll() in socket_wait_event() and socket_read()
     */

    /* make the socket non-blocking. */
    flags = fcntl(fd, F_GETFL, 0);
    if(flags < 0) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_ERROR, 0, "Error getting socket options, errno: %d", errno);
        close(fd);
        return PLCTAG_ERR_OPEN;
    }

    /* set the non-blocking flag. */
    flags |= O_NONBLOCK;

    if(fcntl(fd, F_SETFL, flags) < 0) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_ERROR, 0, "Error setting socket to non-blocking, errno: %d", errno);
        close(fd);
        return PLCTAG_ERR_OPEN;
    }

    /* set no delay for TCP connections.  Send immediately. */
    sock_opt = 1;
    if(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&sock_opt, sizeof(sock_opt))) {
        /* report before close(), which sets errno of its own on failure. */
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_ERROR, 0, "Error setting TCP_NODELAY option, errno: %d", errno);
        close(fd);
        return PLCTAG_ERR_OPEN;
    }

    /* figure out what address we are connecting to. */

    /* try a numeric IP address conversion first. */
    if(inet_pton(AF_INET, host, (struct in_addr *)ips) > 0) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Found numeric IP address: %s", host);
        num_ips = 1;
    } else {
        struct addrinfo hints;
        struct addrinfo *res_head = NULL;
        struct addrinfo *res = NULL;
        int rc = 0;

        mem_set(&ips, 0, sizeof(ips));
        mem_set(&hints, 0, sizeof(hints));

        hints.ai_socktype = SOCK_STREAM; /* TCP */
        hints.ai_family = AF_INET;       /* IP V4 only */

        if((rc = getaddrinfo(host, NULL, &hints, &res_head)) != 0) {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Error looking up PLC IP address %s, error = %d\n", host, rc);

            if(res_head) { freeaddrinfo(res_head); }

            close(fd);
            return PLCTAG_ERR_BAD_GATEWAY;
        }

        res = res_head;
        for(num_ips = 0; res && num_ips < MAX_IPS; num_ips++) {
            ips[num_ips].s_addr = ((struct sockaddr_in *)(res->ai_addr))->sin_addr.s_addr;
            res = res->ai_next;
        }

        freeaddrinfo(res_head);
    }

    // pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Setting up wake pipe.");
    // rc = sock_create_event_wakeup_channel(s);
    // if(rc != PLCTAG_STATUS_OK) {
    //     pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Unable to create wake pipe, error %s!", plc_tag_decode_error(rc));
    //     return rc;
    // }

    /* now try to connect to the remote gateway.  We may need to
     * try several of the IPs we have.
     */

    i = 0;
    done = 0;

    // NOLINTNEXTLINE
    memset((void *)&gw_addr, 0, sizeof(gw_addr));

    gw_addr.sin_family = AF_INET;
    gw_addr.sin_port = htons((uint16_t)port);

    do {
        /* try each IP until we run out or get a connection started. */
        gw_addr.sin_addr.s_addr = ips[i].s_addr;

        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Attempting to connect to %s:%d", inet_ntoa(*((struct in_addr *)&ips[i])),
               port);

        /* this is done non-blocking. Could be interrupted, so restart if needed.*/
        do { rc = connect(fd, (struct sockaddr *)&gw_addr, sizeof(gw_addr)); } while(rc < 0 && errno == EINTR);

        if(rc == 0) {
            /* instantly connected. */
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Connected instantly to %s:%d.",
                   inet_ntoa(*((struct in_addr *)&ips[i])), port);
            done = 1;
            rc = PLCTAG_STATUS_OK;
        } else if(rc < 0 && (errno == EINPROGRESS)) {
            /* the connection has started. */
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Started connecting to %s:%d successfully.",
                   inet_ntoa(*((struct in_addr *)&ips[i])), port);
            done = 1;
            rc = PLCTAG_STATUS_PENDING;
        } else {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Attempt to connect to %s:%d failed, errno: %d",
                   inet_ntoa(*((struct in_addr *)&ips[i])), port, errno);
            i++;
        }
    } while(!done && i < num_ips);

    if(!done) {
        close(fd);
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_ERROR, 0, "Unable to connect to any gateway host IP address!");
        return PLCTAG_ERR_OPEN;
    }

    /* save the values */
    s->fd = fd;
    s->port = port;

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Done with status %s.", plc_tag_decode_error(rc));

    return rc;
}


/*
 * Wait for events on a small set of descriptors.
 *
 * poll() and not select(): a POSIX fd_set is a bitmap indexed by descriptor value, so
 * FD_SET() with a descriptor at or above FD_SETSIZE writes past the object.  Each
 * connection here holds three descriptors -- the socket and both ends of the wake
 * channel -- so a host application with its own open files reaches that limit long
 * before it has many PLCs.  poll() accepts descriptor values of any magnitude.
 *
 * A signal delivered to the calling thread is the host application's business and not
 * an error in ours, so EINTR is retried against the original deadline.
 *
 * Returns the number of ready descriptors, zero on timeout, or a negative status code.
 * poll() reports a bad descriptor as POLLNVAL in revents rather than as an error, so
 * callers must check for it.
 */
static int32_t wait_on_fds(struct pollfd *fds, nfds_t num_fds, int timeout_ms) {
    int64_t deadline = time_ms() + (int64_t)timeout_ms;

    while(1) {
        int32_t poll_rc = (int32_t)poll(fds, num_fds, timeout_ms);

        if(poll_rc >= 0) { return poll_rc; }

        if(errno == EINTR) {
            int64_t remaining_ms = deadline - time_ms();

            if(remaining_ms <= 0) {
                pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "poll() was interrupted by a signal and the timeout expired.");
                return 0;
            }

            timeout_ms = (int)remaining_ms;

            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "poll() was interrupted by a signal, retrying with %d ms left.",
                   timeout_ms);
            continue;
        }

        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "poll() failed with errno %d (%s)!", errno, strerror(errno));

        switch(errno) {
            /*
             * EFAULT means the descriptor array is not in accessible address space and
             * EINVAL means more descriptors than the process limit allows.  Both are
             * programming errors here rather than anything the PLC did.
             */
            case EFAULT:
            case EINVAL: return PLCTAG_ERR_BAD_PARAM;

            /* no memory for the kernel's internal tables. */
            case ENOMEM: return PLCTAG_ERR_NO_MEM;

            default: return PLCTAG_ERR_BAD_STATUS;
        }
    }
}


int socket_connect_tcp_check(sock_p sock, int timeout_ms) {
    int rc = PLCTAG_STATUS_OK;
    struct pollfd pfd;
    int32_t poll_rc = 0;
    int sock_err = 0;
    socklen_t sock_err_len = (socklen_t)(sizeof(sock_err));


    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Starting.");

    if(!sock) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Null socket pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* a connect still in progress completes by making the socket writable. */
    pfd.fd = sock->fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "socket_connect_tcp_check: polling fd=%d with timeout_ms=%d", sock->fd,
           timeout_ms);

    poll_rc = wait_on_fds(&pfd, 1, timeout_ms);

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "socket_connect_tcp_check: poll() returned %" PRId32 ", revents=0x%x", poll_rc,
           pfd.revents);

    if(poll_rc < 0) { return (int)poll_rc; }

    if(poll_rc == 0) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Socket connection not done yet.");
        return PLCTAG_ERR_TIMEOUT;
    }

    if(pfd.revents & POLLNVAL) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Socket fd=%d is not a valid descriptor!", sock->fd);
        return PLCTAG_ERR_OPEN;
    }

    /*
     * A connect that failed sets POLLERR or POLLHUP, with or without POLLOUT.  Both cases
     * are left to the SO_ERROR check below, which has the actual reason -- the caller needs
     * that and not "the socket is not writable".
     */
    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Socket is ready, checking the connection result.");
    /* now make absolutely sure that the connection is ready. */
    /* Use getsockopt to check socket connection status (standard POSIX method) */
    rc = getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, &sock_err, &sock_err_len);
    if(rc == 0) {
        /* sock_err has the error. */
        switch(sock_err) {
            case 0: pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "No error, socket is connected."); break;

            case EBADF:
                pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Socket fd is not valid!");
                return PLCTAG_ERR_OPEN;
                break;

            case EFAULT:
                pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "The address passed to getsockopt() is not a valid user address!");
                return PLCTAG_ERR_OPEN;
                break;

            case EINVAL:
                pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "The size of the socket error result is invalid!");
                return PLCTAG_ERR_OPEN;
                break;

            case ENOPROTOOPT:
                pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "The option SO_ERROR is not understood at the SOL_SOCKET level!");
                return PLCTAG_ERR_OPEN;
                break;

            case ENOTSOCK:
                pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "The FD is not a socket!");
                return PLCTAG_ERR_OPEN;
                break;

            case ECONNREFUSED:
                pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Connection refused!");
                return PLCTAG_ERR_OPEN;
                break;

            default:
                pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Unexpected error %d returned!", sock_err);
                return PLCTAG_ERR_OPEN;
                break;
        }
    } else {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Error %d getting socket connection status!", errno);
        return PLCTAG_ERR_OPEN;
    }

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


static inline void log_event_bits(const char *prefix, int bits) {
    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "%s events=0x%x: %s%s%s%s%s%s", prefix, bits,
           (bits & SOCK_EVENT_CAN_READ) ? "CAN_READ " : "", (bits & SOCK_EVENT_CAN_WRITE) ? "CAN_WRITE " : "",
           (bits & SOCK_EVENT_CONNECT) ? "CONNECT " : "", (bits & SOCK_EVENT_DISCONNECT) ? "DISCONNECT " : "",
           (bits & SOCK_EVENT_ERROR) ? "ERROR " : "", (bits & SOCK_EVENT_TIMEOUT) ? "TIMEOUT " : "");
}

int socket_wait_event(sock_p sock, int events, int timeout_ms) {
    int result = SOCK_EVENT_NONE;
    struct pollfd pfds[2];
    nfds_t num_fds = 0;
    int32_t main_fd_idx = -1;
    int32_t poll_rc = 0;

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Starting.");

    if(!sock) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Null socket pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(timeout_ms < 0) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Timeout must be zero or positive!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* check if the mask is empty */
    if(events == 0) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Passed event mask is empty!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * Index 0 is always the wake channel.  Index 1 is the main socket, and only when it
     * is open -- it is closed and reopened across reconnects, and waiting on the wake
     * channel alone is a valid state.
     *
     * POLLERR, POLLHUP and POLLNVAL are reported whether or not they were asked for, so
     * there is no equivalent of the old error fd_set to set up.
     */
    if(sock->wake_read_fd == INVALID_SOCKET) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Wake socket is invalid, cannot wait for events!");
        return PLCTAG_ERR_BAD_CONFIG;
    }

    pfds[0].fd = sock->wake_read_fd;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    num_fds = 1;

    if(sock->fd != INVALID_SOCKET) {
        pfds[1].fd = sock->fd;
        pfds[1].events = 0;
        pfds[1].revents = 0;

        /*
         * POLLIN is requested for SOCK_EVENT_DISCONNECT as well as for SOCK_EVENT_CAN_READ.
         * A peer that half-closes makes the socket readable at end of file without raising
         * POLLHUP, so the zero-length read below is the only notice of it, and that read
         * never happens unless POLLIN was asked for.  Reporting CAN_READ stays gated on the
         * caller wanting it.
         */
        if(events & (SOCK_EVENT_CAN_READ | SOCK_EVENT_DISCONNECT)) {
            pfds[1].events |= POLLIN;
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Polling sock->fd=%d for POLLIN.", sock->fd);
        }

        if((events & SOCK_EVENT_CONNECT) || (events & SOCK_EVENT_CAN_WRITE)) {
            pfds[1].events |= POLLOUT;
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0,
                   "Polling sock->fd=%d for POLLOUT for SOCK_EVENT_CONNECT or SOCK_EVENT_CAN_WRITE", sock->fd);
        }

        main_fd_idx = 1;
        num_fds = 2;

        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Main socket fd=%d is valid, wake_read_fd=%d", sock->fd, sock->wake_read_fd);
    } else {
        /* Main socket invalid or closed - only wake socket will be monitored (valid for reconnection/idle) */
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Main socket is INVALID, using only wake_read_fd=%d", sock->wake_read_fd);
    }

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "events=0x%x, sock->fd=%d, timeout_ms=%d, calling poll()", events, sock->fd,
           timeout_ms);

    poll_rc = wait_on_fds(&pfds[0], num_fds, timeout_ms);

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "poll() returned %" PRId32 " for sock->fd=%d", poll_rc, sock->fd);

    if(poll_rc < 0) { return (int)poll_rc; }

    if(poll_rc == 0) {
        result |= (events & SOCK_EVENT_TIMEOUT);
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "poll() timed out, returning TIMEOUT event");
    } else {
        int16_t main_revents = (main_fd_idx >= 0 ? pfds[main_fd_idx].revents : (int16_t)0);

        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "poll() reported %" PRId32 " ready, wake revents=0x%x, main revents=0x%x",
               poll_rc, pfds[0].revents, main_revents);

        /* was there a wake up? */
        if(pfds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
            char buf[32];
            int32_t bytes_read = 0;

            /* empty the channel. */
            do { bytes_read = (int32_t)read(sock->wake_read_fd, &buf[0], sizeof(buf)); } while(bytes_read > 0);

            /* cleared after the drain, so a byte written from here on stays for the next poll. */
            atomic_set_bool(&sock->wake_pending, false);

            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Socket woken up.");
            result |= (events & SOCK_EVENT_WAKE_UP);

            /*
             * Zero is end of file on the wake channel and not an empty one.  Once the write
             * end is gone the read end stays readable forever, so swallowing the zero spins
             * this thread at full speed on every call.  Report it and let the caller tear
             * the socket down.
             */
            if(bytes_read == 0) {
                pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Wake channel closed (read returned 0)!");
                result |= (events & SOCK_EVENT_DISCONNECT);
            } else if(errno != EAGAIN && errno != EWOULDBLOCK) {
                pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Wake channel read error %d!", errno);
                result |= (events & SOCK_EVENT_DISCONNECT);
            }
        }

        /* is read ready for the main fd? */
        if(main_revents & POLLIN) {
            char buf;
            int byte_read = 0;

            byte_read = (int)recv(sock->fd, &buf, sizeof(buf), MSG_PEEK);

            if(byte_read > 0) {
                pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Socket can read.");
                result |= (events & SOCK_EVENT_CAN_READ);
            } else if(byte_read == 0) {
                pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Socket disconnected (recv returned 0).");
                result |= (events & SOCK_EVENT_DISCONNECT);
            } else {
                if(errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* Don't report anything, will wait for next event */
                } else {
                    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Socket recv error: %d", errno);
                    result |= (events & SOCK_EVENT_DISCONNECT);
                }
            }
        }

        /* is write ready for the main fd? */
        if(main_revents & POLLOUT) {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Socket can write or just connected.");
            result |= ((events & SOCK_EVENT_CAN_WRITE) | (events & SOCK_EVENT_CONNECT));
        }

        /*
         * The peer closing is not an error.  poll() reports it as POLLHUP whether or not
         * POLLIN was requested, so a caller waiting only for writability still learns of it.
         */
        if(main_revents & POLLHUP) {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Peer hung up on fd=%d.", sock->fd);
            result |= (events & SOCK_EVENT_DISCONNECT);
        }

        if(main_revents & POLLNVAL) {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Socket fd=%d is not a valid descriptor!", sock->fd);
            result |= (events & SOCK_EVENT_ERROR);
        }

        if(main_revents & POLLERR) {
            /* POLLERR can be set with no pending error, so SO_ERROR decides. */
            int sock_error = 0;
            socklen_t sock_error_len = sizeof(sock_error);

            if(getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, &sock_error, &sock_error_len) == 0) {
                if(sock_error != 0) {
                    /* There's a real socket error */
                    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Socket has real error %d!", sock_error);
                    result |= (events & SOCK_EVENT_ERROR);
                } else {
                    /* POLLERR was spurious - there's no actual error */
                    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "POLLERR was set but SO_ERROR is 0 (spurious error flag).");
                }
            } else {
                /* Failed to get socket error state, assume there's an error */
                pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Failed to check socket error state, treating as error.");
                result |= (events & SOCK_EVENT_ERROR);
            }
        }

        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "After all checks, result=0x%x", result);
    }
    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Done.");

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0,
           "socket_wait_event: Final result=0x%x before return, TIMEOUT bit=%d, events=0x%x", result,
           (result & SOCK_EVENT_TIMEOUT) ? 1 : 0, events);
    log_event_bits("socket_wait_event: result", result);
    log_event_bits("socket_wait_event: requested_events", events);

    /* Log result at INFO level for visibility */
    if(result != SOCK_EVENT_NONE) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "fd=%d returning events=0x%x (requested=0x%x) %s%s%s%s%s%s", sock->fd,
               result, events, (result & SOCK_EVENT_CAN_READ) ? "CAN_READ " : "",
               (result & SOCK_EVENT_CAN_WRITE) ? "CAN_WRITE " : "", (result & SOCK_EVENT_CONNECT) ? "CONNECT " : "",
               (result & SOCK_EVENT_DISCONNECT) ? "DISCONNECT " : "", (result & SOCK_EVENT_ERROR) ? "ERROR " : "",
               (result & SOCK_EVENT_TIMEOUT) ? "TIMEOUT " : "");
    }

    return result;
}


/*
 * A wake writes to the wake channel only when none is already pending, so a busy handler is
 * not sent one byte per caller.  The flag is cleared just after the channel is drained, and the
 * handler scans its work after every wait, so a wake that finds the flag set and skips its byte
 * still has its work seen.  Cleared before the drain, the drain could swallow a byte written
 * after the clear and leave the flag set with nothing in the channel.
 */
int socket_wake(sock_p sock) {
    int rc = PLCTAG_STATUS_OK;
    const char dummy_data[] = "Dummy data.";

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Starting.");

    if(!sock) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Null socket pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* The wake pipe is independent of the TCP connection state.
     * Write to the wake pipe to interrupt the handler thread's poll() call.
     * This works regardless of whether the TCP connection is open.
     * Check that the wake pipe is valid before writing to it. */
    if(sock->wake_write_fd == INVALID_SOCKET) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Wake pipe not yet initialized, skipping wake.");
        return PLCTAG_STATUS_OK;
    }

    /* the handler has a wake it has not read yet. */
    if(atomic_compare_and_set_bool(&sock->wake_pending, false, true)) { return PLCTAG_STATUS_OK; }

    // rc = (int)write(sock->wake_write_fd, &dummy_data[0], sizeof(dummy_data));
#ifdef BSD_OS_TYPE
    /* On *BSD and macOS, the socket option is set to prevent SIGPIPE. */
    rc = (int)write(sock->wake_write_fd, &dummy_data[0], sizeof(dummy_data));
#else
    /* on Linux, we use MSG_NOSIGNAL */
    rc = (int)send(sock->wake_write_fd, &dummy_data[0], sizeof(dummy_data), MSG_NOSIGNAL);
#endif
    if(rc >= 0) {
        rc = PLCTAG_STATUS_OK;
    } else {
        int err = errno;

        /* If the write failed with EAGAIN/EWOULDBLOCK, the wake pipe is full.
         * This means a wake is already pending, so return success. */
        if(err == EAGAIN || err == EWOULDBLOCK) {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Wake pipe full (EAGAIN/EWOULDBLOCK), wake already pending.");
            return PLCTAG_STATUS_OK;
        }

        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Socket write error: rc=%d, errno=%d", rc, err);

        /* If the write failed with EBADF (bad file descriptor), the wake pipe
         * has been closed. Mark it as invalid and return success so the system
         * can proceed. The next wake attempt will skip due to INVALID_SOCKET check. */
        if(err == EBADF) {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Wake pipe closed (EBADF), marking as invalid.");
            sock->wake_write_fd = INVALID_SOCKET;
            return PLCTAG_STATUS_OK;
        }

        /* no byte went out, so the next wake must try again. */
        atomic_set_bool(&sock->wake_pending, false);

        return PLCTAG_ERR_WRITE;
    }

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_DETAIL, 0, "Done.");

    return rc;
}


int socket_read(sock_p s, uint8_t *buf, int size, int timeout_ms) {
    int rc;

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Starting.");

    if(!s) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Socket pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!buf) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Buffer pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(s->fd == INVALID_SOCKET) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Socket is not open!");
        return PLCTAG_ERR_READ;
    }

    if(timeout_ms < 0) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Timeout must be zero or positive!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * Try to read immediately.   If we get data, we skip any other
     * delays.   If we do not, then see if we have a timeout.
     */

    /* The socket is non-blocking. */
    rc = (int)read(s->fd, buf, (size_t)size);
    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "read() returned %d for fd=%d (requested %d bytes)", rc, s->fd, size);

    if(rc < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            if(timeout_ms > 0) {
                pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "Would block, will wait for poll() on fd=%d", s->fd);
            } else {
                pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Read resulted in no data.");
            }

            rc = 0;
        } else {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Socket read error: rc=%d, errno=%d (%s)", rc, errno, strerror(errno));
            return PLCTAG_ERR_READ;
        }
    } else if(rc == 0) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Connection closed by peer (read returned 0) on fd=%d", s->fd);
        /* EOF is not 'no data yet': an EOF'd socket always reports readable, so the caller's loop would spin. */
        return PLCTAG_ERR_READ;
    } else if(rc > 0 && rc < size) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "Partial read on fd=%d: read %d of %d bytes", s->fd, rc, size);
    } else if(rc == size) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "Complete read on fd=%d: read all %d bytes", s->fd, size);
    }

    /* only wait if we have a timeout and no error and no data. */
    if(rc == 0 && timeout_ms > 0) {
        struct pollfd pfd;
        int32_t poll_rc = 0;

        pfd.fd = s->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        poll_rc = wait_on_fds(&pfd, 1, timeout_ms);
        if(poll_rc < 0) { return (int)poll_rc; }

        if(poll_rc == 0) {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Socket read timed out.");
            return PLCTAG_ERR_TIMEOUT;
        }

        if(pfd.revents & POLLNVAL) {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Socket fd=%d is not a valid descriptor!", s->fd);
            return PLCTAG_ERR_BAD_PARAM;
        }

        /*
         * POLLERR and POLLHUP without POLLIN fall through to the read below.  The read
         * reports the actual errno or the EOF, which is what the caller needs.
         */
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Socket is ready to read, revents=0x%x.", pfd.revents);
        /* try to read again. */
        rc = (int)read(s->fd, buf, (size_t)size);
        if(rc < 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "No data read.");
                rc = 0;
            } else {
                pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Socket read error: rc=%d, errno=%d", rc, errno);
                return PLCTAG_ERR_READ;
            }
        } else if(rc == 0) {
            /* EOF is not 'no data yet': an EOF'd socket always reports readable, so the caller's loop would spin. */
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Connection closed by peer after poll (read returned 0) on fd=%d",
                   s->fd);
            return PLCTAG_ERR_READ;
        }
    }

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Done: result %d.", rc);

    return rc;
}


int socket_write(sock_p s, uint8_t *buf, int size, int timeout_ms) {
    int rc;

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Starting.");

    if(!s) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Socket pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!buf) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Buffer pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(s->fd == INVALID_SOCKET) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Socket is not open!");
        return PLCTAG_ERR_WRITE;
    }

    if(timeout_ms < 0) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Timeout must be zero or positive!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * Try to write without waiting.
     *
     * In the case that we can immediately write, then we skip a
     * system call to poll().   If we cannot, then we will
     * call poll().
     */

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "About to write %d bytes on fd=%d", size, s->fd);

#ifdef BSD_OS_TYPE
    /* On *BSD and macOS, the socket option is set to prevent SIGPIPE. */
    rc = (int)write(s->fd, buf, (size_t)size);
    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "write() returned %d for fd=%d (requested %d bytes)", rc, s->fd, size);
#else
    /* on Linux, we use MSG_NOSIGNAL */
    rc = (int)send(s->fd, buf, (size_t)size, MSG_NOSIGNAL);
    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "send() returned %d for fd=%d (requested %d bytes)", rc, s->fd, size);
#endif

    if(rc < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "Would block, no immediate data written on fd=%d", s->fd);
            rc = 0;
        } else {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Socket write error: rc=%d, errno=%d (%s)", rc, errno, strerror(errno));
            return PLCTAG_ERR_WRITE;
        }
    } else if(rc > 0 && rc < size) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "Partial write on fd=%d: wrote %d of %d bytes", s->fd, rc, size);
    } else if(rc == size) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "Complete write on fd=%d: wrote all %d bytes", s->fd, size);
    }

    /* only wait if we have a timeout and no error and wrote no data. */
    if(rc == 0 && timeout_ms > 0) {
        struct pollfd pfd;
        int32_t poll_rc = 0;

        pfd.fd = s->fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;

        poll_rc = wait_on_fds(&pfd, 1, timeout_ms);
        if(poll_rc < 0) { return (int)poll_rc; }

        if(poll_rc == 0) {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Socket write timed out.");
            return PLCTAG_ERR_TIMEOUT;
        }

        if(pfd.revents & POLLNVAL) {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Socket fd=%d is not a valid descriptor!", s->fd);
            return PLCTAG_ERR_BAD_PARAM;
        }

        /*
         * POLLERR and POLLHUP without POLLOUT fall through to the write below.  The write
         * reports the actual errno, which is what the caller needs, and "not writable" is not.
         */
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "poll() indicated fd=%d is ready for write, attempting write", s->fd);
#ifdef BSD_OS_TYPE
        /* On *BSD and macOS, the socket option is set to prevent SIGPIPE. */
        rc = (int)write(s->fd, buf, (size_t)size);
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "After poll(), write() returned %d for fd=%d (requested %d bytes)", rc,
               s->fd, size);
#else
        /* on Linux, we use MSG_NOSIGNAL */
        rc = (int)send(s->fd, buf, (size_t)size, MSG_NOSIGNAL);
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "After poll(), send() returned %d for fd=%d (requested %d bytes)", rc, s->fd,
               size);
#endif

        if(rc < 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "After poll(), still would block on fd=%d", s->fd);
                rc = 0;
            } else {
                pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Socket write error after poll: rc=%d, errno=%d (%s)", rc, errno,
                       strerror(errno));
                return PLCTAG_ERR_WRITE;
            }
        } else if(rc > 0 && rc < size) {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "After poll(), partial write on fd=%d: wrote %d of %d bytes", s->fd, rc,
                   size);
        } else if(rc == size) {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "After poll(), complete write on fd=%d: wrote all %d bytes", s->fd,
                   size);
        }
    }

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_SPEW, 0, "Done: result = %d.", rc);

    return rc;
}


int socket_close(sock_p s) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "Starting.");

    if(!s) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Socket pointer or pointer to socket pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(s->fd != INVALID_SOCKET) {
        if(close(s->fd)) {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Error closing socket fd=%d!", s->fd);
            rc = PLCTAG_ERR_CLOSE;
        } else {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "Closed socket fd=%d", s->fd);
        }

        s->fd = INVALID_SOCKET;
    }

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "Done.");

    return rc;
}


int socket_destroy(sock_p *s) {
    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "Starting.");

    if(!s || !*s) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Socket pointer or pointer to socket pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* close the wake sockets */
    if((*s)->wake_read_fd != INVALID_SOCKET) {
        if(close((*s)->wake_read_fd)) { pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Error closing read wake socket!"); }

        (*s)->wake_read_fd = INVALID_SOCKET;
    }

    if((*s)->wake_write_fd != INVALID_SOCKET) {
        if(close((*s)->wake_write_fd)) { pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Error closing write wake socket!"); }

        (*s)->wake_write_fd = INVALID_SOCKET;
    }

    socket_close(*s);

    mem_free(*s);

    *s = NULL;

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


int sock_create_event_wakeup_channel(sock_p sock) {
    int rc = PLCTAG_STATUS_OK;
    int flags = 0;
    int wake_fds[2] = {0};
#ifdef BSD_OS_TYPE
    int sock_opt = 1;
#endif

    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "Starting.");

    do {
        /* open the pipe for waking the poll wait. */
        // if(pipe(wake_fds)) {
#ifdef SOCK_CLOEXEC
        if((rc = socketpair(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0, wake_fds))) {
#else
        if((rc = socketpair(PF_LOCAL, SOCK_STREAM, 0, wake_fds))) {
#endif
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Unable to open waker pipe!");
            switch(errno) {
                case EAFNOSUPPORT:
                    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0,
                           "The specified addresss family is not supported on this machine!");
                    break;

                case EFAULT:
                    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0,
                           "The address socket_vector does not specify a valid part of the process address space.");
                    break;

                case EMFILE:
                    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "No more file descriptors are available for this process.");
                    break;

                case ENFILE:
                    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "No more file descriptors are available for the system.");
                    break;

                case ENOBUFS:
                    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0,
                           "Insufficient resources were available in the system to perform the operation.");
                    break;

                case ENOMEM:
                    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Insufficient memory was available to fulfill the request.");
                    break;

                case EOPNOTSUPP:
                    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0,
                           "The specified protocol does not support creation of socket pairs.");
                    break;

                case EPROTONOSUPPORT:
                    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "The specified protocol is not supported on this machine.");
                    break;

                case EPROTOTYPE:
                    pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "The socket type is not supported by the protocol.");
                    break;

                default: pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Unexpected error %d!", errno); break;
            }

            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

#ifndef SOCK_CLOEXEC
        socket_set_cloexec(wake_fds[0]);
        socket_set_cloexec(wake_fds[1]);
#endif

#ifdef BSD_OS_TYPE
        /* The *BSD family has a different way to suppress SIGPIPE on sockets. */
        if(setsockopt(wake_fds[0], SOL_SOCKET, SO_NOSIGPIPE, (char *)&sock_opt, sizeof(sock_opt))) {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_ERROR, 0,
                   "Error setting wake fd read socket SIGPIPE suppression option, errno: %d", errno);
            rc = PLCTAG_ERR_OPEN;
            break;
        }

        if(setsockopt(wake_fds[1], SOL_SOCKET, SO_NOSIGPIPE, (char *)&sock_opt, sizeof(sock_opt))) {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_ERROR, 0,
                   "Error setting wake fd write socket SIGPIPE suppression option, errno: %d", errno);
            rc = PLCTAG_ERR_OPEN;
            break;
        }
#endif

        /* make the read pipe fd non-blocking. */
        if((flags = fcntl(wake_fds[0], F_GETFL)) < 0) {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Unable to get flags of read socket fd!");
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        /* set read fd non-blocking */
        flags |= O_NONBLOCK;

        if(fcntl(wake_fds[0], F_SETFL, flags) < 0) {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Unable to set flags of read socket fd!");
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        /* make the write pipe fd non-blocking. */
        if((flags = fcntl(wake_fds[1], F_GETFL)) < 0) {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Unable to get flags of write socket fd!");
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        /* set write fd non-blocking */
        flags |= O_NONBLOCK;

        if(fcntl(wake_fds[1], F_SETFL, flags) < 0) {
            pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Unable to set flags of write socket fd!");
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        sock->wake_read_fd = wake_fds[0];
        sock->wake_write_fd = wake_fds[1];
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "Unable to open waker socket!");

        if(wake_fds[0] != INVALID_SOCKET) {
            close(wake_fds[0]);
            wake_fds[0] = INVALID_SOCKET;
        }

        if(wake_fds[1] != INVALID_SOCKET) {
            close(wake_fds[1]);
            wake_fds[1] = INVALID_SOCKET;
        }
    } else {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_INFO, 0, "Done.");
    }

    return rc;
}


/***************************************************************************
 ***************************** Miscellaneous *******************************
 **************************************************************************/


/*
 * sleep_ms
 *
 * Sleep the passed number of milliseconds.  This handles the case of being
 * interrupted by a signal.
 *
 * TODO - should the signal interrupt handling be done here or in app code?
 */
int sleep_ms(int ms) {
    struct timespec wait_time;
    struct timespec remainder;
    int done = 1;
    int rc = PLCTAG_STATUS_OK;

    if(ms < 0) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_WARN, 0, "called with negative time %d!", ms);
        return PLCTAG_ERR_BAD_PARAM;
    }

    wait_time.tv_sec = ms / 1000;
    wait_time.tv_nsec = ((long)ms % 1000) * 1000000; /* convert to nanoseconds */

    do {
        rc = nanosleep(&wait_time, &remainder);
        if(rc < 0 && errno == EINTR) {
            /* we were interrupted, keep going. */
            wait_time = remainder;
            done = 0;
        } else {
            done = 1;

            if(rc < 0) {
                /* error condition. */
                rc = PLCTAG_ERR_BAD_REPLY;
            }
        }
    } while(!done);

    return rc;


    // rc = nanosleep(&wait_time, &remainder);
    // if(rc < 0 && errno != EINTR) {
    //     rc = PLCTAG_ERR_BAD_REPLY;
    // }

    // return rc;
    // struct timeval tv;

    // tv.tv_sec = ms/1000;
    // tv.tv_usec = (ms % 1000)*1000;

    // return select(0,NULL,NULL,NULL, &tv);
}


/*
 * time_ms
 *
 * Milliseconds on a monotonic clock, for deadlines and elapsed time only: a change to the
 * system clock must not move them.  Not a date.
 */
int64_t time_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return ((int64_t)ts.tv_sec * 1000) + ((int64_t)ts.tv_nsec / 1000000);
}

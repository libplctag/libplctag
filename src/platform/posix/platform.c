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
#include <util/debug.h>



#if defined(__APPLE__) || defined(__FreeBSD__) ||  defined(__NetBSD__) || defined(__OpenBSD__) || defined(__bsdi__) || defined(__DragonFly__)
    #define BSD_OS_TYPE
    #if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
        #define _DARWIN_C_SOURCE _POSIX_C_SOURCE
    #endif
#endif


/***************************************************************************
 ******************************* Memory ************************************
 **************************************************************************/



/*
 * mem_alloc
 *
 * This is a wrapper around the platform's memory allocation routine.
 * It will zero out memory before returning it.
 *
 * It will return NULL on failure.
 */
extern void *mem_alloc(int size)
{
    if(size <= 0) {
        pdebug(DEBUG_WARN, "Allocation size must be greater than zero bytes!");
        return NULL;
    }

    return calloc((size_t)(unsigned int)size, 1);
}



/*
 * mem_realloc
 *
 * This is a wrapper around the platform's memory re-allocation routine.
 *
 * It will return NULL on failure.
 */
extern void *mem_realloc(void *orig, int size)
{
    if(size <= 0) {
        pdebug(DEBUG_WARN, "New allocation size must be greater than zero bytes!");
        return NULL;
    }

    return realloc(orig, (size_t)(ssize_t)size);
}



/*
 * mem_free
 *
 * Free the allocated memory passed in.  If the passed pointer is
 * null, do nothing.
 */
extern void mem_free(const void *mem)
{
    if(mem) {
        free((void *)mem);
    }
}




/*
 * mem_set
 *
 * set memory to the passed argument.
 */
extern void mem_set(void *dest, int c, int size)
{
    if(!dest) {
        pdebug(DEBUG_WARN, "Destination pointer is NULL!");
        return;
    }

    if(size <= 0) {
        pdebug(DEBUG_WARN, "Size to set must be a positive number!");
        return;
    }

    memset(dest, c, (size_t)(ssize_t)size);
}





/*
 * mem_copy
 *
 * copy memory from one pointer to another for the passed number of bytes.
 */
extern void mem_copy(void *dest, void *src, int size)
{
    if(!dest) {
        pdebug(DEBUG_WARN, "Destination pointer is NULL!");
        return;
    }

    if(!src) {
        pdebug(DEBUG_WARN, "Source pointer is NULL!");
        return;
    }

    if(size < 0) {
        pdebug(DEBUG_WARN, "Size to copy must be a positive number!");
        return;
    }

    if(size == 0) {
        /* nothing to do. */
        return;
    }

    memcpy(dest, src, (size_t)(unsigned int)size);
}



/*
 * mem_move
 *
 * move memory from one pointer to another for the passed number of bytes.
 */
extern void mem_move(void *dest, void *src, int size)
{
    if(!dest) {
        pdebug(DEBUG_WARN, "Destination pointer is NULL!");
        return;
    }

    if(!src) {
        pdebug(DEBUG_WARN, "Source pointer is NULL!");
        return;
    }

    if(size < 0) {
        pdebug(DEBUG_WARN, "Size to move must be a positive number!");
        return;
    }

    if(size == 0) {
        /* nothing to do. */
        return;
    }

    memmove(dest, src, (size_t)(unsigned int)size);
}





int mem_cmp(void *src1, int src1_size, void *src2, int src2_size)
{
    if(!src1 || src1_size <= 0) {
        if(!src2 || src2_size <= 0) {
            /* both are NULL or zero length, but that is "equal" for our purposes. */
            return 0;
        } else {
            /* first one is "less" than second. */
            return -1;
        }
    } else {
        if(!src2 || src2_size <= 0) {
            /* first is "greater" than second */
            return 1;
        } else {
            /* both pointers are non-NULL and the lengths are positive. */

            /* short circuit the comparison if the blocks are different lengths */
            if(src1_size != src2_size) {
                return (src1_size - src2_size);
            }

            return memcmp(src1, src2, (size_t)(unsigned int)src1_size);
        }
    }
}






/***************************************************************************
 ******************************* Strings ***********************************
 **************************************************************************/


/*
 * str_cmp
 *
 * Return -1, 0, or 1 depending on whether the first string is "less" than the
 * second, the same as the second, or "greater" than the second.  This routine
 * just passes through to POSIX strcmp.
 *
 * Handle edge cases when NULL or zero length strings are passed.
 */
extern int str_cmp(const char *first, const char *second)
{
    int first_zero = !str_length(first);
    int second_zero = !str_length(second);

    if(first_zero) {
        if(second_zero) {
            pdebug(DEBUG_DETAIL, "NULL or zero length strings passed.");
            return 0;
        } else {
            /* first is "less" than second. */
            return -1;
        }
    } else {
        if(second_zero) {
            /* first is "more" than second. */
            return 1;
        } else {
            /* both are non-zero length. */
            return strcmp(first, second);
        }
    }
}




/*
 * str_cmp_i
 *
 * Returns -1, 0, or 1 depending on whether the first string is "less" than the
 * second, the same as the second, or "greater" than the second.  The comparison
 * is done case insensitive.
 *
 * Handle the usual edge cases.
 */
extern int str_cmp_i(const char *first, const char *second)
{
    int first_zero = !str_length(first);
    int second_zero = !str_length(second);

    if(first_zero) {
        if(second_zero) {
            pdebug(DEBUG_DETAIL, "NULL or zero length strings passed.");
            return 0;
        } else {
            /* first is "less" than second. */
            return -1;
        }
    } else {
        if(second_zero) {
            /* first is "more" than second. */
            return 1;
        } else {
            /* both are non-zero length. */
            return strcasecmp(first,second);
        }
    }
}



/*
 * str_cmp_i_n
 *
 * Returns -1, 0, or 1 depending on whether the first string is "less" than the
 * second, the same as the second, or "greater" than the second.  The comparison
 * is done case insensitive.  Compares only the first count characters.
 *
 * It just passes this through to POSIX strncasecmp.
 */
extern int str_cmp_i_n(const char *first, const char *second, int count)
{
    int first_zero = !str_length(first);
    int second_zero = !str_length(second);

    if(count < 0) {
        pdebug(DEBUG_WARN, "Illegal negative count!");
        return -1;
    }

    if(count == 0) {
        pdebug(DEBUG_DETAIL, "Called with comparison count of zero!");
        return 0;
    }

    if(first_zero) {
        if(second_zero) {
            pdebug(DEBUG_DETAIL, "NULL or zero length strings passed.");
            return 0;
        } else {
            /* first is "less" than second. */
            return -1;
        }
    } else {
        if(second_zero) {
            /* first is "more" than second. */
            return 1;
        } else {
            /* both are non-zero length. */
            return strncasecmp(first, second, (size_t)(unsigned int)count);
        }
    }
    return strncasecmp(first, second, (size_t)(unsigned int)count);
}



/*
 * str_copy
 *
 * Returns
 */
extern int str_copy(char *dst, int dst_size, const char *src)
{
    if (!dst) {
        pdebug(DEBUG_WARN, "Destination string pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if (!src) {
        pdebug(DEBUG_WARN, "Source string pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(dst_size <= 0) {
        pdebug(DEBUG_WARN, "Destination size is negative or zero!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    strncpy(dst, src, (size_t)(unsigned int)dst_size);
    return PLCTAG_STATUS_OK;
}


/*
 * str_length
 *
 * Return the length of the string.  If a null pointer is passed, return
 * null.
 */
extern int str_length(const char *str)
{
    if(!str) {
        return 0;
    }

    return (int)strlen(str);
}




/*
 * str_dup
 *
 * Copy the passed string and return a pointer to the copy.
 * The caller is responsible for freeing the memory.
 */
extern char *str_dup(const char *str)
{
    if(!str) {
        return NULL;
    }

    return strdup(str);
}



/*
 * str_to_int
 *
 * Convert the characters in the passed string into
 * an int.  Return an int in integer in the passed
 * pointer and a status from the function.
 */
extern int str_to_int(const char *str, int *val)
{
    char *endptr;
    long int tmp_val;

    tmp_val = strtol(str,&endptr,0);

    if (errno == ERANGE && (tmp_val == LONG_MAX || tmp_val == LONG_MIN)) {
        pdebug(DEBUG_WARN,"strtol returned %ld with errno %d",tmp_val, errno);
        return -1;
    }

    if (endptr == str) {
        return -1;
    }

    /* FIXME - this will truncate long values. */
    *val = (int)tmp_val;

    return 0;
}


extern int str_to_float(const char *str, float *val)
{
    char *endptr;
    float tmp_val;

    tmp_val = strtof(str,&endptr);

    if (errno == ERANGE && (tmp_val == HUGE_VALF || tmp_val == -HUGE_VALF || tmp_val == 0)) {
        return -1;
    }

    if (endptr == str) {
        return -1;
    }

    /* FIXME - this will truncate long values. */
    *val = tmp_val;

    return 0;
}


extern char **str_split(const char *str, const char *sep)
{
    int sub_str_count=0;
    int size = 0;
    const char *sub;
    const char *tmp;
    char **res;

    /* first, count the sub strings */
    tmp = str;
    sub = strstr(tmp,sep);

    while(sub && *sub) {
        /* separator could be at the front, ignore that. */
        if(sub != tmp) {
            sub_str_count++;
        }

        tmp = sub + str_length(sep);
        sub = strstr(tmp,sep);
    }

    if(tmp && *tmp && (!sub || !*sub))
        sub_str_count++;

    /* calculate total size for string plus pointers */
    size = ((int)sizeof(char *)*(sub_str_count+1)+str_length(str)+1);

    /* allocate enough memory */
    res = mem_alloc(size);

    if(!res)
        return NULL;

    /* calculate the beginning of the string */
    tmp = (char *)res + sizeof(char *) * (size_t)(sub_str_count+1);

    /* copy the string into the new buffer past the first part with the array of char pointers. */
    str_copy((char *)tmp, (int)(size - ((char*)tmp - (char*)res)), str);

    /* set up the pointers */
    sub_str_count=0;
    sub = strstr(tmp,sep);

    while(sub && *sub) {
        /* separator could be at the front, ignore that. */
        if(sub != tmp) {
            /* store the pointer */
            res[sub_str_count] = (char *)tmp;

            sub_str_count++;
        }

        /* zero out the separator chars */
        mem_set((char*)sub,0,str_length(sep));

        /* point past the separator (now zero) */
        tmp = sub + str_length(sep);

        /* find the next separator */
        sub = strstr(tmp,sep);
    }

    /* if there is a chunk at the end, store it. */
    if(tmp && *tmp && (!sub || !*sub)) {
        res[sub_str_count] = (char*)tmp;
    }

    return res;
}



char *str_concat_impl(int num_args, ...)
{
    va_list arg_list;
    int total_length = 0;
    char *result = NULL;
    char *tmp = NULL;

    /* first loop to find the length */
    va_start(arg_list, num_args);
    for(int i=0; i < num_args; i++) {
        tmp = va_arg(arg_list, char *);
        if(tmp) {
            total_length += str_length(tmp);
        }
    }
    va_end(arg_list);

    /* make a buffer big enough */
    total_length += 1;

    result = mem_alloc(total_length);
    if(!result) {
        pdebug(DEBUG_ERROR,"Unable to allocate new string buffer!");
        return NULL;
    }

    /* loop to copy the strings */
    result[0] = 0;
    va_start(arg_list, num_args);
    for(int i=0; i < num_args; i++) {
        tmp = va_arg(arg_list, char *);
        if(tmp) {
            int len = str_length(result);
            str_copy(&result[len], total_length - len, tmp);
        }
    }
    va_end(arg_list);

    return result;
}




/***************************************************************************
 ******************************* Mutexes ***********************************
 **************************************************************************/

struct mutex_t {
    pthread_mutex_t p_mutex;
    int initialized;
};

int mutex_create(mutex_p *m)
{
    pdebug(DEBUG_DETAIL, "Starting.");

    if(*m) {
        pdebug(DEBUG_WARN, "Called with non-NULL pointer!");
    }

    *m = (struct mutex_t *)mem_alloc(sizeof(struct mutex_t));

    if(! *m) {
        pdebug(DEBUG_ERROR,"null mutex pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(pthread_mutex_init(&((*m)->p_mutex),NULL)) {
        mem_free(*m);
        *m = NULL;
        pdebug(DEBUG_ERROR,"Error initializing mutex.");
        return PLCTAG_ERR_MUTEX_INIT;
    }

    (*m)->initialized = 1;

    pdebug(DEBUG_DETAIL, "Done creating mutex %p.", *m);

    return PLCTAG_STATUS_OK;
}


int mutex_lock_impl(const char *func, int line, mutex_p m)
{
    pdebug(DEBUG_SPEW,"locking mutex %p, called from %s:%d.", m, func, line);

    if(!m) {
        pdebug(DEBUG_WARN, "null mutex pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!m->initialized) {
        return PLCTAG_ERR_MUTEX_INIT;
    }

    if(pthread_mutex_lock(&(m->p_mutex))) {
        pdebug(DEBUG_WARN, "error locking mutex.");
        return PLCTAG_ERR_MUTEX_LOCK;
    }

    //pdebug(DEBUG_SPEW,"Done.");

    return PLCTAG_STATUS_OK;
}


int mutex_try_lock_impl(const char *func, int line, mutex_p m)
{
    pdebug(DEBUG_SPEW,"trying to lock mutex %p, called from %s:%d.", m, func, line);

    if(!m) {
        pdebug(DEBUG_WARN, "null mutex pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!m->initialized) {
        return PLCTAG_ERR_MUTEX_INIT;
    }

    if(pthread_mutex_trylock(&(m->p_mutex))) {
        pdebug(DEBUG_SPEW, "error locking mutex.");
        return PLCTAG_ERR_MUTEX_LOCK;
    }

    /*pdebug(DEBUG_DETAIL,"Done.");*/

    return PLCTAG_STATUS_OK;
}



int mutex_unlock_impl(const char *func, int line, mutex_p m)
{
    pdebug(DEBUG_SPEW,"unlocking mutex %p, called from %s:%d.", m, func, line);

    if(!m) {
        pdebug(DEBUG_WARN,"null mutex pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!m->initialized) {
        return PLCTAG_ERR_MUTEX_INIT;
    }

    if(pthread_mutex_unlock(&(m->p_mutex))) {
        pdebug(DEBUG_WARN, "error unlocking mutex.");
        return PLCTAG_ERR_MUTEX_UNLOCK;
    }

    //pdebug(DEBUG_SPEW,"Done.");

    return PLCTAG_STATUS_OK;
}


int mutex_destroy(mutex_p *m)
{
    pdebug(DEBUG_DETAIL, "Starting to destroy mutex %p.", m);

    if(!m || !*m) {
        pdebug(DEBUG_WARN, "null mutex pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(pthread_mutex_destroy(&((*m)->p_mutex))) {
        pdebug(DEBUG_WARN, "error while attempting to destroy mutex.");
        return PLCTAG_ERR_MUTEX_DESTROY;
    }

    mem_free(*m);

    *m = NULL;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}




/***************************************************************************
 *************************** Condition Variables ***************************
 **************************************************************************/

struct condition_var_s {
    pthread_mutex_t p_mutex;
    pthread_cond_t p_cond;
    int initialized;
};



int condition_var_create(condition_var_p *var)
{
    pdebug(DEBUG_DETAIL, "Starting.");

    if(*var) {
        pdebug(DEBUG_WARN, "Called with non-NULL pointer!");
    }

    *var = (struct condition_var_s *)mem_alloc(sizeof(struct condition_var_s));

    if(! *var) {
        pdebug(DEBUG_ERROR,"null mutex pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(pthread_mutex_init(&((*var)->p_mutex),NULL)) {
        mem_free(*var);
        *var = NULL;
        pdebug(DEBUG_ERROR,"Error initializing pthread mutex.");
        return PLCTAG_ERR_CREATE;
    }

    if(pthread_cond_init(&((*var)->p_cond), NULL)) {
        pthread_mutex_destroy(&((*var)->p_mutex));
        mem_free(*var);
        *var = NULL;
        pdebug(DEBUG_ERROR,"Error initializing pthread condition variable.");
        return PLCTAG_ERR_CREATE;
    }

    (*var)->initialized = 1;

    pdebug(DEBUG_DETAIL, "Done creating condition variable %p.", *var);

    return PLCTAG_STATUS_OK;
}



int condition_var_destroy(condition_var_p *var)
{
    pdebug(DEBUG_DETAIL, "Starting to destroy condition variable %p.", var);

    if(!var || !*var) {
        pdebug(DEBUG_WARN, "null condition variable pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(pthread_mutex_destroy(&((*var)->p_mutex))) {
        pdebug(DEBUG_WARN, "error while attempting to destroy pthread mutex.");
        return PLCTAG_ERR_MUTEX_DESTROY;
    }

    if(pthread_cond_destroy(&((*var)->p_cond))) {
        pdebug(DEBUG_WARN, "error while attempting to destroy pthread condition variable.");
        return PLCTAG_ERR_MUTEX_DESTROY;
    }

    mem_free(*var);

    *var = NULL;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


int condition_var_wait_impl(const char *func, int line, condition_var_p var, int64_t timeout_wake_time)
{
    int rc = PLCTAG_STATUS_OK;
    struct timespec abstime;

    pdebug(DEBUG_SPEW,"Waiting on condition var %p, called from %s:%d.", var, func, line);

    if(!var) {
        pdebug(DEBUG_WARN, "Null condition var pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!var->initialized) {
        return PLCTAG_ERR_BAD_STATUS;
    }

    if(timeout_wake_time <= 0) {
        pdebug(DEBUG_WARN, "Illegal wake up time.  Time must be positive!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    abstime.tv_sec = timeout_wake_time / 1000;
    abstime.tv_nsec = (timeout_wake_time % 1000) * (int64_t)1000000;

    /* first lock the mutex */
    if(pthread_mutex_lock(&(var->p_mutex))) {
        pdebug(DEBUG_WARN, "error locking mutex.");
        return PLCTAG_ERR_MUTEX_LOCK;
    }

    /* now wait on the condition variable. */
    rc = pthread_cond_timedwait(&(var->p_cond), &(var->p_mutex), &abstime);
    if(rc) {
        /* either timeout or another error. */
        if(rc == ETIMEDOUT) {
            /* timeout, translate the error. */
            rc = PLCTAG_ERR_TIMEOUT;
        } else {
            pdebug(DEBUG_WARN, "Unable to wait on condition variable!");
            rc = PLCTAG_ERR_MUTEX_LOCK; /* should have a better error. */
        }
    } else {
        /* no error, the condition was signaled.  Perhaps. */
        rc = PLCTAG_STATUS_OK;
    }

    /* try to unlock the mutex. */
    pthread_mutex_unlock(&(var->p_mutex));

    pdebug(DEBUG_SPEW,"Done.");

    return rc;
}


int condition_var_signal_impl(const char *func, int line, condition_var_p var)
{
    pdebug(DEBUG_SPEW,"trying to signal condition variable %p, called from %s:%d.", var, func, line);

    if(!var) {
        pdebug(DEBUG_WARN, "null condition var pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!var->initialized) {
        return PLCTAG_ERR_BAD_STATUS;
    }

    if(pthread_cond_signal(&(var->p_cond))) {
        pdebug(DEBUG_SPEW, "error signaling condition var!");
        return PLCTAG_ERR_BAD_REPLY;
    }

    pdebug(DEBUG_SPEW,"Done.");

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
 * FIXME - use the stacksize!
 */

extern int thread_create(thread_p *t, thread_func_t func, int stacksize, void *arg)
{
    pdebug(DEBUG_DETAIL, "Starting.");

    pdebug(DEBUG_DETAIL, "Warning: ignoring stacksize (%d) parameter.", stacksize);

    if(!t) {
        pdebug(DEBUG_WARN, "null thread pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    *t = (thread_p)mem_alloc(sizeof(struct thread_t));

    if(! *t) {
        pdebug(DEBUG_ERROR, "Failed to allocate memory for thread.");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* create a pthread.  0 means success. */
    if(pthread_create(&((*t)->p_thread), NULL, func, arg)) {
        pdebug(DEBUG_ERROR, "error creating thread.");
        return PLCTAG_ERR_THREAD_CREATE;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * platform_thread_stop()
 *
 * Stop the current thread.  Does not take arguments.  Note: the thread
 * ends completely and this function does not return!
 */
void thread_stop(void)
{
    pthread_exit((void*)0);
}


/*
 * thread_kill()
 *
 * Kill another thread.
 */

void thread_kill(thread_p t)
{
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

int thread_join(thread_p t)
{
    void *unused;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!t) {
        pdebug(DEBUG_WARN, "null thread pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(pthread_join(t->p_thread,&unused)) {
        pdebug(DEBUG_ERROR, "Error joining thread.");
        return PLCTAG_ERR_THREAD_JOIN;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * thread_detach
 *
 * Detach the thread.  You cannot call thread_join on a detached thread!
 */

extern int thread_detach()
{
    pthread_detach(pthread_self());

    return PLCTAG_STATUS_OK;
}



/*
 * thread_destroy
 *
 * This gets rid of the resources of a thread struct.  The thread in
 * question must be dead first!
 */
extern int thread_destroy(thread_p *t)
{
    pdebug(DEBUG_DETAIL, "Starting.");

    if(!t || ! *t) {
        pdebug(DEBUG_WARN, "null thread pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    mem_free(*t);

    *t = NULL;

    pdebug(DEBUG_DETAIL, "Done.");

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

extern int lock_acquire_try(lock_t *lock)
{
    int rc = __sync_lock_test_and_set((int*)lock, ATOMIC_LOCK_VAL);

    if(rc != ATOMIC_LOCK_VAL) {
        return 1;
    } else {
        return 0;
    }
}

int lock_acquire(lock_t *lock)
{
    while(!lock_acquire_try(lock)) ;

    return 1;
}


extern void lock_release(lock_t *lock)
{
    __sync_lock_release((int*)lock);
    /*pdebug("released lock");*/
}


/***************************************************************************
 ******************************* Sockets ***********************************
 **************************************************************************/

struct sock_t {
    int fd;
    int port;
    int is_open;
};


#define MAX_IPS (8)

extern int socket_create(sock_p *s)
{
    pdebug(DEBUG_DETAIL, "Starting.");

    if(!s) {
        pdebug(DEBUG_WARN, "null socket pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    *s = (sock_p)mem_alloc(sizeof(struct sock_t));

    if(! *s) {
        pdebug(DEBUG_ERROR, "Failed to allocate memory for socket.");
        return PLCTAG_ERR_NO_MEM;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


extern int socket_connect_tcp(sock_p s, const char *host, int port)
{
    struct in_addr ips[MAX_IPS];
    int num_ips = 0;
    struct sockaddr_in gw_addr;
    int sock_opt = 1;
    int i = 0;
    int done = 0;
    int fd;
    int flags;
    struct timeval timeout; /* used for timing out connections etc. */
    struct linger so_linger; /* used to set up short/no lingering after connections are close()ed. */

    pdebug(DEBUG_DETAIL,"Starting.");

    /* Open a socket for communication with the gateway. */
    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    /* check for errors */
    if(fd < 0) {
        pdebug(DEBUG_ERROR,"Socket creation failed, errno: %d",errno);
        return PLCTAG_ERR_OPEN;
    }

    /* set up our socket to allow reuse if we crash suddenly. */
    sock_opt = 1;

    if(setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,(char*)&sock_opt,sizeof(sock_opt))) {
        close(fd);
        pdebug(DEBUG_ERROR, "Error setting socket reuse option, errno: %d",errno);
        return PLCTAG_ERR_OPEN;
    }

#ifdef BSD_OS_TYPE
    /* The *BSD family has a different way to suppress SIGPIPE on sockets. */
    if(setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (char*)&sock_opt, sizeof(sock_opt))) {
        close(fd);
        pdebug(DEBUG_ERROR, "Error setting socket SIGPIPE suppression option, errno: %d", errno);
        return PLCTAG_ERR_OPEN;
    }
#endif

    timeout.tv_sec = 10;
    timeout.tv_usec = 0;

    if(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout))) {
        close(fd);
        pdebug(DEBUG_ERROR,"Error setting socket receive timeout option, errno: %d",errno);
        return PLCTAG_ERR_OPEN;
    }

    if(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout))) {
        close(fd);
        pdebug(DEBUG_ERROR, "Error setting socket set timeout option, errno: %d",errno);
        return PLCTAG_ERR_OPEN;
    }

    /* abort the connection immediately upon close. */
    so_linger.l_onoff = 1;
    so_linger.l_linger = 0;

    if(setsockopt(fd, SOL_SOCKET, SO_LINGER,(char*)&so_linger,sizeof(so_linger))) {
        close(fd);
        pdebug(DEBUG_ERROR,"Error setting socket close linger option, errno: %d",errno);
        return PLCTAG_ERR_OPEN;
    }

    /* figure out what address we are connecting to. */

    /* try a numeric IP address conversion first. */
    if(inet_pton(AF_INET,host,(struct in_addr *)ips) > 0) {
        pdebug(DEBUG_DETAIL, "Found numeric IP address: %s",host);
        num_ips = 1;
    } else {
        struct addrinfo hints;
        struct addrinfo *res_head = NULL;
        struct addrinfo *res=NULL;
        int rc = 0;

        mem_set(&ips, 0, sizeof(ips));
        mem_set(&hints, 0, sizeof(hints));

        hints.ai_socktype = SOCK_STREAM; /* TCP */
        hints.ai_family = AF_INET; /* IP V4 only */

        if ((rc = getaddrinfo(host, NULL, &hints, &res_head)) != 0) {
            pdebug(DEBUG_WARN,"Error looking up PLC IP address %s, error = %d\n", host, rc);

            if(res_head) {
                freeaddrinfo(res_head);
            }

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


    /* now try to connect to the remote gateway.  We may need to
     * try several of the IPs we have.
     */

    i = 0;
    done = 0;

    memset((void *)&gw_addr,0, sizeof(gw_addr));
    gw_addr.sin_family = AF_INET ;
    gw_addr.sin_port = htons((uint16_t)port);

    do {
        int rc;
        /* try each IP until we run out or get a connection. */
        gw_addr.sin_addr.s_addr = ips[i].s_addr;

        pdebug(DEBUG_DETAIL, "Attempting to connect to %s",inet_ntoa(*((struct in_addr *)&ips[i])));

        rc = connect(fd,(struct sockaddr *)&gw_addr,sizeof(gw_addr));

        if( rc == 0) {
            pdebug(DEBUG_DETAIL, "Attempt to connect to %s succeeded.",inet_ntoa(*((struct in_addr *)&ips[i])));
            done = 1;
        } else {
            pdebug(DEBUG_DETAIL, "Attempt to connect to %s failed, errno: %d",inet_ntoa(*((struct in_addr *)&ips[i])),errno);
            i++;
        }
    } while(!done && i < num_ips);

    if(!done) {
        close(fd);
        pdebug(DEBUG_ERROR, "Unable to connect to any gateway host IP address!");
        return PLCTAG_ERR_OPEN;
    }


    /* FIXME
     * connect() is a little easier to handle in blocking mode, for now
     * we make the socket non-blocking here, after connect(). */
    flags=fcntl(fd,F_GETFL,0);

    if(flags<0) {
        pdebug(DEBUG_ERROR, "Error getting socket options, errno: %d", errno);
        close(fd);
        return PLCTAG_ERR_OPEN;
    }

    flags |= O_NONBLOCK;

    if(fcntl(fd,F_SETFL,flags)<0) {
        pdebug(DEBUG_ERROR, "Error setting socket to non-blocking, errno: %d", errno);
        close(fd);
        return PLCTAG_ERR_OPEN;
    }

    /* save the values */
    s->fd = fd;
    s->port = port;
    s->is_open = 1;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}




extern int socket_read(sock_p s, uint8_t *buf, int size)
{
    int rc;

    if(!s || !buf) {
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!s->is_open) {
        pdebug(DEBUG_WARN, "Socket is not open!");
        return PLCTAG_ERR_READ;
    }

    /* The socket is non-blocking. */
    rc = (int)read(s->fd,buf,(size_t)size);

    if(rc < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        } else {
            pdebug(DEBUG_WARN,"Socket read error: rc=%d, errno=%d", rc, errno);
            return PLCTAG_ERR_READ;
        }
    }

    return rc;
}


extern int socket_write(sock_p s, uint8_t *buf, int size)
{
    int rc;

    if(!s || !buf) {
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!s->is_open) {
        pdebug(DEBUG_WARN, "Socket is not open!");
        return PLCTAG_ERR_WRITE;
    }

    /* The socket is non-blocking. */
#ifdef BSD_OS_TYPE
    /* On *BSD and macOS, the socket option is set to prevent SIGPIPE. */
    rc = (int)write(s->fd, buf, (size_t)size);
#else
    /* on Linux, we use MSG_NOSIGNAL */
    rc = (int)send(s->fd, buf, (size_t)size, MSG_NOSIGNAL);
#endif

    if(rc < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            return PLCTAG_ERR_NO_DATA;
        } else {
            pdebug(DEBUG_WARN, "Socket write error: rc=%d, errno=%d", rc, errno);
            return PLCTAG_ERR_WRITE;
        }
    }

    return rc;
}



extern int socket_close(sock_p s)
{
    if(!s) {
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!s->is_open) {
        return PLCTAG_STATUS_OK;
    }

    if(!close(s->fd)) {
        return PLCTAG_ERR_CLOSE;
    }

    s->fd = 0;
    s->is_open = 0;

    return PLCTAG_STATUS_OK;
}


extern int socket_destroy(sock_p *s)
{
    if(!s || !*s)
        return PLCTAG_ERR_NULL_PTR;

    socket_close(*s);

    mem_free(*s);

    *s = 0;

    return PLCTAG_STATUS_OK;
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
 * FIXME - should the signal interrupt handling be done here or in app code?
 */
int sleep_ms(int ms)
{
    struct timespec wait_time;
    struct timespec remainder;
    int done = 1;
    int rc = PLCTAG_STATUS_OK;

    if(ms < 0) {
        pdebug(DEBUG_WARN, "called with negative time %d!", ms);
        return PLCTAG_ERR_BAD_PARAM;
    }

    wait_time.tv_sec = ms/1000;
    wait_time.tv_nsec = ((int64_t)ms % 1000)*1000000; /* convert to nanoseconds */

    do {
        int rc = nanosleep(&wait_time, &remainder);
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
 * Return the current epoch time in milliseconds.
 */
int64_t time_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv,NULL);

    return  ((int64_t)tv.tv_sec*1000)+ ((int64_t)tv.tv_usec/1000);
}

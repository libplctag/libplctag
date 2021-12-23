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


#define _GNU_SOURCE

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
#include <inttypes.h>

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
 * str_str_cmp_i
 *
 * Returns a pointer to the location of the needle string in the haystack string
 * or NULL if there is no match.  The comparison is done case-insensitive.
 *
 * Handle the usual edge cases.
 */
extern char *str_str_cmp_i(const char *haystack, const char *needle)
{
    int haystack_zero = !str_length(haystack);
    int needle_zero = !str_length(needle);

    if(haystack_zero) {
        pdebug(DEBUG_DETAIL, "Haystack string is NULL or zero length.");
        return NULL;
    }

    if(needle_zero) {
        pdebug(DEBUG_DETAIL, "Needle string is NULL or zero length.");
        return NULL;
    }

    return strcasestr(haystack, needle);
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
 ************************* Condition Variables *****************************
 ***************************************************************************/

struct cond_t {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int flag;
};

int cond_create(cond_p *c)
{
    int rc = PLCTAG_STATUS_OK;
    cond_p tmp_cond = NULL;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!c) {
        pdebug(DEBUG_WARN, "Null pointer to condition var pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(*c) {
        pdebug(DEBUG_WARN, "Condition var pointer is not null, was it not deleted first?");
    }

    /* clear the output first. */
    *c = NULL;

    tmp_cond = mem_alloc((int)(unsigned int)sizeof(*tmp_cond));
    if(!tmp_cond) {
        pdebug(DEBUG_WARN, "Unable to allocate new condition var!");
        return PLCTAG_ERR_NO_MEM;
    }

    if(pthread_mutex_init(&(tmp_cond->mutex), NULL)) {
        pdebug(DEBUG_WARN, "Unable to initialize pthread mutex!");
        mem_free(tmp_cond);
        return PLCTAG_ERR_CREATE;
    }

    if(pthread_cond_init(&(tmp_cond->cond), NULL)) {
        pdebug(DEBUG_WARN, "Unable to initialize pthread condition var!");
        pthread_mutex_destroy(&(tmp_cond->mutex));
        mem_free(tmp_cond);
        return PLCTAG_ERR_CREATE;
    }

    tmp_cond->flag = 0;

    *c = tmp_cond;

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int cond_wait_impl(const char *func, int line_num, cond_p c, int timeout_ms)
{
    int rc = PLCTAG_STATUS_OK;
    int64_t start_time = time_ms();
    int64_t end_time = start_time + timeout_ms;
    struct timespec timeout;

    pdebug(DEBUG_SPEW, "Starting. Called from %s:%d.", func, line_num);

    if(!c) {
        pdebug(DEBUG_WARN, "Condition var pointer is null in call from %s:%d!", func, line_num);
        return PLCTAG_ERR_NULL_PTR;
    }

    if(timeout_ms <= 0) {
        pdebug(DEBUG_WARN, "Timeout must be a positive value but was %d in call from %s:%d!", timeout_ms, func, line_num);
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(pthread_mutex_lock(& (c->mutex))) {
        pdebug(DEBUG_WARN, "Unable to lock mutex!");
        return PLCTAG_ERR_MUTEX_LOCK;
    }

    /*
     * set up timeout.
     *
     * NOTE: the time is _ABSOLUTE_!  This is not a relative delay.
     */
    timeout.tv_sec = (time_t)(end_time / 1000);
    timeout.tv_nsec = (long)1000000 * (long)(end_time % 1000);

    while(!c->flag) {
        int64_t time_left = (int64_t)timeout_ms - (time_ms() - start_time);

        pdebug(DEBUG_SPEW, "Waiting for %" PRId64 "ms.", time_left);

        if(time_left > 0) {
            int wait_rc = 0;

            wait_rc = pthread_cond_timedwait(&(c->cond), &(c->mutex), &timeout);
            if(wait_rc == ETIMEDOUT) {
                pdebug(DEBUG_SPEW, "Timeout response from condition var wait.");
                rc = PLCTAG_ERR_TIMEOUT;
                break;
            } else if(wait_rc != 0) {
                pdebug(DEBUG_WARN, "Error %d waiting on condition variable!", errno);
                rc = PLCTAG_ERR_BAD_STATUS;
                break;
            } else {
                /* we might need to wait again. could be a spurious wake up. */
                pdebug(DEBUG_SPEW, "Condition var wait returned.");
                rc = PLCTAG_STATUS_OK;
            }
        } else {
            pdebug(DEBUG_DETAIL, "Timed out.");
            rc = PLCTAG_ERR_TIMEOUT;
            break;
        }
    }

    if(c->flag) {
        pdebug(DEBUG_SPEW, "Condition var signaled for call at %s:%d.", func, line_num);

        /* clear the flag now that we've responded. */
        c->flag = 0;
    } else {
        pdebug(DEBUG_SPEW, "Condition wait terminated due to error or timeout for call at %s:%d.", func, line_num);
    }

    if(pthread_mutex_unlock(& (c->mutex))) {
        pdebug(DEBUG_WARN, "Unable to unlock mutex!");
        return PLCTAG_ERR_MUTEX_UNLOCK;
    }

    pdebug(DEBUG_SPEW, "Done for call at %s:%d.", func, line_num);

    return rc;
}


int cond_signal_impl(const char *func, int line_num, cond_p c)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.  Called from %s:%d.", func, line_num);

    if(!c) {
        pdebug(DEBUG_WARN, "Condition var pointer is null in call at %s:%d!", func, line_num);
        return PLCTAG_ERR_NULL_PTR;
    }

    if(pthread_mutex_lock(& (c->mutex))) {
        pdebug(DEBUG_WARN, "Unable to lock mutex!");
        return PLCTAG_ERR_MUTEX_LOCK;
    }

    c->flag = 1;

    if(pthread_cond_signal(&(c->cond))) {
        pdebug(DEBUG_WARN, "Signal of condition var returned error %d in call at %s:%d!", errno, func, line_num);
        rc = PLCTAG_ERR_BAD_STATUS;
    }

    if(pthread_mutex_unlock(& (c->mutex))) {
        pdebug(DEBUG_WARN, "Unable to unlock mutex!");
        return PLCTAG_ERR_MUTEX_UNLOCK;
    }

    pdebug(DEBUG_SPEW, "Done. Called from %s:%d.", func, line_num);

    return rc;
}


int cond_clear_impl(const char *func, int line_num, cond_p c)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.  Called from %s:%d.", func, line_num);

    if(!c) {
        pdebug(DEBUG_WARN, "Condition var pointer is null in call at %s:%d!", func, line_num);
        return PLCTAG_ERR_NULL_PTR;
    }

    if(pthread_mutex_lock(& (c->mutex))) {
        pdebug(DEBUG_WARN, "Unable to lock mutex!");
        return PLCTAG_ERR_MUTEX_LOCK;
    }

    c->flag = 0;

    if(pthread_mutex_unlock(& (c->mutex))) {
        pdebug(DEBUG_WARN, "Unable to unlock mutex!");
        return PLCTAG_ERR_MUTEX_UNLOCK;
    }

    pdebug(DEBUG_SPEW, "Done. Called from %s:%d.", func, line_num);

    return rc;
}


int cond_destroy(cond_p *c)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!c || ! *c) {
        pdebug(DEBUG_WARN, "Condition var pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pthread_cond_destroy(&((*c)->cond));
    pthread_mutex_destroy(&((*c)->mutex));

    mem_free(*c);

    *c = NULL;

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}



/***************************************************************************
 ******************************* Sockets ***********************************
 **************************************************************************/


#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif

struct sock_t {
    int fd;
    int wake_read_fd;
    int wake_write_fd;
    int port;
    int is_open;
};


static int sock_create_event_wakeup_channel(sock_p sock);

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

    (*s)->fd = INVALID_SOCKET;
    (*s)->wake_read_fd = INVALID_SOCKET;
    (*s)->wake_write_fd = INVALID_SOCKET;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


int socket_connect_tcp_start(sock_p s, const char *host, int port)
{
    int rc = PLCTAG_STATUS_OK;
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

    /* make the socket non-blocking. */
    flags=fcntl(fd,F_GETFL,0);
    if(flags<0) {
        pdebug(DEBUG_ERROR, "Error getting socket options, errno: %d", errno);
        close(fd);
        return PLCTAG_ERR_OPEN;
    }

    /* set the non-blocking flag. */
    flags |= O_NONBLOCK;

    if(fcntl(fd,F_SETFL,flags)<0) {
        pdebug(DEBUG_ERROR, "Error setting socket to non-blocking, errno: %d", errno);
        close(fd);
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

    pdebug(DEBUG_DETAIL, "Setting up wake pipe.");
    rc = sock_create_event_wakeup_channel(s);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create wake pipe, error %s!", plc_tag_decode_error(rc));
        return rc;
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
        /* try each IP until we run out or get a connection started. */
        gw_addr.sin_addr.s_addr = ips[i].s_addr;

        pdebug(DEBUG_DETAIL, "Attempting to connect to %s:%d", inet_ntoa(*((struct in_addr *)&ips[i])), port);

        /* this is done non-blocking. Could be interrupted, so restart if needed.*/
        do {
            rc = connect(fd,(struct sockaddr *)&gw_addr,sizeof(gw_addr));
        } while(rc < 0 && errno == EINTR);

        if(rc == 0) {
            /* instantly connected. */
            pdebug(DEBUG_DETAIL, "Connected instantly to %s:%d.", inet_ntoa(*((struct in_addr *)&ips[i])), port);
            done = 1;
            rc = PLCTAG_STATUS_OK;
        } else if(rc < 0 && (errno == EINPROGRESS)) {
            /* the connection has started. */
            pdebug(DEBUG_DETAIL, "Started connecting to %s:%d successfully.", inet_ntoa(*((struct in_addr *)&ips[i])), port);
            done = 1;
            rc = PLCTAG_STATUS_PENDING;
        } else  {
            pdebug(DEBUG_DETAIL, "Attempt to connect to %s:%d failed, errno: %d", inet_ntoa(*((struct in_addr *)&ips[i])),port, errno);
            i++;
        }
    } while(!done && i < num_ips);

    if(!done) {
        close(fd);
        pdebug(DEBUG_ERROR, "Unable to connect to any gateway host IP address!");
        return PLCTAG_ERR_OPEN;
    }

    /* save the values */
    s->fd = fd;
    s->port = port;

    s->is_open = 1;

    pdebug(DEBUG_DETAIL, "Done with status %s.", plc_tag_decode_error(rc));

    return rc;
}



int socket_connect_tcp_check(sock_p sock, int timeout_ms)
{
    int rc = PLCTAG_STATUS_OK;
    fd_set write_set;
    struct timeval tv;
    int select_rc = 0;
    int sock_err = 0;
    socklen_t sock_err_len = (socklen_t)(sizeof(sock_err));


    pdebug(DEBUG_DETAIL,"Starting.");

    if(!sock) {
        pdebug(DEBUG_WARN, "Null socket pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* wait for the socket to be ready. */
    tv.tv_sec = (time_t)(timeout_ms / 1000);
    tv.tv_usec = (suseconds_t)(timeout_ms % 1000) * (suseconds_t)(1000);

    FD_ZERO(&write_set);

    FD_SET(sock->fd, &write_set);

    select_rc = select(sock->fd+1, NULL, &write_set, NULL, &tv);

    if(select_rc == 1) {
        if(FD_ISSET(sock->fd, &write_set)) {
            pdebug(DEBUG_DETAIL, "Socket is probably connected.");
            rc = PLCTAG_STATUS_OK;
        } else {
            pdebug(DEBUG_WARN, "select() returned but socket is not connected!");
            return PLCTAG_ERR_BAD_REPLY;
        }
    } else if(select_rc == 0) {
        pdebug(DEBUG_DETAIL, "Socket connection not done yet.");
        return PLCTAG_ERR_TIMEOUT;
    } else {
        pdebug(DEBUG_WARN, "select() returned status %d!", select_rc);

        switch(errno) {
            case EBADF: /* bad file descriptor */
                pdebug(DEBUG_WARN, "Bad file descriptor used in select()!");
                return PLCTAG_ERR_OPEN;
                break;

            case EINTR: /* signal was caught, this should not happen! */
                pdebug(DEBUG_WARN, "A signal was caught in select() and this should not happen!");
                return PLCTAG_ERR_OPEN;
                break;

            case EINVAL: /* number of FDs was negative or exceeded the max allowed. */
                pdebug(DEBUG_WARN, "The number of fds passed to select() was negative or exceeded the allowed limit or the timeout is invalid!");
                return PLCTAG_ERR_OPEN;
                break;

            case ENOMEM: /* No mem for internal tables. */
                pdebug(DEBUG_WARN, "Insufficient memory for select() to run!");
                return PLCTAG_ERR_NO_MEM;
                break;

            default:
                pdebug(DEBUG_WARN, "Unexpected socket err %d!", errno);
                return PLCTAG_ERR_OPEN;
                break;
        }
    }

    /* now make absolutely sure that the connection is ready. */
    rc = getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, &sock_err, &sock_err_len);
    if(rc == 0) {
        /* sock_err has the error. */
        switch(sock_err) {
            case 0:
                pdebug(DEBUG_DETAIL, "No error, socket is connected.");
                rc = PLCTAG_STATUS_OK;
                break;

            case EBADF:
                pdebug(DEBUG_WARN, "Socket fd is not valid!");
                return PLCTAG_ERR_OPEN;
                break;

            case EFAULT:
                pdebug(DEBUG_WARN, "The address passed to getsockopt() is not a valid user address!");
                return PLCTAG_ERR_OPEN;
                break;

            case EINVAL:
                pdebug(DEBUG_WARN, "The size of the socket error result is invalid!");
                return PLCTAG_ERR_OPEN;
                break;

            case ENOPROTOOPT:
                pdebug(DEBUG_WARN, "The option SO_ERROR is not understood at the SOL_SOCKET level!");
                return PLCTAG_ERR_OPEN;
                break;

            case ENOTSOCK:
                pdebug(DEBUG_WARN, "The FD is not a socket!");
                return PLCTAG_ERR_OPEN;
                break;

            case ECONNREFUSED:
                pdebug(DEBUG_WARN, "Connection refused!");
                return PLCTAG_ERR_OPEN;
                break;

            default:
                pdebug(DEBUG_WARN, "Unexpected error %d returned!", sock_err);
                return PLCTAG_ERR_OPEN;
                break;
        }
    } else {
        pdebug(DEBUG_WARN, "Error %d getting socket connection status!", errno);
        return PLCTAG_ERR_OPEN;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}



int socket_wait_event(sock_p sock, int events, int timeout_ms)
{
    int result = SOCK_EVENT_NONE;
    fd_set read_set;
    fd_set write_set;
    fd_set err_set;
    int max_fd = 0;
    int num_sockets = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!sock) {
        pdebug(DEBUG_WARN, "Null socket pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!sock->is_open) {
        pdebug(DEBUG_WARN, "Socket is not open!");
        return PLCTAG_ERR_READ;
    }

    if(timeout_ms < 0) {
        pdebug(DEBUG_WARN, "Timeout must be zero or positive!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* check if the mask is empty */
    if(events == 0) {
        pdebug(DEBUG_WARN, "Passed event mask is empty!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* set up fd sets */
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    FD_ZERO(&err_set);

    /* calculate the maximum fd */
    max_fd = (sock->fd > sock->wake_read_fd ? sock->fd : sock->wake_read_fd);

    /* add the wake fd */
    FD_SET(sock->wake_read_fd, &read_set);

    /* we always want to know about errors. */
    FD_SET(sock->fd, &err_set);

    /* add more depending on the mask. */
    if(events & SOCK_EVENT_CAN_READ) {
        FD_SET(sock->fd, &read_set);
    }

    if((events & SOCK_EVENT_CONNECT) || (events & SOCK_EVENT_CAN_WRITE)) {
        FD_SET(sock->fd, &write_set);
    }

    /* calculate the timeout. */
    if(timeout_ms > 0) {
        struct timeval tv;

        tv.tv_sec = (time_t)(timeout_ms / 1000);
        tv.tv_usec = (suseconds_t)(timeout_ms % 1000) * (suseconds_t)(1000);

        num_sockets = select(max_fd + 1, &read_set, &write_set, &err_set, &tv);
    } else {
        num_sockets = select(max_fd + 1, &read_set, &write_set, &err_set, NULL);
    }

    if(num_sockets == 0) {
        result |= (events & SOCK_EVENT_TIMEOUT);
    } else if(num_sockets > 0) {
        /* was there a wake up? */
        if(FD_ISSET(sock->wake_read_fd, &read_set)) {
            int bytes_read = 0;
            char buf[32];

            /* empty the socket. */
            while((bytes_read = (int)read(sock->wake_read_fd, &buf[0], sizeof(buf))) > 0) { }

            pdebug(DEBUG_DETAIL, "Socket woken up.");
            result |= (events & SOCK_EVENT_WAKE_UP);
        }

        /* is read ready for the main fd? */
        if(FD_ISSET(sock->fd, &read_set)) {
            char buf;
            int byte_read = 0;

            byte_read = (int)recv(sock->fd, &buf, sizeof(buf), MSG_PEEK);

            if(byte_read) {
                pdebug(DEBUG_DETAIL, "Socket can read.");
                result |= (events & SOCK_EVENT_CAN_READ);
            } else {
                pdebug(DEBUG_DETAIL, "Socket disconnected.");
                result |= (events & SOCK_EVENT_DISCONNECT);
            }
        }

        /* is write ready for the main fd? */
        if(FD_ISSET(sock->fd, &write_set)) {
            pdebug(DEBUG_DETAIL, "Socket can write or just connected.");
            result |= ((events & SOCK_EVENT_CAN_WRITE) | (events & SOCK_EVENT_CONNECT));
        }

        /* is there an error? */
        if(FD_ISSET(sock->fd, &err_set)) {
            pdebug(DEBUG_DETAIL, "Socket has error!");
            result |= (events & SOCK_EVENT_ERROR);
        }
    } else {
        /* error */
        pdebug(DEBUG_WARN, "select() returned status %d!", num_sockets);

        switch(errno) {
            case EBADF: /* bad file descriptor */
                pdebug(DEBUG_WARN, "Bad file descriptor used in select()!");
                return PLCTAG_ERR_BAD_PARAM;
                break;

            case EINTR: /* signal was caught, this should not happen! */
                pdebug(DEBUG_WARN, "A signal was caught in select() and this should not happen!");
                return PLCTAG_ERR_BAD_CONFIG;
                break;

            case EINVAL: /* number of FDs was negative or exceeded the max allowed. */
                pdebug(DEBUG_WARN, "The number of fds passed to select() was negative or exceeded the allowed limit or the timeout is invalid!");
                return PLCTAG_ERR_BAD_PARAM;
                break;

            case ENOMEM: /* No mem for internal tables. */
                pdebug(DEBUG_WARN, "Insufficient memory for select() to run!");
                return PLCTAG_ERR_NO_MEM;
                break;

            default:
                pdebug(DEBUG_WARN, "Unexpected socket err %d!", errno);
                return PLCTAG_ERR_BAD_STATUS;
                break;
        }
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return result;
}


int socket_wake(sock_p sock)
{
    int rc = PLCTAG_STATUS_OK;
    const char dummy_data[] = "Dummy data.";

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!sock) {
        pdebug(DEBUG_WARN, "Null socket pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!sock->is_open) {
        pdebug(DEBUG_WARN, "Socket is not open!");
        return PLCTAG_ERR_READ;
    }

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
        pdebug(DEBUG_WARN, "Socket write error: rc=%d, errno=%d", rc, errno);
        return PLCTAG_ERR_WRITE;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}



int socket_read(sock_p s, uint8_t *buf, int size, int timeout_ms)
{
    int rc;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!s) {
        pdebug(DEBUG_WARN, "Socket pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!buf) {
        pdebug(DEBUG_WARN, "Buffer pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!s->is_open) {
        pdebug(DEBUG_WARN, "Socket is not open!");
        return PLCTAG_ERR_READ;
    }

    if(timeout_ms < 0) {
        pdebug(DEBUG_WARN, "Timeout must be zero or positive!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * Try to read immediately.   If we get data, we skip any other
     * delays.   If we do not, then see if we have a timeout.
     */

    /* The socket is non-blocking. */
    rc = (int)read(s->fd,buf,(size_t)size);
    if(rc < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            if(timeout_ms > 0) {
                pdebug(DEBUG_DETAIL, "Immediate read attempt did not succeed, now wait for select().");
            } else {
                pdebug(DEBUG_DETAIL, "Read resulted in no data.");
            }

            rc = 0;
        } else {
            pdebug(DEBUG_WARN,"Socket read error: rc=%d, errno=%d", rc, errno);
            return PLCTAG_ERR_READ;
        }
    }

    /* only wait if we have a timeout and no error and no data. */
    if(rc == 0 && timeout_ms > 0) {
        fd_set read_set;
        struct timeval tv;
        int select_rc = 0;

        tv.tv_sec = (time_t)(timeout_ms / 1000);
        tv.tv_usec = (suseconds_t)(timeout_ms % 1000) * (suseconds_t)(1000);

        FD_ZERO(&read_set);

        FD_SET(s->fd, &read_set);

        select_rc = select(s->fd+1, &read_set, NULL, NULL, &tv);
        if(select_rc == 1) {
            if(FD_ISSET(s->fd, &read_set)) {
                pdebug(DEBUG_DETAIL, "Socket can read data.");
            } else {
                pdebug(DEBUG_WARN, "select() returned but socket is not ready to read data!");
                return PLCTAG_ERR_BAD_REPLY;
            }
        } else if(select_rc == 0) {
            pdebug(DEBUG_DETAIL, "Socket read timed out.");
            return PLCTAG_ERR_TIMEOUT;
        } else {
            pdebug(DEBUG_WARN, "select() returned status %d!", select_rc);

            switch(errno) {
                case EBADF: /* bad file descriptor */
                    pdebug(DEBUG_WARN, "Bad file descriptor used in select()!");
                    return PLCTAG_ERR_BAD_PARAM;
                    break;

                case EINTR: /* signal was caught, this should not happen! */
                    pdebug(DEBUG_WARN, "A signal was caught in select() and this should not happen!");
                    return PLCTAG_ERR_BAD_CONFIG;
                    break;

                case EINVAL: /* number of FDs was negative or exceeded the max allowed. */
                    pdebug(DEBUG_WARN, "The number of fds passed to select() was negative or exceeded the allowed limit or the timeout is invalid!");
                    return PLCTAG_ERR_BAD_PARAM;
                    break;

                case ENOMEM: /* No mem for internal tables. */
                    pdebug(DEBUG_WARN, "Insufficient memory for select() to run!");
                    return PLCTAG_ERR_NO_MEM;
                    break;

                default:
                    pdebug(DEBUG_WARN, "Unexpected socket err %d!", errno);
                    return PLCTAG_ERR_BAD_STATUS;
                    break;
            }
        }

        /* try to read again. */
        rc = (int)read(s->fd,buf,(size_t)size);
        if(rc < 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                pdebug(DEBUG_DETAIL, "No data read.");
                rc = 0;
            } else {
                pdebug(DEBUG_WARN,"Socket read error: rc=%d, errno=%d", rc, errno);
                return PLCTAG_ERR_READ;
            }
        }
    }

    pdebug(DEBUG_DETAIL, "Done: result %d.", rc);

    return rc;
}



int socket_write(sock_p s, uint8_t *buf, int size, int timeout_ms)
{
    int rc;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!s) {
        pdebug(DEBUG_WARN, "Socket pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!buf) {
        pdebug(DEBUG_WARN, "Buffer pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!s->is_open) {
        pdebug(DEBUG_WARN, "Socket is not open!");
        return PLCTAG_ERR_WRITE;
    }

    if(timeout_ms < 0) {
        pdebug(DEBUG_WARN, "Timeout must be zero or positive!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * Try to write without waiting.
     *
     * In the case that we can immediately write, then we skip a
     * system call to select().   If we cannot, then we will
     * call select().
     */

#ifdef BSD_OS_TYPE
    /* On *BSD and macOS, the socket option is set to prevent SIGPIPE. */
    rc = (int)write(s->fd, buf, (size_t)size);
#else
    /* on Linux, we use MSG_NOSIGNAL */
    rc = (int)send(s->fd, buf, (size_t)size, MSG_NOSIGNAL);
#endif

    if(rc < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            rc = 0;
        } else {
            pdebug(DEBUG_WARN, "Socket write error: rc=%d, errno=%d", rc, errno);
            return PLCTAG_ERR_WRITE;
        }
    }

    /* only wait if we have a timeout and no error and wrote no data. */
    if(rc == 0 && timeout_ms > 0) {
        fd_set write_set;
        struct timeval tv;
        int select_rc = 0;

        tv.tv_sec = (time_t)(timeout_ms / 1000);
        tv.tv_usec = (suseconds_t)(timeout_ms % 1000) * (suseconds_t)(1000);

        FD_ZERO(&write_set);

        FD_SET(s->fd, &write_set);

        select_rc = select(s->fd+1, NULL, &write_set, NULL, &tv);
        if(select_rc == 1) {
            if(FD_ISSET(s->fd, &write_set)) {
                pdebug(DEBUG_DETAIL, "Socket can write data.");
            } else {
                pdebug(DEBUG_WARN, "select() returned but socket is not ready to write data!");
                return PLCTAG_ERR_BAD_REPLY;
            }
        } else if(select_rc == 0) {
            pdebug(DEBUG_DETAIL, "Socket write timed out.");
            return PLCTAG_ERR_TIMEOUT;
        } else {
            pdebug(DEBUG_WARN, "select() returned status %d!", select_rc);

            switch(errno) {
                case EBADF: /* bad file descriptor */
                    pdebug(DEBUG_WARN, "Bad file descriptor used in select()!");
                    return PLCTAG_ERR_BAD_PARAM;
                    break;

                case EINTR: /* signal was caught, this should not happen! */
                    pdebug(DEBUG_WARN, "A signal was caught in select() and this should not happen!");
                    return PLCTAG_ERR_BAD_CONFIG;
                    break;

                case EINVAL: /* number of FDs was negative or exceeded the max allowed. */
                    pdebug(DEBUG_WARN, "The number of fds passed to select() was negative or exceeded the allowed limit or the timeout is invalid!");
                    return PLCTAG_ERR_BAD_PARAM;
                    break;

                case ENOMEM: /* No mem for internal tables. */
                    pdebug(DEBUG_WARN, "Insufficient memory for select() to run!");
                    return PLCTAG_ERR_NO_MEM;
                    break;

                default:
                    pdebug(DEBUG_WARN, "Unexpected socket err %d!", errno);
                    return PLCTAG_ERR_BAD_STATUS;
                    break;
            }
        }

        /* select() passed and said we can write, so try. */
    #ifdef BSD_OS_TYPE
        /* On *BSD and macOS, the socket option is set to prevent SIGPIPE. */
        rc = (int)write(s->fd, buf, (size_t)size);
    #else
        /* on Linux, we use MSG_NOSIGNAL */
        rc = (int)send(s->fd, buf, (size_t)size, MSG_NOSIGNAL);
    #endif

        if(rc < 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                pdebug(DEBUG_DETAIL, "No data written.");
                rc = 0;
            } else {
                pdebug(DEBUG_WARN, "Socket write error: rc=%d, errno=%d", rc, errno);
                return PLCTAG_ERR_WRITE;
            }
        }
    }

    pdebug(DEBUG_DETAIL, "Done: result = %d.", rc);

    return rc;
}



int socket_close(sock_p s)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    if(!s) {
        pdebug(DEBUG_WARN, "Socket pointer or pointer to socket pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(s->wake_read_fd != INVALID_SOCKET) {
        if(close(s->wake_read_fd)) {
            pdebug(DEBUG_WARN, "Error closing read wake socket!");
            rc = PLCTAG_ERR_CLOSE;
        }

        s->wake_read_fd = INVALID_SOCKET;
    }

    if(s->wake_write_fd != INVALID_SOCKET) {
        if(close(s->wake_write_fd)) {
            pdebug(DEBUG_WARN, "Error closing write wake socket!");
            rc = PLCTAG_ERR_CLOSE;
        }

        s->wake_write_fd = INVALID_SOCKET;
    }

    if(s->fd != INVALID_SOCKET) {
        if(close(s->fd)) {
            pdebug(DEBUG_WARN, "Error closing socket!");
            rc = PLCTAG_ERR_CLOSE;
        }

        s->fd = INVALID_SOCKET;
    }

    s->is_open = 0;

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}




int socket_destroy(sock_p *s)
{
    pdebug(DEBUG_INFO, "Starting.");

    if(!s || !*s) {
        pdebug(DEBUG_WARN, "Socket pointer or pointer to socket pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    socket_close(*s);

    mem_free(*s);

    *s = NULL;

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}





int sock_create_event_wakeup_channel(sock_p sock)
{
    int rc = PLCTAG_STATUS_OK;
    int flags = 0;
    int wake_fds[2] = { 0 };
#ifdef BSD_OS_TYPE
    int sock_opt=1;
#endif

    pdebug(DEBUG_INFO, "Starting.");

    do {
        /* open the pipe for waking the select wait. */
        // if(pipe(wake_fds)) {
        if(socketpair(PF_LOCAL, SOCK_STREAM, 0, wake_fds)) {
            pdebug(DEBUG_WARN, "Unable to open waker pipe!");
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

#ifdef BSD_OS_TYPE
        /* The *BSD family has a different way to suppress SIGPIPE on sockets. */
        if(setsockopt(wake_fds[0], SOL_SOCKET, SO_NOSIGPIPE, (char*)&sock_opt, sizeof(sock_opt))) {
            pdebug(DEBUG_ERROR, "Error setting wake fd read socket SIGPIPE suppression option, errno: %d", errno);
            rc = PLCTAG_ERR_OPEN;
            break;
        }

        if(setsockopt(wake_fds[1], SOL_SOCKET, SO_NOSIGPIPE, (char*)&sock_opt, sizeof(sock_opt))) {
            pdebug(DEBUG_ERROR, "Error setting wake fd write socket SIGPIPE suppression option, errno: %d", errno);
            rc = PLCTAG_ERR_OPEN;
            break;
        }
#endif

        /* make the read pipe fd non-blocking. */
        if((flags = fcntl(wake_fds[0], F_GETFL)) < 0) {
            pdebug(DEBUG_WARN, "Unable to get flags of read socket fd!");
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        /* set read fd non-blocking */
        flags |= O_NONBLOCK;

        if(fcntl(wake_fds[0], F_SETFL, flags) < 0) {
            pdebug(DEBUG_WARN, "Unable to set flags of read socket fd!");
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        /* make the write pipe fd non-blocking. */
        if((flags = fcntl(wake_fds[1], F_GETFL)) < 0) {
            pdebug(DEBUG_WARN, "Unable to get flags of write socket fd!");
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        /* set write fd non-blocking */
        flags |= O_NONBLOCK;

        if(fcntl(wake_fds[1], F_SETFL, flags) < 0) {
            pdebug(DEBUG_WARN, "Unable to set flags of write socket fd!");
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        sock->wake_read_fd = wake_fds[0];
        sock->wake_write_fd = wake_fds[1];
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to open waker socket!");

        if(wake_fds[0] != INVALID_SOCKET) {
            close(wake_fds[0]);
            wake_fds[0] = INVALID_SOCKET;
        }

        if(wake_fds[1] != INVALID_SOCKET) {
            close(wake_fds[1]);
            wake_fds[1] = INVALID_SOCKET;
        }
    } else {
        pdebug(DEBUG_INFO, "Done.");
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
    wait_time.tv_nsec = ((long)ms % 1000)*1000000; /* convert to nanoseconds */

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

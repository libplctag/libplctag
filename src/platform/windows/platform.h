/***************************************************************************
 *   Copyright (C) 2015 by OmanTek                                         *
 *   Author Kyle Hayes  kylehayes@omantek.com                              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

 /**************************************************************************
  * CHANGE LOG                                                             *
  *                                                                        *
  * 2012-07-05  KRH - Created file.                                        *
  *                                                                        *
  * 2012-07-25  KRH - Updates for new thread API.                          *
  *                                                                        *
  * 2013-12-23  KRH - re-port to Windows via VS2012.                       *
  *                                                                        *
  **************************************************************************/

/***************************************************************************
 ****************************** WINDOWS ************************************
 **************************************************************************/



#ifndef __PLATFORM_H__
#define __PLATFORM_H__

/*#ifdef __cplusplus
extern "C"
{
#endif
*/

#define _WINSOCKAPI_
#include <windows.h>
#include <tchar.h>
#include <strsafe.h>
#include <io.h>
#include <Winsock2.h>
#include <Ws2tcpip.h>
#include <string.h>
#include <stdlib.h>
#include <winnt.h>
#include <errno.h>
#include <math.h>
#include <process.h>
#include <time.h>
#include <stdio.h>

/*#include <WinSock2.h>*/
/*#include <Ws2tcpip.h>*/
/*#include <sys/types.h>*/
#include <stdint.h>
/*#include <io.h>*/
/*#include <stdlib.h>*/
#include <malloc.h>

//#include "libplctag_lib.h"



/* WinSock does not define this or support signals */
#define MSG_NOSIGNAL 0

#ifdef _MSC_VER
    /* MS Visual Studio C compiler. */
    #define START_PACK __pragma( pack(push, 1) )
    #define END_PACK   __pragma( pack(pop) )
    #define __PRETTY_FUNCTION__ __FUNCTION__
#else
    /* MinGW on Windows. */
    #define START_PACK
    #define END_PACK  __attribute__((packed))
    #define __PRETTY_FUNCTION__  __func__
#endif

/* VS C++ uses foo[] to denote a zero length array. */
#define ZLA_SIZE

/* export definitions. */

#define USE_STD_VARARG_MACROS 1





#ifndef COUNT_NARG
#define COUNT_NARG(...)                                                \
         COUNT_NARG_(__VA_ARGS__,COUNT_RSEQ_N())
#endif

#ifndef COUNT_NARG_
#define COUNT_NARG_(...)                                               \
         COUNT_ARG_N(__VA_ARGS__)
#endif

#ifndef COUNT_ARG_N
#define COUNT_ARG_N(                                                   \
          _1, _2, _3, _4, _5, _6, _7, _8, _9,_10, \
         _11,_12,_13,_14,_15,_16,_17,_18,_19,_20, \
         _21,_22,_23,_24,_25,_26,_27,_28,_29,_30, \
         _31,_32,_33,_34,_35,_36,_37,_38,_39,_40, \
         _41,_42,_43,_44,_45,_46,_47,_48,_49,_50, \
         _51,_52,_53,_54,_55,_56,_57,_58,_59,_60, \
         _61,_62,_63,N,...) N
#endif

#ifndef COUNT_RSEQ_N
#define COUNT_RSEQ_N()                                                 \
         63,62,61,60,                   \
         59,58,57,56,55,54,53,52,51,50, \
         49,48,47,46,45,44,43,42,41,40, \
         39,38,37,36,35,34,33,32,31,30, \
         29,28,27,26,25,24,23,22,21,20, \
         19,18,17,16,15,14,13,12,11,10, \
         9,8,7,6,5,4,3,2,1,0
#endif



/* memory functions/defs */
extern void *mem_alloc(int size);
extern void *mem_realloc(void *orig, int size);
extern void mem_free(const void *mem);
extern void mem_set(void *d1, int c, int size);
extern void mem_copy(void *dest, void *src, int size);
extern void mem_move(void *dest, void *src, int size);
extern int mem_cmp(void *src1, int src1_size, void *src2, int src2_size);

/* string functions/defs */
extern int str_cmp(const char *first, const char *second);
extern int str_cmp_i(const char *first, const char *second);
extern int str_copy(char *dst, int dst_size, const char *src);
extern int str_length(const char *str);
extern char *str_dup(const char *str);
extern int str_to_int(const char *str, int *val);
extern int str_to_float(const char *str, float *val);
extern char **str_split(const char *str, const char *sep);
#define str_concat(s1, ...) str_concat_impl(COUNT_NARG(__VA_ARGS__)+1, s1, __VA_ARGS__)
extern char *str_concat_impl(int num_args, ...);

/* mutex functions/defs */
typedef struct mutex_t *mutex_p;
extern int mutex_create(mutex_p *m);
extern int mutex_lock(mutex_p m);
extern int mutex_try_lock(mutex_p m);
extern int mutex_unlock(mutex_p m);
extern int mutex_destroy(mutex_p *m);

/* macros are evil */

/*
 * Use this one like this:
 *
 *     critical_block(my_mutex) {
 *         locked_data++;
 *         foo(locked_data);
 *     }
 *
 * The macro locks and unlocks for you.  Derived from ideas/code on StackOverflow.com.
 *
 * Do not use break, return, goto or continue inside the synchronized block if
 * you intend to have them apply to a loop outside the synchronized block.
 *
 * You can use break, but it will drop out of the inner for loop and correctly
 * unlock the mutex.  It will NOT break out of any surrounding loop outside the
 * synchronized block.
 */

#define PLCTAG_CAT2(a,b) a##b
#define PLCTAG_CAT(a,b) PLCTAG_CAT2(a,b)
#define LINE_ID(base) PLCTAG_CAT(base,__LINE__)

#define critical_block(lock) \
for(int LINE_ID(__sync_flag_nargle_) = 1; LINE_ID(__sync_flag_nargle_); LINE_ID(__sync_flag_nargle_) = 0, mutex_unlock(lock))  for(int LINE_ID(__sync_rc_nargle_) = mutex_lock(lock); LINE_ID(__sync_rc_nargle_) == PLCTAG_STATUS_OK && LINE_ID(__sync_flag_nargle_) ; LINE_ID(__sync_flag_nargle_) = 0)

/* thread functions/defs */
typedef struct thread_t *thread_p;
//typedef PTHREAD_START_ROUTINE thread_func_t;
//typedef DWORD /*WINAPI*/ (*thread_func_t)(void *lpParam );
extern int thread_create(thread_p *t, LPTHREAD_START_ROUTINE func, int stacksize, void *arg);
extern void thread_stop(void);
extern void thread_kill(thread_p t);
extern int thread_join(thread_p t);
extern int thread_detach();
extern int thread_destroy(thread_p *t);

#define THREAD_FUNC(func) DWORD __stdcall func(LPVOID arg)
#define THREAD_RETURN(val) return (DWORD)val;

#define THREAD_LOCAL __declspec(thread)

/* atomic operations */
#define spin_block(lock) \
for(int LINE_ID(__sync_flag_nargle_lock) = 1; LINE_ID(__sync_flag_nargle_lock); LINE_ID(__sync_flag_nargle_lock) = 0, lock_release(lock))  for(int LINE_ID(__sync_rc_nargle_lock) = lock_acquire(lock); LINE_ID(__sync_rc_nargle_lock) && LINE_ID(__sync_flag_nargle_lock) ; LINE_ID(__sync_flag_nargle_lock) = 0)

typedef volatile long int lock_t;

#define LOCK_INIT (0)

/* returns non-zero when lock acquired, zero when lock operation failed */
extern int lock_acquire_try(lock_t *lock);
extern int lock_acquire(lock_t *lock);
extern void lock_release(lock_t *lock);

/* socket functions */
typedef struct sock_t *sock_p;
extern int socket_create(sock_p *s);
extern int socket_connect_tcp(sock_p s, const char *host, int port);
extern int socket_read(sock_p s, uint8_t *buf, int size);
extern int socket_write(sock_p s, uint8_t *buf, int size);
extern int socket_close(sock_p s);
extern int socket_destroy(sock_p *s);

/* serial handling */
typedef struct serial_port_t *serial_port_p;
#define PLC_SERIAL_PORT_NULL ((plc_serial_port)NULL)
extern serial_port_p plc_lib_open_serial_port(const char *path, int baud_rate, int data_bits, int stop_bits, int parity_type);
extern int plc_lib_close_serial_port(serial_port_p serial_port);
extern int plc_lib_serial_port_read(serial_port_p serial_port, uint8_t *data, int size);
extern int plc_lib_serial_port_write(serial_port_p serial_port, uint8_t *data, int size);


/* time functions */
extern int sleep_ms(int ms);
extern int64_t time_ms(void);
extern struct tm *localtime_r(const time_t *timep, struct tm *result);

/* some functions can be simply replaced */
#define snprintf_platform sprintf_s


/*#ifdef __cplusplus
}
#endif
*/



#endif

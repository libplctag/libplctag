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
  * 2012-07-25  KRH - Updates for new thread API.						   *
  *                                                                        *
  * 2013-12-23	KRH - re-port to Windows via VS2012.                       *
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

#define START_PACK __pragma( pack(push, 1) )
#define END_PACK __pragma( pack(pop) )

/* VS C++ uses foo[] to denote a zero length array. */
#define ZLA_SIZE	0

/* export definitions. */

#define USE_STD_VARARG_MACROS 1

#define __PRETTY_FUNCTION__ __FUNCTION__

#define snprintf sprintf_s


/* memory functions/defs */
extern void *mem_alloc(int size);
extern void mem_free(const void *mem);
extern void mem_set(void *d1, int c, int size);
extern void mem_copy(void *d1, void *d2, int size);

/* string functions/defs */
extern int str_cmp(const char *first, const char *second);
extern int str_cmp_i(const char *first, const char *second);
extern int str_copy(char *dst, const char *src, int size);
extern int str_length(const char *str);
extern char *str_dup(const char *str);
extern int str_to_int(const char *str, int *val);
extern int str_to_float(const char *str, float *val);
extern char **str_split(const char *str, const char *sep);

/* mutex functions/defs */
typedef struct mutex_t *mutex_p;
extern int mutex_create(mutex_p *m);
extern int mutex_lock(mutex_p m);
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
extern int thread_join(thread_p t);
extern int thread_destroy(thread_p *t);

/* atomic operations */
typedef volatile long int lock_t;

#define LOCK_INIT (0)

/* returns non-zero when lock acquired, zero when lock operation failed */
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


/* endian functions */

extern uint16_t h2le16(uint16_t v);
extern uint16_t le2h16(uint16_t v);
extern uint16_t h2be16(uint16_t v);
extern uint16_t be2h16(uint16_t v);

extern uint32_t h2le32(uint32_t v);
extern uint32_t le2h32(uint32_t v);
extern uint32_t h2be32(uint32_t v);
extern uint32_t be2h32(uint32_t v);

/* misc functions */
extern int sleep_ms(int ms);
extern uint64_t time_ms(void);

extern void pdebug_impl(const char *func, int line_num, const char *templ, ...);
#if defined(USE_STD_VARARG_MACROS) || defined(_WIN32)
#define pdebug(d,f,...) \
   do { if(d) pdebug_impl(__PRETTY_FUNCTION__,__LINE__,f,__VA_ARGS__); } while(0)
#else
#define pdebug(d,f,a...) \
   do{ if(d) pdebug_impl(__PRETTY_FUNCTION__,__LINE__,f,##a ); } while(0)
#endif

extern void pdebug_dump_bytes_impl(uint8_t *data,int count);
#define pdebug_dump_bytes(dbg, d,c)  do { if(dbg) pdebug_dump_bytes_impl(d,c); } while(0)




/*#ifdef __cplusplus
}
#endif
*/



#endif

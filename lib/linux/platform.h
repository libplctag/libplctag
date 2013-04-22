/*
 * os.h
 *
 *  Created on: Nov 27, 2012
 *      Author: Kyle Hayes
 */

#ifndef OS_H_
#define OS_H_

#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <stdarg.h>

/* common definitions */
#define ZLA_SIZE	0
#define START_PACK
#define END_PACK __attribute__((__packed__))
#define ZLA_SIZE 0
#define USE_GNU_VARARG_MACROS 1



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
 *     synchronized(my_lock) {
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
#define synchronized(lock) \
for(int flag_##__LINE__ = 1; flag_##__LINE__ ; flag_##__LINE__ = 0, mutex_unlock(lock))  for(int rc_##__LINE__ = mutex_lock(lock); rc_##__LINE__ == PLCTAG_STATUS_OK && flag_##__LINE__ ; flag_##__LINE__ = 0)

/* thread functions/defs */
typedef struct thread_t *thread_p;
typedef void *(*thread_func_t)(void *arg);
extern int thread_create(thread_p *t, thread_func_t func, int stacksize, void *arg);
extern void thread_stop(void);
extern int thread_join(thread_p t);
extern int thread_destroy(thread_p *t);

/* atomic operations */
typedef int lock_t;

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
extern int64_t time_ms(void);

extern void pdebug_impl(const char *func, int line_num, const char *templ, ...);
#if defined(USE_STD_VARARG_MACROS) || defined(WIN32)
#define pdebug(f,...) \
   if(DEBUG) pdebug_impl(__PRETTY_FUNCTION__,__LINE__,f,__VA_ARGS__)
#else
#define pdebug(f,a...) \
   if(DEBUG) pdebug_impl(__PRETTY_FUNCTION__,__LINE__,f,##a )
#endif

extern void pdebug_dump_bytes_impl(uint8_t *data,int count);
#define pdebug_dump_bytes(d,c)  if(DEBUG) pdebug_dump_bytes_impl(d,c)



#endif /* _PLATFORM_H_ */

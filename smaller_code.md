# Ideas for Smaller Code

This document describes ideas that hopefully will lead to much smaller code for implementing industrial network protocols.

## Goals

- to be implemented using the smallest possible code.
- do the simplest possible thing that could work.
- remove app boilerplate.  Anything the app will always have to do we should take on here.
- hide as much implementation as possible to allow for changes without changing app code.
- use poll()/WSAPoll(), or select() on platforms that do not have poll().
- cross platform
  - Windows
  - macOS
  - Linux
  - BSD
  - eventually FreeRTOS
  - eventually NuttX
  - eventually Zephyr
- API should be safe to use.  Do not push safety checks onto the app where it will result in repetitive boilerplate.
- no more than 1000 sockets per event loop instance.

## Event Loop

An event loop runs its own thread and waits for events on sockets.  When the event or timeout occurs, it
wakes up the thread waiting on the event.

There is an event loop that runs in its own thread.  The API might look something like this:

```c
typedef struct ev_loop_s ev_loop_t;

typedef enum {
  EV_OK,
  EV_ENULL,
  EV_ENOMEM,
  EV_ETHREAD,
  EV_EPOLL,
  EV_ETIMEOUT,
  EV_ECLOSED,
  /* ... */
} ev_status_t;

/**
 * @brief Create an event loop with its own thread and set the basic poll time (or equivalent) to poll_timeout_ms.
 * 
 * Internally this will create a wake pipe/socket pair using socketpair() on POSIX and simulating that with TCP sockets on Windows.
 * 
 * @param poll_timeout_ms
 * @return ev_loop_t
 */
ev_loop_t *ev_loop_create(uint32_t poll_timeout_ms);

ev_status_t ev_loop_destroy(ev_loop_t *loop);

ev_status_t ev_loop_wait(ev_loop_t *loop); /* wait for event loop to stop */

/**
 * @brief Event types
 * 
 * Events are not errors or status.  They indicate what kind of I/O is available on a socket.
 * If there was an error with the socket, the return status will indicate the status.
 * 
 */
typedef enum {
  EV_EVENT_READ  = 0x01,
  EV_EVENT_WRITE = 0x02,
  EV_EVENT_WAKE  = 0x04,
} ev_loop_event_t;

/**
 * @brief waits for an event on a socket or until the timeout duration expires.
 * 
 * Internally this sets up the event loop to monitor the socket for the specified event,
 * and blocks on a condition variable until either the event occurs or the timeout duration expires.
 * 
 * The condition variable is thread local and if it is null, it is created for the thread when this
 * is called.
 * 
 * IF the socket is not already registered with the event loop, it is registered.
 * 
 * This wakes up the event loop thread and then it will rebuild the pollfds or equivalent.
 * 
 * Note: events are not status. There is not EV_EVENT_ERROR.  The app will be waiting for READ, WRITE or WAKE. If one of those operations fails, then the READ, WRITE or WAKE event
 * will be raised with a status that indicates what went wrong.
 * 
 * @param loop 
 * @param sock 
 * @param event 
 * @param timeout_duration_ms 
 * @return ev_status_t 
 */
ev_status_t ev_loop_wait_event(ev_loop_t *loop, socket_t sock, uint32_t event, uint32_t timeout_duration_ms);

/**
 * @brief Raise an event for a socket.
 * 
 * This function is used to wake up the event loop for a specific socket.
 * 
 * This wakes up the event thread to deliver this even to this socket.
 * 
 * Probably this is implemented by using the wake pipe/socket trick and sending a small
 * struct instead of just a byte.  The small struct would have the socket and the event.
 * 
 * @param loop 
 * @param sock 
 * @param event 
 * @return ev_status_t
 */
ev_status_t ev_loop_raise_event(ev_loop_t *loop, socket_t sock, uint32_t event);
```

## Threads/Coroutines

We want an API that makes it hard to see what is being created: threads or coroutines.  The app code should not be able to tell.
You must have an event loop before you can create an ev_thread_t.

```c

typedef struct ev_thread_s ev_thread_t;

typedef void (ev_loop_thread_fn)(void *ctx);

ev_thread_t ev_loop_create_thread(ev_loop_t *loop, ev_loop_thread_fn *func, void *ctx, uint32_t stack_size);
```

## Sockets

### Creating and closing sockets

```c
socket_t ev_loop_create_tcp_client(ev_loop_t *loop, const char *remote_host, uint16_t remote_port);
socket_t ev_loop_create_tcp_listener(ev_loop_t *loop, const char *bind_host, uint16_t bind_port, uint16_t backlog);
socket_t ev_loop_create_udp(ev_loop_t *loop, const char *bind_host, uint16_t bind_port, bool enable_broadcast);
ev_status_t ev_loop_close_socket(ev_loop_t *loop, socket_t sock);
```

Q: Should this be higher level?  
Q: Where do we set the amount of space for receiving and sending buffers?
Q: should we even have these?  There is not a lot of gain over standard BSD socket operations.
Q: if we use plain BSD socket functions, how to we ensure that the sockets are non-blocking,
nodelay, reuseaddr etc.?

### Socket data

```c
/* this calls the socket recv function and if it gets EAGAIN or WSAWOULDBLOCK, it waits calling ev_loop_wait_event() */
ev_status_t ev_loop_read_tcp(ev_loop_t *loop, socket_t sock, read_buffer_t *buf);

/* this calls the socket send/write function and if it gets EAGAIN or WSAWOULDBLOCK, it waits calling ev_loop_wait_event() */
/* maybe this is a macro that inserts NULL at the end of the arguments? */
/* see below for why multiple write buffers */
ev_status_t ev_loop_write_tcp(ev_loop_t *loop, socket_t sock, write_buffer_t *buf, ...);

/* this calls the socket recvfrom function and if it gets EAGAIN or WSAWOULDBLOCK, it waits calling ev_loop_wait_event() */
ev_status_t ev_loop_read_udp(ev_loop_t *loop, socket_t sock, const char **remote_addr, size_t remote_addr_size, uint16_t *remote_port, read_buffer_t *buf);

/* this calls the socket sendto function and if it gets EAGAIN or WSAWOULDBLOCK, it waits calling ev_loop_wait_event() */
/* maybe this is a macro that inserts NULL at the end of the arguments? */
/* multiple write buffers to handle the common case that there are multiple layers of the
protocol and you have to write them inside out because you need to know the inner layer's size before you can encode the next layer out. Passing multiple like this allows system with functions like writev to create iovecs and write with one system call. */
ev_status_t ev_loop_write_udp(ev_loop_t *loop, socket_t sock, const char *remote_addr, uint16_t remote_port, write_buffer_t *buf, ...);
```

## Buffers

What are buffers?  We want something that automatically checks bounds.  We want something that makes it very easy to call read_tcp and get more data added to the end of what is there.  

### Read buffers

- read consecutive data from the buffer up to some limit
- write data into a read buffer from a socket with easy appending to the end of the buffer.
- easy to consume data at the beginning of the buffer without external indexes tracking use.
- read functions consume data without external indexes tracking use.

One possibility:

```c
typedef struct ev_read_buf_s ev_ev_read_buf_t;

ev_read_buf_t *ev_read_buf_create(uint32_t data_len);

/* set everything back to zero */
ev_status_t ev_read_buf_reset(ev_read_buf_t *buf);

/* memmove the remaining data down to index zero compressing the consumed data out */
ev_status_t ev_read_buf_compact(ev_read_buf_t *buf);


typedef enum {
    EV_BUF_BE,
    EV_BUF_LE,
} ev_buf_endian_t;

bool ev_read_buf_decode_u8(ev_read_buf_t *buf);
bool ev_read_buf_decode_u16(ev_read_buf_t *buf, ev_buf_endian_t endian);
bool ev_read_buf_decode_u16(ev_read_buf_t *buf, ev_buf_endian_t endian);
bool ev_read_buf_decode_i16(ev_read_buf_t *buf, ev_buf_endian_t endian);
bool ev_read_buf_decode_i16(ev_read_buf_t *buf, ev_buf_endian_t endian);
/* ... read 32 and 64 bit integers and 32 and 64-bit floats */

bool ev_read_buf_decode_bytes(ev_read_buf_t *buf, uint8_t *dest, uint32_t dest_len);
```

### Write Buffers

```c

typedef struct ev_write_buf_s ev_write_buf_t;

ev_write_buf_t *ev_write_buf_create(uint32_t max_size);

ev_status_t ev_write_buf_reset(ev_write_buf_t *buf)

/* adds constraint so that sum of all buffer data_len is less than or equal to size_constraint. */
/* maybe macro that inserts NULL after args? */
ev_status_t ev_write_buf_constrain(uint32_t size_constraint, ev_write_buf_t *buf, ...)

bool ev_write_buf_encode_u8(ev_write_buf_t *buf, uint8_t val);
bool ev_write_buf_encode_u16(ev_write_buf_t *buf, uint16_t val, ev_buf_endian_t endian);
bool ev_write_buf_encode_i16(ev_write_buf_t *buf, int16_t val, ev_buf_endian_t endian);
/* .... */
bool ev_write_buf_encode_bytes(ev_write_buf_t *buf, uint8_t *data, uint32_t data_len);
```

### Buffer dumping/logging

We need logging functions to log buffers, both read and write.  See log.h for ideas.

## Errors/Status

All functions that can fail set status in a thread local variable that acts a lot like errno.

```c
extern _Thread ev_status_t ev_status;
```

App functions can simply check that if there is a concern about failure. 

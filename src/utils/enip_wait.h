/***************************************************************************
 * ENIP wait wrappers for restartable read/write/connect operations.
 ***************************************************************************/

#ifndef __LIBPLCTAG_ENIP_WAIT_H__
#    define __LIBPLCTAG_ENIP_WAIT_H__ 1

#    include <stddef.h>
#    include <stdint.h>

#    include <platform.h>
#    include <utils/bytes.h>

typedef enum {
    ENIP_WAIT_OP_NONE = 0,
    ENIP_WAIT_OP_READ = 1,
    ENIP_WAIT_OP_WRITE = 2,
    ENIP_WAIT_OP_CONNECT = 3,
    ENIP_WAIT_OP_CONNECT_START = 4,
} enip_wait_operation_kind_t;

typedef enum {
    ENIP_WAIT_STATUS_OK = 0,
    ENIP_WAIT_STATUS_TIMEOUT = 1,
    ENIP_WAIT_STATUS_WAKE = 2,
    ENIP_WAIT_STATUS_SOCKET_ERROR = 3,
    ENIP_WAIT_STATUS_REMOTE_CLOSED = 4,
    ENIP_WAIT_STATUS_TERMINATE = 5,
} enip_wait_status_t;

typedef struct {
    enip_wait_status_t status;
    enip_wait_operation_kind_t operation_kind;
    Bytes buffer;
    size_t bytes_done;
    int64_t deadline_ms;
    int32_t phase;
} socket_wait_state_t;

extern int socket_read_wait(sock_p s, Bytes *dst, int timeout_ms, socket_wait_state_t *io_state);
extern int socket_write_wait(sock_p s, const Bytes *src, int timeout_ms, socket_wait_state_t *io_state);
/* Start the TCP connect and block (up to timeout_ms) until it completes. */
extern int socket_connect_wait(sock_p s, const char *host, int port, int timeout_ms, socket_wait_state_t *io_state);

#endif

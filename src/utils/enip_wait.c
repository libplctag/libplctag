/***************************************************************************
 * ENIP wait wrappers that normalize blocking-like socket operations while
 * keeping restart state for WAKE interruptions.
 ***************************************************************************/

#include <limits.h>

#include <libplctag/lib/libplctag.h>
#include <utils/enip_wait.h>


static int64_t wait_deadline_ms(int timeout_ms, enip_wait_operation_kind_t op, socket_wait_state_t *io_state) {
    int64_t now = time_ms();

    if(io_state && io_state->operation_kind == op && io_state->deadline_ms > 0) { return io_state->deadline_ms; }

    if(timeout_ms <= 0) { return now; }

    return now + (int64_t)timeout_ms;
}


static int wait_remaining_ms(int64_t deadline_ms) {
    int64_t now = time_ms();

    if(now >= deadline_ms) { return 0; }

    int64_t remaining = deadline_ms - now;
    if(remaining > INT_MAX) { return INT_MAX; }

    return (int)remaining;
}


static void wait_state_set(socket_wait_state_t *io_state, enip_wait_operation_kind_t op, enip_wait_status_t status, Bytes buffer,
                           size_t bytes_done, int64_t deadline_ms, int32_t phase) {
    if(!io_state) { return; }

    io_state->operation_kind = op;
    io_state->status = status;
    io_state->buffer = buffer;
    io_state->bytes_done = bytes_done;
    io_state->deadline_ms = deadline_ms;
    io_state->phase = phase;
}


static int classify_wait_events(int events, int wake_code, int timeout_code, int disconnect_code, int error_code,
                                enip_wait_operation_kind_t op, Bytes buffer, size_t bytes_done, int64_t deadline_ms,
                                int32_t phase, socket_wait_state_t *io_state) {
    if(events & SOCK_EVENT_WAKE_UP) {
        wait_state_set(io_state, op, ENIP_WAIT_STATUS_WAKE, buffer, bytes_done, deadline_ms, phase);
        return wake_code;
    }

    if(events & SOCK_EVENT_TIMEOUT) {
        wait_state_set(io_state, op, ENIP_WAIT_STATUS_TIMEOUT, buffer, bytes_done, deadline_ms, phase);
        return timeout_code;
    }

    if(events & SOCK_EVENT_DISCONNECT) {
        wait_state_set(io_state, op, ENIP_WAIT_STATUS_REMOTE_CLOSED, buffer, bytes_done, deadline_ms, phase);
        return disconnect_code;
    }

    if(events & SOCK_EVENT_ERROR) {
        wait_state_set(io_state, op, ENIP_WAIT_STATUS_SOCKET_ERROR, buffer, bytes_done, deadline_ms, phase);
        return error_code;
    }

    return PLCTAG_STATUS_OK;
}


int socket_read_wait(sock_p s, Bytes *dst, int timeout_ms, socket_wait_state_t *io_state) {
    size_t bytes_done = 0;
    int64_t deadline_ms;

    if(!s || !dst || !dst->data) { return PLCTAG_ERR_NULL_PTR; }

    if(timeout_ms < 0) { return PLCTAG_ERR_BAD_PARAM; }

    if(io_state && io_state->operation_kind == ENIP_WAIT_OP_READ && io_state->buffer.data == dst->data
       && io_state->buffer.len == dst->len) {
        bytes_done = io_state->bytes_done;
    }

    deadline_ms = wait_deadline_ms(timeout_ms, ENIP_WAIT_OP_READ, io_state);

    while(bytes_done < dst->len) {
        size_t remaining = dst->len - bytes_done;
        int chunk = (remaining > (size_t)INT_MAX) ? INT_MAX : (int)remaining;
        int rc = socket_read(s, dst->data + bytes_done, chunk, 0);

        if(rc > 0) {
            bytes_done += (size_t)rc;
            continue;
        }

        if(rc < 0 && rc != PLCTAG_ERR_TIMEOUT) {
            wait_state_set(io_state, ENIP_WAIT_OP_READ, ENIP_WAIT_STATUS_SOCKET_ERROR, *dst, bytes_done, deadline_ms, 1);
            return rc;
        }

        {
            int events;
            int wait_rc;
            int classify_rc;

            if(wait_remaining_ms(deadline_ms) <= 0) {
                wait_state_set(io_state, ENIP_WAIT_OP_READ, ENIP_WAIT_STATUS_TIMEOUT, *dst, bytes_done, deadline_ms, 1);
                return PLCTAG_ERR_TIMEOUT;
            }

            wait_rc = socket_wait_event(s, SOCK_EVENT_CAN_READ | SOCK_EVENT_DEFAULT_MASK, wait_remaining_ms(deadline_ms));
            if(wait_rc < 0) {
                wait_state_set(io_state, ENIP_WAIT_OP_READ, ENIP_WAIT_STATUS_SOCKET_ERROR, *dst, bytes_done, deadline_ms, 1);
                return wait_rc;
            }

            events = wait_rc;
            classify_rc =
                classify_wait_events(events, PLCTAG_STATUS_PENDING, PLCTAG_ERR_TIMEOUT, PLCTAG_ERR_REMOTE_ERR,
                                     PLCTAG_ERR_BAD_CONNECTION, ENIP_WAIT_OP_READ, *dst, bytes_done, deadline_ms, 1, io_state);
            if(classify_rc != PLCTAG_STATUS_OK) { return classify_rc; }
        }
    }

    wait_state_set(io_state, ENIP_WAIT_OP_READ, ENIP_WAIT_STATUS_OK, *dst, bytes_done, deadline_ms, 1);
    return PLCTAG_STATUS_OK;
}


int socket_write_wait(sock_p s, const Bytes *src, int timeout_ms, socket_wait_state_t *io_state) {
    size_t bytes_done = 0;
    int64_t deadline_ms;

    if(!s || !src || !src->data) { return PLCTAG_ERR_NULL_PTR; }

    if(timeout_ms < 0) { return PLCTAG_ERR_BAD_PARAM; }

    if(io_state && io_state->operation_kind == ENIP_WAIT_OP_WRITE && io_state->buffer.data == src->data
       && io_state->buffer.len == src->len) {
        bytes_done = io_state->bytes_done;
    }

    deadline_ms = wait_deadline_ms(timeout_ms, ENIP_WAIT_OP_WRITE, io_state);

    while(bytes_done < src->len) {
        size_t remaining = src->len - bytes_done;
        int chunk = (remaining > (size_t)INT_MAX) ? INT_MAX : (int)remaining;
        int rc = socket_write(s, src->data + bytes_done, chunk, 0);

        if(rc > 0) {
            bytes_done += (size_t)rc;
            continue;
        }

        if(rc < 0 && rc != PLCTAG_ERR_TIMEOUT) {
            wait_state_set(io_state, ENIP_WAIT_OP_WRITE, ENIP_WAIT_STATUS_SOCKET_ERROR, *src, bytes_done, deadline_ms, 1);
            return rc;
        }

        {
            int events;
            int wait_rc;
            int classify_rc;

            if(wait_remaining_ms(deadline_ms) <= 0) {
                wait_state_set(io_state, ENIP_WAIT_OP_WRITE, ENIP_WAIT_STATUS_TIMEOUT, *src, bytes_done, deadline_ms, 1);
                return PLCTAG_ERR_TIMEOUT;
            }

            wait_rc = socket_wait_event(s, SOCK_EVENT_CAN_WRITE | SOCK_EVENT_DEFAULT_MASK, wait_remaining_ms(deadline_ms));
            if(wait_rc < 0) {
                wait_state_set(io_state, ENIP_WAIT_OP_WRITE, ENIP_WAIT_STATUS_SOCKET_ERROR, *src, bytes_done, deadline_ms, 1);
                return wait_rc;
            }

            events = wait_rc;
            classify_rc =
                classify_wait_events(events, PLCTAG_STATUS_PENDING, PLCTAG_ERR_TIMEOUT, PLCTAG_ERR_REMOTE_ERR,
                                     PLCTAG_ERR_BAD_CONNECTION, ENIP_WAIT_OP_WRITE, *src, bytes_done, deadline_ms, 1, io_state);
            if(classify_rc != PLCTAG_STATUS_OK) { return classify_rc; }
        }
    }

    wait_state_set(io_state, ENIP_WAIT_OP_WRITE, ENIP_WAIT_STATUS_OK, *src, bytes_done, deadline_ms, 1);
    return PLCTAG_STATUS_OK;
}


int socket_connect_wait(sock_p s, int timeout_ms, socket_wait_state_t *io_state) {
    int64_t deadline_ms;

    if(!s) { return PLCTAG_ERR_NULL_PTR; }

    if(timeout_ms < 0) { return PLCTAG_ERR_BAD_PARAM; }

    deadline_ms = wait_deadline_ms(timeout_ms, ENIP_WAIT_OP_CONNECT, io_state);

    for(;;) {
        int rc = socket_connect_tcp_check(s, 0);
        if(rc == PLCTAG_STATUS_OK) {
            wait_state_set(io_state, ENIP_WAIT_OP_CONNECT, ENIP_WAIT_STATUS_OK, (Bytes){0}, 0, deadline_ms, 1);
            return PLCTAG_STATUS_OK;
        }

        if(rc != PLCTAG_ERR_TIMEOUT) {
            wait_state_set(io_state, ENIP_WAIT_OP_CONNECT, ENIP_WAIT_STATUS_SOCKET_ERROR, (Bytes){0}, 0, deadline_ms, 1);
            return rc;
        }

        if(wait_remaining_ms(deadline_ms) <= 0) {
            wait_state_set(io_state, ENIP_WAIT_OP_CONNECT, ENIP_WAIT_STATUS_TIMEOUT, (Bytes){0}, 0, deadline_ms, 1);
            return PLCTAG_ERR_TIMEOUT;
        }

        {
            int events;
            int wait_rc = socket_wait_event(s, SOCK_EVENT_CONNECT | SOCK_EVENT_DEFAULT_MASK, wait_remaining_ms(deadline_ms));
            if(wait_rc < 0) {
                wait_state_set(io_state, ENIP_WAIT_OP_CONNECT, ENIP_WAIT_STATUS_SOCKET_ERROR, (Bytes){0}, 0, deadline_ms, 1);
                return wait_rc;
            }

            events = wait_rc;
            rc = classify_wait_events(events, PLCTAG_STATUS_PENDING, PLCTAG_ERR_TIMEOUT, PLCTAG_ERR_REMOTE_ERR,
                                      PLCTAG_ERR_BAD_CONNECTION, ENIP_WAIT_OP_CONNECT, (Bytes){0}, 0, deadline_ms, 1, io_state);
            if(rc != PLCTAG_STATUS_OK) { return rc; }
        }
    }
}


int socket_connect_tcp_start_wait(sock_p s, const char *host, int port, int timeout_ms, socket_wait_state_t *io_state) {
    int rc;

    if(!s || !host) { return PLCTAG_ERR_NULL_PTR; }

    if(timeout_ms < 0) { return PLCTAG_ERR_BAD_PARAM; }

    wait_state_set(io_state, ENIP_WAIT_OP_CONNECT_START, ENIP_WAIT_STATUS_OK, (Bytes){0}, 0, 0, 1);

    rc = socket_connect_tcp_start(s, host, port);
    if(rc == PLCTAG_STATUS_OK) {
        wait_state_set(io_state, ENIP_WAIT_OP_CONNECT_START, ENIP_WAIT_STATUS_OK, (Bytes){0}, 0, 0, 2);
        return PLCTAG_STATUS_OK;
    }

    if(rc != PLCTAG_STATUS_PENDING) {
        wait_state_set(io_state, ENIP_WAIT_OP_CONNECT_START, ENIP_WAIT_STATUS_SOCKET_ERROR, (Bytes){0}, 0, 0, 2);
        return rc;
    }

    wait_state_set(io_state, ENIP_WAIT_OP_CONNECT_START, ENIP_WAIT_STATUS_OK, (Bytes){0}, 0, 0, 3);
    return socket_connect_wait(s, timeout_ms, io_state);
}

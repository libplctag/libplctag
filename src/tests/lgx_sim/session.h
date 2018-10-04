
#pragma once

//#include "buffer.h"


#define BUFFER_LEN (4096)


typedef struct {
    int sock;

    uint64_t sender_context;
    uint32_t session_handle;
    uint32_t connection_id;
    uint32_t connection_id_targ;
    uint16_t connection_seq;

    uint16_t max_packet_size;

    uint8_t buf[BUFFER_LEN];
    uint16_t buf_len;
} session_context;



extern void *session_handler(void *session_arg);

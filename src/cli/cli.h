#pragma once

#include <platform.h>

/* cli operations */
#define CLI_INVALID_OPERATION      (-1)
#define CLI_READ_OPERATION          (0)
#define CLI_WATCH_OPERATION         (1)
#define CLI_WRITE_OPERATION         (2)

struct request_s {
    int operation;
    char *ip;
} Request = {-1, NULL};

typedef struct request_s request_t;

void parse_args(int argc, char *argv[], request_t *request);
#pragma once

#include "./uthash.h"

/* cli requirements */
#define LIBPLCTAG_REQUIRED_VERSION 2,1,4

/* tag paths */
#define TAG_PATH                  "protocol=%s&gateway=%s&path=%s&plc=%s&debug=%d&name=%s"
#define TAG_PATH_AUTO_READ_SYNC   "protocol=%s&gateway=%s&path=%s&plc=%s&debug=%d&auto_sync_read_ms=%d&name=%s"
#define DATA_TIMEOUT              5000

/* cli operations */
typedef enum {
    READ,
    WRITE,
    WATCH
} cli_operation_t;

/* cli request definition */
typedef struct {
    const char *protocol;
    const char *ip;
    const char *path;
    const char *plc;
    cli_operation_t operation;
    int interval;
    int debug_level;
} cli_request_t;

/* data types */
typedef enum {
    UINT64, INT64,
    UINT32, INT32,
    UINT16, INT16,
    UINT8, INT8, 
    FLOAT64, FLOAT32, 
    BOOL 
} data_type_t;

/* tag definition */
typedef struct {
    int id;
    data_type_t type;
    const char *name;
    union {
        uint64_t UINT64_val;
        int64_t INT64_val;
        uint32_t UINT32_val;
        int32_t INT32_val;
        uint16_t UINT16_val;
        int16_t INT16_val;
        uint8_t UINT8_val;
        int8_t INT8_val;
        double FLOAT64_val;
        float FLOAT32_val;
        bool BOOL_val;
    } val, last_val, write_val;
} tag_t;

/* tags hash definition */
struct tags {
    int tag_handle;
    tag_t tag;
    UT_hash_handle hh;
};

void usage(void);
int parse_args(int argc, char *argv[]);
void print_request();
int process_tags();
int is_comment(const char *line);
void trim_line(char *line);
char **split_string(const char *str, const char *sep);
int process_line(const char *line, tag_t *tag);
void add_tag(int32_t tag_handle, tag_t tag);
int check_tags(void);
int get_tag(int32_t tag_handle, tag_t tag, int offset);
int read_tags(void);
int write_tags(void);
int watch_tags(void);
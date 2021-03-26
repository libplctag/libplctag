#pragma once

#include "./uthash.h"

/* cli requirements */
#define LIBPLCTAG_REQUIRED_VERSION 2,1,4

/* tag paths */
#define TAG_PATH                  "protocol=%s&gateway=%s&path=%s&plc=%s&debug=%d&name=%s%s"
#define TAG_PATH_AUTO_READ_SYNC   "protocol=%s&gateway=%s&path=%s&plc=%s&debug=%d&auto_sync_read_ms=%d&name=%s%s"
#define DATA_TIMEOUT              5000

/* bool utils */
#define btoa(x) ( (x) ? "true" : "false" )

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
    const char *attributes;
    bool offline;
} cli_request_t;

/* data types */
typedef enum {
    t_UINT64, t_INT64,
    t_UINT32, t_INT32,
    t_UINT16, t_INT16,
    t_UINT8, t_INT8, 
    t_FLOAT64, t_FLOAT32, 
    t_BOOL 
} data_type_t;

/* tag definition */
typedef struct {
    const char *key;
    data_type_t type;
    const char *path;
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
    int bit;
    int offset;
    bool watch;
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
int validate_line(char **parts);
void add_tag(int32_t tag_handle, tag_t tag);
int check_tags(void);
int get_tag(int32_t tag_handle, tag_t *tag);
int read_tags(void);
int set_tag(int32_t tag_handle, tag_t *tag);
int verify_write_tags(void);
int write_tags(void);
int watch_tags(void);
void tag_callback(int32_t tag_handle, int event, int status);
int destroy_tags(void);
int do_offline(void);

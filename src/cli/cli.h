#pragma once

#include <platform.h>

/* cli requirements */
#define LIBPLCTAG_REQUIRED_VERSION 2,1,4

/* cli operations */
#define CLI_INVALID_OPERATION      (-1)
#define CLI_READ_OPERATION          (0)
#define CLI_WATCH_OPERATION         (1)
#define CLI_WRITE_OPERATION         (2)

/* tag sync type */
#define READ_SYNC               (0)
#define WRITE_SYNC              (1)

/* platform specifics */
#define AB_TAG_PATH             "protocol=%s&gateway=%s&path=%s&plc=%s&debug=%s&name=%s"
#define AB_TAG_PATH_READ_SYNC   "protocol=%s&gateway=%s&path=%s&plc=%s&debug=%s&auto_sync_read_ms=%s&name=%s"
#define AB_TAG_PATH_WRITE_SYNC  "protocol=%s&gateway=%s&path=%s&plc=%s&debug=%s&auto_sync_write_ms=%s&name=%s"
#define DATA_TIMEOUT            0

typedef struct request_s {
    int operation;
    char *ip;
    char *interval;
} request_t; 

typedef struct platform_s {
    char *protocol;
    char *gateway;
    char *path;
    char *plc;
    char *debug;
    char *sync_interval;
} platform_t;

typedef struct tag_s {
    char *id;
    char *type;
    char *path;
    bool watch;
    int32_t handle;
    char *last_value;
    char *write_value;
} tag_t;

typedef struct tag_array_s {
    tag_t *tag_array;
    size_t size;
} tag_array_t;

void init_tag_array(tag_array_t *tag_array, size_t size);
void add_tag_to_array(tag_array_t *tag_array, tag_t tag);
void free_tag_array(tag_array_t *tag_array);

void parse_args(int argc, char *argv[], request_t *request);
int get_stdin_l(char *line);
void read_tags(platform_t *platform,  bool watch);
void write_tags(platform_t *platform);
void create_tag(platform_t *platform, tag_t tag, int sync_type);
void tag_read_callback(int32_t tag_handle, int event, int status);
void do_tag_get(int32_t tag_handle);
void process_tag_get(int32_t tag_handlde, char *tag_id, void *val, char *type_modifier);
void tag_write_callback(int32_t tag_handle, int event, int status);
void do_tag_set(int32_t tag_handle);
void destroy_tags(tag_array_t *tag_array);
void destroy_tag(int32_t tag_handle);

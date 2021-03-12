#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h> 
#include "../lib/libplctag.h"
#include "./cli.h"

tag_array_t tags = {NULL};

void parse_args(int argc, char *argv[], request_t *request)
{
    if (argc != 3) {
        printf("ERROR: invalid number of arguments!\n");
        return;
    }

    char *operation = argv[1];
    char *args = argv[2];

    if (!strcmp(operation, "--read")) {
        request->operation = CLI_READ_OPERATION;
    } else if (!strcmp(operation, "--watch")) {
        request->operation = CLI_WATCH_OPERATION;
    } else if (!strcmp(operation, "--write")) {
        request->operation = CLI_WRITE_OPERATION;
    } else {
        printf("cli operation: INVALID.\n");
        return;
    }

    strtok(args, "=");
    request->ip = strtok(NULL, "=");

    return;    
}

int get_stdin_l(char *line)
{
    char *line_l = NULL;
    size_t len = 0;
    ssize_t line_len;
    int quit = 0;

    line_len = getline(&line_l, &len, stdin);

    strcpy(line, line_l);

    if (!strcmp(line, "quit\n")) {
        quit = 1;
    }

    free(line_l);
    return quit;
}

void read_tags(platform_t *platform, bool watch)
{
    char *line;
    int quit;
    tag_t tag = {NULL};
    
    line = (char *) malloc(1024);
    tag.id = (char *) malloc(100);
    tag.type = (char *) malloc(100);
    tag.path = (char *) malloc(100);

    do {
        quit = get_stdin_l(line);
        
        if (quit != 1) {
            if (line[0] != '#') {
                strcpy(tag.id, strtok(line, ","));
                strcpy(tag.type, strtok(NULL, ","));
                strcpy(tag.path, strtok(strtok(NULL, ","), "\n"));
                tag.watch = watch;
                tag.last_value = NULL;
                tag.write_value = NULL;
                
                if (!tag.id || !tag.type || !tag.path) {
                    printf("ERROR: invalid input.\n");
                } else {
                    create_tag(platform, tag, READ_SYNC);
                }
            }
        }
    } while (quit != 1);

    destroy_tags(&tags);
    plc_tag_shutdown();
    free(line);
    printf("DONE.\n");
    return;
}

void write_tags(platform_t *platform)
{
    char *line;
    int quit;
    tag_t tag = {NULL};
    
    line = (char *) malloc(1024);
    tag.id = (char *) malloc(100);
    tag.type = (char *) malloc(100);
    tag.write_value = (char *) malloc(100);
    tag.path = (char *) malloc(100);

    do {
        quit = get_stdin_l(line);
        
        if (quit != 1) {
            if (line[0] != '#') {
                strcpy(tag.id, strtok(line, ","));
                strcpy(tag.type, strtok(NULL, ","));
                strcpy(tag.write_value, strtok(NULL, ","));
                strcpy(tag.path, strtok(strtok(NULL, ","), "\n"));
                tag.watch = false;
                tag.last_value = NULL;
                
                if (!tag.id || !tag.type || !tag.path || !tag.write_value) {
                    printf("ERROR: invalid input.\n");
                } else {
                    create_tag(platform, tag, WRITE_SYNC);
                }
            }
        }
    } while (quit != 1);

    destroy_tags(&tags);
    plc_tag_shutdown();
    free(line);
    printf("DONE.\n");
    return;
}

void create_tag(platform_t *platform, tag_t tag, int sync_type) {
    char *tag_path = (char *) malloc(1024);
    int rc;
    int tag_path_size;

    switch (sync_type) {
        case READ_SYNC:
            tag_path_size = sprintf(tag_path, AB_TAG_PATH_READ_SYNC, platform->protocol, platform->gateway, 
                platform->path, platform->plc, platform->debug, platform->sync_interval, tag.path);
            break;
        case WRITE_SYNC:
            tag_path_size = sprintf(tag_path, AB_TAG_PATH_WRITE_SYNC, platform->protocol, platform->gateway, 
                platform->path, platform->plc, platform->debug, "50", tag.path);
            break;
        default:
            printf("ERROR: Invalid tag sync_type.\n");
            return;
    }
    
    tag_path = realloc(tag_path, tag_path_size);

    printf("%s\n", tag_path);
    tag.handle = plc_tag_create(tag_path, DATA_TIMEOUT);
    if(tag.handle < 0) {
        printf("ERROR %s: Could not create tag!\n", plc_tag_decode_error(tag.handle));
        free(tag_path);
        return;
    }

    add_tag_to_array(&tags, tag);

    switch (sync_type) {
        case READ_SYNC:
            rc = plc_tag_register_callback(tag.handle, tag_read_callback);
            break;
        case WRITE_SYNC:
            rc = plc_tag_register_callback(tag.handle, tag_write_callback);
            break;
        default:
            printf("ERROR: Invalid tag sync_type.\n");
            return;
    }

    if(rc != PLCTAG_STATUS_OK) {
        plc_tag_destroy(tag.handle);
        free(tag_path);
        return;
    }

    free(tag_path);
    return;
}

void tag_read_callback(int32_t tag_handle, int event, int status) {
    switch(event) {
        case PLCTAG_EVENT_ABORTED:
            printf("tag(%i): Tag operation was aborted!\n", tag_handle);
            break;
        case PLCTAG_EVENT_DESTROYED:
            printf( "tag(%i): Tag was destroyed.\n", tag_handle);
            break;
        case PLCTAG_EVENT_READ_COMPLETED:
            if(status == PLCTAG_STATUS_OK) {
                do_tag_get(tag_handle);
            }
            if (!tags.tag_array[tag_handle].watch) {
                destroy_tag(tag_handle);
            }
            printf("tag(%i): Tag read operation completed with status %s.\n", tag_handle, plc_tag_decode_error(status));
            break;
        case PLCTAG_EVENT_READ_STARTED:
            printf("tag(%i): Tag read operation started.\n", tag_handle);
            break;    
        default:
            printf("tag(%i): Unexpected event %d!\n", tag_handle, event);
            break;
    }
}

void do_tag_get(int32_t tag_handle) {
    char *tag_type = tags.tag_array[tag_handle].type;
    char *tag_id = tags.tag_array[tag_handle].id;
    char *val_str;
    int val_str_length;

    printf("tag.id=%s\ntag.type=%s\n", tag_id, tag_type);
    printf("tag.path=%s\n", tags.tag_array[tag_handle].path);
    printf("tag.watch=%d\n", tags.tag_array[tag_handle].watch);
    printf("tag.handle=%d\n", tags.tag_array[tag_handle].handle);
    printf("tag.last_value=%s\n", tags.tag_array[tag_handle].last_value);
    printf("tag.write_value=%s\n", tags.tag_array[tag_handle].write_value);

    if (!strcmp(tag_type, "uint64")) {
        uint64_t val = plc_tag_get_uint64(tag_handle, 0);
        // check if process_tag_get works here!
        val_str_length = snprintf(NULL, 0, "%" PRIu64, val);
        val_str = (char *) malloc(val_str_length + 1);
        if (tags.tag_array[tag_handle].last_value == NULL) {
            tags.tag_array[tag_handle].last_value = (char *) malloc(val_str_length + 1);
            snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "%" PRIu64, val);
            printf("{\"%s\"=%" PRIu64"}\n", tag_id, val);    
        } else {
            snprintf(val_str, val_str_length + 1, "%" PRIu64, val);
            if (strcmp(tags.tag_array[tag_handle].last_value, val_str)) {
                snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "%" PRIu64, val);
                printf("{\"%s\"=%" PRIu64"}\n", tag_id, val); 
            }
        } 
    } else if (!strcmp(tag_type, "int64")) {
        int64_t val = plc_tag_get_int64(tag_handle, 0);
        val_str_length = snprintf(NULL, 0, "%" PRIi64, val);
        val_str = (char *) malloc(val_str_length + 1);
        if (tags.tag_array[tag_handle].last_value == NULL) {
            tags.tag_array[tag_handle].last_value = (char *) malloc(val_str_length + 1);
            snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "%" PRIi64, val);
            printf("{\"%s\"=%" PRIi64"}\n", tag_id, val);   
        } else {
            snprintf(val_str, val_str_length + 1, "%" PRIi64, val);
            if (strcmp(tags.tag_array[tag_handle].last_value, val_str)) {
                snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "%" PRIi64, val);
                printf("{\"%s\"=%" PRIi64"}\n", tag_id, val); 
            }
        }
    } else if (!strcmp(tag_type, "uint32")) {
        uint32_t val = plc_tag_get_uint32(tag_handle, 0);
        val_str_length = snprintf(NULL, 0, "%" PRIu32, val);
        val_str = (char *) malloc(val_str_length + 1);
        if (tags.tag_array[tag_handle].last_value == NULL) {
            tags.tag_array[tag_handle].last_value = (char *) malloc(val_str_length + 1);
            snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "%" PRIu32, val);
            printf("{\"%s\"=%" PRIu32"}\n", tag_id, val);   
        } else {
            snprintf(val_str, val_str_length + 1, "%" PRIu32, val);
            if (strcmp(tags.tag_array[tag_handle].last_value, val_str)) {
                snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "%" PRIu32, val);
                printf("{\"%s\"=%" PRIu32"}\n", tag_id, val);
            }
        }
    } else if (!strcmp(tag_type, "int32")) {
        int32_t val = plc_tag_get_int32(tag_handle, 0);
        val_str_length = snprintf(NULL, 0, "%" PRIi32, val);
        val_str = (char *) malloc(val_str_length + 1);
        if (tags.tag_array[tag_handle].last_value == NULL) {
            tags.tag_array[tag_handle].last_value = (char *) malloc(val_str_length + 1);
            snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "%" PRIi32, val);
            printf("{\"%s\"=%" PRIi32"}\n", tag_id, val);   
        } else {
            snprintf(val_str, val_str_length + 1, "%" PRIi32, val);
            if (strcmp(tags.tag_array[tag_handle].last_value, val_str)) {
                snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "%" PRIi32, val);
                printf("{\"%s\"=%" PRIi32"}\n", tag_id, val);
            }
        }
    } else if (!strcmp(tag_type, "uint16")) {
        uint16_t val = plc_tag_get_uint16(tag_handle, 0);
        val_str_length = snprintf(NULL, 0, "%" PRIu16, val);
        val_str = (char *) malloc(val_str_length + 1);
        if (tags.tag_array[tag_handle].last_value == NULL) {
            tags.tag_array[tag_handle].last_value = (char *) malloc(val_str_length + 1);
            snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "%" PRIu16, val);
            printf("{\"%s\"=%" PRIu16"}\n", tag_id, val);  
        } else {
            snprintf(val_str, val_str_length + 1, "%" PRIu16, val);
            if (strcmp(tags.tag_array[tag_handle].last_value, val_str)) {
                snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "%" PRIu16, val);
                printf("{\"%s\"=%" PRIu16"}\n", tag_id, val); 
            }
        }
    } else if (!strcmp(tag_type, "int16")) {
        int16_t val = plc_tag_get_int16(tag_handle, 0);
        val_str_length = snprintf(NULL, 0, "%" PRIi16, val);
        val_str = (char *) malloc(val_str_length + 1);
        if (tags.tag_array[tag_handle].last_value == NULL) {
            tags.tag_array[tag_handle].last_value = (char *) malloc(val_str_length + 1);
            snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "%" PRIi16, val);
            printf("{\"%s\"=%" PRIi16"}\n", tag_id, val); 
        } else {
            snprintf(val_str, val_str_length + 1, "%" PRIi16, val);
            if (strcmp(tags.tag_array[tag_handle].last_value, val_str)) {
                snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "%" PRIi16, val);
                printf("{\"%s\"=%" PRIi16"}\n", tag_id, val);
            }
        }
    } else if (!strcmp(tag_type, "uint8")) {
        uint8_t val = plc_tag_get_uint8(tag_handle, 0);
        val_str_length = snprintf(NULL, 0, "%" PRIu8, val);
        val_str = (char *) malloc(val_str_length + 1);
        if (tags.tag_array[tag_handle].last_value == NULL) {
            tags.tag_array[tag_handle].last_value = (char *) malloc(val_str_length + 1);
            snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "%" PRIu8, val);
            printf("{\"%s\"=%" PRIu8"}\n", tag_id, val);
        } else {
            snprintf(val_str, val_str_length + 1, "%" PRIu8, val);
            if (strcmp(tags.tag_array[tag_handle].last_value, val_str)) {
                snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "%" PRIu8, val);
                printf("{\"%s\"=%" PRIu8"}\n", tag_id, val);
            }
        }
    } else if (!strcmp(tag_type, "int8")) {
        int8_t val = plc_tag_get_int8(tag_handle, 0);
        val_str_length = snprintf(NULL, 0, "%" PRIi8, val);
        val_str = (char *) malloc(val_str_length + 1);
        if (tags.tag_array[tag_handle].last_value == NULL) {
            tags.tag_array[tag_handle].last_value = (char *) malloc(val_str_length + 1);
            snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "%" PRIi8, val);
            printf("{\"%s\"=%" PRIi8"}\n", tag_id, val);
        } else {
            snprintf(val_str, val_str_length + 1, "%" PRIi8, val);
            if (strcmp(tags.tag_array[tag_handle].last_value, val_str)) {
                snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "%" PRIi8, val);
                printf("{\"%s\"=%" PRIi8"}\n", tag_id, val);
            }
        }
    } else if (!strcmp(tag_type, "float64")) {
        double val = plc_tag_get_float64(tag_handle, 0);
        val_str_length = snprintf(NULL, 0, "%lf", val);
        val_str = (char *) malloc(val_str_length + 1);
        if (tags.tag_array[tag_handle].last_value == NULL) {
            tags.tag_array[tag_handle].last_value = (char *) malloc(val_str_length + 1);
            snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "%lf", val);
            printf("{\"%s\"=%lf}\n", tag_id, val);
        } else {
            snprintf(val_str, val_str_length + 1, "%lf", val);
            if (strcmp(tags.tag_array[tag_handle].last_value, val_str)) {
                snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "%lf", val);
                printf("{\"%s\"=%lf}\n", tag_id, val);
            }
        }
    } else if (!strcmp(tag_type, "float32")) {
        float val = plc_tag_get_float32(tag_handle, 0);
        val_str_length = snprintf(NULL, 0, "%f", val);
        val_str = (char *) malloc(val_str_length + 1);
        if (tags.tag_array[tag_handle].last_value == NULL) {
            tags.tag_array[tag_handle].last_value = (char *) malloc(val_str_length + 1);
            snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "%f", val);
            printf("{\"%s\"=%f}\n", tag_id, val);
        } else {
            snprintf(val_str, val_str_length + 1, "%f", val);
            if (strcmp(tags.tag_array[tag_handle].last_value, val_str)) {
                snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "%f", val);
                printf("{\"%s\"=%f}\n", tag_id, val);
            }
        }
    } else if (!strcmp(tag_type, "bool")) {
        int val = plc_tag_get_uint8(tag_handle, 0);
        if (val) {
            val_str_length = snprintf(NULL, 0, "true");
            val_str = (char *) malloc(val_str_length + 1);
            if (tags.tag_array[tag_handle].last_value == NULL) {
                tags.tag_array[tag_handle].last_value = (char *) malloc(val_str_length + 1);
                snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "true");
                printf("{\"%s\"=true}\n", tag_id);
            } else {
                snprintf(val_str, val_str_length + 1, "true");
                if (strcmp(tags.tag_array[tag_handle].last_value, val_str)) {
                    snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "true");
                    printf("{\"%s\"=true}\n", tag_id);
                }
            }
        } else {
            val_str_length = snprintf(NULL, 0, "false");
            val_str = (char *) malloc(val_str_length + 1);
            if (tags.tag_array[tag_handle].last_value == NULL) {
                tags.tag_array[tag_handle].last_value = (char *) malloc(val_str_length + 1);
                snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "false");
                printf("{\"%s\"=false}\n", tag_id);
            } else {
                snprintf(val_str, val_str_length + 1, "false");
                if (strcmp(tags.tag_array[tag_handle].last_value, val_str)) {
                    snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, "false");
                    printf("{\"%s\"=false}\n", tag_id);
                }
            }
        }
    } else {
        printf("ERROR: Invalid tag type referenced!\n");
        return;
    }

    free(val_str);
}

// this is experimental!
void process_tag_get(int32_t tag_handle, char *tag_id, void *val, char *type_modifier) {
    char *val_str;
    int val_str_length;
    char type_str[100];

    strcpy(type_str, "{\"%s\"=");
    strcat(type_str, type_modifier);
    strcat(type_str, "}\n");

    val_str_length = snprintf(NULL, 0, type_modifier, val);
    val_str = (char *) malloc(val_str_length + 1);
    if (tags.tag_array[tag_handle].last_value == NULL) {
        tags.tag_array[tag_handle].last_value = (char *) malloc(val_str_length + 1);
        snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, type_modifier, val);
        printf(type_str, tag_id, val);    
    } else {
        snprintf(val_str, val_str_length + 1, type_modifier, val);
        if (strcmp(tags.tag_array[tag_handle].last_value, val_str)) {
            snprintf(tags.tag_array[tag_handle].last_value, val_str_length + 1, type_modifier, val);
            printf(type_str, tag_id, val); 
        }
    }

    free(val_str); 
}

void tag_write_callback(int32_t tag_handle, int event, int status) {
    switch(event) {
        case PLCTAG_EVENT_ABORTED:
            printf("tag(%i): Tag operation was aborted!\n", tag_handle);
            break;
        case PLCTAG_EVENT_DESTROYED:
            printf( "tag(%i): Tag was destroyed.\n", tag_handle);
            break;
        case PLCTAG_EVENT_READ_COMPLETED:
            if(status == PLCTAG_STATUS_OK) {
                do_tag_set(tag_handle);
            }
            printf("tag(%i): Tag read operation completed with status %s.\n", tag_handle, plc_tag_decode_error(status));
            break;
        case PLCTAG_EVENT_READ_STARTED:
            printf("tag(%i): Tag read operation started.\n", tag_handle);
            break;
        case PLCTAG_EVENT_WRITE_COMPLETED:
            if(status == PLCTAG_STATUS_OK) {
                do_tag_get(tag_handle);
                destroy_tag(tag_handle);
                printf("tag(%i): Tag write operation completed.\n", tag_handle);
            } else {
                printf("tag(%i): Tag write operation failed with status %s!\n", tag_handle, plc_tag_decode_error(status));
            }
            break;
        case PLCTAG_EVENT_WRITE_STARTED:
            printf("tag(%i): Tag write operation started with status %s.\n", tag_handle, plc_tag_decode_error(status));
            break;    
        default:
            printf("tag(%i): Unexpected event %d!\n", tag_handle, event);
            break;
    }
}

void do_tag_set(int32_t tag_handle) {
    char *tag_type = tags.tag_array[tag_handle].type;
    char *tag_id = tags.tag_array[tag_handle].id;

    if (!strcmp(tag_type, "uint64")) {
        uint64_t val;
        sscanf(tags.tag_array[tag_handle].write_value, "%" SCNu64, &val);        
        plc_tag_set_uint64(tag_handle, 0, val);
    } else if (!strcmp(tag_type, "int64")) {
        int64_t val;
        sscanf(tags.tag_array[tag_handle].write_value, "%" SCNi64, &val);        
        plc_tag_set_int64(tag_handle, 0, val);
    } else if (!strcmp(tag_type, "uint32")) {
        uint32_t val;
        sscanf(tags.tag_array[tag_handle].write_value, "%" SCNu32, &val);        
        plc_tag_set_uint32(tag_handle, 0, val);
    } else if (!strcmp(tag_type, "int32")) {
        int32_t val;
        sscanf(tags.tag_array[tag_handle].write_value, "%" SCNi32, &val);        
        plc_tag_set_int32(tag_handle, 0, val);
    } else if (!strcmp(tag_type, "uint16")) {
        uint16_t val;
        sscanf(tags.tag_array[tag_handle].write_value, "%" SCNu16, &val);        
        plc_tag_set_uint16(tag_handle, 0, val);
    } else if (!strcmp(tag_type, "int16")) {
        int16_t val;
        sscanf(tags.tag_array[tag_handle].write_value, "%" SCNi16, &val);        
        plc_tag_set_int16(tag_handle, 0, val);
    } else if (!strcmp(tag_type, "uint8")) {
        uint8_t val;
        sscanf(tags.tag_array[tag_handle].write_value, "%" SCNu8, &val);        
        plc_tag_set_uint8(tag_handle, 0, val);
    } else if (!strcmp(tag_type, "int8")) {
        int8_t val;
        sscanf(tags.tag_array[tag_handle].write_value, "%" SCNi8, &val);        
        plc_tag_set_int8(tag_handle, 0, val);
    } else if (!strcmp(tag_type, "float64")) {
        double val;
        sscanf(tags.tag_array[tag_handle].write_value, "%lf", &val);        
        plc_tag_set_float64(tag_handle, 0, val);
    } else if (!strcmp(tag_type, "float32")) {
        float val;
        sscanf(tags.tag_array[tag_handle].write_value, "%f", &val);        
        plc_tag_set_float32(tag_handle, 0, val);
    } else if (!strcmp(tag_type, "bool")) {
        int val;
        if (!strcmp(tags.tag_array[tag_handle].write_value, "true")) {
            val = 1;
        } else {
            val = 0;
        }
        plc_tag_set_uint8(tag_handle, 0, val);
    } else {
        printf("ERROR: Invalid tag type referenced!\n");
        return;
    }
}

void destroy_tags(tag_array_t *tag_array) {
    int i;
    int32_t tag_handle;

    for (i=0; i < tag_array->size; i++) {
        tag_handle = tag_array->tag_array[i].handle;
        if (tag_handle != -1) {
            destroy_tag(tag_handle);
        }
    }
}

void destroy_tag(int32_t tag_handle) {
    plc_tag_abort(tag_handle);
    plc_tag_unregister_callback(tag_handle);
    plc_tag_destroy(tag_handle);
    tags.tag_array[tag_handle].handle = -1;
}

void init_tag_array(tag_array_t *tag_array, size_t size) {
    int i;

    tag_array->tag_array = malloc(size * sizeof(tag_t));
    tag_array->size = size;

    for (i=0; i < tag_array->size; i++) {
        tag_array->tag_array[i].id = NULL;
        tag_array->tag_array[i].type = NULL;
        tag_array->tag_array[i].path = NULL;
        tag_array->tag_array[i].watch = false;
        tag_array->tag_array[i].handle = -1;
        tag_array->tag_array[i].last_value = NULL;
        tag_array->tag_array[i].write_value = NULL;
    }
}

void add_tag_to_array(tag_array_t *tag_array, tag_t tag) {
    size_t prev_size = tag_array->size;
    size_t new_size;
    size_t i;

    if (tag.handle > tag_array->size - 1) {
        new_size = tag.handle + 1;
        
        tag_array->tag_array = realloc(tag_array->tag_array, new_size * sizeof(tag_t));
        tag_array->size = new_size;

        for (i=prev_size; i < tag_array->size; i++) {
            tag_array->tag_array[i].id = NULL;
            tag_array->tag_array[i].type = NULL;
            tag_array->tag_array[i].path = NULL;
            tag_array->tag_array[i].watch = false;
            tag_array->tag_array[i].handle = -1;
            tag_array->tag_array[i].last_value = NULL;
            tag_array->tag_array[i].write_value = NULL;
        }
    }

    tag_array->tag_array[tag.handle] = tag;
}

void free_tag_array(tag_array_t *tag_array) {
    free(tag_array->tag_array);
    tag_array->tag_array = NULL;
    tag_array->size = 0;
}

int main(int argc, char *argv[])
{
    request_t request = {-1, NULL};
    parse_args(argc, argv, &request);

    if (request.operation == CLI_INVALID_OPERATION) {
        printf("ERROR: operation invalid!\n");
        exit(-1);
    }

    if (request.ip == NULL) {
        printf("ERROR: ip invalid!\n");
        exit(-1);
    }

    platform_t platform = {NULL};
    platform.protocol = "ab_eip";
    platform.gateway = request.ip;
    platform.path = "1,0";
    platform.plc = "controllogix";
    platform.debug = "2";
    platform.sync_interval = "5000";

    if(plc_tag_check_lib_version(LIBPLCTAG_REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("ERROR: Required compatible library version %d.%d.%d not available!\n", LIBPLCTAG_REQUIRED_VERSION);
        exit(-1);
    }

    init_tag_array(&tags, 100);

    switch (request.operation) {
        case CLI_READ_OPERATION:
            read_tags(&platform, false);
            break;
        case CLI_WATCH_OPERATION:
            read_tags(&platform, true);
            break;
        case CLI_WRITE_OPERATION:
            write_tags(&platform);
            break;
        default:
            break;
    }

    free_tag_array(&tags);

    return 0;
}

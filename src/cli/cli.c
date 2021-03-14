#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <inttypes.h>
#include "../lib/libplctag.h"
#include "./cli.h"

/* globals here */
struct tags *tags = NULL;
cli_request_t cli_request = {
    "ab_eip",
    "127.0.0.1",
    "1,0",
    "controllogix",
    READ,
    500,
    2
};

void usage(void) 
{
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "\tLIBPLCTAG CLI.\n");
}

int parse_args(int argc, char *argv[]) 
{
    if (argc < 2) {
        fprintf(stderr, "ERROR: invalid number of arguments.\n");
        return -1;
    }

    int i = 0;

    char *operation = argv[++i];
    if (!strcasecmp(operation, "--read")) {
        cli_request.operation = READ;
    } else if (!strcasecmp(operation, "--write")) {
        cli_request.operation = WRITE;
    } else if (!strcasecmp(operation, "--watch")) {
         cli_request.operation = WATCH;
    } else {
        fprintf(stderr, "ERROR: invalid PLC operation.\n");
        fprintf(stderr, "INFO: Use one of --read, --write, --watch.\n");
        return -1;
    }       
 
    char *param;
    char *val;
    while (++i < argc) {
        param = strtok(argv[i], "="); 
        val = strtok(NULL, "=");

        if (!strcasecmp(param, "-protocol")) {
            cli_request.protocol = val;
        } else if (!strcasecmp(param, "-ip")) {
            cli_request.ip = val;
        } else if (!strcasecmp(param, "-path")) {
            cli_request.path = val; 
        } else if (!strcasecmp(param, "-plc")) {
            cli_request.plc = val;
        } else if (!strcasecmp(param, "-debug")) {
            sscanf(val, "%d", &cli_request.debug_level);
        } else if (!strcasecmp(param, "-interval")) {
            sscanf(val, "%d", &cli_request.interval);
        } else {
            fprintf(stderr, "ERROR: invalid PLC parameter: %s.\n", param);
            fprintf(stderr, "INFO: Supported params -ip, -debug, -interval.\n");
            return -1;
        } 
    }

    return 0;
}

void print_request() 
{
    fprintf(stdout, "Running with params:\n");
    fprintf(stdout, "\tProtocol: %s\n", cli_request.protocol);
    fprintf(stdout, "\tIP: %s\n", cli_request.ip);
    fprintf(stdout, "\tPath: %s\n", cli_request.path);
    fprintf(stdout, "\tPLC: %s\n", cli_request.plc);
    switch (cli_request.operation) {
    case READ:
        fprintf(stdout, "\tOperation: READ.\n");
        break;
    case WRITE:
        fprintf(stdout, "\tOperation: WRITE.\n");
        break;
    case WATCH:
        fprintf(stdout, "\tOperation: WATCH.\n");
        break;
    default:
        fprintf(stdout, "\tOperation: INVALID.\n");
        break;
    }
    fprintf(stdout, "\tInterval: %d\n", cli_request.interval);
    fprintf(stdout, "\tDebug Level: %d\n", cli_request.debug_level);
}

int is_comment(const char *line)
{
    int i = 0;

    /* scan past the first whitespace */
    for(i=0; line[i] && isspace(line[i]); i++);

    return (line[i] == '#') ? 1 : 0;
}

void trim_line(char *line)
{
    int len = 0;

    if(!line || strlen(line) == 0) {
        return;
    }

    len = (int)strlen(line);

    while(len>0 && line[len - 1] == '\n') {
        line[len - 1] = 0;
        len--;
    }
}

char **split_string(const char *str, const char *sep)
{
    int sub_str_count=0;
    int size = 0;
    const char *sub;
    const char *tmp;
    char **res = NULL;

    /* first, count the sub strings */
    tmp = str;
    sub = strstr(tmp,sep);

    while(sub && *sub) {
        /* separator could be at the front, ignore that. */
        if(sub != tmp) {
            sub_str_count++;
        }

        tmp = sub + strlen(sep);
        sub = strstr(tmp,sep);
    }

    if(tmp && *tmp && (!sub || !*sub)) {
        sub_str_count++;
    }

    /* calculate total size for string plus pointers */
    size = (int)(sizeof(char *)*((size_t)sub_str_count+1))+(int)strlen(str)+1;

    /* allocate enough memory */
    res = malloc((size_t)size);
    if(!res) {
        return NULL;
    }

    /* calculate the beginning of the string */
    tmp = (char *)res + sizeof(char *) * (size_t)(sub_str_count+1);

    /* copy the string into the new buffer past the first part with the array of char pointers. */
    strncpy((char *)tmp, str, (size_t)(size - ((char*)tmp - (char*)res)));

    /* set up the pointers */
    sub_str_count=0;
    sub = strstr(tmp,sep);

    while(sub && *sub) {
        /* separator could be at the front, ignore that. */
        if(sub != tmp) {
            /* store the pointer */
            res[sub_str_count] = (char *)tmp;

            sub_str_count++;
        }

        /* zero out the separator chars */
        memset((char*)sub, 0, strlen(sep));

        /* point past the separator (now zero) */
        tmp = sub + strlen(sep);

        /* find the next separator */
        sub = strstr(tmp,sep);
    }

    /* if there is a chunk at the end, store it. */
    if(tmp && *tmp && (!sub || !*sub)) {
        res[sub_str_count] = (char*)tmp;
    }

    return res;
}

int process_line(const char *line, tag_t *tag) 
{
    char **parts = NULL;

    parts = split_string(line, ",");
    if(!parts) {
        fprintf(stderr,"Splitting string failed for string %s!", line);
        return -1;
    }

    /* make sure we got enough pieces. */
    int required = 3;
    if (cli_request.operation == WRITE) required = 4;
    for(int i=0; i < required; i++) {
        if(parts[i] == NULL) {
            fprintf(stderr, "Line does not contain enough parts. Line: %s\n", line);
            return -1;
        }
    }

    /* setup all the associated tag values here. */
    tag->id = atoi(parts[0]);

    if(!strcasecmp("uint64", parts[1])) {
        tag->type = UINT64;
        if (cli_request.operation == WRITE) {
            sscanf(parts[required-2], "%" SCNu64, &tag->write_val.UINT64_val);
        }
    } else if(!strcasecmp("int64", parts[1])) {
        tag->type = INT64;
        if (cli_request.operation == WRITE) {
            sscanf(parts[required-2], "%" SCNi64, &tag->write_val.INT64_val); 
        }
    } else if(!strcasecmp("uint32", parts[1])) {
        tag->type = UINT32;
        if (cli_request.operation == WRITE) {
            sscanf(parts[required-2], "%" SCNu32, &tag->write_val.UINT32_val);
        }
    } else if(!strcasecmp("int32", parts[1])) {
        tag->type = INT32;
        if (cli_request.operation == WRITE) {
            sscanf(parts[required-2], "%" SCNi32, &tag->write_val.INT32_val);
        }
    } else if(!strcasecmp("uint16", parts[1])) {
        tag->type = UINT16;
        if (cli_request.operation == WRITE) {
            sscanf(parts[required-2], "%" SCNu16, &tag->write_val.UINT16_val);
        }
    } else if(!strcasecmp("int16", parts[1])) {
        tag->type = INT16;
        if (cli_request.operation == WRITE) {
            sscanf(parts[required-2], "%" SCNi16, &tag->write_val.INT16_val); 
        }
    } else if(!strcasecmp("uint8", parts[1])) {
        tag->type = UINT8;
        if (cli_request.operation == WRITE) {
            sscanf(parts[required-2], "%" SCNu8, &tag->write_val.UINT8_val);
        }
    } else if(!strcasecmp("int8", parts[1])) {
        tag->type = INT8;
        if (cli_request.operation == WRITE) {
            sscanf(parts[required-2], "%" SCNi8, &tag->write_val.INT8_val); 
        }
    } else if(!strcasecmp("float64", parts[1])) {
        tag->type = FLOAT64;
        if (cli_request.operation == WRITE) {
            sscanf(parts[required-2], "%lf", &tag->write_val.FLOAT64_val); 
        }
    } else if(!strcasecmp("float32", parts[1])) {
        tag->type = FLOAT32;
        if (cli_request.operation == WRITE) {
            sscanf(parts[required-2], "%f", &tag->write_val.FLOAT32_val);
        }
    } else if(!strcasecmp("bool", parts[1])) {
        tag->type = BOOL;
        if (cli_request.operation == WRITE) {
            if (!strcasecmp("true", parts[required-2])) {
                tag->write_val.BOOL_val = true;
            } else {
                tag->write_val.BOOL_val = false;
            }
        }
    } else {
        fprintf(stderr, "Unknown data type for %s!\n", parts[1]);
        return -1;
    }

    tag->name = strdup(parts[required - 1]);

    free(parts);

    return 0;
}

void add_tag(int tag_handle, tag_t tag)
{
    struct tags *t;

    t = malloc(sizeof(struct tags));
    t->tag_handle = tag_handle;
    t->tag = tag;
    HASH_ADD_INT(tags, tag_handle, t);
}

int check_tags()
{
    int rc = PLCTAG_STATUS_OK;
    struct tags *t;

    for(t = tags; t != NULL; t = t->hh.next) {
        rc = plc_tag_status(t->tag_handle);

        if(rc != PLCTAG_STATUS_OK) {
            return rc;
        }
    }

    return rc;
}

int process_tags()
{
    char *line = NULL;
    size_t line_len = 0;
    char *tag_path = (char *) malloc(1024);

    while (getline(&line, &line_len, stdin) > 0) {
        if (is_comment(line)) {
            continue;
        }

        fprintf(stdout, "%s", line);
        tag_t tag;
        trim_line(line);
        /* ignore lines that can't be processed */
        if (process_line(line, &tag) == -1) {
            continue;
        }

        switch (cli_request.operation) {
        case WATCH:
            sprintf(tag_path, TAG_PATH_AUTO_READ_SYNC, cli_request.protocol, 
                cli_request.ip, cli_request.path, cli_request.plc, 
                cli_request.debug_level, cli_request.interval, tag.name);
            break;   
        default:
            sprintf(tag_path, TAG_PATH, cli_request.protocol, 
                cli_request.ip, cli_request.path, cli_request.plc, 
                cli_request.debug_level, tag.name);
            break;
        }

        fprintf(stdout, "%s\n", tag_path);
        int tag_handle = plc_tag_create(tag_path, 0);
        if (tag_handle < 0) {
            fprintf(stderr, "Error, %s, creating tag %s with string %s!\n", plc_tag_decode_error(tag_handle), tag.name, tag_path);
            free(tag_path);
            free(line);
            return -1;
        }

        add_tag(tag_handle, tag);
    }

    free(tag_path);
    free(line);
    fprintf(stdout, "DONE processing tags.\n");
    return 0;
}

int get_tag(int32_t tag_handle, tag_t tag, int offset) {
    switch (tag.type) {
    case UINT64:
        tag.val.UINT64_val = plc_tag_get_uint64(tag_handle, offset);
        tag.last_val.UINT64_val = tag.val.UINT64_val; 
        fprintf(stdout, "{\"%d\"=%" PRIu64"}\n", tag.id, tag.val.UINT64_val);
        break;
    case INT64:
        tag.val.INT64_val = plc_tag_get_int64(tag_handle, offset);
        tag.last_val.INT64_val = tag.val.INT64_val;
        fprintf(stdout, "{\"%d\"=%" PRIi64"}\n", tag.id, tag.val.INT64_val);
        break;
    case UINT32:
        tag.val.UINT32_val = plc_tag_get_uint32(tag_handle, offset);
        tag.last_val.UINT32_val = tag.val.UINT32_val;
        fprintf(stdout, "{\"%d\"=%" PRIu32"}\n", tag.id, tag.val.UINT32_val);
        break;
    case INT32:
        tag.val.INT32_val = plc_tag_get_int32(tag_handle, offset);
        tag.last_val.INT32_val = tag.val.INT32_val;
        fprintf(stdout, "{\"%d\"=%" PRIi32"}\n", tag.id, tag.val.INT32_val);
        break;
    case UINT16:
        tag.val.UINT16_val = plc_tag_get_uint16(tag_handle, offset);
        tag.last_val.UINT16_val = tag.val.UINT16_val;
        fprintf(stdout, "{\"%d\"=%" PRIu16"}\n", tag.id, tag.val.UINT16_val);
        break;
    case INT16:
        tag.val.INT16_val = plc_tag_get_int16(tag_handle, offset);
        tag.last_val.INT16_val = tag.val.INT16_val;
        fprintf(stdout, "{\"%d\"=%" PRIi16"}\n", tag.id, tag.val.INT16_val);
        break;
    case UINT8:
        tag.val.UINT8_val = plc_tag_get_uint8(tag_handle, offset);
        tag.last_val.UINT8_val = tag.val.UINT8_val;
        fprintf(stdout, "{\"%d\"=%" PRIu8"}\n", tag.id, tag.val.UINT8_val);
        break;
    case INT8:
        tag.val.INT8_val = plc_tag_get_int8(tag_handle, offset);
        tag.last_val.INT8_val = tag.val.INT8_val;
        fprintf(stdout, "{\"%d\"=%" PRIi8"}\n", tag.id, tag.val.INT8_val);
        break;
    case FLOAT64:
        tag.val.FLOAT64_val = plc_tag_get_float64(tag_handle, offset);
        tag.last_val.FLOAT64_val = tag.val.FLOAT64_val;
        fprintf(stdout, "{\"%d\"=%lf}\n", tag.id, tag.val.FLOAT64_val);
        break;
    case FLOAT32:
        tag.val.FLOAT32_val = plc_tag_get_float32(tag_handle, offset);
        tag.last_val.FLOAT32_val = tag.val.FLOAT32_val;
        fprintf(stdout, "{\"%d\"=%f}\n", tag.id, tag.val.FLOAT32_val);
        break;
    case BOOL:
        tag.val.BOOL_val = plc_tag_get_uint8(tag_handle, offset);
        tag.last_val.BOOL_val = tag.val.BOOL_val;
        if (tag.val.BOOL_val) {
            fprintf(stdout, "{\"%d\"=true}\n", tag.id);
        } else {
            fprintf(stdout, "{\"%d\"=false}\n", tag.id);
        }
        break;
    default:
        break;
    }

    return PLCTAG_STATUS_OK;
}

int read_tags(void) {
    int rc = PLCTAG_STATUS_OK;
    struct tags *t;

    for(t = tags; t != NULL; t = t->hh.next) {
        rc = plc_tag_read(t->tag_handle, 0);
        if(rc != PLCTAG_STATUS_PENDING) {
            fprintf(stderr,"Unable to read tag %s!\n", plc_tag_decode_error(rc));
            return rc;
        }
    }

    /* wait for all tags to be ready */
    while(check_tags() == PLCTAG_STATUS_PENDING){}

    for(t = tags; t != NULL; t = t->hh.next) {
        rc = get_tag(t->tag_handle, t->tag, 0);
        if(rc != PLCTAG_STATUS_OK) {
            fprintf(stderr,"Unable to parse value of tag %s!\n", plc_tag_decode_error(rc));
            return rc;
        }
    }

    return 0;
}

int write_tags(void) {

    return 0;
}

int watch_tags(void) {

    return 0;
}

int main(int argc, char *argv[])
{
    if (parse_args(argc, argv) == -1) {
        fprintf(stderr, "ERROR: invalid arguments.\n");
        usage();
        return -1;
    }

    print_request();

    if(plc_tag_check_lib_version(LIBPLCTAG_REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "ERROR: Required compatible library version %d.%d.%d not available!\n", LIBPLCTAG_REQUIRED_VERSION);
        return -1;
    }

    if (process_tags() != PLCTAG_STATUS_OK) {
        fprintf(stderr, "ERROR: Could not process tags.\n");
        return -1;
    }

    /* wait for all tags to be ready */
    while(check_tags() == PLCTAG_STATUS_PENDING){}

    switch (cli_request.operation) {
    case READ:
        if (read_tags() != PLCTAG_STATUS_OK) {
            fprintf(stderr, "ERROR: Tag read failed.\n");
            return -1;
        }
        break;
    case WRITE:
        if (write_tags() != PLCTAG_STATUS_OK) {
            fprintf(stderr, "ERROR: Tag write failed.\n");
            return -1;
        }
        break;
    case WATCH:
        watch_tags();
        break;
    default:
        break;
    }

    fprintf(stdout, "DONE.\n");

    return 0;
}
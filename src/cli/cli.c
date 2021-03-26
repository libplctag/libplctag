#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <inttypes.h>
#include "../lib/libplctag.h"
#include "./cli.h"
#include "./getline.h"
#include "../examples/utils.h"

/* globals here */
struct tags *tags = NULL;
cli_request_t cli_request = {
    "ab_eip",
    "127.0.0.1",
    "1,0",
    "controllogix",
    READ,
    500,
    2,
    "",
    false // offline
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
    if (!strcmp(operation, "--read")) {
        cli_request.operation = READ;
    } else if (!strcmp(operation, "--write")) {
        cli_request.operation = WRITE;
    } else if (!strcmp(operation, "--watch")) {
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
        val = strtok(NULL, "");

        if (!strcmp(param, "-protocol")) {
            cli_request.protocol = val;
        } else if (!strcmp(param, "-ip")) {
            cli_request.ip = val;
        } else if (!strcmp(param, "-path")) {
            cli_request.path = val; 
        } else if (!strcmp(param, "-plc")) {
            cli_request.plc = val;
        } else if (!strcmp(param, "-debug")) {
            sscanf(val, "%d", &cli_request.debug_level);
        } else if (!strcmp(param, "-interval")) {
            sscanf(val, "%d", &cli_request.interval);
        } else if (!strcmp(param, "-attributes")) {
            cli_request.attributes = val;
        } else if (!strcmp(param, "-offline")) {
            if (!strcmp(val, "true")) {
                cli_request.offline = true;
            } else if (!strcmp(val, "false")) {
                cli_request.offline = false;
            } else {
                fprintf(stderr, "ERROR: invalid parameter value for offline.");
                fprintf(stderr, "INFO: Supported values 'true' or 'false'.");
            }
        } else {
            fprintf(stderr, "ERROR: invalid PLC parameter: %s.\n", param);
            fprintf(stderr, "INFO: Supported params -protocol, -ip, -path, -plc, -debug, -interval, -attributes, -offline.\n");
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
    fprintf(stdout, "\tAdditional Attributes: %s\n", cli_request.attributes);
    fprintf(stdout, "\tOffline: %s\n", btoa(cli_request.offline));
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

int validate_line(char **parts) {
    char req_params[4][10] = 
    {
        "key",
        "type",
        "path",
        "value"
    };

    int required = 3;
    if (cli_request.operation == WRITE) required = 4;
    
    bool found = false;
    int j;
    char *part;
    for(int i=0; i < required; i++) {
        j = 0;
        while(parts[j] != NULL) {
            part = strdup(parts[j]);
            if (!strcmp(req_params[i], strtok(part, "="))) {
                found = true;
                break;
            }
            ++j;
        }
        if (!found) {
            fprintf(stderr, "Line missing: %s\n", req_params[i]);
            return -1;
        }
        found = false;
    }

    free(part);
    return 0;
}

void print_tag(tag_t *tag) {
    fprintf(stdout, "Tag created:\n");
    fprintf(stdout, "\tKey: %s\n", tag->key);
    switch (tag->type) {
    case t_UINT64:
        fprintf(stdout, "\tType: uint64.\n");
        break;
    case t_INT64:
        fprintf(stdout, "\tType: int64.\n");
        break;
    case t_UINT32:
        fprintf(stdout, "\tType: uint32.\n");
        break;
    case t_INT32:
        fprintf(stdout, "\tType: int32.\n");
        break;
    case t_UINT16:
        fprintf(stdout, "\tType: uint16.\n");
        break;
    case t_INT16:
        fprintf(stdout, "\tType: int16.\n");
        break;
    case t_UINT8:
        fprintf(stdout, "\tType: uint8.\n");
        break;
    case t_INT8:
        fprintf(stdout, "\tType: int8.\n");
        break;
    case t_FLOAT64:
        fprintf(stdout, "\tType: float64.\n");
        break;
    case t_FLOAT32:
        fprintf(stdout, "\tType: float32.\n");
        break;
    case t_BOOL:
        fprintf(stdout, "\tType: bool.\n");
        break;
    default:
        fprintf(stdout, "\tType: INVALID.\n");
        break;
    }
    fprintf(stdout, "\tPath: %s\n", tag->path);
    fprintf(stdout, "\tBit: %d\n", tag->bit);
    fprintf(stdout, "\tOffset: %d\n", tag->offset);
    fprintf(stdout, "\tWatch: %s\n", btoa(tag->watch));
}

int process_line(const char *line, tag_t *tag) 
{
    char **parts = NULL;

    parts = split_string(line, ",");
    if(!parts) {
        fprintf(stderr,"Splitting string failed for string %s!", line);
        return -1;
    }

    /* check if the relevant parameters are there or not */
    if (validate_line(parts) != 0) {
        fprintf(stderr, "Line does not contain enough parts. Line: %s\n", line);
        return -1;
    }

    fprintf(stdout, "Line validated!\n");

    /* setup all the associated tag values here. */

    /* set defaults here first */
    tag->bit = -1;
    tag->offset = 0;
    tag->watch = false;

    /* loop through all the val pairs now */
    char *type;
    char *write_val;

    int i = 0;
    char *param;
    char *val;
    while (parts[i] != NULL) {
        param = strtok(parts[i], "=");
        val = strtok(NULL, "");

        if(!strcmp("key", param)) {
            tag->key = strdup(val);
        } else if (!strcmp("type", param)) {
            type = val;
        } else if (!strcmp("value", param)) {
            write_val = val;
        } else if (!strcmp("bit", param)) {
            sscanf(val, "%d", &tag->bit);
        } else if (!strcmp("offset", param)) {
            sscanf(val, "%d", &tag->offset);
        } else if (!strcmp("path", param)) {
            tag->path = strdup(val);
        } else {
            fprintf(stderr, "Unknown param %s!\n", param);
            return -1;
        }

        ++i;
    }

    if(!strcmp("uint64", type)) {
        tag->type = t_UINT64;
        if (cli_request.operation == WRITE) {
            sscanf(write_val, "%" SCNu64, &tag->write_val.UINT64_val);
        }
    } else if(!strcmp("int64", type)) {
        tag->type = t_INT64;
        if (cli_request.operation == WRITE) {
            sscanf(write_val, "%" SCNi64, &tag->write_val.INT64_val); 
        }
    } else if(!strcmp("uint32", type)) {
        tag->type = t_UINT32;
        if (cli_request.operation == WRITE) {
            sscanf(write_val, "%" SCNu32, &tag->write_val.UINT32_val);
        }
    } else if(!strcmp("int32", type)) {
        tag->type = t_INT32;
        if (cli_request.operation == WRITE) {
            sscanf(write_val, "%" SCNi32, &tag->write_val.INT32_val);
        }
    } else if(!strcmp("uint16", type)) {
        tag->type = t_UINT16;
        if (cli_request.operation == WRITE) {
            sscanf(write_val, "%" SCNu16, &tag->write_val.UINT16_val);
        }
    } else if(!strcmp("int16", type)) {
        tag->type = t_INT16;
        if (cli_request.operation == WRITE) {
            sscanf(write_val, "%" SCNi16, &tag->write_val.INT16_val); 
        }
    } else if(!strcmp("uint8", type)) {
        tag->type = t_UINT8;
        if (cli_request.operation == WRITE) {
            sscanf(write_val, "%" SCNu8, &tag->write_val.UINT8_val);
        }
    } else if(!strcmp("int8", type)) {
        tag->type = t_INT8;
        if (cli_request.operation == WRITE) {
            sscanf(write_val, "%" SCNi8, &tag->write_val.INT8_val); 
        }
    } else if(!strcmp("float64", type)) {
        tag->type = t_FLOAT64;
        if (cli_request.operation == WRITE) {
            sscanf(write_val, "%lf", &tag->write_val.FLOAT64_val); 
        }
    } else if(!strcmp("float32", type)) {
        tag->type = t_FLOAT32;
        if (cli_request.operation == WRITE) {
            sscanf(write_val, "%f", &tag->write_val.FLOAT32_val);
        }
    } else if(!strcmp("bool", type)) {
        tag->type = t_BOOL;
        if (cli_request.operation == WRITE) {
            if (!strcmp("true", write_val)) {
                tag->write_val.BOOL_val = true;
            } else {
                tag->write_val.BOOL_val = false;
            }
        }
    } else {
        fprintf(stderr, "Unknown data type for %s!\n", type);
        return -1;
    }

    free(parts);
    print_tag(tag);
    fprintf(stdout, "Line processed!\n");

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

        if (!strcmp(line, "break\n")) {
            break;
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
                cli_request.debug_level, cli_request.interval, tag.path, 
                cli_request.attributes);
            break;   
        default:
            sprintf(tag_path, TAG_PATH, cli_request.protocol, 
                cli_request.ip, cli_request.path, cli_request.plc, 
                cli_request.debug_level, tag.path, cli_request.attributes);
            break;
        }

        fprintf(stdout, "%s\n", tag_path);
        int tag_handle = plc_tag_create(tag_path, 0);
        if (tag_handle < 0) {
            fprintf(stderr, "Error, %s, creating tag %s with string %s!\n", plc_tag_decode_error(tag_handle), tag.key, tag_path);
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

int get_tag(int32_t tag_handle, tag_t *tag) {
    switch (tag->type) {
    case t_UINT64:
        tag->val.UINT64_val = plc_tag_get_uint64(tag_handle, tag->offset);
        if (!tag->watch) {
            tag->last_val.UINT64_val = tag->val.UINT64_val; 
            fprintf(stdout, "{\"%s\":%" PRIu64"}\n", tag->key, tag->val.UINT64_val);
            break;
        }
        if (tag->val.UINT64_val != tag->last_val.UINT64_val) {
            tag->last_val.UINT64_val = tag->val.UINT64_val; 
            fprintf(stdout, "{\"%s\":%" PRIu64"}\n", tag->key, tag->val.UINT64_val);
        }
        break;
    case t_INT64:
        tag->val.INT64_val = plc_tag_get_int64(tag_handle, tag->offset);
        if (!tag->watch) {
            tag->last_val.INT64_val = tag->val.INT64_val;
            fprintf(stdout, "{\"%s\":%" PRIi64"}\n", tag->key, tag->val.INT64_val);
            break;
        }
        if (tag->val.INT64_val != tag->last_val.INT64_val) {
            tag->last_val.INT64_val = tag->val.INT64_val; 
            fprintf(stdout, "{\"%s\":%" PRIi64"}\n", tag->key, tag->val.INT64_val);
        }
        break;
    case t_UINT32:
        tag->val.UINT32_val = plc_tag_get_uint32(tag_handle, tag->offset);
        if (!tag->watch) {
            tag->last_val.UINT32_val = tag->val.UINT32_val;
            fprintf(stdout, "{\"%s\":%" PRIu32"}\n", tag->key, tag->val.UINT32_val);
            break;
        }
        if (tag->val.UINT32_val != tag->last_val.UINT32_val) {
            tag->last_val.UINT32_val = tag->val.UINT32_val; 
            fprintf(stdout, "{\"%s\":%" PRIu32"}\n", tag->key, tag->val.UINT32_val);
        }
        break;
    case t_INT32:
        tag->val.INT32_val = plc_tag_get_int32(tag_handle, tag->offset);
        if (!tag->watch) {
            tag->last_val.INT32_val = tag->val.INT32_val;
            fprintf(stdout, "{\"%s\":%" PRIi32"}\n", tag->key, tag->val.INT32_val);
            break;
        }
        if (tag->val.INT32_val != tag->last_val.INT32_val) {
            tag->last_val.INT32_val = tag->val.INT32_val; 
            fprintf(stdout, "{\"%s\":%" PRIi32"}\n", tag->key, tag->val.INT32_val);
        }
        break;
    case t_UINT16:
        tag->val.UINT16_val = plc_tag_get_uint16(tag_handle, tag->offset);
        if (!tag->watch) {
            tag->last_val.UINT16_val = tag->val.UINT16_val;
            fprintf(stdout, "{\"%s\":%" PRIu16"}\n", tag->key, tag->val.UINT16_val);
            break;
        }
        if (tag->val.UINT16_val != tag->last_val.UINT16_val) {
            tag->last_val.UINT16_val = tag->val.UINT16_val; 
            fprintf(stdout, "{\"%s\":%" PRIu16"}\n", tag->key, tag->val.UINT16_val);
        }
        break;
    case t_INT16:
        tag->val.INT16_val = plc_tag_get_int16(tag_handle, tag->offset);
        if (!tag->watch) {
            tag->last_val.INT16_val = tag->val.INT16_val;
            fprintf(stdout, "{\"%s\":%" PRIi16"}\n", tag->key, tag->val.INT16_val);
            break;
        }
        if (tag->val.INT16_val != tag->last_val.INT16_val) {
            tag->last_val.INT16_val = tag->val.INT16_val; 
            fprintf(stdout, "{\"%s\":%" PRIi16"}\n", tag->key, tag->val.UINT16_val);
        }
        break;
    case t_UINT8:
        tag->val.UINT8_val = plc_tag_get_uint8(tag_handle, tag->offset);
        if (!tag->watch) {
            tag->last_val.UINT8_val = tag->val.UINT8_val;
            fprintf(stdout, "{\"%s\":%" PRIu8"}\n", tag->key, tag->val.UINT8_val);
            break;
        }
        if (tag->val.UINT8_val != tag->last_val.UINT8_val) {
            tag->last_val.UINT8_val = tag->val.UINT8_val; 
            fprintf(stdout, "{\"%s\":%" PRIu8"}\n", tag->key, tag->val.UINT8_val);
        }
        break;
    case t_INT8:
        tag->val.INT8_val = plc_tag_get_int8(tag_handle, tag->offset);
        if (!tag->watch) {
            tag->last_val.INT8_val = tag->val.INT8_val;
            fprintf(stdout, "{\"%s\":%" PRIi8"}\n", tag->key, tag->val.INT8_val);
            break;
        }
        if (tag->val.INT8_val != tag->last_val.INT8_val) {
            tag->last_val.INT8_val = tag->val.INT8_val; 
            fprintf(stdout, "{\"%s\":%" PRIi8"}\n", tag->key, tag->val.INT8_val);
        }
        break;
    case t_FLOAT64:
        tag->val.FLOAT64_val = plc_tag_get_float64(tag_handle, tag->offset);
        if (!tag->watch) {
            tag->last_val.FLOAT64_val = tag->val.FLOAT64_val;
            fprintf(stdout, "{\"%s\":%lf}\n", tag->key, tag->val.FLOAT64_val);
            break;
        }
        if (tag->val.FLOAT64_val != tag->last_val.FLOAT64_val) {
            tag->last_val.FLOAT64_val = tag->val.FLOAT64_val; 
            fprintf(stdout, "{\"%s\":%lf}\n", tag->key, tag->val.FLOAT64_val);
        }
        break;
    case t_FLOAT32:
        tag->val.FLOAT32_val = plc_tag_get_float32(tag_handle, tag->offset);
        if (!tag->watch) {
            tag->last_val.FLOAT32_val = tag->val.FLOAT32_val;
            fprintf(stdout, "{\"%s\":%f}\n", tag->key, tag->val.FLOAT32_val);
            break;
        }
        if (tag->val.FLOAT32_val != tag->last_val.FLOAT32_val) {
            tag->last_val.FLOAT32_val = tag->val.FLOAT32_val; 
            fprintf(stdout, "{\"%s\":%f}\n", tag->key, tag->val.FLOAT32_val);
        }
        break;
    case t_BOOL:
        if (tag->bit == -1) {
            tag->val.BOOL_val = plc_tag_get_uint8(tag_handle, tag->offset);
        } else {
            tag->val.BOOL_val = plc_tag_get_bit(tag_handle, tag->bit);
        }
        if (!tag->watch) {
            tag->last_val.BOOL_val = tag->val.BOOL_val;
            fprintf(stdout, "{\"%s\":%s}\n", tag->key, btoa(tag->val.BOOL_val));
            break;
        }
        if (tag->val.BOOL_val != tag->last_val.BOOL_val) {
            tag->last_val.BOOL_val = tag->val.BOOL_val;
            fprintf(stdout, "{\"%s\":%s}\n", tag->key, btoa(tag->val.BOOL_val));
        }
        break;
    default:
        return PLCTAG_ERR_BAD_STATUS;
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
    while(check_tags() == PLCTAG_STATUS_PENDING){
        util_sleep_ms(1);
    }

    for(t = tags; t != NULL; t = t->hh.next) {
        rc = get_tag(t->tag_handle, &t->tag);
        if(rc != PLCTAG_STATUS_OK) {
            fprintf(stderr,"Unable to parse value of tag %s!\n", plc_tag_decode_error(rc));
            return rc;
        }
    }

    return 0;
}

int set_tag(int32_t tag_handle, tag_t *tag) {
    switch (tag->type) {
    case t_UINT64:
        plc_tag_set_uint64(tag_handle, tag->offset, tag->write_val.UINT64_val);
        break;
    case t_INT64:
        plc_tag_set_int64(tag_handle, tag->offset, tag->write_val.INT64_val);
        break;
    case t_UINT32:
        plc_tag_set_uint32(tag_handle, tag->offset, tag->write_val.UINT32_val);
        break;
    case t_INT32:
        plc_tag_set_int32(tag_handle, tag->offset, tag->write_val.INT32_val);
        break;
    case t_UINT16:
        plc_tag_set_uint16(tag_handle, tag->offset, tag->write_val.UINT16_val);
        break;
    case t_INT16:
        plc_tag_set_int16(tag_handle, tag->offset, tag->write_val.INT16_val);
        break;
    case t_UINT8:
        plc_tag_set_uint8(tag_handle, tag->offset, tag->write_val.UINT8_val);
        break;
    case t_INT8:
        plc_tag_set_int8(tag_handle, tag->offset, tag->write_val.INT8_val);
        break;
    case t_FLOAT64:
        plc_tag_set_float64(tag_handle, tag->offset, tag->write_val.FLOAT64_val);
        break;
    case t_FLOAT32:
        plc_tag_set_float32(tag_handle, tag->offset, tag->write_val.FLOAT32_val);
        break;
    case t_BOOL:
        if (tag->bit == -1) {
            plc_tag_set_uint8(tag_handle, tag->offset, tag->write_val.BOOL_val);
            break;
        }
        plc_tag_set_bit(tag_handle, tag->bit, tag->write_val.BOOL_val);
        break;
    default:
        return PLCTAG_ERR_BAD_STATUS;
        break;
    }

    return PLCTAG_STATUS_OK;
}

int verify_write_tags(void) {
    int rc = PLCTAG_STATUS_OK;
    struct tags *t;

    for(t = tags; t != NULL; t = t->hh.next) {
        switch (t->tag.type) {
        case t_UINT64:
            if (t->tag.last_val.UINT64_val != t->tag.write_val.UINT64_val) {
                fprintf(stderr,"Unable to write value of tag %s!\n", t->tag.key);
                return PLCTAG_ERR_BAD_STATUS;
            }
            break;
        case t_INT64:
            if (t->tag.last_val.INT64_val != t->tag.write_val.INT64_val) {
                fprintf(stderr,"Unable to write value of tag %s!\n", t->tag.key);
                return PLCTAG_ERR_BAD_STATUS;
            }
            break;
        case t_UINT32:
            if (t->tag.last_val.UINT32_val != t->tag.write_val.UINT32_val) {
                fprintf(stderr,"Unable to write value of tag %s!\n", t->tag.key);
                return PLCTAG_ERR_BAD_STATUS;
            }
            break;
        case t_INT32:
            if (t->tag.last_val.INT32_val != t->tag.write_val.INT32_val) {
                fprintf(stderr,"Unable to write value of tag %s!\n", t->tag.key);
                return PLCTAG_ERR_BAD_STATUS;
            }
            break;
        case t_UINT16:
            if (t->tag.last_val.UINT16_val != t->tag.write_val.UINT16_val) {
                fprintf(stderr,"Unable to write value of tag %s!\n", t->tag.key);
                return PLCTAG_ERR_BAD_STATUS;
            }
            break;
        case t_INT16:
            if (t->tag.last_val.INT16_val != t->tag.write_val.INT16_val) {
                fprintf(stderr,"Unable to write value of tag %s!\n", t->tag.key);
                return PLCTAG_ERR_BAD_STATUS;
            }
            break;
        case t_UINT8:
            if (t->tag.last_val.UINT8_val != t->tag.write_val.UINT8_val) {
                fprintf(stderr,"Unable to write value of tag %s!\n", t->tag.key);
                return PLCTAG_ERR_BAD_STATUS;
            }
            break;
        case t_INT8:
            if (t->tag.last_val.INT8_val != t->tag.write_val.INT8_val) {
                fprintf(stderr,"Unable to write value of tag %s!\n", t->tag.key);
                return PLCTAG_ERR_BAD_STATUS;
            }
            break;
        case t_FLOAT64:
            if (t->tag.last_val.FLOAT64_val != t->tag.write_val.FLOAT64_val) {
                fprintf(stderr,"Unable to write value of tag %s!\n", t->tag.key);
                return PLCTAG_ERR_BAD_STATUS;
            }
            break;
        case t_FLOAT32:
            if (t->tag.last_val.FLOAT32_val != t->tag.write_val.FLOAT32_val) {
                fprintf(stderr,"Unable to write value of tag %s!\n", t->tag.key);
                return PLCTAG_ERR_BAD_STATUS;
            }
            break;
        case t_BOOL:
            if (t->tag.last_val.BOOL_val != t->tag.write_val.BOOL_val) {
                fprintf(stderr,"Unable to write value of tag %s!\n", t->tag.key);
                return PLCTAG_ERR_BAD_STATUS;
            }
            break;
        default:
            return PLCTAG_ERR_BAD_STATUS;
            break;
        }
    }

    return rc;
}

int write_tags(void) {
    int rc = PLCTAG_STATUS_OK;
    struct tags *t;

    for(t = tags; t != NULL; t = t->hh.next) {
        rc = set_tag(t->tag_handle, &t->tag);
        if(rc != PLCTAG_STATUS_OK) {
            fprintf(stderr,"Unable to set value of tag %s!\n", plc_tag_decode_error(rc));
            return rc;
        }
        rc = plc_tag_write(t->tag_handle, 0);
        if(rc != PLCTAG_STATUS_PENDING) {
            fprintf(stderr,"Unable to read tag %s!\n", plc_tag_decode_error(rc));
            return rc;
        }
    }

    /* wait for all tags to be ready */
    while(check_tags() == PLCTAG_STATUS_PENDING){
        util_sleep_ms(1);
    }

    read_tags();

    rc = verify_write_tags();
    if (rc != PLCTAG_STATUS_OK) {
        fprintf(stderr,"Unable to write value to tags %s!\n", plc_tag_decode_error(rc));
        return rc;
    }

    return 0;
}

void tag_callback(int32_t tag_handle, int event, int status) {
    /* get that tag associated with the handle */
    struct tags *t;
    HASH_FIND_INT(tags, &tag_handle, t);

    /* handle the events. */
    switch(event) {
    case PLCTAG_EVENT_ABORTED:
        fprintf(stdout, "tag(%s): Tag operation was aborted!\n", t->tag.key);
        break;
    case PLCTAG_EVENT_DESTROYED:
        fprintf(stdout, "tag(%s): Tag was destroyed.\n", t->tag.key);
        break;
    case PLCTAG_EVENT_READ_COMPLETED:
        get_tag(tag_handle, &t->tag);
        fprintf(stdout, "tag(%s): Tag read operation completed with status %s.\n", t->tag.key, plc_tag_decode_error(status));
        break;
    case PLCTAG_EVENT_READ_STARTED:
        fprintf(stdout, "tag(%s): Tag read operation started.\n", t->tag.key);
        break;
    case PLCTAG_EVENT_WRITE_COMPLETED:
        break;
    case PLCTAG_EVENT_WRITE_STARTED:
        break;
    default:
        fprintf(stdout, "tag(%s): Unexpected event %d!\n", t->tag.key, event);
        break;
    }
}

int watch_tags(void) {
    int rc = PLCTAG_STATUS_OK;
    struct tags *t;

    /* set all the tags to watch and register callbacks */
    for(t = tags; t != NULL; t = t->hh.next) {
        t->tag.watch = true;
        rc = plc_tag_register_callback(t->tag_handle, tag_callback);
        if(rc != PLCTAG_STATUS_OK) {
            fprintf(stdout, "Unable to register callback for tag %s!\n", plc_tag_decode_error(rc));
            return rc;
        }
    }

    while(true){
        util_sleep_ms(1000);
    }

    return 0;
}

int destroy_tags(void) {
    struct tags *t;
    int rc = PLCTAG_STATUS_OK;

    for(t = tags; t != NULL; t = t->hh.next) {
        rc = plc_tag_destroy(t->tag_handle);
        if(rc != PLCTAG_STATUS_OK) {
            fprintf(stdout, "Unable to destroy tag %s!\n", plc_tag_decode_error(rc));
            return rc;
        }
    }

    return rc;
}

int do_offline(void) {
    struct tags *t = tags;
    int val = 0;

    fprintf(stdout, "Running offline!\n");

    switch (cli_request.operation) {
    case READ:
        fprintf(stdout, "{}\n");
        break;
    case WRITE:
        fprintf(stdout, "{}\n");
        break;
    case WATCH:
        while (true) {
            ++val;
            val = val%10;
            fprintf(stdout, "{\"%s\":%d}\n", t->tag.key, val);
            fflush(stdout);
            util_sleep_ms(2000);
        }
        break;
    default:
        break;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    if (parse_args(argc, argv) == -1) {
        fprintf(stderr, "ERROR: invalid arguments.\n");
        usage();
        exit(1);
    }

    print_request();

    if(plc_tag_check_lib_version(LIBPLCTAG_REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "ERROR: Required compatible library version %d.%d.%d not available!\n", LIBPLCTAG_REQUIRED_VERSION);
        exit(1);
    }

    if (process_tags() != PLCTAG_STATUS_OK) {
        fprintf(stderr, "ERROR: Could not process tags.\n");
        exit(1);
    }

    if (cli_request.offline) {
        exit(do_offline());
    }

    /* wait for all tags to be ready */
    while(check_tags() == PLCTAG_STATUS_PENDING){
        util_sleep_ms(1);
    }

    switch (cli_request.operation) {
    case READ:
        if (read_tags() != PLCTAG_STATUS_OK) {
            fprintf(stderr, "ERROR: Tag read failed.\n");
            exit(1);
        }
        break;
    case WRITE:
        if (write_tags() != PLCTAG_STATUS_OK) {
            fprintf(stderr, "ERROR: Tag write failed.\n");
            exit(1);
        }
        break;
    case WATCH:
        if (read_tags() != PLCTAG_STATUS_OK) {
            fprintf(stderr, "ERROR: Tag read failed.\n");
            exit(1);
        }
        watch_tags();
        break;
    default:
        break;
    }

    destroy_tags();
    plc_tag_shutdown();

    fprintf(stdout, "DONE.\n");
    exit(0);
}

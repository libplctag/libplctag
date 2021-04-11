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
#include "../util/debug.h"

/* globals here */
struct tags *tags = NULL;
cli_request_t cli_request = {
    "ab_eip",
    "127.0.0.1",
    "1,0",
    "controllogix",
    READ,
    500,
    1, // DEBUG_ERROR
    "",
    false // offline
};

void usage(void) 
{
    fprintf(stdout, "Usage:\n");
    fprintf(stdout, "\tLIBPLCTAG CLI.\n");
    fprintf(stdout, "\tThis is a command-line interface to access tags/registers, in PLCs supported by libplctag.\n");
    fprintf(stdout, "\n\tcli {--read | --write | --watch} {-protocol} {-ip} {-path} {-plc}\n");
    fprintf(stdout, "\t\t[-debug] [-interval] [-attributes] [-offline]\n");

    fprintf(stdout, "\n\tCLI Action (Required):\n");
    fprintf(stdout, "\n\t--read\t\t- Perform a one-shot READ operation.\n");
    fprintf(stdout, "\t--write\t\t- Perform a one-shot WRITE operation.\n");
    fprintf(stdout, "\t--watch\t\t- Perform a continuous WATCH operation at the specified interval.\n");
    fprintf(stdout, "\t-h | --help\t- Prints the Usage details.\n");
    
    fprintf(stdout, "\n\tLIBPLCTAG Parameters (Required):\n");
    fprintf(stdout, "\n\t-protocol\t- type of plc protocol. (default: ab_eip)\n");
    fprintf(stdout, "\t-ip\t\t- network address for the host PLC. (default: 127.0.0.1)\n");
    fprintf(stdout, "\t-path\t\t- routing path for the Tags. (default: 1,0)\n");
    fprintf(stdout, "\t-plc\t\t- type of the PLC. (default: controllogix)\n");

    fprintf(stdout, "\n\tLIBPLCTAG Parameters (Optional):\n");
    fprintf(stdout, "\n\t-debug\t\t- logging output level. (default: 1)\n");
    fprintf(stdout, "\t-interval\t- interval in ms for WATCH operation. (default: 500)\n");
    fprintf(stdout, "\t-attributes\t- additional attributes. (default: '')\n");
    fprintf(stdout, "\t-offline\t- operation mode. (default: false)\n");

    fflush(stdout);
}

int parse_args(int argc, char *argv[]) 
{
    if (argc < 2) {
        fprintf(stderr, "ERROR: invalid number of arguments.\n");
        fflush(stderr);
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
    } else if (!strcmp(operation, "--help") || !strcmp(operation, "-h")) {
        usage();
        exit(0);
    } else {
        fprintf(stderr, "ERROR: invalid PLC operation.\n");
        fprintf(stderr, "INFO: Use one of --read, --write, --watch.\n");
        fflush(stderr);
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
                fflush(stderr);
            }
        } else {
            fprintf(stderr, "ERROR: invalid PLC parameter: %s.\n", param);
            fprintf(stderr, "INFO: Supported params -protocol, -ip, -path, -plc, -debug, -interval, -attributes, -offline.\n");
            fflush(stderr);
            return -1;
        } 
    }

    return 0;
}

void print_request() 
{
    pdebug(DEBUG_INFO, "Running with params:");
    pdebug(DEBUG_INFO, "Protocol: %s", cli_request.protocol);
    pdebug(DEBUG_INFO, "IP: %s", cli_request.ip);
    pdebug(DEBUG_INFO, "Path: %s", cli_request.path);
    pdebug(DEBUG_INFO, "PLC: %s", cli_request.plc);
    switch (cli_request.operation) {
    case READ:
        pdebug(DEBUG_INFO, "Operation: READ.");
        break;
    case WRITE:
        pdebug(DEBUG_INFO, "Operation: WRITE.");
        break;
    case WATCH:
        pdebug(DEBUG_INFO, "Operation: WATCH.");
        break;
    default:
        pdebug(DEBUG_INFO, "Operation: INVALID.");
        break;
    }
    pdebug(DEBUG_INFO, "Interval: %d", cli_request.interval);
    pdebug(DEBUG_INFO, "Debug Level: %d", cli_request.debug_level);
    pdebug(DEBUG_INFO, "Additional Attributes: %s", cli_request.attributes);
    pdebug(DEBUG_INFO, "Offline: %s", btoa(cli_request.offline));
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

tag_line_parts_t split_string(const char *str, const char *sep)
{
    int sub_str_count=0;
    int size = 0;
    const char *sub;
    const char *tmp;
    char **res = NULL;
    tag_line_parts_t tag_line_parts_t = {NULL, -1};

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

    tag_line_parts_t.num_parts = sub_str_count;

    /* calculate total size for string plus pointers */
    size = (int)(sizeof(char *)*((size_t)sub_str_count+1))+(int)strlen(str)+1;

    /* allocate enough memory */
    res = malloc((size_t)size);
    if(!res) {
        return tag_line_parts_t;
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

    tag_line_parts_t.parts = res;

    return tag_line_parts_t;
}

int validate_line(tag_line_parts_t tag_line_parts) {
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
        while(j < tag_line_parts.num_parts) {
            part = strdup(tag_line_parts.parts[j]);
            if (!strcmp(req_params[i], strtok(part, "="))) {
                found = true;
                break;
            }
            ++j;
        }
        if (!found) {
            pdebug(DEBUG_ERROR, "Line missing: %s", req_params[i]);
            return -1;
        }
        found = false;
    }

    free(part);
    return 0;
}

void print_tag(tag_t *tag) {
    pdebug(DEBUG_INFO, "Tag created:");
    pdebug(DEBUG_INFO, "Key: %s", tag->key);
    switch (tag->type) {
    case t_UINT64:
        pdebug(DEBUG_INFO, "Type: uint64.");
        break;
    case t_INT64:
        pdebug(DEBUG_INFO, "Type: int64.");
        break;
    case t_UINT32:
        pdebug(DEBUG_INFO, "Type: uint32.");
        break;
    case t_INT32:
        pdebug(DEBUG_INFO, "Type: int32.");
        break;
    case t_UINT16:
        pdebug(DEBUG_INFO, "Type: uint16.");
        break;
    case t_INT16:
        pdebug(DEBUG_INFO, "Type: int16.");
        break;
    case t_UINT8:
        pdebug(DEBUG_INFO, "Type: uint8.");
        break;
    case t_INT8:
        pdebug(DEBUG_INFO, "Type: int8.");
        break;
    case t_FLOAT64:
        pdebug(DEBUG_INFO, "Type: float64.");
        break;
    case t_FLOAT32:
        pdebug(DEBUG_INFO, "Type: float32.");
        break;
    case t_BOOL:
        pdebug(DEBUG_INFO, "Type: bool.");
        break;
    default:
        pdebug(DEBUG_INFO, "Type: INVALID.");
        break;
    }
    pdebug(DEBUG_INFO, "Path: %s", tag->path);
    pdebug(DEBUG_INFO, "Bit: %d", tag->bit);
    pdebug(DEBUG_INFO, "Offset: %d", tag->offset);
    pdebug(DEBUG_INFO, "Watch: %s", btoa(tag->watch));
}

int process_line(const char *line, tag_t *tag) 
{
    tag_line_parts_t tag_line_parts;

    tag_line_parts = split_string(line, ",");
    if(tag_line_parts.num_parts < 0) {
        pdebug(DEBUG_ERROR, "Splitting string failed for string %s!", line);
        return -1;
    }

    /* check if the relevant parameters are there or not */
    if (validate_line(tag_line_parts) != 0) {
        pdebug(DEBUG_ERROR, "Line does not contain enough parts. Line: %s", line);
        return -1;
    }

    pdebug(DEBUG_INFO, "Line validated!");

    /* setup all the associated tag values here. */

    /* set defaults here first */
    tag->bit = -1;
    tag->offset = 0;
    tag->watch = false;
    pdebug(DEBUG_INFO, "Tag defaults set!");

    /* loop through all the val pairs now */
    char *type;
    char *write_val;

    int i = 0;
    char *part;
    char *param;
    char *val;
    while (i < tag_line_parts.num_parts) {
        part = strdup(tag_line_parts.parts[i]);
        pdebug(DEBUG_INFO, "Part: %s", part);
        param = strtok(part, "=");
        val = strtok(NULL, "");
        pdebug(DEBUG_INFO, "[Param, Value]: [%s, %s]", param, val);

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
            pdebug(DEBUG_ERROR, "Unknown param %s!", param);
            return -1;
        }

        ++i;
    }

    pdebug(DEBUG_INFO, "Parsing tag type now...");
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
        pdebug(DEBUG_ERROR, "Unknown data type for %s!", type);
        return -1;
    }

    free(tag_line_parts.parts);
    print_tag(tag);
    pdebug(DEBUG_INFO, "Line processed!");

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
            pdebug(DEBUG_INFO, "(%d) TAG STATUS NOT OK! Tag Status: %s.", t->tag_handle, plc_tag_decode_error(rc));
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

        tag_t tag;
        trim_line(line);
        pdebug(DEBUG_INFO, "Trimmed Line: %s", line);
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

        pdebug(DEBUG_INFO, "%s", tag_path);
        int tag_handle = plc_tag_create(tag_path, 0);
        if (tag_handle < 0) {
            pdebug(DEBUG_ERROR, "Error, %s, creating tag %s with string %s!", plc_tag_decode_error(tag_handle), tag.key, tag_path);
            free(tag_path);
            free(line);
            return -1;
        }

        add_tag(tag_handle, tag);
    }

    free(tag_path);
    free(line);
    pdebug(DEBUG_INFO, "DONE processing tags.");
    return 0;
}

int get_tag(int32_t tag_handle, tag_t *tag) {
    switch (tag->type) {
    case t_UINT64:
        tag->val.UINT64_val = plc_tag_get_uint64(tag_handle, tag->offset);
        if (!tag->watch) {
            tag->last_val.UINT64_val = tag->val.UINT64_val; 
            fprintf(stdout, "{\"%s\":%" PRIu64"}\n", tag->key, tag->val.UINT64_val);
            fflush(stdout);
            break;
        }
        if (tag->val.UINT64_val != tag->last_val.UINT64_val) {
            tag->last_val.UINT64_val = tag->val.UINT64_val; 
            fprintf(stdout, "{\"%s\":%" PRIu64"}\n", tag->key, tag->val.UINT64_val);
            fflush(stdout);
        }
        break;
    case t_INT64:
        tag->val.INT64_val = plc_tag_get_int64(tag_handle, tag->offset);
        if (!tag->watch) {
            tag->last_val.INT64_val = tag->val.INT64_val;
            fprintf(stdout, "{\"%s\":%" PRIi64"}\n", tag->key, tag->val.INT64_val);
            fflush(stdout);
            break;
        }
        if (tag->val.INT64_val != tag->last_val.INT64_val) {
            tag->last_val.INT64_val = tag->val.INT64_val; 
            fprintf(stdout, "{\"%s\":%" PRIi64"}\n", tag->key, tag->val.INT64_val);
            fflush(stdout);
        }
        break;
    case t_UINT32:
        tag->val.UINT32_val = plc_tag_get_uint32(tag_handle, tag->offset);
        if (!tag->watch) {
            tag->last_val.UINT32_val = tag->val.UINT32_val;
            fprintf(stdout, "{\"%s\":%" PRIu32"}\n", tag->key, tag->val.UINT32_val);
            fflush(stdout);
            break;
        }
        if (tag->val.UINT32_val != tag->last_val.UINT32_val) {
            tag->last_val.UINT32_val = tag->val.UINT32_val; 
            fprintf(stdout, "{\"%s\":%" PRIu32"}\n", tag->key, tag->val.UINT32_val);
            fflush(stdout);
        }
        break;
    case t_INT32:
        tag->val.INT32_val = plc_tag_get_int32(tag_handle, tag->offset);
        if (!tag->watch) {
            tag->last_val.INT32_val = tag->val.INT32_val;
            fprintf(stdout, "{\"%s\":%" PRIi32"}\n", tag->key, tag->val.INT32_val);
            fflush(stdout);
            break;
        }
        if (tag->val.INT32_val != tag->last_val.INT32_val) {
            tag->last_val.INT32_val = tag->val.INT32_val; 
            fprintf(stdout, "{\"%s\":%" PRIi32"}\n", tag->key, tag->val.INT32_val);
            fflush(stdout);
        }
        break;
    case t_UINT16:
        tag->val.UINT16_val = plc_tag_get_uint16(tag_handle, tag->offset);
        if (!tag->watch) {
            tag->last_val.UINT16_val = tag->val.UINT16_val;
            fprintf(stdout, "{\"%s\":%" PRIu16"}\n", tag->key, tag->val.UINT16_val);
            fflush(stdout);
            break;
        }
        if (tag->val.UINT16_val != tag->last_val.UINT16_val) {
            tag->last_val.UINT16_val = tag->val.UINT16_val; 
            fprintf(stdout, "{\"%s\":%" PRIu16"}\n", tag->key, tag->val.UINT16_val);
            fflush(stdout);
        }
        break;
    case t_INT16:
        tag->val.INT16_val = plc_tag_get_int16(tag_handle, tag->offset);
        if (!tag->watch) {
            tag->last_val.INT16_val = tag->val.INT16_val;
            fprintf(stdout, "{\"%s\":%" PRIi16"}\n", tag->key, tag->val.INT16_val);
            fflush(stdout);
            break;
        }
        if (tag->val.INT16_val != tag->last_val.INT16_val) {
            tag->last_val.INT16_val = tag->val.INT16_val; 
            fprintf(stdout, "{\"%s\":%" PRIi16"}\n", tag->key, tag->val.UINT16_val);
            fflush(stdout);
        }
        break;
    case t_UINT8:
        tag->val.UINT8_val = plc_tag_get_uint8(tag_handle, tag->offset);
        if (!tag->watch) {
            tag->last_val.UINT8_val = tag->val.UINT8_val;
            fprintf(stdout, "{\"%s\":%" PRIu8"}\n", tag->key, tag->val.UINT8_val);
            fflush(stdout);
            break;
        }
        if (tag->val.UINT8_val != tag->last_val.UINT8_val) {
            tag->last_val.UINT8_val = tag->val.UINT8_val; 
            fprintf(stdout, "{\"%s\":%" PRIu8"}\n", tag->key, tag->val.UINT8_val);
            fflush(stdout);
        }
        break;
    case t_INT8:
        tag->val.INT8_val = plc_tag_get_int8(tag_handle, tag->offset);
        if (!tag->watch) {
            tag->last_val.INT8_val = tag->val.INT8_val;
            fprintf(stdout, "{\"%s\":%" PRIi8"}\n", tag->key, tag->val.INT8_val);
            fflush(stdout);
            break;
        }
        if (tag->val.INT8_val != tag->last_val.INT8_val) {
            tag->last_val.INT8_val = tag->val.INT8_val; 
            fprintf(stdout, "{\"%s\":%" PRIi8"}\n", tag->key, tag->val.INT8_val);
            fflush(stdout);
        }
        break;
    case t_FLOAT64:
        tag->val.FLOAT64_val = plc_tag_get_float64(tag_handle, tag->offset);
        if (!tag->watch) {
            tag->last_val.FLOAT64_val = tag->val.FLOAT64_val;
            fprintf(stdout, "{\"%s\":%lf}\n", tag->key, tag->val.FLOAT64_val);
            fflush(stdout);
            break;
        }
        if (tag->val.FLOAT64_val != tag->last_val.FLOAT64_val) {
            tag->last_val.FLOAT64_val = tag->val.FLOAT64_val; 
            fprintf(stdout, "{\"%s\":%lf}\n", tag->key, tag->val.FLOAT64_val);
            fflush(stdout);
        }
        break;
    case t_FLOAT32:
        tag->val.FLOAT32_val = plc_tag_get_float32(tag_handle, tag->offset);
        if (!tag->watch) {
            tag->last_val.FLOAT32_val = tag->val.FLOAT32_val;
            fprintf(stdout, "{\"%s\":%f}\n", tag->key, tag->val.FLOAT32_val);
            fflush(stdout);
            break;
        }
        if (tag->val.FLOAT32_val != tag->last_val.FLOAT32_val) {
            tag->last_val.FLOAT32_val = tag->val.FLOAT32_val; 
            fprintf(stdout, "{\"%s\":%f}\n", tag->key, tag->val.FLOAT32_val);
            fflush(stdout);
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
            fflush(stdout);
            break;
        }
        if (tag->val.BOOL_val != tag->last_val.BOOL_val) {
            tag->last_val.BOOL_val = tag->val.BOOL_val;
            fprintf(stdout, "{\"%s\":%s}\n", tag->key, btoa(tag->val.BOOL_val));
            fflush(stdout);
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
            pdebug(DEBUG_ERROR, "Unable to read tag %s!", plc_tag_decode_error(rc));
            return rc;
        }
    }

    /* wait for all tags to be ready */
    while(check_tags() == PLCTAG_STATUS_PENDING){
        pdebug(DEBUG_INFO, "Waiting for tags to be ready...");
        util_sleep_ms(1);
    }

    for(t = tags; t != NULL; t = t->hh.next) {
        rc = get_tag(t->tag_handle, &t->tag);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_ERROR, "Unable to parse value of tag %s!", plc_tag_decode_error(rc));
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
                pdebug(DEBUG_ERROR, "Unable to write value of tag %s!", t->tag.key);
                return PLCTAG_ERR_BAD_STATUS;
            }
            break;
        case t_INT64:
            if (t->tag.last_val.INT64_val != t->tag.write_val.INT64_val) {
                pdebug(DEBUG_ERROR, "Unable to write value of tag %s!", t->tag.key);
                return PLCTAG_ERR_BAD_STATUS;
            }
            break;
        case t_UINT32:
            if (t->tag.last_val.UINT32_val != t->tag.write_val.UINT32_val) {
                pdebug(DEBUG_ERROR, "Unable to write value of tag %s!", t->tag.key);
                return PLCTAG_ERR_BAD_STATUS;
            }
            break;
        case t_INT32:
            if (t->tag.last_val.INT32_val != t->tag.write_val.INT32_val) {
                pdebug(DEBUG_ERROR, "Unable to write value of tag %s!", t->tag.key);
                return PLCTAG_ERR_BAD_STATUS;
            }
            break;
        case t_UINT16:
            if (t->tag.last_val.UINT16_val != t->tag.write_val.UINT16_val) {
                pdebug(DEBUG_ERROR, "Unable to write value of tag %s!", t->tag.key);
                return PLCTAG_ERR_BAD_STATUS;
            }
            break;
        case t_INT16:
            if (t->tag.last_val.INT16_val != t->tag.write_val.INT16_val) {
                pdebug(DEBUG_ERROR, "Unable to write value of tag %s!", t->tag.key);
                return PLCTAG_ERR_BAD_STATUS;
            }
            break;
        case t_UINT8:
            if (t->tag.last_val.UINT8_val != t->tag.write_val.UINT8_val) {
                pdebug(DEBUG_ERROR, "Unable to write value of tag %s!", t->tag.key);
                return PLCTAG_ERR_BAD_STATUS;
            }
            break;
        case t_INT8:
            if (t->tag.last_val.INT8_val != t->tag.write_val.INT8_val) {
                pdebug(DEBUG_ERROR, "Unable to write value of tag %s!", t->tag.key);
                return PLCTAG_ERR_BAD_STATUS;
            }
            break;
        case t_FLOAT64:
            if (t->tag.last_val.FLOAT64_val != t->tag.write_val.FLOAT64_val) {
                pdebug(DEBUG_ERROR, "Unable to write value of tag %s!", t->tag.key);
                return PLCTAG_ERR_BAD_STATUS;
            }
            break;
        case t_FLOAT32:
            if (t->tag.last_val.FLOAT32_val != t->tag.write_val.FLOAT32_val) {
                pdebug(DEBUG_ERROR, "Unable to write value of tag %s!", t->tag.key);
                return PLCTAG_ERR_BAD_STATUS;
            }
            break;
        case t_BOOL:
            if (t->tag.last_val.BOOL_val != t->tag.write_val.BOOL_val) {
                pdebug(DEBUG_ERROR, "Unable to write value of tag %s!", t->tag.key);
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
            pdebug(DEBUG_ERROR, "Unable to set value of tag %s!", plc_tag_decode_error(rc));
            return rc;
        }
        rc = plc_tag_write(t->tag_handle, 0);
        if(rc != PLCTAG_STATUS_PENDING) {
            pdebug(DEBUG_ERROR, "Unable to read tag %s!", plc_tag_decode_error(rc));
            return rc;
        }
    }

    /* wait for all tags to be ready */
    while(check_tags() == PLCTAG_STATUS_PENDING){
        pdebug(DEBUG_INFO, "Waiting for tags to be ready...");
        util_sleep_ms(1);
    }

    read_tags();

    rc = verify_write_tags();
    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to write value to tags %s!", plc_tag_decode_error(rc));
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
        pdebug(DEBUG_INFO, "tag(%s): Tag operation was aborted!", t->tag.key);
        break;
    case PLCTAG_EVENT_DESTROYED:
        pdebug(DEBUG_INFO, "tag(%s): Tag was destroyed.", t->tag.key);
        break;
    case PLCTAG_EVENT_READ_COMPLETED:
        get_tag(tag_handle, &t->tag);
        pdebug(DEBUG_INFO, "tag(%s): Tag read operation completed with status %s.", t->tag.key, plc_tag_decode_error(status));
        break;
    case PLCTAG_EVENT_READ_STARTED:
        pdebug(DEBUG_INFO, "tag(%s): Tag read operation started.", t->tag.key);
        break;
    case PLCTAG_EVENT_WRITE_COMPLETED:
        break;
    case PLCTAG_EVENT_WRITE_STARTED:
        break;
    default:
        pdebug(DEBUG_INFO, "tag(%s): Unexpected event %d!", t->tag.key, event);
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
            pdebug(DEBUG_ERROR, "Unable to register callback for tag %s!", plc_tag_decode_error(rc));
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
            pdebug(DEBUG_ERROR, "Unable to destroy tag %s!", plc_tag_decode_error(rc));
            return rc;
        }
    }

    return rc;
}

int do_offline(void) {
    struct tags *t = tags;
    int val = 0;

    pdebug(DEBUG_INFO, "Running offline!");

    switch (cli_request.operation) {
    case READ:
        fprintf(stdout, "{}\n");
        fflush(stdout);
        break;
    case WRITE:
        fprintf(stdout, "{}\n");
        fflush(stdout);
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
        fflush(stderr);
        usage();
        exit(1);
    }

    plc_tag_set_debug_level(cli_request.debug_level);

    print_request();

    if (process_tags() != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Could not process tags.");
        exit(1);
    }

    if (cli_request.offline) {
        exit(do_offline());
    }

    /* wait for all tags to be ready */
    while(check_tags() == PLCTAG_STATUS_PENDING){
        pdebug(DEBUG_INFO, "Waiting for tags to be ready...");
        util_sleep_ms(1);
    }

    switch (cli_request.operation) {
    case READ:
        if (read_tags() != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_ERROR, "Tag read failed.");
            exit(1);
        }
        break;
    case WRITE:
        if (write_tags() != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_ERROR, "Tag write failed.");
            exit(1);
        }
        break;
    case WATCH:
        if (read_tags() != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_ERROR, "Tag read failed.");
            exit(1);
        }
        watch_tags();
        break;
    default:
        break;
    }

    destroy_tags();
    plc_tag_shutdown();

    pdebug(DEBUG_INFO, "DONE.");
    exit(0);
}

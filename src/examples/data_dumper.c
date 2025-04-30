/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever     *
 * you choose.                                                             *
 *                                                                         *
 * MPL 2.0:                                                                *
 *                                                                         *
 *   This Source Code Form is subject to the terms of the Mozilla Public   *
 *   License, v. 2.0. If a copy of the MPL was not distributed with this   *
 *   file, You can obtain one at http://mozilla.org/MPL/2.0/.              *
 *                                                                         *
 *                                                                         *
 * LGPL 2:                                                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/


#include "compat_utils.h"
#include <ctype.h>
#include <libplctag/lib/libplctag.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REQUIRED_VERSION 2, 6, 4

#define MAX_TAGS 5000
#define RECONNECT_DELAY_MS 5000


typedef enum { UNKNOWN = 0, i32_type, i16_type, i8_type, f32_type } data_type_t;

struct {
    char *name;
    int rpi;
    int64_t next_read;
    int32_t tag_id;
    int reading;
    data_type_t data_type;
    union {
        int32_t DINT_val;
        int16_t INT_val;
        int8_t SINT_val;
        float REAL_val;
    } val;
} tags[MAX_TAGS];

int num_tags = 0;


volatile int terminate = 0;


int is_comment(const char *line) {
    int i = 0;

    /* scan past the first whitespace */
    for(i = 0; line[i] && isspace(line[i]); i++);

    return (line[i] == '#') ? 1 : 0;
}


char **split_string(const char *str, const char *sep) {
    size_t sub_str_count = 0;
    size_t source_size = 0;
    size_t result_size = 0;
    const char *sub;
    const char *tmp;
    char **res = NULL;

    /* first, count the sub strings */
    tmp = str;
    sub = strstr(tmp, sep);

    while(sub && *sub) {
        /* separator could be at the front. */
        if(sub != tmp) { sub_str_count++; }

        tmp = sub + strlen(sep);
        sub = strstr(tmp, sep);
    }

    if(tmp && *tmp && (!sub || !*sub)) { sub_str_count++; }

    /* calculate total size for string plus pointers */
    source_size = strlen(str) + 1;
    result_size = (sizeof(char *) * (sub_str_count + 1)) + source_size;

    /* allocate enough memory */
    res = malloc(result_size);
    if(!res) { return NULL; }

    /* calculate the beginning of the string */
    tmp = (char *)res + (sizeof(char *) * (size_t)(sub_str_count + 1));

    /* copy the string into the new buffer past the first part with the array of char pointers. */
    // NOLINTNEXTLINE
    strcpy((char *)tmp, str);

    /* set up the pointers */
    sub_str_count = 0;
    sub = strstr(tmp, sep);

    while(sub && *sub) {
        /* separator could be at the front */
        if(sub != tmp) {
            /* store the pointer */
            res[sub_str_count] = (char *)tmp;

            sub_str_count++;
        }

        /* zero out the separator chars */
        // NOLINTNEXTLINE
        memset((char *)sub, 0, strlen(sep));

        /* point past the separator (now zero) */
        tmp = sub + strlen(sep);

        /* find the next separator */
        sub = strstr(tmp, sep);
    }

    /* if there is a chunk at the end, store it. */
    if(tmp && *tmp && (!sub || !*sub)) { res[sub_str_count] = (char *)tmp; }

    return res;
}


/*
 * line format:
 *
 * <name>\t<type>\t<rpi>\t<tag string>
 */
int process_line(const char *line) {
    int rc = PLCTAG_STATUS_OK;
    char **parts = NULL;

    do {
        parts = split_string(line, "\t");
        if(!parts) {
            // NOLINTNEXTLINE
            fprintf(stderr, "Splitting string failed for string %s!", line);
            rc = PLCTAG_ERR_BAD_CONFIG;
            break;
        }


        /* make sure we got 4 pieces. */
        for(int i = 0; i < 4; i++) {
            if(parts[i] == NULL) {
                // NOLINTNEXTLINE
                fprintf(stderr, "Line does not contain enough parts. Line: %s\n", line);
                free(parts);
                parts = NULL;
                rc = PLCTAG_ERR_BAD_CONFIG;
                break;
            }
        }

        /* if we dropped out of the for loop, we still need to drop out of the do/while. */
        if(rc != PLCTAG_STATUS_OK) { break; }

        tags[num_tags].name = strdup(parts[0]);

        if(compat_strcasecmp("dint", parts[1]) == 0) {
            tags[num_tags].data_type = i32_type;
        } else if(compat_strcasecmp("int", parts[1]) == 0) {
            tags[num_tags].data_type = i16_type;
        } else if(compat_strcasecmp("sint", parts[1]) == 0) {
            tags[num_tags].data_type = i8_type;
        } else if(compat_strcasecmp("real", parts[1]) == 0) {
            tags[num_tags].data_type = f32_type;
        } else {
            // NOLINTNEXTLINE
            fprintf(stderr, "Unknown data type for %s!\n", parts[1]);
            rc = PLCTAG_ERR_BAD_CONFIG;
            break;
        }

        tags[num_tags].rpi = atoi(parts[2]);
        tags[num_tags].next_read = 0;
        tags[num_tags].tag_id = plc_tag_create(parts[3], 0); /* create async */

        if(tags[num_tags].tag_id < 0) {
            // NOLINTNEXTLINE
            fprintf(stderr, "Error, %s, creating tag %s with string %s!\n", plc_tag_decode_error(tags[num_tags].tag_id),
                    tags[num_tags].name, parts[3]);
            rc = tags[num_tags].tag_id;
        }
    } while(0);

    if(parts) {
        free(parts);
        parts = NULL;
    }

    if(rc != PLCTAG_STATUS_OK) {
        free(tags[num_tags].name);
    } else {
        num_tags++;
    }

    // printf("Tag %d has name %s, type %s, RPI %d, and tag string %s\n",num_tags, parts[0], parts[1], tags[num_tags].rpi,
    // parts[3]);

    return rc;
}


void trim_line(char *line) {
    int len = 0;

    if(!line || strlen(line) == 0) { return; }

    len = (int)strlen(line);

    while(len > 0 && line[len - 1] == '\n') {
        line[len - 1] = 0;
        len--;
    }
}


int read_config(const char *config_filename) {
    int rc = PLCTAG_STATUS_OK;
    FILE *config = NULL;
    char line[1024] = {
        0,
    };
    int line_num = 0;

    //    printf("Reading config file %s.\n", config_filename);

    /* open the config file */
    config = fopen(config_filename, "r");
    if(!config) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Unable to open config file %s!\n", config_filename);
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* read all the lines */
    while(fgets(line, sizeof(line), config)) {
        line_num++;

        trim_line(line);

        //        printf("Processing line %d: %s\n", line_num, line);

        /* skip blank lines and comments */
        if(strlen(line) < 25 || is_comment(line)) { continue; }

        if((rc = process_line(line)) != PLCTAG_STATUS_OK) {
            // NOLINTNEXTLINE
            fprintf(stderr, "Error, %s, processing config file on line %d!\n", plc_tag_decode_error(rc), line_num);
            fclose(config);
            return rc;
        }
    }

    fclose(config);

    printf("Read %d tags from the config file.\n", num_tags);

    return rc;
}


FILE *check_log_file(void) {
    static int log_year = 0;
    static int log_month = 0;
    static int log_day = 0;
    static FILE *log = NULL;
    time_t t = time(NULL);
    struct tm *tm_struct;

    tm_struct = localtime(&t);

    /* do we need to (re)open the file?*/
    if(log_year != tm_struct->tm_year || log_month != tm_struct->tm_mon || log_day != tm_struct->tm_mday) {
        char log_file_name[128];

        log_year = tm_struct->tm_year;
        log_month = tm_struct->tm_mon;
        log_day = tm_struct->tm_mday;

        // NOLINTNEXTLINE
        snprintf(log_file_name, sizeof(log_file_name), "log-%04d-%02d-%02d.log", 1900 + log_year, log_month, log_day);
        if(log) { fclose(log); }

        log = fopen(log_file_name, "a");
    }

    return log;
}


int make_prefix(char *prefix_buf, int prefix_buf_size) {
    struct tm t;
    time_t epoch;
    int64_t epoch_ms;
    int remainder_ms;
    int rc = PLCTAG_STATUS_OK;

    /* make sure we have room, MAGIC */
    if(prefix_buf_size < 37) { return PLCTAG_ERR_TOO_SMALL; }

    /* build the prefix */

    /* get the time parts */
    epoch_ms = compat_time_ms();
    epoch = (time_t)(epoch_ms / 1000);
    remainder_ms = (int)(epoch_ms % 1000);

    /* FIXME - should capture error return! */
    localtime_r(&epoch, &t);

    /* create the prefix and format for the file entry. */
    // NOLINTNEXTLINE
    rc = snprintf(prefix_buf, (size_t)prefix_buf_size, "%04d-%02d-%02d %02d:%02d:%02d.%03d", t.tm_year + 1900, t.tm_mon,
                  t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, remainder_ms);

    /* enforce zero string termination */
    if(rc > 1 && rc < prefix_buf_size) {
        prefix_buf[rc] = 0;
    } else {
        prefix_buf[prefix_buf_size - 1] = 0;
    }

    return rc;
}


int log_data(void) {
    FILE *log = check_log_file();
    char timestamp_buf[128];
    int rc = PLCTAG_STATUS_OK;

    if(!log) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Error opening log file!\n");
        return PLCTAG_ERR_OPEN;
    }

    rc = make_prefix(timestamp_buf, sizeof(timestamp_buf));
    if(rc < 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Unable to make prefix, error %s!\n", plc_tag_decode_error(rc));
        return rc;
    }

    for(int tag = 0; tag < num_tags; tag++) {
        /* skip if this tag is not being read. */
        if(!tags[tag].reading) { continue; }

        // NOLINTNEXTLINE
        fprintf(log, "%s,%s", timestamp_buf, tags[tag].name);

        switch(tags[tag].data_type) {
            case i32_type:
                // NOLINTNEXTLINE
                fprintf(log, ",%d\n", plc_tag_get_int32(tags[tag].tag_id, 0));
                break;

            case i16_type:
                // NOLINTNEXTLINE
                fprintf(log, ",%d\n", plc_tag_get_int16(tags[tag].tag_id, 0));
                break;

            case i8_type:
                // NOLINTNEXTLINE
                fprintf(log, ",%d\n", plc_tag_get_int8(tags[tag].tag_id, 0));
                break;

            case f32_type:
                // NOLINTNEXTLINE
                fprintf(log, ",%f\n", plc_tag_get_float32(tags[tag].tag_id, 0));
                break;

            default:
                // NOLINTNEXTLINE
                fprintf(stderr, "Unknown datatype (%d) for tag %d!\n", tags[tag].data_type, tag);
                return PLCTAG_ERR_BAD_CONFIG;
                break;
        }
    }

    fflush(log);

    return PLCTAG_STATUS_OK;
}


/* loop while any tag is still in PLCTAG_STATUS_PENDING status */
int check_tags(void) {
    int rc = PLCTAG_STATUS_OK;

    for(int t = 0; t < num_tags; t++) {
        rc = plc_tag_status(tags[t].tag_id);

        if(rc != PLCTAG_STATUS_OK) { return rc; }
    }

    return rc;
}


void destroy_tags(void) {
    for(int t = 0; t < num_tags; t++) {
        if(tags[t].name) {
            free((char *)tags[t].name);
            plc_tag_destroy(tags[t].tag_id);
        }
    }
}


int start_reads(void) {
    int64_t now = compat_time_ms();
    int rc = PLCTAG_STATUS_OK;

    /* kick off any reads that need to happen */
    for(int t = 0; t < num_tags; t++) {
        /* trigger a read if the interval is overdue. */
        if(tags[t].next_read < now) {
            tags[t].next_read = now + tags[t].rpi;

            tags[t].reading = 1;

            rc = plc_tag_read(tags[t].tag_id, 0);
            if(rc != PLCTAG_STATUS_PENDING) {
                // NOLINTNEXTLINE
                fprintf(stderr, "Unable to start reading tag %s!\n", plc_tag_decode_error(rc));
                destroy_tags();
                return rc;
            }
        } else {
            /* this tag is not being read. */
            tags[t].reading = 0;
        }
    }

    return PLCTAG_STATUS_OK;
}


void interrupt_handler(void) { terminate = 1; }

void usage(void) {
    // NOLINTNEXTLINE
    fprintf(stderr, "Usage: data_dumper <config file>\n");
    // NOLINTNEXTLINE
    fprintf(stderr, "The config file must contain tab-delimited rows in the following format:\n");
    // NOLINTNEXTLINE
    fprintf(stderr, "\t<name>\\t<type>\\t<rpi>\\t<tag string>\n");
    // NOLINTNEXTLINE
    fprintf(stderr, "\t<name> = a name used when outputting the data.\n");
    // NOLINTNEXTLINE
    fprintf(stderr, "\t<type> = The type of the tag.  One of 'dint', 'int', 'sint', 'real'.\n");
    // NOLINTNEXTLINE
    fprintf(stderr, "\t<rpi> = The number of milliseconds between reads of the tag.\n");
    // NOLINTNEXTLINE
    fprintf(stderr, "\t<tag string> = The tag attribute string for this tag.  E.g.:\n");
    // NOLINTNEXTLINE
    fprintf(stderr, "\t\tprotocol=ab-eip&gateway=10.206.1.40&path=1,4&cpu=lgx&elem_size=4&elem_count=10&name=TestDINTArray[0]\n");
    // NOLINTNEXTLINE
    fprintf(stderr, "Example:\n");
    // NOLINTNEXTLINE
    fprintf(
        stderr,
        "TestData\tdint\t100\tprotocol=ab-eip&gateway=10.206.1.40&path=1,4&cpu=lgx&elem_size=4&elem_count=10&name=TestDINTArray[0]\n");
}


int main(int argc, char **argv) {
    int rc;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    /* set up signal handler first. */
    compat_set_interrupt_handler(interrupt_handler);

    /* clear the array of tags. */
    // NOLINTNEXTLINE
    memset(&tags, 0, sizeof(tags));

    if(argc < 2) {
        usage();

        return 1;
    }

    if((rc = read_config(argv[1])) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Unable to read config or set up tags. %s!\n", plc_tag_decode_error(rc));
        destroy_tags();
        return 1;
    }

    /* wait for all tags to be ready */
    while((rc = check_tags()) == PLCTAG_STATUS_PENDING) { compat_sleep_ms(10, NULL); }

    if(rc != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Error waiting for tags to finish being set up, %s!\n", plc_tag_decode_error(rc));
        destroy_tags();
        return 1;
    }

    while(!terminate) {
        int num_tags_read = 0;
        int64_t start, end;

        start = compat_time_ms();

        rc = start_reads();

        if(rc == PLCTAG_STATUS_OK) {
            /* reads kicked off successfully */

            /* wait for the reads to complete */
            while((rc = check_tags()) == PLCTAG_STATUS_PENDING) { compat_sleep_ms(10, NULL); }

            end = compat_time_ms();

            if(rc == PLCTAG_STATUS_OK) {
                /* tags are ready. */

                for(int t = 0; t < num_tags; t++) {
                    if(tags[t].reading) { num_tags_read++; }
                }

                if(num_tags_read > 0) { printf("Read %d tags in %dms\n", num_tags_read, (int)(end - start)); }

                rc = log_data();
            }
        }

        if(rc != PLCTAG_STATUS_OK) {
            /* delay a long delay to let the library reconnect. */
            compat_sleep_ms(RECONNECT_DELAY_MS, NULL);
        } else {
            /* delay a tiny bit. */
            compat_sleep_ms(10, NULL);
        }
    }

    printf("Terminating!\n");

    destroy_tags();

    return 0;
}

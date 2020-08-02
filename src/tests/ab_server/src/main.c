/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
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

#include "compat.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(IS_WINDOWS)
#include <Windows.h>
#else
 /* assume it is POSIX of some sort... */
#include <signal.h>
#include <strings.h>
#endif

#include "eip.h"
#include "plc.h"
#include "slice.h"
#include "tcp_server.h"
#include "utils.h"

static void usage(void);
static void process_args(int argc, const char **argv, plc_s *plc);
static void parse_path(const char *path, plc_s *plc);
static void parse_pccc_tag(const char *tag, plc_s *plc);
static void parse_cip_tag(const char *tag, plc_s *plc);
static slice_s request_handler(slice_s input, slice_s output, void *plc);


#ifdef IS_WINDOWS

typedef volatile int sig_flag_t;

sig_flag_t done = 0;

/* straight from MS' web site :-) */
int WINAPI CtrlHandler(DWORD fdwCtrlType)
{
    switch (fdwCtrlType)
    {
        // Handle the CTRL-C signal.
    case CTRL_C_EVENT:
        info("^C event");
        done = 1;
        return TRUE;

        // CTRL-CLOSE: confirm that the user wants to exit.
    case CTRL_CLOSE_EVENT:
        info("Close event");
        done = 1;
        return TRUE;

        // Pass other signals to the next handler.
    case CTRL_BREAK_EVENT:
        info("^Break event");
        done = 1;
        return TRUE;

    case CTRL_LOGOFF_EVENT:
        info("Logoff event");
        done = 1;
        return TRUE;

    case CTRL_SHUTDOWN_EVENT:
        info("Shutdown event");
        done = 1;
        return TRUE;

    default:
        info("Default Event: %d", fdwCtrlType);
        return FALSE;
    }
}


void setup_break_handler(void)
{
    if (!SetConsoleCtrlHandler(CtrlHandler, TRUE))
    {
        printf("\nERROR: Could not set control handler!\n");
        usage();
    }
}

#else

typedef volatile sig_atomic_t sig_flag_t;

sig_flag_t done = 0;

void SIGINT_handler(int not_used)
{
    (void)not_used;

    done = 1;
}

void setup_break_handler(void)
{
    struct sigaction act;

    /* set up signal handler. */
    memset(&act, 0, sizeof(act));
    act.sa_handler = SIGINT_handler;
    sigaction(SIGINT, &act, NULL);
}

#endif


int main(int argc, const char **argv)
{
    tcp_server_p server = NULL;
    uint8_t buf[4200];  /* CIP only allows 4002 for the CIP request, but there is overhead. */
    slice_s server_buf = slice_make(buf, sizeof(buf));
    plc_s plc;

    /* set up handler for ^C etc. */
    setup_break_handler();

    debug_off();

    /* clear out context to make sure we do not get gremlins */
    memset(&plc, 0, sizeof(plc));

    /* set the random seed. */
    srand((unsigned int)time(NULL));

    process_args(argc, argv, &plc);

    /* open a server connection and listen on the right port. */
    server = tcp_server_create("0.0.0.0", "44818", server_buf, request_handler, &plc);

    tcp_server_start(server, &done);

    tcp_server_destroy(server);

    return 0;
}


void usage(void)
{
    fprintf(stderr, "Usage: ab_server --plc=<plc_type> [--path=<path>] --tag=<tag>\n"
                    "   <plc type> = one of the CIP PLCs: \"ControlLogix\", \"Micro800\" or \"Omron\",\n"
                    "                or one of the PCCC PLCs: \"PLC/5\", \"SLC500\" or \"Micrologix\".\n"
                    "\n"
                    "   <path> = (required for ControlLogix) internal path to CPU in PLC.  E.g. \"1,0\".\n"
                    "\n"
                    "    PCCC-based PLC tags are in the format: <file>[<size>] where:\n"
                    "        <file> is the data file, only the following are supported:\n"
                    "            N7 - 2-byte signed integer.\n"
                    "            F8 - 4-byte floating point number.\n"
                    "            L19 - 4-byte signed integer.\n"
                    "\n"
                    "        <size> field is the length of the data file.\n"
                    "\n"
                    "    CIP-based PLC tags are in the format: <name>:<type>[<sizes>] where:\n"
                    "        <name> is alphanumeric, starting with an alpha character.\n"
                    "        <type> is one of:\n"
                    "            INT - 2-byte signed integer.  Requires array size(s).\n"
                    "            DINT - 4-byte signed integer.  Requires array size(s).\n"
                    "            LINT - 8-byte signed integer.  Requires array size(s).\n"
                    "            REAL - 4-byte floating point number.  Requires array size(s).\n"
                    "            LREAL - 8-byte floating point number.  Requires array size(s).\n"
                    "\n"
                    "        <sizes>> field is one or more (up to 3) numbers separated by commas.\n"
                    "\n"
                    "Example: ab_server --plc=ControlLogix --path=1,0 --tag=MyTag:DINT[10,10]\n");

    exit(1);
}


void process_args(int argc, const char **argv, plc_s *plc)
{
    bool has_path = false;
    bool needs_path = false;
    bool has_plc = false;
    bool has_tag = false;

    /* make sure that the reject FO count is zero. */
    plc->reject_fo_count = 0;

    for(int i=0; i < argc; i++) {
        if(strncmp(argv[i],"--plc=",6) == 0) {
            if(has_plc) {
                fprintf(stderr, "PLC type can only be specified once!\n");
                usage();
            }

            if(str_cmp_i(&(argv[i][6]), "ControlLogix") == 0) {
                fprintf(stderr, "Selecting ControlLogix simulator.\n");
                plc->plc_type = PLC_CONTROL_LOGIX;
                plc->path[0] = (uint8_t)0x00; /* filled in later. */
                plc->path[1] = (uint8_t)0x00; /* filled in later. */
                plc->path[2] = (uint8_t)0x20;
                plc->path[3] = (uint8_t)0x02;
                plc->path[4] = (uint8_t)0x24;
                plc->path[5] = (uint8_t)0x01;
                plc->path_len = 6;
                plc->client_to_server_max_packet = 508;
                plc->server_to_client_max_packet = 508;
                needs_path = true;
                has_plc = true;
            } else if(str_cmp_i(&(argv[i][6]), "Micro800") == 0) {
                fprintf(stderr, "Selecting Micro8xx simulator.\n");
                plc->plc_type = PLC_MICRO800;
                plc->path[0] = (uint8_t)0x20;
                plc->path[1] = (uint8_t)0x02;
                plc->path[2] = (uint8_t)0x24;
                plc->path[3] = (uint8_t)0x01;
                plc->path_len = 4;
                plc->client_to_server_max_packet = 508;
                plc->server_to_client_max_packet = 508;
                needs_path = false;
                has_plc = true;
            } else if(str_cmp_i(&(argv[i][6]), "Omron") == 0) {
                fprintf(stderr, "Selecting Omron NJ/NX simulator.\n");
                plc->plc_type = PLC_OMRON;
                plc->path[0] = (uint8_t)0x12;  /* Extended segment, port A */
                plc->path[1] = (uint8_t)0x09;  /* 9 bytes length. */
                plc->path[2] = (uint8_t)0x31;  /* '1' */
                plc->path[3] = (uint8_t)0x32;  /* '2' */
                plc->path[4] = (uint8_t)0x37;  /* '7' */
                plc->path[5] = (uint8_t)0x2e;  /* '.' */
                plc->path[6] = (uint8_t)0x30;  /* '0' */
                plc->path[7] = (uint8_t)0x2e;  /* '.' */
                plc->path[8] = (uint8_t)0x30;  /* '0' */
                plc->path[9] = (uint8_t)0x2e;  /* '.' */
                plc->path[10] = (uint8_t)0x31; /* '1' */
                plc->path[11] = (uint8_t)0x00; /* padding */
                plc->path[12] = (uint8_t)0x20;
                plc->path[13] = (uint8_t)0x02;
                plc->path[14] = (uint8_t)0x24;
                plc->path[15] = (uint8_t)0x01;
                plc->path_len = 16;
                plc->client_to_server_max_packet = 508;
                plc->server_to_client_max_packet = 508;
                needs_path = false;
                has_plc = true;
            } else if(str_cmp_i(&(argv[i][6]), "PLC/5") == 0) {
                fprintf(stderr, "Selecting PLC/5 simulator.\n");
                plc->plc_type = PLC_PLC5;
                plc->path[0] = (uint8_t)0x20;
                plc->path[1] = (uint8_t)0x02;
                plc->path[2] = (uint8_t)0x24;
                plc->path[3] = (uint8_t)0x01;
                plc->path_len = 4;
                plc->client_to_server_max_packet = 244;
                plc->server_to_client_max_packet = 244;
                needs_path = false;
                has_plc = true;
            } else if(str_cmp_i(&(argv[i][6]), "SLC500") == 0) {
                fprintf(stderr, "Selecting SLC 500 simulator.\n");
                plc->plc_type = PLC_SLC;
                plc->path[0] = (uint8_t)0x20;
                plc->path[1] = (uint8_t)0x02;
                plc->path[2] = (uint8_t)0x24;
                plc->path[3] = (uint8_t)0x01;
                plc->path_len = 4;
                plc->client_to_server_max_packet = 244;
                plc->server_to_client_max_packet = 244;
                needs_path = false;
                has_plc = true;
            } else if(str_cmp_i(&(argv[i][6]), "Micrologix") == 0) {
                fprintf(stderr, "Selecting Micrologix simulator.\n");
                plc->plc_type = PLC_MICROLOGIX;
                plc->path[0] = (uint8_t)0x20;
                plc->path[1] = (uint8_t)0x02;
                plc->path[2] = (uint8_t)0x24;
                plc->path[3] = (uint8_t)0x01;
                plc->path_len = 4;
                plc->client_to_server_max_packet = 244;
                plc->server_to_client_max_packet = 244;
                needs_path = false;
                has_plc = true;
            } else if(str_cmp_i(&(argv[i][6]), "SLC500") == 0) {
                fprintf(stderr, "Selecting SLC 500 simulator.\n");
                plc->plc_type = PLC_SLC;
                plc->path[0] = (uint8_t)0x20;  
                plc->path[1] = (uint8_t)0x02;
                plc->path[2] = (uint8_t)0x24; 
                plc->path[3] = (uint8_t)0x01;
                plc->path_len = 4;
                plc->client_to_server_max_packet = 244;
                plc->server_to_client_max_packet = 244;
                needs_path = false;
                has_plc = true;
            } else if(str_cmp_i(&(argv[i][6]), "Micrologix") == 0) {
                fprintf(stderr, "Selecting Micrologix simulator.\n");
                plc->plc_type = PLC_MICROLOGIX;
                plc->path[0] = (uint8_t)0x20;  
                plc->path[1] = (uint8_t)0x02;
                plc->path[2] = (uint8_t)0x24; 
                plc->path[3] = (uint8_t)0x01;
                plc->path_len = 4;
                plc->client_to_server_max_packet = 244;
                plc->server_to_client_max_packet = 244;
                needs_path = false;
                has_plc = true;
            } else {
                fprintf(stderr, "Unsupported PLC type %s!\n", &(argv[i][6]));
                usage();
            }
        }

        if(strncmp(argv[i],"--path=",7) == 0) {
            parse_path(&(argv[i][7]), plc);
            has_path = true;
        }

        if(strncmp(argv[i],"--tag=",6) == 0) {
            if(plc && (plc->plc_type == PLC_PLC5 || plc->plc_type == PLC_SLC || plc->plc_type == PLC_MICROLOGIX)) {
                parse_pccc_tag(&(argv[i][6]), plc);
            } else {
                parse_cip_tag(&(argv[i][6]), plc);
            }
            has_tag = true;
        }

        if(strcmp(argv[i],"--debug") == 0) {
            debug_on();
        }

        if(strncmp(argv[i],"--reject_fo=", 12) == 0) {
            if(plc) {
                info("Setting reject ForwardOpen count to %d.", atoi(&argv[i][12]));
                plc->reject_fo_count = atoi(&argv[i][12]);
            }
        }
    }

    if(needs_path && !has_path) {
        fprintf(stderr, "This PLC type requires a path argument.\n");
        usage();
    }

    if(!has_plc) {
        fprintf(stderr, "You must pass a --plc= argument!\n");
        usage();
    }

    if(!has_tag) {
        fprintf(stderr, "You must define at least one tag.\n");
        usage();
    }
}


void parse_path(const char *path_str, plc_s *plc)
{
    int tmp_path[2];

    if (str_scanf(path_str, "%d,%d", &tmp_path[0], &tmp_path[1]) == 2) {
        plc->path[0] = (uint8_t)tmp_path[0];
        plc->path[1] = (uint8_t)tmp_path[1];

        info("Processed path %d,%d.", plc->path[0], plc->path[1]);
    } else {
        fprintf(stderr, "Error processing path \"%s\"!  Path must be two numbers separated by a comma.\n", path_str);
        usage();
    }
}




/*
 * PCCC tags are in the format:
 *    <data file>[<size>]
 *
 * Where data file is one of the following:
 *     N7 - 2 byte signed integer.  Requires size.
 *     F8 - 4-byte floating point number.   Requires size.
 *     L19 - 4 byte signed integer.   Requires size.
 *
 * The size field is a single positive integer.
 */

void parse_pccc_tag(const char *tag_str, plc_s *plc)
{
    tag_def_s *tag = calloc(1, sizeof(*tag));
    char data_file_name[200] = { 0 };
    char size_str[200] = { 0 };
    int num_dims = 0;
    size_t start = 0;
    size_t len = 0;

    info("Starting.");

    if(!tag) {
        error("Unable to allocate memory for new tag!");
    }

    /* try to match the two parts of a tag definition string. */

    info("Match data file.");

    /* first match the data file. */
    start = 0;
    len = strspn(tag_str + start, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    if (!len) {
        fprintf(stderr, "Unable to parse tag definition string, cannot find tag name in \"%s\"!\n", tag_str);
        usage();
    } else {
        /* copy the string. */
        for (size_t i = 0; i < len && i < (size_t)200; i++) {
            data_file_name[i] = tag_str[start + i];
        }

        /* check data file for a match. */
        if(str_cmp_i(data_file_name, "N7") == 0) {
            info("Found N7 data file.");
            tag->tag_type = TAG_PCCC_TYPE_INT;
            tag->elem_size = 2;
            tag->data_file_num = 7;
        } else if(str_cmp_i(data_file_name, "F8") == 0) {
            info("Found N7 data file.");
            tag->tag_type = TAG_PCCC_TYPE_REAL;
            tag->elem_size = 4;
            tag->data_file_num = 8;
        } else if(str_cmp_i(data_file_name, "L19") == 0) {
            info("Found N7 data file.");
            tag->tag_type = TAG_PCCC_TYPE_DINT;
            tag->elem_size = 4;
            tag->data_file_num = 19;
        } else {
            fprintf(stderr, "Unknown data file %s, unable to create tag!", data_file_name);
            usage();
        }

        start += len;
    }

    /* get the array size delimiter. */
    if (tag_str[start] != '[') {
        fprintf(stderr, "Unable to parse tag definition string, cannot find starting square bracket after data file in \"%s\"!\n", tag_str);
        usage();
    } else {
        start++;
    }

    /* get the size field */
    len = strspn(tag_str + start, "0123456789");
    if (!len) {
        fprintf(stderr, "Unable to parse tag definition string, cannot match array size in \"%s\"!\n", tag_str);
        usage();
    } else {
        /* copy the string. */
        for (size_t i = 0; i < len && i < (size_t)200; i++) {
            size_str[i] = tag_str[start + i];
        }

        start += len;
    }

    if (tag_str[start] != ']') {
        fprintf(stderr, "Unable to parse tag definition string, cannot find ending square bracket after size in \"%s\"!\n", tag_str);
        usage();
    }

    /* make sure all the dimensions are defaulted to something sane. */
    tag->dimensions[0] = 1;
    tag->dimensions[1] = 1;
    tag->dimensions[2] = 1;

    /* match the size. */
    num_dims = str_scanf(size_str, "%zu", &tag->dimensions[0]);
    if(num_dims != 1) {
        fprintf(stderr, "Unable to parse tag size in \"%s\"!\n", tag_str);
        usage();
    }

    /* check the size. */
    if(tag->dimensions[0] <= 0) {
        fprintf(stderr, "The array size must least 1 and may not be negative!\n");
        usage();
    } else {
        tag->elem_count = tag->dimensions[0];
        tag->num_dimensions = 1;
    }
    /* copy the tag name */
    tag->name = strdup(data_file_name);
    if (!tag->name) {
        fprintf(stderr, "Unable to allocate a copy of the data file \"%s\"!\n", data_file_name);
        usage();
    }

    /* allocate the tag data array. */
    info("allocating %d elements of %d bytes each.", tag->elem_count, tag->elem_size);
    tag->data = calloc(tag->elem_count, (size_t)tag->elem_size);
    if(!tag->data) {
        fprintf(stderr, "Unable to allocate tag data buffer!\n");
        free(tag->name);
    }

    info("Processed \"%s\" into tag %s of type %x with dimensions (%d, %d, %d).", tag_str, tag->name, tag->tag_type, tag->dimensions[0], tag->dimensions[1], tag->dimensions[2]);

    /* add the tag to the list. */
    tag->next_tag = plc->tags;
    plc->tags = tag;
}




/*
 * CIP tags are in the format:
 *    <name>:<type>[<sizes>]
 *
 * Where name is alphanumeric, starting with an alpha character.
 *
 * Type is one of:
 *     INT - 2-byte signed integer.  Requires array size(s).
 *     DINT - 4-byte signed integer.  Requires array size(s).
 *     LINT - 8-byte signed integer.  Requires array size(s).
 *     REAL - 4-byte floating point number.  Requires array size(s).
 *     LREAL - 8-byte floating point number.  Requires array size(s).
 *
 * Array size field is one or more (up to 3) numbers separated by commas.
 */

void parse_cip_tag(const char *tag_str, plc_s *plc)
{
    tag_def_s *tag = calloc(1, sizeof(*tag));
    char tag_name[200] = { 0 };
    char type_str[200] = { 0 };
    char dim_str[200] = { 0 };
    int num_dims = 0;
    size_t start = 0;
    size_t len = 0;

    if(!tag) {
        error("Unable to allocate memory for new tag!");
    }

    /* try to match the three parts of a tag definition string. */

    /* first match the name. */
    start = 0;
    len = strspn(tag_str + start, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");
    if (!len) {
        fprintf(stderr, "Unable to parse tag definition string, cannot find tag name in \"%s\"!\n", tag_str);
        usage();
    } else {
        /* copy the string. */
        for (size_t i = 0; i < len && i < (size_t)200; i++) {
            tag_name[i] = tag_str[start + i];
        }

        start += len;
    }

    if(tag_str[start] != ':') {
        fprintf(stderr, "Unable to parse tag definition string, cannot find colon after tag name in \"%s\"!\n", tag_str);
        usage();
    } else {
        start++;
    }

    /* get the type field */
    len = strspn(tag_str + start, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
    if (!len) {
        fprintf(stderr, "Unable to parse tag definition string, cannot match tag type in \"%s\"!\n", tag_str);
        usage();
    } else {
        /* copy the string. */
        for (size_t i = 0; i < len && i < (size_t)200; i++) {
            type_str[i] = tag_str[start + i];
        }

        start += len;
    }

    if (tag_str[start] != '[') {
        fprintf(stderr, "Unable to parse tag definition string, cannot find starting square bracket after tag type in \"%s\"!\n", tag_str);
        usage();
    } else {
        start++;
    }

    /* get the dimension field */
    len = strspn(tag_str + start, "0123456789,");
    if (!len) {
        fprintf(stderr, "Unable to parse tag definition string, cannot match dimension in \"%s\"!\n", tag_str);
        usage();
    } else {
        /* copy the string. */
        for (size_t i = 0; i < len && i < (size_t)200; i++) {
            dim_str[i] = tag_str[start + i];
        }

        start += len;
    }

    if (tag_str[start] != ']') {
        fprintf(stderr, "Unable to parse tag definition string, cannot find ending square bracket after tag type in \"%s\"!\n", tag_str);
        usage();
    }

    /* match the type. */
    if(str_cmp_i(type_str, "SINT") == 0) {
        tag->tag_type = TAG_CIP_TYPE_SINT;
        tag->elem_size = 1;
    } else if(str_cmp_i(type_str, "INT") == 0) {
        tag->tag_type = TAG_CIP_TYPE_INT;
        tag->elem_size = 2;
    } else if(str_cmp_i(type_str, "DINT") == 0) {
        tag->tag_type = TAG_CIP_TYPE_DINT;
        tag->elem_size = 4;
    } else if(str_cmp_i(type_str, "LINT") == 0) {
        tag->tag_type = TAG_CIP_TYPE_LINT;
        tag->elem_size = 8;
    } else if(str_cmp_i(type_str, "REAL") == 0) {
        tag->tag_type = TAG_CIP_TYPE_REAL;
        tag->elem_size = 4;
    } else if(str_cmp_i(type_str, "LREAL") == 0) {
        tag->tag_type = TAG_CIP_TYPE_LREAL;
        tag->elem_size = 8;
    } else {
        fprintf(stderr, "Unsupported tag type \"%s\"!", type_str);
        usage();
    }

    /* match the dimensions. */
    tag->dimensions[0] = 0;
    tag->dimensions[1] = 0;
    tag->dimensions[2] = 0;

    num_dims = str_scanf(dim_str, "%zu,%zu,%zu,%*u", &tag->dimensions[0], &tag->dimensions[1], &tag->dimensions[2]);
    if(num_dims < 1 || num_dims > 3) {
        fprintf(stderr, "Tag dimensions must have at least one dimension non-zero and no more than three dimensions.");
        usage();
    }

    /* check the dimensions. */
    if(tag->dimensions[0] <= 0) {
        fprintf(stderr, "The first tag dimension must be at least 1 and may not be negative!\n");
        usage();
    } else {
        tag->elem_count = tag->dimensions[0];
        tag->num_dimensions = 1;
    }

    if(tag->dimensions[1] > 0) {
        tag->elem_count *= tag->dimensions[1];
        tag->num_dimensions = 2;
    } else {
        tag->dimensions[1] = 1;
    }

    if(tag->dimensions[2] > 0) {
        tag->elem_count *= tag->dimensions[2];
        tag->num_dimensions = 3;
    } else {
        tag->dimensions[2] = 1;
    }
    /* copy the tag name */
    tag->name = strdup(tag_name);
    if (!tag->name) {
        fprintf(stderr, "Unable to allocate a copy of the tag name \"%s\"!\n", tag_name);
        usage();
    }

    /* allocate the tag data array. */
    info("allocating %d elements of %d bytes each.", tag->elem_count, tag->elem_size);
    tag->data = calloc(tag->elem_count, (size_t)tag->elem_size);
    if(!tag->data) {
        fprintf(stderr, "Unable to allocate tag data buffer!\n");
        free(tag->name);
    }

    info("Processed \"%s\" into tag %s of type %x with dimensions (%d, %d, %d).", tag_str, tag->name, tag->tag_type, tag->dimensions[0], tag->dimensions[1], tag->dimensions[2]);

    /* add the tag to the list. */
    tag->next_tag = plc->tags;
    plc->tags = tag;
}


/*
 * Process each request.  Dispatch to the correct
 * request type handler.
 */

slice_s request_handler(slice_s input, slice_s output, void *plc)
{
    /* check to see if we have a full packet. */
    if(slice_len(input) >= EIP_HEADER_SIZE) {
        uint16_t eip_len = slice_get_uint16_le(input, 2);

        if(slice_len(input) >= (size_t)(EIP_HEADER_SIZE + eip_len)) {
            return eip_dispatch_request(input, output, (plc_s *)plc);
        }
    }

    /* we do not have a complete packet, get more data. */
    return slice_make_err(TCP_SERVER_INCOMPLETE);
}

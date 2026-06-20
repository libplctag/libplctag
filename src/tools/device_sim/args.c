/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever    *
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

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <libplctag/lib/libplctag.h>
#include "platform.h"
#include "args.h"
#include "device.h"

/* ============================================================================
 * CIP type table
 * ============================================================================ */

typedef struct {
    const char *name;
    tag_type_t  type;
    size_t      elem_size;
} cip_type_entry_t;

static const cip_type_entry_t CIP_TYPES[] = {
    {"BOOL",  TAG_CIP_TYPE_BOOL,  1},
    {"SINT",  TAG_CIP_TYPE_SINT,  1},
    {"INT",   TAG_CIP_TYPE_INT,   2},
    {"DINT",  TAG_CIP_TYPE_DINT,  4},
    {"LINT",  TAG_CIP_TYPE_LINT,  8},
    {"REAL",  TAG_CIP_TYPE_REAL,  4},
    {"LREAL", TAG_CIP_TYPE_LREAL, 8},
    {NULL,    0,                  0},
};

/* ============================================================================
 * PCCC type table  (letter, CIP-equivalent type code, elem_size)
 * ============================================================================ */

typedef struct {
    char       letter;
    tag_type_t type;
    size_t     elem_size;
} pccc_type_entry_t;

static const pccc_type_entry_t PCCC_TYPES[] = {
    {'B', TAG_PCCC_TYPE_BIT,  2},
    {'N', TAG_PCCC_TYPE_INT,  2},
    {'L', TAG_PCCC_TYPE_DINT, 4},
    {'F', TAG_PCCC_TYPE_REAL, 4},
    {'R', TAG_PCCC_TYPE_REAL, 4},
    {0,   0,                  0},
};

/* ============================================================================
 * String helpers (no string.h)
 * ============================================================================ */

/* Return pointer to the character after prefix if arg starts with prefix, else NULL. */
static const char *find_prefix(const char *arg, const char *prefix) {
    while(*prefix && *arg == *prefix) { arg++; prefix++; }
    return (*prefix == '\0') ? arg : NULL;
}

/* Find first occurrence of c in s. Returns pointer to it or NULL. */
static const char *find_char(const char *s, char c) {
    while(*s && *s != c) { s++; }
    return *s ? s : NULL;
}

/* Case-insensitive equality of two null-terminated strings. */
static bool str_eq_i(const char *a, const char *b) {
    while(*a && *b) {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
        if(ca != cb) { return false; }
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Case-insensitive equality with explicit lengths. */
static bool str_neq_i(const char *a, size_t alen, const char *b, size_t blen) {
    if(alen != blen) { return false; }
    for(size_t i = 0; i < alen; i++) {
        char ca = (a[i] >= 'a' && a[i] <= 'z') ? (char)(a[i] - 32) : a[i];
        char cb = (b[i] >= 'a' && b[i] <= 'z') ? (char)(b[i] - 32) : b[i];
        if(ca != cb) { return false; }
    }
    return true;
}

/*
 * Parse a decimal integer starting at *s.
 * Advances *s past consumed digits.
 * Returns false if no digits were present.
 */
static bool parse_digits(const char **s, int32_t *out) {
    if(**s < '0' || **s > '9') { return false; }
    int32_t val = 0;
    while(**s >= '0' && **s <= '9') {
        val = val * 10 + (**s - '0');
        (*s)++;
    }
    *out = val;
    return true;
}

/* ============================================================================
 * Scalar argument parsers
 * ============================================================================ */

static uint16_t parse_uint16_val(const char *s, uint16_t def) {
    if(!s || *s == '\0') { return def; }
    int32_t v = 0;
    if(!parse_digits(&s, &v) || v <= 0 || v > 65535) { return def; }
    return (uint16_t)v;
}

static int32_t parse_int32_val(const char *s, int32_t def) {
    if(!s || *s == '\0') { return def; }
    bool neg = (*s == '-');
    if(neg) { s++; }
    int32_t v = 0;
    if(!parse_digits(&s, &v)) { return def; }
    return neg ? -v : v;
}

/* ============================================================================
 * PLC type parser
 * ============================================================================ */

static plc_type_t parse_plc_type(const char *s) {
    if(!s || *s == '\0') { return PLC_CONTROL_LOGIX; }
    if(str_eq_i(s, "ControlLogix") || str_eq_i(s, "logix") || str_eq_i(s, "LGX")) {
        return PLC_CONTROL_LOGIX;
    }
    if(str_eq_i(s, "Micro800") || str_eq_i(s, "micro800")) {
        return PLC_MICRO800;
    }
    if(str_eq_i(s, "Omron") || str_eq_i(s, "omron-njnx") || str_eq_i(s, "omron_njnx")) {
        return PLC_OMRON;
    }
    if(str_eq_i(s, "PLC5") || str_eq_i(s, "PLC/5")) {
        return PLC_PLC5;
    }
    if(str_eq_i(s, "SLC") || str_eq_i(s, "SLC500")) {
        return PLC_SLC;
    }
    if(str_eq_i(s, "Micrologix") || str_eq_i(s, "MicroLogix")) {
        return PLC_MICROLOGIX;
    }
    fprintf(stderr, "device_sim: unknown PLC type '%s', defaulting to ControlLogix.\n", s);
    return PLC_CONTROL_LOGIX;
}

/* ============================================================================
 * Tag spec parsers
 * ============================================================================ */

/* Allocate and copy a name string of length name_len (not null-terminated in src). */
static char *alloc_name(const char *src, size_t name_len) {
    char *name = (char *)mem_alloc((int)(name_len + 1));
    if(!name) { return NULL; }
    mem_copy(name, (void*)src, (int)name_len);
    name[name_len] = '\0';
    return name;
}

/*
 * Parse CIP dimensions from "[d1]", "[d1,d2]", or "[d1,d2,d3]".
 * Returns false on parse error.
 */
static bool parse_dims(const char *s, size_t *num_dim_out, size_t dims[3]) {
    *num_dim_out = 0;
    dims[0] = dims[1] = dims[2] = 0;

    if(*s != '[') {
        fprintf(stderr, "device_sim: expected '[' in tag dimensions, got '%c'.\n", *s);
        return false;
    }
    s++;

    for(int32_t d = 0; d < 3; d++) {
        int32_t v = 0;
        if(!parse_digits(&s, &v) || v <= 0) {
            fprintf(stderr, "device_sim: invalid dimension value in tag spec.\n");
            return false;
        }
        dims[(size_t)d] = (size_t)v;
        (*num_dim_out)++;

        if(*s == ']') { s++; break; }
        if(*s == ',') { s++; continue; }
        fprintf(stderr, "device_sim: expected ',' or ']' in tag spec, got '%c'.\n", *s);
        return false;
    }

    if(*num_dim_out == 0) {
        fprintf(stderr, "device_sim: zero dimensions in tag spec.\n");
        return false;
    }
    return true;
}

/*
 * Parse a CIP tag: "Name:TYPE[d1]" or "Name:TYPE[d1,d2,d3]"
 */
static tag_def_t *parse_cip_tag(const char *spec) {
    const char *colon = find_char(spec, ':');
    if(!colon || colon == spec) {
        fprintf(stderr, "device_sim: CIP tag spec '%s' missing name or ':'.\n", spec);
        return NULL;
    }

    size_t name_len = (size_t)(colon - spec);
    const char *type_start = colon + 1;
    const char *bracket = find_char(type_start, '[');
    if(!bracket || bracket == type_start) {
        fprintf(stderr, "device_sim: CIP tag spec '%s' missing type or '['.\n", spec);
        return NULL;
    }

    size_t type_len = (size_t)(bracket - type_start);

    const cip_type_entry_t *entry = CIP_TYPES;
    while(entry->name) {
        if(str_neq_i(type_start, type_len, entry->name, (size_t)str_length(entry->name))) { break; }
        entry++;
    }
    if(!entry->name) {
        fprintf(stderr, "device_sim: unknown CIP type '%.*s' in tag spec '%s'.\n",
                (int)type_len, type_start, spec);
        return NULL;
    }

    size_t num_dim = 0;
    size_t dims[3] = {0, 0, 0};
    if(!parse_dims(bracket, &num_dim, dims)) { return NULL; }

    size_t elem_count = 1;
    for(size_t d = 0; d < num_dim; d++) { elem_count *= dims[d]; }

    tag_def_t *tag = (tag_def_t *)mem_alloc((int)sizeof(tag_def_t));
    if(!tag) { return NULL; }
    mem_set(tag, 0, (int)sizeof(tag_def_t));

    tag->name = alloc_name(spec, name_len);
    if(!tag->name) { mem_free(tag); return NULL; }

    tag->tag_type      = entry->type;
    tag->elem_size     = entry->elem_size;
    tag->elem_count    = elem_count;
    tag->num_dimensions = num_dim;
    tag->dimensions[0] = dims[0];
    tag->dimensions[1] = dims[1];
    tag->dimensions[2] = dims[2];

    tag->data = (uint8_t *)mem_alloc((int)(elem_count * entry->elem_size));
    if(!tag->data) { mem_free(tag->name); mem_free(tag); return NULL; }
    mem_set(tag->data, 0, (int)(elem_count * entry->elem_size));

    if(mutex_create(&tag->data_mutex) != PLCTAG_STATUS_OK) {
        mem_free(tag->data); mem_free(tag->name); mem_free(tag);
        return NULL;
    }

    return tag;
}

/*
 * Parse a PCCC tag: "B3[10]", "N7[10]", "L19[10]", etc.
 * Format: <letter><file_num>[<count>]
 */
static tag_def_t *parse_pccc_tag(const char *spec) {
    const char *s = spec;

    /* Identify the type letter. */
    char letter = *s;
    if(letter >= 'a' && letter <= 'z') { letter = (char)(letter - 32); }
    const pccc_type_entry_t *entry = PCCC_TYPES;
    while(entry->letter && entry->letter != letter) { entry++; }
    if(!entry->letter) {
        fprintf(stderr, "device_sim: unknown PCCC type letter '%c' in tag spec '%s'.\n",
                *spec, spec);
        return NULL;
    }
    s++;

    /* Parse file number. */
    int32_t file_num = 0;
    if(!parse_digits(&s, &file_num) || file_num < 0) {
        fprintf(stderr, "device_sim: missing file number in PCCC tag spec '%s'.\n", spec);
        return NULL;
    }

    /* Parse element count. */
    size_t num_dim = 0;
    size_t dims[3] = {0, 0, 0};
    if(!parse_dims(s, &num_dim, dims)) { return NULL; }
    if(num_dim != 1) {
        fprintf(stderr, "device_sim: PCCC tags support only 1D arrays (got %zu dims).\n", num_dim);
        return NULL;
    }

    size_t elem_count = dims[0];

    /* Build name string like "B3". */
    char name_buf[32];
    size_t ni = 0;
    name_buf[ni++] = *spec; /* original letter (may be lowercase) */
    int32_t fn = file_num;
    if(fn == 0) {
        name_buf[ni++] = '0';
    } else {
        char digits[12];
        size_t di = 0;
        while(fn > 0) { digits[di++] = (char)('0' + fn % 10); fn /= 10; }
        /* digits is reversed */
        for(size_t r = di; r > 0; r--) { name_buf[ni++] = digits[r - 1]; }
    }
    name_buf[ni] = '\0';

    tag_def_t *tag = (tag_def_t *)mem_alloc((int)sizeof(tag_def_t));
    if(!tag) { return NULL; }
    mem_set(tag, 0, (int)sizeof(tag_def_t));

    tag->name = alloc_name(name_buf, ni);
    if(!tag->name) { mem_free(tag); return NULL; }

    tag->tag_type       = entry->type;
    tag->elem_size      = entry->elem_size;
    tag->elem_count     = elem_count;
    tag->data_file_num  = (size_t)file_num;
    tag->num_dimensions = 1;
    tag->dimensions[0]  = elem_count;

    tag->data = (uint8_t *)mem_alloc((int)(elem_count * entry->elem_size));
    if(!tag->data) { mem_free(tag->name); mem_free(tag); return NULL; }
    mem_set(tag->data, 0, (int)(elem_count * entry->elem_size));

    if(mutex_create(&tag->data_mutex) != PLCTAG_STATUS_OK) {
        mem_free(tag->data); mem_free(tag->name); mem_free(tag);
        return NULL;
    }

    return tag;
}

/* Dispatch to CIP or PCCC parser based on presence of ':' in spec. */
static tag_def_t *parse_tag_spec(const char *spec) {
    return find_char(spec, ':') ? parse_cip_tag(spec) : parse_pccc_tag(spec);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

extern void args_print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  --port=N         TCP port to listen on (default: 44818)\n"
        "  --bind=ADDR      Bind address (default: all interfaces)\n"
        "  --plc=TYPE       PLC personality: ControlLogix, Micro800, Omron,\n"
        "                   PLC5, SLC, Micrologix  (default: ControlLogix)\n"
        "  --tag=SPEC       Add a CIP tag:  Name:TYPE[count]  or multi-dim\n"
        "                                   Name:TYPE[d1,d2,d3]\n"
        "                   Add a PCCC tag: B<n>[count]  N<n>[count]\n"
        "                                   L<n>[count]  F<n>[count]\n"
        "                   TYPE: BOOL SINT INT DINT LINT REAL LREAL\n"
        "                   May be repeated.\n"
        "  --delay=MS       Artificial response delay in milliseconds\n"
        "  --debug=N        Debug level 1-5 (default: 2)\n"
        "  --help           Show this message\n"
        "\n"
        "Examples:\n"
        "  %s --tag=MyDINT:DINT[1] --tag=MyArray:DINT[100]\n"
        "  %s --plc=Micrologix --tag=B3[10] --tag=N7[10]\n",
        prog, prog, prog);
}


extern int32_t args_parse(int argc, char **argv, device_t *dev, int32_t *debug_level_out) {
    /* Defaults. */
    mem_set(dev, 0, (int)sizeof(device_t));
    dev->plc_type                    = PLC_CONTROL_LOGIX;
    dev->port                        = 44818;
    dev->bind_addr                   = NULL;
    dev->client_to_server_max_packet = 508;
    dev->server_to_client_max_packet = 508;
    dev->response_delay_ms           = 0;
    dev->tags                        = NULL;
    *debug_level_out                 = PLCTAG_DEBUG_WARN;

    tag_def_t *tail = NULL;

    for(int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *val;

        if((val = find_prefix(arg, "--port="))) {
            dev->port = parse_uint16_val(val, 44818);

        } else if((val = find_prefix(arg, "--bind="))) {
            dev->bind_addr = val; /* points into argv — valid for process lifetime */

        } else if((val = find_prefix(arg, "--plc="))) {
            dev->plc_type = parse_plc_type(val);

        } else if((val = find_prefix(arg, "--delay="))) {
            dev->response_delay_ms = parse_int32_val(val, 0);

        } else if((val = find_prefix(arg, "--debug="))) {
            *debug_level_out = parse_int32_val(val, PLCTAG_DEBUG_WARN);

        } else if((val = find_prefix(arg, "--tag="))) {
            tag_def_t *tag = parse_tag_spec(val);
            if(!tag) {
                fprintf(stderr, "device_sim: failed to parse --tag='%s'.\n", val);
                return PLCTAG_ERR_BAD_PARAM;
            }
            if(!dev->tags) { dev->tags = tag; } else { tail->next_tag = tag; }
            tail = tag;

        } else if(find_prefix(arg, "--help") == arg + 6 || find_prefix(arg, "-h") == arg + 2) {
            args_print_usage(argv[0]);
            return 1;

        } else {
            fprintf(stderr, "device_sim: unknown argument '%s'.\n", arg);
            return PLCTAG_ERR_BAD_PARAM;
        }
    }

    return PLCTAG_STATUS_OK;
}


extern void args_free_tags(tag_def_t *tag) {
    while(tag) {
        tag_def_t *next = tag->next_tag;
        if(tag->data_mutex) { mutex_destroy(&tag->data_mutex); }
        if(tag->data)       { mem_free(tag->data); }
        if(tag->name)       { mem_free(tag->name); }
        mem_free(tag);
        tag = next;
    }
}

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
#include <libplctag/protocols/enip/server/device.h>
#include <libplctag/protocols/enip/server/device_sim.h>

/* ============================================================================
 * CIP type table
 * ============================================================================ */

typedef struct {
    const char *name;
    tag_type_t  type;
} cip_type_entry_t;

static const cip_type_entry_t CIP_TYPES[] = {
    {"BOOL",  TAG_CIP_TYPE_BOOL},
    {"SINT",  TAG_CIP_TYPE_SINT},
    {"INT",   TAG_CIP_TYPE_INT},
    {"DINT",  TAG_CIP_TYPE_DINT},
    {"LINT",  TAG_CIP_TYPE_LINT},
    {"REAL",  TAG_CIP_TYPE_REAL},
    {"LREAL", TAG_CIP_TYPE_LREAL},
    {NULL,    0},
};

/* ============================================================================
 * PCCC type table
 * ============================================================================ */

typedef struct {
    char       letter;
    tag_type_t type;
} pccc_type_entry_t;

static const pccc_type_entry_t PCCC_TYPES[] = {
    {'B', TAG_PCCC_TYPE_BIT},
    {'N', TAG_PCCC_TYPE_INT},
    {'L', TAG_PCCC_TYPE_DINT},
    {'F', TAG_PCCC_TYPE_REAL},
    {'R', TAG_PCCC_TYPE_REAL},
    {0,   0},
};

/* ============================================================================
 * String helpers (no string.h)
 * ============================================================================ */

static const char *find_prefix(const char *arg, const char *prefix) {
    while(*prefix && *arg == *prefix) { arg++; prefix++; }
    return (*prefix == '\0') ? arg : NULL;
}

static const char *find_char(const char *s, char c) {
    while(*s && *s != c) { s++; }
    return *s ? s : NULL;
}

static bool str_neq_i(const char *a, size_t alen, const char *b, size_t blen) {
    if(alen != blen) { return false; }
    for(size_t i = 0; i < alen; i++) {
        char ca = (a[i] >= 'a' && a[i] <= 'z') ? (char)(a[i] - 32) : a[i];
        char cb = (b[i] >= 'a' && b[i] <= 'z') ? (char)(b[i] - 32) : b[i];
        if(ca != cb) { return false; }
    }
    return true;
}

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

static enip_plc_type_t parse_plc_type(const char *s) {
    if(!s || *s == '\0') { return ENIP_PLC_LGX; }
    if(str_cmp_i(s, "ControlLogix") == 0 || str_cmp_i(s, "logix") == 0 || str_cmp_i(s, "LGX") == 0) {
        return ENIP_PLC_LGX;
    }
    if(str_cmp_i(s, "Micro800") == 0) {
        return ENIP_PLC_MICRO800;
    }
    if(str_cmp_i(s, "Omron") == 0 || str_cmp_i(s, "omron-njnx") == 0 || str_cmp_i(s, "omron_njnx") == 0) {
        return ENIP_PLC_OMRON_NJNX;
    }
    if(str_cmp_i(s, "PLC5") == 0 || str_cmp_i(s, "PLC/5") == 0) {
        return ENIP_PLC_PLC5;
    }
    if(str_cmp_i(s, "SLC") == 0 || str_cmp_i(s, "SLC500") == 0) {
        return ENIP_PLC_SLC;
    }
    if(str_cmp_i(s, "Micrologix") == 0 || str_cmp_i(s, "MicroLogix") == 0) {
        return ENIP_PLC_MLGX;
    }
    fprintf(stderr, "device_sim: unknown PLC type '%s', defaulting to ControlLogix.\n", s);
    return ENIP_PLC_LGX;
}

/* ============================================================================
 * Dimension parser
 * ============================================================================ */

static bool parse_dims(const char *s, uint32_t *num_dim_out, uint32_t dims[3]) {
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
        dims[(size_t)d] = (uint32_t)v;
        (*num_dim_out)++;

        if(*s == ']') { break; }
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

/* ============================================================================
 * Tag spec parsers
 * ============================================================================ */

static int32_t parse_cip_tag(device_sim_t *sim, const char *spec) {
    const char *colon = find_char(spec, ':');
    if(!colon || colon == spec) {
        fprintf(stderr, "device_sim: CIP tag spec '%s' missing name or ':'.\n", spec);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* Extract name as null-terminated string. */
    size_t name_len = (size_t)(colon - spec);
    char name_buf[256];
    if(name_len >= sizeof(name_buf)) {
        fprintf(stderr, "device_sim: tag name too long in spec '%s'.\n", spec);
        return PLCTAG_ERR_BAD_PARAM;
    }
    mem_copy(name_buf, (void*)spec, (int)name_len);
    name_buf[name_len] = '\0';

    const char *type_start = colon + 1;
    const char *bracket = find_char(type_start, '[');
    if(!bracket || bracket == type_start) {
        fprintf(stderr, "device_sim: CIP tag spec '%s' missing type or '['.\n", spec);
        return PLCTAG_ERR_BAD_PARAM;
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
        return PLCTAG_ERR_BAD_PARAM;
    }

    uint32_t num_dim = 0;
    uint32_t dims[3] = {0, 0, 0};
    if(!parse_dims(bracket, &num_dim, dims)) { return PLCTAG_ERR_BAD_PARAM; }

    return device_sim_add_tag(sim, name_buf, entry->type, dims, num_dim,
                               NULL, NULL, NULL);
}


static int32_t parse_pccc_tag(device_sim_t *sim, const char *spec) {
    const char *s = spec;

    char letter = *s;
    if(letter >= 'a' && letter <= 'z') { letter = (char)(letter - 32); }

    const pccc_type_entry_t *entry = PCCC_TYPES;
    while(entry->letter && entry->letter != letter) { entry++; }
    if(!entry->letter) {
        fprintf(stderr, "device_sim: unknown PCCC type letter '%c' in tag spec '%s'.\n",
                *spec, spec);
        return PLCTAG_ERR_BAD_PARAM;
    }
    s++;

    int32_t file_num = 0;
    if(!parse_digits(&s, &file_num) || file_num < 0) {
        fprintf(stderr, "device_sim: missing file number in PCCC tag spec '%s'.\n", spec);
        return PLCTAG_ERR_BAD_PARAM;
    }

    uint32_t num_dim = 0;
    uint32_t dims[3] = {0, 0, 0};
    if(!parse_dims(s, &num_dim, dims)) { return PLCTAG_ERR_BAD_PARAM; }
    if(num_dim != 1) {
        fprintf(stderr, "device_sim: PCCC tags support only 1D arrays (got %u dims).\n", num_dim);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* Build name string: letter + file_num (e.g. "B3", "N7"). */
    char name_buf[32];
    size_t ni = 0;
    name_buf[ni++] = *spec;   /* original letter (may be lowercase) */
    int32_t fn = file_num;
    if(fn == 0) {
        name_buf[ni++] = '0';
    } else {
        char digits[12];
        size_t di = 0;
        while(fn > 0) { digits[di++] = (char)('0' + fn % 10); fn /= 10; }
        for(size_t r = di; r > 0; r--) { name_buf[ni++] = digits[r - 1]; }
    }
    name_buf[ni] = '\0';

    return device_sim_add_pccc_tag(sim, name_buf, entry->type,
                                    (uint32_t)file_num, dims[0],
                                    NULL, NULL, NULL);
}


static int32_t parse_tag_spec(device_sim_t *sim, const char *spec) {
    return find_char(spec, ':')
        ? parse_cip_tag(sim, spec)
        : parse_pccc_tag(sim, spec);
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


extern int32_t args_parse(int argc, char **argv, device_sim_t **sim_out, int32_t *debug_level_out) {
    *sim_out         = NULL;
    *debug_level_out = PLCTAG_DEBUG_WARN;

    /* First pass: collect scalar options before creating the sim. */
    enip_plc_type_t plc_type = ENIP_PLC_LGX;
    uint16_t    port       = 44818;
    const char *bind_addr  = NULL;
    int32_t     delay_ms   = 0;

    for(int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *val;

        if((val = find_prefix(arg, "--port="))) {
            port = parse_uint16_val(val, 44818);
        } else if((val = find_prefix(arg, "--bind="))) {
            bind_addr = val;
        } else if((val = find_prefix(arg, "--plc="))) {
            plc_type = parse_plc_type(val);
        } else if((val = find_prefix(arg, "--delay="))) {
            delay_ms = parse_int32_val(val, 0);
        } else if((val = find_prefix(arg, "--debug="))) {
            *debug_level_out = parse_int32_val(val, PLCTAG_DEBUG_WARN);
        } else if((val = find_prefix(arg, "--tag="))) {
            (void)val;   /* handled in second pass */
        } else if(find_prefix(arg, "--help") == arg + 6 || find_prefix(arg, "-h") == arg + 2) {
            args_print_usage(argv[0]);
            return 1;
        } else {
            fprintf(stderr, "device_sim: unknown argument '%s'.\n", arg);
            return PLCTAG_ERR_BAD_PARAM;
        }
    }

    device_sim_t *sim = device_sim_create(plc_type, bind_addr, port);
    if(!sim) {
        fprintf(stderr, "device_sim: failed to create simulator.\n");
        return PLCTAG_ERR_NO_MEM;
    }

    if(delay_ms != 0) { device_sim_set_response_delay(sim, (uint32_t)delay_ms); }

    /* Second pass: add tags. */
    for(int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *val;
        if((val = find_prefix(arg, "--tag="))) {
            int32_t rc = parse_tag_spec(sim, val);
            if(rc != PLCTAG_STATUS_OK) {
                fprintf(stderr, "device_sim: failed to parse --tag='%s'.\n", val);
                device_sim_destroy(sim);
                return rc;
            }
        }
    }

    *sim_out = sim;
    return PLCTAG_STATUS_OK;
}

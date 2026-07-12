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

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <libplctag/lib/libplctag.h>
#include "platform.h"
#include "args.h"

/* ============================================================================
 * CIP / PCCC type tables — mirrors
 * protocols/enip/server/eip_server_tag.c's elem_type=/pccc_type= tables.
 * ============================================================================ */

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

static bool str_neq_i_n(const char *a, const char *b, size_t len) {
    for(size_t i = 0; i < len; i++) {
        char ca = (a[i] >= 'a' && a[i] <= 'z') ? (char)(a[i] - 32) : a[i];
        char cb = (b[i] >= 'a' && b[i] <= 'z') ? (char)(b[i] - 32) : b[i];
        if(ca != cb) { return false; }
    }
    return true;
}

static const char *const CIP_TYPE_NAMES[] = {"BOOL", "SINT", "INT", "DINT", "LINT", "REAL", "LREAL", NULL};

static bool is_known_cip_type(const char *name, size_t len) {
    for(const char *const *e = CIP_TYPE_NAMES; *e; e++) {
        if(str_length(*e) == (int)len && str_neq_i_n(*e, name, len)) { return true; }
    }
    return false;
}

static bool is_known_pccc_letter(char letter) {
    if(letter >= 'a' && letter <= 'z') { letter = (char)(letter - 32); }
    return letter == 'B' || letter == 'N' || letter == 'L' || letter == 'F' || letter == 'R';
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
 * Dimension parser: "[d1]" or "[d1,d2]" or "[d1,d2,d3]"
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
 * Tag spec validation (catches bad --tag= specs at parse time, before any
 * connection to the library; actual attr-string construction happens later
 * in args_build_tag_attr_str, once for each spec).
 * ============================================================================ */

static int32_t validate_cip_tag(const char *spec) {
    const char *colon = find_char(spec, ':');
    if(!colon || colon == spec) {
        fprintf(stderr, "device_sim: CIP tag spec '%s' missing name or ':'.\n", spec);
        return PLCTAG_ERR_BAD_PARAM;
    }

    const char *type_start = colon + 1;
    const char *bracket = find_char(type_start, '[');
    if(!bracket || bracket == type_start) {
        fprintf(stderr, "device_sim: CIP tag spec '%s' missing type or '['.\n", spec);
        return PLCTAG_ERR_BAD_PARAM;
    }

    size_t type_len = (size_t)(bracket - type_start);
    if(!is_known_cip_type(type_start, type_len)) {
        fprintf(stderr, "device_sim: unknown CIP type '%.*s' in tag spec '%s'.\n", (int)type_len, type_start, spec);
        return PLCTAG_ERR_BAD_PARAM;
    }

    uint32_t num_dim = 0;
    uint32_t dims[3] = {0, 0, 0};
    if(!parse_dims(bracket, &num_dim, dims)) { return PLCTAG_ERR_BAD_PARAM; }

    return PLCTAG_STATUS_OK;
}

static int32_t validate_pccc_tag(const char *spec) {
    if(!is_known_pccc_letter(*spec)) {
        fprintf(stderr, "device_sim: unknown PCCC type letter '%c' in tag spec '%s'.\n", *spec, spec);
        return PLCTAG_ERR_BAD_PARAM;
    }

    const char *s = spec + 1;
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

    return PLCTAG_STATUS_OK;
}

static int32_t validate_tag_spec(const char *spec) {
    return find_char(spec, ':') ? validate_cip_tag(spec) : validate_pccc_tag(spec);
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
        "  --model=NAME     Catalog model within --plc='s family (e.g. NX102 for\n"
        "                   Omron); default is the family's built-in default\n"
        "  --tag=SPEC       Add a CIP tag:  Name:TYPE[count]  or multi-dim\n"
        "                                   Name:TYPE[d1,d2,d3]\n"
        "                   Add a PCCC tag: B<n>[count]  N<n>[count]\n"
        "                                   L<n>[count]  F<n>[count]\n"
        "                   TYPE: BOOL SINT INT DINT LINT REAL LREAL\n"
        "                   May be repeated (up to 64 tags).\n"
        "  --delay=MS       Artificial response delay in milliseconds\n"
        "  --debug=N        Debug level 1-5 (default: 2)\n"
        "  --help           Show this message\n"
        "\n"
        "Examples:\n"
        "  %s --tag=MyDINT:DINT[1] --tag=MyArray:DINT[100]\n"
        "  %s --plc=Micrologix --tag=B3[10] --tag=N7[10]\n",
        prog, prog, prog);
}

extern int32_t args_parse(int argc, char **argv, sim_args_t *args_out, int32_t *debug_level_out) {
    mem_set(args_out, 0, (int)sizeof(*args_out));
    args_out->port = 44818;
    *debug_level_out = PLCTAG_DEBUG_WARN;

    for(int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *val;

        if((val = find_prefix(arg, "--port="))) {
            args_out->port = parse_uint16_val(val, 44818);
        } else if((val = find_prefix(arg, "--bind="))) {
            args_out->bind_addr = val;
        } else if((val = find_prefix(arg, "--plc="))) {
            args_out->plc_type = val;
        } else if((val = find_prefix(arg, "--model="))) {
            args_out->model = val;
        } else if((val = find_prefix(arg, "--delay="))) {
            args_out->delay_ms = parse_int32_val(val, 0);
        } else if((val = find_prefix(arg, "--debug="))) {
            *debug_level_out = parse_int32_val(val, PLCTAG_DEBUG_WARN);
        } else if((val = find_prefix(arg, "--tag="))) {
            if(args_out->num_tags >= (int)(sizeof(args_out->tag_specs) / sizeof(args_out->tag_specs[0]))) {
                fprintf(stderr, "device_sim: too many --tag= options (max %d).\n",
                        (int)(sizeof(args_out->tag_specs) / sizeof(args_out->tag_specs[0])));
                return PLCTAG_ERR_BAD_PARAM;
            }
            int32_t rc = validate_tag_spec(val);
            if(rc != PLCTAG_STATUS_OK) {
                fprintf(stderr, "device_sim: failed to parse --tag='%s'.\n", val);
                return rc;
            }
            args_out->tag_specs[args_out->num_tags++] = val;
        } else if(find_prefix(arg, "--help") == arg + 6 || find_prefix(arg, "-h") == arg + 2) {
            args_print_usage(argv[0]);
            return 1;
        } else {
            fprintf(stderr, "device_sim: unknown argument '%s'.\n", arg);
            return PLCTAG_ERR_BAD_PARAM;
        }
    }

    if(args_out->num_tags == 0) {
        fprintf(stderr, "device_sim: at least one --tag= is required.\n");
        return PLCTAG_ERR_BAD_PARAM;
    }

    return PLCTAG_STATUS_OK;
}

/* ============================================================================
 * attr-string construction
 *
 * attr_create_from_str rejects "key=" with an empty value, so optional
 * options (bind_addr, model, delay) are appended only when set, via
 * append_attr rather than baked into one snprintf template.
 * ============================================================================ */

static bool append_attr(char *out, size_t out_cap, const char *fmt, ...) {
    size_t used = str_length(out);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + used, out_cap - used, fmt, ap);
    va_end(ap);
    return n >= 0 && used + (size_t)n < out_cap;
}

static bool append_shared_attrs(const sim_args_t *args, char *out, size_t out_cap) {
    if(!append_attr(out, out_cap, "&port=%u", (unsigned)args->port)) { return false; }
    if(args->bind_addr && *args->bind_addr && !append_attr(out, out_cap, "&gateway=%s", args->bind_addr)) { return false; }
    if(args->plc_type && *args->plc_type && !append_attr(out, out_cap, "&plc=%s", args->plc_type)) { return false; }
    if(args->model && *args->model && !append_attr(out, out_cap, "&model=%s", args->model)) { return false; }
    if(args->delay_ms != 0 && !append_attr(out, out_cap, "&sim_delay_ms=%d", (int)args->delay_ms)) { return false; }
    return true;
}

static int32_t build_cip_tag_attr_str(const sim_args_t *args, const char *spec, char *out, size_t out_cap) {
    const char *colon = find_char(spec, ':');
    size_t name_len = (size_t)(colon - spec);

    const char *type_start = colon + 1;
    const char *bracket = find_char(type_start, '[');
    size_t type_len = (size_t)(bracket - type_start);

    uint32_t num_dim = 0;
    uint32_t dims[3] = {0, 0, 0};
    if(!parse_dims(bracket, &num_dim, dims)) { return PLCTAG_ERR_BAD_PARAM; }

    if(out_cap == 0) { return PLCTAG_ERR_BAD_PARAM; }
    out[0] = '\0';
    if(!append_attr(out, out_cap, "protocol=ab-eip&role=server&name=%.*s&elem_type=%.*s&dim0=%u&dim1=%u&dim2=%u",
                     (int)name_len, spec, (int)type_len, type_start,
                     (unsigned)dims[0], num_dim >= 2 ? (unsigned)dims[1] : 0u, num_dim >= 3 ? (unsigned)dims[2] : 0u)) {
        return PLCTAG_ERR_BAD_PARAM;
    }
    if(!append_shared_attrs(args, out, out_cap)) { return PLCTAG_ERR_BAD_PARAM; }

    return PLCTAG_STATUS_OK;
}

static int32_t build_pccc_tag_attr_str(const sim_args_t *args, const char *spec, char *out, size_t out_cap) {
    char letter = *spec;
    const char *s = spec + 1;
    int32_t file_num = 0;
    parse_digits(&s, &file_num);

    uint32_t num_dim = 0;
    uint32_t dims[3] = {0, 0, 0};
    if(!parse_dims(s, &num_dim, dims)) { return PLCTAG_ERR_BAD_PARAM; }

    if(out_cap == 0) { return PLCTAG_ERR_BAD_PARAM; }
    out[0] = '\0';
    /* PCCC tags are addressed by (letter,file_num) on the wire, not by name;
     * name= is required by the constructor but purely cosmetic here. */
    if(!append_attr(out, out_cap, "protocol=ab-eip&role=server&name=%c%d&pccc_type=%c&pccc_file=%d&elem_count=%u",
                     letter, (int)file_num, letter, (int)file_num, (unsigned)dims[0])) {
        return PLCTAG_ERR_BAD_PARAM;
    }
    if(!append_shared_attrs(args, out, out_cap)) { return PLCTAG_ERR_BAD_PARAM; }

    return PLCTAG_STATUS_OK;
}

extern int32_t args_build_tag_attr_str(const sim_args_t *args, const char *spec, char *out, size_t out_cap) {
    return find_char(spec, ':') ? build_cip_tag_attr_str(args, spec, out, out_cap)
                                 : build_pccc_tag_attr_str(args, spec, out, out_cap);
}

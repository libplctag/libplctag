/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever    *
 * you choose.                                                             *
 ***************************************************************************/

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <winsock2.h>
#    include <ws2tcpip.h>
typedef SOCKET socket_t;
#    define CLOSESOCK closesocket
#    define SOCK_ERRNO WSAGetLastError()
#else
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <sys/types.h>
#    include <unistd.h>
typedef int socket_t;
#    define INVALID_SOCKET (-1)
#    define SOCKET_ERROR (-1)
#    define CLOSESOCK close
#    define SOCK_ERRNO errno
#endif

#ifdef _WIN32
typedef int sock_io_t;
typedef int sock_len_t;
#    define SOCK_LEN_MAX ((size_t)INT_MAX)
#else
typedef ssize_t sock_io_t;
typedef size_t sock_len_t;
#    define SOCK_LEN_MAX ((size_t)SSIZE_MAX)
#endif

#define MAX_PACKET_SIZE (1024U * 1024U)

typedef enum {
    PATCH_RECALC_EIP_LENGTH = 1,
    PATCH_SET_SESSION_HANDLE,
    PATCH_COPY_FROM_REQUEST,
    PATCH_SET_CONNECTED_SEQUENCE
} patch_type_t;

typedef struct {
    patch_type_t type;
    size_t offset;
    size_t size;
    size_t src_offset;
    size_t dst_offset;
} patch_rule_t;

typedef struct {
    int id;
    char *key;

    uint8_t *req_template;
    uint8_t *req_mask;
    size_t req_len;

    uint8_t *rsp_template;
    uint8_t *rsp_mask;
    size_t rsp_len;

    patch_rule_t *rules;
    size_t rule_count;
} replay_entry_t;

typedef struct {
    replay_entry_t *entries;
    size_t count;
} replay_dataset_t;

typedef struct {
    uint32_t session_handle;
    uint16_t connected_sequence;
    uint32_t client_connection_id;
    uint32_t server_connection_id;
    uint16_t connection_serial_number;
} client_state_t;

typedef struct {
    size_t matched_count;
    size_t tolerant_count;
    size_t expected_total;
    bool complete;
    bool had_error;
} session_stats_t;

static uint32_t g_next_session_handle = 1;
static uint32_t g_next_server_connection_id = 0x01000000U;

typedef struct {
    const uint8_t *cip;
    size_t cip_len;
    uint8_t service;
    bool connected_item;
} cip_view_t;

static uint16_t get_u16_le(const uint8_t *buf);
static bool get_cip_view(const uint8_t *packet, size_t packet_len, cip_view_t *view);

static bool get_cpf_item_data_offset(const uint8_t *packet, size_t packet_len, uint16_t wanted_item_type, size_t min_item_len,
                                     size_t *out_data_offset, size_t *out_item_len) {
    uint16_t cmd = 0;
    uint16_t eip_len = 0;
    const uint8_t *eip_payload = NULL;
    size_t eip_payload_len = 0;
    uint16_t item_count = 0;
    size_t off = 0;
    size_t i = 0;

    if(!packet || packet_len < 24U || !out_data_offset) { return false; }

    cmd = get_u16_le(packet + 0);
    if(cmd != 0x006FU && cmd != 0x0070U) { return false; }

    eip_len = get_u16_le(packet + 2);
    if(packet_len < 24U + (size_t)eip_len) { return false; }

    eip_payload = packet + 24U;
    eip_payload_len = (size_t)eip_len;
    if(eip_payload_len < 8U) { return false; }

    item_count = get_u16_le(eip_payload + 6U);
    off = 8U;
    for(i = 0; i < (size_t)item_count; i++) {
        uint16_t item_type = 0;
        uint16_t item_len = 0;
        size_t data_off = 0;

        if(off + 4U > eip_payload_len) { return false; }

        item_type = get_u16_le(eip_payload + off);
        item_len = get_u16_le(eip_payload + off + 2U);
        data_off = off + 4U;

        if(data_off + (size_t)item_len > eip_payload_len) { return false; }

        if(item_type == wanted_item_type && (size_t)item_len >= min_item_len) {
            *out_data_offset = 24U + data_off;
            if(out_item_len) { *out_item_len = (size_t)item_len; }
            return true;
        }

        off = data_off + (size_t)item_len;
    }

    return false;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s --dataset=<path> [--bind=<ip>] [--port=<port>] [--verbose] [--strict-only]\n"
            "\n"
            "Replay requests from a generated dataset JSON file.\n"
            "With --strict-only, any byte-level request mismatch fails immediately.\n"
            "\n"
            "Example:\n"
            "  %s --dataset=/tmp/libplctag_capture/replay_dataset.json --bind=0.0.0.0 --port=44818\n",
            prog, prog);
}

static char *read_file_text(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    long sz = 0;
    size_t n = 0;

    if(!f) {
        fprintf(stderr, "ERROR: cannot open dataset file: %s\n", path);
        return NULL;
    }

    if(fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    sz = ftell(f);
    if(sz < 0) {
        fclose(f);
        return NULL;
    }

    if(fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    buf = (char *)malloc((size_t)sz + 1U);
    if(!buf) {
        fclose(f);
        return NULL;
    }

    n = fread(buf, 1U, (size_t)sz, f);
    fclose(f);

    if(n != (size_t)sz) {
        free(buf);
        return NULL;
    }

    buf[n] = '\0';
    if(out_len) { *out_len = n; }

    return buf;
}

static bool hex_val(char c, uint8_t *v) {
    if(c >= '0' && c <= '9') {
        *v = (uint8_t)(c - '0');
        return true;
    }
    if(c >= 'a' && c <= 'f') {
        *v = (uint8_t)(10 + (c - 'a'));
        return true;
    }
    if(c >= 'A' && c <= 'F') {
        *v = (uint8_t)(10 + (c - 'A'));
        return true;
    }
    return false;
}

static bool decode_hex(const char *hex, uint8_t **out, size_t *out_len) {
    size_t len = 0;
    size_t i = 0;
    uint8_t *buf = NULL;

    if(!hex) { return false; }

    len = strlen(hex);
    if((len % 2U) != 0U) { return false; }

    buf = (uint8_t *)malloc(len / 2U);
    if(!buf) { return false; }

    for(i = 0; i < len; i += 2U) {
        uint8_t hi = 0;
        uint8_t lo = 0;
        if(!hex_val(hex[i], &hi) || !hex_val(hex[i + 1U], &lo)) {
            free(buf);
            return false;
        }
        buf[i / 2U] = (uint8_t)((hi << 4U) | lo);
    }

    *out = buf;
    *out_len = len / 2U;
    return true;
}

static const char *skip_ws(const char *p) {
    while(p && *p && isspace((unsigned char)*p)) { p++; }
    return p;
}

static const char *find_key(const char *json, const char *key) {
    char needle[128];
    int rc = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if(rc < 0 || (size_t)rc >= sizeof(needle)) { return NULL; }
    return strstr(json, needle);
}

static bool extract_int(const char *json, const char *key, int *out) {
    const char *k = find_key(json, key);
    const char *p = NULL;
    char *end = NULL;
    long v = 0;

    if(!k) { return false; }

    p = strchr(k, ':');
    if(!p) { return false; }

    p = skip_ws(p + 1);
    v = strtol(p, &end, 10);
    if(end == p) { return false; }

    *out = (int)v;
    return true;
}

static bool extract_string(const char *json, const char *key, char **out) {
    const char *k = find_key(json, key);
    const char *p = NULL;
    const char *q = NULL;
    char *s = NULL;
    size_t len = 0;

    if(!k) { return false; }

    p = strchr(k, ':');
    if(!p) { return false; }

    p = skip_ws(p + 1);
    if(*p != '"') { return false; }
    p++;

    q = p;
    while(*q) {
        if(*q == '"' && q[-1] != '\\') { break; }
        q++;
    }

    if(*q != '"') { return false; }

    len = (size_t)(q - p);
    s = (char *)malloc(len + 1U);
    if(!s) { return false; }

    memcpy(s, p, len);
    s[len] = '\0';

    *out = s;
    return true;
}

static bool extract_object_span(const char *json, const char *key, const char **obj_start, const char **obj_end) {
    const char *k = find_key(json, key);
    const char *p = NULL;
    int depth = 0;
    bool in_str = false;

    if(!k) { return false; }

    p = strchr(k, ':');
    if(!p) { return false; }

    p = skip_ws(p + 1);
    if(*p != '{') { return false; }

    *obj_start = p;

    while(*p) {
        char c = *p;
        if(c == '"' && (p == *obj_start || p[-1] != '\\')) {
            in_str = !in_str;
        } else if(!in_str) {
            if(c == '{') {
                depth++;
            } else if(c == '}') {
                depth--;
                if(depth == 0) {
                    *obj_end = p;
                    return true;
                }
            }
        }
        p++;
    }

    return false;
}

static bool extract_array_span(const char *json, const char *key, const char **arr_start, const char **arr_end) {
    const char *k = find_key(json, key);
    const char *p = NULL;
    int depth = 0;
    bool in_str = false;

    if(!k) { return false; }

    p = strchr(k, ':');
    if(!p) { return false; }

    p = skip_ws(p + 1);
    if(*p != '[') { return false; }

    *arr_start = p;

    while(*p) {
        char c = *p;
        if(c == '"' && (p == *arr_start || p[-1] != '\\')) {
            in_str = !in_str;
        } else if(!in_str) {
            if(c == '[') {
                depth++;
            } else if(c == ']') {
                depth--;
                if(depth == 0) {
                    *arr_end = p;
                    return true;
                }
            }
        }
        p++;
    }

    return false;
}

static char *dup_span(const char *start, const char *end) {
    size_t len = 0;
    char *buf = NULL;

    if(!start || !end || end < start) { return NULL; }

    len = (size_t)(end - start + 1);
    buf = (char *)malloc(len + 1U);
    if(!buf) { return NULL; }

    memcpy(buf, start, len);
    buf[len] = '\0';
    return buf;
}

static size_t count_top_level_objects(const char *arr_json) {
    const char *p = arr_json;
    int depth = 0;
    bool in_str = false;
    size_t count = 0;

    while(p && *p) {
        char c = *p;
        if(c == '"' && (p == arr_json || p[-1] != '\\')) {
            in_str = !in_str;
        } else if(!in_str) {
            if(c == '{') {
                if(depth == 1) { count++; }
                depth++;
            } else if(c == '}') {
                depth--;
            } else if(c == '[') {
                depth++;
            } else if(c == ']') {
                depth--;
                if(depth == 0) { break; }
            }
        }
        p++;
    }

    return count;
}

static bool parse_patch_type(const char *s, patch_type_t *type) {
    if(strcmp(s, "recalc_eip_length") == 0) {
        *type = PATCH_RECALC_EIP_LENGTH;
        return true;
    }
    if(strcmp(s, "set_session_handle") == 0) {
        *type = PATCH_SET_SESSION_HANDLE;
        return true;
    }
    if(strcmp(s, "copy_from_request") == 0) {
        *type = PATCH_COPY_FROM_REQUEST;
        return true;
    }
    if(strcmp(s, "set_connected_sequence") == 0) {
        *type = PATCH_SET_CONNECTED_SEQUENCE;
        return true;
    }
    return false;
}

static void free_dataset(replay_dataset_t *dataset) {
    size_t i = 0;
    if(!dataset || !dataset->entries) { return; }

    for(i = 0; i < dataset->count; i++) {
        replay_entry_t *e = &dataset->entries[i];
        free(e->key);
        free(e->req_template);
        free(e->req_mask);
        free(e->rsp_template);
        free(e->rsp_mask);
        free(e->rules);
    }

    free(dataset->entries);
    dataset->entries = NULL;
    dataset->count = 0;
}

static bool parse_patch_rules(const char *entry_json, replay_entry_t *entry) {
    const char *arr_start = NULL;
    const char *arr_end = NULL;
    const char *p = NULL;
    size_t cap = 4;

    entry->rules = (patch_rule_t *)calloc(cap, sizeof(patch_rule_t));
    if(!entry->rules) { return false; }

    if(!extract_array_span(entry_json, "patch_rules", &arr_start, &arr_end)) {
        entry->rule_count = 0;
        return true;
    }

    p = arr_start;
    while(p && p <= arr_end) {
        const char *obj_s = NULL;
        const char *obj_e = NULL;
        char *obj_json = NULL;
        char *type_str = NULL;
        patch_rule_t r;
        int iv = 0;

        p = strchr(p, '{');
        if(!p || p > arr_end) { break; }

        obj_s = p;
        {
            int depth = 0;
            bool in_str = false;
            while(*p && p <= arr_end) {
                char c = *p;
                if(c == '"' && (p == obj_s || p[-1] != '\\')) {
                    in_str = !in_str;
                } else if(!in_str) {
                    if(c == '{') {
                        depth++;
                    } else if(c == '}') {
                        depth--;
                        if(depth == 0) {
                            obj_e = p;
                            break;
                        }
                    }
                }
                p++;
            }
        }

        if(!obj_e) { break; }

        obj_json = dup_span(obj_s, obj_e);
        if(!obj_json) { return false; }

        memset(&r, 0, sizeof(r));

        if(!extract_string(obj_json, "type", &type_str)) {
            free(obj_json);
            return false;
        }

        if(!parse_patch_type(type_str, &r.type)) {
            free(type_str);
            free(obj_json);
            return false;
        }

        free(type_str);
        type_str = NULL;

        if(extract_int(obj_json, "offset", &iv)) { r.offset = (size_t)iv; }
        if(extract_int(obj_json, "size", &iv)) { r.size = (size_t)iv; }
        if(extract_int(obj_json, "src_offset", &iv)) { r.src_offset = (size_t)iv; }
        if(extract_int(obj_json, "dst_offset", &iv)) { r.dst_offset = (size_t)iv; }

        if(entry->rule_count >= cap) {
            patch_rule_t *tmp = NULL;
            cap *= 2U;
            tmp = (patch_rule_t *)realloc(entry->rules, cap * sizeof(patch_rule_t));
            if(!tmp) {
                free(obj_json);
                return false;
            }
            entry->rules = tmp;
        }

        entry->rules[entry->rule_count++] = r;
        free(obj_json);

        p = obj_e + 1;
    }

    return true;
}

static bool parse_entry(const char *entry_json, replay_entry_t *entry) {
    const char *req_start = NULL;
    const char *req_end = NULL;
    const char *rsp_start = NULL;
    const char *rsp_end = NULL;
    char *req_json = NULL;
    char *rsp_json = NULL;
    char *hex = NULL;

    memset(entry, 0, sizeof(*entry));

    if(!extract_int(entry_json, "id", &entry->id)) { return false; }

    if(!extract_string(entry_json, "key", &entry->key)) { return false; }

    if(!extract_object_span(entry_json, "request", &req_start, &req_end)
       || !extract_object_span(entry_json, "response", &rsp_start, &rsp_end)) {
        return false;
    }

    req_json = dup_span(req_start, req_end);
    rsp_json = dup_span(rsp_start, rsp_end);
    if(!req_json || !rsp_json) {
        free(req_json);
        free(rsp_json);
        return false;
    }

    if(!extract_string(req_json, "template_hex", &hex) || !decode_hex(hex, &entry->req_template, &entry->req_len)) {
        free(hex);
        free(req_json);
        free(rsp_json);
        return false;
    }
    free(hex);
    hex = NULL;

    if(!extract_string(req_json, "mask_hex", &hex) || !decode_hex(hex, &entry->req_mask, &entry->req_len)) {
        free(hex);
        free(req_json);
        free(rsp_json);
        return false;
    }
    free(hex);
    hex = NULL;

    if(!extract_string(rsp_json, "template_hex", &hex) || !decode_hex(hex, &entry->rsp_template, &entry->rsp_len)) {
        free(hex);
        free(req_json);
        free(rsp_json);
        return false;
    }
    free(hex);
    hex = NULL;

    if(!extract_string(rsp_json, "mask_hex", &hex) || !decode_hex(hex, &entry->rsp_mask, &entry->rsp_len)) {
        free(hex);
        free(req_json);
        free(rsp_json);
        return false;
    }
    free(hex);

    if(!parse_patch_rules(rsp_json, entry)) {
        free(req_json);
        free(rsp_json);
        return false;
    }

    free(req_json);
    free(rsp_json);

    return true;
}

static bool load_dataset(const char *path, replay_dataset_t *dataset) {
    char *json = NULL;
    const char *arr_start = NULL;
    const char *arr_end = NULL;
    const char *p = NULL;
    size_t file_len = 0;
    size_t idx = 0;
    size_t count = 0;

    memset(dataset, 0, sizeof(*dataset));

    json = read_file_text(path, &file_len);
    if(!json || file_len == 0U) {
        free(json);
        return false;
    }

    if(!extract_array_span(json, "entries", &arr_start, &arr_end)) {
        fprintf(stderr, "ERROR: dataset has no entries array.\n");
        free(json);
        return false;
    }

    count = count_top_level_objects(arr_start);
    if(count == 0U) {
        fprintf(stderr, "ERROR: dataset entries array is empty.\n");
        free(json);
        return false;
    }

    dataset->entries = (replay_entry_t *)calloc(count, sizeof(replay_entry_t));
    if(!dataset->entries) {
        free(json);
        return false;
    }

    p = arr_start;
    while(p && p <= arr_end && idx < count) {
        const char *obj_s = strchr(p, '{');
        const char *obj_e = NULL;
        char *obj_json = NULL;
        int depth = 0;
        bool in_str = false;

        if(!obj_s || obj_s > arr_end) { break; }

        p = obj_s;
        while(*p && p <= arr_end) {
            char c = *p;
            if(c == '"' && (p == obj_s || p[-1] != '\\')) {
                in_str = !in_str;
            } else if(!in_str) {
                if(c == '{') {
                    depth++;
                } else if(c == '}') {
                    depth--;
                    if(depth == 0) {
                        obj_e = p;
                        break;
                    }
                }
            }
            p++;
        }

        if(!obj_e) { break; }

        obj_json = dup_span(obj_s, obj_e);
        if(!obj_json) {
            free_dataset(dataset);
            free(json);
            return false;
        }

        if(!parse_entry(obj_json, &dataset->entries[idx])) {
            fprintf(stderr, "ERROR: failed parsing entry %zu\n", idx + 1U);
            free(obj_json);
            free_dataset(dataset);
            free(json);
            return false;
        }

        free(obj_json);
        idx++;
        p = obj_e + 1;
    }

    dataset->count = idx;
    free(json);

    if(dataset->count == 0U) {
        free_dataset(dataset);
        return false;
    }

    return true;
}

static bool packet_matches(const replay_entry_t *entry, const uint8_t *req, size_t req_len) {
    size_t i = 0;

    if(req_len != entry->req_len) { return false; }

    for(i = 0; i < req_len; i++) {
        if(entry->req_mask[i] != 0U && entry->req_template[i] != req[i]) { return false; }
    }

    return true;
}

static bool packet_matches_with_first_diff(const replay_entry_t *entry, const uint8_t *req, size_t req_len,
                                           size_t *first_diff_idx) {
    size_t i = 0;

    if(first_diff_idx) { *first_diff_idx = (size_t)-1; }

    if(req_len != entry->req_len) {
        if(first_diff_idx) { *first_diff_idx = 0U; }
        return false;
    }

    for(i = 0; i < req_len; i++) {
        if(entry->req_mask[i] != 0U && entry->req_template[i] != req[i]) {
            if(first_diff_idx) { *first_diff_idx = i; }
            return false;
        }
    }

    return true;
}

static void build_request_key(const uint8_t *req, size_t req_len, char *out, size_t out_sz) {
    uint16_t cmd = 0;
    cip_view_t v;

    if(!out || out_sz == 0U) { return; }

    if(!req || req_len < 2U) {
        (void)snprintf(out, out_sz, "raw_len:%zu", req_len);
        return;
    }

    cmd = get_u16_le(req + 0);
    if(get_cip_view(req, req_len, &v)) {
        (void)snprintf(out, out_sz, "eip_cmd:0x%04x|cip_srv:0x%02x", (unsigned)cmd, (unsigned)v.service);
    } else {
        (void)snprintf(out, out_sz, "eip_cmd:0x%04x", (unsigned)cmd);
    }
}

static void put_u16_le(uint8_t *buf, uint16_t v) {
    buf[0] = (uint8_t)(v & 0xFFU);
    buf[1] = (uint8_t)((v >> 8U) & 0xFFU);
}

static void put_u32_le(uint8_t *buf, uint32_t v) {
    buf[0] = (uint8_t)(v & 0xFFU);
    buf[1] = (uint8_t)((v >> 8U) & 0xFFU);
    buf[2] = (uint8_t)((v >> 16U) & 0xFFU);
    buf[3] = (uint8_t)((v >> 24U) & 0xFFU);
}

static uint16_t get_u16_le(const uint8_t *buf) { return (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8U)); }

static uint32_t get_u32_le(const uint8_t *buf) {
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8U) | ((uint32_t)buf[2] << 16U) | ((uint32_t)buf[3] << 24U);
}

static bool get_cip_view(const uint8_t *packet, size_t packet_len, cip_view_t *view) {
    uint16_t cmd = 0;
    uint16_t eip_len = 0;
    const uint8_t *eip_payload = NULL;
    size_t eip_payload_len = 0;
    size_t off = 0;
    uint16_t item_count = 0;
    size_t i = 0;

    if(!packet || !view || packet_len < 24U) { return false; }

    cmd = get_u16_le(packet + 0);
    if(cmd != 0x006FU && cmd != 0x0070U) { return false; }

    eip_len = get_u16_le(packet + 2);
    if(packet_len < 24U + (size_t)eip_len) { return false; }

    eip_payload = packet + 24;
    eip_payload_len = (size_t)eip_len;
    if(eip_payload_len < 8U) { return false; }

    item_count = get_u16_le(eip_payload + 6);
    off = 8U;

    for(i = 0; i < (size_t)item_count; i++) {
        uint16_t item_type = 0;
        uint16_t item_len = 0;
        const uint8_t *item_data = NULL;

        if(off + 4U > eip_payload_len) { return false; }

        item_type = get_u16_le(eip_payload + off);
        item_len = get_u16_le(eip_payload + off + 2U);
        off += 4U;

        if(off + (size_t)item_len > eip_payload_len) { return false; }

        item_data = eip_payload + off;

        if(item_type == 0x00B2U && item_len >= 1U) {
            view->cip = item_data;
            view->cip_len = (size_t)item_len;
            view->service = item_data[0];
            view->connected_item = false;
            return true;
        }

        if(item_type == 0x00B1U && item_len >= 3U) {
            view->cip = item_data + 2U; /* skip connected sequence */
            view->cip_len = (size_t)item_len - 2U;
            view->service = item_data[2];
            view->connected_item = true;
            return true;
        }

        off += (size_t)item_len;
    }

    return false;
}

static bool patch_forward_open_response(const uint8_t *rsp, size_t rsp_len, client_state_t *state) {
    cip_view_t v;
    size_t payload_off = 0;
    uint8_t ext_words = 0;

    if(!rsp || !state) { return false; }

    if(!get_cip_view(rsp, rsp_len, &v)) { return true; }

    if(v.service != 0xD4U && v.service != 0xDBU) { return true; }

    if(v.cip_len < 4U) { return false; }

    /* CIP response header: service, reserved, gen_status, ext_status_words */
    ext_words = v.cip[3];
    payload_off = 4U + ((size_t)ext_words * 2U);

    if(payload_off + 10U > v.cip_len) { return false; }

    /* Forward Open response body: server conn ID, client conn ID, conn serial number, ... */
    put_u32_le((uint8_t *)(v.cip + payload_off + 0U), state->server_connection_id);
    put_u32_le((uint8_t *)(v.cip + payload_off + 4U), state->client_connection_id);
    put_u16_le((uint8_t *)(v.cip + payload_off + 8U), state->connection_serial_number);

    return true;
}

static bool update_state_from_request(const uint8_t *req, size_t req_len, client_state_t *state) {
    cip_view_t v;
    uint16_t cmd = 0;
    size_t path_bytes = 0;
    size_t body_off = 0;

    if(req_len < 24U) { return false; }

    cmd = get_u16_le(req + 0);

    if(cmd == 0x0065U) {
        state->session_handle = g_next_session_handle++;
        if(g_next_session_handle == 0U) { g_next_session_handle = 1U; }
    } else if(cmd == 0x0066U) {
        state->session_handle = 0U;
        state->client_connection_id = 0U;
        state->server_connection_id = 0U;
        state->connection_serial_number = 0U;
        state->connected_sequence = 1U;
        return false;
    }

    if(!get_cip_view(req, req_len, &v)) { return false; }

    if(v.service != 0x54U && v.service != 0x5BU) { return false; }

    if(v.cip_len < 2U) { return false; }

    path_bytes = (size_t)v.cip[1] * 2U;
    body_off = 2U + path_bytes;

    /* Forward Open request body fields at fixed offsets after pri/ticks. */
    if(body_off + 12U > v.cip_len) { return false; }

    state->client_connection_id = get_u32_le(v.cip + body_off + 2U);
    state->connection_serial_number = get_u16_le(v.cip + body_off + 10U);
    state->server_connection_id = g_next_server_connection_id++;
    if(g_next_server_connection_id == 0U) { g_next_server_connection_id = 1U; }

    state->connected_sequence = 1U;
    return true;
}

static bool apply_patch_rules(const replay_entry_t *entry, const uint8_t *req, size_t req_len, uint8_t *rsp, size_t rsp_len,
                              client_state_t *state) {
    size_t i = 0;

    for(i = 0; i < entry->rule_count; i++) {
        const patch_rule_t *r = &entry->rules[i];

        switch(r->type) {
            case PATCH_RECALC_EIP_LENGTH:
                if(r->offset + 2U > rsp_len || rsp_len < 24U) { return false; }
                put_u16_le(rsp + r->offset, (uint16_t)(rsp_len - 24U));
                break;

            case PATCH_SET_SESSION_HANDLE:
                if(r->offset + 4U > rsp_len) { return false; }
                put_u32_le(rsp + r->offset, state->session_handle);
                break;

            case PATCH_COPY_FROM_REQUEST:
                if(r->src_offset + r->size > req_len || r->dst_offset + r->size > rsp_len) { return false; }
                memcpy(rsp + r->dst_offset, req + r->src_offset, r->size);
                break;

            case PATCH_SET_CONNECTED_SEQUENCE:
                if(r->offset + 2U > rsp_len) { return false; }
                put_u16_le(rsp + r->offset, state->connected_sequence++);
                break;

            default: return false;
        }
    }

    return true;
}

static bool entry_has_connected_seq_patch(const replay_entry_t *entry, size_t seq_off) {
    size_t i = 0;
    if(!entry) { return false; }
    for(i = 0; i < entry->rule_count; i++) {
        if(entry->rules[i].type == PATCH_SET_CONNECTED_SEQUENCE && entry->rules[i].offset == seq_off) { return true; }
    }
    return false;
}

static bool patch_runtime_connected_fields(const replay_entry_t *entry, const uint8_t *req, size_t req_len, uint8_t *rsp,
                                           size_t rsp_len, client_state_t *state) {
    size_t req_conn_off = 0;
    size_t rsp_conn_off = 0;
    size_t req_conn_len = 0;
    size_t rsp_conn_len = 0;
    size_t req_seq_off = 0;
    size_t rsp_seq_off = 0;
    size_t req_seq_len = 0;
    size_t rsp_seq_len = 0;

    if(!entry || !req || !rsp || !state) { return false; }

    /* Safety: always set response EIP session handle + sender context if possible. */
    if(rsp_len >= 8U) { put_u32_le(rsp + 4U, state->session_handle); }
    if(req_len >= 20U && rsp_len >= 20U) { memcpy(rsp + 12U, req + 12U, 8U); }

    if(get_cpf_item_data_offset(req, req_len, 0x00A1U, 4U, &req_conn_off, &req_conn_len)
       && get_cpf_item_data_offset(rsp, rsp_len, 0x00A1U, 4U, &rsp_conn_off, &rsp_conn_len)) {
        (void)req_conn_len;
        (void)rsp_conn_len;
        /* Echo the request connected address ID back to client. */
        memcpy(rsp + rsp_conn_off, req + req_conn_off, 4U);
    } else if(get_cpf_item_data_offset(rsp, rsp_len, 0x00A1U, 4U, &rsp_conn_off, &rsp_conn_len)) {
        (void)rsp_conn_len;
        put_u32_le(rsp + rsp_conn_off, state->server_connection_id);
    }

    if(get_cpf_item_data_offset(req, req_len, 0x00B1U, 2U, &req_seq_off, &req_seq_len)
       && get_cpf_item_data_offset(rsp, rsp_len, 0x00B1U, 2U, &rsp_seq_off, &rsp_seq_len)) {
        (void)req_seq_off;
        (void)req_seq_len;
        (void)rsp_seq_len;
        /* If dataset rule already patches this field, do not advance sequence twice. */
        if(!entry_has_connected_seq_patch(entry, rsp_seq_off)) { put_u16_le(rsp + rsp_seq_off, state->connected_sequence++); }
    }

    return true;
}

static int recv_all(socket_t sock, uint8_t *buf, size_t len) {
    size_t off = 0;
    while(off < len) {
        size_t chunk = len - off;
        if(chunk > SOCK_LEN_MAX) { chunk = SOCK_LEN_MAX; }
        sock_io_t rc = (sock_io_t)recv(sock, (char *)(buf + off), (sock_len_t)chunk, 0);
        if(rc == 0) { return 0; }
        if(rc < 0) { return -1; }
        off += (size_t)rc;
    }
    return 1;
}

static int send_all(socket_t sock, const uint8_t *buf, size_t len) {
    size_t off = 0;
    while(off < len) {
        size_t chunk = len - off;
        if(chunk > SOCK_LEN_MAX) { chunk = SOCK_LEN_MAX; }
        sock_io_t rc = (sock_io_t)send(sock, (const char *)(buf + off), (sock_len_t)chunk, 0);
        if(rc <= 0) { return -1; }
        off += (size_t)rc;
    }
    return 0;
}

static int handle_client(socket_t csock, const replay_dataset_t *dataset, bool verbose, bool strict_only,
                         session_stats_t *stats) {
    client_state_t state;
    uint8_t hdr[24];
    size_t replay_index = 0;

    memset(&state, 0, sizeof(state));
    state.connected_sequence = 1U;

    if(stats) {
        memset(stats, 0, sizeof(*stats));
        stats->expected_total = dataset->count;
    }

    for(;;) {
        int rr = recv_all(csock, hdr, sizeof(hdr));
        bool is_forward_open = false;
        uint16_t payload_len = 0;
        size_t req_len = 0;
        uint8_t *req = NULL;
        const replay_entry_t *match = NULL;
        uint8_t *rsp = NULL;

        if(rr <= 0) {
            if(stats) { stats->complete = (replay_index == dataset->count); }
            return 0;
        }

        payload_len = get_u16_le(hdr + 2);
        req_len = 24U + (size_t)payload_len;

        if(req_len > MAX_PACKET_SIZE) {
            fprintf(stderr, "ERROR: oversized packet (%zu bytes)\n", req_len);
            if(stats) {
                stats->had_error = true;
                stats->complete = false;
            }
            return -1;
        }

        req = (uint8_t *)malloc(req_len);
        if(!req) {
            if(stats) {
                stats->had_error = true;
                stats->complete = false;
            }
            return -1;
        }

        memcpy(req, hdr, 24U);

        if(payload_len > 0U) {
            rr = recv_all(csock, req + 24U, payload_len);
            if(rr <= 0) {
                free(req);
                if(stats) {
                    stats->had_error = true;
                    stats->complete = false;
                }
                return -1;
            }
        }

        if(replay_index >= dataset->count) {
            fprintf(stderr, "WARN: received extra request after end of dataset, closing client\n");
            free(req);
            if(stats) {
                stats->had_error = true;
                stats->complete = false;
            }
            return -1;
        }

        match = &dataset->entries[replay_index];

        if(!packet_matches(match, req, req_len)) {
            char req_key[128];
            size_t first_diff_idx = (size_t)-1;
            bool strict_ok = false;
            bool tolerant_ok = false;

            strict_ok = packet_matches_with_first_diff(match, req, req_len, &first_diff_idx);
            build_request_key(req, req_len, req_key, sizeof(req_key));

            tolerant_ok = (!strict_ok && match->key && strcmp(match->key, req_key) == 0 && req_len == match->req_len);

            if(strict_only) { tolerant_ok = false; }

            if(!tolerant_ok) {
                fprintf(
                    stderr,
                    "WARN: request order mismatch at dataset index %zu: expected entry id=%d key=%s len=%zu, got key=%s len=%zu\n",
                    replay_index + 1U, match->id, match->key ? match->key : "(null)", match->req_len, req_key, req_len);
                if(first_diff_idx != (size_t)-1 && first_diff_idx < req_len) {
                    fprintf(stderr, "WARN: first strict diff at byte %zu: expected=0x%02x got=0x%02x mask=0x%02x\n",
                            first_diff_idx, match->req_template[first_diff_idx], req[first_diff_idx],
                            match->req_mask[first_diff_idx]);
                }
                free(req);
                if(stats) {
                    stats->had_error = true;
                    stats->complete = false;
                }
                return -1;
            }

            if(verbose) {
                printf("tolerant in-order accept at seq=%zu key=%s (strict diff byte=%zu)\n", replay_index + 1U, req_key,
                       first_diff_idx);
            }
            if(stats) { stats->tolerant_count++; }
        } else if(verbose) {
            printf("in-order match at seq=%zu/%zu entry_id=%d key=%s\n", replay_index + 1U, dataset->count, match->id,
                   match->key ? match->key : "(null)");
        }

        if(stats) { stats->matched_count++; }

        is_forward_open = update_state_from_request(req, req_len, &state);

        rsp = (uint8_t *)malloc(match->rsp_len);
        if(!rsp) {
            free(req);
            if(stats) {
                stats->had_error = true;
                stats->complete = false;
            }
            return -1;
        }

        memcpy(rsp, match->rsp_template, match->rsp_len);

        if(!apply_patch_rules(match, req, req_len, rsp, match->rsp_len, &state)) {
            fprintf(stderr, "ERROR: failed to apply patch rules for entry id=%d\n", match->id);
            free(req);
            free(rsp);
            if(stats) {
                stats->had_error = true;
                stats->complete = false;
            }
            return -1;
        }

        if(!patch_runtime_connected_fields(match, req, req_len, rsp, match->rsp_len, &state)) {
            fprintf(stderr, "ERROR: failed to patch runtime connected fields for entry id=%d\n", match->id);
            free(req);
            free(rsp);
            if(stats) {
                stats->had_error = true;
                stats->complete = false;
            }
            return -1;
        }

        if(is_forward_open) {
            if(!patch_forward_open_response(rsp, match->rsp_len, &state)) {
                fprintf(stderr, "ERROR: failed to patch Forward Open response for entry id=%d\n", match->id);
                free(req);
                free(rsp);
                if(stats) {
                    stats->had_error = true;
                    stats->complete = false;
                }
                return -1;
            }
        }

        if(send_all(csock, rsp, match->rsp_len) != 0) {
            free(req);
            free(rsp);
            if(stats) {
                stats->had_error = true;
                stats->complete = false;
            }
            return -1;
        }

        free(req);
        free(rsp);

        replay_index++;

        if(get_u16_le(hdr + 0) == 0x0066U) {
            if(stats) { stats->complete = (replay_index == dataset->count); }
            return 0;
        }
    }
}

static bool parse_port(const char *s, uint16_t *out) {
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if(end == s || *end != '\0' || v == 0UL || v > 65535UL) { return false; }
    *out = (uint16_t)v;
    return true;
}

static int run_server(const replay_dataset_t *dataset, const char *bind_ip, uint16_t port, bool verbose, bool strict_only) {
    socket_t lsock = INVALID_SOCKET;
    struct sockaddr_in addr;

#ifdef _WIN32
    WSADATA wsa_data;
    if(WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "ERROR: WSAStartup failed\n");
        return 1;
    }
#endif

    lsock = socket(AF_INET, SOCK_STREAM, 0);
    if(lsock == INVALID_SOCKET) {
        fprintf(stderr, "ERROR: socket() failed (%d)\n", SOCK_ERRNO);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    {
        int on = 1;
        setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if(inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "ERROR: invalid bind IP: %s\n", bind_ip);
        CLOSESOCK(lsock);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    if(bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "ERROR: bind() failed (%d)\n", SOCK_ERRNO);
        CLOSESOCK(lsock);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    if(listen(lsock, 16) == SOCKET_ERROR) {
        fprintf(stderr, "ERROR: listen() failed (%d)\n", SOCK_ERRNO);
        CLOSESOCK(lsock);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    printf("replay listening on %s:%u with %zu entries\n", bind_ip, (unsigned)port, dataset->count);

    for(;;) {
        socket_t csock;
        struct sockaddr_in caddr;
        session_stats_t stats;
        int client_rc = 0;
#ifdef _WIN32
        int clen = (int)sizeof(caddr);
#else
        socklen_t clen = (socklen_t)sizeof(caddr);
#endif

        csock = accept(lsock, (struct sockaddr *)&caddr, &clen);
        if(csock == INVALID_SOCKET) {
            fprintf(stderr, "WARN: accept() failed (%d)\n", SOCK_ERRNO);
            continue;
        }

        if(verbose) {
            char ipbuf[64];
            const char *ip = inet_ntop(AF_INET, &caddr.sin_addr, ipbuf, sizeof(ipbuf));
            printf("client connected from %s:%u\n", ip ? ip : "?", (unsigned)ntohs(caddr.sin_port));
        }

        client_rc = handle_client(csock, dataset, verbose, strict_only, &stats);

        printf("session summary: matched=%zu/%zu tolerant=%zu complete=%s error=%s\n", stats.matched_count, stats.expected_total,
               stats.tolerant_count, stats.complete ? "yes" : "no", stats.had_error ? "yes" : "no");

        CLOSESOCK(csock);

        if(client_rc != 0 || !stats.complete) {
            fprintf(stderr, "ERROR: replay session failed or incomplete; exiting with error for CI.\n");
            CLOSESOCK(lsock);
#ifdef _WIN32
            WSACleanup();
#endif
            return 1;
        }
    }

    CLOSESOCK(lsock);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}

int main(int argc, char **argv) {
    const char *dataset_path = NULL;
    const char *bind_ip = "0.0.0.0";
    uint16_t port = 44818U;
    bool verbose = false;
    bool strict_only = false;
    int i = 0;
    replay_dataset_t dataset;

    memset(&dataset, 0, sizeof(dataset));

    for(i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--dataset=", 10) == 0) {
            dataset_path = argv[i] + 10;
        } else if(strncmp(argv[i], "--bind=", 7) == 0) {
            bind_ip = argv[i] + 7;
        } else if(strncmp(argv[i], "--port=", 7) == 0) {
            if(!parse_port(argv[i] + 7, &port)) {
                fprintf(stderr, "ERROR: invalid port: %s\n", argv[i] + 7);
                return 1;
            }
        } else if(strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if(strcmp(argv[i], "--strict-only") == 0) {
            strict_only = true;
        } else if(strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "ERROR: unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if(!dataset_path) {
        fprintf(stderr, "ERROR: --dataset is required.\n");
        usage(argv[0]);
        return 1;
    }

    if(!load_dataset(dataset_path, &dataset)) {
        fprintf(stderr, "ERROR: failed to load dataset from %s\n", dataset_path);
        return 1;
    }

    printf("loaded dataset: %zu entries\n", dataset.count);

    if(run_server(&dataset, bind_ip, port, verbose, strict_only) != 0) {
        free_dataset(&dataset);
        return 1;
    }

    free_dataset(&dataset);
    return 0;
}

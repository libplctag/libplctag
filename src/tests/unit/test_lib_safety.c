/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
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

/*
 * API safety checks in lib.c (review C1, C4, C6, C7, C8).  lib.c is compiled in so the test can
 * hook rc_dec() and mem_free(), reach static functions and set tag state by hand.
 *
 * C1: an API call must touch nothing after it drops its tag reference.  The rc_dec() hook
 * poisons tag_id and status at the release; a later write shows in status, a later read of
 * tag_id shows in the logger.  The test holds its own reference, so the tag stays valid.
 */

/* the release builds cap pdebug() at INFO; these checks count DETAIL lines. */
#undef PLCTAG_COMPILE_DEBUG_LEVEL

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <platform.h>
#include <utils/rc.h>

static void *test_rc_dec(const char *func, int line_num, void *ref);
static void test_mem_free(const void *mem);

#undef rc_dec
#define rc_dec(ref) test_rc_dec(__func__, __LINE__, ref)
#define mem_free(mem) test_mem_free(mem)

#include "../../libplctag/lib/lib.c"

#define SYSTEM_TAG "protocol=system&name=version"
#define POISON_TAG_ID (INT32_C(-1234567))
#define SENTINEL_STATUS (77)
#define MAX_FREED (32)

#define check(cond, what)                                                          \
    do {                                                                           \
        if(!(cond)) {                                                              \
            fprintf(stderr, "FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, what, #cond); \
            failures++;                                                            \
        }                                                                          \
    } while(0)

static int32_t failures = 0;

static const char *watch_func = NULL;
static plc_tag_p watch_tag = NULL;
static int32_t saved_tag_id = 0;
static bool released = false;
static bool saw_poison = false;

static int32_t logger_lines = 0;
static int32_t logger_bad = 0;

static const void *freed[MAX_FREED];
static size_t freed_count = 0;


static void *test_rc_dec(const char *func, int line_num, void *ref) {
    if(watch_func && ref == (void *)watch_tag && strcmp(func, watch_func) == 0 && !released) {
        released = true;
        saved_tag_id = watch_tag->tag_id;
        watch_tag->tag_id = POISON_TAG_ID;
        watch_tag->status = SENTINEL_STATUS;
    }

    return rc_dec_impl(func, line_num, ref);
}


static void test_mem_free(const void *mem) {
    if(freed_count < MAX_FREED) { freed[freed_count++] = mem; }
    (mem_free)(mem);
}


/* the line came from func: the debug prefix names it as " func:<line>". */
static bool logged_by(const char *message, const char *func) {
    size_t len = strlen(func);

    for(const char *at = strstr(message, func); at; at = strstr(at + 1, func)) {
        if(at > message && at[-1] == ' ' && at[len] == ':') { return true; }
    }

    return false;
}


static void logger(int32_t tag_id, int debug_level, const char *message) {
    const char *func = watch_func;

    /* C1 is about the API call's own code.  The tickler thread logs the tag's ID too, but under its own reference. */
    if(tag_id == POISON_TAG_ID && func && logged_by(message, func)) { saw_poison = true; }

    if(strstr(message, " plc_tag_register_logger:") || strstr(message, " plc_tag_unregister_logger:")) {
        logger_lines++;
        if(debug_level != PLCTAG_DEBUG_DETAIL || tag_id != 0) { logger_bad++; }
    }
}


static void ext_callback(int32_t tag_id, int event, int status, void *user_data) {
    (void)tag_id;
    (void)event;
    (void)status;
    (void)user_data;
}


static void watch(const char *func, plc_tag_p tag) {
    watch_func = func;
    watch_tag = tag;
    released = false;
    saw_poison = false;
}


static void check_untouched(const char *func) {
    if(!released) {
        fprintf(stderr, "FAIL %s: never released its reference.\n", func);
        failures++;
        watch_func = NULL;
        return;
    }

    if(watch_tag->status != SENTINEL_STATUS) {
        fprintf(stderr, "FAIL %s: wrote tag->status after rc_dec() (C1).\n", func);
        failures++;
    }

    if(saw_poison) {
        fprintf(stderr, "FAIL %s: read tag->tag_id after rc_dec() (C1).\n", func);
        failures++;
    }

    watch_tag->tag_id = saved_tag_id;
    watch_func = NULL;
}


/* C1: every path here ends in an rc_dec(); none may touch the tag after it. */
static void test_no_touch_after_release(void) {
    static tag_byte_order_t no_strings = {.int16_order = {0, 1},
                                          .int32_order = {0, 1, 2, 3},
                                          .int64_order = {0, 1, 2, 3, 4, 5, 6, 7},
                                          .float32_order = {0, 1, 2, 3},
                                          .float64_order = {0, 1, 2, 3, 4, 5, 6, 7}};
    int32_t id = plc_tag_create(SYSTEM_TAG, 0);
    plc_tag_p tag = lookup_tag(id);
    tag_byte_order_t *old_order = NULL;
    uint8_t buf[4] = {0};
    uint8_t *data = NULL;

    printf("C1: no tag access after the API call releases its reference.\n");

    if(!tag) {
        check(tag != NULL, "system tag");
        return;
    }

    /* with no strings defined the string calls take their early exits. */
    old_order = tag->byte_order;
    tag->byte_order = &no_strings;

    watch("plc_tag_get_string_capacity", tag);
    (void)plc_tag_get_string_capacity(id, 0);
    check_untouched("plc_tag_get_string_capacity");

    watch("plc_tag_get_string_length", tag);
    (void)plc_tag_get_string_length(id, 0);
    check_untouched("plc_tag_get_string_length");

    watch("plc_tag_get_string_total_length", tag);
    (void)plc_tag_get_string_total_length(id, 0);
    check_untouched("plc_tag_get_string_total_length");

    watch("plc_tag_get_string", tag);
    (void)plc_tag_get_string(id, 0, (char *)buf, (int)sizeof(buf));
    check_untouched("plc_tag_get_string");

    watch("plc_tag_set_string", tag);
    (void)plc_tag_set_string(id, 0, "x");
    check_untouched("plc_tag_set_string");

    tag->byte_order = old_order;

    watch("plc_tag_register_callback_ex", tag);
    (void)plc_tag_register_callback_ex(id, ext_callback, NULL);
    check_untouched("plc_tag_register_callback_ex");

    watch("plc_tag_unregister_callback", tag);
    (void)plc_tag_unregister_callback(id);
    check_untouched("plc_tag_unregister_callback");

    /* no data takes the raw byte calls' first early exit. */
    data = tag->data;
    tag->data = NULL;

    watch("plc_tag_set_raw_bytes", tag);
    (void)plc_tag_set_raw_bytes(id, 0, buf, (int)sizeof(buf));
    check_untouched("plc_tag_set_raw_bytes");

    watch("plc_tag_get_raw_bytes", tag);
    (void)plc_tag_get_raw_bytes(id, 0, buf, (int)sizeof(buf));
    check_untouched("plc_tag_get_raw_bytes");

    tag->data = data;

    rc_dec_impl(__func__, __LINE__, tag);
    plc_tag_destroy(id);
}


/* C4: a string that changes size must not resize the buffer under an operation in flight. */
static void test_set_string_busy(void) {
    static tag_byte_order_t order = {.str_is_defined = 1,
                                     .str_is_counted = 1,
                                     .str_count_word_bytes = 4,
                                     .str_max_capacity = 82,
                                     .int16_order = {0, 1},
                                     .int32_order = {0, 1, 2, 3},
                                     .int64_order = {0, 1, 2, 3, 4, 5, 6, 7},
                                     .float32_order = {0, 1, 2, 3},
                                     .float64_order = {0, 1, 2, 3, 4, 5, 6, 7}};
    int32_t id = plc_tag_create(SYSTEM_TAG, 0);
    plc_tag_p tag = lookup_tag(id);
    tag_byte_order_t *old_order = NULL;
    uint8_t *old_data = NULL;
    int32_t old_size = 0;
    int32_t rc = 0;

    printf("C4: plc_tag_set_string() refuses to resize while an operation is in flight.\n");

    if(!tag) {
        check(tag != NULL, "system tag");
        return;
    }

    old_order = tag->byte_order;
    old_data = tag->data;
    old_size = tag->size;

    /* count word 3, then "abc". */
    tag->byte_order = &order;
    tag->data = mem_alloc(7);
    tag->size = 7;
    tag->data[0] = 3;
    mem_copy(&tag->data[4], "abc", 3);
    tag->allow_field_resize = 1;
    tag->data_is_fixed = 0;

    tag->read_in_flight = 1;
    rc = plc_tag_set_string(id, 0, "abcdef");
    check(rc == PLCTAG_ERR_BUSY, "C4: set_string during a read returns PLCTAG_ERR_BUSY");
    check(tag->size == 7, "C4: buffer not resized during a read");
    tag->read_in_flight = 0;

    rc = plc_tag_set_string(id, 0, "abcdef");
    check(rc == PLCTAG_STATUS_OK, "C4: set_string with nothing in flight succeeds");
    check(tag->size == 10, "C4: buffer resized with nothing in flight");

    (mem_free)(tag->data);
    tag->data = old_data;
    tag->size = old_size;
    tag->byte_order = old_order;
    tag->allow_field_resize = 0;
    tag->data_is_fixed = 1;

    rc_dec_impl(__func__, __LINE__, tag);
    plc_tag_destroy(id);
}


/* C6: a NULL tag is reported, not dereferenced, with logging on. */
static void test_wake_null_tag(void) {
    printf("C6: plc_tag_generic_wake_tag_impl(NULL) returns PLCTAG_ERR_NULL_PTR.\n");

    check(plc_tag_generic_wake_tag_impl(__func__, __LINE__, NULL) == PLCTAG_ERR_NULL_PTR, "C6: NULL tag");
}


/* C7: the logger calls log at DETAIL with tag ID 0. */
static void test_logger_log_level(void) {
    printf("C7: plc_tag_register_logger() and plc_tag_unregister_logger() log at DETAIL for tag 0.\n");

    logger_lines = 0;
    logger_bad = 0;

    (void)plc_tag_unregister_logger();
    (void)plc_tag_register_logger(logger);

    check(logger_lines > 0, "C7: logger calls logged through the callback");
    check(logger_bad == 0, "C7: logger calls log at DETAIL with tag ID 0");
}


/* C8: replacing an allocated byte order frees the old one, or reuses it. */
static void test_byte_order_no_leak(void) {
    int32_t id = plc_tag_create(SYSTEM_TAG, 0);
    plc_tag_p tag = lookup_tag(id);
    attr attribs = attr_create_from_str(SYSTEM_TAG "&int16_byte_order=10");
    tag_byte_order_t *first = NULL;
    bool first_freed = false;

    printf("C8: a second byte order override does not leak the first.\n");

    if(!tag || !attribs) {
        check(tag && attribs, "system tag and attributes");
        return;
    }

    check(set_tag_byte_order(tag, attribs) == PLCTAG_STATUS_OK, "C8: first override");
    first = tag->byte_order;
    check(first && first->is_allocated, "C8: first override allocates");

    freed_count = 0;
    check(set_tag_byte_order(tag, attribs) == PLCTAG_STATUS_OK, "C8: second override");

    for(size_t i = 0; i < freed_count; i++) {
        if(freed[i] == first) { first_freed = true; }
    }

    check(tag->byte_order == first || first_freed, "C8: the first allocated byte order is reused or freed");

    attr_destroy(attribs);
    rc_dec_impl(__func__, __LINE__, tag);
    plc_tag_destroy(id);
}


/* plc_tag_get_int64_attribute(): no protocol has a 64-bit attribute yet, so the test gives a
 * system tag a copy of its vtable whose table holds one. */
#define TEST_INT64_VALUE (INT64_C(0x123456789ABCDEF0))
#define TEST_INT64_DEFAULT (INT64_C(-42))

static int32_t test_get_int64_ok(plc_tag_p tag, int64_t *result) {
    (void)tag;
    *result = TEST_INT64_VALUE;
    return PLCTAG_STATUS_OK;
}

static int32_t test_get_int64_fails(plc_tag_p tag, int64_t *result) {
    (void)tag;
    (void)result;
    return PLCTAG_ERR_NOT_FOUND;
}

static const attr_def_t test_int64_attribs[] = {
    {.name = "testint64", .type = ATTR_TYPE_INT64, .description = "test", .get_int64 = test_get_int64_ok},
    {.name = "testint64fails", .type = ATTR_TYPE_INT64, .description = "test", .get_int64 = test_get_int64_fails},
    {.name = NULL},
};

static void test_get_int64_attribute(void) {
    int32_t id = plc_tag_create(SYSTEM_TAG, 0);
    plc_tag_p tag = lookup_tag(id);
    struct tag_vtable_t test_vtable;
    tag_vtable_p saved_vtable = NULL;

    printf("plc_tag_get_int64_attribute() reads a 64-bit attribute whole.\n");

    if(!tag || !tag->vtable) {
        check(tag && tag->vtable, "system tag");
        return;
    }

    saved_vtable = tag->vtable;
    test_vtable = *saved_vtable;
    test_vtable.attribs = test_int64_attribs;
    tag->vtable = &test_vtable;

    check(plc_tag_get_int64_attribute(id, "test_int64", TEST_INT64_DEFAULT) == TEST_INT64_VALUE, "int64: value read whole");
    check(plc_tag_status(id) == PLCTAG_STATUS_OK, "int64: status OK after a good read");
    check(plc_tag_get_attribute_size(id, "test_int64") == (int)sizeof(int64_t), "int64: size is 8");

    /* the 32-bit getter does not truncate a 64-bit value; it refuses it. */
    check(plc_tag_get_int_attribute(id, "test_int64", -7) == -7, "int64: the 32-bit getter refuses it");

    /* a getter's error comes back as the default and the status. */
    check(plc_tag_get_int64_attribute(id, "test_int64_fails", TEST_INT64_DEFAULT) == TEST_INT64_DEFAULT,
          "int64: a failing getter returns the default");

    tag->vtable = saved_vtable;
    rc_dec_impl(__func__, __LINE__, tag);
    plc_tag_destroy(id);
}


int main(void) {
    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);
    (void)plc_tag_register_logger(logger);

    test_no_touch_after_release();
    test_set_string_busy();
    test_logger_log_level();
    test_byte_order_no_leak();
    test_get_int64_attribute();

    /* last: under UBSan the C6 defect aborts the run. */
    test_wake_null_tag();

    (void)plc_tag_unregister_logger();
    plc_tag_shutdown();

    printf("%s: %" PRId32 " failure(s).\n", failures ? "FAILED" : "PASSED", failures);

    return failures ? 1 : 0;
}

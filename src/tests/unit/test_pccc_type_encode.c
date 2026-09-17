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
 * Byte-exact expectations for pccc_encode_dt_byte() and pccc_encode_type_info().
 *
 * pccc_encode_dt_byte() shipped uncalled for years, so nothing had ever run it.
 * eip_lgx_pccc.c kept the type descriptor that came back on a read instead of
 * building one, which is what 1.10 in docs/deferred_fixes.md is about.  These
 * cases are the first exercise it has had, and three of them cover bugs that
 * were in it the whole time.
 *
 * The anchor for the whole file is a real reply.  A ControlLogix answered a
 * typed read of one N7:0 element with the descriptor
 *
 *      99 09 03 42
 *
 * which decodes as an array of three bytes -- one element descriptor plus two
 * bytes of data -- holding one AB_PCCC_DATA_INT of two bytes.  The encoder
 * produces 93 09 42 for the same tag: the same meaning, one byte shorter,
 * because it puts a size of 3 in the nybble where the PLC escaped it.  Both
 * forms are checked here, and the round-trip test proves they decode alike.
 * The same CPU accepted a write carrying the shorter form, so both are known
 * good on hardware.
 */

#include "mini_mock.h"

#include <libplctag/lib/libplctag.h>
#include <libplctag/protocols/ab/defs.h>
#include <libplctag/protocols/ab/tag.h>
#include <libplctag/protocols/ab/pccc.h>
#include <stdio.h>
#include <string.h>


#define MAX_EXPECTED (16)


typedef struct {
    const char *name;
    uint32_t data_type;
    uint32_t data_size;
    int buf_size;

    /* the number of bytes expected, or 0 when the call is expected to refuse. */
    int expected_size;
    uint8_t expected[MAX_EXPECTED];
} dt_byte_case_t;


static const dt_byte_case_t dt_byte_cases[] = {
    /* both nybbles hold their value: a two byte integer element. */
    {"INT of 2 bytes", AB_PCCC_DATA_INT, 2, 16, 1, {0x42}},
    {"REAL of 4 bytes", AB_PCCC_DATA_REAL, 4, 16, 2, {0x94, 0x08}},
    {"type 0, size 0", 0, 0, 16, 1, {0x00}},
    {"largest inline pair", 7, 7, 16, 1, {0x77}},

    /*
     * Type 8 and up does not fit in three bits, so the nybble becomes 0x08 plus a
     * byte count and the value follows little-endian.  AB_PCCC_DATA_ARRAY is 9,
     * which is the case every array descriptor hits.
     */
    {"ARRAY of 3 bytes", AB_PCCC_DATA_ARRAY, 3, 16, 2, {0x93, 0x09}},
    {"both nybbles escaped", AB_PCCC_DATA_REAL, 8, 16, 3, {0x99, 0x08, 0x08}},
    {"BCD needs an extension byte", AB_PCCC_DATA_BCD, 2, 16, 2, {0x92, 0x10}},

    /*
     * WAS BROKEN.  Both extension loops ran while (value & 0xFF) rather than while
     * (value), so a size whose low byte is zero ended the loop before it started.
     * 256 is the first such size and an array of 255 single byte elements plus its
     * element descriptor is exactly 256.
     */
    {"size 256 needs two bytes", AB_PCCC_DATA_ARRAY, 256, 16, 4, {0x9A, 0x09, 0x00, 0x01}},
    {"size 65536 needs three", AB_PCCC_DATA_ARRAY, 65536, 16, 5, {0x9B, 0x09, 0x00, 0x00, 0x01}},

    /*
     * WAS BROKEN.  The type loop tested the SIZE -- its condition read
     * (data_type & 0xFF) && data_size -- so an extended type with a zero size wrote
     * no extension bytes and then failed its own consistency check.
     */
    {"extended type, zero size", AB_PCCC_DATA_ARRAY, 0, 16, 2, {0x90, 0x09}},

    /*
     * WAS BROKEN.  Bytes went into the buffer before its size was checked, and the
     * check that followed counted exactly filling the buffer as a failure.
     */
    {"exactly fills the buffer", AB_PCCC_DATA_ARRAY, 3, 2, 2, {0x93, 0x09}},
    {"one byte short", AB_PCCC_DATA_ARRAY, 3, 1, 0, {0}},
    {"no room at all", AB_PCCC_DATA_INT, 2, 0, 0, {0}},
};


static void test_dt_byte_table(void **state) {
    (void)state;

    for(size_t i = 0; i < sizeof(dt_byte_cases) / sizeof(dt_byte_cases[0]); i++) {
        const dt_byte_case_t *test_case = &dt_byte_cases[i];
        uint8_t buf[MAX_EXPECTED];
        int rc = 0;

        memset(buf, 0xAA, sizeof(buf));

        rc = pccc_encode_dt_byte(buf, test_case->buf_size, test_case->data_type, test_case->data_size);

        if(rc != test_case->expected_size) {
            fprintf(stderr, "case \"%s\": expected %d bytes, got %d\n", test_case->name, test_case->expected_size, rc);
        }

        assert_int_equal(rc, test_case->expected_size);

        for(int byte_index = 0; byte_index < test_case->expected_size; byte_index++) {
            if(buf[byte_index] != test_case->expected[byte_index]) {
                fprintf(stderr, "case \"%s\": byte %d expected 0x%02x, got 0x%02x\n", test_case->name, byte_index,
                        test_case->expected[byte_index], buf[byte_index]);
            }

            assert_int_equal(buf[byte_index], test_case->expected[byte_index]);
        }
    }
}


/*
 * Whatever the encoder writes, the decoder must read back unchanged.  This is the
 * check that matters most, because the two halves disagreeing is how a wrong type
 * byte would reach a PLC without any single case here looking wrong.
 */
static void test_dt_byte_round_trip(void **state) {
    static const uint32_t values[] = {0, 1, 7, 8, 15, 16, 255, 256, 257, 65535, 65536, 0x00FFFFFF, 0x01000000};

    (void)state;

    for(size_t type_index = 0; type_index < sizeof(values) / sizeof(values[0]); type_index++) {
        for(size_t size_index = 0; size_index < sizeof(values) / sizeof(values[0]); size_index++) {
            uint8_t buf[MAX_EXPECTED];
            int written = 0;
            int decoded_type = 0;
            int decoded_size = 0;
            uint8_t *end = NULL;

            memset(buf, 0, sizeof(buf));

            written = pccc_encode_dt_byte(buf, (int)sizeof(buf), values[type_index], values[size_index]);
            assert_int_equal(written > 0, 1);

            /*
             * pccc_decode_dt_byte() refuses a buffer holding nothing but the
             * descriptor -- its bounds check reserves at least one data byte -- so it
             * is given the whole buffer, which is what a real reply looks like.
             */
            end = pccc_decode_dt_byte(buf, (int)sizeof(buf), &decoded_type, &decoded_size);

            if(!end) {
                fprintf(stderr, "round trip type %u size %u: decode refused %d encoded bytes\n",
                        (unsigned int)values[type_index], (unsigned int)values[size_index], written);
            }

            assert_int_equal(end != NULL, 1);
            assert_int_equal((int)(end - buf), written);
            assert_int_equal((uint32_t)decoded_type, values[type_index]);
            assert_int_equal((uint32_t)decoded_size, values[size_index]);
        }
    }
}


typedef struct {
    const char *name;
    pccc_file_t file_type;
    int elem_size;
    int elem_count;

    /* a negative value is the status expected instead of a byte count. */
    int expected_size;
    uint8_t expected[MAX_EXPECTED];
} type_info_case_t;


static const type_info_case_t type_info_cases[] = {
    /*
     * The anchor.  A ControlLogix sent 99 09 03 42 for this tag; the encoder's
     * 93 09 42 says the same thing with the array size inline instead of escaped.
     */
    {"N7:0, one INT element", PCCC_FILE_INT, 2, 1, 3, {0x93, 0x09, 0x42}},

    /* ten INTs: the array covers the element descriptor plus 20 bytes of data. */
    {"N7:0, ten INT elements", PCCC_FILE_INT, 2, 10, 4, {0x99, 0x09, 0x15, 0x42}},

    /*
     * 128 INTs: the array covers the element descriptor plus 256 bytes, so its size needs
     * two extension bytes.  This is the case the old encoder could not express at all.
     */
    {"N7:0, 128 INT elements", PCCC_FILE_INT, 2, 128, 5, {0x9A, 0x09, 0x01, 0x01, 0x42}},

    /*
     * Every file type but N is left to the PLC.  They each line up with an AB_PCCC_DATA_*
     * value by name, which is why they are refused rather than guessed: only N has been on
     * a wire and had its write accepted.  If one of these ever starts encoding, the change
     * should come with a probe run against a PLC that has the data file.
     */
    {"float file is unsupported", PCCC_FILE_FLOAT, 4, 1, PLCTAG_ERR_UNSUPPORTED, {0}},
    {"bit file is unsupported", PCCC_FILE_BIT, 2, 1, PLCTAG_ERR_UNSUPPORTED, {0}},
    {"timer file is unsupported", PCCC_FILE_TIMER, 6, 1, PLCTAG_ERR_UNSUPPORTED, {0}},
    {"counter file is unsupported", PCCC_FILE_COUNTER, 6, 1, PLCTAG_ERR_UNSUPPORTED, {0}},
    {"control file is unsupported", PCCC_FILE_CONTROL, 6, 1, PLCTAG_ERR_UNSUPPORTED, {0}},
    {"BCD file is unsupported", PCCC_FILE_BCD, 2, 1, PLCTAG_ERR_UNSUPPORTED, {0}},
    {"status file is unsupported", PCCC_FILE_STATUS, 2, 1, PLCTAG_ERR_UNSUPPORTED, {0}},
    {"string file is unsupported", PCCC_FILE_STRING, 84, 1, PLCTAG_ERR_UNSUPPORTED, {0}},
    {"long int file is unsupported", PCCC_FILE_LONG_INT, 4, 1, PLCTAG_ERR_UNSUPPORTED, {0}},
    {"unknown file is unsupported", PCCC_FILE_UNKNOWN, 2, 1, PLCTAG_ERR_UNSUPPORTED, {0}},

    /* nonsense geometry is refused rather than encoded. */
    {"zero elements", PCCC_FILE_INT, 2, 0, PLCTAG_ERR_BAD_PARAM, {0}},
    {"zero element size", PCCC_FILE_INT, 0, 1, PLCTAG_ERR_BAD_PARAM, {0}},
};


static void test_type_info_table(void **state) {
    (void)state;

    for(size_t i = 0; i < sizeof(type_info_cases) / sizeof(type_info_cases[0]); i++) {
        const type_info_case_t *test_case = &type_info_cases[i];
        uint8_t buf[MAX_EXPECTED];
        int rc = 0;

        memset(buf, 0xAA, sizeof(buf));

        rc = pccc_encode_type_info(buf, (int)sizeof(buf), test_case->file_type, test_case->elem_size,
                                   test_case->elem_count);

        if(rc != test_case->expected_size) {
            fprintf(stderr, "case \"%s\": expected %d, got %d\n", test_case->name, test_case->expected_size, rc);
        }

        assert_int_equal(rc, test_case->expected_size);

        if(test_case->expected_size <= 0) { continue; }

        for(int byte_index = 0; byte_index < test_case->expected_size; byte_index++) {
            if(buf[byte_index] != test_case->expected[byte_index]) {
                fprintf(stderr, "case \"%s\": byte %d expected 0x%02x, got 0x%02x\n", test_case->name, byte_index,
                        test_case->expected[byte_index], buf[byte_index]);
            }

            assert_int_equal(buf[byte_index], test_case->expected[byte_index]);
        }
    }
}


/*
 * The descriptor the PLC sent and the one the encoder builds must decode to the same
 * type and size.  If this ever fails, the shorter form is not equivalent and
 * eip_lgx_pccc.c must send the escaped form instead.
 */
static void test_encoder_matches_the_plc(void **state) {
    /* 99 09 03 42, then two bytes of element data, exactly as N7:0 came back. */
    static uint8_t from_plc[] = {0x99, 0x09, 0x03, 0x42, 0x2b, 0x00};
    uint8_t built[MAX_EXPECTED];
    uint8_t *plc_data = NULL;
    uint8_t *built_data = NULL;
    int plc_type = 0, plc_size = 0, built_type = 0, built_size = 0;
    int written = 0;

    (void)state;

    memset(built, 0, sizeof(built));

    written = pccc_encode_type_info(built, (int)sizeof(built), PCCC_FILE_INT, 2, 1);
    assert_int_equal(written, 3);

    /* the outer array descriptor */
    plc_data = pccc_decode_dt_byte(from_plc, (int)sizeof(from_plc), &plc_type, &plc_size);
    built_data = pccc_decode_dt_byte(built, (int)sizeof(built), &built_type, &built_size);

    assert_int_equal(plc_data != NULL, 1);
    assert_int_equal(built_data != NULL, 1);
    assert_int_equal(plc_type, AB_PCCC_DATA_ARRAY);
    assert_int_equal(built_type, plc_type);
    assert_int_equal(plc_size, 3);
    assert_int_equal(built_size, plc_size);

    /* the inner element descriptor */
    plc_data = pccc_decode_dt_byte(plc_data, (int)(sizeof(from_plc) - (size_t)(plc_data - from_plc)), &plc_type, &plc_size);
    built_data = pccc_decode_dt_byte(built_data, (int)(sizeof(built) - (size_t)(built_data - built)), &built_type, &built_size);

    assert_int_equal(plc_data != NULL, 1);
    assert_int_equal(built_data != NULL, 1);
    assert_int_equal(plc_type, AB_PCCC_DATA_INT);
    assert_int_equal(built_type, plc_type);
    assert_int_equal(plc_size, 2);
    assert_int_equal(built_size, plc_size);
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_dt_byte_table),
        cmocka_unit_test(test_dt_byte_round_trip),
        cmocka_unit_test(test_type_info_table),
        cmocka_unit_test(test_encoder_matches_the_plc),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}

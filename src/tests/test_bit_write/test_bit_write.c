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
 * Prove that writing one bit changes one bit.
 *
 * A bit write is a read-modify-write everywhere the library implements it: the CIP masked
 * write, the PLC/5 AND/OR mask pair, and the SLC mask-then-data word.  A mask that is too
 * wide still reads back correctly through the bit tag that wrote it, so --check on a bit tag
 * cannot see the damage.  Only reading the whole enclosing word can.
 *
 * The method: stamp a known alternating pattern on the word, flip one bit through the bit
 * tag, and read the word back.  Exactly one bit may differ, and it must be the one asked
 * for.  Both polarities are exercised, because an AND mask and an OR mask fail differently.
 *
 * The original word is restored on the way out, including after a failure, so running this
 * against a live PLC leaves nothing behind.
 */

#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/api/libplctag.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 2, 1
#define DEFAULT_TIMEOUT (5000)

/*
 * Alternating bits, so a mask that smears into the neighbours changes the readback whichever
 * direction it smears.  An all-zero or all-one word would hide half of that.
 */
#define DEFAULT_PATTERN ((uint64_t)0x5A5A5A5A5A5A5A5AULL)

static void usage(void);
static int32_t open_tag(const char *tag_string, int timeout);
static int read_word(int32_t tag, int word_size, int timeout, uint64_t *result);
static int write_word(int32_t tag, int word_size, int timeout, uint64_t value);
static int write_bit(int32_t tag, int timeout, int value, bool refresh);
static int check_word(uint64_t expected, uint64_t actual, int bit, int value);


int main(int argc, char **argv) {
    const char *word_tag_string = NULL;
    const char *bit_tag_string = NULL;
    int bit = -1;
    int timeout = DEFAULT_TIMEOUT;
    int debug = 0;
    uint64_t pattern = DEFAULT_PATTERN;
    uint64_t original = 0;
    uint64_t actual = 0;
    uint64_t mask = 0;
    int word_size = 0;
    bool refresh = true;
    int32_t word_tag = PLCTAG_ERR_CREATE;
    int32_t bit_tag = PLCTAG_ERR_CREATE;
    int rc = PLCTAG_STATUS_OK;
    bool original_saved = false;

    for(int i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--word-tag=", 11) == 0) {
            word_tag_string = &(argv[i][11]);
        } else if(strncmp(argv[i], "--bit-tag=", 10) == 0) {
            bit_tag_string = &(argv[i][10]);
        } else if(strncmp(argv[i], "--bit=", 6) == 0) {
            bit = atoi(&(argv[i][6]));
        } else if(strncmp(argv[i], "--pattern=", 10) == 0) {
            pattern = strtoull(&(argv[i][10]), NULL, 0);
        } else if(strncmp(argv[i], "--timeout=", 10) == 0) {
            timeout = atoi(&(argv[i][10]));
        } else if(strncmp(argv[i], "--debug=", 8) == 0) {
            debug = atoi(&(argv[i][8]));
        } else if(strcmp(argv[i], "--stale") == 0) {
            refresh = false;
        } else {
            printf("ERROR: unknown argument \"%s\"!\n", argv[i]);
            usage();
            return 1;
        }
    }

    if(!word_tag_string || !bit_tag_string || bit < 0) {
        printf("ERROR: --word-tag, --bit-tag and --bit are all required!\n");
        usage();
        return 1;
    }

    if(bit > 63) {
        printf("ERROR: --bit must be 0 through 63!\n");
        return 1;
    }

    if(timeout <= 0) {
        printf("ERROR: --timeout must be greater than zero!\n");
        return 1;
    }

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("ERROR: Library version %d.%d.%d requested, but linked library is not compatible!\n", REQUIRED_VERSION);
        return 1;
    }

    if(debug > 0) { plc_tag_set_debug_level(debug); }

    mask = (uint64_t)1 << (unsigned int)bit;

    do {
        word_tag = open_tag(word_tag_string, timeout);
        if(word_tag < 0) {
            rc = word_tag;
            break;
        }

        bit_tag = open_tag(bit_tag_string, timeout);
        if(bit_tag < 0) {
            rc = bit_tag;
            break;
        }

        word_size = plc_tag_get_size(word_tag);
        if(word_size != 1 && word_size != 2 && word_size != 4 && word_size != 8) {
            printf("ERROR: the word tag is %d bytes; it must be 1, 2, 4 or 8 and hold exactly one element!\n", word_size);
            rc = PLCTAG_ERR_BAD_PARAM;
            break;
        }

        if(bit >= word_size * 8) {
            printf("ERROR: bit %d is outside a %d byte word!\n", bit, word_size);
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            break;
        }

        /* keep the word size from truncating the comparison later. */
        if(word_size < 8) { pattern &= ((uint64_t)1 << (unsigned int)(word_size * 8)) - 1; }

        rc = read_word(word_tag, word_size, timeout, &original);
        if(rc != PLCTAG_STATUS_OK) { break; }

        original_saved = true;

        printf("Original word value is 0x%" PRIx64 ".\n", original);

        /* stamp the pattern and confirm it took, or the rest of the test proves nothing. */
        rc = write_word(word_tag, word_size, timeout, pattern);
        if(rc != PLCTAG_STATUS_OK) { break; }

        rc = read_word(word_tag, word_size, timeout, &actual);
        if(rc != PLCTAG_STATUS_OK) { break; }

        if(actual != pattern) {
            printf("ERROR: wrote pattern 0x%" PRIx64 " to the word but read back 0x%" PRIx64 "!\n", pattern, actual);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        printf("Pattern 0x%" PRIx64 " written to the word.\n", pattern);

        /* set the bit: every other bit must survive. */
        rc = write_bit(bit_tag, timeout, 1, refresh);
        if(rc != PLCTAG_STATUS_OK) { break; }

        rc = read_word(word_tag, word_size, timeout, &actual);
        if(rc != PLCTAG_STATUS_OK) { break; }

        rc = check_word(pattern | mask, actual, bit, 1);
        if(rc != PLCTAG_STATUS_OK) { break; }

        /* clear the bit: an AND mask that is too wide fails here and not above. */
        rc = write_bit(bit_tag, timeout, 0, refresh);
        if(rc != PLCTAG_STATUS_OK) { break; }

        rc = read_word(word_tag, word_size, timeout, &actual);
        if(rc != PLCTAG_STATUS_OK) { break; }

        rc = check_word(pattern & ~mask, actual, bit, 0);
        if(rc != PLCTAG_STATUS_OK) { break; }

        printf("PASS: setting and clearing bit %d changed only bit %d.\n", bit, bit);
    } while(0);

    /* put the word back however this ended. */
    if(original_saved) {
        int restore_rc = write_word(word_tag, word_size, timeout, original);

        if(restore_rc != PLCTAG_STATUS_OK) {
            printf("WARNING: unable to restore the original word value 0x%" PRIx64 ", %s!\n", original,
                   plc_tag_decode_error(restore_rc));
        } else {
            printf("Original word value restored.\n");
        }
    }

    if(bit_tag >= 0) { plc_tag_destroy(bit_tag); }
    if(word_tag >= 0) { plc_tag_destroy(word_tag); }

    plc_tag_shutdown();

    return (rc == PLCTAG_STATUS_OK) ? 0 : 1;
}


void usage(void) {
    printf("Usage:\n"
           "test_bit_write --word-tag=<tag string> --bit-tag=<tag string> --bit=<n>\n"
           "               [--pattern=<value>] [--timeout=<ms>] [--debug=<n>]\n"
           "\n"
           "  --word-tag  A tag string addressing the whole word that contains the bit.  It must\n"
           "              hold exactly one element of 1, 2, 4 or 8 bytes.\n"
           "  --bit-tag   A tag string addressing one bit of that same word.\n"
           "  --bit       Which bit of the word --bit-tag addresses, counting from 0.  The two\n"
           "              tag strings spell this differently per protocol, so it is given here.\n"
           "  --pattern   The value stamped on the word before each bit write.  Default is an\n"
           "              alternating 0x5A5A... truncated to the word size.\n"
           "  --stale     Do not read the bit tag before writing it, so its buffer is whatever it\n"
           "              held at create time.  The word must survive that too, which only a\n"
           "              device that applies the mask can promise.\n"
           "  --timeout   Milliseconds per operation.  Default 5000.\n"
           "  --debug     Library debug level, 1 through 5.\n"
           "\n"
           "Example:\n"
           "  test_bit_write --bit=6 \\\n"
           "    --word-tag='protocol=ab-eip&gateway=10.1.2.3&plc=micrologix&name=B3:0' \\\n"
           "    --bit-tag='protocol=ab-eip&gateway=10.1.2.3&plc=micrologix&name=B3:0/6'\n");
}


int32_t open_tag(const char *tag_string, int timeout) {
    int32_t tag = plc_tag_create(tag_string, timeout);

    if(tag < 0) {
        printf("ERROR: unable to create tag \"%s\", %s!\n", tag_string, plc_tag_decode_error(tag));
        return tag;
    }

    if(plc_tag_status(tag) != PLCTAG_STATUS_OK) {
        int status = plc_tag_status(tag);

        printf("ERROR: tag \"%s\" is not usable, %s!\n", tag_string, plc_tag_decode_error(status));
        plc_tag_destroy(tag);

        return status;
    }

    return tag;
}


int read_word(int32_t tag, int word_size, int timeout, uint64_t *result) {
    int rc = plc_tag_read(tag, timeout);

    if(rc != PLCTAG_STATUS_OK) {
        printf("ERROR: unable to read the word tag, %s!\n", plc_tag_decode_error(rc));
        return rc;
    }

    switch(word_size) {
        case 1: *result = (uint64_t)plc_tag_get_uint8(tag, 0); break;
        case 2: *result = (uint64_t)plc_tag_get_uint16(tag, 0); break;
        case 4: *result = (uint64_t)plc_tag_get_uint32(tag, 0); break;
        default: *result = plc_tag_get_uint64(tag, 0); break;
    }

    return PLCTAG_STATUS_OK;
}


int write_word(int32_t tag, int word_size, int timeout, uint64_t value) {
    int rc = PLCTAG_STATUS_OK;

    switch(word_size) {
        case 1: rc = plc_tag_set_uint8(tag, 0, (uint8_t)value); break;
        case 2: rc = plc_tag_set_uint16(tag, 0, (uint16_t)value); break;
        case 4: rc = plc_tag_set_uint32(tag, 0, (uint32_t)value); break;
        default: rc = plc_tag_set_uint64(tag, 0, value); break;
    }

    if(rc != PLCTAG_STATUS_OK) {
        printf("ERROR: unable to set the word value, %s!\n", plc_tag_decode_error(rc));
        return rc;
    }

    rc = plc_tag_write(tag, timeout);
    if(rc != PLCTAG_STATUS_OK) { printf("ERROR: unable to write the word tag, %s!\n", plc_tag_decode_error(rc)); }

    return rc;
}


/*
 * Read before writing, or deliberately do not.
 *
 * Where a bit write is a local read-modify-write rather than a masked write on the wire, the
 * bytes that go out are whatever the tag last read plus the one changed bit.  Refreshing
 * first keeps the default test about the mask rather than about cache staleness.
 *
 * --stale skips the refresh: with the tag's buffer left as it was at create time, does the
 * word still survive?  Only where the device applies the mask.  A PCCC-mapped Logix bit write
 * is a whole-word write of the buffer, so --stale does not apply to it.
 */
int write_bit(int32_t tag, int timeout, int value, bool refresh) {
    int rc = PLCTAG_STATUS_OK;

    if(refresh) {
        rc = plc_tag_read(tag, timeout);

        if(rc != PLCTAG_STATUS_OK) {
            printf("ERROR: unable to read the bit tag before writing it, %s!\n", plc_tag_decode_error(rc));
            return rc;
        }
    }

    rc = plc_tag_set_bit(tag, 0, value);
    if(rc != PLCTAG_STATUS_OK) {
        printf("ERROR: unable to set the bit to %d, %s!\n", value, plc_tag_decode_error(rc));
        return rc;
    }

    rc = plc_tag_write(tag, timeout);
    if(rc != PLCTAG_STATUS_OK) { printf("ERROR: unable to write the bit tag, %s!\n", plc_tag_decode_error(rc)); }

    return rc;
}


/*
 * Report every bit that moved, not just that something did.  Which neighbours changed says
 * what the mask got wrong: the whole word means the mask was ignored, one adjacent bit means
 * an off-by-one, the high half means a width or endian error.
 */
int check_word(uint64_t expected, uint64_t actual, int bit, int value) {
    uint64_t differing = expected ^ actual;

    if(differing == 0) {
        printf("Bit %d set to %d; the rest of the word is untouched.\n", bit, value);
        return PLCTAG_STATUS_OK;
    }

    printf("ERROR: after setting bit %d to %d the word should be 0x%" PRIx64 " but is 0x%" PRIx64 "!\n", bit, value, expected,
           actual);
    printf("ERROR: these bits are wrong:");

    for(int i = 0; i < 64; i++) {
        if(differing & ((uint64_t)1 << (unsigned int)i)) { printf(" %d", i); }
    }

    printf("\n");

    if(!(differing & ((uint64_t)1 << (unsigned int)bit))) {
        printf("ERROR: bit %d itself is correct, so the masked write damaged only its neighbours.\n", bit);
    }

    return PLCTAG_ERR_BAD_DATA;
}

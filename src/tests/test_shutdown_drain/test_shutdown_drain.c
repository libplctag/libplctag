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
 * Shutdown with many slow connections.
 *
 * Opens one connection per connection group to a server that delays every reply, reads once on
 * each, then calls plc_tag_shutdown().  Passes when shutdown finishes within --max-ms and the
 * process has the same number of threads afterwards as before the library started.
 *
 * Today each connection's destructor runs a Forward Close one after another on the single
 * cleanup thread, so shutdown takes about conns * delay.  With 60 connections and a 100 ms
 * delay that passes the CIP module's 5 s teardown timeout, and the late destructors then lock
 * the destroyed connection-list mutex (review defects U7, P6, U5).  With the handlers closing
 * their own connections in parallel it takes about one delay.
 *
 * The delay must stay under the 150 ms Forward Close reply timeout, or every close times out
 * and the test measures that timeout instead.
 *
 * Usage: test_shutdown_drain --tag=<attribs without connection_group_id> [--conns=N] [--max-ms=M]
 */

#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/api/libplctag.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#    include <mach/mach.h>
#elif defined(__linux__)
#    include <dirent.h>
#endif


#define REQUIRED_VERSION 2, 5, 5
#define DATA_TIMEOUT_MS (20000)
#define DEFAULT_CONNS (60)
#define DEFAULT_MAX_MS (3000)
#define MAX_CONNS (200)


static int32_t thread_count(void);
static bool wait_all(const int32_t *tags, int32_t num_tags, const char *what);


int main(int argc, char **argv) {
    const char *tag_attribs = NULL;
    int32_t num_conns = DEFAULT_CONNS;
    int64_t max_ms = DEFAULT_MAX_MS;
    int32_t tags[MAX_CONNS] = {0};
    char attr_str[1024] = {0};
    int32_t threads_before = 0;
    int32_t threads_after = 0;
    int64_t start_ms = 0;
    int64_t shutdown_ms = 0;
    bool ok = true;

    for(int32_t i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--tag=", 6) == 0) {
            tag_attribs = &argv[i][6];
        } else if(strncmp(argv[i], "--conns=", 8) == 0) {
            num_conns = (int32_t)atoi(&argv[i][8]);
        } else if(strncmp(argv[i], "--max-ms=", 9) == 0) {
            max_ms = (int64_t)atoll(&argv[i][9]);
        }
    }

    if(!tag_attribs || num_conns < 1 || num_conns > MAX_CONNS || max_ms < 1) {
        fprintf(stderr, "Usage: %s --tag=<attribs> [--conns=1..%d] [--max-ms=M]\n", argv[0], MAX_CONNS);
        return 1;
    }

    /* count before the library starts any thread of its own. */
    threads_before = thread_count();

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        return 1;
    }

    /* one connection per group, so each has its own handler and its own Forward Close. */
    for(int32_t i = 0; i < num_conns; i++) {
        snprintf(attr_str, sizeof(attr_str), "%s&connection_group_id=%" PRId32, tag_attribs, i);

        /* asynchronous, so the connections open in parallel. */
        tags[i] = plc_tag_create(attr_str, 0);
        if(tags[i] < 0) {
            fprintf(stderr, "ERROR: create %" PRId32 " failed: %s\n", i, plc_tag_decode_error(tags[i]));
            plc_tag_shutdown();
            return 1;
        }
    }

    if(!wait_all(tags, num_conns, "create")) {
        plc_tag_shutdown();
        return 1;
    }

    /* one read on each, all at once, so every connection is fully open before shutdown. */
    for(int32_t i = 0; i < num_conns; i++) { plc_tag_read(tags[i], 0); }

    if(!wait_all(tags, num_conns, "read")) {
        plc_tag_shutdown();
        return 1;
    }

    fprintf(stderr, "%" PRId32 " connections open, shutting down.\n", num_conns);

    start_ms = compat_time_ms();
    plc_tag_shutdown();
    shutdown_ms = compat_time_ms() - start_ms;

    threads_after = thread_count();

    fprintf(stderr, "plc_tag_shutdown() took %" PRId64 " ms (limit %" PRId64 " ms).\n", shutdown_ms, max_ms);

    if(shutdown_ms > max_ms) {
        fprintf(stderr, "FAIL: shutdown took longer than the limit.\n");
        ok = false;
    }

    if(threads_before < 0 || threads_after < 0) {
        fprintf(stderr, "Thread count is not available on this platform; not checked.\n");
    } else {
        fprintf(stderr, "Threads before %" PRId32 ", after %" PRId32 ".\n", threads_before, threads_after);

        if(threads_after != threads_before) {
            fprintf(stderr, "FAIL: library threads are still running after plc_tag_shutdown().\n");
            ok = false;
        }
    }

    fprintf(stderr, "%s\n", ok ? "PASS" : "FAIL");

    return ok ? 0 : 1;
}


/* the number of threads in this process, or -1 where it cannot be counted. */
static int32_t thread_count(void) {
#if defined(__APPLE__)
    thread_act_array_t threads = NULL;
    mach_msg_type_number_t count = 0;

    if(task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) { return -1; }

    for(mach_msg_type_number_t i = 0; i < count; i++) { mach_port_deallocate(mach_task_self(), threads[i]); }

    vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(thread_act_t));

    return (int32_t)count;
#elif defined(__linux__)
    DIR *dir = opendir("/proc/self/task");
    struct dirent *entry = NULL;
    int32_t count = 0;

    if(!dir) { return -1; }

    while((entry = readdir(dir)) != NULL) {
        if(entry->d_name[0] != '.') { count++; }
    }

    closedir(dir);

    return count;
#else
    return -1;
#endif
}


/* wait until no tag is pending; false if any fails or the wait runs past DATA_TIMEOUT_MS. */
static bool wait_all(const int32_t *tags, int32_t num_tags, const char *what) {
    int64_t end_ms = compat_time_ms() + DATA_TIMEOUT_MS;

    while(compat_time_ms() < end_ms) {
        bool pending = false;

        for(int32_t i = 0; i < num_tags; i++) {
            int32_t rc = plc_tag_status(tags[i]);

            if(rc == PLCTAG_STATUS_PENDING) {
                pending = true;
            } else if(rc != PLCTAG_STATUS_OK) {
                fprintf(stderr, "ERROR: %s %" PRId32 " failed: %s\n", what, i, plc_tag_decode_error(rc));
                return false;
            }
        }

        if(!pending) { return true; }

        compat_sleep_ms(10, NULL);
    }

    fprintf(stderr, "ERROR: timed out waiting for every %s to finish.\n", what);
    return false;
}

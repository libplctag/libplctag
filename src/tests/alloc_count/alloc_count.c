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
 * Allocation counter, loaded into a test binary from outside (docs/release_2_7_3_must_do.md
 * item 13).  It needs no library change, so it measures any libplctag version the same way.
 *
 *   macOS:  DYLD_INSERT_LIBRARIES=bin_dist/liballoc_count.dylib ./test_many_tags ...
 *   Linux:  LD_PRELOAD=bin_dist/liballoc_count.so ./test_many_tags ...
 *
 * Counts malloc, calloc and realloc calls with the bytes asked for, and free calls on non-NULL
 * pointers.  posix_memalign, aligned_alloc and the macOS zone calls are not counted; libplctag
 * allocates only through calloc, realloc and free (src/platform/platform_common.c).
 *
 * alloc_count_mark(label) prints the counts since the previous mark and the totals to stderr;
 * a test finds it with dlsym(RTLD_DEFAULT, "alloc_count_mark") and skips marking when it is not
 * loaded.  The totals are printed again at exit with the label "exit".  Line format:
 *
 *   alloc_count <label>: <n> allocs, <n> frees, <n> bytes since last mark; <n> allocs, <n> frees, <n> bytes total
 *
 * It only works where the allocator comes from a shared library: not in a -static binary (Linux
 * Release links the tests -static unless configured with -DUSE_VALGRIND=1), and not with ASan,
 * which replaces the allocator itself.  macOS SIP strips DYLD_ variables for system binaries,
 * so run the test binary directly.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if !defined(__APPLE__)
#    include <dlfcn.h>
#endif

typedef struct {
    uint64_t allocs;
    uint64_t frees;
    uint64_t bytes;
} alloc_counts_t;

static alloc_counts_t counts = {0, 0, 0};
static alloc_counts_t last_mark = {0, 0, 0};

extern void alloc_count_mark(const char *label);

static void count_alloc(size_t size);
static void count_free(void *ptr);
static void print_at_exit(void) __attribute__((destructor));


extern void alloc_count_mark(const char *label) {
    char line[256];
    alloc_counts_t now;
    int len = 0;

    now.allocs = __atomic_load_n(&counts.allocs, __ATOMIC_RELAXED);
    now.frees = __atomic_load_n(&counts.frees, __ATOMIC_RELAXED);
    now.bytes = __atomic_load_n(&counts.bytes, __ATOMIC_RELAXED);

    /* snprintf of integers does not allocate, so printing does not move the counts. */
    len = snprintf(line, sizeof(line),
                   "alloc_count %s: %llu allocs, %llu frees, %llu bytes since last mark; %llu allocs, %llu frees, %llu bytes total\n",
                   label ? label : "(null)", (unsigned long long)(now.allocs - last_mark.allocs),
                   (unsigned long long)(now.frees - last_mark.frees), (unsigned long long)(now.bytes - last_mark.bytes),
                   (unsigned long long)now.allocs, (unsigned long long)now.frees, (unsigned long long)now.bytes);
    if(len > 0) { (void)!write(STDERR_FILENO, line, (size_t)len < sizeof(line) ? (size_t)len : sizeof(line) - 1); }

    last_mark = now;
}


static void count_alloc(size_t size) {
    __atomic_fetch_add(&counts.allocs, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&counts.bytes, (uint64_t)size, __ATOMIC_RELAXED);
}


static void count_free(void *ptr) {
    if(ptr) { __atomic_fetch_add(&counts.frees, 1, __ATOMIC_RELAXED); }
}


static void print_at_exit(void) { alloc_count_mark("exit"); }


#if defined(__APPLE__)

/*
 * dyld rebinds every other image's calls to the originals through the __interpose section.
 * Calls from this library are not rebound, so the replacements call the originals directly.
 */
static void *count_malloc(size_t size);
static void *count_calloc(size_t count, size_t size);
static void *count_realloc(void *ptr, size_t size);
static void count_free_call(void *ptr);

/* dyld reads two addresses; function pointer types keep the casts within ISO C */
typedef void (*any_func_t)(void);

typedef struct {
    any_func_t replacement;
    any_func_t original;
} interpose_t;

__attribute__((used, section("__DATA,__interpose"))) static const interpose_t interposers[] = {
    {(any_func_t)count_malloc, (any_func_t)malloc},
    {(any_func_t)count_calloc, (any_func_t)calloc},
    {(any_func_t)count_realloc, (any_func_t)realloc},
    {(any_func_t)count_free_call, (any_func_t)free},
};


static void *count_malloc(size_t size) {
    count_alloc(size);
    return malloc(size);
}


static void *count_calloc(size_t count, size_t size) {
    count_alloc(count * size);
    return calloc(count, size);
}


static void *count_realloc(void *ptr, size_t size) {
    count_alloc(size);
    return realloc(ptr, size);
}


static void count_free_call(void *ptr) {
    count_free(ptr);
    free(ptr);
}

#else

/*
 * The originals come from dlsym(RTLD_NEXT), and dlsym can call calloc itself, so allocations
 * made while resolving come from a small static buffer and are never freed.
 */
#    define BOOTSTRAP_SIZE (8192)

static void *(*real_malloc)(size_t) = NULL;
static void *(*real_calloc)(size_t, size_t) = NULL;
static void *(*real_realloc)(void *, size_t) = NULL;
static void (*real_free)(void *) = NULL;

static uint8_t bootstrap[BOOTSTRAP_SIZE];
static size_t bootstrap_used = 0;
static bool resolving = false;

static void resolve(void) __attribute__((constructor));
static void *bootstrap_alloc(size_t size);
static bool in_bootstrap(void *ptr);


extern void *malloc(size_t size) {
    if(!real_malloc) { resolve(); }
    if(!real_malloc) { return bootstrap_alloc(size); }
    count_alloc(size);
    return real_malloc(size);
}


extern void *calloc(size_t count, size_t size) {
    if(!real_calloc) { resolve(); }
    if(!real_calloc) { return bootstrap_alloc(count * size); } /* static memory is already zero */
    count_alloc(count * size);
    return real_calloc(count, size);
}


extern void *realloc(void *ptr, size_t size) {
    void *moved = NULL;

    if(!real_realloc) { resolve(); }
    if(!real_realloc) { return NULL; }
    count_alloc(size);
    if(!in_bootstrap(ptr)) { return real_realloc(ptr, size); }

    /* a bootstrap block's size is unknown, so copy up to the end of the buffer */
    moved = real_malloc(size);
    if(moved) {
        size_t avail = (size_t)(bootstrap + BOOTSTRAP_SIZE - (uint8_t *)ptr);
        memcpy(moved, ptr, size < avail ? size : avail);
    }
    return moved;
}


extern void free(void *ptr) {
    if(!ptr || in_bootstrap(ptr)) { return; }
    if(!real_free) { resolve(); }
    count_free(ptr);
    if(real_free) { real_free(ptr); }
}


static void resolve(void) {
    if(resolving || real_free) { return; }
    resolving = true;
    real_malloc = (void *(*)(size_t))dlsym(RTLD_NEXT, "malloc");
    real_calloc = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
    real_realloc = (void *(*)(void *, size_t))dlsym(RTLD_NEXT, "realloc");
    real_free = (void (*)(void *))dlsym(RTLD_NEXT, "free");
    resolving = false;
}


static void *bootstrap_alloc(size_t size) {
    size_t start = (bootstrap_used + 15) & ~(size_t)15;

    if(size > BOOTSTRAP_SIZE - start || start > BOOTSTRAP_SIZE) { return NULL; }
    bootstrap_used = start + size;
    return bootstrap + start;
}


static bool in_bootstrap(void *ptr) { return (uint8_t *)ptr >= bootstrap && (uint8_t *)ptr < bootstrap + BOOTSTRAP_SIZE; }

#endif

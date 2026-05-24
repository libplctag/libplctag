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
 * fiber_net event loop and socket API implementation.
 *
 * Suspend/resume protocol detail:
 *   - Fibers call fiber_net_wait() which stack-allocates a fiber_wait_t,
 *     then calls yafl_fiber_suspend(&wait).
 *   - The event loop receives the fiber_wait_t* as the return value of
 *     yafl_fiber_resume(), stores fd/events in the slot, and polls.
 *   - On event: yafl_fiber_resume(fiber, NULL) — fiber continues.
 *   - On shutdown: yafl_fiber_resume(fiber, &cancel_sentinel) — non-NULL
 *     signals cancellation; fiber_net_wait returns UTIL_ECANCELLED.
 */

#ifdef _WIN32
#    include <winsock2.h>
#    include <ws2tcpip.h>
#else
#    include <arpa/inet.h>
#    include <errno.h>
#    include <fcntl.h>
#    include <netinet/in.h>
#    include <netinet/tcp.h>
#    include <poll.h>
#    include <sys/socket.h>
#    include <unistd.h>
#endif

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#    include <psapi.h>
#else
#    include <sys/resource.h>
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#    define FIBER_BSD_OS
#endif

#if !defined(_WIN32) && !defined(FIBER_BSD_OS) && !defined(MSG_NOSIGNAL)
#    define MSG_NOSIGNAL 0
#endif

#include "err.h"
#include "fiber_net.h"
#include "log.h"
#include "utils.h"


/* ============================================================================
 * Internal types
 * ============================================================================ */

/*
 * IO wait descriptor placed on the fiber's stack and passed via
 * yafl_fiber_suspend().  The event loop reads fd/events from it immediately
 * after the fiber suspends, while the fiber stack is still frozen.
 */
typedef struct {
    fiber_socket_t fd;
    int16_t events;
    int64_t deadline_ms; /* 0 = no deadline; util_time_ms() target for sleeps/timeouts */
} fiber_wait_t;

/* Per-slot fiber state. */
typedef struct {
    yafl_fiber_t *fiber;
    void *context;            /* arg passed on first resume */
    bool pending_start;       /* needs initial-resume on next tick */
    bool active;              /* fiber not yet complete */
    fiber_socket_t wait_fd;   /* fd being waited on; FIBER_INVALID_SOCKET = timer-only sleep */
    int16_t wait_events;      /* POLLIN / POLLOUT */
    int32_t wait_list_idx;    /* index in net->wait_list, -1 if not waiting */
    int64_t wait_deadline_ms; /* 0 = no deadline; wakeup time for sleeps and I/O timeouts */
} fiber_slot_t;

/* Platform-agnostic poll fd type. */
#ifdef _WIN32
typedef WSAPOLLFD fiber_pollfd_t;
#else
typedef struct pollfd fiber_pollfd_t;
#endif

struct fiber_net_s {
    size_t max_tasks;
    size_t stack_size;
    yafl_stack_flags_t stack_flags;
    volatile bool stop_requested;

    fiber_socket_t wakeup_read;
    fiber_socket_t wakeup_write;

    /*
     * Single allocation block:
     *   slots          [max_tasks]
     *   pollfds        [max_tasks + 1]  (extra slot for wakeup fd)
     *   slot_for_pollfd[max_tasks + 1]  (maps pollfd index → slot index, -1 = wakeup)
     */
    fiber_slot_t *slots;
    fiber_pollfd_t *pollfds;
    int32_t *slot_for_pollfd;

    /* Compact active-fiber lists — eliminate O(max_tasks) scans each tick. */
    size_t *pending_list; /* slot indices with pending_start */
    size_t pending_count;
    size_t *wait_list; /* slot indices currently waiting on an fd */
    size_t wait_count;

    /* ---- Loop instrumentation ---- */
    int64_t stat_loop_iters;
    int64_t stat_poll_timeout_iters;
    int64_t stat_poll_wakeup_iters;
    int64_t stat_total_resumes;
    int64_t stat_step1_us;        /* pending-start scan + initial resumes */
    int64_t stat_step2_us;        /* pollfd build */
    int64_t stat_step3_us;        /* poll() call (includes idle blocking) */
    int64_t stat_step4_us;        /* wakeup pipe drain */
    int64_t stat_step5_scan_us;   /* ready-fd detection, excluding resume calls */
    int64_t stat_step5_resume_us; /* yafl_fiber_resume() calls (fiber execution quanta) */
    int64_t stat_min_resume_us;
    int64_t stat_max_resume_us;

    /* ---- Stack watermark stats ---- */
    int64_t stat_watermark_count; /* fibers with valid watermark data */
    size_t stat_watermark_total;  /* sum of high-watermarks in bytes */
    size_t stat_watermark_min;    /* smallest high-watermark seen */
    size_t stat_watermark_max;    /* largest high-watermark seen */

    /* CPU time captured at start/end of fiber_net_run() */
    int64_t wall_start_us;
    double cpu_user_start_ms;
    double cpu_system_start_ms;
    double cpu_user_end_ms;
    double cpu_system_end_ms;
    int64_t wall_end_us;
};

/* Sentinel passed to fibers on cancellation (address is what matters, not value). */
static const uint8_t CANCEL_SENTINEL = 1;


/* ============================================================================
 * CPU time helper
 * ============================================================================ */

static void get_cpu_time(double *user_ms, double *system_ms) {
#ifdef _WIN32
    FILETIME creation, exitt, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &creation, &exitt, &kernel, &user);
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    *system_ms = (double)k.QuadPart / 10000.0; /* 100ns → ms */
    *user_ms = (double)u.QuadPart / 10000.0;
#else
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    *user_ms = (double)usage.ru_utime.tv_sec * 1000.0 + (double)usage.ru_utime.tv_usec / 1000.0;
    *system_ms = (double)usage.ru_stime.tv_sec * 1000.0 + (double)usage.ru_stime.tv_usec / 1000.0;
#endif
}


/* ============================================================================
 * Forward declarations
 * ============================================================================ */

static util_err_t create_wakeup(fiber_socket_t *reader, fiber_socket_t *writer);
static void wakeup_signal(fiber_socket_t wfd);
static void wakeup_drain(fiber_socket_t rfd);
static util_err_t set_nonblocking(fiber_socket_t fd);
static void close_raw(fiber_socket_t fd);
static util_err_t get_socket_err(void);
static void wait_list_add(fiber_net_t *net, size_t si);
static void wait_list_remove(fiber_net_t *net, size_t si);
static void process_resume(fiber_net_t *net, size_t si, void *rv);
static util_err_t fiber_net_wait(fiber_net_t *net, fiber_socket_t fd, int16_t events, int64_t deadline_ms);


/* ============================================================================
 * Public: lifecycle
 * ============================================================================ */

util_err_t fiber_net_create(fiber_net_t **out, size_t max_tasks, size_t stack_size, yafl_stack_flags_t flags) {
    if(!out || max_tasks == 0) { return UTIL_EINVAL; }
    if(!(flags & (YAFL_STACK_FLAGS_MALLOC | YAFL_STACK_FLAGS_VMEM))) { return UTIL_EINVAL; }

    fiber_net_t *net = (fiber_net_t *)calloc(1, sizeof(fiber_net_t));
    if(!net) { return UTIL_ERESOURCE; }

    net->max_tasks = max_tasks;
    net->stack_size = stack_size;
    net->stack_flags = flags;
    net->stop_requested = false;
    net->wakeup_read = FIBER_INVALID_SOCKET;
    net->wakeup_write = FIBER_INVALID_SOCKET;

    /* Single allocation for all internal arrays.
     * Round each section up to 8-byte alignment so that size_t arrays
     * (pending_list, wait_list) are never placed at a misaligned offset. */
#define ALIGN8(n) (((n) + 7u) & ~7u)
    size_t slot_bytes = ALIGN8(max_tasks * sizeof(fiber_slot_t));
    size_t pollfd_bytes = ALIGN8((max_tasks + 1u) * sizeof(fiber_pollfd_t));
    size_t map_bytes = ALIGN8((max_tasks + 1u) * sizeof(int32_t));
    size_t pending_bytes = ALIGN8(max_tasks * sizeof(size_t));
    size_t wait_bytes = ALIGN8(max_tasks * sizeof(size_t));
#undef ALIGN8
    uint8_t *mem = (uint8_t *)calloc(1, slot_bytes + pollfd_bytes + map_bytes + pending_bytes + wait_bytes);
    if(!mem) {
        free(net);
        return UTIL_ERESOURCE;
    }

    net->slots = (fiber_slot_t *)mem;
    net->pollfds = (fiber_pollfd_t *)(mem + slot_bytes);
    net->slot_for_pollfd = (int32_t *)(mem + slot_bytes + pollfd_bytes);
    net->pending_list = (size_t *)(mem + slot_bytes + pollfd_bytes + map_bytes);
    net->wait_list = (size_t *)(mem + slot_bytes + pollfd_bytes + map_bytes + pending_bytes);

    /* calloc zeroed memory; explicitly set wait_list_idx to -1 (not waiting). */
    for(size_t i = 0; i < max_tasks; i++) { net->slots[i].wait_list_idx = -1; }

    util_err_t rc = create_wakeup(&net->wakeup_read, &net->wakeup_write);
    if(rc != UTIL_OK) {
        free(mem);
        free(net);
        return rc;
    }

    *out = net;
    pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_INFO, "fiber_net created (max_tasks=%zu)", max_tasks);
    return UTIL_OK;
}


/* Helper: record one yafl_fiber_resume() duration into the instrumentation fields. */
static void record_resume(fiber_net_t *net, int64_t resume_us) {
    net->stat_total_resumes++;
    if(net->stat_min_resume_us == 0 || resume_us < net->stat_min_resume_us) { net->stat_min_resume_us = resume_us; }
    if(resume_us > net->stat_max_resume_us) { net->stat_max_resume_us = resume_us; }
}

/* Helper: record a fiber's stack high-watermark before it is destroyed. */
static void record_watermark(fiber_net_t *net, yafl_fiber_t *fiber) {
    size_t wm = yafl_fiber_stack_high_watermark(fiber);
    if(wm == 0) { return; } /* watermark not enabled or fiber invalid */
    net->stat_watermark_count++;
    net->stat_watermark_total += wm;
    if(net->stat_watermark_min == 0 || wm < net->stat_watermark_min) { net->stat_watermark_min = wm; }
    if(wm > net->stat_watermark_max) { net->stat_watermark_max = wm; }
}

/* Append slot si to wait_list. */
static void wait_list_add(fiber_net_t *net, size_t si) {
    size_t idx = net->wait_count++;
    net->wait_list[idx] = si;
    net->slots[si].wait_list_idx = (int32_t)idx;
}

/* Remove slot si from wait_list in O(1) via swap-with-last. */
static void wait_list_remove(fiber_net_t *net, size_t si) {
    int32_t idx = net->slots[si].wait_list_idx;
    if(idx < 0) { return; }
    size_t n = --net->wait_count;
    if((size_t)idx < n) {
        size_t moved = net->wait_list[n];
        net->wait_list[(size_t)idx] = moved;
        net->slots[moved].wait_list_idx = idx;
    }
    net->slots[si].wait_list_idx = -1;
}


util_err_t fiber_net_run(fiber_net_t *net, uint32_t tick_ms) {
    if(!net) { return UTIL_EINVAL; }

    /* Capture CPU time at start. */
    net->wall_start_us = util_time_us();
    get_cpu_time(&net->cpu_user_start_ms, &net->cpu_system_start_ms);

    pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_INFO, "fiber_net event loop starting");

    while(!net->stop_requested) {
        net->stat_loop_iters++;

        /* ---- Step 1: Initial-resume any newly added fibers. ---- */
        int64_t t0 = util_time_us();

        size_t n_pending = net->pending_count;
        for(size_t i = 0; i < n_pending; i++) {
            size_t si = net->pending_list[i];
            fiber_slot_t *slot = &net->slots[si];
            slot->pending_start = false;
            pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_DETAIL, "Initial-resuming fiber in slot %zu", si);

            int64_t tr0 = util_time_us();
            void *rv = yafl_fiber_resume(slot->fiber, slot->context);
            int64_t tr1 = util_time_us();
            record_resume(net, tr1 - tr0);
            net->stat_step5_resume_us += tr1 - tr0; /* counts toward resume budget */

            process_resume(net, si, rv);
        }
        /* Compact: any tasks added *during* Step 1 resumes go beyond n_pending; move them to front. */
        size_t newly_added = net->pending_count - n_pending;
        memmove(&net->pending_list[0], &net->pending_list[n_pending], newly_added * sizeof(size_t));
        net->pending_count = newly_added;

        int64_t t1 = util_time_us();
        net->stat_step1_us += t1 - t0;

        /* ---- Step 2: Build pollfd array. ---- */
        int32_t nfds = 0;

        /* Wakeup fd is always at index 0. */
        net->pollfds[nfds].fd = net->wakeup_read;
        net->pollfds[nfds].events = POLLIN;
        net->pollfds[nfds].revents = 0;
        net->slot_for_pollfd[nfds] = -1;
        nfds++;

        for(size_t i = 0; i < net->wait_count; i++) {
            size_t si = net->wait_list[i];
            fiber_slot_t *slot = &net->slots[si];
            net->pollfds[nfds].fd = slot->wait_fd;
            net->pollfds[nfds].events = (short)slot->wait_events;
            net->pollfds[nfds].revents = 0;
            net->slot_for_pollfd[nfds] = (int32_t)si;
            nfds++;
        }

        int64_t t2 = util_time_us();
        net->stat_step2_us += t2 - t1;

        /* ---- Step 2.5: Compute effective poll() timeout (deadline-aware). ---- */
        int effective_tick = (int)tick_ms;
        {
            int64_t now_ms = util_time_ms();
            for(size_t i = 0; i < net->wait_count; i++) {
                int64_t dl = net->slots[net->wait_list[i]].wait_deadline_ms;
                if(dl > 0) {
                    int64_t remaining = dl - now_ms;
                    if(remaining <= 0) {
                        effective_tick = 0;
                        break;
                    }
                    if(remaining < (int64_t)effective_tick) { effective_tick = (int)remaining; }
                }
            }
        }

        /* ---- Step 3: poll(). ---- */
#ifdef _WIN32
        int poll_rc = WSAPoll(net->pollfds, (ULONG)nfds, (INT)effective_tick);
#else
        int poll_rc = poll(net->pollfds, (nfds_t)nfds, effective_tick);
#endif

        int64_t t3 = util_time_us();
        net->stat_step3_us += t3 - t2;

        if(poll_rc < 0) {
#ifdef _WIN32
            if(WSAGetLastError() == WSAEINTR) { continue; }
#else
            if(errno == EINTR) { continue; }
#endif
            pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_ERROR, "poll() failed: %s", util_err_str(get_socket_err()));
            break;
        }

        if(poll_rc == 0) {
            net->stat_poll_timeout_iters++;
        } else {
            net->stat_poll_wakeup_iters++;
        }

        /* ---- Step 4: Drain wakeup fd if it fired. ---- */
        if(poll_rc > 0 && (net->pollfds[0].revents & POLLIN)) { wakeup_drain(net->wakeup_read); }

        int64_t t4 = util_time_us();
        net->stat_step4_us += t4 - t3;

        /* ---- Step 4.5: Wake fibers with expired deadlines. ---- */
        {
            int64_t now_ms = util_time_ms();
            size_t i = 0;
            int64_t expired_resume_us = 0;
            while(i < net->wait_count) {
                size_t si = net->wait_list[i];
                fiber_slot_t *slot = &net->slots[si];
                if(slot->wait_deadline_ms > 0 && now_ms >= slot->wait_deadline_ms) {
                    wait_list_remove(net, si);
                    /* wait_list_remove swapped last into position i — do not advance i */
                    pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_DETAIL, "Deadline expired for fiber in slot %zu", si);
                    int64_t tr0 = util_time_us();
                    void *rv = yafl_fiber_resume(slot->fiber, NULL); /* NULL = deadline expired */
                    int64_t tr1 = util_time_us();
                    int64_t r_us = tr1 - tr0;
                    expired_resume_us += r_us;
                    record_resume(net, r_us);
                    process_resume(net, si, rv);
                } else {
                    i++;
                }
            }
            net->stat_step5_resume_us += expired_resume_us;
        }

        if(poll_rc == 0) { continue; } /* timeout tick — no fd events to process */

        /* ---- Step 5: Resume fibers whose fds fired. ---- */
        int64_t step5_resume_us = 0;

        for(int32_t p = 1; p < nfds; p++) {
            short rev = net->pollfds[p].revents;
            if(!(rev & (POLLIN | POLLOUT | POLLERR | POLLHUP))) { continue; }

            int32_t si = net->slot_for_pollfd[p];
            if(si < 0 || (size_t)si >= net->max_tasks) { continue; }

            fiber_slot_t *slot = &net->slots[si];
            wait_list_remove(net, (size_t)si);

            pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_DETAIL, "Resuming fiber in slot %d (revents=0x%x)", si, (unsigned)rev);

            int64_t tr0 = util_time_us();
            void *rv = yafl_fiber_resume(slot->fiber, NULL); /* NULL = event fired */
            int64_t tr1 = util_time_us();
            int64_t r_us = tr1 - tr0;
            step5_resume_us += r_us;
            record_resume(net, r_us);

            process_resume(net, (size_t)si, rv);
        }

        int64_t t5 = util_time_us();
        net->stat_step5_resume_us += step5_resume_us;
        net->stat_step5_scan_us += (t5 - t4) - step5_resume_us;
    }

    pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_INFO, "fiber_net stopping — cancelling active fibers");

    /* Cancel all active fibers. */
    for(size_t i = 0; i < net->max_tasks; i++) {
        fiber_slot_t *slot = &net->slots[i];
        if(!slot->active || !slot->fiber) { continue; }

        if(yafl_fiber_status(slot->fiber) == YAFL_FIBER_STATUS_SUSPENDED) {
            yafl_fiber_resume(slot->fiber, (void *)&CANCEL_SENTINEL);
        }
        record_watermark(net, slot->fiber);
        yafl_fiber_destroy(slot->fiber);
        slot->fiber = NULL;
        slot->active = false;
    }

    pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_INFO, "fiber_net event loop stopped");

    /* Capture CPU time at end. */
    net->wall_end_us = util_time_us();
    get_cpu_time(&net->cpu_user_end_ms, &net->cpu_system_end_ms);

    return UTIL_OK;
}


void fiber_net_get_loop_stats(const fiber_net_t *net, fiber_net_loop_stats_t *out) {
    if(!net || !out) { return; }

    int64_t iters = net->stat_loop_iters > 0 ? net->stat_loop_iters : 1;
    int64_t resumes = net->stat_total_resumes > 0 ? net->stat_total_resumes : 1;

    out->loop_iterations = net->stat_loop_iters;
    out->poll_timeout_iters = net->stat_poll_timeout_iters;
    out->poll_wakeup_iters = net->stat_poll_wakeup_iters;
    out->total_resumes = net->stat_total_resumes;

    out->avg_step1_us = (double)net->stat_step1_us / (double)iters;
    out->avg_step2_us = (double)net->stat_step2_us / (double)iters;
    out->avg_step3_us = (double)net->stat_step3_us / (double)iters;
    out->avg_step4_us = (double)net->stat_step4_us / (double)iters;
    out->avg_step5_scan_us = (double)net->stat_step5_scan_us / (double)iters;
    out->avg_step5_resume_us = (double)net->stat_step5_resume_us / (double)iters;

    out->avg_resume_us = (double)(net->stat_step1_us /* initial resumes counted here */
                                  + net->stat_step5_resume_us)
                         / (double)resumes;
    /* Note: stat_step1_us includes both the scan AND initial resume time together;
     * for avg_resume_us we use only stat_step5_resume_us (step 5 resumes are tracked
     * cleanly), so avg_resume_us reflects step-5 resumes only. */
    out->avg_resume_us = (double)net->stat_step5_resume_us / (double)resumes;
    out->min_resume_us = net->stat_min_resume_us;
    out->max_resume_us = net->stat_max_resume_us;

    /* CPU usage from getrusage / GetProcessTimes */
    out->cpu_user_ms = net->cpu_user_end_ms - net->cpu_user_start_ms;
    out->cpu_system_ms = net->cpu_system_end_ms - net->cpu_system_start_ms;
    out->wall_time_ms = (double)(net->wall_end_us - net->wall_start_us) / 1000.0;
    if(out->wall_time_ms > 0.0) {
        out->cpu_load_pct = (out->cpu_user_ms + out->cpu_system_ms) / out->wall_time_ms * 100.0;
    } else {
        out->cpu_load_pct = 0.0;
    }

    /* Stack watermark */
    out->watermark_count = net->stat_watermark_count;
    out->stack_size_bytes = net->stack_size;
    if(net->stat_watermark_count > 0) {
        out->avg_stack_used_bytes = (double)net->stat_watermark_total / (double)net->stat_watermark_count;
        out->min_stack_used_bytes = net->stat_watermark_min;
        out->max_stack_used_bytes = net->stat_watermark_max;
    } else {
        out->avg_stack_used_bytes = 0.0;
        out->min_stack_used_bytes = 0;
        out->max_stack_used_bytes = 0;
    }
}


util_err_t fiber_net_stop(fiber_net_t *net) {
    if(!net) { return UTIL_EINVAL; }
    net->stop_requested = true;
    wakeup_signal(net->wakeup_write);
    return UTIL_OK;
}


void fiber_net_destroy(fiber_net_t **net_ptr) {
    if(!net_ptr || !*net_ptr) { return; }

    fiber_net_t *net = *net_ptr;

    for(size_t i = 0; i < net->max_tasks; i++) {
        if(net->slots[i].fiber) { yafl_fiber_destroy(net->slots[i].fiber); }
    }

    if(net->wakeup_read != FIBER_INVALID_SOCKET) { close_raw(net->wakeup_read); }
    if(net->wakeup_write != FIBER_INVALID_SOCKET) { close_raw(net->wakeup_write); }

    /* net->slots is the base of the single allocation block. */
    free(net->slots);
    free(net);
    *net_ptr = NULL;
}


/* ============================================================================
 * Public: task management
 * ============================================================================ */

util_err_t fiber_net_add_task(fiber_net_t *net, fiber_task_fn fn, void *context) {
    if(!net || !fn) { return UTIL_EINVAL; }

    fiber_slot_t *slot = NULL;
    for(size_t i = 0; i < net->max_tasks; i++) {
        if(!net->slots[i].active) {
            slot = &net->slots[i];
            break;
        }
    }
    if(!slot) {
        pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_ERROR, "No free fiber slot (max_tasks=%zu)", net->max_tasks);
        return UTIL_ERESOURCE;
    }

    yafl_fiber_t *fiber = yafl_fiber_create((yafl_fiber_fn)fn, net->stack_size, net->stack_flags);
    if(!fiber) {
        pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_ERROR, "yafl_fiber_create failed");
        return UTIL_ERESOURCE;
    }

    size_t si = (size_t)(slot - net->slots);
    slot->fiber = fiber;
    slot->context = context;
    slot->pending_start = true;
    slot->active = true;
    slot->wait_list_idx = -1;
    net->pending_list[net->pending_count++] = si;

    wakeup_signal(net->wakeup_write);
    return UTIL_OK;
}


/* ============================================================================
 * Public: socket API
 * ============================================================================ */

util_err_t fiber_socket_listen(fiber_net_t *net, fiber_socket_t *out_sock, const char *bind_ip, uint16_t port, size_t backlog) {
    if(!net || !out_sock || !bind_ip) { return UTIL_EINVAL; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if(inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
        pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_ERROR, "Invalid bind address: %s", bind_ip);
        return UTIL_EINVAL;
    }

#ifdef _WIN32
    SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(fd == INVALID_SOCKET) { return util_err_from_wsa(WSAGetLastError()); }
#else
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(fd < 0) { return util_err_from_errno(errno); }
#endif

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

#ifdef FIBER_BSD_OS
    int nosigpipe = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif

    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));

    util_err_t rc = set_nonblocking((fiber_socket_t)fd);
    if(rc != UTIL_OK) {
        close_raw((fiber_socket_t)fd);
        return rc;
    }

    if(bind(fd, (struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0) {
        rc = get_socket_err();
        pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_ERROR, "bind() failed on %s:%u: %s", bind_ip, port, util_err_str(rc));
        close_raw((fiber_socket_t)fd);
        return rc;
    }

    if(listen(fd, (int)backlog) != 0) {
        rc = get_socket_err();
        pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_ERROR, "listen() failed on %s:%u: %s", bind_ip, port, util_err_str(rc));
        close_raw((fiber_socket_t)fd);
        return rc;
    }

    *out_sock = (fiber_socket_t)fd;
    pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_INFO, "Listening on %s:%u", bind_ip, port);
    return UTIL_OK;
}


util_err_t fiber_socket_accept(fiber_net_t *net, fiber_socket_t listener, fiber_socket_t *out_client, size_t timeout_ms) {
    if(!net || listener == FIBER_INVALID_SOCKET || !out_client) { return UTIL_EINVAL; }

    int64_t deadline = (timeout_ms > 0) ? (util_time_ms() + (int64_t)timeout_ms) : INT64_MAX;

    for(;;) {
        struct sockaddr_storage client_addr;
        socklen_t client_addr_len = (socklen_t)sizeof(client_addr);

#ifdef _WIN32
        SOCKET client = accept(listener, (struct sockaddr *)&client_addr, &client_addr_len);
        if(client != INVALID_SOCKET) {
            set_nonblocking((fiber_socket_t)client);
            int nodelay = 1;
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
            *out_client = (fiber_socket_t)client;
            return UTIL_OK;
        }
        int werr = WSAGetLastError();
        if(werr != WSAEWOULDBLOCK) { return util_err_from_wsa(werr); }
#else
        int client = accept(listener, (struct sockaddr *)&client_addr, &client_addr_len);
        if(client >= 0) {
#    ifdef FIBER_BSD_OS
            int nosigpipe = 1;
            setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#    endif
            int nodelay = 1;
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
            set_nonblocking((fiber_socket_t)client);
            *out_client = (fiber_socket_t)client;
            return UTIL_OK;
        }
        if(errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { return util_err_from_errno(errno); }
#endif

        if(timeout_ms > 0 && util_time_ms() >= deadline) { return UTIL_ETIMEOUT; }

        util_err_t rc = fiber_net_wait(net, listener, POLLIN, deadline);
        if(rc != UTIL_OK) { return rc; }

        if(timeout_ms > 0 && util_time_ms() >= deadline) { return UTIL_ETIMEOUT; }
    }
}


util_err_t fiber_socket_recv(fiber_net_t *net, fiber_socket_t sock, Bytes buf, size_t timeout_ms) {
    if(!net || sock == FIBER_INVALID_SOCKET || bytes_is_null(buf)) {
        pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_WARN, "Invalid argument to fiber_socket_recv");
        return UTIL_EINVAL;
    }

    int64_t deadline = (timeout_ms > 0) ? (util_time_ms() + (int64_t)timeout_ms) : INT64_MAX;
    size_t total_read = 0;

    while(total_read < buf.len) {
#ifdef _WIN32
        int rc = recv(sock, (char *)(buf.data + total_read), (int)(buf.len - total_read), 0);
        if(rc > 0) {
            total_read += (size_t)rc;
            continue;
        }
        if(rc == 0) {
            pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_INFO, "Socket closed by peer during recv");
            return UTIL_ECLOSED;
        }
        int werr = WSAGetLastError();
        if(werr != WSAEWOULDBLOCK) { return util_err_from_wsa(werr); }
#else
        ssize_t rc = recv(sock, buf.data + total_read, buf.len - total_read, 0);
        if(rc > 0) {
            total_read += (size_t)rc;
            continue;
        }
        if(rc == 0) {
            pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_INFO, "Socket closed by peer during recv");
            return UTIL_ECLOSED;
        }
        if(errno != EAGAIN && errno != EWOULDBLOCK) {
            pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_WARN, "recv() failed: %s", util_err_str(util_err_from_errno(errno)));
            return util_err_from_errno(errno);
        }
#endif

        if(timeout_ms > 0 && util_time_ms() >= deadline) {
            pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_INFO, "fiber_socket_recv timed out after %zu ms", timeout_ms);
            return UTIL_ETIMEOUT;
        }

        util_err_t wrc = fiber_net_wait(net, sock, POLLIN, deadline);
        if(wrc != UTIL_OK) {
            pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_DETAIL, "fiber_socket_recv wait failed: %s", util_err_str(wrc));
            return wrc;
        }

        if(timeout_ms > 0 && util_time_ms() >= deadline) {
            pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_INFO, "fiber_socket_recv timed out after %zu ms", timeout_ms);
            return UTIL_ETIMEOUT;
        }
    }

    // bytes_hexdump(buf, "fiber_socket_recv data:");

    return UTIL_OK;
}


util_err_t fiber_socket_send(fiber_net_t *net, fiber_socket_t sock, Bytes buf, size_t timeout_ms) {
    if(!net || sock == FIBER_INVALID_SOCKET || bytes_is_null(buf)) {
        pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_WARN, "Invalid argument to fiber_socket_send");
        return UTIL_EINVAL;
    }

    int64_t deadline = (timeout_ms > 0) ? (util_time_ms() + (int64_t)timeout_ms) : INT64_MAX;
    size_t total_sent = 0;

    while(total_sent < buf.len) {
#ifdef _WIN32
        int rc = send(sock, (const char *)(buf.data + total_sent), (int)(buf.len - total_sent), 0);
        if(rc > 0) {
            total_sent += (size_t)rc;
            continue;
        }
        int werr = WSAGetLastError();
        if(werr != WSAEWOULDBLOCK) {
            pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_WARN, "send() failed: %s", util_err_str(util_err_from_wsa(werr)));
            return util_err_from_wsa(werr);
        }
#else
#    ifdef FIBER_BSD_OS
        ssize_t rc = send(sock, buf.data + total_sent, buf.len - total_sent, 0);
#    else
        ssize_t rc = send(sock, buf.data + total_sent, buf.len - total_sent, MSG_NOSIGNAL);
#    endif
        if(rc > 0) {
            total_sent += (size_t)rc;
            continue;
        }
        if(errno != EAGAIN && errno != EWOULDBLOCK) {
            pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_WARN, "send() failed: %s", util_err_str(util_err_from_errno(errno)));
            return util_err_from_errno(errno);
        }
#endif

        if(timeout_ms > 0 && util_time_ms() >= deadline) {
            pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_INFO, "fiber_socket_send timed out after %zu ms", timeout_ms);
            return UTIL_ETIMEOUT;
        }

        util_err_t wrc = fiber_net_wait(net, sock, POLLOUT, deadline);
        if(wrc != UTIL_OK) {
            pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_DETAIL, "fiber_socket_send wait failed: %s", util_err_str(wrc));
            return wrc;
        }

        if(timeout_ms > 0 && util_time_ms() >= deadline) {
            pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_INFO, "fiber_socket_send timed out after %zu ms", timeout_ms);
            return UTIL_ETIMEOUT;
        }
    }

    // bytes_hexdump(buf, "fiber_socket_send data:");

    return UTIL_OK;
}


util_err_t fiber_socket_close(fiber_net_t *net, fiber_socket_t sock) {
    (void)net;
    if(sock == FIBER_INVALID_SOCKET) { return UTIL_OK; }
    close_raw(sock);
    return UTIL_OK;
}


util_err_t fiber_net_sleep_ms(fiber_net_t *net, uint32_t ms) {
    if(!net) { return UTIL_EINVAL; }
    if(ms == 0) { return UTIL_OK; }
    /* Use FIBER_INVALID_SOCKET as sentinel — poll() ignores negative fds (POSIX).
     * The deadline-expired wakeup in Step 4.5 will resume this fiber after ms. */
    int64_t deadline = util_time_ms() + (int64_t)ms;
    util_err_t rc = fiber_net_wait(net, FIBER_INVALID_SOCKET, 0, deadline);
    return (rc == UTIL_ECANCELLED) ? UTIL_ECANCELLED : UTIL_OK;
}


/* ============================================================================
 * Static helpers
 * ============================================================================ */

/*
 * Process the return value of yafl_fiber_resume().
 * Updates slot state: marks complete, or stores the new IO wait.
 */
static void process_resume(fiber_net_t *net, size_t si, void *rv) {
    fiber_slot_t *slot = &net->slots[si];

    if(yafl_fiber_status(slot->fiber) == YAFL_FIBER_STATUS_COMPLETE) {
        pdlog(LOG_MODULE_FIBER_NET, LOG_LEVEL_DETAIL, "Fiber completed normally");
        record_watermark(net, slot->fiber);
        yafl_fiber_destroy(slot->fiber);
        slot->fiber = NULL;
        slot->active = false;
        return;
    }

    if(rv != NULL) {
        /* Fiber suspended with an IO wait or sleep request — add to wait_list.
         * fd == FIBER_INVALID_SOCKET means timer-only sleep; poll() ignores
         * negative fds (sets revents=0) so no special-case needed in Step 2. */
        fiber_wait_t *wait = (fiber_wait_t *)rv;
        slot->wait_fd = wait->fd;
        slot->wait_events = wait->events;
        slot->wait_deadline_ms = wait->deadline_ms;
        wait_list_add(net, si);
    }
    /* else: suspended without IO wait — not in wait_list (unusual path). */
}


/*
 * Internal: suspend the current fiber until fd becomes ready.
 * Returns UTIL_OK on event, UTIL_ECANCELLED on shutdown signal.
 * deadline_ms is informational only; timeout checking is caller's responsibility.
 */
static util_err_t fiber_net_wait(fiber_net_t *net, fiber_socket_t fd, int16_t events, int64_t deadline_ms) {
    (void)net;

    fiber_wait_t wait = {fd, events, deadline_ms};
    void *cancel = yafl_fiber_suspend(&wait);

    return (cancel != NULL) ? UTIL_ECANCELLED : UTIL_OK;
}


/* Platform socket helpers */

static util_err_t set_nonblocking(fiber_socket_t fd) {
#ifdef _WIN32
    u_long mode = 1;
    if(ioctlsocket(fd, FIONBIO, &mode) != 0) { return util_err_from_wsa(WSAGetLastError()); }
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags < 0) { return util_err_from_errno(errno); }
    if(fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) { return util_err_from_errno(errno); }
#endif
    return UTIL_OK;
}


static void close_raw(fiber_socket_t fd) {
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}


static util_err_t get_socket_err(void) {
#ifdef _WIN32
    return util_err_from_wsa(WSAGetLastError());
#else
    return util_err_from_errno(errno);
#endif
}


/* Create the wakeup fd pair used to interrupt poll(). */
static util_err_t create_wakeup(fiber_socket_t *reader, fiber_socket_t *writer) {
#ifdef _WIN32
    /*
     * WSAPoll() cannot poll regular pipe handles, so create two connected
     * TCP sockets on the loopback interface.
     */
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(listener == INVALID_SOCKET) { return util_err_from_wsa(WSAGetLastError()); }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* OS chooses port */

    if(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(listener);
        return util_err_from_wsa(WSAGetLastError());
    }
    if(listen(listener, 1) != 0) {
        closesocket(listener);
        return util_err_from_wsa(WSAGetLastError());
    }

    int addrlen = (int)sizeof(addr);
    if(getsockname(listener, (struct sockaddr *)&addr, &addrlen) != 0) {
        closesocket(listener);
        return util_err_from_wsa(WSAGetLastError());
    }

    SOCKET wr = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(wr == INVALID_SOCKET) {
        closesocket(listener);
        return util_err_from_wsa(WSAGetLastError());
    }
    if(connect(wr, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(listener);
        closesocket(wr);
        return util_err_from_wsa(WSAGetLastError());
    }

    SOCKET rd = accept(listener, NULL, NULL);
    closesocket(listener);
    if(rd == INVALID_SOCKET) {
        closesocket(wr);
        return util_err_from_wsa(WSAGetLastError());
    }

    set_nonblocking((fiber_socket_t)rd);
    set_nonblocking((fiber_socket_t)wr);

    *reader = (fiber_socket_t)rd;
    *writer = (fiber_socket_t)wr;
    return UTIL_OK;

#else
    /* POSIX: use a simple pipe. */
    int fds[2];
    if(pipe(fds) != 0) { return util_err_from_errno(errno); }
    set_nonblocking(fds[0]);
    set_nonblocking(fds[1]);
    *reader = fds[0];
    *writer = fds[1];
    return UTIL_OK;
#endif
}


static void wakeup_signal(fiber_socket_t wfd) {
#ifdef _WIN32
    char c = 1;
    send(wfd, &c, 1, 0);
#else
    uint8_t c = 1;
    (void)write(wfd, &c, 1);
#endif
}


static void wakeup_drain(fiber_socket_t rfd) {
#ifdef _WIN32
    char buf[64];
    recv(rfd, buf, sizeof(buf), 0);
#else
    uint8_t buf[64];
    (void)read(rfd, buf, sizeof(buf));
#endif
}

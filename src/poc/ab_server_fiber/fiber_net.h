#pragma once

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
 * fiber_net — reusable fiber-per-connection network I/O library.
 *
 * Uses yafl stackful fibers and poll()/WSAPoll() to provide a blocking-style
 * socket API.  Each connection runs in its own fiber; the event loop
 * multiplexes them via a single poll() call.
 *
 * Suspend/resume protocol (internal detail):
 *   IO functions suspend via yafl_fiber_suspend(fiber_wait_t*).
 *   The event loop receives the wait descriptor, polls the fd, then resumes
 *   the fiber with NULL (event fired) or non-NULL (cancellation/shutdown).
 *
 * API design:
 *   - fiber_socket_t is just a file descriptor (no heap allocation).
 *   - fiber_net_t* is passed explicitly to every socket operation.
 *   - Fibers receive fiber_net_t* through their context struct.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#    include <winsock2.h>
typedef SOCKET fiber_socket_t;
#    define FIBER_INVALID_SOCKET INVALID_SOCKET
#else
typedef int fiber_socket_t;
#    define FIBER_INVALID_SOCKET (-1)
#endif

#include "bytes.h"
#include "err.h"
#include "yafl.h"

/* ============================================================================
 * Opaque event-loop handle
 * ============================================================================ */

typedef struct fiber_net_s fiber_net_t;

/* ============================================================================
 * Fiber entry function type — matches yafl_fiber_fn
 * ============================================================================ */

typedef void *(*fiber_task_fn)(void *arg);

/* ============================================================================
 * Event-loop lifecycle
 * ============================================================================ */

/*
 * Create an event loop.
 *   max_tasks  — maximum concurrent fibers (listeners + clients combined).
 *   stack_size — per-fiber stack size in bytes (0 = yafl default).
 *   flags      — yafl stack allocation flags (must include MALLOC or VMEM).
 */
extern util_err_t fiber_net_create(fiber_net_t **out, size_t max_tasks, size_t stack_size, yafl_stack_flags_t flags);

/*
 * Run the event loop until fiber_net_stop() is called.
 * tick_ms — maximum poll() wait in milliseconds per iteration.
 * Blocks until stopped.
 */
extern util_err_t fiber_net_run(fiber_net_t *net, uint32_t tick_ms);

/* Signal the event loop to exit after the current tick. */
extern util_err_t fiber_net_stop(fiber_net_t *net);

/* Destroy and NULL-out *net.  Call after fiber_net_run() returns. */
extern void fiber_net_destroy(fiber_net_t **net);

/* ============================================================================
 * Task management
 * ============================================================================ */

/*
 * Register a new fiber task.  The fiber is created now and initial-resumed
 * on the next event loop tick.  context is passed as the arg to fn().
 * Safe to call from inside a running fiber or from outside the loop.
 */
extern util_err_t fiber_net_add_task(fiber_net_t *net, fiber_task_fn fn, void *context);

/* ============================================================================
 * Socket API — must be called from within a running fiber.
 * Pass the fiber_net_t* received through the fiber's context struct.
 * ============================================================================ */

/*
 * Create a non-blocking TCP listening socket bound to bind_ip:port.
 * Sets SO_REUSEADDR and TCP_NODELAY.  Stores the fd in *out_sock.
 */
extern util_err_t fiber_socket_listen(fiber_net_t *net, fiber_socket_t *out_sock, const char *bind_ip, uint16_t port,
                                      size_t backlog);

/*
 * Wait for an incoming connection on listener and store the client fd in
 * *out_client.  Suspends until a connection arrives or timeout_ms elapses.
 * timeout_ms == 0 means no timeout.
 * Returns UTIL_OK, UTIL_ETIMEOUT, UTIL_ECANCELLED, or a network error.
 */
extern util_err_t fiber_socket_accept(fiber_net_t *net, fiber_socket_t listener, fiber_socket_t *out_client, size_t timeout_ms);

/*
 * Receive exactly buf.len bytes into buf.data.
 * Loops recv()+suspend until full or an error occurs.
 * timeout_ms == 0 means no timeout.
 * Returns UTIL_OK, UTIL_ETIMEOUT, UTIL_ECLOSED, UTIL_ECANCELLED, or error.
 */
extern util_err_t fiber_socket_recv(fiber_net_t *net, fiber_socket_t sock, Bytes buf, size_t timeout_ms);

/*
 * Send all buf.len bytes from buf.data.
 * Loops send()+suspend until fully drained or an error occurs.
 * timeout_ms == 0 means no timeout.
 */
extern util_err_t fiber_socket_send(fiber_net_t *net, fiber_socket_t sock, Bytes buf, size_t timeout_ms);

/* Close the socket fd. */
extern util_err_t fiber_socket_close(fiber_net_t *net, fiber_socket_t sock);

/*
 * Suspend the current fiber for ms milliseconds without holding any fd.
 * Uses FIBER_INVALID_SOCKET as a sentinel; poll() ignores negative fds so no
 * special-case is needed in the event loop.  Returns UTIL_OK when the sleep
 * completes normally, UTIL_ECANCELLED if the event loop is shutting down.
 * Must be called from within a running fiber.
 */
extern util_err_t fiber_net_sleep_ms(fiber_net_t *net, uint32_t ms);

/* ============================================================================
 * Event-loop instrumentation
 * ============================================================================ */

/*
 * Per-phase timing breakdown of fiber_net_run().
 *
 * The loop body has five steps:
 *   Step 1 — scan for newly-added (pending_start) fibers; do their initial resume.
 *   Step 2 — build the pollfd array from active waiting fibers.
 *   Step 3 — call poll() / WSAPoll().  Includes idle blocking time.
 *   Step 4 — drain the wakeup pipe if it fired.
 *   Step 5 scan   — walk the returned pollfds to find ready fds.
 *   Step 5 resume — call yafl_fiber_resume() for each ready fiber.
 *                   Includes the full fiber execution quantum (both
 *                   jump_fcontext calls + everything the fiber runs until
 *                   its next suspend or completion).
 *
 * All "avg_*" fields are per loop iteration.
 * avg_resume_us is per individual yafl_fiber_resume() call.
 */
typedef struct {
    int64_t loop_iterations;    /* total while-loop iterations */
    int64_t poll_timeout_iters; /* iterations where poll() returned 0 (no events) */
    int64_t poll_wakeup_iters;  /* iterations where at least one fd was ready */
    int64_t total_resumes;      /* total yafl_fiber_resume() calls (steps 1+5) */

    /* average time (µs) per loop iteration */
    double avg_step1_us;        /* pending-start scan + initial resumes */
    double avg_step2_us;        /* pollfd array build */
    double avg_step3_us;        /* poll() — dominated by idle blocking */
    double avg_step4_us;        /* wakeup pipe drain */
    double avg_step5_scan_us;   /* ready-fd detection (excluding resumes) */
    double avg_step5_resume_us; /* fiber execution quanta (including ctx switch) */

    /* per yafl_fiber_resume() call */
    double avg_resume_us;
    int64_t min_resume_us;
    int64_t max_resume_us;

    /* CPU time consumed during fiber_net_run() */
    double cpu_user_ms;   /* user-mode CPU time (ms) */
    double cpu_system_ms; /* kernel-mode CPU time (ms) */
    double wall_time_ms;  /* wall-clock time (ms) */
    double cpu_load_pct;  /* (user+system) / wall_time * 100 */

    /* Stack high-watermark (zero if YAFL_STACK_FLAGS_WATERMARK was not set) */
    int64_t watermark_count;      /* fibers with a valid watermark reading */
    double  avg_stack_used_bytes; /* mean high-watermark across all measured fibers */
    size_t  min_stack_used_bytes; /* minimum high-watermark */
    size_t  max_stack_used_bytes; /* maximum high-watermark */
    size_t  stack_size_bytes;     /* configured stack size (denominator for % used) */
} fiber_net_loop_stats_t;

/*
 * Fill *out with accumulated loop statistics.
 * Safe to call after fiber_net_run() returns (or from a signal handler
 * after fiber_net_stop() has been called).
 */
extern void fiber_net_get_loop_stats(const fiber_net_t *net, fiber_net_loop_stats_t *out);

#ifdef __cplusplus
}
#endif

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


#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Exponential backoff with jitter, shared by every protocol module's connection
 * retry loop.
 *
 * The caller owns the state.  Initialize it once, call backoff_connected() when a connection
 * comes up and backoff_retry_wait_ms() each time a connect fails or a connection drops.
 */

/* more drops than this in one window is a reconnect storm: the backoff grows instead of the minimum wait. */
#define BACKOFF_RECONNECT_WINDOW_MS (1000)
#define BACKOFF_MAX_RECONNECTS_PER_WINDOW (5)

typedef struct {
    int64_t initial_ms; /* the first delay, and what a reset returns to */
    int64_t max_ms;     /* the delay never grows past this */
    int64_t base_ms;    /* the current, un-jittered delay */
    uint32_t attempts;  /* consecutive failures; for logging only, never feeds the delay */
    bool up;                 /* a connection came up and has not dropped yet */
    int64_t up_since_ms;     /* when it came up */
    int64_t window_start_ms; /* the window drops are counted in */
    uint32_t window_drops;   /* drops in that window */
} backoff_t;

/**
 * @brief Set up a backoff that starts at initial_ms and saturates at max_ms.
 *
 * Both bounds are clamped to at least one millisecond, and max_ms to at least
 * initial_ms, so no combination of arguments can produce a zero or shrinking delay.
 *
 * @param backoff The state to initialize.
 * @param initial_ms The delay for the first failure.
 * @param max_ms The ceiling the delay grows to.
 */
extern void backoff_init(backoff_t *backoff, int64_t initial_ms, int64_t max_ms);

/**
 * @brief Return the backoff to its initial delay.  Call this when an attempt succeeds.
 *
 * @param backoff The state to reset.
 */
extern void backoff_reset(backoff_t *backoff);

/**
 * @brief Jump straight to the longest delay.
 *
 * For a failure that retrying faster cannot fix -- a route to nothing, a device that
 * will not do what was asked.  The attempt is still made, just at the slowest rate, so
 * a condition that does clear is still picked up without the caller tracking it.
 *
 * @param backoff The state to saturate.
 */
extern void backoff_saturate(backoff_t *backoff);

/**
 * @brief Change the ceiling of an already-running backoff.
 *
 * For a maximum the application can set while the retries are happening.  Lowering it
 * takes the current delay down with it, so the next wait honours the new ceiling rather
 * than waiting out the old one first.
 *
 * @param backoff The state to change.
 * @param max_ms The new ceiling.
 */
extern void backoff_set_max_ms(backoff_t *backoff, int64_t max_ms);

/**
 * @brief How long to wait before the next attempt, and advance the backoff.
 *
 * The result is drawn fresh on every call: half the current delay plus a random share
 * of the other half.  Clients that failed together therefore spread out instead of
 * retrying in lockstep.  The returned value is always at least one millisecond.
 *
 * @param backoff The state to advance.
 * @return int64_t Milliseconds to wait.
 */
extern int64_t backoff_wait_ms(backoff_t *backoff);

/**
 * @brief Note that a connection is up.  Only the first call after a drop counts, so a caller
 * may call it each time it finds the connection working.
 * @param backoff The state to update.
 * @param now_ms The current time.
 */
extern void backoff_connected(backoff_t *backoff, int64_t now_ms);

/**
 * @brief How long to wait before connecting again, after a failed connect or a dropped connection.
 * No wait is shorter than initial_ms.  A connect that never came up grows the backoff, as
 * backoff_wait_ms() does.  A connection that came up waits initial_ms, unless more than BACKOFF_MAX_RECONNECTS_PER_WINDOW drops fell in the
 * current window: then the backoff grows, never below initial_ms, and keeps growing on every drop
 * after that.  Only a connection that stayed up a whole window restarts the backoff, so a device
 * that accepts and then drops at once backs off to the maximum and stays there.
 * @param backoff The state to advance.
 * @param now_ms The current time.
 * @return int64_t Milliseconds to wait.
 */
extern int64_t backoff_retry_wait_ms(backoff_t *backoff, int64_t now_ms);

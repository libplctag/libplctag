/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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

/*
 * This file contains various compatibility includes and definitions
 * to allow compilation across POSIX and Windows systems.
 */
#if defined(APPLE) || defined (__APPLE__) || defined(DARWIN) || defined(__DARWIN__)
    #define IS_MACOS (1)
    #define IS_POSIX (1)
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
    #define IS_BSD (1)
    #define IS_POSIX (1)
#elif defined(__linux__)
    #define IS_LINUX (1)
    #define IS_POSIX (1)
#elif defined(__unix__)
    #define IS_POSIX (1) 
#elif defined(WIN32) || defined(WIN64) || defined(_WIN32) || defined(_WIN64) || defined(__MINGW32__) || defined(__MINGW64__)
    #define IS_WINDOWS (1)

    #if defined(_MSC_VER)
        #define IS_MSVC (1)
    #endif

#endif


#ifdef IS_WINDOWS
    #define str_cmp_i(first, second) _stricmp(first, second)
    #define strdup _strdup
    #define str_scanf sscanf_s
#else
    #define str_cmp_i(first, second) strcasecmp(first, second)
    #define str_scanf sscanf
#endif

/* Atomic operations for fairness tracking */
#ifdef IS_WINDOWS
    /* Include winsock2.h before windows.h to avoid conflicts with old winsock.h */
    #ifndef _WINSOCKAPI_
        #define _WINSOCKAPI_
    #endif
    #include <winsock2.h>
    #include <windows.h>
    typedef struct { volatile LONG value; } atomic_int32_t;
    typedef struct { volatile LONG64 value; } atomic_int64_t;

    #define atomic_load_int32(ptr) InterlockedCompareExchange((volatile LONG*)&(ptr)->value, 0, 0)
    #define atomic_store_int32(ptr, val) InterlockedExchange((volatile LONG*)&(ptr)->value, (LONG)(val))
    #define atomic_inc_int32(ptr) InterlockedIncrement((volatile LONG*)&(ptr)->value)
    #define atomic_load_int64(ptr) InterlockedCompareExchange64((volatile LONG64*)&(ptr)->value, 0, 0)
    #define atomic_store_int64(ptr, val) InterlockedExchange64((volatile LONG64*)&(ptr)->value, (LONG64)(val))
#else
    typedef struct { volatile int value; } atomic_int32_t;
    typedef struct { volatile long long value; } atomic_int64_t;
    
    #define atomic_load_int32(ptr) __atomic_load_n(&(ptr)->value, __ATOMIC_SEQ_CST)
    #define atomic_store_int32(ptr, val) __atomic_store_n(&(ptr)->value, (val), __ATOMIC_SEQ_CST)
    #define atomic_inc_int32(ptr) __atomic_add_fetch(&(ptr)->value, 1, __ATOMIC_SEQ_CST)
    #define atomic_load_int64(ptr) __atomic_load_n(&(ptr)->value, __ATOMIC_SEQ_CST)
    #define atomic_store_int64(ptr, val) __atomic_store_n(&(ptr)->value, (val), __ATOMIC_SEQ_CST)
#endif

/* Define ssize_t */
#ifdef IS_MSVC
    #include <BaseTsd.h>
    typedef SSIZE_T ssize_t;
#else
    #include <sys/types.h>
#endif

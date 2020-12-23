/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
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
 * This file contains all the main ifdefs to figure out the underlying platform
 * and to set up any required macros.
 */

#if defined(_WIN32) || defined(WIN32) || defined (_WIN64) || defined (WIN64)
    #define PLATFORM_IS_WINDOWS 1
#else
    #define PLATFORM_IS_POSIX 1

    /* check for Apple and *BSD platforms */
    #if defined(__APPLE__) || defined(__FreeBSD__) ||  defined(__NetBSD__) || defined(__OpenBSD__) || defined(__bsdi__) || defined(__DragonFly__)
        #define PLATFORM_IS_BSD 1

        #if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
            #define PLATFORM_IS_APPLE
            #define _DARWIN_C_SOURCE _POSIX_C_SOURCE
        #endif
    #endif

    #if __ANDROID__
        #define PLATFORM_IS_ANDROID 1
    #endif

#endif

/* common definitions */
#ifdef PLATFORM_IS_WINDOWS
    #define MSG_NOSIGNAL 0

    #ifdef _MSC_VER
        /* MS Visual Studio C compiler. */
        #define START_PACK __pragma( pack(push, 1) )
        #define END_PACK   __pragma( pack(pop) )
        #define __PRETTY_FUNCTION__ __FUNCTION__
        #define __func__ __FUNCTION__
    #else
        /* MinGW on Windows. */
        #define START_PACK
        #define END_PACK  __attribute__((packed))
        #define __PRETTY_FUNCTION__  __func__
    #endif

    /* VS C++ uses foo[] to denote a zero length array. */
    #define ZLA_SIZE

    /* export definitions. */

    #define USE_STD_VARARG_MACROS 1

    /* Apparently ssize_t is not on Windows. */
    #if defined(_MSC_VER)
        #include <BaseTsd.h>
        typedef SSIZE_T ssize_t;
    #endif

    #define INT_TO_SIZE_T(i) (i)
#elif defined(PLATFORM_IS_POSIX)
    #define START_PACK
    #define END_PACK __attribute__((__packed__))

    #define ZLA_SIZE 0

    #define USE_GNU_VARARG_MACROS 1

    /* get definition of ssize_t */
    #include <sys/types.h>

    #define INT_TO_SIZE_T(i) ((size_t)(ssize_t)(i))
#else
    #error "Unsupported platform!"
#endif

#ifndef COUNT_NARG
#define COUNT_NARG(...)                                                \
         COUNT_NARG_(__VA_ARGS__,COUNT_RSEQ_N())
#endif

#ifndef COUNT_NARG_
#define COUNT_NARG_(...)                                               \
         COUNT_ARG_N(__VA_ARGS__)
#endif

#ifndef COUNT_ARG_N
#define COUNT_ARG_N(                                                   \
          _1, _2, _3, _4, _5, _6, _7, _8, _9,_10, \
         _11,_12,_13,_14,_15,_16,_17,_18,_19,_20, \
         _21,_22,_23,_24,_25,_26,_27,_28,_29,_30, \
         _31,_32,_33,_34,_35,_36,_37,_38,_39,_40, \
         _41,_42,_43,_44,_45,_46,_47,_48,_49,_50, \
         _51,_52,_53,_54,_55,_56,_57,_58,_59,_60, \
         _61,_62,_63, N,...) N
#endif

#ifndef COUNT_RSEQ_N
#define COUNT_RSEQ_N()                                                 \
         63,62,61,60,                   \
         59,58,57,56,55,54,53,52,51,50, \
         49,48,47,46,45,44,43,42,41,40, \
         39,38,37,36,35,34,33,32,31,30, \
         29,28,27,26,25,24,23,22,21,20, \
         19,18,17,16,15,14,13,12,11,10, \
         9,8,7,6,5,4,3,2,1,0
#endif

/* from Wikipedia. */
#ifndef container_of
#define container_of(ptr, type, member) ((type *)((char *)(1 ? (ptr) : &((type *)0)->member) - offsetof(type, member)))
#endif

/* handle bools */
#ifndef bool
    typedef int bool;
#endif

#ifndef TRUE
    #define TRUE (1)
#endif

#ifndef FALSE
    #define FALSE (0)
#endif

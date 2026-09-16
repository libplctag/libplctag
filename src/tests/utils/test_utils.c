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

#include "test_utils.h"

#include <string.h>


/* FIXME - centralize this platform setting! */
#if defined(__unix__) || defined(APPLE) || defined(__APPLE__) || defined(__MACH__) || defined(__linux__)
#    define POSIX_PLATFORM
#elif defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__) || defined(WIN64) || defined(_WIN64)
#    define WINDOWS_PLATFORM
#else
#    error "Unsupported platform!"
#endif


#if defined(POSIX_PLATFORM)

#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <signal.h>
#    include <sys/socket.h>
#    include <unistd.h>

static void (*interrupt_handler)(void) = NULL;

static void interrupt_handler_wrapper(int sig) {
    (void)sig;

    if(interrupt_handler) { interrupt_handler(); }
}


int test_set_interrupt_handler(void (*handler)(void)) {
    interrupt_handler = handler;

    signal(SIGINT, interrupt_handler_wrapper);
    signal(SIGTERM, interrupt_handler_wrapper);
    signal(SIGHUP, interrupt_handler_wrapper);

    return 0;
}


int test_cpu_count(void) {
    long count = sysconf(_SC_NPROCESSORS_ONLN);

    return (count > 0) ? (int)count : 1;
}

#elif defined(WINDOWS_PLATFORM)

#    define WIN32_LEAN_AND_MEAN

#    include <windows.h>

#    include <winsock2.h>
#    include <ws2tcpip.h>

static void (*interrupt_handler)(void) = NULL;

static void interrupt_handler_wrapper(void) {
    if(interrupt_handler) { interrupt_handler(); }
}


/* straight from MS' web site */
static BOOL WINAPI CtrlHandler(DWORD fdwCtrlType) {
    switch(fdwCtrlType) {
            /* ^C. */
        case CTRL_C_EVENT: interrupt_handler_wrapper(); return TRUE;

        case CTRL_CLOSE_EVENT: interrupt_handler_wrapper(); return TRUE;

            /* Pass other signals to the next handler. */
        case CTRL_BREAK_EVENT: interrupt_handler_wrapper(); return FALSE;

        case CTRL_LOGOFF_EVENT: interrupt_handler_wrapper(); return FALSE;

        case CTRL_SHUTDOWN_EVENT: interrupt_handler_wrapper(); return FALSE;

        default: return FALSE;
    }
}


int test_set_interrupt_handler(void (*handler)(void)) {
    interrupt_handler = handler;

    /* FIXME - this can fail! */
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    return 0;
}


int test_cpu_count(void) {
    SYSTEM_INFO sys_info;

    GetSystemInfo(&sys_info);

    return (sys_info.dwNumberOfProcessors > 0) ? (int)sys_info.dwNumberOfProcessors : 1;
}


static volatile long wsa_startup_once = 0;

/* Winsock only needs starting once per process. */
static void wsa_startup(void) {
    if(InterlockedCompareExchange(&wsa_startup_once, 1, 0) == 0) {
        WSADATA wsa_data;
        WSAStartup(MAKEWORD(2, 2), &wsa_data);
    }
}

#endif


bool test_wait_for_listener(const char *host, uint16_t port, uint32_t timeout_ms) {
    int64_t deadline_ms = time_ms() + (int64_t)timeout_ms;

#if defined(WINDOWS_PLATFORM)
    wsa_startup();
#endif

    do {
        struct sockaddr_in addr;
        int rc;

#if defined(WINDOWS_PLATFORM)
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if(sock == INVALID_SOCKET) { return false; }
#else
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if(sock < 0) { return false; }
#endif

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        if(inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
#if defined(WINDOWS_PLATFORM)
            closesocket(sock);
#else
            close(sock);
#endif
            return false;
        }

        rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));

#if defined(WINDOWS_PLATFORM)
        closesocket(sock);
#else
        close(sock);
#endif

        if(rc == 0) { return true; }

        sleep_ms(100);
    } while(time_ms() < deadline_ms);

    return false;
}

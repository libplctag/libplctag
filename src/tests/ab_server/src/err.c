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

#include "compat.h"
#include "err.h"
#include "log.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* ===== PROTOCOL ERROR STRINGS ===== */

static const char *cip_error_to_string(uint8_t cip_err) {
    switch(cip_err) {
        case CIP_OK: return "CIP_OK";
        case CIP_ERR_EXT_ERR: return "CIP_ERR_EXT_ERR (Extended error)";
        case CIP_ERR_INVALID_PARAM: return "CIP_ERR_INVALID_PARAM";
        case CIP_ERR_PATH_SEGMENT: return "CIP_ERR_PATH_SEGMENT";
        case CIP_ERR_PATH_DEST_UNKNOWN: return "CIP_ERR_PATH_DEST_UNKNOWN";
        case CIP_ERR_FRAG: return "CIP_ERR_FRAG";
        case CIP_ERR_UNSUPPORTED: return "CIP_ERR_UNSUPPORTED";
        case CIP_ERR_INSUFFICIENT_DATA: return "CIP_ERR_INSUFFICIENT_DATA";
        case CIP_ERR_TOO_MUCH_DATA: return "CIP_ERR_TOO_MUCH_DATA";
        case CIP_ERR_EXTENDED: return "CIP_ERR_EXTENDED";
        default: return "CIP_ERR (Unknown)";
    }
}

static const char *cip_extended_error_to_string(uint16_t ext_err) {
    switch(ext_err) {
        case CIP_ERR_EX_DUPLICATE_CONN: return "CIP_ERR_EX_DUPLICATE_CONN";
        case CIP_ERR_EX_INVALID_CONN_SIZE: return "CIP_ERR_EX_INVALID_CONN_SIZE";
        case CIP_ERR_EX_TOO_LONG: return "CIP_ERR_EX_TOO_LONG";
        default: return "CIP_ERR_EX (Unknown)";
    }
}

/* ===== INTERNAL ERROR STRINGS ===== */

static const char *internal_error_to_string(int err) {
    switch(err) {
        /* Success */
        case ERR_OK: return "ERR_OK";

        /* Socket Layer */
        case ERR_SOCKET_STARTUP: return "ERR_SOCKET_STARTUP";
        case ERR_SOCKET_CREATE: return "ERR_SOCKET_CREATE";
        case ERR_SOCKET_BIND: return "ERR_SOCKET_BIND";
        case ERR_SOCKET_LISTEN: return "ERR_SOCKET_LISTEN";
        case ERR_SOCKET_ACCEPT: return "ERR_SOCKET_ACCEPT";
        case ERR_SOCKET_CONNECT: return "ERR_SOCKET_CONNECT";
        case ERR_SOCKET_SETOPT: return "ERR_SOCKET_SETOPT";
        case ERR_SOCKET_READ: return "ERR_SOCKET_READ";
        case ERR_SOCKET_WRITE: return "ERR_SOCKET_WRITE";
        case ERR_SOCKET_SELECT: return "ERR_SOCKET_SELECT";
        case ERR_SOCKET_TIMEOUT: return "ERR_SOCKET_TIMEOUT";
        case ERR_SOCKET_EOF: return "ERR_SOCKET_EOF";
        case ERR_SOCKET_BAD_PARAM: return "ERR_SOCKET_BAD_PARAM";

        /* TCP Server */
        case ERR_TCP_INCOMPLETE: return "ERR_TCP_INCOMPLETE";
        case ERR_TCP_BAD_REQUEST: return "ERR_TCP_BAD_REQUEST";
        case ERR_TCP_UNSUPPORTED: return "ERR_TCP_UNSUPPORTED";
        case ERR_TCP_PROCESSED: return "ERR_TCP_PROCESSED";
        case ERR_TCP_DONE: return "ERR_TCP_DONE";

        default: return "ERR (Unknown internal error)";
    }
}

/* ===== POSIX ERRNO STRINGS ===== */

#ifndef IS_WINDOWS
static const char *posix_errno_to_string(int posix_err) { return strerror(posix_err); }
#endif

/* ===== WINSOCK ERROR STRINGS ===== */

#ifdef IS_WINDOWS
static const char *winsock_error_to_string(int winsock_err) {
    static char buf[256];

    switch(winsock_err) {
        case WSASYSNOTREADY: return "WSASYSNOTREADY";
        case WSAVERNOTSUPPORTED: return "WSAVERNOTSUPPORTED";
        case WSANOTINITIALISED: return "WSANOTINITIALISED";
        case WSAEDISCON: return "WSAEDISCON";
        case WSAEACCES: return "WSAEACCES";
        case WSAEADDRINUSE: return "WSAEADDRINUSE";
        case WSAEADDRNOTAVAIL: return "WSAEADDRNOTAVAIL";
        case WSAEAFNOSUPPORT: return "WSAEAFNOSUPPORT";
        case WSAEALREADY: return "WSAEALREADY";
        case WSAECONNABORTED: return "WSAECONNABORTED";
        case WSAECONNREFUSED: return "WSAECONNREFUSED";
        case WSAECONNRESET: return "WSAECONNRESET";
        case WSAEDESTADDRREQ: return "WSAEDESTADDRREQ";
        case WSAEFAULT: return "WSAEFAULT";
        case WSAEHOSTDOWN: return "WSAEHOSTDOWN";
        case WSAEHOSTUNREACH: return "WSAEHOSTUNREACH";
        case WSAEINPROGRESS: return "WSAEINPROGRESS";
        case WSAEINTR: return "WSAEINTR";
        case WSAEINVAL: return "WSAEINVAL";
        case WSAEISCONN: return "WSAEISCONN";
        case WSAELOOP: return "WSAELOOP";
        case WSAEMFILE: return "WSAEMFILE";
        case WSAEMSGSIZE: return "WSAEMSGSIZE";
        case WSAENAMETOOLONG: return "WSAENAMETOOLONG";
        case WSAENETDOWN: return "WSAENETDOWN";
        case WSAENETRESET: return "WSAENETRESET";
        case WSAENETUNREACH: return "WSAENETUNREACH";
        case WSAENOBUFS: return "WSAENOBUFS";
        case WSAENOPROTOOPT: return "WSAENOPROTOOPT";
        case WSAENOTCONN: return "WSAENOTCONN";
        case WSAENOTSOCK: return "WSAENOTSOCK";
        case WSAEOPNOTSUPP: return "WSAEOPNOTSUPP";
        case WSAEPFNOSUPPORT: return "WSAEPFNOSUPPORT";
        case WSAEPROCLIM: return "WSAEPROCLIM";
        case WSAEPROTONOSUPPORT: return "WSAEPROTONOSUPPORT";
        case WSAEPROTOTYPE: return "WSAEPROTOTYPE";
        case WSAEREMOTE: return "WSAEREMOTE";
        case WSAESHUTDOWN: return "WSAESHUTDOWN";
        case WSAESOCKTNOSUPPORT: return "WSAESOCKTNOSUPPORT";
        case WSAETIMEDOUT: return "WSAETIMEDOUT";
        case WSAETOOMANYREFS: return "WSAETOOMANYREFS";
        case WSAEWOULDBLOCK: return "WSAEWOULDBLOCK";
        default: snprintf(buf, sizeof(buf), "WSAERROR(%d)", winsock_err); return buf;
    }
}
#endif

/* ===== ERROR CLASSIFICATION ===== */

bool err_is_protocol_error(int err) {
    /* Protocol errors are specific values that come from the protocol specifications */
    if(err >= 0x00 && err <= 0xFF) {
        /* Could be a CIP error code */
        return err_is_valid_cip_error((uint8_t)err);
    }
    if(err >= 0x0100 && err <= 0xFFFF) {
        /* Could be a CIP extended error code */
        return err_is_valid_cip_extended_error((uint16_t)err);
    }
    if(err == EIP_ERR_BAD_REQUEST) { return true; }
    return false;
}

bool err_is_internal_error(int err) { return !err_is_protocol_error(err); }

bool err_is_posix_error(int err) { return (err >= ERR_POSIX_BASE && err < ERR_WINSOCK_BASE); }

bool err_is_winsock_error(int err) {
#ifdef IS_WINDOWS
    return (err >= ERR_WINSOCK_BASE && err < 4000);
#else
    (void)err;
    return false;
#endif
}

/* ===== PROTOCOL ERROR VALIDATION ===== */

bool err_is_valid_cip_error(uint8_t cip_err) {
    /* Valid CIP errors from ODVA specification */
    switch(cip_err) {
        case CIP_OK:
        case CIP_ERR_EXT_ERR:
        case CIP_ERR_INVALID_PARAM:
        case CIP_ERR_PATH_SEGMENT:
        case CIP_ERR_PATH_DEST_UNKNOWN:
        case CIP_ERR_FRAG:
        case CIP_ERR_UNSUPPORTED:
        case CIP_ERR_INSUFFICIENT_DATA:
        case CIP_ERR_TOO_MUCH_DATA:
        case CIP_ERR_EXTENDED: return true;
        default: return false;
    }
}

bool err_is_valid_cip_extended_error(uint16_t cip_extended_err) {
    /* Valid CIP extended errors from ODVA specification */
    switch(cip_extended_err) {
        case CIP_ERR_EX_DUPLICATE_CONN:
        case CIP_ERR_EX_INVALID_CONN_SIZE:
        case CIP_ERR_EX_TOO_LONG: return true;
        default: return false;
    }
}

bool err_is_valid_eip_error(uint32_t eip_err) {
    /* Valid EIP errors from ODVA specification */
    switch(eip_err) {
        case EIP_ERR_BAD_REQUEST: return true;
        default: return false;
    }
}

/* ===== ERROR CONVERSION ===== */

const char *err_to_string(int err) {
    /* Check for protocol errors first */
    if(err >= 0x00 && err <= 0xFF) {
        if(err_is_valid_cip_error((uint8_t)err)) { return cip_error_to_string((uint8_t)err); }
    }
    if(err >= 0x0100 && err <= 0xFFFF) {
        if(err_is_valid_cip_extended_error((uint16_t)err)) { return cip_extended_error_to_string((uint16_t)err); }
    }
    if(err == EIP_ERR_BAD_REQUEST) { return "EIP_ERR_BAD_REQUEST"; }

    /* Check for internal errors */
    if(err_is_internal_error(err)) {
        if(err_is_posix_error(err)) {
#ifndef IS_WINDOWS
            int posix_errno = err - ERR_POSIX_BASE;
            return posix_errno_to_string(posix_errno);
#else
            return "POSIX error (on Windows)";
#endif
        }
        if(err_is_winsock_error(err)) {
#ifdef IS_WINDOWS
            int winsock_err = err - ERR_WINSOCK_BASE + 10000;
            return winsock_error_to_string(winsock_err);
#else
            return "Winsock error (on POSIX)";
#endif
        }
        return internal_error_to_string(err);
    }

    return "UNKNOWN_ERROR";
}

int err_from_errno(int posix_errno) {
#ifndef IS_WINDOWS
    if(posix_errno == 0) { return ERR_OK; }
    /* Map POSIX errno to internal error code */
    if(posix_errno > 0 && posix_errno < 500) { return ERR_POSIX_BASE + posix_errno; }
#else
    /* On Windows, shouldn't be called, but handle gracefully */
    (void)posix_errno;
#endif
    return ERR_OK;
}

#ifdef IS_WINDOWS
int err_from_winsock(int winsock_error) {
    if(winsock_error == 0) { return ERR_OK; }
    /* Map Winsock error to internal error code */
    /* Winsock errors are typically 10000-11999 */
    if(winsock_error >= 10000 && winsock_error < 12000) { return ERR_WINSOCK_BASE + (winsock_error - 10000); }
    return ERR_OK;
}
#endif

int err_get_posix_errno(int err) {
    if(err_is_posix_error(err)) { return err - ERR_POSIX_BASE; }
    return 0;
}

int err_get_winsock_error(int err) {
    if(err_is_winsock_error(err)) {
#ifdef IS_WINDOWS
        return err - ERR_WINSOCK_BASE + 10000;
#else
        return 0;
#endif
    }
    return 0;
}

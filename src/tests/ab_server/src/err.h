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

#include <stdbool.h>
#include <stdint.h>

/* ===== PROTOCOL ERRORS (ODVA Specified - DO NOT CHANGE) ===== */

/* CIP Service Error Codes (ODVA CIP Specification) */
#define CIP_OK                      ((uint8_t)0x00)
#define CIP_ERR_EXT_ERR             ((uint8_t)0x01)  /* Extended error status */
#define CIP_ERR_INVALID_PARAM       ((uint8_t)0x03)  /* Invalid parameter */
#define CIP_ERR_PATH_SEGMENT        ((uint8_t)0x04)  /* Bad path segment */
#define CIP_ERR_PATH_DEST_UNKNOWN   ((uint8_t)0x05)  /* Unknown destination */
#define CIP_ERR_FRAG                ((uint8_t)0x06)  /* Fragmentation error */
#define CIP_ERR_UNSUPPORTED         ((uint8_t)0x08)  /* Unsupported service */
#define CIP_ERR_INSUFFICIENT_DATA   ((uint8_t)0x13)  /* Not enough data */
#define CIP_ERR_TOO_MUCH_DATA       ((uint8_t)0x15)  /* Too much data */
#define CIP_ERR_EXTENDED            ((uint8_t)0xff)  /* Extended error indicator */

/* CIP Extended Error Codes (ODVA CIP Specification) */
#define CIP_ERR_EX_DUPLICATE_CONN   ((uint16_t)0x0100)
#define CIP_ERR_EX_INVALID_CONN_SIZE ((uint16_t)0x0109)
#define CIP_ERR_EX_TOO_LONG         ((uint16_t)0x2105)

/* EIP Protocol Error Codes (ODVA EIP Specification) */
#define EIP_ERR_BAD_REQUEST         ((uint32_t)1)

/* ===== INTERNAL ERRORS (Application-level, never sent on wire) ===== */

typedef enum {
    /* Success */
    ERR_OK = 0,

    /* Socket/Network Layer (1000-1999) */
    ERR_SOCKET_STARTUP = 1000,      /* Socket subsystem startup failed (Winsock only) */
    ERR_SOCKET_CREATE = 1001,       /* Socket creation failed */
    ERR_SOCKET_BIND = 1002,         /* Socket bind failed */
    ERR_SOCKET_LISTEN = 1003,       /* Socket listen failed */
    ERR_SOCKET_ACCEPT = 1004,       /* Socket accept failed */
    ERR_SOCKET_CONNECT = 1005,      /* Socket connect failed */
    ERR_SOCKET_SETOPT = 1006,       /* Socket set option failed */
    ERR_SOCKET_READ = 1007,         /* Socket read failed */
    ERR_SOCKET_WRITE = 1008,        /* Socket write failed */
    ERR_SOCKET_SELECT = 1009,       /* Socket select failed */
    ERR_SOCKET_TIMEOUT = 1010,      /* Socket operation timeout */
    ERR_SOCKET_EOF = 1011,          /* End of file (connection closed) */
    ERR_SOCKET_BAD_PARAM = 1012,    /* Bad parameter to socket function */

    /* TCP Server/Protocol Layer (2000-2999) */
    ERR_TCP_INCOMPLETE = 2000,      /* More data needed */
    ERR_TCP_BAD_REQUEST = 2001,     /* Invalid request format */
    ERR_TCP_UNSUPPORTED = 2002,     /* Unsupported command/feature */
    ERR_TCP_PROCESSED = 2003,       /* Successfully processed */
    ERR_TCP_DONE = 2004,            /* Close connection */

    /* OS/System POSIX Errors (3000-3499) */
    /* These map errno values to internal error codes */
    ERR_POSIX_BASE = 3000,
    /* Note: errno values are typically 1-133, so we map them directly:
       ERR_POSIX_BASE + errno_value (e.g., ERR_POSIX_BASE + EPERM = 3001) */

    /* OS/System Windows Winsock Errors (3500-3999) */
    /* These map Winsock error values to internal error codes */
    ERR_WINSOCK_BASE = 3500,
    /* Note: Winsock errors are typically 10000-11999, we map them as:
       ERR_WINSOCK_BASE + (winsock_error - 10000) */
} err_t;

/* ===== ERROR CLASSIFICATION ===== */

/* Determine if error code is a protocol-level error (must be sent on-wire) */
bool err_is_protocol_error(int err);

/* Determine if error code is an internal/system error (never sent on-wire) */
bool err_is_internal_error(int err);

/* Determine if error is a POSIX errno mapping */
bool err_is_posix_error(int err);

/* Determine if error is a Winsock error mapping */
bool err_is_winsock_error(int err);

/* ===== PROTOCOL ERROR VALIDATION ===== */

/* Validate that error code is a well-formed CIP error */
bool err_is_valid_cip_error(uint8_t cip_err);

/* Validate that error code is a well-formed CIP extended error */
bool err_is_valid_cip_extended_error(uint16_t cip_extended_err);

/* Validate that error code is a well-formed EIP error */
bool err_is_valid_eip_error(uint32_t eip_err);

/* ===== ERROR CONVERSION ===== */

/* Convert any error code to human-readable string */
const char *err_to_string(int err);

/* Translate POSIX errno to unified internal error code */
int err_from_errno(int posix_errno);

/* Translate Windows Winsock error to unified internal error code */
#ifdef IS_WINDOWS
int err_from_winsock(int winsock_error);
#endif

/* Extract original errno/winsock value from internal error code */
int err_get_posix_errno(int err);
int err_get_winsock_error(int err);

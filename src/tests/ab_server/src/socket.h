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

#include "slice.h"

typedef enum {
    SOCKET_STATUS_OK = -1,
    SOCKET_ERR_STARTUP = -2,
    SOCKET_ERR_OPEN = -3,
    SOCKET_ERR_CREATE = -4,
    SOCKET_ERR_BIND = -5,
    SOCKET_ERR_LISTEN = -6,
    SOCKET_ERR_SETOPT = -7,
    SOCKET_ERR_CONNECT = -8,
    SOCKET_ERR_READ = -9,
    SOCKET_ERR_WRITE = -10,
    SOCKET_ERR_SELECT = -11,
    SOCKET_ERR_ACCEPT = -12
} socket_err_t;

extern int socket_open_tcp_client(const char *remote_host, const char *remote_port);
extern int socket_open_tcp_server(const char *listening_port);
extern void socket_close(int sock);
extern int socket_accept(int sock);
extern slice_s socket_read(int sock, slice_s in_buf);
extern int socket_write(int sock, slice_s out_buf);

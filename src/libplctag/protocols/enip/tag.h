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

#ifndef __LIBPLCTAG_ENIP_TAG_H__
#    define __LIBPLCTAG_ENIP_TAG_H__ 1

#    include <libplctag/lib/tag.h>

typedef enum {
    ENIP_TAG_OP_IDLE = 0,
    ENIP_TAG_OP_METADATA_PHASE1 = 1,
    ENIP_TAG_OP_METADATA_PHASE2 = 2,
    ENIP_TAG_OP_REQUEST = 3,
    ENIP_TAG_OP_RESPONSE = 4,
    ENIP_TAG_OP_COMPLETE = 5,
    ENIP_TAG_OP_ERROR = 6,
} enip_tag_op_state_t;

typedef struct enip_tag_t {
    TAG_BASE_STRUCT;

    int32_t op_state;
    int32_t metadata_state;

    uint32_t sequence_id;
    uint32_t transaction_id;

    uint8_t metadata_phase1_ready;
    uint8_t metadata_phase2_ready;
    uint8_t metadata_required;

    uint8_t rearm_on_reconnect;
    uint8_t was_in_response_state;
} enip_tag_t;

typedef struct enip_connection_tag_t {
    TAG_BASE_STRUCT;

    int32_t callback_latency_last_ms;
    int32_t callback_latency_max_ms;
    int32_t queue_depth;
} enip_connection_tag_t;

#endif

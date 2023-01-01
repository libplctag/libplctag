/***************************************************************************
 *   Copyright (C) 2022 by Kyle Hayes                                      *
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


#include <stdint.h>
#include <lib/libplctag.h>
#include <common_protocol/slice.h>
#include <ab/ab_pdu.h>
#include <util/debug.h>


#define EIP_ENCAP_T_SIZE (24)

int eip_encap_reserve(slice_t src, slice_t *result)
{
    pdebug(DEBUG_DETAIL, "Starting.");

    /* take 24 bytes from the source. */
    if(src.length < EIP_ENCAP_T_SIZE) {
        return PLCTAG_ERR_NO_DATA; // should this be PLCTAG_ERR_TOO_SMALL?
    }

    result->buffer = src.buffer;
    result->start = src.start + EIP_ENCAP_T_SIZE;
    result->length = src.length - EIP_ENCAP_T_SIZE;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


int eip_encap_deserialize(eip_encap_t *eip_encap, slice_t src, slice_t *result)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* take 24 bytes from the source. */
    if(src.length < EIP_ENCAP_T_SIZE) {
        return PLCTAG_ERR_PARTIAL;
    }

    *result = src;

    rc = slice_get_u16le(&(eip_encap->command), *result, result);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Error %s getting command data from source slice!", plc_tag_decode_error(rc));
        return rc;
    }

    rc = slice_get_u16le(&(eip_encap->length), *result, result);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Error %s getting length data from source slice!", plc_tag_decode_error(rc));
        return rc;
    }

    /* check whether we have all the data available */
    if(src.length < eip_encap->length + EIP_ENCAP_T_SIZE) {
        return PLCTAG_ERR_PARTIAL;
    }

    rc = slice_get_u32le(&(eip_encap->session_handle), *result, result);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Error %s getting session handle data from source slice!", plc_tag_decode_error(rc));
        return rc;
    }

    rc = slice_get_u32le(&(eip_encap->status), *result, result);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Error %s getting status data from source slice!", plc_tag_decode_error(rc));
        return rc;
    }

    rc = slice_get_u64le(&(eip_encap->sender_context), *result, result);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Error %s getting sender context data from source slice!", plc_tag_decode_error(rc));
        return rc;
    }

    rc = slice_get_u32le(&(eip_encap->options), *result, result);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Error %s getting status data from source slice!", plc_tag_decode_error(rc));
        return rc;
    }

    /* stitch up the payload slice */
    eip_encap->payload.buffer = src.buffer;
    eip_encap->payload.start = src.start + EIP_ENCAP_T_SIZE;
    eip_encap->payload.length = eip_encap->length;

    result->buffer = src.buffer;
    result->start = src.start + EIP_ENCAP_T_SIZE + eip_encap->length;
    result->length = src.length - (EIP_ENCAP_T_SIZE + eip_encap->length);

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;

}


int eip_encap_serialize(eip_encap_t *eip_encap, slice_t dest, slice_t *result)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* take 24 bytes from the source. */
    if(dest.length < EIP_ENCAP_T_SIZE) {
        return PLCTAG_ERR_PARTIAL;
    }

    *result = dest;

    rc = slice_put_u16le(eip_encap->command, *result, result);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Error %s serializing command data into destination slice!", plc_tag_decode_error(rc));
        return rc;
    }

    rc = slice_put_u16le(eip_encap->length, *result, result);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Error %s serializing length data into destination slice!", plc_tag_decode_error(rc));
        return rc;
    }

    rc = slice_put_u32le(eip_encap->session_handle, *result, result);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Error %s serializing session handle data into destination slice!", plc_tag_decode_error(rc));
        return rc;
    }

    rc = slice_put_u32le(eip_encap->status, *result, result);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Error %s serializing status data into destination slice!", plc_tag_decode_error(rc));
        return rc;
    }

    rc = slice_put_u64le(eip_encap->sender_context, *result, result);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Error %s serializing sender context data into destination slice!", plc_tag_decode_error(rc));
        return rc;
    }

    rc = slice_put_u32le(eip_encap->options, *result, result);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Error %s serializing options data into destination slice!", plc_tag_decode_error(rc));
        return rc;
    }

    result->buffer = dest.buffer;
    result->start = dest.start + EIP_ENCAP_T_SIZE + eip_encap->length;
    result->length = dest.length - (EIP_ENCAP_T_SIZE + eip_encap->length);

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}




#define EIP_SESSION_REGISTRATION_T_SIZE (4)

int eip_session_registration_reserve(slice_t source, slice_t *result)
{
    pdebug(DEBUG_DETAIL, "Starting.");

    /* take 4 bytes from the source. */
    if(source.length < EIP_SESSION_REGISTRATION_T_SIZE) {
        return PLCTAG_ERR_PARTIAL; // should this be PLCTAG_ERR_TOO_SMALL?
    }

    result->buffer = source.buffer;
    result->start = source.start + EIP_SESSION_REGISTRATION_T_SIZE;
    result->length = source.length - EIP_SESSION_REGISTRATION_T_SIZE;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


int eip_session_registration_deserialize(eip_session_registration_t *eip_reg, slice_t source, slice_t *result)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* take 24 bytes from the source. */
    if(source.length < EIP_SESSION_REGISTRATION_T_SIZE) {
        return PLCTAG_ERR_PARTIAL;
    }

    *result = source;

    rc = slice_get_u16le(&(eip_reg->eip_version), *result, result);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Error %s getting version data from source slice!", plc_tag_decode_error(rc));
        return rc;
    }

    rc = slice_get_u16le(&(eip_reg->option_flags), *result, result);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Error %s getting option flags data from source slice!", plc_tag_decode_error(rc));
        return rc;
    }

    result->buffer = source.buffer;
    result->start = source.start + EIP_SESSION_REGISTRATION_T_SIZE;
    result->length = source.length - EIP_SESSION_REGISTRATION_T_SIZE;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


int eip_session_registration_serialize(eip_session_registration_t *eip_reg, slice_t dest, slice_t *result)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* take 24 bytes from the source. */
    if(dest.length < EIP_SESSION_REGISTRATION_T_SIZE) {
        return PLCTAG_ERR_PARTIAL;
    }

    *result = dest;

    rc = slice_put_u16le(&(eip_reg->eip_version), *result, result);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Error %s putting version data into destination slice!", plc_tag_decode_error(rc));
        return rc;
    }

    rc = slice_put_u16le(&(eip_reg->option_flags), *result, result);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Error %s putting option flags data into destination slice!", plc_tag_decode_error(rc));
        return rc;
    }

    result->buffer = dest.buffer;
    result->start = dest.start + EIP_SESSION_REGISTRATION_T_SIZE;
    result->length = dest.length - EIP_SESSION_REGISTRATION_T_SIZE;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}




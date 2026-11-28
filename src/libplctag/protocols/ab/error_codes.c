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

#include <libplctag/lib/libplctag.h>
#include <libplctag/protocols/ab/defs.h>
#include <libplctag/protocols/ab/error_codes.h>
#include <platform.h>
#include <stdint.h>
#include <utils/debug.h>


struct error_code_entry {
    int primary_code;
    int secondary_code;
    int translated_code;
    const char *short_desc;
};


/* CIP general and extended status codes, with a short description of each. */


static const struct error_code_entry error_code_table[] = {
    {0x01, 0x0100, PLCTAG_ERR_DUPLICATE, "Connection In Use/Duplicate Forward Open"},
    {0x01, 0x0103, PLCTAG_ERR_UNSUPPORTED, "Transport Class/Trigger Combination not supported"},
    {0x01, 0x0106, PLCTAG_ERR_NOT_ALLOWED, "Owner Conflict"},
    {0x01, 0x0107, PLCTAG_ERR_NOT_FOUND, "Target Connection Not Found"},
    {0x01, 0x0108, PLCTAG_ERR_BAD_PARAM, "Invalid Network Connection Parameter"},
    {0x01, 0x0109, PLCTAG_ERR_BAD_PARAM, "Invalid Connection Size"},
    {0x01, 0x0110, PLCTAG_ERR_NOT_FOUND, "Target for Connection Not Configured"},
    {0x01, 0x0111, PLCTAG_ERR_UNSUPPORTED, "RPI Not Supported"},
    {0x01, 0x0112, PLCTAG_ERR_BAD_PARAM, "RPI Value not acceptable"},
    {0x01, 0x0113, PLCTAG_ERR_NO_RESOURCES, "Out of Connections"},
    {0x01, 0x0114, PLCTAG_ERR_NOT_FOUND, "Vendor ID or Product Code Mismatch"},
    {0x01, 0x0115, PLCTAG_ERR_NOT_FOUND, "Device Type Mismatch"},
    {0x01, 0x0116, PLCTAG_ERR_NO_MATCH, "Revision Mismatch"},
    {0x01, 0x0117, PLCTAG_ERR_BAD_PARAM, "Invalid Produced or Consumed Application Path"},
    {0x01, 0x0118, PLCTAG_ERR_BAD_PARAM, "Invalid or Inconsistent Configuration Application Path"},
    {0x01, 0x0119, PLCTAG_ERR_OPEN, "Non-listen Only Connection Not Opened"},
    {0x01, 0x011A, PLCTAG_ERR_NO_RESOURCES, "Target Object Out of Connections"},
    {0x01, 0x011B, PLCTAG_ERR_TOO_SMALL, "RPI is smaller than the Production Inhibit Time"},
    {0x01, 0x011C, PLCTAG_ERR_UNSUPPORTED, "Transport Class Not Supported"},
    {0x01, 0x011D, PLCTAG_ERR_UNSUPPORTED, "Production Trigger Not Supported"},
    {0x01, 0x011E, PLCTAG_ERR_UNSUPPORTED, "Direction Not Supported"},
    {0x01, 0x011F, PLCTAG_ERR_BAD_PARAM, "Invalid Originator to Target Network Connection Fixed/Variable Flag"},
    {0x01, 0x0120, PLCTAG_ERR_BAD_PARAM, "Invalid Target to Originator Network Connection Fixed/Variable Flag"},
    {0x01, 0x0121, PLCTAG_ERR_BAD_PARAM, "Invalid Originator to Target Network Connection Priority"},
    {0x01, 0x0122, PLCTAG_ERR_BAD_PARAM, "Invalid Target to Originator Network Connection Priority"},
    {0x01, 0x0123, PLCTAG_ERR_BAD_PARAM, "Invalid Originator to Target Network Connection Type"},
    {0x01, 0x0124, PLCTAG_ERR_BAD_PARAM, "Invalid Target to Originator Network Connection Type"},
    {0x01, 0x0125, PLCTAG_ERR_BAD_PARAM, "Invalid Originator to Target Network Connection Redundant_Owner"},
    {0x01, 0x0126, PLCTAG_ERR_BAD_PARAM, "Invalid Configuration Size"},
    {0x01, 0x0127, PLCTAG_ERR_BAD_PARAM, "Invalid Originator to Target Size"},
    {0x01, 0x0128, PLCTAG_ERR_BAD_PARAM, "Invalid Target to Originator Size"},
    {0x01, 0x0129, PLCTAG_ERR_BAD_PARAM, "Invalid Configuration Application Path"},
    {0x01, 0x012A, PLCTAG_ERR_BAD_PARAM, "Invalid Consuming Application Path"},
    {0x01, 0x012B, PLCTAG_ERR_BAD_PARAM, "Invalid Producing Application Path"},
    {0x01, 0x012C, PLCTAG_ERR_NOT_FOUND, "Configuration Symbol Does not Exist"},
    {0x01, 0x012D, PLCTAG_ERR_NOT_FOUND, "Consuming Symbol Does not Exist"},
    {0x01, 0x012E, PLCTAG_ERR_NOT_FOUND, "Producing Symbol Does not Exist"},
    {0x01, 0x012F, PLCTAG_ERR_BAD_DATA, "Inconsistent Application Path Combination"},
    {0x01, 0x0130, PLCTAG_ERR_BAD_DATA, "Inconsistent Consume data format"},
    {0x01, 0x0131, PLCTAG_ERR_BAD_DATA, "Inconsistent Product data format"},
    {0x01, 0x0132, PLCTAG_ERR_UNSUPPORTED, "Null Forward Open function not supported"},
    {0x01, 0x0133, PLCTAG_ERR_BAD_PARAM, "Connection Timeout Multiplier not acceptable"},
    {0x01, 0x0203, PLCTAG_ERR_TIMEOUT, "Connection Timed Out"},
    {0x01, 0x0204, PLCTAG_ERR_TIMEOUT, "Unconnected Request Timed Out"},
    {0x01, 0x0205, PLCTAG_ERR_BAD_PARAM, "Parameter Error in Unconnected Request Service"},
    {0x01, 0x0206, PLCTAG_ERR_TOO_LARGE, "Message Too Large for Unconnected_Send Service"},
    {0x01, 0x0207, PLCTAG_ERR_BAD_REPLY, "Unconnected Acknowledge without Reply"},
    {0x01, 0x0301, PLCTAG_ERR_NO_MEM, "No Buffer Memory Available"},
    {0x01, 0x0302, PLCTAG_ERR_NO_RESOURCES, "Network Bandwidth not Available for Data"},
    {0x01, 0x0303, PLCTAG_ERR_NO_RESOURCES, "No Consumed Connection ID Filter Available"},
    {0x01, 0x0304, PLCTAG_ERR_BAD_CONFIG, "Not Configured to Send Scheduled Priority Data"},
    {0x01, 0x0305, PLCTAG_ERR_NO_MATCH, "Schedule Signature Mismatch"},
    {0x01, 0x0306, PLCTAG_ERR_UNSUPPORTED, "Schedule Signature Validation not Possible"},
    {0x01, 0x0311, PLCTAG_ERR_BAD_DEVICE, "Port Not Available"},
    {0x01, 0x0312, PLCTAG_ERR_BAD_PARAM, "Link Address Not Valid"},
    {0x01, 0x0315, PLCTAG_ERR_BAD_PARAM, "Invalid Segment in Connection Path"},
    {0x01, 0x0316, PLCTAG_ERR_NO_MATCH, "Forward Close Service Connection Path Mismatch"},
    {0x01, 0x0317, PLCTAG_ERR_BAD_PARAM, "Scheduling Not Specified"},
    {0x01, 0x0318, PLCTAG_ERR_BAD_PARAM, "Link Address to Self Invalid"},
    {0x01, 0x0319, PLCTAG_ERR_NO_RESOURCES, "Secondary Resource Unavailable"},
    {0x01, 0x031A, PLCTAG_ERR_DUPLICATE, "Rack Connection Already established"},
    {0x01, 0x031B, PLCTAG_ERR_DUPLICATE, "Module Connection Already established"},
    {0x01, 0x031C, PLCTAG_ERR_REMOTE_ERR, "Miscellaneous"},
    {0x01, 0x031D, PLCTAG_ERR_NO_MATCH, "Redundant Connection Mismatch"},
    {0x01, 0x031E, PLCTAG_ERR_NO_RESOURCES, "No more User Configurable Link Resources Available in the Producing Module"},
    {0x01, 0x031F, PLCTAG_ERR_NO_RESOURCES, "No User Configurable Link Consumer Resources Configured in the Producing Module"},
    {0x01, 0x0800, PLCTAG_ERR_BAD_DEVICE, "Network Link Offline"},
    {0x01, 0x0810, PLCTAG_ERR_NO_DATA, "No Target Application Data Available"},
    {0x01, 0x0811, PLCTAG_ERR_NO_DATA, "No Originator Application Data Available"},
    {0x01, 0x0812, PLCTAG_ERR_UNSUPPORTED, "Node Address has changed since the Network was scheduled"},
    {0x01, 0x0813, PLCTAG_ERR_UNSUPPORTED, "Not Configured for Off-subnet Multicast"},
    {0x01, 0x0814, PLCTAG_ERR_BAD_DATA, "Invalid Produce/Consume Data format"},
    {0x02, -1, PLCTAG_ERR_NO_RESOURCES, "Resource Unavailable for Unconnected Send"},
    {0x03, -1, PLCTAG_ERR_BAD_PARAM, "Parameter value invalid."},
    {0x04, -1, PLCTAG_ERR_NOT_FOUND, "IOI could not be deciphered or tag does not exist."},
    {0x05, -1, PLCTAG_ERR_BAD_PARAM, "Path Destination Error"},
    {0x06, -1, PLCTAG_ERR_TOO_LARGE, "Data requested would not fit in response packet."},
    {0x07, -1, PLCTAG_ERR_BAD_CONNECTION, "Connection lost"},
    {0x08, -1, PLCTAG_ERR_UNSUPPORTED, "Unsupported service."},
    {0x09, -1, PLCTAG_ERR_BAD_DATA, "Error in Data Segment"},
    {0x0A, -1, PLCTAG_ERR_BAD_STATUS, "Attribute list error"},
    {0x0B, -1, PLCTAG_ERR_DUPLICATE, "Already in requested mode/state"},
    {0x0C, -1, PLCTAG_ERR_BAD_STATUS, "Object State Error"},
    {0x0D, -1, PLCTAG_ERR_DUPLICATE, "Object already exists."},
    {0x0E, -1, PLCTAG_ERR_NOT_ALLOWED, "Attribute not settable"},
    {0x0F, -1, PLCTAG_ERR_NOT_ALLOWED, "Permission denied."},
    {0x10, -1, PLCTAG_ERR_BAD_STATUS, "Device State Error"},
    {0x11, -1, PLCTAG_ERR_TOO_LARGE, "Reply data too large"},
    {0x12, -1, PLCTAG_ERR_NOT_ALLOWED, "Fragmentation of a primitive value"},
    {0x13, -1, PLCTAG_ERR_TOO_SMALL, "Not Enough Data"},
    {0x14, -1, PLCTAG_ERR_UNSUPPORTED, "Attribute not supported."},
    {0x15, -1, PLCTAG_ERR_TOO_LARGE, "Too Much Data"},
    {0x16, -1, PLCTAG_ERR_NOT_FOUND, "Object does not exist."},
    {0x17, -1, PLCTAG_ERR_NOT_ALLOWED, "Service fragmentation sequence not in progress."},
    {0x18, -1, PLCTAG_ERR_NO_DATA, "No stored attribute data."},
    {0x19, -1, PLCTAG_ERR_REMOTE_ERR, "Store operation failure."},
    {0x1A, -1, PLCTAG_ERR_TOO_LARGE, "Routing failure, request packet too large."},
    {0x1B, -1, PLCTAG_ERR_TOO_LARGE, "Routing failure, response packet too large."},
    {0x1C, -1, PLCTAG_ERR_NO_DATA, "Missing attribute list entry data."},
    {0x1E, -1, PLCTAG_ERR_PARTIAL, "One or more bundled requests failed.."},
    {0x1D, -1, PLCTAG_ERR_BAD_DATA, "Invalid attribute value list."},
    {0x20, -1, PLCTAG_ERR_BAD_PARAM, "Invalid parameter."},
    {0x21, -1, PLCTAG_ERR_DUPLICATE, "Write-once value or medium already written."},
    {0x22, -1, PLCTAG_ERR_BAD_REPLY, "Invalid Reply Received"},
    {0x25, -1, PLCTAG_ERR_BAD_PARAM, "Key failure in path"},
    {0x26, -1, PLCTAG_ERR_BAD_PARAM, "The number of IOI words specified does not match IOI word count."},
    {0x27, -1, PLCTAG_ERR_BAD_PARAM, "Unexpected attribute in list"},
    {0x28, -1, PLCTAG_ERR_BAD_PARAM, "Invalid Member ID."},
    {0x29, -1, PLCTAG_ERR_NOT_ALLOWED, "Member not writable."},
    {0xFF, 0x2104, PLCTAG_ERR_OUT_OF_BOUNDS, "Address is out of range."},
    {0xFF, 0x2105, PLCTAG_ERR_OUT_OF_BOUNDS, "Attempt to access beyond the end of the data object."},
    {0xFF, 0x2107, PLCTAG_ERR_BAD_PARAM, "The data type is invalid or not supported."},
    {-1, -1, PLCTAG_ERR_REMOTE_ERR, "Unknown error code."},
};


static int lookup_error_code(uint8_t *data, size_t data_size) {
    int index = 0;
    int primary_code = 0;
    int secondary_code = 0;

    /* build the error status, but only read bytes the caller told us are actually there. */
    if(data_size >= 1) {
        primary_code = (int)*data;

        if(primary_code != 0 && data_size >= 2) {
            int num_status_words = 0;

            data++;
            num_status_words = (int)*data;

            if(num_status_words > 0 && data_size >= 4) {
                data++;
                secondary_code = (int)data[0] + (int)(data[1] << 8);
            }
        }
    }

    while(error_code_table[index].primary_code != -1) {
        if(error_code_table[index].primary_code == primary_code) {
            if(error_code_table[index].secondary_code == secondary_code || error_code_table[index].secondary_code == -1) {
                break;
            }
        }

        index++;
    }

    return index;
}


const char *decode_cip_error_short(uint8_t *data, size_t data_size) {
    int index = lookup_error_code(data, data_size);

    return error_code_table[index].short_desc;
}


int decode_cip_error_code(uint8_t *data, size_t data_size) {
    int index = lookup_error_code(data, data_size);

    return error_code_table[index].translated_code;
}


/*
 * Classify a Forward Open reply from its raw status: general status, the count of status words,
 * then the words.  *offered_size is the size a 0x0109 refusal offers, or 0.
 */
fo_reply_t classify_fo_reply(const uint8_t *status, size_t status_size, uint16_t *offered_size) {
    uint16_t extended = 0;

    *offered_size = 0;

    if(status_size < 1) { return FO_OTHER; }

    if(status_size >= 4 && status[1] >= 1) { extended = (uint16_t)(status[2] | (status[3] << 8)); }

    switch(status[0]) {
        case 0x00: return FO_ACCEPTED;
        case 0x04: /* path segment error */
        case 0x05: /* path destination unknown */
            return FO_ROUTE_REFUSED;
        case 0x08: return FO_UNSUPPORTED;
        case 0x01: break;
        default: return FO_OTHER;
    }

    switch(extended) {
        case 0x0100: return FO_DUP_ID;
        case 0x0109:
            if(status_size >= 6 && status[1] >= 2) { *offered_size = (uint16_t)(status[4] | (status[5] << 8)); }
            return FO_SIZE_REFUSED;
        case 0x0113: return FO_NO_CONNECTIONS;
        case 0x0800: return FO_LINK_OFFLINE;
        case 0x0311: /* invalid port */
        case 0x0312: /* invalid link address */
        case 0x0315: /* invalid segment in the path */
            return FO_ROUTE_REFUSED;
        default: return FO_OTHER;
    }
}


int cip_check_reply(int32_t tag_id, uint8_t *reply, uint8_t *reply_end, uint8_t service, bool *partial) {
    cip_header *resp = (cip_header *)reply;
    size_t status_size = 0;

    if(cip_error_data_size(reply, reply_end) < sizeof(cip_header)) {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, tag_id, "CIP reply is too short for its header!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    if(resp->reply_service != (uint8_t)(service | AB_EIP_CMD_CIP_OK)) {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, tag_id, "CIP response reply service unexpected: 0x%02x",
               (unsigned int)resp->reply_service);
        return PLCTAG_ERR_BAD_DATA;
    }

    if(partial) { *partial = (resp->status == AB_CIP_STATUS_FRAG); }

    if(resp->status == AB_CIP_STATUS_OK || (partial && *partial)) { return PLCTAG_STATUS_OK; }

    status_size = cip_error_data_size(&resp->status, reply_end);

    pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, tag_id, "CIP request failed with status: 0x%x %s", (unsigned int)resp->status,
           decode_cip_error_short(&resp->status, status_size));

    return decode_cip_error_code(&resp->status, status_size);
}

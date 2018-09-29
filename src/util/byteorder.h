/***************************************************************************
 *   Copyright (C) 2018 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#pragma once

#include <platform.h>


START_PACK typedef struct {
    union {
        uint8_t b_val[2];
        uint16_t u_val;
    } val;
} END_PACK uint16_le;

START_PACK typedef struct {
    union {
        uint8_t b_val[4];
        uint32_t u_val;
    } val;
} END_PACK uint32_le;

START_PACK typedef struct {
    union {
        uint8_t b_val[8];
        uint64_t u_val;
    } val;
} END_PACK uint64_le;




inline static uint16_le h2le16(uint16_t val)
{
    uint16_le result;

    result.val.b_val[0] = (uint8_t)(val & 0xFF);
    result.val.b_val[1] = (uint8_t)((val >> 8) & 0xFF);

    return result;
}


inline static uint16_t le2h16(uint16_le src)
{
    uint16_t result = 0;

    result = (uint16_t)(src.val.b_val[0] + ((src.val.b_val[1]) << 8));

    return result;
}




inline static uint32_le h2le32(uint32_t val)
{
    uint32_le result;

    result.val.b_val[0] = (uint8_t)(val & 0xFF);
    result.val.b_val[1] = (uint8_t)((val >> 8) & 0xFF);
    result.val.b_val[2] = (uint8_t)((val >> 16) & 0xFF);
    result.val.b_val[3] = (uint8_t)((val >> 24) & 0xFF);

    return result;
}


inline static uint32_t le2h32(uint32_le src)
{
    uint32_t result = 0;

    result |= (uint32_t)(src.val.b_val[0]);
    result |= ((uint32_t)(src.val.b_val[1]) << 8);
    result |= ((uint32_t)(src.val.b_val[2]) << 16);
    result |= ((uint32_t)(src.val.b_val[3]) << 24);

    return result;
}






inline static uint64_le h2le64(uint64_t val)
{
    uint64_le result;

    result.val.b_val[0] = (uint8_t)(val & 0xFF);
    result.val.b_val[1] = (uint8_t)((val >> 8) & 0xFF);
    result.val.b_val[2] = (uint8_t)((val >> 16) & 0xFF);
    result.val.b_val[3] = (uint8_t)((val >> 24) & 0xFF);
    result.val.b_val[4] = (uint8_t)((val >> 32) & 0xFF);
    result.val.b_val[5] = (uint8_t)((val >> 40) & 0xFF);
    result.val.b_val[6] = (uint8_t)((val >> 48) & 0xFF);
    result.val.b_val[7] = (uint8_t)((val >> 56) & 0xFF);

    return result;
}


inline static uint64_t le2h64(uint64_le src)
{
    uint64_t result = 0;

    result |= (uint64_t)(src.val.b_val[0]);
    result |= ((uint64_t)(src.val.b_val[1]) << 8);
    result |= ((uint64_t)(src.val.b_val[2]) << 16);
    result |= ((uint64_t)(src.val.b_val[3]) << 24);
    result |= ((uint64_t)(src.val.b_val[4]) << 32);
    result |= ((uint64_t)(src.val.b_val[5]) << 40);
    result |= ((uint64_t)(src.val.b_val[6]) << 48);
    result |= ((uint64_t)(src.val.b_val[7]) << 56);

    return result;
}


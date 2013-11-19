/***************************************************************************
 *   Copyright (C) 2012 by Process Control Engineers                       *
 *   Author Kyle Hayes  kylehayes@processcontrolengineers.com              *
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

 /**************************************************************************
  * CHANGE LOG                                                             *
  *                                                                        *
  * 2012-06-15  KRH - Created file.                                        *
  *                                                                        *
  **************************************************************************/

#ifndef __LIBPLCTAG_AB_UTIL_H__
#define __LIBPLCTAG_AB_UTIL_H__


#ifdef __cplusplus
extern "C"
{
#endif


#include "libplctag.h"
#include "libplctag_tag.h"


/* PCCC commands */
#define AB_PCCC_TYPED_CMD ((uint8_t)0x0F)
#define AB_PCCC_TYPED_READ_FUNC ((uint8_t)0x68)
#define AB_PCCC_TYPED_WRITE_FUNC ((uint8_t)0x67)

/* PCCC defs */
#define AB_PCCC_DATA_BIT            1
#define AB_PCCC_DATA_BIT_STRING     2
#define AB_PCCC_DATA_BYTE_STRING    3
#define AB_PCCC_DATA_INT            4
#define AB_PCCC_DATA_TIMER          5
#define AB_PCCC_DATA_COUNTER        6
#define AB_PCCC_DATA_CONTROL        7
#define AB_PCCC_DATA_REAL           8
#define AB_PCCC_DATA_ARRAY          9
#define AB_PCCC_DATA_ADDRESS        15
#define AB_PCCC_DATA_BCD            16



int ab_make_laa(uint8_t *data, int *size, const char *name, int max_tag_name_size);
uint8_t calculate_bcc(uint8_t *data,int size);
uint16_t calculate_crc16(uint8_t *data, int size);
void decode_pccc_error(plc_tag tag, int error);
uint8_t *ab_decode_pccc_dt_byte(uint8_t *data,int data_size, int *pccc_res_type, int *pccc_res_length);
int ab_encode_pccc_dt_byte(uint8_t *data,int buf_size, uint32_t data_type, uint32_t data_size);




#ifdef __cplusplus
}
#endif

#endif

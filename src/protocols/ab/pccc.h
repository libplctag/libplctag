/***************************************************************************
 *   Copyright (C) 2015 by OmanTek                                         *
 *   Author Kyle Hayes  kylehayes@omantek.com                              *
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
  * 2013-11-19  KRH - Created file.                                        *
  **************************************************************************/

#ifndef __LIBPLCTAG_AB_PCCC_H__
#define __LIBPLCTAG_AB_PCCC_H__



#include <lib/libplctag.h>
#include <lib/tag.h>
#include <platform.h>

typedef enum { PCCC_FILE_UNKNOWN, PCCC_FILE_ASCII, PCCC_FILE_BIT, PCCC_FILE_BLOCK_TRANSFER, PCCC_FILE_COUNTER,
               PCCC_FILE_BCD, PCCC_FILE_FLOAT, PCCC_FILE_INPUT, PCCC_FILE_MESSAGE, PCCC_FILE_INT, PCCC_FILE_OUTPUT,
               PCCC_FILE_PID, PCCC_FILE_CONTROL, PCCC_FILE_STATUS, PCCC_FILE_SFC, PCCC_FILE_STRING, PCCC_FILE_TIMER
             } pccc_file_t;

extern int plc5_encode_tag_name(uint8_t *data, int *size, pccc_file_t *file_type, const char *name, int max_tag_name_size);
extern int slc_encode_tag_name(uint8_t *data, int *size, pccc_file_t *file_type, const char *name, int max_tag_name_size);
extern uint8_t pccc_calculate_bcc(uint8_t *data,int size);
extern uint16_t pccc_calculate_crc16(uint8_t *data, int size);
extern const char *pccc_decode_error(uint8_t *error_ptr);
extern uint8_t *pccc_decode_dt_byte(uint8_t *data,int data_size, int *pccc_res_type, int *pccc_res_length);
extern int pccc_encode_dt_byte(uint8_t *data,int buf_size, uint32_t data_type, uint32_t data_size);



#endif

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
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

 /**************************************************************************
  * CHANGE LOG                                                             *
  *                                                                        *
  * 2012-07-05  KRH - Created file.                                        *
  *                                                                        *
  * 2012-07-25  KRH - Updates for new thread API.                          *
  *                                                                        *
  **************************************************************************/
  
/***************************************************************************
 ****************************** WINDOWS ************************************
 **************************************************************************/



#ifndef __PLC_OS_SUPPORT_H__
#define __PLC_OS_SUPPORT_H__

#ifdef __cplusplus
extern "C"
{
#endif



/*#include <WinSock2.h>*/
/*#include <Ws2tcpip.h>*/
/*#include <sys/types.h>*/
#include <stdint.h>
/*#include <io.h>*/
/*#include <stdlib.h>*/
#include <malloc.h>

#include "libplctag_lib.h"



/* WinSock does not define this or support signals */
#define MSG_NOSIGNAL 0

#define START_PACK __pragma( pack(push, 1) )
#define END_PACK __pragma( pack(pop) )

/* VS C++ uses foo[] to denote a zero length array. */
#define ZLA_SIZE

/* VS C++ uses _strnicmp instead of strncasecmp, there seems to be no agreement
 * between compilers. */
#define strncasecmp _strnicmp
#define strcasecmp _stricmp
#define snprintf _snprintf
#define strdup _strdup
#define close _close
#define localtime_platform(epoch,tm_struct) localtime_s(tm_struct,epoch)

/* export definitions. */

#define USE_STD_VARARG_MACROS 1

#define __PRETTY_FUNCTION__ __FUNCTION__

/*typedef uint32_t in_addr_t;*/




#ifndef PLC_BIG_ENDIAN

	#define h2le16(v) ((uint16_t)(v))
	#define h2le32(v) ((uint32_t)(v))

	#define le2h16(v) h2le16(v)
	#define le2h32(v) h2le32(v)

#else

	#define h2le16(v) (((((uint16_t)(v)) & 0xFF) << 8) + ((((uint16_t)(v)) >> 8) & 0xFF))
	#define h2le32(v) (((((uint32_t)(v)) & 0xFF) << 24) + (((((uint32_t)(v)) >> 8) & 0xFF) << 16) + (((((uint32_t)(v)) >> 16) & 0xFF) << 8) + ((((uint32_t)(v)) >> 24) & 0xFF))

	#define le2h16(v) h2le16(v)
	#define le2h32(v) h2le32(v)

#endif

/* tickler thread support */
extern int plc_lib_start_tickler_thread(plc_lib lib);
extern int plc_lib_stop_tickler_thread(plc_lib lib);
extern int plc_lib_lock_tickler(plc_lib lib);
extern int plc_lib_unlock_tickler(plc_lib lib); 

/* time handling */
extern int sleep_ms(int ms);
extern uint64_t time_ms(void);


/* serial handling */
typedef struct serial_port_t *serial_port_p;
#define PLC_SERIAL_PORT_NULL ((plc_serial_port)NULL)

extern serial_port_p plc_lib_open_serial_port(plc_lib lib, const char *path, int baud_rate, int data_bits, int stop_bits, int parity_type);
extern int plc_lib_close_serial_port(plc_lib lib, serial_port_p serial_port);
extern int plc_lib_serial_port_read(serial_port_p serial_port, uint8_t *data, int size);
extern int plc_lib_serial_port_write(serial_port_p serial_port, uint8_t *data, int size);



/* socket handling */
typedef struct plc_socket_t *plc_socket;
#define PLC_SOCKET_NULL ((plc_socket)NULL)

extern plc_socket plc_lib_open_socket(plc_lib lib, const char *host_and_port, int default_port);
extern int plc_lib_close_socket(plc_lib lib, plc_socket sock);
extern int plc_lib_socket_get_port(plc_socket sock);
extern int plc_lib_send_buf(plc_socket sock, uint8_t *buf, int size);
extern int plc_lib_recv_buf(plc_socket sock, uint8_t *buf, int size, int timeout);


#ifdef __cplusplus
}
#endif




#endif

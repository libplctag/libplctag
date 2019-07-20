/***************************************************************************
 *   Copyright (C) 2016 by OmanTek                                         *
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
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#ifndef __EXAMPLE_UTILS_H__
#define __EXAMPLE_UTILS_H__

#ifdef _WIN32
    #include <windows.h>
    #define strcasecmp _stricmp
    #define strdup _strdup
	#define snprintf_platform sprintf_s
	#define sscanf_platform sscanf_s
#else
    #include <unistd.h>
    #include <strings.h>
	#define snprintf_platform snprintf
	#define sscanf_platform sscanf
#endif


#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

extern int util_sleep_ms(int ms);
extern int64_t util_time_ms(void);


#ifdef __cplusplus
}
#endif

#endif


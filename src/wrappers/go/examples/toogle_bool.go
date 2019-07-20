/***************************************************************************
 *   Copyright (C) 2019 Aníbal Limón <limon.anibal@gmail.com>                                        *
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


package main

import (
	"fmt"
	"os"
	"plctag"
	"time"
)

const (
	TAG_PATH = "protocol=ab_eip&gateway=10.206.1.27&path=1,0&cpu=LGX&elem_size=1&elem_count=1&debug=1&name=pcomm_test_bool"
	DATA_TIMEOUT = 5000
)

func main() {
	tag := plctag.Create(TAG_PATH, DATA_TIMEOUT);
	if (tag < 0) {
		tag := int(tag) // XXX: plctag.Create returns int32 for tags but DecodeError,Exit expects int
		fmt.Printf("ERROR %s: Could not create tag!\n", plctag.DecodeError(tag))
		os.Exit(tag)
	}

	var status int
	for {
		status = plctag.Status(tag)
		if status != plctag.STATUS_PENDING {
			break
		}
		time.Sleep(100)
	}
	if status != plctag.STATUS_OK {
		fmt.Printf("Error setting up tag internal state. Error %s\n", plctag.DecodeError(status))
		os.Exit(status)
	}

	result := plctag.Read(tag, DATA_TIMEOUT)
	if result != plctag.STATUS_OK {
		fmt.Printf("ERROR: Unable to read the data! Got error code %d: %s\n", result, plctag.DecodeError(result))
		os.Exit(result)
	}
	b := plctag.GetUint8(tag, 0)
	fmt.Printf("bool = %d\n", b)
	if b == 0 {
		b = 255
	} else if b == 255 {
		b = 0
	}
	plctag.SetUint8(tag, 0, b)
	result = plctag.Write(tag, DATA_TIMEOUT)
	if result != plctag.STATUS_OK {
		fmt.Printf("ERROR: Unable to write the data! Got error code %d: %s\n", result, plctag.DecodeError(result))
		os.Exit(result)
	}
	result = plctag.Read(tag, DATA_TIMEOUT)
	if result != plctag.STATUS_OK {
		fmt.Printf("ERROR: Unable to read the data! Got error code %d: %s\n", result, plctag.DecodeError(result))
		os.Exit(result)
	}
	b = plctag.GetUint8(tag, 0)
	fmt.Printf("bool = %d\n", b)

	plctag.Destroy(tag)
}

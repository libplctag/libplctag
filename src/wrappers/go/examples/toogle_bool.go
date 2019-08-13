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
)

const (
	TAG_PATH     = "protocol=ab_eip&gateway=192.168.0.250&path=1,0&cpu=LGX&elem_size=1&elem_count=1&debug=1&name=pcomm_test_bool"
	DATA_TIMEOUT = 5000
)

func main() {
	/* create the tag */
	tag := plctag.Create(TAG_PATH, DATA_TIMEOUT)
	if tag < 0 {
		fmt.Printf("ERROR %s: Could not create tag!\n", plctag.DecodeError(int(tag)))
		os.Exit(0)
	}

	/* everything OK? */
	if rc := plctag.Status(tag); rc != plctag.STATUS_OK {
		fmt.Printf("ERROR %s: Error setting up tag internal state.\n", plctag.DecodeError(rc))
		plctag.Destroy(tag)
		os.Exit(0)
	}

	/* get the data */
	if rc := plctag.Read(tag, DATA_TIMEOUT); rc != plctag.STATUS_OK {
		fmt.Printf("ERROR: Unable to read the data! Got error code %d: %s\n", rc, plctag.DecodeError(rc))
		plctag.Destroy(tag)
		os.Exit(0)
	}

	b := plctag.GetUint8(tag, 0)
	if b > 0 {
		b = 0
		fmt.Printf("bool = ON\n")
	} else {
		b = 255
		fmt.Printf("bool = OFF\n")
	}

	plctag.SetUint8(tag, 0, b)
	if rc := plctag.Write(tag, DATA_TIMEOUT); rc != plctag.STATUS_OK {
		fmt.Printf("ERROR: Unable to write the data! Got error code %d: %s\n", rc, plctag.DecodeError(rc))
		os.Exit(0)
	}

	/* get the data */
	if rc := plctag.Read(tag, DATA_TIMEOUT); rc != plctag.STATUS_OK {
		fmt.Printf("ERROR: Unable to read the data! Got error code %d: %s\n", rc, plctag.DecodeError(rc))
		plctag.Destroy(tag)
		os.Exit(0)
	}

	b = plctag.GetUint8(tag, 0)
	if b > 0 {
		b = 0
		fmt.Printf("bool = ON\n")
	} else {
		b = 255
		fmt.Printf("bool = OFF\n")
	}

	plctag.Destroy(tag)
}

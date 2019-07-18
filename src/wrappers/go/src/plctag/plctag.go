/***************************************************************************
 *   Copyright (C) 2019 Aníbal Limón <limon.anibal@gmail.com>              *
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

package plctag

/*
#cgo pkg-config: libplctag
#include <stdio.h>
#include <stdlib.h>
#include <libplctag.h>
*/
import "C"
import "unsafe"

const (
	STATUS_PENDING = C.PLCTAG_STATUS_PENDING
	STATUS_OK = C.PLCTAG_STATUS_OK

	ERR_ABORT = C.PLCTAG_ERR_ABORT
	ERR_BAD_CONFIG = C.PLCTAG_ERR_BAD_CONFIG
	ERR_BAD_CONNECTION = C.PLCTAG_ERR_BAD_CONFIG
	ERR_BAD_DATA = C.PLCTAG_ERR_BAD_DATA
	ERR_BAD_DEVICE = C.PLCTAG_ERR_BAD_DEVICE
	ERR_BAD_GATEWAY = C.PLCTAG_ERR_BAD_GATEWAY
	ERR_BAD_PARAM = C.PLCTAG_ERR_BAD_PARAM
	ERR_BAD_REPLY = C.PLCTAG_ERR_BAD_REPLY
	ERR_BAD_STATUS = C.PLCTAG_ERR_BAD_STATUS
	ERR_CLOSE = C.PLCTAG_ERR_CLOSE
	ERR_CREATE = C.PLCTAG_ERR_CREATE
	ERR_DUPLICATE = C.PLCTAG_ERR_DUPLICATE
	ERR_ENCODE = C.PLCTAG_ERR_ENCODE
	ERR_MUTEX_DESTROY = C.PLCTAG_ERR_MUTEX_DESTROY
	ERR_MUTEX_INIT = C.PLCTAG_ERR_MUTEX_INIT
	ERR_MUTEX_LOCK = C.PLCTAG_ERR_MUTEX_LOCK
	ERR_MUTEX_UNLOCK = C.PLCTAG_ERR_MUTEX_UNLOCK
	ERR_NOT_ALLOWED = C.PLCTAG_ERR_NOT_ALLOWED
	ERR_NOT_FOUND = C.PLCTAG_ERR_NOT_FOUND
	ERR_NOT_IMPLEMENTED = C.PLCTAG_ERR_NOT_IMPLEMENTED
	ERR_NO_DATA = C.PLCTAG_ERR_NO_DATA
	ERR_NO_MATCH = C.PLCTAG_ERR_NO_MATCH
	ERR_NO_MEM = C.PLCTAG_ERR_NO_MEM
	ERR_NO_RESOURCES = C.PLCTAG_ERR_NO_RESOURCES
	ERR_NULL_PTR = C.PLCTAG_ERR_NULL_PTR
	ERR_OPEN = C.PLCTAG_ERR_OPEN
	ERR_OUT_OF_BOUNDS = C.PLCTAG_ERR_OUT_OF_BOUNDS
	ERR_READ = C.PLCTAG_ERR_READ
	ERR_REMOTE_ERR = C.PLCTAG_ERR_REMOTE_ERR
	ERR_THREAD_CREATE = C.PLCTAG_ERR_THREAD_CREATE
	ERR_THREAD_JOIN = C.PLCTAG_ERR_THREAD_JOIN
	ERR_TIMEOUT = C.PLCTAG_ERR_TIMEOUT
	ERR_TOO_LARGE = C.PLCTAG_ERR_TOO_LARGE
	ERR_TOO_SMALL = C.PLCTAG_ERR_TOO_SMALL
	ERR_UNSUPPORTED = C.PLCTAG_ERR_UNSUPPORTED
	ERR_WINSOCK = C.PLCTAG_ERR_WINSOCK
	ERR_WRITE = C.PLCTAG_ERR_WRITE
	ERR_PARTIAL = C.PLCTAG_ERR_PARTIAL
)

func DecodeError(err int) string {
	cstr := C.plc_tag_decode_error(C.int(err))
	return C.GoString(cstr)
}

func Create(attrib_str string, timeout int) int32 {
	cattrib_str := C.CString(attrib_str)
	result := C.plc_tag_create(cattrib_str, C.int(timeout))
	C.free(unsafe.Pointer(cattrib_str))
	return int32(result)
}

func Destroy(tag int32) int {
	result := C.plc_tag_destroy(C.int32_t(tag))
	return int(result)
}

func Lock(tag int32) int {
	result := C.plc_tag_lock(C.int32_t(tag))
	return int(result)
}

func Unlock(tag int32) int {
	result := C.plc_tag_unlock(C.int32_t(tag))
	return int(result)
}

func Abort(tag int32) int {
	result := C.plc_tag_abort(C.int32_t(tag))
	return int(result)
}

func Status(tag int32) int {
	result := C.plc_tag_status(C.int32_t(tag))
	return int(result)
}

func GetSize(tag int32) int {
	result := C.plc_tag_get_size(C.int32_t(tag))
	return int(result)
}

func Read(tag int32, timeout int) int {
	result := C.plc_tag_read(C.int32_t(tag), C.int(timeout))
	return int(result)
}

func Write(tag int32, timeout int) int {
	result := C.plc_tag_write(C.int32_t(tag), C.int(timeout))
	return int(result)
}

func GetUint64(tag int32, offset int) uint64 {
	result := C.plc_tag_get_uint64(C.int32_t(tag), C.int(offset))
	return uint64(result)
}

func SetUint64(tag int32, offset int, val uint64) int {
	result := C.plc_tag_set_uint64(C.int32_t(tag), C.int(offset), C.uint64_t(val))
	return int(result)
}

func GetInt64(tag int32, offset int) int64 {
	result := C.plc_tag_get_int64(C.int32_t(tag), C.int(offset))
	return int64(result)
}

func SetInt64(tag int32, offset int, val int64) int {
	result := C.plc_tag_set_int64(C.int32_t(tag), C.int(offset), C.int64_t(val))
	return int(result)
}

func GetUint32(tag int32, offset int) uint32 {
	result := C.plc_tag_get_uint32(C.int32_t(tag), C.int(offset))
	return uint32(result)
}

func SetUint32(tag int32, offset int, val uint32) int {
	result := C.plc_tag_set_uint32(C.int32_t(tag), C.int(offset), C.uint32_t(val))
	return int(result)
}

func GetInt32(tag int32, offset int) int32 {
	result := C.plc_tag_get_int32(C.int32_t(tag), C.int(offset))
	return int32(result)
}

func SetInt32(tag int32, offset int, val int32) int {
	result := C.plc_tag_set_int32(C.int32_t(tag), C.int(offset), C.int32_t(val))
	return int(result)
}

func GetUint16(tag int32, offset int) uint16 {
	result := C.plc_tag_get_uint16(C.int32_t(tag), C.int(offset))
	return uint16(result)
}

func SetUint16(tag int32, offset int, val uint16) int {
	result := C.plc_tag_set_uint16(C.int32_t(tag), C.int(offset), C.uint16_t(val))
	return int(result)
}

func GetInt16(tag int32, offset int) int16 {
	result := C.plc_tag_get_int16(C.int32_t(tag), C.int(offset))
	return int16(result)
}

func SetInt16(tag int32, offset int, val int16) int {
	result := C.plc_tag_set_int16(C.int32_t(tag), C.int(offset), C.int16_t(val))
	return int(result)
}

func GetUint8(tag int32, offset int) uint8 {
	result := C.plc_tag_get_uint8(C.int32_t(tag), C.int(offset))
	return uint8(result)
}

func SetUint8(tag int32, offset int, val uint8) int {
	result := C.plc_tag_set_uint8(C.int32_t(tag), C.int(offset), C.uint8_t(val))
	return int(result)
}

func GetInt8(tag int32, offset int) int8 {
	result := C.plc_tag_get_int8(C.int32_t(tag), C.int(offset))
	return int8(result)
}

func SetInt8(tag int32, offset int, val int8) int {
	result := C.plc_tag_set_int8(C.int32_t(tag), C.int(offset), C.int8_t(val))
	return int(result)
}

func GetFloat64(tag int32, offset int) float64 {
	result := C.plc_tag_get_float64(C.int32_t(tag), C.int(offset))
	return float64(result)
}

func SetFloat64(tag int32, offset int, val float64) int {
	result := C.plc_tag_set_float64(C.int32_t(tag), C.int(offset), C.double(val))
	return int(result)
}

func GetFloat32(tag int32, offset int) float32 {
	result := C.plc_tag_get_float32(C.int32_t(tag), C.int(offset))
	return float32(result)
}

func SetFloat32(tag int32, offset int, val float32) int {
	result := C.plc_tag_set_float32(C.int32_t(tag), C.int(offset), C.float(val))
	return int(result)
}

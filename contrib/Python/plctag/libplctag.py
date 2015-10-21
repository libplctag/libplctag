#!/usr/bin/python
import ctypes
import platform

PLCTAG_STATUS_PENDING = 1
PLCTAG_STATUS_OK      = 0

# References:
#
# https://docs.python.org/2/library/ctypes.html see in particular the
# Fundamental data types secion and all about Foreign functions


# This is the creator function for the public plc_tag_create()
# method.  This method returns a ctypes c_void_p, because we
# need to keep a C pointer to the plc_tag struct defining the tag
# session for future calls.

def definePointerFunc(name, args):
    func = name
    func.restype = ctypes.c_void_p
    func.argtypes = args
    return func


# This is the creator function for all public methods returning
# a ctypes c_int type, a standard 32-bit int in C code.

def defineIntFunc(name, args):
    func = name
    func.restype = ctypes.c_int
    func.argtypes = args
    return func


# The following are creator functions for the data accessors that
# return unsigned and smaller types

def defineUIntFunc(name, args):
    func = name
    func.restype = ctypes.c_uint
    func.argtypes = args
    return func

def defineShortFunc(name, args):
    func = name
    func.restype = ctypes.c_short
    func.argtypes = args
    return func

def defineUShortFunc(name, args):
    func = name
    func.restype = ctypes.c_ushort
    func.argtypes = args
    return func

def defineByteFunc(name, args):
    func = name
    func.restype = ctypes.c_byte
    func.argtypes = args
    return func

def defineUByteFunc(name, args):
    func = name
    func.restype = ctypes.c_ubyte
    func.argtypes = args
    return func

def defineFloatFunc(name, args):
    func = name
    func.restype = ctypes.c_float
    func.argtypes = args
    return func



# Load the C library; should be platform independent.  This must
# succeed for the python bindings to work.  The system must be able
# to load the compiled libplctag C library from the current environment
# or LD_LIBRARY path.
system = platform.system()
library = "plctag.dll" if system == "Windows" else "libplctag.so"
lib = ctypes.cdll.LoadLibrary(library)


# Create the tag functions below

plcTagCreate  = definePointerFunc(lib.plc_tag_create, [ctypes.c_char_p])
plcTagLock    = defineIntFunc(lib.plc_tag_lock, [ctypes.c_char_p])
plcTagUnlock  = defineIntFunc(lib.plc_tag_unlock, [ctypes.c_char_p])
plcTagAbort   = defineIntFunc(lib.plc_tag_abort, [ctypes.c_void_p])
plcTagDestroy = defineIntFunc(lib.plc_tag_destroy, [ctypes.c_void_p])
plcTagRead    = defineIntFunc(lib.plc_tag_read, [ctypes.c_void_p, ctypes.c_int])
plcTagStatus  = defineIntFunc(lib.plc_tag_status, [ctypes.c_void_p])
plcTagWrite   = defineIntFunc(lib.plc_tag_write, [ctypes.c_void_p, ctypes.c_int])


# Create the tag data accessor functions below:

plcTagGetSize = defineIntFunc(lib.plc_tag_get_size, [ctypes.c_void_p, ctypes.c_int])


# Create the 32-bit tag data accessors

# Creates UIntFunc because it returns a uint32_t type
plcTagGetUInt32 = defineUIntFunc(lib.plc_tag_get_uint32, [ctypes.c_void_p, ctypes.c_int])
# Creates IntFunc because it returns int for the result, but notice it takes a uint for the val
plcTagSetUInt32 = defineIntFunc(lib.plc_tag_set_uint32, [ctypes.c_void_p, ctypes.c_int, ctypes.c_uint])

# Creates IntFunc because it returns a int32_t type
plcTagGetInt32  = defineIntFunc(lib.plc_tag_get_int32, [ctypes.c_void_p, ctypes.c_int])
# Creates IntFunc because it returns int for the result, and sets int for the value
plcTagSetInt32  = defineIntFunc(lib.plc_tag_set_int32, [ctypes.c_void_p, ctypes.c_int, ctypes.c_int])


# Create the 16-bit tag data accessors

# Creates UShortFunc because it returns a uint16_t type
plcTagGetUInt16 = defineUShortFunc(lib.plc_tag_get_uint16, [ctypes.c_void_p, ctypes.c_int])
# Creates IntFunc because it returns int for the result, but notice it takes a ushort for the val
plcTagSetUInt16 = defineIntFunc(lib.plc_tag_set_uint16, [ctypes.c_void_p, ctypes.c_int, ctypes.c_ushort])

# Creates ShortFunc because it returns a int16_t type
plcTagGetInt16  = defineShortFunc(lib.plc_tag_get_int16, [ctypes.c_void_p, ctypes.c_int])
# Creates IntFunc because it returns int for the result, but notice it takes a short for the val
plcTagSetInt16  = defineIntFunc(lib.plc_tag_set_int16, [ctypes.c_void_p, ctypes.c_int, ctypes.c_short])


# Create the 8-bit tag data accessors

# Creates UByteFunc because it returns a uint8_t type
plcTagGetUInt8 = defineUByteFunc(lib.plc_tag_get_uint8, [ctypes.c_void_p, ctypes.c_int])
# Creates IntFunc because it returns int for the result, but notice it takes a ubyte for the val
plcTagSetUInt8 = defineIntFunc(lib.plc_tag_set_uint8, [ctypes.c_void_p, ctypes.c_int, ctypes.c_ubyte])

# Creates ByteFunc because it returns a int8_t type
plcTagGetInt8  = defineByteFunc(lib.plc_tag_get_int8, [ctypes.c_void_p, ctypes.c_int])
# Creates IntFunc because it returns int for the result, but notice it takes a byte for the val
plcTagSetInt8  = defineIntFunc(lib.plc_tag_set_int8, [ctypes.c_void_p, ctypes.c_int, ctypes.c_byte])


# Create the floating point tag data accessors

# Creates FloatFunc because it returns a float type
plcTagGetFloat32 = defineFloatFunc(lib.plc_tag_get_float32, [ctypes.c_void_p, ctypes.c_int])
# Creates IntFunc because it returns int for the result, but notice it takes a float for the val
plcTagSetFloat32 = defineIntFunc(lib.plc_tag_set_float32, [ctypes.c_void_p, ctypes.c_int, ctypes.c_float])



##############################################################################
# What follows is the definitions for the actual public python methods.
# They return the result of the methods we created using the ctypes library,
# and all the "defineFunc" methods.  These below are what a client python
# program should call: plc_tag_create, plc_tag_destroy, etc.
#
# Do not call on accident the created methods that these call.  Underscores
# should be present in client calls just like in tag_rw.c !
##############################################################################

# tag functions
#
# The following is the public API for tag operations
#
# These are implemented in a protocol-specific manner.

# plc_tag_create
# 
# Create a new tag based on the passed attributed string.  The attributes
# are protocol-specific.  The only required part of the string is the key-
# value pair "protocol=XXX" where XXX is one of the supported protocol
# types.
#
# An opaque pointer is returned on success.  NULL is returned on allocation
# failure.  Other failures will set the tag status.
#
def plc_tag_create(attributeString):
    return plcTagCreate(attributeString)

# plc_tag_lock
#
# Lock the tag against use by other threads.  Because operations on a tag are
# very much asynchronous, actions like getting and extracting the data from
# a tag take more than one API call.  If more than one thread is using the same tag,
# then the internal state of the tag will get broken and you will probably experience
# a crash.
#
# This should be used to initially lock a tag when starting operations with it
# followed by a call to plc_tag_unlock when you have everything you need from the tag.
#
def plc_tag_lock(tag):
    return plcTagLock(tag)

# plc_tag_unlock
#
# The opposite action of plc_tag_unlock.  This allows other threads to access the
# tag.
#
def plc_tag_unlock(tag):
    return plcTagUnlock(tag)

# plc_tag_abort
#
# Abort any outstanding IO to the PLC.  If there is something in flight, then
# it is marked invalid.  Note that this does not abort anything that might
# be still processing in the report PLC.
#
# The status will be PLCTAG_STATUS_OK unless there is an error such as
# a null pointer.
#
# This is a function provided by the underlying protocol implementation.
#
def plc_tag_abort(tag):
    return plcTagAbort(tag)

# plc_tag_destroy
# 
# This frees all resources associated with the tag.  Internally, it may result in closed
# connections etc.   This calls through to a protocol-specific function.
#
# This is a function provided by the underlying protocol implementation.
#
def plc_tag_destroy(tag):
    return plcTagDestroy(tag)

# plc_tag_read
#
# Start a read.  If the timeout value is zero, then wait until the read
# returns or the timeout occurs, whichever is first.  Return the status.
# If the timeout value is zero, then plc_tag_read will normally return
# PLCTAG_STATUS_PENDING.
#
# This is a function provided by the underlying protocol implementation.
#
def plc_tag_read(tag, timeout):
    return plcTagRead(tag, timeout)

# plc_tag_status
#
# Return the current status of the tag.  This will be PLCTAG_STATUS_PENDING if there is
# an uncompleted IO operation.  It will be PLCTAG_STATUS_OK if everything is fine.  Other
# errors will be returned as appropriate.
#
# This is a function provided by the underlying protocol implementation.
#
def plc_tag_status(tag):
    return plcTagStatus(tag)

# plc_tag_write
#
# Start a write.  If the timeout value is zero, then wait until the write
# returns or the timeout occurs, whichever is first.  Return the status.
# If the timeout value is zero, then plc_tag_write will usually return
# PLCTAG_STATUS_PENDING.  The write is considered done
# when it has been written to the socket.
#
# This is a function provided by the underlying protocol implementation.
#
def plc_tag_write(tag, timeout):
    return plcTagWrite(tag, timeout)


# Tag data accessors follow:

def plc_tag_get_size(tag):
    return plcTagGetSize(tag)

# 32-bit

def plc_tag_get_uint32(tag, offset):
    return plcTagGetUInt32(tag, offset)

def plc_tag_set_uint32(tag, offset, value):
    return plcTagSetUInt32(tag, offset, value)

def plc_tag_get_int32(tag, offset):
    return plcTagGetInt32(tag, offset)

def plc_tag_set_int32(tag, offset, value):
    return plcTagSetInt32(tag, offset, value)

# 16-bit

def plc_tag_get_uint16(tag, offset):
    return plcTagGetUInt16(tag, offset)

def plc_tag_set_uint16(tag, offset, value):
    return plcTagSetUInt16(tag, offset, value)

def plc_tag_get_int16(tag, offset):
    return plcTagGetInt16(tag, offset)

def plc_tag_set_int16(tag, offset, value):
    return plcTagSetInt16(tag, offset, value)

# 8-bit

def plc_tag_get_uint8(tag, offset):
    return plcTagGetUInt8(tag, offset)

def plc_tag_set_uint8(tag, offset, value):
    return plcTagSetUInt8(tag, offset, value)

def plc_tag_get_int8(tag, offset):
    return plcTagGetInt8(tag, offset)

def plc_tag_set_int8(tag, offset, value):
    return plcTagSetInt8(tag, offset, value)


# Floating point (real)

def plc_tag_get_float32(tag, offset):
    return plcTagGetFloat32(tag, offset)

def plc_tag_set_float32(tag, offset, value):
    return plcTagSetFloat32(tag, offset, value)



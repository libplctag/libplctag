#!/usr/bin/python
import platform
import ctypes

PLCTAG_STATUS_PENDING = 1
PLCTAG_STATUS_OK      = 0

# References:
#
# https://docs.python.org/2/library/ctypes.html see in particular the
# Fundamental data types secion and all about Foreign functions



# This is the creator function for all public methods returning
# a ctypes c_int type, a standard 32-bit int in C code.

def defineIntFunc(name, args):
    func = name
    func.restype = ctypes.c_int
    func.argtypes = args
    return func

# other creator functions for non 32-bit int values

def defineUIntFunc(name, args):
    func = name
    func.restype = ctypes.c_uint
    func.argtypes = args
    return func

def defineLongFunc(name, args):
    func = name
    func.restype = ctypes.c_int64
    func.argstypes = args
    return func

def defineULongFunc(name, args):
    func = name
    func.restype = ctypes.c_uint64
    func.argstypes = args
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

def defineDoubleFunc(name, args):
    func = name
    func.restype = ctypes.c_double
    func.argtypes = args
    return func

def defineStringFunc(name, args):
    func = name
    func.restype = ctypes.c_char_p
    func.argtypes = args
    return func



# Load the C library; should be platform independent.  This must
# succeed for the python bindings to work.  The system must be able
# to load the compiled libplctag C library from the current environment
# or LD_LIBRARY path.

# New:
# Updated code requires creating a folder structure as it is in the
# included pictures, with each folder containing its corresponding library.
# Optionally, you can only create the folder for the Operating System you will be using.
# The string tag data accessors were introduced in v2.2.0, so get at least that release.

system = platform.system()

library = ""

if system == "Windows":
    if platform.machine().endswith('64'):
        library = "./windows_x64/plctag.dll"
    else:
        library = "./windows_x86/plctag.dll"
elif system == "Darwin":
    library = "./macos_x64/libplctag.dylib"
elif system == "Linux":
    if platform.machine() == 'armv7l': # Android or Linux
        library = "./armeabi-v7a/libplctag.so"
    elif platform.machine().startswith('aarch64') or platform.machine().startswith('armv8'): # Android or Linux
        library = "./arm64-v8a/libplctag.so"
    elif platform.machine().endswith('64'):
        library = "./ubuntu_x64/libplctag.so"
    else:
        library = "./ubuntu_x86/libplctag.so"
else: # Java or some other
    pass

if library != "":
    lib = ctypes.cdll.LoadLibrary(library)

# Create the tag functions below

plcTagCheckLibVersion = defineIntFunc(lib.plc_tag_check_lib_version, [ctypes.c_int, ctypes.c_int, ctypes.c_int])
plcTagGetIntAttribute = defineIntFunc(lib.plc_tag_get_int_attribute, [ctypes.c_int, ctypes.c_char_p, ctypes.c_int])
plcTagSetIntAttribute = defineIntFunc(lib.plc_tag_set_int_attribute, [ctypes.c_int, ctypes.c_char_p, ctypes.c_int])
plcTagDecodeError = defineStringFunc(lib.plc_tag_decode_error, [ctypes.c_int])
plcTagCreate  = defineIntFunc(lib.plc_tag_create, [ctypes.c_char_p, ctypes.c_int])
plcTagLock    = defineIntFunc(lib.plc_tag_lock, [ctypes.c_int])
plcTagUnlock  = defineIntFunc(lib.plc_tag_unlock, [ctypes.c_int])
plcTagAbort   = defineIntFunc(lib.plc_tag_abort, [ctypes.c_int])
plcTagDestroy = defineIntFunc(lib.plc_tag_destroy, [ctypes.c_int])
plcTagRead    = defineIntFunc(lib.plc_tag_read, [ctypes.c_int, ctypes.c_int])
plcTagStatus  = defineIntFunc(lib.plc_tag_status, [ctypes.c_int])
plcTagWrite   = defineIntFunc(lib.plc_tag_write, [ctypes.c_int, ctypes.c_int])

# Create the tag data accessor functions below:

plcTagGetSize = defineIntFunc(lib.plc_tag_get_size, [ctypes.c_int])

# create the bit/bool tag data accessors
plcTagGetBit = defineIntFunc(lib.plc_tag_get_bit, [ctypes.c_int, ctypes.c_int])
plcTagSetBit = defineIntFunc(lib.plc_tag_set_bit, [ctypes.c_int, ctypes.c_int, ctypes.c_int])

# create the 64-bit tag data accessors
plcTagGetInt64 = defineLongFunc(lib.plc_tag_get_int64, [ctypes.c_int, ctypes.c_int])
plcTagSetInt64 = defineIntFunc(lib.plc_tag_set_int64, [ctypes.c_int, ctypes.c_int, ctypes.c_int64])

plcTagGetUInt64 = defineULongFunc(lib.plc_tag_get_uint64, [ctypes.c_int, ctypes.c_int])
plcTagSetUInt64 = defineIntFunc(lib.plc_tag_set_uint64, [ctypes.c_int, ctypes.c_int, ctypes.c_int64])

# Create the 32-bit tag data accessors

# Creates UIntFunc because it returns a uint32_t type
plcTagGetUInt32 = defineUIntFunc(lib.plc_tag_get_uint32, [ctypes.c_int, ctypes.c_int])
# Creates IntFunc because it returns int for the result, but notice it takes a uint for the val
plcTagSetUInt32 = defineIntFunc(lib.plc_tag_set_uint32, [ctypes.c_int, ctypes.c_int, ctypes.c_uint])

# Creates IntFunc because it returns a int32_t type
plcTagGetInt32  = defineIntFunc(lib.plc_tag_get_int32, [ctypes.c_int, ctypes.c_int])
# Creates IntFunc because it returns int for the result, and sets int for the value
plcTagSetInt32  = defineIntFunc(lib.plc_tag_set_int32, [ctypes.c_int, ctypes.c_int, ctypes.c_int])

# Create the 16-bit tag data accessors

# Creates UShortFunc because it returns a uint16_t type
plcTagGetUInt16 = defineUShortFunc(lib.plc_tag_get_uint16, [ctypes.c_int, ctypes.c_int])
# Creates IntFunc because it returns int for the result, but notice it takes a ushort for the val
plcTagSetUInt16 = defineIntFunc(lib.plc_tag_set_uint16, [ctypes.c_int, ctypes.c_int, ctypes.c_ushort])

# Creates ShortFunc because it returns a int16_t type
plcTagGetInt16  = defineShortFunc(lib.plc_tag_get_int16, [ctypes.c_int, ctypes.c_int])
# Creates IntFunc because it returns int for the result, but notice it takes a short for the val
plcTagSetInt16  = defineIntFunc(lib.plc_tag_set_int16, [ctypes.c_int, ctypes.c_int, ctypes.c_short])

# Create the 8-bit tag data accessors

# Creates UByteFunc because it returns a uint8_t type
plcTagGetUInt8 = defineUByteFunc(lib.plc_tag_get_uint8, [ctypes.c_int, ctypes.c_int])
# Creates IntFunc because it returns int for the result, but notice it takes a ubyte for the val
plcTagSetUInt8 = defineIntFunc(lib.plc_tag_set_uint8, [ctypes.c_int, ctypes.c_int, ctypes.c_ubyte])

# Creates ByteFunc because it returns a int8_t type
plcTagGetInt8  = defineByteFunc(lib.plc_tag_get_int8, [ctypes.c_int, ctypes.c_int])
# Creates IntFunc because it returns int for the result, but notice it takes a byte for the val
plcTagSetInt8  = defineIntFunc(lib.plc_tag_set_int8, [ctypes.c_int, ctypes.c_int, ctypes.c_byte])

# Create the floating point tag data accessors

plcTagGetFloat64 = defineDoubleFunc(lib.plc_tag_get_float64, [ctypes.c_int, ctypes.c_int])
plcTagSetFloat64 = defineIntFunc(lib.plc_tag_set_float64, [ctypes.c_int, ctypes.c_int, ctypes.c_double])

# Creates FloatFunc because it returns a float type
plcTagGetFloat32 = defineFloatFunc(lib.plc_tag_get_float32, [ctypes.c_int, ctypes.c_int])
# Creates IntFunc because it returns int for the result, but notice it takes a float for the val
plcTagSetFloat32 = defineIntFunc(lib.plc_tag_set_float32, [ctypes.c_int, ctypes.c_int, ctypes.c_float])



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


# plc_tag_check_lib_version
#
# Check that the library supports the required API version.
#
# PLCTAG_STATUS_OK (0) is returned if the version matches.  If it does not,
# PLCTAG_ERR_UNSUPPORTED (-35) is returned.
#
def plc_tag_check_lib_version(req_major, req_minor, req_patch):
    return plcTagCheckLibVersion(req_major, req_minor, req_patch)


# plc_tag_get_int_attribute
#
# Get library attribute by specifying id as follows:
# for id = 0 the following attributes are available
# 'version_major', 'version_minor', 'version_patch', 'debug', deprecated 'debug_level'
# for id = tag_id the following attributes are available
# 'size', 'read_cache_ms', 'auto_sync_read_ms', 'auto_sync_write_ms'
#
def plc_tag_get_int_attribute(id, attrib_name, default_value):
    return plcTagGetIntAttribute(id, attrib_name.encode('utf-8'), default_value)


# plc_tag_set_int_attribute
#
# Set library attribute by specifying id as follows:
# for id = 0 the following attributes are available
# 'debug', deprecated 'debug_level'
# for id = tag_id the following attributes are available
# 'read_cache_ms', 'auto_sync_read_ms', 'auto_sync_write_ms'
#
def plc_tag_set_int_attribute(id, attrib_name, new_value):
    return plcTagSetIntAttribute(id, attrib_name.encode('utf-8'), new_value)


# plc_tag_decode_error
#
# decode the passed error integer value into a C string.
#
def plc_tag_decode_error(errCode):
    return plcTagDecodeError(errCode)


# plc_tag_create
#
# Create a new tag based on the passed attributed string.  The attributes
# are protocol-specific.  The only required part of the string is the key-
# value pair "protocol=XXX" where XXX is one of the supported protocol
# types.
#
# An handle integer is returned on success.  If negative, the return provides
# the status.
#
def plc_tag_create(attributeString, timeout):
    #print ("Creating tag with attributes '%s' and timeout %d" % (attributeString, timeout))
    return plcTagCreate(attributeString.encode('utf-8'), timeout)


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

# bit/bool

def plc_tag_get_bit(tag, offset):
    return plcTagGetBit(tag, offset)

def plc_tag_set_bit(tag, offset, value):
    return plcTagSetBit(tag, offset, value)

# 64-bit

def plc_tag_get_uint64(tag, offset):
    return plcTagGetUInt64(tag, offset)

def plc_tag_set_uint64(tag, offset, value):
    return plcTagSetUInt64(tag, offset, value)

def plc_tag_get_int64(tag, offset):
    return plcTagGetInt64(tag, offset)

def plc_tag_set_int64(tag, offset, value):
    return plcTagSetInt64(tag, offset, value)

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

def plc_tag_get_float64(tag, offset):
    return plcTagGetFloat64(tag, offset)

def plc_tag_set_float64(tag, offset, value):
    return plcTagSetFloat64(tag, offset, value)

def plc_tag_get_float32(tag, offset):
    return plcTagGetFloat32(tag, offset)

def plc_tag_set_float32(tag, offset, value):
    return plcTagSetFloat32(tag, offset, value)

# String, library dependent

if plc_tag_check_lib_version(2, 2, 0) == 0:
    # create the string tag data accessors
    plcTagGetString = defineIntFunc(lib.plc_tag_get_string, [ctypes.c_int, ctypes.c_int, ctypes.c_char_p, ctypes.c_int])
    plcTagSetString = defineIntFunc(lib.plc_tag_set_string, [ctypes.c_int, ctypes.c_int, ctypes.c_char_p])
    plcTagGetStringLength = defineIntFunc(lib.plc_tag_get_string_length, [ctypes.c_int, ctypes.c_int])
    plcTagGetStringCapacity = defineIntFunc(lib.plc_tag_get_string_capacity, [ctypes.c_int, ctypes.c_int])
    plcTagGetStringTotalLength = defineIntFunc(lib.plc_tag_get_string_total_length, [ctypes.c_int, ctypes.c_int])

    def plc_tag_get_string(tag, string_start_offset, char_buffer, buffer_length):
        return plcTagGetString(tag, string_start_offset, char_buffer, buffer_length)

    def plc_tag_set_string(tag, string_start_offset, string_value):
        return plcTagSetString(tag, string_start_offset, string_value)

    def plc_tag_get_string_length(tag, string_start_offset):
        return plcTagGetStringLength(tag, string_start_offset)

    def plc_tag_get_string_capacity(tag, string_start_offset):
        return plcTagGetStringCapacity(tag, string_start_offset)

    def plc_tag_get_string_total_length(tag, string_start_offset):
        return plcTagGetStringTotalLength(tag, string_start_offset)

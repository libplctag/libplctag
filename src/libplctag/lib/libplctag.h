/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever     *
 * you choose.                                                             *
 *                                                                         *
 * MPL 2.0:                                                                *
 *                                                                         *
 *   This Source Code Form is subject to the terms of the Mozilla Public   *
 *   License, v. 2.0. If a copy of the MPL was not distributed with this   *
 *   file, You can obtain one at http://mozilla.org/MPL/2.0/.              *
 *                                                                         *
 *                                                                         *
 * LGPL 2:                                                                 *
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

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


#if defined(_WIN32) || defined(WIN32) || defined(WIN64) || defined(_WIN64)
#    ifdef __cplusplus
#        define C_FUNC extern "C"
#    else
#        define C_FUNC
#    endif

#    ifdef LIBPLCTAGDLL_EXPORTS
#        define LIB_EXPORT __declspec(dllexport)
#    else
#        define LIB_EXPORT extern
#    endif

// #ifdef LIBPLCTAG_STATIC
//     #define LIB_EXPORT extern
// #elif defined(LIBPLCTAGDLL_EXPORTS)
//     #define LIB_EXPORT __declspec(dllexport)
//     #error "DLL Export"
// #else
//     #define LIB_EXPORT __declspec(dllimport)
//     #error "DLL Import"
// #endif
#else
#    ifdef LIBPLCTAGDLL_EXPORTS
#        define LIB_EXPORT __attribute__((visibility("default")))
#    else
#        define LIB_EXPORT extern
#    endif
#endif

/* Experimental API marker - generates compiler warnings when used */
#if defined(_MSC_VER)
/* MSVC */
#    define LIBPLCTAG_EXPERIMENTAL __declspec(deprecated("This function is experimental and may change in future releases"))
#elif defined(__GNUC__) || defined(__clang__)
/* GCC and Clang */
#    define LIBPLCTAG_EXPERIMENTAL __attribute__((deprecated("This function is experimental and may change in future releases")))
#else
/* Unknown compiler - no warning */
#    define LIBPLCTAG_EXPERIMENTAL
#endif


/* library internal status and error codes - generated from error_codes.def */
typedef enum {
    PLCTAG_STATUS_PENDING = 1,
    PLCTAG_STATUS_OK = 0,
    PLCTAG_ERR_ABORT = -1,
    PLCTAG_ERR_BAD_CONFIG = -2,
    PLCTAG_ERR_BAD_CONNECTION = -3,
    PLCTAG_ERR_BAD_DATA = -4,
    PLCTAG_ERR_BAD_DEVICE = -5,
    PLCTAG_ERR_BAD_GATEWAY = -6,
    PLCTAG_ERR_BAD_PARAM = -7,
    PLCTAG_ERR_BAD_REPLY = -8,
    PLCTAG_ERR_BAD_STATUS = -9,
    PLCTAG_ERR_CLOSE = -10,
    PLCTAG_ERR_CREATE = -11,
    PLCTAG_ERR_DUPLICATE = -12,
    PLCTAG_ERR_ENCODE = -13,
    PLCTAG_ERR_MUTEX_DESTROY = -14,
    PLCTAG_ERR_MUTEX_INIT = -15,
    PLCTAG_ERR_MUTEX_LOCK = -16,
    PLCTAG_ERR_MUTEX_UNLOCK = -17,
    PLCTAG_ERR_NOT_ALLOWED = -18,
    PLCTAG_ERR_NOT_FOUND = -19,
    PLCTAG_ERR_NOT_IMPLEMENTED = -20,
    PLCTAG_ERR_NO_DATA = -21,
    PLCTAG_ERR_NO_MATCH = -22,
    PLCTAG_ERR_NO_MEM = -23,
    PLCTAG_ERR_NO_RESOURCES = -24,
    PLCTAG_ERR_NULL_PTR = -25,
    PLCTAG_ERR_OPEN = -26,
    PLCTAG_ERR_OUT_OF_BOUNDS = -27,
    PLCTAG_ERR_READ = -28,
    PLCTAG_ERR_REMOTE_ERR = -29,
    PLCTAG_ERR_THREAD_CREATE = -30,
    PLCTAG_ERR_THREAD_JOIN = -31,
    PLCTAG_ERR_TIMEOUT = -32,
    PLCTAG_ERR_TOO_LARGE = -33,
    PLCTAG_ERR_TOO_SMALL = -34,
    PLCTAG_ERR_UNSUPPORTED = -35,
    PLCTAG_ERR_WINSOCK = -36,
    PLCTAG_ERR_WRITE = -37,
    PLCTAG_ERR_PARTIAL = -38,
    PLCTAG_ERR_BUSY = -39
} plctag_error_code_t;


/**
 * @brief Convert error code to human-readable string.
 *
 * Takes one of the error codes and turns it into a const char * suitable
 * for printing.
 *
 * @param err Error code to decode.
 * @return Human-readable error string.
 */

LIB_EXPORT const char *plc_tag_decode_error(int err);


/**
 * @brief Debug Level Definitions.
 *
 * This is the canonical list of debug levels used throughout the library.
 * Debug levels control the verbosity of library output.
 * This is the canonical list of debug levels used throughout the library.
 * From this list the debug level name strings used in the debug functions are derived.
 */

/** Debug level constants - generated from debug_levels.def */
typedef enum {
    PLCTAG_DEBUG_NONE = 0,
    PLCTAG_DEBUG_ERROR = 1,
    PLCTAG_DEBUG_WARN = 2,
    PLCTAG_DEBUG_INFO = 3,
    PLCTAG_DEBUG_DETAIL = 4,
    PLCTAG_DEBUG_SPEW = 5
} plctag_debug_level_t;

LIB_EXPORT void plc_tag_set_debug_level(int debug_level);


/**
 * @brief Debug Module Definitions.
 *
 * The following module IDs can be used with the module-specific debug functions.
 * Each module represents a different subsystem within libplctag and can have its
 * own independent debug level setting.
 *
 * This is the canonical list of module IDs used throughout the library. From this
 * list the module name strings used in the debug functions are derived.
 *
 * @note Do not rely on these values remaining constant between library versions
 *       as they will likely be added to in future releases.
 */

/** Debug module IDs - these are 64-bit pre-shifted values for bitmask operations */
typedef enum {
    PLCTAG_MODULE_LIB = (1ULL << 0),
    PLCTAG_MODULE_INIT = (1ULL << 1),
    PLCTAG_MODULE_VERSION = (1ULL << 2),
    PLCTAG_MODULE_UTILS = (1ULL << 3),
    PLCTAG_MODULE_AB_SESSION = (1ULL << 4),
    PLCTAG_MODULE_AB_PCCC = (1ULL << 5),
    PLCTAG_MODULE_AB_CIP = (1ULL << 6),
    PLCTAG_MODULE_AB_COMMON = (1ULL << 7),
    PLCTAG_MODULE_AB_EIP_CIP = (1ULL << 8),
    PLCTAG_MODULE_AB_EIP_CIP_SPECIAL = (1ULL << 9),
    PLCTAG_MODULE_AB_EIP_LGX_PCCC = (1ULL << 10),
    PLCTAG_MODULE_AB_EIP_PLC5_PCCC = (1ULL << 11),
    PLCTAG_MODULE_AB_EIP_PLC5_DHP = (1ULL << 12),
    PLCTAG_MODULE_AB_EIP_SLC_PCCC = (1ULL << 13),
    PLCTAG_MODULE_AB_EIP_SLC_DHP = (1ULL << 14),
    PLCTAG_MODULE_AB_ERROR = (1ULL << 15),
    PLCTAG_MODULE_OMRON_CONN = (1ULL << 16),
    PLCTAG_MODULE_OMRON_CIP = (1ULL << 17),
    PLCTAG_MODULE_OMRON_COMMON = (1ULL << 18),
    PLCTAG_MODULE_OMRON_STANDARD_TAG = (1ULL << 19),
    PLCTAG_MODULE_OMRON_RAW_TAG = (1ULL << 20),
    PLCTAG_MODULE_MODBUS = (1ULL << 21),
    PLCTAG_MODULE_SYSTEM = (1ULL << 22),
    PLCTAG_MODULE_PLATFORM = (1ULL << 23)
} plctag_debug_module_t;


/**
 * @brief Set debug level for a specific module.
 *
 * Allows fine-grained control over debug output by setting the debug level
 * for a specific module. Module names are case-insensitive strings such as:
 * "LIB", "INIT", "VERSION", "UTILS", "AB_SESSION", "AB_PCCC", "AB_CIP", "AB_COMMON",
 * "AB_EIP_CIP", "AB_EIP_CIP_SPECIAL", "AB_EIP_LGX_PCCC", "AB_EIP_PLC5_PCCC", "AB_EIP_PLC5_DHP",
 * "AB_EIP_SLC_PCCC", "AB_EIP_SLC_DHP", "AB_ERROR", "OMRON_CONN", "OMRON_CIP", "OMRON_COMMON",
 * "OMRON_STANDARD_TAG", "OMRON_RAW_TAG", "MODBUS", "SYSTEM", and "PLATFORM".
 *
 * @param module_name Case-insensitive module name string.
 * @param debug_level Debug level value (same as plc_tag_set_debug_level()).
 * @return PLCTAG_STATUS_OK on success, PLCTAG_ERR_NOT_FOUND if module name is not recognized.
 */
LIBPLCTAG_EXPERIMENTAL
LIB_EXPORT int plc_tag_set_debug_module_level(const char *module_name, int debug_level);


/**
 * @brief Get the debug level for a specific module.
 *
 * @param module_name Case-insensitive module name string.
 * @return Current debug level for the specified module, or PLCTAG_ERR_NOT_FOUND if module name is not recognized.
 */
LIBPLCTAG_EXPERIMENTAL
LIB_EXPORT int plc_tag_get_debug_module_level(const char *module_name);


/**
 * @brief Get the current global debug level.
 *
 * @return Current global debug level set by plc_tag_set_debug_level().
 */
LIBPLCTAG_EXPERIMENTAL
LIB_EXPORT int plc_tag_get_debug_level(void);


/**
 * @brief Convert a debug module name to a module ID.
 *
 * Takes a string like "AB_SESSION" or "OMRON_CONN" and returns
 * the corresponding module ID. Module names are case-insensitive.
 *
 * @param module_name Case-insensitive module name string.
 * @return Corresponding module ID bitmask, or 0 if the module name is not recognized.
 */
LIBPLCTAG_EXPERIMENTAL
LIB_EXPORT uint64_t plc_tag_debug_module_id(const char *module_name);


/**
 * @brief Convert a debug level name to a debug level ID.
 *
 * Takes a string like "ERROR", "DEBUG_DETAIL", "SPEW", etc.
 * and returns the corresponding debug level ID. Level names are case-insensitive
 * and can be prefixed with "DEBUG_" or used without it.
 *
 * @param level_name Case-insensitive debug level name string.
 * @return Corresponding debug level ID, or -1 if the level name is not recognized.
 */
LIBPLCTAG_EXPERIMENTAL
LIB_EXPORT int plc_tag_debug_level_id(const char *level_name);


/**
 * @brief Check that the library supports the required API version.
 *
 * The version is passed as integers with the following semantics:
 * - ver_major: major version of the library (must be an exact match).
 * - ver_minor: minor version (library version must be >= requested).
 * - ver_patch: patch version (must be >= requested if minor matches; any patch is accepted if library minor > requested).
 *
 * @param req_major Required major version number.
 * @param req_minor Required minor version number.
 * @param req_patch Required patch version number.
 * @return PLCTAG_STATUS_OK if version is compatible, PLCTAG_ERR_UNSUPPORTED if not.
 *
 * @note Example: To check for version 2.1.4, call plc_tag_check_lib_version(2, 1, 4).
 */

LIB_EXPORT int plc_tag_check_lib_version(int req_major, int req_minor, int req_patch);


/**
 * @defgroup Tag_Operations Tag Operations.
 * @brief Public API for tag operations.
 *
 * These functions are implemented in a protocol-specific manner.
 * @{
 */

/**
 * @brief Create a new tag based on an attribute string.
 *
 * Creates a new tag based on the passed attribute string. The attributes
 * are protocol-specific. The only required part of the string is the key-value pair
 * "protocol=XXX" where XXX is one of the supported protocol types.
 *
 * The function will wait for the specified timeout (in milliseconds) for the tag
 * to finish the creation process. If timeout is zero, return immediately.
 * The application will need to poll the tag status with plc_tag_status() while
 * the status is PLCTAG_STATUS_PENDING until the status changes to PLCTAG_STATUS_OK
 * (if successful) or another PLCTAG_ERR_xyz error code.
 *
 * @param attrib_str Protocol-specific attribute string (must include "protocol=XXX").
 * @param timeout Milliseconds to wait for creation (0 = return immediately).
 * @return Opaque tag handle (>0 on success, <0 on error with PLCTAG_ERR_xyz code).
 */

LIB_EXPORT int32_t plc_tag_create(const char *attrib_str, int timeout);


/**
 * @brief Create a new tag with callback support.
 *
 * Extended version of plc_tag_create() with the addition of a callback function
 * and user-supplied data pointer.
 *
 * The callback will be set as early as possible in the creation process,
 * allowing early creation-time events to be sent to user code.
 *
 * @param attrib_str Protocol-specific attribute string (must include "protocol=XXX").
 * @param tag_callback_func Callback function for tag events.
 * @param userdata User-supplied data pointer passed to callback.
 * @param timeout Milliseconds to wait for creation (0 = return immediately).
 * @return Opaque tag handle (>0 on success, <0 on error with PLCTAG_ERR_xyz code).
 */

LIB_EXPORT int32_t plc_tag_create_ex(const char *attrib_str,
                                     void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                     void *userdata, int timeout);


/**
 * @brief Shut down the library and release all resources.
 *
 * Some systems may not call the atexit() handlers. In those cases, wrappers should
 * call this function before unloading the library or terminating. Most OSes cleanly
 * recover all system resources when a process terminates, so this may not be necessary.
 *
 * @warning THIS IS NOT THREAD SAFE! Do not call this if multiple threads are running
 *          against the library. Close all tags first with plc_tag_destroy() and ensure
 *          that nothing can call any library functions until this function returns.
 *
 * @note Normally you do not need to call this function. This is only for certain
 *       wrappers or operating environments that prevent normal exit handlers from working.
 */

LIB_EXPORT void plc_tag_shutdown(void);


/**
 * @brief Tag event type enumeration.
 *
 * Event types that trigger tag callbacks:
 * - PLCTAG_EVENT_READ_STARTED: Tag read operation has started.
 * - PLCTAG_EVENT_READ_COMPLETED: Tag read operation has completed.
 * - PLCTAG_EVENT_WRITE_STARTED: Tag write operation has started.
 * - PLCTAG_EVENT_WRITE_COMPLETED: Tag write operation has completed.
 * - PLCTAG_EVENT_ABORTED: Tag operation has been aborted.
 * - PLCTAG_EVENT_DESTROYED: Tag is being destroyed.
 * - PLCTAG_EVENT_CREATED: Tag has been created.
 */

typedef enum {
    PLCTAG_EVENT_READ_STARTED = 1,    /*!< Read operation started */
    PLCTAG_EVENT_READ_COMPLETED = 2,  /*!< Read operation completed */
    PLCTAG_EVENT_WRITE_STARTED = 3,   /*!< Write operation started */
    PLCTAG_EVENT_WRITE_COMPLETED = 4, /*!< Write operation completed */
    PLCTAG_EVENT_ABORTED = 5,         /*!< Operation aborted */
    PLCTAG_EVENT_DESTROYED = 6,       /*!< Tag destroyed */
    PLCTAG_EVENT_CREATED = 7,         /*!< Tag created */
    PLCTAG_EVENT_MAX = 8              /*!< Maximum event type value */
} plctag_event_t;

/**
 * @brief Register a callback function for tag events.
 *
 * Registers the passed callback function with the tag. Only one callback function
 * may be registered on a tag at a time.
 *
 * Once registered, the callback will be invoked for these events:
 * - PLCTAG_EVENT_READ_STARTED: When a tag read operation starts.
 * - PLCTAG_EVENT_READ_COMPLETED: When a tag read operation completes.
 * - PLCTAG_EVENT_WRITE_STARTED: When a tag write operation starts.
 * - PLCTAG_EVENT_WRITE_COMPLETED: When a tag write operation completes.
 * - PLCTAG_EVENT_ABORTED: When a tag operation is aborted.
 * - PLCTAG_EVENT_DESTROYED: When a tag is being destroyed.
 * - PLCTAG_EVENT_CREATED: When a tag is created.
 *
 * @warning The callback is called outside the internal tag mutex, allowing it to call
 *          tag functions safely. However, it is called in the context of the internal
 *          tag helper thread, not the client library thread(s). YOU are responsible for
 *          ensuring all client application data structures accessed by the callback are
 *          thread-safe.
 *
 * @warning Do not perform blocking operations in the callback. This will degrade
 *          library performance or cause failures.
 *
 * @warning When the callback is called with PLCTAG_EVENT_DESTROYED, do not call
 *          any tag functions as they are not guaranteed to work and may hang or fail.
 *
 * @param tag_id The tag ID handle returned by plc_tag_create().
 * @param tag_callback_func Callback function pointer.
 * @return PLCTAG_STATUS_OK on success, PLCTAG_ERR_DUPLICATE if a callback is already registered.
 */

LIB_EXPORT int plc_tag_register_callback(int32_t tag_id, void (*tag_callback_func)(int32_t tag_id, int event, int status));

/**
 * @brief Register a callback function for tag events with user data.
 *
 * Extended version of plc_tag_register_callback() that includes a user-supplied
 * data pointer passed to the callback function.
 *
 * @param tag_id The tag ID handle returned by plc_tag_create().
 * @param tag_callback_func Callback function pointer.
 * @param userdata User-supplied data pointer passed to callback.
 * @return PLCTAG_STATUS_OK on success, PLCTAG_ERR_DUPLICATE if a callback is already registered.
 *
 * @see plc_tag_register_callback()
 */

LIB_EXPORT int plc_tag_register_callback_ex(int32_t tag_id,
                                            void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                            void *userdata);


/*
 * plc_tag_unregister_callback
 *
 * This function removes the callback already registered on the tag.
 *
 * Return values:
 *
 * The function returns PLCTAG_STATUS_OK if there was a registered callback and removing it went well.
 * An error of PLCTAG_ERR_NOT_FOUND is returned if there was no registered callback.
 */

LIB_EXPORT int plc_tag_unregister_callback(int32_t tag_id);


/*
 * plc_tag_register_logger
 *
 * This function registers the passed callback function with the library.  Only one callback function
 * may be registered with the library at a time!
 *
 * Once registered, the function will be called with any logging message that is normally printed due
 * to the current log level setting.
 *
 * WARNING: the callback will usually be called when the internal tag API mutex is held.   You cannot
 * call any tag functions within the callback!
 *
 * Return values:
 *
 * If there is already a callback registered, the function will return PLCTAG_ERR_DUPLICATE.   Only one callback
 * function may be registered at a time on each tag.
 *
 * If all is successful, the function will return PLCTAG_STATUS_OK.
 */

LIB_EXPORT int plc_tag_register_logger(void (*log_callback_func)(int32_t tag_id, int debug_level, const char *message));


/*
 * plc_tag_unregister_logger
 *
 * This function removes the logger callback already registered for the library.
 *
 * Return values:
 *
 * The function returns PLCTAG_STATUS_OK if there was a registered callback and removing it went well.
 * An error of PLCTAG_ERR_NOT_FOUND is returned if there was no registered callback.
 */

LIB_EXPORT int plc_tag_unregister_logger(void);


/*
 * plc_tag_lock
 *
 * Lock the tag against use by other threads.  Because operations on a tag are
 * very much asynchronous, actions like getting and extracting the data from
 * a tag take more than one API call.  If more than one thread is using the same tag,
 * then the internal state of the tag will get broken and you will probably experience
 * a crash.
 *
 * This should be used to initially lock a tag when starting operations with it
 * followed by a call to plc_tag_unlock when you have everything you need from the tag.
 */


LIB_EXPORT int plc_tag_lock(int32_t tag);


/*
 * plc_tag_unlock
 *
 * The opposite action of plc_tag_unlock.  This allows other threads to access the
 * tag.
 */

LIB_EXPORT int plc_tag_unlock(int32_t tag);


/*
 * plc_tag_abort
 *
 * Abort any outstanding IO to the PLC.  If there is something in flight, then
 * it is marked invalid.  Note that this does not abort anything that might
 * be still processing in the report PLC.
 *
 * The status will be PLCTAG_STATUS_OK unless there is an error such as
 * a null pointer.
 *
 * This is a function provided by the underlying protocol implementation.
 */
LIB_EXPORT int plc_tag_abort(int32_t tag);


/*
 * plc_tag_destroy
 *
 * This frees all resources associated with the tag.  Internally, it may result in closed
 * connections etc.   This calls through to a protocol-specific function.
 *
 * This is a function provided by the underlying protocol implementation.
 */
LIB_EXPORT int plc_tag_destroy(int32_t tag);


/*
 * plc_tag_read
 *
 * Start a read.  If the timeout value is zero, then wait until the read
 * returns or the timeout occurs, whichever is first.  Return the status.
 * If the timeout value is zero, then plc_tag_read will normally return
 * PLCTAG_STATUS_PENDING.
 *
 * This is a function provided by the underlying protocol implementation.
 */
LIB_EXPORT int plc_tag_read(int32_t tag, int timeout);


/*
 * plc_tag_status
 *
 * Return the current status of the tag.  This will be PLCTAG_STATUS_PENDING if there is
 * an uncompleted IO operation.  It will be PLCTAG_STATUS_OK if everything is fine.  Other
 * errors will be returned as appropriate.
 *
 * This is a function provided by the underlying protocol implementation.
 */
LIB_EXPORT int plc_tag_status(int32_t tag);


/*
 * plc_tag_write
 *
 * Start a write.  If the timeout value is zero, then wait until the write
 * returns or the timeout occurs, whichever is first.  Return the status.
 * If the timeout value is zero, then plc_tag_write will usually return
 * PLCTAG_STATUS_PENDING.  The write is considered done
 * when it has been written to the socket.
 *
 * This is a function provided by the underlying protocol implementation.
 */
LIB_EXPORT int plc_tag_write(int32_t tag, int timeout);


/*
 * Tag data accessors.
 */

/**
 * @brief Connection Status Values.
 *
 * Values returned by plc_tag_get_int_attribute() when querying
 * the "connection_status" attribute on a tag.
 */
typedef enum {
    PLCTAG_CONN_STATUS_UP = 0,            /* Connected and ready for operations */
    PLCTAG_CONN_STATUS_DOWN = 1,          /* Not connected */
    PLCTAG_CONN_STATUS_DISCONNECTING = 2, /* In process of disconnecting */
    PLCTAG_CONN_STATUS_CONNECTING = 3,    /* In process of connecting */
    PLCTAG_CONN_STATUS_IDLE_WAIT = 4,     /* Waiting to reconnect after idle disconnect */
    PLCTAG_CONN_STATUS_ERR_WAIT = 5       /* Waiting to reconnect after error */
} plc_tag_conn_status_t;

/* attributes */
LIB_EXPORT int plc_tag_get_int_attribute(int32_t tag, const char *attrib_name, int default_value);
LIB_EXPORT int plc_tag_set_int_attribute(int32_t tag, const char *attrib_name, int new_value);

LIB_EXPORT int plc_tag_get_byte_array_attribute(int32_t tag, const char *attrib_name, uint8_t *buffer, int buffer_length);

LIB_EXPORT int plc_tag_get_size(int32_t tag);
/* return the old size or negative for errors. */
LIB_EXPORT int plc_tag_set_size(int32_t tag, int new_size);

LIB_EXPORT int plc_tag_get_bit(int32_t tag, int offset_bit);
LIB_EXPORT int plc_tag_set_bit(int32_t tag, int offset_bit, int val);

LIB_EXPORT uint64_t plc_tag_get_uint64(int32_t tag, int offset);
LIB_EXPORT int plc_tag_set_uint64(int32_t tag, int offset, uint64_t val);

LIB_EXPORT int64_t plc_tag_get_int64(int32_t tag, int offset);
LIB_EXPORT int plc_tag_set_int64(int32_t, int offset, int64_t val);


LIB_EXPORT uint32_t plc_tag_get_uint32(int32_t tag, int offset);
LIB_EXPORT int plc_tag_set_uint32(int32_t tag, int offset, uint32_t val);

LIB_EXPORT int32_t plc_tag_get_int32(int32_t tag, int offset);
LIB_EXPORT int plc_tag_set_int32(int32_t, int offset, int32_t val);


LIB_EXPORT uint16_t plc_tag_get_uint16(int32_t tag, int offset);
LIB_EXPORT int plc_tag_set_uint16(int32_t tag, int offset, uint16_t val);

LIB_EXPORT int16_t plc_tag_get_int16(int32_t tag, int offset);
LIB_EXPORT int plc_tag_set_int16(int32_t, int offset, int16_t val);


LIB_EXPORT uint8_t plc_tag_get_uint8(int32_t tag, int offset);
LIB_EXPORT int plc_tag_set_uint8(int32_t tag, int offset, uint8_t val);

LIB_EXPORT int8_t plc_tag_get_int8(int32_t tag, int offset);
LIB_EXPORT int plc_tag_set_int8(int32_t, int offset, int8_t val);


LIB_EXPORT double plc_tag_get_float64(int32_t tag, int offset);
LIB_EXPORT int plc_tag_set_float64(int32_t tag, int offset, double val);

LIB_EXPORT float plc_tag_get_float32(int32_t tag, int offset);
LIB_EXPORT int plc_tag_set_float32(int32_t tag, int offset, float val);

/* raw byte bulk access */
LIB_EXPORT int plc_tag_set_raw_bytes(int32_t id, int offset, uint8_t *buffer, int buffer_length);
LIB_EXPORT int plc_tag_get_raw_bytes(int32_t id, int offset, uint8_t *buffer, int buffer_length);

/* string accessors */

LIB_EXPORT int plc_tag_get_string(int32_t tag_id, int string_start_offset, char *buffer, int buffer_length);
LIB_EXPORT int plc_tag_set_string(int32_t tag_id, int string_start_offset, const char *string_val);
LIB_EXPORT int plc_tag_get_string_length(int32_t tag_id, int string_start_offset);
LIB_EXPORT int plc_tag_get_string_capacity(int32_t tag_id, int string_start_offset);
LIB_EXPORT int plc_tag_get_string_total_length(int32_t tag_id, int string_start_offset);

#ifdef __cplusplus
}
#endif

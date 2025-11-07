#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum {
    UTIL_OK = 0,
    UTIL_EABORT,        // operation aborted
    UTIL_EAGAIN,        // would block
    UTIL_EBIND,         // bind failed
    UTIL_EBOUNDS,       // out of bounds
    UTIL_EBUSY,         // resource busy
    UTIL_ECANCELLED,    // operation cancelled
    UTIL_ECLOSED,       // connection closed
    UTIL_ECONNECT,      // connect failed
    UTIL_ECONNREFUSED,  // connection refused
    UTIL_ECONNRESET,    // connection reset by peer
    UTIL_EDESTROYED,    // object destroyed
    UTIL_EHOSTUNREACH,  // host unreachable
    UTIL_EINTERNAL,     // internal error
    UTIL_EINVAL,        // invalid argument
    UTIL_EIO,           // I/O error
    UTIL_ELISTEN,       // listen failed
    UTIL_ENETUNREACH,   // network unreachable
    UTIL_ENOTCONNECT,   // socket not connected
    UTIL_ENOTFOUND,     // not found
    UTIL_ENOTSUPPORTED, // operation not supported
    UTIL_ENULL,         // null pointer
    UTIL_EREAD,         // read failed
    UTIL_ERESOLVE,      // address resolution failed
    UTIL_ERESOURCE,     // resource exhausted
    UTIL_ETIMEOUT,      // operation timed out
    UTIL_EWRITE,        // write failed
    UTIL_EARGS_MISSING_REQUIRED,  // required flag not provided
    UTIL_EARGS_UNKNOWN_FLAG,      // unknown flag provided
    UTIL_EARGS_INVALID_VALUE,     // value failed to parse
    UTIL_EARGS_INVALID_FORMAT,    // argument format invalid
    UTIL_EARGS_DUPLICATE,         // flag appears multiple times but ARGS_ONCE
    UTIL_EARGS_PARSE_ERROR,       // generic parse error
} util_err_t;

/**
 * @brief Get a string representation of the error code.
 * 
 * @param e Error code.
 * @return const char* String description of the error.
 */
const char* util_err_str(util_err_t e);

/**
 * @brief Convert a system errno value to a util_err_t.
 * 
 * @param e System errno value.
 * @return util_err_t Corresponding util_err_t value.
 */
util_err_t  util_err_from_errno(int e);
#ifdef _WIN32

/**
 * @brief Convert a Windows Sockets API error code to a util_err_t.
 * 
 * @param w Windows Sockets API error code.
 * @return util_err_t Corresponding util_err_t value.
 */
util_err_t  util_err_from_wsa(int w);
#endif

#ifdef __cplusplus
}
#endif

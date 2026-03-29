/*
 * AUTO-GENERATED FILE - DO NOT EDIT
 */

#pragma once

#include <stdint.h>

/* Debug levels - generated from plctag_debug_level_t enum */
typedef enum {
    DEBUG_NONE = 0,
    DEBUG_ERROR = 1,
    DEBUG_WARN = 2,
    DEBUG_INFO = 3,
    DEBUG_DETAIL = 4,
    DEBUG_SPEW = 5,
    DEBUG_END = 6
} debug_level_t;

/* Debug modules - generated from PLCTAG_MODULE_* defines */
typedef enum {
    DEBUG_MODULE_LIB = 0,
    DEBUG_MODULE_INIT = 1,
    DEBUG_MODULE_VERSION = 2,
    DEBUG_MODULE_UTILS = 3,
    DEBUG_MODULE_AB_SESSION = 4,
    DEBUG_MODULE_AB_PCCC = 5,
    DEBUG_MODULE_AB_CIP = 6,
    DEBUG_MODULE_AB_COMMON = 7,
    DEBUG_MODULE_AB_EIP_CIP = 8,
    DEBUG_MODULE_AB_EIP_CIP_SPECIAL = 9,
    DEBUG_MODULE_AB_EIP_LGX_PCCC = 10,
    DEBUG_MODULE_AB_EIP_PLC5_PCCC = 11,
    DEBUG_MODULE_AB_EIP_PLC5_DHP = 12,
    DEBUG_MODULE_AB_EIP_SLC_PCCC = 13,
    DEBUG_MODULE_AB_EIP_SLC_DHP = 14,
    DEBUG_MODULE_AB_ERROR = 15,
    DEBUG_MODULE_OMRON_CONN = 16,
    DEBUG_MODULE_OMRON_CIP = 17,
    DEBUG_MODULE_OMRON_COMMON = 18,
    DEBUG_MODULE_OMRON_STANDARD_TAG = 19,
    DEBUG_MODULE_OMRON_RAW_TAG = 20,
    DEBUG_MODULE_MODBUS = 21,
    DEBUG_MODULE_SYSTEM = 22,
    DEBUG_MODULE_PLATFORM = 23,
} debug_module_t;

#define DEBUG_MODULE_COUNT 24

/* Module name lookup table - indexed by debug_module_t value */
extern const char *debug_module_names[DEBUG_MODULE_COUNT];

/* Error codes - generated from plctag_error_code_t enum */
/* (Available if needed for internal error handling) */
/* typedef enum {
    STATUS_PENDING = 1,
    STATUS_OK = 0,
    ERR_ABORT = -1,
    ERR_BAD_CONFIG = -2,
    ERR_BAD_CONNECTION = -3,
    ERR_BAD_DATA = -4,
    ERR_BAD_DEVICE = -5,
    ERR_BAD_GATEWAY = -6,
    ERR_BAD_PARAM = -7,
    ERR_BAD_REPLY = -8,
    ERR_BAD_STATUS = -9,
    ERR_CLOSE = -10,
    ERR_CREATE = -11,
    ERR_DUPLICATE = -12,
    ERR_ENCODE = -13,
    ERR_MUTEX_DESTROY = -14,
    ERR_MUTEX_INIT = -15,
    ERR_MUTEX_LOCK = -16,
    ERR_MUTEX_UNLOCK = -17,
    ERR_NOT_ALLOWED = -18,
    ERR_NOT_FOUND = -19,
    ERR_NOT_IMPLEMENTED = -20,
    ERR_NO_DATA = -21,
    ERR_NO_MATCH = -22,
    ERR_NO_MEM = -23,
    ERR_NO_RESOURCES = -24,
    ERR_NULL_PTR = -25,
    ERR_OPEN = -26,
    ERR_OUT_OF_BOUNDS = -27,
    ERR_READ = -28,
    ERR_REMOTE_ERR = -29,
    ERR_THREAD_CREATE = -30,
    ERR_THREAD_JOIN = -31,
    ERR_TIMEOUT = -32,
    ERR_TOO_LARGE = -33,
    ERR_TOO_SMALL = -34,
    ERR_UNSUPPORTED = -35,
    ERR_WINSOCK = -36,
    ERR_WRITE = -37,
    ERR_PARTIAL = -38,
    ERR_BUSY = -39,
} internal_error_code_t; */

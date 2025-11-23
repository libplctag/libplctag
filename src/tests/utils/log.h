#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include "buf.h"

typedef enum {
    LOG_LEVEL_NONE = 0,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_WARN,
    LOG_LEVEL_INFO,
    LOG_LEVEL_DETAIL,
    LOG_LEVEL_SPEW,
    
    LOG_LEVEL_END
} log_level_t;

/* Log modules - generated from log_modules.def */
typedef enum {
#define LOG_MODULE_ENTRY(name, bit) LOG_MODULE_##name = (1ULL << bit),
#include "log_modules.def"
#undef LOG_MODULE_ENTRY
} log_module_t;

typedef uint64_t log_module_mask_t;

/**
 * @brief Set log level for a specific module.
 * 
 * Setting to LOG_LEVEL_NONE disables logging for that module.
 * 
 * @param module The module to configure (single bit value)
 * @param level Maximum level to log for this module
 */
void log_module_set_level(log_module_t module, log_level_t level);

/**
 * @brief Get current log level for a module.
 * 
 * @param module The module to query (single bit value)
 * @return log_level_t Current log level for this module
 */
log_level_t log_module_get_level(log_module_t module);

/**
 * @brief Set all modules to the same level.
 * 
 * @param level Log level to apply to all modules
 */
void log_set_all_modules(log_level_t level);

/**
 * @brief Check if logging is enabled for the given modules and level.
 * 
 * @param modules Bitmask of modules (any match enables logging)
 * @param level Log level to check
 * @return true if logging is enabled for at least one module at this level
 */
bool log_is_enabled(log_module_mask_t modules, log_level_t level);

/**
 * @brief Log implementation function (internal).
 * 
 * @param func name of the function in which the log is generated
 * @param line_num line number in the source file
 * @param lvl log level
 * @param modules bitmask of modules this log applies to
 * @param templ format string for the log message
 * @param ... additional arguments for the format string
 */
void log_impl(const char *func, int line_num, log_level_t lvl, 
              log_module_mask_t modules, const char *templ, ...);

/**
 * @brief Dump bytes from a buffer to the log (internal).
 * 
 * @param func name of the function in which the log is generated
 * @param line_num line number in the source file
 * @param lvl log level
 * @param modules bitmask of modules this log applies to
 * @param data buffer containing the bytes to dump
 */
void log_bytes_impl(const char *func, int line_num, log_level_t lvl, 
                    log_module_mask_t modules, buf_t *data);

/* Logging macros */

/**
 * @brief Log a message if enabled for the given modules and level.
 * 
 * @param modules Bitmask of modules this log applies to (use LOG_MODULE_* or combine with |)
 * @param level Log level (LOG_LEVEL_ERROR, LOG_LEVEL_WARN, etc.)
 * @param ... Format string and arguments
 */
#define pdlog(modules, level, ...) \
    do { if(log_is_enabled(modules, level)) \
        log_impl(__func__, __LINE__, level, modules, __VA_ARGS__); } while(0)

/**
 * @brief Log buffer contents if enabled for the given modules and level.
 * 
 * @param modules Bitmask of modules this log applies to
 * @param level Log level
 * @param buf Buffer to dump
 */
#define pdlog_bytes(modules, level, buf) \
    do { if (log_is_enabled(modules, level)) \
        log_bytes_impl(__func__, __LINE__, level, modules, (buf)); } while (0)


#ifdef __cplusplus
}
#endif

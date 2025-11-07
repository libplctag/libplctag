#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>
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

/**
 * @brief Get the current log level.
 * 
 * @return log_level_t 
 */
log_level_t log_get_level(void);

/**
 * @brief Set the current log level.
 * 
 * @param level New log level
 * @return log_level_t Previous log level
 */
log_level_t log_set_level(log_level_t level);

/**
 * @brief Log a message.
 * 
 * @param func name of the function in which the log is generated
 * @param line_num line number in the source file
 * @param lvl log level
 * @param templ format string for the log message
 * @param ... additional arguments for the format string
 */
void log_impl(const char *func, int line_num, log_level_t lvl, const char *templ, ...);

/**
 * @brief Dump bytes from a buffer to the log.
 * 
 * @param func name of the function in which the log is generated
 * @param line_num line number in the source file
 * @param lvl log level
 * @param data buffer containing the bytes to dump
 */
void log_bytes_impl(const char *func, int line_num, log_level_t lvl, buf_t *data);

/* helper macros */

#define log_error(...)   do { if((LOG_LEVEL_ERROR) <= log_get_level()) \
                            log_impl(__func__, __LINE__, LOG_LEVEL_ERROR, __VA_ARGS__); } while(0)
#define log_warn(...)    do { if((LOG_LEVEL_WARN)  <= log_get_level()) \
                            log_impl(__func__, __LINE__, LOG_LEVEL_WARN,  __VA_ARGS__); } while(0)
#define log_info(...)    do { if((LOG_LEVEL_INFO)  <= log_get_level()) \
                            log_impl(__func__, __LINE__, LOG_LEVEL_INFO,  __VA_ARGS__); } while(0)
#define log_detail(...)  do { if((LOG_LEVEL_DETAIL)<= log_get_level()) \
                            log_impl(__func__, __LINE__, LOG_LEVEL_DETAIL,__VA_ARGS__); } while(0)
#define log_spew(...)    do { if((LOG_LEVEL_SPEW)  <= log_get_level()) \
                            log_impl(__func__, __LINE__, LOG_LEVEL_SPEW,  __VA_ARGS__); } while(0)

#define log_bytes_error(buf)  \
    do { if ((LOG_LEVEL_ERROR) <= log_get_level()) \
        log_bytes_impl(__func__, __LINE__, LOG_LEVEL_ERROR, (buf)); } while (0)
#define log_bytes_warn(buf)   \
    do { if ((LOG_LEVEL_WARN) <= log_get_level()) \
        log_bytes_impl(__func__, __LINE__, LOG_LEVEL_WARN, (buf)); } while (0)
#define log_bytes_info(buf)   \
    do { if ((LOG_LEVEL_INFO) <= log_get_level()) \
        log_bytes_impl(__func__, __LINE__, LOG_LEVEL_INFO, (buf)); } while (0)
#define log_bytes_detail(buf) \
    do { if ((LOG_LEVEL_DETAIL) <= log_get_level()) \
        log_bytes_impl(__func__, __LINE__, LOG_LEVEL_DETAIL, (buf)); } while (0)
#define log_bytes_spew(buf)   \
    do { if ((LOG_LEVEL_SPEW) <= log_get_level()) \
        log_bytes_impl(__func__, __LINE__, LOG_LEVEL_SPEW, (buf)); } while (0)


#ifdef __cplusplus
}
#endif

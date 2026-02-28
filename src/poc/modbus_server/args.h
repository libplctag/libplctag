#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "err.h"

/**
 * @brief Command-line argument parsing module.
 *
 * Provides declarative, table-driven parsing of command-line arguments.
 * Supports typed flags (strings, integers, floats, booleans), required/optional
 * flags, and repeated flags with iteration support.
 *
 * Features:
 * - Name-based accessor functions (no index tracking)
 * - Debug names for structured logging
 * - Type validation with error reporting
 * - Zero-copy string values
 * - Stack-allocatable result structure
 *
 * Example:
 *   args_flag_def_t flags[] = {
 *       { "host", ARGS_TYPE_STRING, ARGS_REQUIRED, ARGS_ONCE,
 *         "server.host", "Server hostname" },
 *       { "port", ARGS_TYPE_INT, ARGS_REQUIRED, ARGS_ONCE,
 *         "server.port", "Server port" },
 *       { "verbose", ARGS_TYPE_BOOL, ARGS_OPTIONAL, ARGS_ONCE,
 *         "logging.verbose", "Verbose output" },
 *   };
 *
 *   args_result_t result;
 *   util_err_t err = args_parse(argc, argv, flags, 3, &result);
 *   if (err != UTIL_OK) {
 *       log_error("Parse failed: %s", args_get_error_debug_name(&result));
 *       args_free(&result);
 *       return 1;
 *   }
 *
 *   const char *host = args_get_string(&result, "host");
 *   int64_t port = args_get_int(&result, "port");
 *   args_free(&result);
 */

/* ================================================================
 * Constants
 * ================================================================ */

#define ARGS_MAX_FLAGS 64
#define ARGS_MAX_REPETITIONS 256

/* ================================================================
 * Type Definitions
 * ================================================================ */

/**
 * Type of value a flag accepts
 */
typedef enum {
    ARGS_TYPE_STRING,      // --flag=value (string)
    ARGS_TYPE_INT,         // --flag=123 (integer, base 10)
    ARGS_TYPE_INT_HEX,     // --flag=0xFF (integer, base 16)
    ARGS_TYPE_BOOL,        // --flag=true|false|yes|no|on|off|1|0
    ARGS_TYPE_FLOAT,       // --flag=3.14 (floating point)
} args_type_t;

/**
 * Whether a flag is required or optional
 */
typedef enum {
    ARGS_REQUIRED = 0,     // Flag must be present
    ARGS_OPTIONAL = 1,     // Flag is optional
} args_required_t;

/**
 * Whether a flag can appear multiple times
 */
typedef enum {
    ARGS_ONCE = 0,         // Flag appears at most once
    ARGS_MULTIPLE = 1,     // Flag can appear multiple times
} args_repetition_t;

/**
 * Single parsed value with type and presence indicator
 */
typedef struct {
    args_type_t type;
    union {
        const char *string_val;
        int64_t int_val;
        double float_val;
        bool bool_val;
    } value;
    bool present;
} args_value_t;

/**
 * Array of values for a repeated flag
 */
typedef struct {
    args_value_t *values;
    size_t count;
} args_repeated_t;

/**
 * Default value for a flag (union of all types)
 * Used when flag is not provided on command line
 */
typedef struct {
    bool has_default;       // Whether a default is defined
    union {
        const char *string_val;
        int64_t int_val;
        double float_val;
        bool bool_val;
    } value;
} args_default_t;

/**
 * Flag definition for declarative configuration
 *
 * name: CLI flag name (e.g., "host" for --host=value)
 * type: Type of value expected
 * required: Whether flag must be present (ignored if has default)
 * repeat: Whether flag can appear multiple times
 * debug_name: Qualified name for logging (e.g., "server.host")
 * description: Help text
 * default_value: Default value if flag not provided (optional)
 *
 * Example with defaults:
 *   { "port", ARGS_TYPE_INT, ARGS_REQUIRED, ARGS_ONCE,
 *     "server.port", "Server port",
 *     { .has_default = true, .value.int_val = 8080 } }
 *
 *   { "verbose", ARGS_TYPE_BOOL, ARGS_OPTIONAL, ARGS_ONCE,
 *     "logging.verbose", "Verbose output",
 *     { .has_default = true, .value.bool_val = false } }
 *
 *   { "name", ARGS_TYPE_STRING, ARGS_REQUIRED, ARGS_ONCE,
 *     "app.name", "Application name",
 *     { .has_default = false } }  // No default
 */
typedef struct {
    const char *name;
    args_type_t type;
    args_required_t required;
    args_repetition_t repeat;
    const char *debug_name;
    const char *description;
    args_default_t default_value;
} args_flag_def_t;

/**
 * Complete result of argument parsing
 * Internal structure - access via accessor functions
 */
typedef struct {
    args_value_t *values;
    args_repeated_t *repeat_groups;
    const args_flag_def_t *flags;
    size_t flags_count;
    util_err_t error;
    const char *error_detail;
    const char *error_debug_name;
    int error_flag_index;
} args_result_t;

/* ================================================================
 * Parsing
 * ================================================================ */

/**
 * Parse command-line arguments according to flag definitions.
 *
 * @param argc Number of arguments
 * @param argv Array of argument strings
 * @param flags Array of flag definitions
 * @param flags_count Number of flag definitions
 * @param result OUT: Parsed result
 * @return UTIL_OK on success, error code on failure
 */
util_err_t args_parse(int argc, const char *argv[],
                      const args_flag_def_t *flags, size_t flags_count,
                      args_result_t *result);

/* ================================================================
 * Value Access (by Flag Name)
 * ================================================================ */

/**
 * Get a flag value by name.
 *
 * @return args_value_t with presence indicator
 */
args_value_t args_get_value(const args_result_t *result, const char *flag_name);

/**
 * Get values for a repeated flag by name.
 */
args_repeated_t args_get_repeated(const args_result_t *result, const char *flag_name);

/**
 * Get string value by flag name.
 * @return NULL if not present or wrong type
 */
const char* args_get_string(const args_result_t *result, const char *flag_name);

/**
 * Get integer value by flag name.
 * @return Value if present, INT64_MIN on error, 0 if not present
 */
int64_t args_get_int(const args_result_t *result, const char *flag_name);

/**
 * Get boolean value by flag name.
 * @return true if flag present, false otherwise
 */
bool args_get_bool(const args_result_t *result, const char *flag_name);

/**
 * Get float value by flag name.
 * @return Value if present, NaN if not present or error
 */
double args_get_float(const args_result_t *result, const char *flag_name);

/* ================================================================
 * Iteration (for ARGS_MULTIPLE flags)
 * ================================================================ */

/**
 * Get count of how many times a repeated flag appeared.
 * @return 0 if not present, > 0 for repeated
 */
size_t args_get_count(const args_result_t *result, const char *flag_name);

/**
 * Get value at index in a repeated flag's values.
 * @return args_value_t with presence indicator
 */
args_value_t args_get_at(const args_result_t *result, const char *flag_name, size_t index);

/* ================================================================
 * Error Handling & Introspection
 * ================================================================ */

/**
 * Check if parsing succeeded
 */
bool args_is_ok(const args_result_t *result);

/**
 * Get error status
 */
util_err_t args_get_error(const args_result_t *result);

/**
 * Get human-readable error message
 */
const char* args_get_error_detail(const args_result_t *result);

/**
 * Get debug name of flag that caused error
 */
const char* args_get_error_debug_name(const args_result_t *result);

/**
 * Get index of flag that caused error (-1 for general errors)
 */
int args_get_error_flag_index(const args_result_t *result);

/**
 * Free resources associated with parsed result
 */
void args_free(args_result_t *result);

/* ================================================================
 * Introspection
 * ================================================================ */

/**
 * Look up flag definition by flag name
 * @return Pointer to flag definition, or NULL if not found
 */
const args_flag_def_t* args_get_flag_def(const args_flag_def_t *flags,
                                         size_t flags_count,
                                         const char *flag_name);

/**
 * Get debug name from a flag definition
 */
const char* args_get_debug_name(const args_flag_def_t *flag_def);

/**
 * Print usage/help information with program name and all flags
 */
void args_print_help(const char *program_name,
                     const args_flag_def_t *flags, size_t flags_count);

/**
 * Print all available flags with their metadata for usage functions.
 * Displays each flag on a separate line with:
 * - Flag name
 * - Required/Optional status
 * - Single/Multiple values status
 * - Description
 *
 * This is useful for building custom usage() functions.
 *
 * @param flags Array of flag definitions
 * @param flags_count Number of flag definitions
 */
void args_print_flags(const args_flag_def_t *flags, size_t flags_count);

#ifdef __cplusplus
}
#endif

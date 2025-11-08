#include "args.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>

/* Cross-platform compatibility for strcasecmp */
#ifdef _WIN32
  #define strcasecmp _stricmp
#endif

/* ================================================================
 * Internal Structures
 * ================================================================ */

typedef struct {
    size_t flag_index;      // Index into original flags array
    args_value_t *values;   // Allocated array for values
    size_t count;           // Number of values
    size_t capacity;        // Allocated capacity
} internal_flag_t;

/* ================================================================
 * Parsing Helpers
 * ================================================================ */

/**
 * Find flag definition by name
 */
static int find_flag_index(const args_flag_def_t *flags, size_t flags_count,
                           const char *flag_name) {
    if (!flag_name) {
        return -1;
    }

    for (size_t i = 0; i < flags_count; i++) {
        if (strcmp(flags[i].name, flag_name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * Parse integer value (base 10 or 16)
 */
static bool parse_int(const char *str, int64_t *out, bool allow_hex) {
    if (!str || !out) {
        return false;
    }

    errno = 0;
    char *endptr = NULL;
    int base = 10;

    // Check for hex prefix
    if (allow_hex && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        base = 16;
    }

    int64_t val = strtoll(str, &endptr, base);

    // Check for errors
    if (errno != 0 || endptr == str || *endptr != '\0') {
        return false;
    }

    *out = val;
    return true;
}

/**
 * Parse floating point value
 */
static bool parse_float(const char *str, double *out) {
    if (!str || !out) {
        return false;
    }

    errno = 0;
    char *endptr = NULL;
    double val = strtod(str, &endptr);

    // Check for errors
    if (errno != 0 || endptr == str || *endptr != '\0') {
        return false;
    }

    *out = val;
    return true;
}

/**
 * Parse a single argument value according to its type
 */
static bool parse_value(args_value_t *val, const char *str,
                        const args_flag_def_t *flag_def) {
    if (!val || !flag_def) {
        return false;
    }

    val->type = flag_def->type;
    val->present = true;

    switch (flag_def->type) {
        case ARGS_TYPE_STRING:
            val->value.string_val = str;
            return true;

        case ARGS_TYPE_INT:
            if (!parse_int(str, &val->value.int_val, false)) {
                return false;
            }
            return true;

        case ARGS_TYPE_INT_HEX:
            if (!parse_int(str, &val->value.int_val, true)) {
                return false;
            }
            return true;

        case ARGS_TYPE_BOOL: {
            // For bool, we require a value: true|false|yes|no|on|off|1|0
            if (!str || strlen(str) == 0) {
                return false;  // Bool now requires explicit value
            }

            // True variants
            if (strcasecmp(str, "true") == 0 ||
                strcasecmp(str, "1") == 0 ||
                strcasecmp(str, "yes") == 0 ||
                strcasecmp(str, "on") == 0) {
                val->value.bool_val = true;
                return true;
            }

            // False variants
            if (strcasecmp(str, "false") == 0 ||
                strcasecmp(str, "0") == 0 ||
                strcasecmp(str, "no") == 0 ||
                strcasecmp(str, "off") == 0) {
                val->value.bool_val = false;
                return true;
            }

            return false;
        }

        case ARGS_TYPE_FLOAT:
            if (!parse_float(str, &val->value.float_val)) {
                return false;
            }
            return true;

        default:
            return false;
    }
}

/* ================================================================
 * Main Parsing Implementation
 * ================================================================ */

util_err_t args_parse(int argc, const char *argv[],
                      const args_flag_def_t *flags, size_t flags_count,
                      args_result_t *result) {
    log_info("args_parse: entry (argc=%d, flags_count=%zu)", argc, flags_count);

    if (!argv || !flags || !result) {
        log_error("args_parse: NULL argument");
        return UTIL_EINVAL;
    }

    if (flags_count == 0 || flags_count > ARGS_MAX_FLAGS) {
        log_error("args_parse: invalid flags_count %zu", flags_count);
        return UTIL_EINVAL;
    }

    // Initialize result
    memset(result, 0, sizeof(*result));
    result->flags = flags;
    result->flags_count = flags_count;

    // Allocate arrays
    result->values = calloc(flags_count, sizeof(args_value_t));
    result->repeat_groups = calloc(flags_count, sizeof(args_repeated_t));

    if (!result->values || !result->repeat_groups) {
        log_error("args_parse: allocation failed");
        args_free(result);
        result->error = UTIL_ERESOURCE;
        return UTIL_ERESOURCE;
    }

    // Initialize repeat group capacities
    for (size_t i = 0; i < flags_count; i++) {
        if (flags[i].repeat == ARGS_MULTIPLE) {
            result->repeat_groups[i].values = malloc(
                sizeof(args_value_t) * ARGS_MAX_REPETITIONS);
            if (!result->repeat_groups[i].values) {
                log_error("args_parse: repeat group allocation failed");
                args_free(result);
                result->error = UTIL_ERESOURCE;
                return UTIL_ERESOURCE;
            }
        }
    }

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        log_detail("args_parse: processing arg[%d]=%s", i, arg);

        // Must start with --
        if (arg[0] != '-' || arg[1] != '-') {
            log_error("args_parse: invalid format '%s'", arg);
            result->error = UTIL_EARGS_INVALID_FORMAT;
            result->error_detail = "arguments must start with '--'";
            result->error_flag_index = -1;
            return UTIL_EARGS_INVALID_FORMAT;
        }

        // Find the = separator
        const char *name_start = arg + 2;
        const char *eq = strchr(name_start, '=');

        char flag_name[256];
        const char *flag_value = NULL;

        if (eq) {
            // --flag=value
            size_t name_len = (size_t)(ptrdiff_t)(eq - name_start);
            if (name_len >= sizeof(flag_name)) {
                log_error("args_parse: flag name too long");
                result->error = UTIL_EARGS_INVALID_FORMAT;
                result->error_detail = "flag name too long";
                return UTIL_EARGS_INVALID_FORMAT;
            }
            strncpy(flag_name, name_start, name_len);
            flag_name[name_len] = '\0';
            flag_value = eq + 1;
        } else {
            // --flag (no value)
            if (strlen(name_start) >= sizeof(flag_name)) {
                log_error("args_parse: flag name too long");
                result->error = UTIL_EARGS_INVALID_FORMAT;
                result->error_detail = "flag name too long";
                return UTIL_EARGS_INVALID_FORMAT;
            }
            strcpy(flag_name, name_start);
            flag_value = NULL;
        }

        log_detail("args_parse: flag_name=%s value=%s", flag_name, flag_value ? flag_value : "(none)");

        // Find flag definition
        int flag_index = find_flag_index(flags, flags_count, flag_name);
        if (flag_index < 0) {
            log_error("args_parse: unknown flag '%s'", flag_name);
            result->error = UTIL_EARGS_UNKNOWN_FLAG;
            result->error_detail = "flag not recognized";
            result->error_debug_name = flag_name;
            result->error_flag_index = -1;
            return UTIL_EARGS_UNKNOWN_FLAG;
        }

        const args_flag_def_t *flag_def = &flags[flag_index];
        log_detail("args_parse: found flag at index %d, debug_name=%s",
                   flag_index, flag_def->debug_name);

        // Check for duplicate if ARGS_ONCE
        if (flag_def->repeat == ARGS_ONCE && result->values[flag_index].present) {
            log_error("args_parse: flag '%s' appears multiple times but is ARGS_ONCE",
                      flag_name);
            result->error = UTIL_EARGS_DUPLICATE;
            result->error_detail = "flag cannot appear multiple times";
            result->error_debug_name = flag_def->debug_name;
            result->error_flag_index = flag_index;
            return UTIL_EARGS_DUPLICATE;
        }

        // Parse value
        args_value_t val = {0};
        if (!parse_value(&val, flag_value, flag_def)) {
            log_error("args_parse: failed to parse value '%s' for flag '%s'",
                      flag_value ? flag_value : "(none)", flag_name);
            result->error = UTIL_EARGS_INVALID_VALUE;
            result->error_detail = "value failed to parse for this type";
            result->error_debug_name = flag_def->debug_name;
            result->error_flag_index = flag_index;
            return UTIL_EARGS_INVALID_VALUE;
        }

        log_detail("args_parse: parsed value successfully for %s", flag_def->debug_name);

        // Store value
        if (flag_def->repeat == ARGS_ONCE) {
            result->values[flag_index] = val;
        } else {
            // ARGS_MULTIPLE: append to repeat group
            args_repeated_t *repeat = &result->repeat_groups[flag_index];
            if (repeat->count >= ARGS_MAX_REPETITIONS) {
                log_error("args_parse: too many repetitions for flag '%s'", flag_name);
                result->error = UTIL_ERESOURCE;
                result->error_detail = "too many values for repeated flag";
                result->error_debug_name = flag_def->debug_name;
                result->error_flag_index = flag_index;
                return UTIL_ERESOURCE;
            }
            repeat->values[repeat->count++] = val;
        }
    }

    // Check for missing required flags and apply defaults
    for (size_t i = 0; i < flags_count; i++) {
        if (flags[i].repeat == ARGS_ONCE) {
            // Check if flag is present on command line
            if (!result->values[i].present) {
                // Flag not provided - check for default or required
                if (flags[i].default_value.has_default) {
                    log_detail("args_parse: applying default for flag '%s'", flags[i].name);
                    result->values[i].type = flags[i].type;
                    memcpy(&result->values[i].value, &flags[i].default_value.value,
                           sizeof(result->values[i].value));
                    result->values[i].present = true;
                } else if (flags[i].required == ARGS_REQUIRED) {
                    log_error("args_parse: required flag '%s' not provided", flags[i].name);
                    result->error = UTIL_EARGS_MISSING_REQUIRED;
                    result->error_detail = "required flag not provided";
                    result->error_debug_name = flags[i].debug_name;
                    result->error_flag_index = (int)i;
                    return UTIL_EARGS_MISSING_REQUIRED;
                }
            }
        } else {
            // ARGS_MULTIPLE flag - check for missing required
            if (result->repeat_groups[i].count == 0) {
                if (flags[i].required == ARGS_REQUIRED && !flags[i].default_value.has_default) {
                    log_error("args_parse: required flag '%s' not provided", flags[i].name);
                    result->error = UTIL_EARGS_MISSING_REQUIRED;
                    result->error_detail = "required flag not provided";
                    result->error_debug_name = flags[i].debug_name;
                    result->error_flag_index = (int)i;
                    return UTIL_EARGS_MISSING_REQUIRED;
                }
                // Note: defaults are not applied to ARGS_MULTIPLE flags
            }
        }
    }

    log_info("args_parse: exit (success)");
    return UTIL_OK;
}

/* ================================================================
 * Value Access
 * ================================================================ */

static int get_flag_index_by_name(const args_result_t *result, const char *flag_name) {
    if (!result || !flag_name) {
        return -1;
    }
    return find_flag_index(result->flags, result->flags_count, flag_name);
}

args_value_t args_get_value(const args_result_t *result, const char *flag_name) {
    args_value_t empty = {0};

    if (!result || !flag_name) {
        return empty;
    }

    int idx = get_flag_index_by_name(result, flag_name);
    if (idx < 0) {
        return empty;
    }

    return result->values[idx];
}

args_repeated_t args_get_repeated(const args_result_t *result, const char *flag_name) {
    args_repeated_t empty = {0};

    if (!result || !flag_name) {
        return empty;
    }

    int idx = get_flag_index_by_name(result, flag_name);
    if (idx < 0) {
        return empty;
    }

    return result->repeat_groups[idx];
}

const char* args_get_string(const args_result_t *result, const char *flag_name) {
    args_value_t val = args_get_value(result, flag_name);
    if (!val.present || val.type != ARGS_TYPE_STRING) {
        return NULL;
    }
    return val.value.string_val;
}

int64_t args_get_int(const args_result_t *result, const char *flag_name) {
    args_value_t val = args_get_value(result, flag_name);
    if (!val.present) {
        return 0;
    }
    if (val.type != ARGS_TYPE_INT && val.type != ARGS_TYPE_INT_HEX) {
        return INT64_MIN;
    }
    return val.value.int_val;
}

bool args_get_bool(const args_result_t *result, const char *flag_name) {
    args_value_t val = args_get_value(result, flag_name);
    if (!val.present || val.type != ARGS_TYPE_BOOL) {
        return false;
    }
    return val.value.bool_val;
}

double args_get_float(const args_result_t *result, const char *flag_name) {
    args_value_t val = args_get_value(result, flag_name);
    if (!val.present || val.type != ARGS_TYPE_FLOAT) {
        return NAN;
    }
    return val.value.float_val;
}

/* ================================================================
 * Iteration
 * ================================================================ */

size_t args_get_count(const args_result_t *result, const char *flag_name) {
    args_repeated_t repeated = args_get_repeated(result, flag_name);
    return repeated.count;
}

args_value_t args_get_at(const args_result_t *result, const char *flag_name, size_t index) {
    args_value_t empty = {0};
    args_repeated_t repeated = args_get_repeated(result, flag_name);

    if (index >= repeated.count) {
        return empty;
    }

    return repeated.values[index];
}

/* ================================================================
 * Error Handling
 * ================================================================ */

bool args_is_ok(const args_result_t *result) {
    if (!result) {
        return false;
    }
    return result->error == UTIL_OK;
}

util_err_t args_get_error(const args_result_t *result) {
    if (!result) {
        return UTIL_EINVAL;
    }
    return result->error;
}

const char* args_get_error_detail(const args_result_t *result) {
    if (!result || !result->error_detail) {
        return "unknown error";
    }
    return result->error_detail;
}

const char* args_get_error_debug_name(const args_result_t *result) {
    if (!result || !result->error_debug_name) {
        return NULL;
    }
    return result->error_debug_name;
}

int args_get_error_flag_index(const args_result_t *result) {
    if (!result) {
        return -1;
    }
    return result->error_flag_index;
}

void args_free(args_result_t *result) {
    if (!result) {
        return;
    }

    if (result->values) {
        free(result->values);
        result->values = NULL;
    }

    if (result->repeat_groups) {
        for (size_t i = 0; i < result->flags_count; i++) {
            if (result->repeat_groups[i].values) {
                free(result->repeat_groups[i].values);
            }
        }
        free(result->repeat_groups);
        result->repeat_groups = NULL;
    }
}

/* ================================================================
 * Introspection
 * ================================================================ */

const args_flag_def_t* args_get_flag_def(const args_flag_def_t *flags,
                                         size_t flags_count,
                                         const char *flag_name) {
    if (!flags || !flag_name) {
        return NULL;
    }

    int idx = find_flag_index(flags, flags_count, flag_name);
    if (idx < 0) {
        return NULL;
    }

    return &flags[idx];
}

const char* args_get_debug_name(const args_flag_def_t *flag_def) {
    if (!flag_def) {
        return NULL;
    }
    return flag_def->debug_name;
}

void args_print_help(const char *program_name,
                     const args_flag_def_t *flags, size_t flags_count) {
    if (!program_name || !flags) {
        return;
    }

    printf("Usage: %s [OPTIONS]\n\n", program_name);
    printf("Options:\n");

    for (size_t i = 0; i < flags_count; i++) {
        const args_flag_def_t *flag = &flags[i];
        const char *req = (flag->required == ARGS_REQUIRED) ? " (required)" : "";
        const char *rep = (flag->repeat == ARGS_MULTIPLE) ? " (repeatable)" : "";
        const char *type_str = "";

        switch (flag->type) {
            case ARGS_TYPE_STRING:      type_str = " VALUE"; break;
            case ARGS_TYPE_INT:         type_str = " INT"; break;
            case ARGS_TYPE_INT_HEX:     type_str = " HEX"; break;
            case ARGS_TYPE_FLOAT:       type_str = " FLOAT"; break;
            case ARGS_TYPE_BOOL:        type_str = ""; break;
        }

        printf("  --%s%s %-20s [%s]%s%s\n",
               flag->name, type_str, flag->description ? flag->description : "",
               flag->debug_name, req, rep);
    }
}

void args_print_flags(const args_flag_def_t *flags, size_t flags_count) {
    if (!flags || flags_count == 0) {
        return;
    }

    log_info("args_print_flags: enter");

    for (size_t i = 0; i < flags_count; i++) {
        const args_flag_def_t *flag = &flags[i];
        const char *required_str = (flag->required == ARGS_REQUIRED) ? "required" : "optional";
        const char *multiple_str = (flag->repeat == ARGS_MULTIPLE) ? "multiple" : "single";
        const char *description = flag->description ? flag->description : "";

        printf("  %-20s %-10s %-10s %s\n",
               flag->name, required_str, multiple_str, description);
    }

    log_info("args_print_flags: exit");
}

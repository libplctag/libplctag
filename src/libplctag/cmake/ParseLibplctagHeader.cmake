# ParseLibplctagHeader.cmake
# Parses libplctag.h to extract enum definitions and generate internal headers
#
# Usage:
#   include("path/to/ParseLibplctagHeader.cmake")
#   parse_libplctag_header(INPUT_HEADER OUTPUT_HEADER OUTPUT_NAMES_C)
#
# This creates:
#   - add_custom_command that regenerates outputs when INPUT_HEADER changes
#   - Sets OUTPUT_HEADER and OUTPUT_NAMES_C in parent scope for use in target sources
#   - Creates 'generate_debug_headers' target for targets that only need the header
#
# For targets that include OUTPUT_NAMES_C in their sources, CMake automatically
# tracks the dependency. For targets that only #include the header, use:
#   add_dependencies(my_target generate_debug_headers)

# Save the script path at include time (CMAKE_CURRENT_LIST_DIR changes inside functions)
set(_PARSE_LIBPLCTAG_SCRIPT "${CMAKE_CURRENT_LIST_FILE}")

function(parse_libplctag_header INPUT_HEADER OUTPUT_HEADER OUTPUT_NAMES_C)
    # Custom command regenerates outputs when INPUT_HEADER changes
    add_custom_command(
        OUTPUT "${OUTPUT_HEADER}" "${OUTPUT_NAMES_C}"
        COMMAND ${CMAKE_COMMAND}
            -DINPUT_HEADER=${INPUT_HEADER}
            -DOUTPUT_HEADER=${OUTPUT_HEADER}
            -DOUTPUT_NAMES_C=${OUTPUT_NAMES_C}
            -P ${_PARSE_LIBPLCTAG_SCRIPT}
        DEPENDS "${INPUT_HEADER}"
        COMMENT "Generating debug headers from libplctag.h"
        VERBATIM
    )

    # Export paths to parent scope so they can be added to target sources
    set(DEBUG_GENERATED_HEADER "${OUTPUT_HEADER}" PARENT_SCOPE)
    set(DEBUG_GENERATED_NAMES_C "${OUTPUT_NAMES_C}" PARENT_SCOPE)

    # Custom target for targets that only need the header (not the .c file)
    # Targets that include the .c file in their sources don't need this
    add_custom_target(generate_debug_headers
        DEPENDS "${OUTPUT_HEADER}" "${OUTPUT_NAMES_C}"
    )
endfunction()

# Internal function used by the script mode (called via -P)
function(parse_libplctag_header_impl INPUT_HEADER OUTPUT_HEADER OUTPUT_NAMES_C)
    message(STATUS "Parsing ${INPUT_HEADER} to generate ${OUTPUT_HEADER}")

    # Read the entire header file
    file(READ "${INPUT_HEADER}" HEADER_CONTENT)
    
    # Extract error codes enum
    string(REGEX MATCH "typedef enum \\{[^}]*PLCTAG_ERR_BUSY[^}]*\\} plctag_error_code_t" ERROR_ENUM "${HEADER_CONTENT}")
    
    # Extract debug levels enum
    string(REGEX MATCH "typedef enum \\{[^}]*PLCTAG_DEBUG_SPEW[^}]*\\} plctag_debug_level_t" DEBUG_ENUM "${HEADER_CONTENT}")
    
    # Parse error codes
    set(ERROR_CODE_ENTRIES "")
    string(REGEX MATCHALL "PLCTAG_[A-Z_]+ = -?[0-9]+" ERROR_MATCHES "${ERROR_ENUM}")
    foreach(MATCH ${ERROR_MATCHES})
        string(REGEX REPLACE "PLCTAG_" "" MATCH_STRIPPED "${MATCH}")
        set(ERROR_CODE_ENTRIES "${ERROR_CODE_ENTRIES}    ${MATCH_STRIPPED},\n")
    endforeach()

    # Parse debug levels
    set(DEBUG_LEVEL_ENTRIES "")
    string(REGEX MATCHALL "PLCTAG_DEBUG_[A-Z_]+ = [0-9]+" DEBUG_MATCHES "${DEBUG_ENUM}")
    foreach(MATCH ${DEBUG_MATCHES})
        string(REGEX REPLACE "PLCTAG_DEBUG_" "DEBUG_" MATCH_CONVERTED "${MATCH}")
        set(DEBUG_LEVEL_ENTRIES "${DEBUG_LEVEL_ENTRIES}    ${MATCH_CONVERTED},\n")
    endforeach()

    # Parse debug modules from header and create internal representation
    # Note: Extract directly from HEADER_CONTENT because CMake regex doesn't match newlines across groups
    # Pattern accounts for alignment spacing between name and = sign
    set(MODULE_ENUM_ENTRIES "")
    set(MODULE_NAME_TABLE "")
    set(MODULE_COUNT 0)
    string(REGEX MATCHALL "PLCTAG_MODULE_[A-Z_0-9]+[ \t]*=[ \t]*\\(1ULL << [0-9]+\\)" MODULE_MATCHES "${HEADER_CONTENT}")
    foreach(MATCH ${MODULE_MATCHES})
        # Extract module name
        string(REGEX MATCH "PLCTAG_MODULE_([A-Z_0-9]+)" MODULE_NAME_MATCH "${MATCH}")
        set(MODULE_NAME "${CMAKE_MATCH_1}")

        # Extract bit position
        string(REGEX MATCH "1ULL << ([0-9]+)" BIT_MATCH "${MATCH}")
        set(BIT_POS "${CMAKE_MATCH_1}")

        if(MODULE_NAME AND DEFINED BIT_POS)
            set(MODULE_ENUM_ENTRIES "${MODULE_ENUM_ENTRIES}    DEBUG_MODULE_${MODULE_NAME} = (1ULL << ${BIT_POS}),\n")
            set(MODULE_NAME_TABLE "${MODULE_NAME_TABLE}    [${BIT_POS}] = \"${MODULE_NAME}\",\n")
            math(EXPR MODULE_COUNT "${MODULE_COUNT} + 1")
        endif()
    endforeach()
    
    # Generate the output header file
    set(GENERATED_HEADER "/*\n")
    set(GENERATED_HEADER "${GENERATED_HEADER} * AUTO-GENERATED FILE - DO NOT EDIT\n")
    set(GENERATED_HEADER "${GENERATED_HEADER} */\n\n")
    set(GENERATED_HEADER "${GENERATED_HEADER}#pragma once\n\n")
    set(GENERATED_HEADER "${GENERATED_HEADER}#include <stdint.h>\n\n")
    
    # Debug levels
    set(GENERATED_HEADER "${GENERATED_HEADER}/* Debug levels - generated from plctag_debug_level_t enum */\n")
    set(GENERATED_HEADER "${GENERATED_HEADER}typedef enum {\n")
    set(GENERATED_HEADER "${GENERATED_HEADER}${DEBUG_LEVEL_ENTRIES}")
    set(GENERATED_HEADER "${GENERATED_HEADER}    DEBUG_END = 6\n")
    set(GENERATED_HEADER "${GENERATED_HEADER}} debug_level_t;\n\n")
    
    # Debug modules
    set(GENERATED_HEADER "${GENERATED_HEADER}/* Debug modules - generated from PLCTAG_MODULE_* defines */\n")
    set(GENERATED_HEADER "${GENERATED_HEADER}typedef enum {\n")
    set(GENERATED_HEADER "${GENERATED_HEADER}${MODULE_ENUM_ENTRIES}")
    set(GENERATED_HEADER "${GENERATED_HEADER}} debug_module_t;\n\n")
    set(GENERATED_HEADER "${GENERATED_HEADER}typedef uint64_t debug_module_mask_t;\n\n")
    
    # Module name table - declaration (in header)
    set(GENERATED_HEADER "${GENERATED_HEADER}/* Module name lookup table */\n")
    set(GENERATED_HEADER "${GENERATED_HEADER}extern const char *debug_module_names[];\n\n")
    set(GENERATED_HEADER "${GENERATED_HEADER}#define DEBUG_MODULE_COUNT ${MODULE_COUNT}\n\n")

    # Error codes (for potential use)
    set(GENERATED_HEADER "${GENERATED_HEADER}/* Error codes - generated from plctag_error_code_t enum */\n")
    set(GENERATED_HEADER "${GENERATED_HEADER}/* (Available if needed for internal error handling) */\n")
    set(GENERATED_HEADER "${GENERATED_HEADER}/* typedef enum {\n")
    set(GENERATED_HEADER "${GENERATED_HEADER}${ERROR_CODE_ENTRIES}")
    set(GENERATED_HEADER "${GENERATED_HEADER}} internal_error_code_t; */\n")

    # Write the header file
    file(WRITE "${OUTPUT_HEADER}" "${GENERATED_HEADER}")

    # Generate the .c file with the actual array definition
    set(GENERATED_C "/*\n")
    set(GENERATED_C "${GENERATED_C} * AUTO-GENERATED FILE - DO NOT EDIT\n")
    set(GENERATED_C "${GENERATED_C} */\n\n")
    set(GENERATED_C "${GENERATED_C}#include <stdint.h>\n\n")
    set(GENERATED_C "${GENERATED_C}/* Module name lookup table - defined once */\n")
    set(GENERATED_C "${GENERATED_C}const char *debug_module_names[] = {\n")
    set(GENERATED_C "${GENERATED_C}${MODULE_NAME_TABLE}")
    set(GENERATED_C "${GENERATED_C}};\n")

    # Write the .c file
    file(WRITE "${OUTPUT_NAMES_C}" "${GENERATED_C}")

    message(STATUS "Generated ${OUTPUT_HEADER} with ${MODULE_COUNT} modules")
    message(STATUS "Generated ${OUTPUT_NAMES_C} with module names array")
endfunction()

# Script mode entry point (when called with cmake -P)
if(CMAKE_SCRIPT_MODE_FILE)
    parse_libplctag_header_impl("${INPUT_HEADER}" "${OUTPUT_HEADER}" "${OUTPUT_NAMES_C}")
endif()

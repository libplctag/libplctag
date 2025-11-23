# ParseLibplctagHeader.cmake
# Parses libplctag.h to extract enum definitions and generate internal headers

function(parse_libplctag_header INPUT_HEADER OUTPUT_HEADER OUTPUT_NAMES_C)
    message(STATUS "Parsing ${INPUT_HEADER} to generate ${OUTPUT_HEADER}")
    
    # Read the entire header file
    file(READ "${INPUT_HEADER}" HEADER_CONTENT)
    
    # Extract error codes enum
    string(REGEX MATCH "typedef enum \\{[^}]*PLCTAG_ERR_BUSY[^}]*\\} plctag_error_code_t" ERROR_ENUM "${HEADER_CONTENT}")
    
    # Extract debug levels enum
    string(REGEX MATCH "typedef enum \\{[^}]*PLCTAG_DEBUG_SPEW[^}]*\\} plctag_debug_level_t" DEBUG_ENUM "${HEADER_CONTENT}")
    
    # Extract all PLCTAG_MODULE_ defines (handles multi-line formatting)
    string(REGEX MATCHALL "#define[ \t]+PLCTAG_MODULE_[A-Z_0-9]+[ \t\n]*\\(1ULL << [0-9]+\\)" MODULE_DEFINES "${HEADER_CONTENT}")
    
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
    
    # Parse module defines and create enum
    set(MODULE_ENUM_ENTRIES "")
    set(MODULE_NAME_TABLE "")
    set(MODULE_COUNT 0)
    foreach(MODULE_DEF ${MODULE_DEFINES})
        # Extract module name and bit position
        string(REGEX MATCH "PLCTAG_MODULE_([A-Z_0-9]+)" MODULE_NAME_MATCH "${MODULE_DEF}")
        set(MODULE_NAME "${CMAKE_MATCH_1}")
        
        string(REGEX MATCH "1ULL << ([0-9]+)" BIT_MATCH "${MODULE_DEF}")
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
    set(GENERATED_HEADER "${GENERATED_HEADER} * Generated from ${INPUT_HEADER}\n")
    set(GENERATED_HEADER "${GENERATED_HEADER} * by ParseLibplctagHeader.cmake\n")
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
    set(GENERATED_C "${GENERATED_C} * Generated from ${INPUT_HEADER}\n")
    set(GENERATED_C "${GENERATED_C} * by ParseLibplctagHeader.cmake\n")
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

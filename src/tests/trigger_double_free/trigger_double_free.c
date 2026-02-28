#include "compat_utils.h"
#include <libplctag/lib/libplctag.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define REQUIRED_VERSION 2, 4, 0

#define STR_IMPL_(x) #x      // stringify argument
#define STR(x) STR_IMPL_(x)  // indirection to expand argument macros

#define TABLE_SIZE 10
#define TABLE_SIZE2 1
#define DATA_TIMEOUT 5000
#define TRY_TIMES 100
#define TIME_BETW_OP 100
#define TIME_BETW_TAG 0
#define PLC5000_PATH1 "protocol=ab-eip&elem_count=" STR(TABLE_SIZE) "&gateway=10.206.1.40&path=1,5&cpu=logixpccc&name=N7:0"
#define PLC5000_PATH2 "protocol=ab-eip&elem_count=" STR(TABLE_SIZE) "&gateway=10.206.1.40&path=1,5&cpu=logixpccc&name=N7:10"

static void FATAL(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    exit(1);
}

void ConcurrentRead(const char *tagPath1, const char *tagPath2) {
    int32_t tagHandles[2] = {plc_tag_create(tagPath1, DATA_TIMEOUT), plc_tag_create(tagPath2, DATA_TIMEOUT)};

    for(int i = 0; i < 2; i++) {
        if(tagHandles[i] < 0) { FATAL("Error[%d] creating tag handle: %s\n", i, plc_tag_decode_error(tagHandles[i])); }
    }

    for(int t = 0; t < TRY_TIMES; t++) {

        int rc[2] = {0, 0};
        int32_t elem_size[2] = {0, 0};

        // Invoke async read for both tag handles
        for(int i = 0; i < 2; i++) {
            rc[i] = plc_tag_read(tagHandles[i], 0);
            compat_sleep_ms(TIME_BETW_TAG, NULL);
        }

        // Wait for both tag handles to be finish reading
        for(int i = 0; i < 2; i++) {
            while(rc[i] != PLCTAG_STATUS_OK) {
                if(rc[i] != PLCTAG_STATUS_OK && rc[i] != PLCTAG_STATUS_PENDING) {
                    FATAL("Error[%d] %d: %s\n", i, rc[i], plc_tag_decode_error(rc[i]));
                }

                compat_sleep_ms(10, NULL);
                rc[i] = plc_tag_status(tagHandles[i]);
            }
        }

        // Print their values
        for(int i = 0; i < 2; i++) {
            elem_size[i] = plc_tag_get_int_attribute(tagHandles[i], "elem_size", 0);
            for(int j = 0; j < TABLE_SIZE2; j++) {
                printf("Tag[%d][%d] = %d\n", i, j, plc_tag_get_int16(tagHandles[i], (j * elem_size[i])));
            }
        }

        compat_sleep_ms(TIME_BETW_OP, NULL);
    }

    for(int i = 0; i < 2; i++) { plc_tag_destroy(tagHandles[i]); }
    return;
}

int main(void) {
    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    printf("Test: Concurrently Read Same Table\n");
    // SequentialRead(PLC5000_PATH1, PLC5000_PATH2);
    ConcurrentRead(PLC5000_PATH1, PLC5000_PATH2);

    printf("Test: finished successfully\n");
    return 0;
}

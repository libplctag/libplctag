#include "compat_utils.h"
#include <libplctag/lib/libplctag.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>


#ifdef _WIN32
#    define SERVER_CMD_START "start /B ab_server --plc=ControlLogix --path=1,0 --tag=TestTag:DINT[1]"
#    define SERVER_CMD_STOP "taskkill /IM ab_server.exe /F"
#else
#    define SERVER_CMD_START "./ab_server --plc=ControlLogix --path=1,0 --tag=TestTag:DINT[1] &"
#    define SERVER_CMD_STOP "pkill -f ab_server"
#endif

#define REQUIRED_VERSION 2, 6, 4
#define TAG_ATTRIBS \
    "protocol=ab-eip&gateway=127.0.0.1&path=1,0&cpu=ControlLogix&elem_type=DINT&elem_count=1&name=TestTag[0]&debug=2"
#define TIMEOUT_MS 5000
#define RUN_TIME_MS 5000
#define SLEEP_MS 100
#define EXPECTED_VALUE 42

static volatile int running = 1;
static volatile int read_passed = 0;
static volatile int write_passed = 0;

void *reader_thread(void *arg) {
    int32_t tag = plc_tag_create(TAG_ATTRIBS, TIMEOUT_MS);
    if(tag < 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "[ERROR] Could not create tag for reading: %s\n", plc_tag_decode_error(tag));
        read_passed = 0;
        return NULL;
    }

    while(running) {
        int rc = plc_tag_read(tag, TIMEOUT_MS);
        if(rc == PLCTAG_STATUS_OK) {
            int val = plc_tag_get_int32(tag, 0);
            if(val == EXPECTED_VALUE) {
                // NOLINTNEXTLINE
                fprintf(stdout, "[INFO ] Value is %d.\n", EXPECTED_VALUE);
                read_passed = 1;
                running = 0;
            } else {
                // Failed
                // NOLINTNEXTLINE
                fprintf(stderr, "[ERROR] Unexpected value: %d\n", val);
            }
        } else {
            // NOLINTNEXTLINE
            fprintf(stderr, "[ERROR] Read failed: %s\n", plc_tag_decode_error(rc));
            read_passed = 0;
        }
        compat_sleep_ms(SLEEP_MS, NULL);
    }

    plc_tag_destroy(tag);
    return NULL;
}

void *writer_thread(void *arg) {
    int32_t tag = plc_tag_create(TAG_ATTRIBS, TIMEOUT_MS);
    if(tag < 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "[ERROR] Could not create tag for writing: %s\n", plc_tag_decode_error(tag));
        write_passed = 0;
        return NULL;
    }

    plc_tag_set_int32(tag, 0, EXPECTED_VALUE);
    int rc = plc_tag_write(tag, TIMEOUT_MS);
    if(rc == PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stdout, "[WRITE] Value: %d\n", EXPECTED_VALUE);
        write_passed = 1;
    } else {
        // NOLINTNEXTLINE
        fprintf(stderr, "[ERROR] Write failed: %s\n", plc_tag_decode_error(rc));
        write_passed = 0;
    }

    plc_tag_destroy(tag);

    return NULL;
}

void start_server() {
    // NOLINTNEXTLINE
    fprintf(stdout, "[INFO ] Starting ab_server...\n");

    int rc = system(SERVER_CMD_START);

    if(rc != 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "[ERROR] Failed to start ab_server\n");
        exit(1);
    }

    compat_sleep_ms(1000, NULL);  // wait for server to initialize
}

void stop_server() {
    // NOLINTNEXTLINE
    fprintf(stdout, "[INFO ] Stopping ab_server...\n");
    system(SERVER_CMD_STOP);
    compat_sleep_ms(500, NULL);
}

int main(void) {
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: libplctag version not compatible\n");
        return 1;
    }

    start_server();

    int32_t tag = plc_tag_create(TAG_ATTRIBS, TIMEOUT_MS);
    if(tag < 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: Could not create tag: %s\n", plc_tag_decode_error(tag));
        stop_server();
        return 1;
    }

    compat_thread_t reader, writer;
    compat_thread_create(&writer, writer_thread, (void *)(intptr_t)tag);
    compat_thread_create(&reader, reader_thread, (void *)(intptr_t)tag);

    compat_sleep_ms(RUN_TIME_MS, NULL);
    running = 0;

    compat_thread_join(reader, NULL);
    compat_thread_join(writer, NULL);

    plc_tag_destroy(tag);
    stop_server();

    // NOLINTNEXTLINE
    fprintf(stdout, "[DONE ] Test completed.\n");

    if(!read_passed) {
        // NOLINTNEXTLINE
        fprintf(stderr, "[FAIL ] Read operations failed.\n");
        return 1;
    } else {
        // NOLINTNEXTLINE
        fprintf(stdout, "[PASS ] Read operations succeeded.\n");
    }

    if(!write_passed) {
        // NOLINTNEXTLINE
        fprintf(stderr, "[FAIL ] Write operation failed.\n");
        return 1;
    } else {
        // NOLINTNEXTLINE
        fprintf(stdout, "[PASS ] Write operation succeeded.\n");
    }

    return 0;
}

#include <errno.h>
#include <modbus-tcp.h>
#include <modbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    modbus_t *ctx;
    modbus_mapping_t *mb_mapping;
    int rc;
    int s = -1;

    ctx = modbus_new_tcp("127.0.0.1", 1502);
    if(ctx == NULL) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Unable to create the libmodbus context\n");
        return -1;
    }

    mb_mapping = modbus_mapping_new(100, 100, 100, 100);
    if(mb_mapping == NULL) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Failed to allocate the mapping: %s\n", modbus_strerror(errno));
        modbus_free(ctx);
        return -1;
    }

    s = modbus_tcp_listen(ctx, 1);
    if(s == -1) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Unable to listen: %s\n", modbus_strerror(errno));
        modbus_free(ctx);
        return -1;
    }

    modbus_tcp_accept(ctx, &s);

    for(;;) {
        uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];
        rc = modbus_receive(ctx, query);
        if(rc > 0) {
            modbus_reply(ctx, query, rc, mb_mapping);
        } else if(rc == -1) {
            break;
        }
    }

    if(s != -1) { close(s); }
    modbus_mapping_free(mb_mapping);
    modbus_free(ctx);

    return 0;
}


#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "log.h"
#include "packet.h"
#include "session.h"
#include "tags.h"


#define BUFFER_LEN (4096)

typedef struct {
    int sock;

//    uint16_t command;
//    uint16_t length;
//    uint32_t status;

    uint64_t sender_context;
    uint32_t session_handle;
    uint32_t connection_id;
    uint32_t connection_id_targ;
    uint16_t connection_seq;

    uint16_t max_packet_size;

    uint8_t buf[BUFFER_LEN];
    uint16_t buf_len;
} session_context;



static void print_buf(uint8_t *buf, size_t data_len);
static int process_packet(session_context *context);
static void register_session(session_context *context);

static void process_unconnected_data(session_context *context);
static void handle_forward_open_ex(session_context *context);
static void handle_forward_close(session_context *context);

static void process_connected_data(session_context *context);
static void handle_cip_read(session_context *context);
static void handle_cip_write(session_context *context);

static uint8_t *read_tag_path(uint8_t *buf, char **tag_name, int *item);


static _Atomic uint32_t session_id;
static _Atomic uint32_t connection_id;



void *session_handler(void *sockptr)
{
    int continue_running = 1;
    int sock = *(int*)sockptr;
    session_context context;

    free(sockptr);

    memset(&context, 0, sizeof(context));
    context.sock = sock;

    while(continue_running) {
        ssize_t rc = read(context.sock, context.buf, BUFFER_LEN);

        if(rc < 0) {
            log("read() failed!\n");
            break;
        }

        log("session_handler() got packet:\n");
        print_buf(context.buf, (size_t)rc); /* safe cast because we checked for negative above */

        context.buf_len = (uint16_t)rc;

        continue_running = process_packet(&context);
    }

    log("client handler thread terminating.\n");

//    buffer_free(buf);
    close(sock);

    return NULL;
}


int process_packet(session_context *context)
{
    eip_header *header = (eip_header*)context->buf;

    if(context->buf_len < sizeof(eip_header)) {
        log("process_packet() got less data (%d) than needed for EIP header!", (int)context->buf_len);
        return 0;
    }

    if(context->buf_len != sizeof(*header) + header->length) {
        log("process_packet() got wrong amount (%d) of data than required (%d)\n",(int)context->buf_len, (int)(sizeof(*header) + header->length));
        return 0;
    }

    switch(header->command) {
    case EIP_REGISTER_SESSION:
        register_session(context);
        break;

    case EIP_UNCONNECTED_DATA:
        process_unconnected_data(context);
        break;

    case EIP_CONNECTED_SEND:
        process_connected_data(context);
        break;

    default:
        log("process_packet() got unsupported packet type %x!\n",header->command);
        break;
    }

    return 1;
}


void register_session(session_context *context)
{
    session_registration *reg = (session_registration*)context->buf;
    ssize_t rc = 0;

    log("register_session() got request:\n");
    print_buf(context->buf, sizeof(*reg));

    reg->session_handle = ++session_id;

    log("register_session() sending response:\n");
    print_buf(context->buf, sizeof(*reg));

    rc = write(context->sock, context->buf, sizeof(*reg));
    if(rc != sizeof(*reg)) {
        log("Amount written, %d, does not equal the response size, %d!\n", (int)rc, (int)sizeof(*reg));
    }
}


void process_unconnected_data(session_context *context)
{
    unconnected_message *header = (unconnected_message *)context->buf;

    switch(header->service_code) {
    case CIP_CMD_FORWARD_OPEN_EX:
        handle_forward_open_ex(context);
        break;

    case CIP_CMD_FORWARD_CLOSE:
        handle_forward_close(context);
        break;

    default:
        log("process_unconnected_data() unsupported service code %x!\n", header->service_code);
        print_buf(context->buf,sizeof(eip_header) + header->length);

        break;
    }
}





void handle_forward_open_ex(session_context *context)
{
    forward_open_ex_request *req = (forward_open_ex_request *)context->buf;
    forward_open_response resp;
    ssize_t rc = 0;

    log("handle_forward_open_ex() got request:\n");
    print_buf(context->buf, sizeof(eip_header) + req->length);

    if((req->orig_to_targ_conn_params_ex & 0xFFFF) > BUFFER_LEN) {
        log("Requested packet size, %d, is too large!\n", (int)(req->orig_to_targ_conn_params_ex & 0xFFFF));
        return;
    }

    context->max_packet_size = req->orig_to_targ_conn_params_ex & 0xFFFF;

    memset(&resp, 0, sizeof(resp));

    resp.command = req->command;
    resp.session_handle = req->session_handle;
    resp.sender_context = req->sender_context;

    resp.length = sizeof(resp) - sizeof(eip_header);

    resp.interface_handle = req->interface_handle;
    resp.router_timeout = req->router_timeout;

    resp.cpf_item_count = 2;
    resp.cpf_udi_item_type = CPF_ITEM_UDI;
    resp.cpf_udi_item_length = (uint8_t*)(&resp + 1) - (uint8_t *)(&resp.resp_service_code);

    resp.resp_service_code = CIP_CMD_FORWARD_OPEN | CIP_CMD_RESPONSE;
    resp.general_status = 0;
    resp.status_size = 0;
    /* make the connection ID global, seems that way on LGX??? */
    resp.orig_to_targ_conn_id = ++connection_id;
    resp.targ_to_orig_conn_id = req->targ_to_orig_conn_id;
    resp.conn_serial_number = req->conn_serial_number;
    resp.orig_vendor_id = req->orig_vendor_id;
    resp.orig_serial_number = req->orig_serial_number;
    resp.orig_to_targ_api = req->orig_to_targ_rpi;
    resp.targ_to_orig_api = req->targ_to_orig_rpi;
    resp.app_data_size = 0;

    log("handle_forward_open_ex() called with remote connection ID %x and local connection ID %x\n", resp.targ_to_orig_conn_id, resp.orig_to_targ_conn_id);

    context->connection_id_targ = req->targ_to_orig_conn_id;

    memcpy(context->buf, &resp, sizeof(resp));

    log("handle_forward_open_ex() sending response:\n");
    print_buf(context->buf, sizeof(resp));

    rc = write(context->sock, context->buf, sizeof(resp));
    if(rc != sizeof(resp)) {
        log("Amount written, %d, does not equal the response size, %d!\n", (int)rc, (int)sizeof(resp));
    }
}



void handle_forward_close(session_context *context)
{
    forward_close_request *req = (forward_close_request *)context->buf;
    forward_close_response resp;
    ssize_t rc = 0;
    uint8_t *data_end = context->buf + sizeof(eip_header) + req->length;
    uint8_t *path_start = context->buf + sizeof(*req);
    ssize_t path_size = data_end - path_start;
    uint8_t path_data[path_size];

    log("handle_forward_close() got request:\n");
    print_buf(context->buf, sizeof(eip_header) + req->length);

    memset(&resp, 0, sizeof(resp));

    memcpy(path_data, path_start, (size_t)path_size); /* FIXME - check if negative first! */

    log("handle_forward_close() path_size=%d\n",(int)path_size);
    print_buf(path_data, (size_t)path_size);

    resp.command = req->command;
    resp.session_handle = req->session_handle;
    resp.sender_context = req->sender_context;

    resp.length = (uint16_t)((size_t)path_size + sizeof(resp) - sizeof(eip_header));

    resp.interface_handle = req->interface_handle;
    resp.router_timeout = req->router_timeout;

    resp.cpf_item_count = 2;
    resp.cpf_udi_item_type = CPF_ITEM_UDI;
    resp.cpf_udi_item_length = (uint8_t*)(&resp + 1) - (uint8_t *)(&resp.resp_service_code);

    resp.resp_service_code = CIP_CMD_FORWARD_CLOSE | CIP_CMD_RESPONSE;
    resp.general_status = 0;
    resp.status_size = 0;

    resp.conn_serial_number = req->conn_serial_number;
    resp.orig_vendor_id = req->orig_vendor_id;
    resp.orig_serial_number = req->orig_serial_number;

    memcpy(context->buf, &resp, sizeof(resp));
    memcpy(context->buf + sizeof(resp), path_data, (size_t)path_size);

    log("handle_forward_close() sending response:\n");
    print_buf(context->buf, sizeof(resp) + (size_t)path_size);

    rc = write(context->sock, context->buf, sizeof(resp) + (size_t)path_size);
    if(rc != (int)(sizeof(resp) + (size_t)path_size)) {
        log("Amount written, %d, does not equal the response size, %d!\n", (int)rc, (int)(sizeof(resp) + (size_t)path_size));
    }
}





void process_connected_data(session_context *context)
{
    connected_message *header = (connected_message *)context->buf;

    switch(header->service_code) {
    case CIP_CMD_READ:
    case CIP_CMD_READ_FRAG:
        handle_cip_read(context);
        break;

    case CIP_CMD_WRITE:
    case CIP_CMD_WRITE_FRAG:
        handle_cip_write(context);
        break;


    default:
        log("process_connected_data() unsupported service code %x!\n", header->service_code);
        print_buf(context->buf,sizeof(eip_header) + header->length);

        break;
    }
}




void handle_cip_read(session_context *context)
{
    int rc = 0;
    connected_message *req = (connected_message *)(context->buf);
    connected_message_cip_resp resp;
    connected_message_cip_resp *resp_ptr = NULL;
    uint8_t *data = NULL;
    char *tag_name = NULL;
    int item_offset = 0;
    int elem_count = 0;
    int byte_offset = 0;
    tag_data *tag = NULL;
    int data_remaining = 0;
//    int items_remaining = 0;
    int data_avail = 0;
    int items_that_fit = 0;
    int base_offset = 0;

    log("Starting.");

    memset(&resp, 0, sizeof(resp));

    resp.command = req->command;
    resp.sender_context = req->sender_context;
    resp.options = req->options;
    resp.interface_handle = req->interface_handle;
    resp.router_timeout = req->router_timeout;
    resp.cpf_item_count = 2;
    resp.cpf_cai_item_type = CPF_ITEM_CAI;
    resp.cpf_cai_item_length = 4;
    resp.cpf_targ_conn_id = context->connection_id_targ;
    resp.cpf_cdi_item_type = CPF_ITEM_CDI;
    resp.cpf_cdi_item_length = 0; /* patch this later! */
    resp.cpf_conn_seq_num = req->cpf_conn_seq_num;
    resp.service_code = req->service_code | CIP_CMD_OK;

    data = (uint8_t*)(&(req->service_code)) + 1;

    /* read the tag path. */
    data = read_tag_path(data, &tag_name, &item_offset);
    if(!data) {
        log("Unable to read tag path data!");
        return;
    }

    tag = find_tag(tag_name);

    free(tag_name);

    if(!tag) {
        log("tag %s not found!\n", tag_name);
        return;
    }

    /* read the number of elements to read */
    elem_count = (data[0]) + ((data[1]) << 8);
    data += 2;

    log("tag elem_count=%d\n",elem_count);

    if(req->service_code == CIP_CMD_READ_FRAG) {
        byte_offset =  (data[0])
                       + ((data[1]) << 8)
                       + ((data[2]) << 16)
                       + ((data[3]) << 24);
        data += 4;

        log("tag byte offset=%d\n", byte_offset);
    }

    if(item_offset + elem_count > tag->elem_count) {
        log("handle_tag_read() number of items requested is too many.  Item offset is %d, number of items requested is %d and total items is %d\n", item_offset, elem_count, tag->elem_count);
        return;
    }

    base_offset = (item_offset * tag->elem_size) + byte_offset;

    log("reading %d elements of tag %s starting at offset %d.\n", elem_count, tag_name, base_offset);

    /* copy data into response */
    memcpy(context->buf, &resp, sizeof(resp));

    resp_ptr = (connected_message_cip_resp *)(context->buf);
    data = (uint8_t*)(resp_ptr+1);

    memcpy(data, tag->data_type, 2);
    data += 2;

    /* how much data is left to read? */
    data_remaining = (elem_count * tag->elem_size) - byte_offset;
//    items_remaining = data_remaining / tag->elem_size;

    /* how much can we actually send? */
    data_avail = context->max_packet_size - 8; /* MAGIC overhead */
    items_that_fit = data_avail / tag->elem_size;

    if((int)((items_that_fit * tag->elem_size) + byte_offset) > (int)(elem_count * tag->elem_size)) {
        items_that_fit = (((elem_count * tag->elem_size) - byte_offset) / tag->elem_size);
        log("Clamping number of items to write to %d\n", items_that_fit);
    }

    memcpy(data, &tag->data[base_offset], (size_t)items_that_fit * tag->elem_size);
    data += (items_that_fit * tag->elem_size);

    /* set the status based on whether there is more to read or not. */
    if(data_remaining > data_avail) {
        resp_ptr->cip_status = CIP_STATUS_FRAG;
    } else {
        resp_ptr->cip_status = CIP_STATUS_OK;
    }

    resp_ptr->length = (uint16_t)(data - (uint8_t*)&(resp_ptr->interface_handle));
    resp_ptr->cpf_cdi_item_length = (uint16_t)(data - (uint8_t*)(&(resp_ptr->cpf_conn_seq_num)));

    log("handle_cip_read() sending response:\n");
    print_buf(context->buf, (size_t)(data - context->buf));

    rc = (int)write(context->sock, context->buf, (size_t)(data - context->buf));
    if(rc != (int)(data - context->buf)) {
        log("Amount written, %d, does not equal the response size, %d!\n", (int)rc, (int)(data - context->buf));
    }

    log("Done.\n");
}


void handle_cip_write(session_context *context)
{
    int rc = 0;
    connected_message *req = (connected_message *)(context->buf);
    connected_message_cip_resp resp;
//    connected_message_cip_resp *resp_ptr = NULL;
    uint8_t *data = NULL;
    char *tag_name = NULL;
    int item_offset = 0;
    int elem_count = 0;
    int byte_offset = 0;
    tag_data *tag = NULL;
    int data_remaining = 0;
//    int items_remaining = 0;
    int data_avail = 0;
//    int items_that_fit = 0;
    int base_offset = 0;

    log("Starting.");

    memset(&resp, 0, sizeof(resp));

    resp.command = req->command;
    resp.sender_context = req->sender_context;
    resp.options = req->options;
    resp.interface_handle = req->interface_handle;
    resp.router_timeout = req->router_timeout;
    resp.cpf_item_count = 2;
    resp.cpf_cai_item_type = CPF_ITEM_CAI;
    resp.cpf_cai_item_length = 4;
    resp.cpf_targ_conn_id = context->connection_id_targ;
    resp.cpf_cdi_item_type = CPF_ITEM_CDI;
    resp.cpf_cdi_item_length = 0; /* patch this later! */
    resp.cpf_conn_seq_num = req->cpf_conn_seq_num;
    resp.cpf_conn_seq_num = req->cpf_conn_seq_num;
    resp.service_code = req->service_code | CIP_CMD_OK;

    data = (uint8_t*)(&(req->service_code)) + 1;

    /* read the tag name. */
    data = read_tag_path(data, &tag_name, &item_offset);
    if(!data) {
        log("Unable to read tag path data!");
        return;
    }

    tag = find_tag(tag_name);

    free(tag_name);

    if(!tag) {
        log("tag %s not found!\n", tag_name);
        return;
    }

    /* check the data type. */
    if(data[0] != tag->data_type[0]) {
        log("tag data type not matching.  Expected %x but got %x!\n", tag->data_type[0], data[0]);
        return;
    }

    if(data[1] != tag->data_type[1]) {
        log("tag data type not matching.  Expected %x but got %x!\n", tag->data_type[1], data[1]);
        return;
    }

    data += 2;

    /* read the number of elements to write */
    elem_count = (data[0]) + ((data[1]) << 8);
    data += 2;

    log("tag elem_count=%d\n",elem_count);

    if(req->service_code == CIP_CMD_WRITE_FRAG) {
        byte_offset =  (data[0])
                       +((data[1]) << 8)
                       +((data[2]) << 16)
                       +((data[3]) << 24);
        data += 4;

        log("tag byte offset=%d\n", byte_offset);
    }

    if(item_offset + elem_count > tag->elem_count) {
        log("handle_tag_write() number of items requested is too many.  Item offset is %d, number of items requested is %d and total items is %d\n", item_offset, elem_count, tag->elem_count);
        return;
    }

    base_offset = (item_offset * tag->elem_size) + byte_offset;

    log("writing %d elements of tag %s starting at offset %d.\n", elem_count, tag->name, base_offset);

    data_avail = context->buf_len - (int)(data - context->buf);

    /* FIXME - must check to see if data_avail is negative! */
    memcpy(tag->data + base_offset, data, (size_t)data_avail);

    /* how much data is left to write? */
    data_remaining = (elem_count * tag->elem_size) - byte_offset - data_avail;

    log("handle_cip_write() elem_count=%d, tag->elem_size=%d, byte_offset=%d, base_offset=%d, data_avail=%d\n",elem_count, tag->elem_size, byte_offset, base_offset, data_avail);

    /* set the status based on whether there is more to write or not. */
    if(data_remaining > 0) {
        resp.cip_status = CIP_STATUS_FRAG;
    } else {
        resp.cip_status = CIP_STATUS_OK;
    }

    resp.length = (sizeof(resp) - sizeof(eip_header));
    resp.cpf_cdi_item_length = ((uint8_t*)(&resp.cip_status_words) - (uint8_t*)(&(resp.cpf_conn_seq_num)))+1;

    memcpy(context->buf, &resp, sizeof(resp));

    log("handle_cip_write() sending response:\n");
    print_buf(context->buf, sizeof(resp));

    rc = (int)write(context->sock, context->buf, sizeof(resp));
    if(rc != sizeof(resp)) {
        log("Amount written, %d, does not equal the response size, %d!\n", (int)rc, (int)(sizeof(resp)));
    }

    log("Done.\n");
}




uint8_t *read_tag_path(uint8_t *buf, char **tag_name, int *item_offset)
{
    /* read the length in words, convert to bytes. */
    uint8_t *data = buf;
    uint8_t path_len = (uint8_t)((*data)*2); /* translate to bytes */
    uint8_t name_len = 0;
    int index = 0;

    log("read_tag_path() starting with path_len=%d\n", path_len);

    *item_offset = 0;

    index++;

    /* read the tag path */
    if(buf[index] == CIP_SYMBOLIC_SEGMENT) {
        index++;

        name_len = buf[index];
        index++;

        log("read_tag_path() reading symbolic segment of length %d\n", name_len);

        *tag_name = (char *)calloc(1, (size_t)(name_len+1));

        if(!*tag_name) {
            log("Warning! Unable to allocate new tag name buf!");
            return NULL;
        }

        /* copy the tag name */
        memcpy(*tag_name, &buf[index], name_len);

        /* if the name length is odd, then there is a zero byte of padding.  Skip it. */
        if(name_len & 0x01) {
            name_len++;
        }

        index += name_len;

        log("read_tag_path() found tag '%s'\n", *tag_name);
    } else {
        log("read_tag_path() unsupported segment type %x\n",buf[index]);
        return NULL;
    }

    /* is there a numeric segment? */
    if(index < path_len) {
        switch(buf[index]) {
        case 0x28:
            /* one byte */
            index++;
            *item_offset = buf[index];
            index++;
            break;

        case 0x29:
            /* two bytes */
            index += 2; /* type plus padding. */
            *item_offset = buf[index]
                           +((int)buf[index+1] << 8);
            index += 2;
            break;

        case 0x2A:
            /* four bytes */
            index += 2; /* type plus padding. */
            *item_offset = buf[index]
                           +((int)buf[index+1] << 8)
                           +((int)buf[index+2] << 16)
                           +((int)buf[index+3] << 24);
            index += 4;
            break;

        default:
            log("read_tag_path() unknown segment type %x\n", buf[index]);
            return NULL;
            break;
        }
    }

    if(index < path_len) {
        log("read_tag_path() unable to parse entire tag path!  %d bytes remaining!\n", path_len - index);
        return NULL;
    }

    log("read_tag_path() processed path of length %d.\n", index);

    return buf + path_len + 1; /* 1 for the path length byte. */
}



void print_buf(uint8_t *buf, size_t data_len)
{
    int index = 0;
    int column = 0;
    int row = 0;
    int num_rows = 0;
    int total_bytes = 0;

    if(!buf) {
        log("buffer is null.\n");
        return;
    }

    total_bytes = (int)data_len;
    num_rows = (total_bytes + 9)/10;

    log("packet of length %d:\n",(int)data_len);

    for(row = 0; row < num_rows; row++) {
        log("%03d", row*10);
        for(column=0; column < 10; column++) {
            index = (row * 10) + column;

            if(index < total_bytes) {
                log(" %02x",buf[index]);
            }
        }
        log("\n");
    }
}

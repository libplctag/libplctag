
#pragma once


#include <stdint.h>

#define EIP_REGISTER_SESSION     ((uint16_t)0x0065)
#define EIP_UNREGISTER_SESSION   ((uint16_t)0x0066)
#define EIP_UNCONNECTED_DATA     ((uint16_t)0x006F)
#define EIP_CONNECTED_SEND       ((uint16_t)0x0070)


#define CIP_CMD_RESPONSE             ((uint8_t)0x80)
#define CIP_CMD_PCCC_EXECUTE         ((uint8_t)0x4B)
#define CIP_CMD_FORWARD_CLOSE        ((uint8_t)0x4E)
#define CIP_CMD_FORWARD_OPEN         ((uint8_t)0x54)
#define CIP_CMD_FORWARD_OPEN_EX      ((uint8_t)0x5B)
#define CIP_CMD_READ                 ((uint8_t)0x4C)
#define CIP_CMD_WRITE                ((uint8_t)0x4D)
#define CIP_CMD_READ_FRAG            ((uint8_t)0x52)
#define CIP_CMD_WRITE_FRAG           ((uint8_t)0x53)



#define CIP_CMD_OK                   ((uint8_t)0x80)

#define CIP_STATUS_OK               ((uint8_t)0)
#define CIP_STATUS_FRAG             ((uint8_t)0x06)

/* CPF Item Types */
#define CPF_ITEM_NAI ((uint16_t)0x0000) /* NULL Address Item */
#define CPF_ITEM_CAI ((uint16_t)0x00A1) /* connected address item */
#define CPF_ITEM_CDI ((uint16_t)0x00B1) /* connected data item */
#define CPF_ITEM_UDI ((uint16_t)0x00B2) /* Unconnected data item */

#define CIP_SYMBOLIC_SEGMENT  ((uint8_t)0x91)



typedef struct {
    uint16_t command;
    uint16_t length;
    uint32_t session_handle;
    uint32_t status;
    uint64_t sender_context;
    uint32_t options;
} __attribute__((packed)) eip_header;


/* Session Registration Request/Response */
typedef struct {
    uint16_t command;
    uint16_t length;
    uint32_t session_handle;
    uint32_t status;
    uint64_t sender_context;
    uint32_t options;

    /* session registration request */
    uint16_t eip_version;
    uint16_t option_flags;
} __attribute__((packed)) session_registration;



typedef struct {
    uint16_t command;
    uint16_t length;
    uint32_t session_handle;
    uint32_t status;
    uint64_t sender_context;
    uint32_t options;

    /* Interface Handle etc. */
    uint32_t interface_handle;      /* ALWAYS 0 */
    uint16_t router_timeout;        /* in seconds */

    /* Common Packet Format - CPF Unconnected */
    uint16_t cpf_item_count;        /* ALWAYS 2 */
    uint16_t cpf_nai_item_type;     /* ALWAYS 0 */
    uint16_t cpf_nai_item_length;   /* ALWAYS 0 */
    uint16_t cpf_udi_item_type;     /* ALWAYS 0x00B2 - Unconnected Data Item */
    uint16_t cpf_udi_item_length;   /* REQ: fill in with length of remaining data. */

    /* CM Service Request - Connection Manager */
    uint8_t service_code;
} __attribute__((packed)) unconnected_message;



typedef struct {
    uint16_t command;
    uint16_t length;
    uint32_t session_handle;
    uint32_t status;
    uint64_t sender_context;
    uint32_t options;

    /* Interface Handle etc. */
    uint32_t interface_handle;      /* ALWAYS 0 */
    uint16_t router_timeout;        /* in seconds */

    /* Common Packet Format - CPF Unconnected */
    uint16_t cpf_item_count;        /* ALWAYS 2 */
    uint16_t cpf_nai_item_type;     /* ALWAYS 0 */
    uint16_t cpf_nai_item_length;   /* ALWAYS 0 */
    uint16_t cpf_udi_item_type;     /* ALWAYS 0x00B2 - Unconnected Data Item */
    uint16_t cpf_udi_item_length;   /* REQ: fill in with length of remaining data. */

    /* CM Service Request - Connection Manager */
    uint8_t cm_service_code;        /* ALWAYS 0x54 Forward Open Request */
    uint8_t cm_req_path_size;       /* ALWAYS 2, size in words of path, next field */
    uint8_t cm_req_path[4];         /* ALWAYS 0x20,0x06,0x24,0x01 for CM, instance 1*/

    /* Forward Open Params */
    uint8_t secs_per_tick;          /* seconds per tick */
    uint8_t timeout_ticks;          /* timeout = srd_secs_per_tick * src_timeout_ticks */
    uint32_t orig_to_targ_conn_id;  /* 0, returned by target in reply. */
    uint32_t targ_to_orig_conn_id;  /* what is _our_ ID for this connection, use ab_connection ptr as id ? */
    uint16_t conn_serial_number;    /* our connection serial number */
    uint16_t orig_vendor_id;        /* our unique vendor ID */
    uint32_t orig_serial_number;    /* our unique serial number */
    uint8_t conn_timeout_multiplier;/* timeout = mult * RPI */
    uint8_t reserved[3];            /* reserved, set to 0 */
    uint32_t orig_to_targ_rpi;      /* us to target RPI - Request Packet Interval in microseconds */
    uint32_t orig_to_targ_conn_params_ex; /* some sort of identifier of what kind of PLC we are??? */
    uint32_t targ_to_orig_rpi;      /* target to us RPI, in microseconds */
    uint32_t targ_to_orig_conn_params_ex; /* some sort of identifier of what kind of PLC the target is ??? */
    uint8_t transport_class;        /* ALWAYS 0xA3, server transport, class 3, application trigger */
    uint8_t path_size;              /* size of connection path in 16-bit words
                                     * connection path from MSG instruction.
                                     *
                                     * EG LGX with 1756-ENBT and CPU in slot 0 would be:
                                     * 0x01 - backplane port of 1756-ENBT
                                     * 0x00 - slot 0 for CPU
                                     * 0x20 - class
                                     * 0x02 - MR Message Router
                                     * 0x24 - instance
                                     * 0x01 - instance #1.
                                     */
} __attribute__((packed)) forward_open_ex_request;


typedef struct {
    uint16_t command;
    uint16_t length;
    uint32_t session_handle;
    uint32_t status;
    uint64_t sender_context;
    uint32_t options;

    /* Interface Handle etc. */
    uint32_t interface_handle;      /* ALWAYS 0 */
    uint16_t router_timeout;        /* in seconds */

    /* Common Packet Format - CPF Unconnected */
    uint16_t cpf_item_count;        /* ALWAYS 2 */
    uint16_t cpf_nai_item_type;     /* ALWAYS 0 */
    uint16_t cpf_nai_item_length;   /* ALWAYS 0 */
    uint16_t cpf_udi_item_type;     /* ALWAYS 0x00B2 - Unconnected Data Item */
    uint16_t cpf_udi_item_length;   /* REQ: fill in with length of remaining data. */

    /* Forward Open Reply */
    uint8_t resp_service_code;      /* returned as 0xD4 or 0xDB */
    uint8_t reserved1;               /* returned as 0x00? */
    uint8_t general_status;         /* 0 on success */
    uint8_t status_size;            /* number of 16-bit words of extra status, 0 if success */
    uint32_t orig_to_targ_conn_id;  /* target's connection ID for us, save this. */
    uint32_t targ_to_orig_conn_id;  /* our connection ID back for reference */
    uint16_t conn_serial_number;    /* our connection serial number from request */
    uint16_t orig_vendor_id;        /* our unique vendor ID from request*/
    uint32_t orig_serial_number;    /* our unique serial number from request*/
    uint32_t orig_to_targ_api;      /* Actual packet interval, microsecs */
    uint32_t targ_to_orig_api;      /* Actual packet interval, microsecs */
    uint8_t app_data_size;          /* size in 16-bit words of send_data at end */
    uint8_t reserved2;
} __attribute__((packed)) forward_open_response;


typedef struct {
    uint16_t command;
    uint16_t length;
    uint32_t session_handle;
    uint32_t status;
    uint64_t sender_context;
    uint32_t options;

    /* Interface Handle etc. */
    uint32_t interface_handle;      /* ALWAYS 0 */
    uint16_t router_timeout;        /* in seconds */

    /* Common Packet Format - CPF Unconnected */
    uint16_t cpf_item_count;        /* ALWAYS 2 */
    uint16_t cpf_nai_item_type;     /* ALWAYS 0 */
    uint16_t cpf_nai_item_length;   /* ALWAYS 0 */
    uint16_t cpf_udi_item_type;     /* ALWAYS 0x00B2 - Unconnected Data Item */
    uint16_t cpf_udi_item_length;   /* REQ: fill in with length of remaining data. */

    /* CM Service Request - Connection Manager */
    uint8_t cm_service_code;        /* ALWAYS 0x4E Forward Close Request */
    uint8_t cm_req_path_size;       /* ALWAYS 2, size in words of path, next field */
    uint8_t cm_req_path[4];         /* ALWAYS 0x20,0x06,0x24,0x01 for CM, instance 1*/

    /* Forward Open Params */
    uint8_t secs_per_tick;       /* seconds per tick */
    uint8_t timeout_ticks;       /* timeout = srd_secs_per_tick * src_timeout_ticks */
    uint16_t conn_serial_number;    /* our connection serial number */
    uint16_t orig_vendor_id;        /* our unique vendor ID */
    uint32_t orig_serial_number;    /* our unique serial number */
} __attribute__((packed)) forward_close_request;


typedef struct {
    uint16_t command;
    uint16_t length;
    uint32_t session_handle;
    uint32_t status;
    uint64_t sender_context;
    uint32_t options;

    /* Interface Handle etc. */
    uint32_t interface_handle;      /* ALWAYS 0 */
    uint16_t router_timeout;        /* in seconds */

    /* Common Packet Format - CPF Unconnected */
    uint16_t cpf_item_count;        /* ALWAYS 2 */
    uint16_t cpf_nai_item_type;     /* ALWAYS 0 */
    uint16_t cpf_nai_item_length;   /* ALWAYS 0 */
    uint16_t cpf_udi_item_type;     /* ALWAYS 0x00B2 - Unconnected Data Item */
    uint16_t cpf_udi_item_length;   /* REQ: fill in with length of remaining data. */

    /* Forward Close Response */
    uint8_t resp_service_code;      /* returned as 0xCE */
    uint8_t reserved1;               /* returned as 0x00? */
    uint8_t general_status;         /* 0 on success */
    uint8_t status_size;            /* number of 16-bit words of extra status, 0 if success */
    uint16_t conn_serial_number;    /* our connection serial number */
    uint16_t orig_vendor_id;        /* our unique vendor ID */
    uint32_t orig_serial_number;    /* our unique serial number */
} __attribute__((packed)) forward_close_response;


typedef struct {
    uint16_t command;
    uint16_t length;
    uint32_t session_handle;
    uint32_t status;
    uint64_t sender_context;
    uint32_t options;

    /* Interface Handle etc. */
    uint32_t interface_handle;      /* ALWAYS 0 */
    uint16_t router_timeout;        /* in seconds */

    uint16_t cpf_item_count;        /* ALWAYS 2 */
    uint16_t cpf_cai_item_type;     /* ALWAYS 0x00A1 Connected Address Item */
    uint16_t cpf_cai_item_length;   /* ALWAYS 2 ? */
    uint32_t cpf_targ_conn_id;      /* the connection id from Forward Open */
    uint16_t cpf_cdi_item_type;     /* ALWAYS 0x00B1, Connected Data Item type */
    uint16_t cpf_cdi_item_length;   /* length in bytes of the rest of the packet */

    /* Connection sequence number */
    uint16_t cpf_conn_seq_num;      /* connection sequence ID, inc for each message */

    /* service type for the target object */
    uint8_t service_code;
} __attribute__((packed)) connected_message;




typedef struct {
    uint16_t command;
    uint16_t length;
    uint32_t session_handle;
    uint32_t status;
    uint64_t sender_context;
    uint32_t options;

    /* Interface Handle etc. */
    uint32_t interface_handle;      /* ALWAYS 0 */
    uint16_t router_timeout;        /* in seconds */

    uint16_t cpf_item_count;        /* ALWAYS 2 */
    uint16_t cpf_cai_item_type;     /* ALWAYS 0x00A1 Connected Address Item */
    uint16_t cpf_cai_item_length;   /* ALWAYS 2 ? */
    uint32_t cpf_targ_conn_id;      /* the connection id from Forward Open */
    uint16_t cpf_cdi_item_type;     /* ALWAYS 0x00B1, Connected Data Item type */
    uint16_t cpf_cdi_item_length;   /* length in bytes of the rest of the packet */

    /* Connection sequence number */
    uint16_t cpf_conn_seq_num;      /* connection sequence ID, inc for each message */

    /* service type for the target object */
    uint8_t service_code;
    uint8_t reserved1;
    uint8_t cip_status;
    uint8_t cip_status_words;
} __attribute__((packed)) connected_message_cip_resp;



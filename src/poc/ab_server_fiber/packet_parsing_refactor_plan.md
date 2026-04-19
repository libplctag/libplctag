# Refactoring Plan: Unified Parsing and Encoding Functions

## Overview

This plan combines the use of single-call `bytes_unpack` / `bytes_pack` macros (which can safely decode/encode many fields at once) with the abstraction of dedicated parsing and encoding functions. By mapping raw bytes to explicitly defined C structs (e.g., `cpf_connected_hdr_t`), we separate the mechanical byte-shuffling from the protocol business logic.

## 1. Define Protocol Structs

Instead of declaring 10+ local variables for every protocol field inside the dispatch handlers, we define clear C structs for the protocol headers.

```c
typedef struct {
    uint32_t iface_handle;
    uint16_t timeout;
    uint16_t item_count;
    uint16_t item0_type;
    uint16_t item0_len;
    uint32_t conn_id;
    uint16_t item1_type;
    uint16_t item1_len;
    uint16_t seq_num;
} cpf_connected_hdr_t;
```

## 2. Dedicated Parsing Functions

We extract the decoding logic into isolated functions whose sole responsibility is to hydrate a struct from a `Bytes` payload. By exploiting the macro-generated `bytes_unpack` (which loops internally and short-circuits on failure), we can safely populate all fields simultaneously. These functions return the remaining `Bytes` struct representing the unparsed inner payload, or `{NULL, 0}` on failure, handling their own warning logging to match the existing code footprint.

```c
static Bytes cpf_parse_connected(Bytes payload, cpf_connected_hdr_t *hdr) {
    /* Unpack all header fields in one shot */
    Bytes rest = bytes_unpack(payload, BYTES_LE,
        &hdr->iface_handle,
        &hdr->timeout,
        &hdr->item_count,
        &hdr->item0_type,
        &hdr->item0_len,
        &hdr->conn_id,
        &hdr->item1_type,
        &hdr->item1_len,
        &hdr->seq_num
    );

    if (bytes_is_null(rest)) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF connected: header unpack failed");
        return (Bytes){0}; /* Unpack failed due to malformed or truncated packet */
    }

    /* The remaining bytes inherently form the CIP payload.
     * Ensure their length matches the CPF item1_len (less 2 bytes for the seq_num). */
    if (rest.len != (size_t)(hdr->item1_len - 2)) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF connected: payload length mismatch, expected %zu, got %zu",
              (size_t)(hdr->item1_len - 2), rest.len);
        return (Bytes){0};
    }
    
    return rest;
}
```

## 3. Dedicated Encoding Functions

Similarly, creating functions to encode the structs back into wire format centralizes the layout logic. The encoder takes the populated struct and writes it into the `Arena` using an inline `bytes_pack` call.

```c
static Bytes cpf_encode_connected(Arena *a, const cpf_connected_hdr_t *hdr) {
    return bytes_pack(a, BYTES_LE,
        hdr->iface_handle,
        hdr->timeout,
        hdr->item_count,
        hdr->item0_type,
        hdr->item0_len,
        hdr->conn_id,
        hdr->item1_type,
        hdr->item1_len,
        hdr->seq_num
    );
}
```

## 4. Simplified Dispatcher Logic

With parsing and encoding fully isolated, the actual business logic inside `cpf.c` / `eip.c` / `cip.c` becomes remarkably clean. The handler purely focuses on structural validation, state management, and routing.

```c
extern Bytes cpf_handle_connected(Arena *a, Bytes payload, eip_session_t *sess, plc_config_t *cfg) {
    cpf_connected_hdr_t hdr = {0};

    pdlog(LOG_MODULE_CPF, LOG_LEVEL_DETAIL, "cpf_handle_connected: payload len=%zu", payload.len);

    Bytes cip_data = cpf_parse_connected(payload, &hdr);
    if (bytes_is_null(cip_data)) {
        /* Logging is already handled inside the parser on failure */
        return (Bytes){0};
    }

    /* Protocol Validation */
    if (hdr.item_count != 2 || hdr.item0_type != CPF_ITEM_CONN_ADDR || hdr.item1_type != CPF_ITEM_CONN_DATA) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF connected: unexpected items count=%u item0=0x%04x item1=0x%04x", 
              hdr.item_count, hdr.item0_type, hdr.item1_type);
        return (Bytes){0};
    }

    if (hdr.conn_id != sess->server_connection_id) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF connected: connection ID mismatch: got 0x%08x expected 0x%08x", 
              hdr.conn_id, sess->server_connection_id);
        return (Bytes){0};
    }

    sess->client_connection_seq = hdr.seq_num;
    sess->server_connection_seq++;

    /* Dispatch inner CIP request */
    size_t max_resp = (sess->server_to_client_max_packet > 0) ? 
        (size_t)sess->server_to_client_max_packet : (size_t)cfg->server_to_client_max_packet;
        
    Bytes cip_response = cip_dispatch_connected(a, cip_data, sess, cfg, max_resp);
    
    if(bytes_is_null(cip_response)) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF connected: CIP dispatch returned null");
        return (Bytes){0};
    }

    return wrap_connected(a, cip_response, hdr.conn_id, sess->server_connection_seq);
}
```

## Benefits Summary

1. **Clear Separation of Concerns**: Parsing bytes is decoupled from routing/handling.
2. **Simplified Reading / Less Boilerplate**: The repetitive cascading `rest = bytes_unpack(...)` checks collapse into a single unified step.
3. **Easier Debugging**: Inside GDB, developers can run `print hdr` to view a completely formatted protocol header rather than needing to evaluate an assortment of floating local variables (`conn_id`, `item1_type`, etc.). 
4. **Testability**: The `cpf_parse_*` and `cpf_encode_*` functions have no dispatch side-effects and do not require full system context, enabling robust targeted unit testing of packet boundaries.
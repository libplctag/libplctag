# ENIP Layered Protocol Architecture

**Date:** 2026-05-29  
**Status:** Implemented  
**Purpose:** Document the three-layer separation for request building and response parsing in ENIP

## Overview

The ENIP protocol implementation separates concerns into three cleanly isolated layers:

1. **EIP Layer** (`enip_eip.h/c`) — Ethernet/IP 24-byte encapsulation header
2. **CPF Layer** (`enip_cpf.h/c`) — Common Packet Format items (NAI, UDI, connected address/data)
3. **CIP Layer** (via `mfg_ops` callbacks) — Common Industrial Protocol service/path/data

Each layer:
- Builds requests **inside-out** (CIP → CPF → EIP)
- Parses responses **outside-in** (EIP → CPF → CIP)
- Uses **zero-copy slicing** for efficient response extraction
- Leverages the **Bytes API** (`bytes_pack`, `bytes_unpack`, `bytes_concat`, `bytes_slice`) for all serialization

This design enables:
- **Testability:** Each layer can be tested in isolation
- **Clarity:** Protocol logic separated by network layer
- **Efficiency:** No unnecessary allocations or copies
- **Maintainability:** Manufacturer-specific behavior isolated in strategy modules

---

## Layer 1: EIP (Ethernet/IP Encapsulation)

**File:** `src/libplctag/protocols/enip/enip_eip.h/c`

### Purpose
Implements the RFC 3171 EIP framing header (24 bytes, little-endian).

### Request Building

```c
// 1. Create EIP header struct with command, length, session, status, context, options
enip_eip_header_t hdr = {
    .command = ENIP_CMD_UNCONNECTED_SEND,  // 0x006F
    .length = cpf_payload.len,
    .session_handle = session,
    .status = 0,
    .sender_context = *context_counter,  // Incremented after build
    .options = 0
};

// 2. Pack header into arena using bytes_pack()
Bytes eip_header = enip_eip_pack_header(arena, &hdr);

// 3. Wrap CPF payload with bytes_concat()
Bytes full_request = bytes_concat(arena, eip_header, cpf_payload);
```

**Function Signatures:**

```c
/* Parse EIP header from response using bytes_unpack (no direct byte access) */
int enip_eip_parse_header(Bytes response, enip_eip_header_t *out_header);

/* Pack EIP header into arena using bytes_pack */
Bytes enip_eip_pack_header(Arena *arena, const enip_eip_header_t *header);

/* Build complete EIP request by wrapping CPF payload and incrementing context */
Bytes enip_eip_build_request(Arena *arena, uint32_t session, uint64_t *context_inout, Bytes cpf_payload);

/* Extract CPF payload from response by skipping 24-byte header */
Bytes enip_eip_extract_cpf_payload(Bytes response);
```

### Response Parsing

```c
// Extract CPF payload from EIP response (skips 24-byte header)
Bytes cpf_frame = enip_eip_extract_cpf_payload(eip_response);
// cpf_frame is a slice starting at byte offset 24
```

### Key Constants

```c
#define ENIP_EIP_HEADER_SIZE 24
#define ENIP_CMD_REGISTER_SESSION 0x0065
#define ENIP_CMD_UNCONNECTED_SEND 0x006F    /* SendRRData (unconnected explicit) */
#define ENIP_CMD_CONNECTED_SEND 0x0070      /* SendUnitData (connected) */
```

### EIP Header Layout (24 bytes, little-endian)

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0 | 2 | command | ENIP operation code |
| 2 | 2 | length | Payload size (excluding 24-byte header) |
| 4 | 4 | session_handle | Session ID from RegisterSession |
| 8 | 4 | status | 0 in requests, error code in responses |
| 12 | 8 | sender_context | Request-response correlation token (incremented for routing) |
| 20 | 4 | options | Reserved, always 0 |

---

## Layer 2: CPF (Common Packet Format)

**File:** `src/libplctag/protocols/enip/enip_cpf.h/c`

### Purpose
Implements CPF item framing that wraps the CIP payload in unconnected or connected message format.

### Request Building (Unconnected)

```c
// 1. Create CPF header (interface_handle=0, router_timeout=0, item_count=2)
Bytes cpf_hdr = enip_cpf_pack_header(arena, 0, 0, 2);

// 2. Create NAI item (null address)
Bytes nai = enip_cpf_pack_nai(arena);

// 3. Create UDI header with payload length
Bytes udi_hdr = enip_cpf_pack_udi_header(arena, cip_payload.len);

// 4. Compose complete frame using bytes_concat (no memcpy)
Bytes cpf_frame = bytes_concat(arena, cpf_hdr, nai, udi_hdr, cip_payload);
```

**Function Signatures:**

```c
/* Pack CPF header (interface_handle + router_timeout + item_count) */
Bytes enip_cpf_pack_header(Arena *arena, uint32_t interface_handle, uint16_t router_timeout, uint16_t item_count);

/* Pack NAI item header */
Bytes enip_cpf_pack_nai(Arena *arena);

/* Pack UDI item header with given payload length */
Bytes enip_cpf_pack_udi_header(Arena *arena, size_t payload_len);

/* Build complete unconnected CPF frame (header + NAI + UDI + CIP) */
Bytes enip_cpf_build_unconnected(Arena *arena, Bytes cip_payload);

/* Build connected CPF frame (NOT YET IMPLEMENTED) */
Bytes enip_cpf_build_connected(Arena *arena, uint32_t connection_id, uint16_t seq_num, Bytes cip_payload);

/* Parse CPF header from frame using bytes_unpack (no direct byte access) */
int enip_cpf_parse_header(Bytes cpf_frame, enip_cpf_header_t *out_header);

/* Extract CIP payload from unconnected response by finding UDI item */
Bytes enip_cpf_extract_udi_payload(Bytes cpf_frame);
```

### Response Parsing

```c
// Find UDI item and extract CIP payload
Bytes cip_payload = enip_cpf_extract_udi_payload(cpf_frame);
// cip_payload is a zero-copy slice pointing into cpf_frame
```

The parser iterates CPF items using `bytes_unpack()` for item headers and returns a slice of the UDI payload.

### Key Constants

```c
#define ENIP_CPF_ITEM_NAI 0x0000           /* Null Address Item */
#define ENIP_CPF_ITEM_CONN_ADDR 0x00A1     /* Connected Address Item */
#define ENIP_CPF_ITEM_CONN_DATA 0x00B1     /* Connected Data Item */
#define ENIP_CPF_ITEM_UCONN_DATA 0x00B2    /* Unconnected Data Item */

#define ENIP_CPF_HEADER_SIZE 8              /* iface_handle(4) + timeout(2) + item_count(2) */
#define ENIP_CPF_NAI_ITEM_SIZE 4            /* type(2) + length(2) */
#define ENIP_CPF_UDI_ITEM_HEADER_SIZE 4     /* type(2) + length(2) */
```

### CPF Unconnected Frame Layout

```
[CPF Header: 8 bytes] [NAI Item: 4 bytes] [UDI Header: 4 bytes] [CIP Payload: variable]
```

---

## Layer 3: CIP (Common Industrial Protocol)

**File:** Implemented via manufacturer strategy modules (`enip_mfg_*.c`)

### Purpose
Encodes/decodes service-specific requests and responses. Varies by PLC vendor (AB, OMRON, PCCC).

### Budget-Based Encoding

The manufacturer strategy functions receive a **CIP payload budget** — a Bytes struct representing the maximum allowable CIP packet size. As the strategy encodes the CIP request, it advances the budget pointer and shrinks the remaining length:

```c
// Allocate CIP request buffer based on connection type
size_t cip_max = determine_cip_max_size(tag->connection_type);  // 502–3996 bytes
Bytes *cip_req_budget = allocate_cip_buffer(cip_max);

// Manufacturer strategy encodes CIP request, advancing budget pointer
struct enip_mfg_ops *mfg = select_mfg_ops(tag->device_identity);
struct req_desc req = mfg->encode_request(tag, cip_req_budget);  // Modifies cip_req_budget
if(bytes_is_null(req.payload)) { /* CIP budget exhausted */ }

// req.payload = CIP request data (pointer into cip_req_budget, length = bytes consumed)
Bytes cip_payload = req.payload;
```

**CIP Budget Sizes by Connection Type:**
- **Unconnected (all vendors):** 502 bytes (508 EIP max − 6 bytes CPF data item header)
- **Connected (AB old/new):** 3996 bytes (4002 EIP max − 6 bytes CPF data item header)
- **Connected (OMRON):** 1886 bytes (1892 EIP max − 6 bytes CPF data item header)

### Request Building

Once CIP payload is encoded within its budget, the shared code wraps it with CPF/EIP overhead, all from the same arena:

```c
// All allocations from single arena (32–64 kB for connection)
Arena *arena = &conn->arena;

// CPF/EIP overhead
Bytes eip_header = enip_eip_pack_header(arena, &hdr);
Bytes cpf_header = enip_cpf_pack_header(arena, 0, 0, 2);
Bytes nai = enip_cpf_pack_nai(arena);
Bytes udi_header = enip_cpf_pack_udi_header(arena, cip_payload.len);

// CIP payload: from budget (already allocated from same arena)
// Total frame = bytes_concat(arena, eip_header, cpf_header, nai, udi_header, cip_payload)
// Send frame to socket
```

### Response Parsing

When response arrives, the shared code extracts CIP and passes it to manufacturer with a **CIP response budget**:

```c
// Layer 1: Extract CPF from EIP response (zero-copy slice, skip 24-byte header)
Bytes cpf_frame = enip_eip_extract_cpf_payload(eip_response);

// Layer 2: Extract CIP from CPF (zero-copy slice, find UDI item)
Bytes cip_response = enip_cpf_extract_udi_payload(cpf_frame);

// Layer 3: Create response budget and decode via manufacturer handler
Bytes *cip_resp_budget = (Bytes *)&cip_response;  // Points into response buffer
struct mfg_ops *mfg = get_connection_mfg_ops();
struct chunk_result result = mfg->decode_response(tag, cip_resp_budget);  // Modifies cip_resp_budget
```

---

## Budget Allocation Model

### Request-Side Budget

The **CIP request budget** is a fixed-size Bytes buffer allocated from the connection arena based on connection type:

```c
// Determine max CIP payload based on connection type and vendor
size_t cip_max_size = 502;  // Unconnected (default)
if (tag->connection_type == ENIP_CONN_TYPE_CONNECTED) {
    if (tag->vendor == VENDOR_AB) {
        cip_max_size = 3996;  // AB with Forward Open Extended/Long (4002 − 6 CPF header)
    } else if (tag->vendor == VENDOR_OMRON) {
        cip_max_size = 1886;  // OMRON with Extended (1892 − 6 CPF header)
    }
}

// Allocate CIP request buffer from arena (usable portion after CPF/EIP overhead ~50 bytes)
Bytes cip_req_budget = arena_allocate(&conn->arena, cip_max_size);

// Pass budget to manufacturer strategy
struct req_desc req = mfg->encode_request(tag, &cip_req_budget);
// After encode, cip_req_budget.ptr has advanced, cip_req_budget.len shows remaining space
// req.payload = the CIP data actually encoded (may be less than cip_max_size)
```

### Response-Side Budget

The **CIP response budget** is extracted from the received packet:

```c
// Parse response layers to extract CIP portion
Bytes cpf_frame = enip_eip_extract_cpf_payload(eip_response);
Bytes cip_response_buffer = enip_cpf_extract_udi_payload(cpf_frame);

// Create response budget pointing into the extracted CIP data
Bytes cip_resp_budget = cip_response_buffer;  // Full CIP response data

// Pass budget to manufacturer strategy for decoding
struct chunk_result result = mfg->decode_response(tag, &cip_resp_budget);
// After decode, cip_resp_budget.ptr has advanced, cip_resp_budget.len shows unconsumed data
```

### CPF/EIP Overhead (Same Arena)

The CPF header, address item, and data item header are allocated from the same arena as the CIP buffer:

```c
// Single arena for entire connection (32–64 kB)
Arena *arena = &conn->arena;

// Reset arena at start of request (clears previous request state)
arena_reset(arena);

// EIP overhead: 24 bytes
Bytes eip_header = enip_eip_pack_header(arena, &hdr);

// CPF overhead: 8 bytes header + 4 bytes NAI + 4 bytes UDI header = 16 bytes
Bytes cpf_header = enip_cpf_pack_header(arena, 0, 0, 2);
Bytes nai = enip_cpf_pack_nai(arena);
Bytes udi_header = enip_cpf_pack_udi_header(arena, cip_payload.len);

// CIP payload: also from same arena (allocated by encode_request)
// Total frame = bytes_concat(arena, eip_header, cpf_header, nai, udi_header, cip_payload)
//             = 24 + 8 + 4 + 4 + (502–3996) = 542–4036 bytes total
//             = Always within 32–64 kB arena capacity
```

---

## Complete Example: Request → Response Flow

### Request Phase (Inside-Out with Budgets)

```
1. CIP Request Budget Allocation
   Allocate cip_req_budget[502–3996] based on connection type
   ↓
2. CIP Service Request (from mfg_ops)
   mfg->encode_request(tag, &cip_req_budget) → advances budget.ptr, shrinks budget.len
   ↓
3. CPF/EIP Overhead (arena allocated)
   Pack EIP header (24 bytes)
   Pack CPF header + NAI + UDI header (16 bytes)
   ↓
4. Frame Composition
   bytes_concat(arena, eip_hdr, cpf_hdr, nai, udi_hdr, cip_payload)
   ↓
5. Transmission
   send(socket, frame_data, frame_len)
Network
```

### Response Phase (Outside-In with Budgets)

```
Network
   ↓ recv(socket, response_buffer, max_len)
1. EIP Header Extraction
   enip_eip_extract_cpf_payload() → bytes_slice(response, 24, ...)
   ↓
2. CPF Header & Item Parsing
   enip_cpf_extract_udi_payload() → finds UDI item via bytes_unpack(), returns payload slice
   ↓
3. CIP Response Budget
   Create Bytes cip_resp_budget from extracted CIP slice
   ↓
4. CIP Decoding (from mfg_ops)
   mfg->decode_response(tag, &cip_resp_budget) → advances budget.ptr, shrinks budget.len
   ↓
5. Tag Updated
   Copy decoded values to tag->data[], tag->status = PLCTAG_STATUS_OK
```

---

## Memory Efficiency

### Single Arena Model

All root allocations (CIP buffer, CPF/EIP headers, items) come from a single connection arena:

```c
// Per-connection arena (32–64 kB)
struct {
    Arena arena;
    uint8_t buffer[65536];
} conn;

// Reset arena at start of each request
arena_reset(&conn->arena);

// All allocations now use same arena
Bytes cip_budget = arena_allocate(&conn->arena, cip_max_size);
Bytes eip_hdr = enip_eip_pack_header(&conn->arena, &hdr);
Bytes cpf = enip_cpf_build_unconnected(&conn->arena, cip_payload);
```

### Request Building
- **Single arena:** 32–64 kB per connection, reset at start of each request
- **CIP buffer allocation:** Calculated per connection type, allocated from arena
- **CPF/EIP overhead:** Allocated from same arena (~50 bytes)
- **No intermediate copies:** `bytes_pack()` and `bytes_concat()` place data directly into arena
- **Budget tracking:** Manufacturer strategy advances CIP pointer to show space consumed
- **Sequential requests:** Only one request in flight at a time; arena reset ensures isolation

### Response Parsing
- **Zero-copy slicing:** `bytes_slice()` returns pointer + length from received buffer
- **Response budget:** Extracted CIP slice creates budget without copying data
- **Efficient iteration:** CPF item parsing uses `bytes_unpack()` with advancing position
- **Stack-friendly:** No heap allocations for extraction; slices reference received buffer
- **Manufacturer tracking:** Strategy advances pointer to show bytes consumed

---

## Bytes API Usage

All serialization and deserialization uses type-safe Bytes API:

### bytes_pack() — Request Building

```c
// Pack 24-byte EIP header
Bytes eip_hdr = bytes_pack(arena, BYTES_LE,
    header->command,
    header->length,
    header->session_handle,
    header->status,
    header->sender_context,
    header->options);
```

### bytes_unpack() — Response Parsing

```c
// Extract EIP header from response
Bytes rest = bytes_unpack(response, BYTES_LE,
    &out_header->command,
    &out_header->length,
    &out_header->session_handle,
    &out_header->status,
    &out_header->sender_context,
    &out_header->options);
```

### bytes_concat() — Composition

```c
// Combine EIP header + CPF payload into single frame
Bytes full_request = bytes_concat(arena, eip_header, cpf_payload);
```

### bytes_slice() — Zero-Copy Extraction

```c
// Extract CPF portion (skip 24-byte EIP header)
Bytes cpf_frame = bytes_slice(response, 24, response.len - 24);
// cpf_frame points into response, no copy
```

---

## Testing

Each layer can be tested independently:

```c
// Test EIP layer
Bytes eip_request = enip_eip_build_request(arena, session, &context, test_cpf);
enip_eip_header_t hdr;
enip_eip_parse_header(eip_request, &hdr);
assert_equal(hdr.command, ENIP_CMD_UNCONNECTED_SEND);

// Test CPF layer
Bytes cpf_frame = enip_cpf_build_unconnected(arena, test_cip);
Bytes extracted_cip = enip_cpf_extract_udi_payload(cpf_frame);
assert_bytes_equal(extracted_cip, test_cip);

// Test integration
Bytes full_request = enip_eip_build_request(arena, session, &context, 
                                            enip_cpf_build_unconnected(arena, cip_payload));
// Decode
Bytes cpf = enip_eip_extract_cpf_payload(full_request);
Bytes cip = enip_cpf_extract_udi_payload(cpf);
assert_bytes_equal(cip, cip_payload);
```

---

## Design Principles

1. **All root allocation from arena:** CIP budget, CPF/EIP headers, and all intermediate data come from single per-connection arena (32–64 kB)
2. **Single request in flight:** No pipelining; arena reset at start of each request ensures clean isolation
3. **No raw byte manipulation:** All field access via `bytes_pack` / `bytes_unpack`
4. **No intermediate copies:** Direct `bytes_concat()` composition and slice-based extraction
5. **Type safety:** Endianness and type specified at compile-time via Bytes API macros
6. **Composability:** Each layer's output is another layer's input; CIP budget passed to manufacturer strategy
7. **Zero-copy responses:** All response extraction uses `bytes_slice()` for efficiency
8. **Manufacturer isolation:** CIP payload creation/parsing delegated to `mfg_ops` callbacks with budget tracking
9. **Budget transparency:** Manufacturer advances budget.ptr/.len as it encodes/decodes CIP data
10. **Bounded memory:** 32–64 kB arena sufficient for all requests (max 4036 bytes + overhead)

---

## Future Work

- [ ] Implement `enip_cpf_build_connected()` for connected messaging (0x0070 SendUnitData)
- [ ] Add connected address item packing and sequence number handling
- [ ] Optimize arena sizing based on typical message patterns
- [ ] Add comprehensive layer unit tests with mocked CPF/CIP payloads

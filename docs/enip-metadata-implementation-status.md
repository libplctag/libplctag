> **DEPRECATED.** This document predates the current EtherNet/IP design and is
> retained for historical reference only. The authoritative design is
> [ENIP-SESSION-DESIGN.md](../src/libplctag/protocols/enip/ENIP-SESSION-DESIGN.md). Do not use this document to guide new work.

# ENIP Metadata Implementation Status

## Overview

The ENIP metadata system implements a two-phase, connection-wide caching strategy to support efficient tag operations. Phase-1 retrieves root symbol metadata from the PLC (name → instance_id); Phase-2 fetches detailed tag information on-demand (instance_id → element size, array dimensions, UDT structure).

## Architecture

### Phase-1: Root Symbol Inventory
- **Scope:** Connection-wide, fetched once during setup
- **Key:** First symbolic segment of tag path (e.g., "myTag" from "myTag[0].field")
- **Value:** Instance ID (unique identifier from PLC)
- **Storage:** Connection hashtable `conn->metadata_hashtable`
- **Trigger:** Executed during connection setup after RegisterSession and GetIdentity
- **Result:** Populates metadata for all root symbols available on PLC

### Phase-2: Tag-Specific Metadata
- **Scope:** Per-tag, lazy-loaded on first operation
- **Trigger:** When tag reads/writes for first time (gated by `metadata_phase2_ready` flag)
- **Input:** Tag's `tag_instance_id` (from Phase-1)
- **Output:** Element size, array dimensions, UDT structure details
- **Cache Key:** `(int64_t)tag_instance_id` (direct numeric key, no hashing)
- **Special Case:** PCCC tags skip Phase-2; element size derived from file type prefix

## Tag Creation & Lifecycle

### Single Allocation Strategy
Tag creation encodes the full tag path and allocates storage in one operation:

```c
size_t total_size = sizeof(enip_tag_t) + encoded_len + tag_name_len + 1;
enip_tag_t *tag = rc_alloc(total_size, enip_tag_destructor);
```

All data stored contiguously after the tag structure:
1. **Encoded tag path** (ANSI CIP format, binary)
2. **Root tag name** (null-terminated string, e.g., "myTag")

### Tag Fields
| Field | Type | Purpose | Set When |
|-------|------|---------|----------|
| `tag_name` | `char*` | Root symbol name (first segment) | Tag creation |
| `tag_instance_id` | `uint32_t` | PLC instance ID | Phase-1 metadata arrives |
| `encoded_tag_path` | `uint8_t*` | Full path in CIP ANSI format | Tag creation |
| `encoded_tag_path_len` | `size_t` | Length of encoded path | Tag creation |
| `metadata_phase2_ready` | `int` | Flag: detailed metadata available | Phase-2 complete or skipped |

### Root Tag Name Extraction
During tag creation, the root tag name is extracted from the full tag path:
- Input: `"myTag[0].field.subfield"` (from `attribs["name"]`)
- Output: `"myTag"` (stored in `tag->tag_name`)
- Algorithm: Extract characters until first `[` or `.`

## Hashtable Convergence

Tag metadata hashtable entries are created when **either** of two events occurs:

**Pathway A: Tag Created First**
1. Application creates tag with root name "myTag"
2. Tag creation code adds entry: `hashtable["myTag"] = {instance_id=0, ...}`
3. Phase-1 metadata arrives later, updates instance_id in existing entry

**Pathway B: Phase-1 Metadata Arrives First**
1. Phase-1 metadata fetch returns: `{"myTag": instance_id=42, ...}`
2. Metadata handler creates entry: `hashtable["myTag"] = {instance_id=42, ...}`
3. Future tag creation looks up and uses instance_id from existing entry

Both pathways converge to the same hashtable state regardless of order.

## Manufacturer-Specific Behavior

### Allen-Bradley (EtherNet/IP)
- **Phase-2 Gate:** Check `metadata_phase2_ready`; if false, call `enip_metadata_fetch_tag_info(conn, tag->tag_instance_id, ...)`
- **Blocking:** Tag waits (`PLCTAG_STATUS_PENDING`) until Phase-2 metadata available
- **Cache Lookup:** Uses `tag_instance_id` as cache key

### Omron
- **Phase-2 Gate:** Identical to AB strategy
- **Blocking:** Same pending behavior
- **Cache Lookup:** Uses `tag_instance_id` as cache key

### PCCC (PLC-5, SLC-5/05)
- **Phase-2 Gate:** Skipped; element size derived immediately
- **Implementation:**
  ```c
  char file_type = tag->tag_name[0];  // e.g., 'N' from "N7:0"
  if(file_type == 'F') elem_size = 4;      // 32-bit float
  else if(file_type in {'N','I','L','S','R','B','T','C'}) elem_size = 2;  // 16-bit
  else if(tag->tag_name == "ST") elem_size = 84;  // STRING (special case)
  tag->elem_size = elem_size;
  tag->elem_count = tag->size / elem_size;
  tag->metadata_phase2_ready = 1;
  ```
- **No Metadata Fetch:** PCCC does not support PLC metadata queries; all information inferred from protocol

## Implementation Status

### ✅ Completed
- **enip_tag.h:** Tag structure with `tag_name`, `tag_instance_id`, `encoded_tag_path`, `encoded_tag_path_len` fields
- **enip_tag.c:** Tag creation with root name extraction and single allocation (tag + encoded path + name string)
- **enip_metadata.c:** Cache refactored from tag_name-based to instance_id-based lookup
  - `enip_metadata_tag_key()`: Returns `(int64_t)tag_instance_id`
  - `enip_metadata_get_cached()`: Takes `uint32_t tag_instance_id` parameter
  - `enip_metadata_set_cached()`: Takes `uint32_t tag_instance_id` parameter
  - `enip_metadata_fetch_tag_info()`: Signature updated to use `uint32_t tag_instance_id`
- **Manufacturer Implementations:** All three manufacturers updated with Phase-2 gates
  - AB & Omron: Full gate logic with fetch calls
  - PCCC: Direct element size calculation, no fetch
- **Build:** Clean compilation; `[100%] Built target plctag_dyn`

### ⚠️ Pending
- **Phase-1 Metadata Fetch:** Implement `enip_metadata_fetch_root_symbols()` to query all root tag names and populate hashtable
- **Hashtable Population:** Create connections between tag instances and instance_ids during Phase-1
- **Pending Requests Vector:** Wire tag into `conn->pending_requests` on successful encode; remove on response
- **Sender Context Matching:** Call `extract_sender_context()`, `find_pending_request()` in main loop response handler
- **Phase-2 Fetch Implementation:** Fill out `enip_metadata_fetch_tag_info()` body to query detailed metadata (element size, array dims, UDT structure)
- **Response Decode:** Implement `enip_mfg_*_decode_response()` to extract CIP response and populate tag data

## Data Flow Example

**Scenario:** Create tag "myTag[0].field" on Allen-Bradley PLC

1. **Tag Creation:**
   - Extract root name: "myTag"
   - Encode path: `myTag[0].field` → ANSI CIP bytes
   - Allocate: `tag = rc_alloc(sizeof(enip_tag_t) + encoded_len + 6)`
   - Store: `tag->tag_name = "myTag"`, `tag->tag_instance_id = 0`

2. **Phase-1 Metadata (connection setup):**
   - Fetch: Query all root symbols from PLC
   - Response: `{..., "myTag": {instance_id: 42}, ...}`
   - Hashtable: `metadata_hashtable["myTag"] = {instance_id: 42, elem_size: ?, array_dims: ?}`
   - Tag Update: `tag->tag_instance_id = 42`

3. **First Read Operation:**
   - Gate Check: `if(!tag->metadata_phase2_ready)`
   - Phase-2 Fetch: `enip_metadata_fetch_tag_info(conn, 42, ...)`
   - Response: `{elem_size: 4, array_dims: [10], ...}`
   - Cache: `metadata_hashtable[42] = {elem_size: 4, ...}`
   - Tag Update: `tag->metadata_phase2_ready = 1`
   - Read Proceeds: Send read request using element size and array info

## Key Implementation Details

### Cache Key Strategy
- **Phase-1 Hashtable:** Key = root tag name (string), Value = metadata with instance_id
- **Phase-2 Cache:** Key = instance_id (uint32_t cast to int64_t), Value = element size, array dimensions, UDT info

### Matching on First Segment Only
- Tag "myTag[0].field.subfield" matches "myTag" in hashtable
- Array indices and member navigation ignored for initial lookup
- Instance ID identifies complete tag structure on PLC

### PCCC Exception
- No PLC metadata available; hardcoded file type mappings
- Element size inferred from first character of tag name ('N'=2 bytes, 'F'=4 bytes, 'ST'=84 bytes)
- Supports backward compatibility with legacy PLC-5 and SLC-5/05 systems

## Testing Notes

- Build succeeds with all changes integrated
- Metadata hashtable initialization and cleanup verified in connection lifecycle
- Sender context extraction functions available for pending request tracking
- PCCC file type parsing tested for common file types (N, F, B, T, C, I, L, S, R)

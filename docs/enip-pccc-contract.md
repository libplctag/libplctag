# ENIP+PCCC Contract

Date: 2026-05-28
Status: Normative behavior contract for ENIP+PCCC work

## 1. Scope

This contract covers:
1. Logix-over-PCCC (Execute PCCC through Logix path).
2. Direct PLC5 ENIP PCCC.
3. Direct SLC500/MicroLogix ENIP PCCC.
4. DH+ bridged PLC5 and SLC/MicroLogix PCCC.

## 2. Manufacturer and Strategy Isolation

1. ENIP+PCCC logic stays in a dedicated strategy path.
2. Generic ENIP scheduler and packetizer provide only budgets and generic request lifecycle.
3. PCCC address stepping, transfer-size fields, and response-length rules are owned by the ENIP+PCCC strategy.

## 3. Current Behavioral Facts (Locked)

1. Existing PCCC implementations treat each read/write request as single-packet and reject oversize operations.
2. Existing status handlers assume one response payload for one request and validate tight size expectations.
3. Bit writes are read-modify-write style masked operations and are per-element operations.
4. Logix-over-PCCC has typed read/write but no dedicated bit-write masked function path.

## 4. Chunking Contract

Chunking is manual request slicing, not protocol fragmentation.

Rules:
1. Do not introduce fragmented CIP services for PCCC paths.
2. Split large reads and writes into multiple legal range requests.
3. Keep one outstanding chunk request per tag.
4. Advance logical file element index per chunk.
5. Preserve strict status validation on each chunk before advancing.

Chunk math by operation type:
1. Read chunk upper bound is response_data_budget.
2. Write chunk upper bound is request_data_budget.
3. chunk_bytes = floor(chunk_upper_bound / elem_size_bytes) * elem_size_bytes.
4. If chunk_bytes < elem_size_bytes, return too large.
5. Optional safety check: validate the non-dominant side still fits, and reduce chunk_bytes if needed.

Progression:
1. bytes_done starts at 0.
2. chunk_elem_index = base_elem + (bytes_done / elem_size_bytes).
3. Build request for chunk_elem_index and chunk transfer size.
4. On success, bytes_done += chunk_bytes_sent_or_received.

## 5. PLC-Type-Specific Chunking Rules

### 5.1 PLC5 Direct

1. Transfer fields are word-oriented for range read/write functions.
2. Chunk bytes must map cleanly to word count where required.
3. Address re-encoding by element index is the primary chunk progression method.

### 5.2 SLC500/MicroLogix Direct

1. Transfer fields are byte-oriented for range read/write functions.
2. Address re-encoding by element index is the primary chunk progression method.

### 5.3 Logix-over-PCCC

1. Typed read/write request shape remains Logix-over-PCCC specific.
2. Chunking uses request repetition with adjusted logical element progression.
3. PCCC offset field is optional for first implementation and may remain zero initially.

### 5.4 DH+ Bridged

1. Same PCCC data semantics as direct PLC5/SLC paths.
2. Additional DH+ routing header bytes reduce available payload budgets.
3. Chunking loop is identical aside from overhead calculation and DH+ framing.

## 6. Bit Write Contract Per PLC Type

1. PLC5 direct bit write uses PLC5 read-modify-write masked operation and is not chunked.
2. SLC/MicroLogix direct bit write uses SLC masked write function and is not chunked.
3. PLC5 DH+ bit write uses PLC5-style masked operation with DH+ header and is not chunked.
4. SLC/MicroLogix DH+ bit write uses SLC-style masked operation with DH+ header and is not chunked.
5. Logix-over-PCCC currently has no dedicated bit-write masked function path; use word/element write semantics for now.

## 7. Error and Retry Contract

1. Any chunk failure fails the operation unless retry policy explicitly retries that chunk.
2. Retry, if used, retries current chunk only.
3. No speculative advancement of element index on failed chunk.
4. Response size mismatch per chunk is a hard error.

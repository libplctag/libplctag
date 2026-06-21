> **DEPRECATED.** This document predates the current EtherNet/IP design and is
> retained for historical reference only. The authoritative design is
> [ENIP-SESSION-DESIGN.md](../src/libplctag/protocols/enip/ENIP-SESSION-DESIGN.md). Do not use this document to guide new work.

# ENIP+PCCC Test Plan

Date: 2026-05-28
Status: Focused validation plan for ENIP+PCCC chunking and bit writes

## 1. Objectives

1. Validate chunked read/write correctness for direct and DH+ PCCC paths.
2. Validate bit-write behavior per PLC type.
3. Validate failure handling and chunk retry behavior.

## 2. Read Tests

1. PLC5 direct read single-packet fit.
2. PLC5 direct read multi-chunk over packet threshold.
3. SLC/MicroLogix direct read single-packet fit.
4. SLC/MicroLogix direct read multi-chunk.
5. Logix-over-PCCC read single-packet fit.
6. Logix-over-PCCC read multi-chunk.
7. PLC5 DH+ read multi-chunk.
8. SLC DH+ read multi-chunk.

Checks:
1. Final byte image equals expected source.
2. Chunk boundaries preserve ordering.
3. No off-by-one element jumps.

## 3. Write Tests

1. PLC5 direct write single-packet fit.
2. PLC5 direct write multi-chunk.
3. SLC/MicroLogix direct write single-packet fit.
4. SLC/MicroLogix direct write multi-chunk.
5. Logix-over-PCCC write single-packet fit.
6. Logix-over-PCCC write multi-chunk.
7. PLC5 DH+ write multi-chunk.
8. SLC DH+ write multi-chunk.

Checks:
1. Device memory matches payload exactly.
2. Chunk ordering is monotonic and complete.
3. Chunk retries do not duplicate committed writes.

## 4. Bit Write Tests Per PLC Type

1. PLC5 direct bit write set and clear paths.
2. SLC/MicroLogix direct bit write set and clear paths.
3. PLC5 DH+ bit write set and clear paths.
4. SLC/MicroLogix DH+ bit write set and clear paths.
5. Logix-over-PCCC bit write request behavior:
   1. Verify there is no dedicated masked-bit path.
   2. Verify fallback behavior is explicit and documented.

Checks:
1. Only target bit changes.
2. Neighbor bits remain unchanged.
3. Invalid size constraints are rejected correctly for SLC bit mask path.

## 5. Error and Edge Tests

1. Oversized chunk request rejected before send.
2. Response shorter than expected chunk data.
3. Response larger than expected chunk data.
4. Mid-sequence PLC status error.
5. Mid-sequence transport error with retry policy enabled.
6. Retry current chunk only after transient error.

Checks:
1. Final status code is deterministic.
2. Partial progress tracking is correct.
3. No silent truncation.

## 6. Regression Safeguards

1. Existing non-chunked operations still pass.
2. Existing direct bit-write semantics unchanged.
3. Existing DH+ routing behavior unchanged except chunk loop progression.

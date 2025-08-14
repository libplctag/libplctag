# Rockwell PLC Boolean Array Test Log Reference

This document summarizes the handling of boolean arrays by a real Rockwell PLC, based on the provided test log files. Each section describes the behavior and protocol details for a specific test log.

---

## test_1.log: Read a single DINT from TestBigArray.
- No index
- elem_count=1

---

## test_2.log: Write a single DINT from TestBigArray.
- No index
- elem_count=1
- new value=42

---

## test_3.log: Read a single DINT from TestBigArray[4]
- index is 4
- elem_count=1

---

## test_4.log: Read a single bit from TestBigArray[432]
- element index is 432
- bit tag with the bit index=1
- elem_count=1

## test_5.log: Write a single bit from TestBigArray[432]
- element index is 432
- bit index is 1.
- Different CIP command->0x4e, Read/Modify/Write
- OR and AND masks, each 32-bits

---

## test_6.log: Large read of from TestBigArray
- element index is 0,
- element count is 1000
- Notice multiple request packets with changing offsets.
- Notice PLC response with CIP status 0x06 (partial result)

---

## test_7.log: Tag listing of the PLC
- packets first
- Then processed tag output.
- Then processed UDT definitions.
- Uses get attribute list of instances CIP command 0x55.
- Start sending that to the class 0x6B.
- Start with instance ID 0x0000.  Looks like it has to be in 16-bit form.

---

## test_8.log: Reading the Entire 512-BOOL Array
- The PLC now returns all 512 values from the BOOL array.
- All values are sent and retrieved as 32-bit bit strings (CIP type 0xD3).
- The array is packed: 512 bits = 16 x 32-bit elements.
- Each 32-bit element represents 32 boolean values (bits).

---

## test_9.log: Writing a Single Boolean Early in the Array
- The read operation shows the value at index 10, but this is the 11th bit in the first 32-bit element.
- The write operation targets element index 10, which is the 11th bit of the first packed element.
- The value written is still encoded as CIP type 0xd3 (32-bit).
- The PLC expects writes to be packed into the correct bit position within the 32-bit element.

---

## test_10.log: Writing a Single Boolean Toward the End of the Array
- The encoding for the index uses the 0x29 numeric segment type, indicating a specific bit index.
- The PLC still returns a 0xd3 type for the value.
- The lowest bit of the returned value is the boolean value for the indexed bit.

---

## test_11.log: Reading, Writing, and Reading Two Elements of the Boolean Array
- The test reads two elements of the boolean array, writes to them, and then reads them again.
- Data is passed as two consecutive 32-bit CIP 0xd3 types, each representing 32 packed booleans.
- The PLC protocol allows reading and writing multiple packed elements at once.

---

## test_12.log: Reading 16 Consecutive Values Starting at Index 0
- The test reads 16 consecutive values from the boolean array, starting at index 0.
- Each value is a 32-bit CIP 0xd3 type, representing 32 packed booleans.
- The log shows that index 10 is set (bit 10 is 1), reflecting a previous write operation.
- The returned data array shows only the 11th bit (index 10) set, all others are zero.

---

## Key Points for Boolean Array Handling
- **CIP Type 0xd3**: Used for packed 32-bit boolean arrays. Each element is a 32-bit integer, with each bit representing a boolean value.
- **Indexing**: When accessing a single boolean, the PLC returns a 32-bit value (0xd3), with the target boolean in the lowest bit.
- **Bit Packing**: Writes and reads are performed on packed 32-bit elements. The bit index determines the position within the element.
- **Segment Types**: Numeric segment type 0x29 is used for bit-level indexing in the array.

---

## Practical Implications
- When reading or writing a boolean array, always consider the packing: 32 booleans per 32-bit element.
- Single boolean accesses will return a 32-bit value, with the relevant bit in the lowest position.
- Writing to a boolean array requires setting the correct bit in the packed element.
- The PLC protocol uses specific segment types for bit-level access.
- Reading multiple elements returns packed 32-bit values for each requested segment.

---

This summary helps keep track of the meaning and structure of the test log files for Rockwell PLC boolean array handling.

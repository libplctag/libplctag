# Example Tag Strings

Real tag attribute strings pulled from the example programs (`src/examples/`),
test programs (`src/tests/`), the hardware test script
(`src/tests/scripts/run_hardware_tests.sh`), and the simulator test runner
(`src/tests/scripts/run_simulator_tests_parallel.py`). IP addresses are
whatever the original author's lab used — replace `gateway=` with your own
PLC's address.

Full attribute reference: [Tag-String-Attributes wiki page](https://github.com/libplctag/libplctag/wiki/Tag-String-Attributes),
[All-Attributes.asciidoc](https://github.com/libplctag/libplctag/wiki/All-Attributes),
[Special-Tags wiki page](https://github.com/libplctag/libplctag/wiki/Special-Tags).

---

## 1. Regular (scalar/simple) tags

### ControlLogix / CompactLogix DINT tag

```
protocol=ab_eip&gateway=127.0.0.1&path=1,0&cpu=LGX&elem_size=4&elem_count=1&name=TestDINTArray[0]
```
Source: `src/examples/multithread.c`

| Attribute | Meaning |
|---|---|
| `protocol=ab_eip` | Allen-Bradley EIP/CIP protocol (`ab_eip`/`ab-eip` are equivalent). |
| `gateway` | IP (optionally `host:port`) of the PLC or the router used to reach it. |
| `path=1,0` | CIP routing path from the EtherNet/IP module to the CPU: port 1 (backplane), slot 0 (CPU slot in the chassis). |
| `cpu=LGX` | PLC family; `LGX`/`lgx`/`logix`/`controllogix` are equivalent, deprecated in favor of `plc=`. |
| `elem_size=4` | Bytes per element (4 for DINT). Not required on Logix-class PLCs since v2.4.0 but harmless to include. |
| `elem_count=1` | Number of array elements to read/write as one tag (1 = scalar). |
| `name=TestDINTArray[0]` | Tag name; `[0]` selects a single element of an array tag. |

### PLC/5

```
protocol=ab_eip&gateway=10.206.1.38&cpu=PLC5&elem_count=5&name=F8:10
```
Source: `src/examples/plc5.c`

| Attribute | Meaning |
|---|---|
| `cpu=PLC5` | Tells the library to speak PCCC to a PLC/5. Preferred spelling is `plc=plc5`. |
| `name=F8:10` | PCCC address: file type `F` (floating point), file 8, element 10. |
| `elem_count=5` | Reads 5 consecutive floats starting at F8:10. |
| _no `path`_ | `path` is only needed when routing through a bridge/DHRIO module. |

### SLC 500

```
protocol=ab_eip&gateway=10.206.1.26&cpu=SLC&elem_size=2&elem_count=1&name=N7:0&debug=1
```
Source: `src/examples/slc500.c`

| Attribute | Meaning |
|---|---|
| `cpu=SLC` | SLC 500 (PCCC-based), preferred spelling `plc=slc500`. |
| `name=N7:0` | PCCC address: `N` = integer file, file 7, element 0. |
| `elem_size=2` | INT elements are 2 bytes. |
| `debug=1` | Minimal (error-only) debug logging; see debug levels below. |

### MicroLogix

```
protocol=ab-eip&gateway=10.206.1.36&plc=micrologix&name=B3:0
```
```
protocol=ab-eip&gateway=10.206.1.36&plc=micrologix&name=B3:0/6
```
```
protocol=ab-eip&gateway=10.206.1.36&plc=micrologix&elem_count=4&name=L10:0
```
Source: `src/tests/scripts/run_hardware_tests.sh`

| Attribute | Meaning |
|---|---|
| `plc=micrologix` | PCCC MicroLogix family. |
| `name=B3:0` | Binary/bit file 3, word 0 (whole word). |
| `name=B3:0/6` | Same word, bit 6 — the `/n` suffix addresses a single bit inside a word. |
| `name=L10:0` | Long integer file 10, element 0 (32-bit). |
| `elem_count=4` | Read/write 4 consecutive LINT elements starting at L10:0. |

### Micro800

```
protocol=ab-eip&gateway=127.0.0.1:44818&plc=micro800&name=TestDINTArray
```
Source: `src/tests/scripts/run_simulator_tests_parallel.py`

| Attribute | Meaning |
|---|---|
| `plc=micro800` | Micro800-family CPU. **Must not** include a `path` attribute — Micro800 has no backplane routing. |

### Omron NJ/NX

```
protocol=ab-eip&gateway=10.206.1.30&path=18,127.0.0.1&plc=omron-njnx&name=TestDINTArray
```
Source: `src/tests/scripts/run_simulator_tests_parallel.py`

| Attribute | Meaning |
|---|---|
| `plc=omron-njnx` | Omron NJ/NX-series controller, speaks CIP like ControlLogix but with Omron-specific quirks. |
| `path=18,127.0.0.1` | CIP route: port type 18 (symbolic/backplane-equivalent for Omron) then the target unit's own address. |

### Modbus TCP

```
protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&elem_count=1&name=hr0
```
Source: `src/tests/test_reconnect/test_reconnect.c`

| Attribute | Meaning |
|---|---|
| `protocol=modbus-tcp` | Modbus over TCP (`modbus_tcp` also accepted). |
| `gateway=127.0.0.1:1502` | Host and TCP port (Modbus default is 502; simulators here use 1502). |
| `path=0` | Modbus unit/slave ID (0-255). |
| `name=hr0` | Register type prefix + address: `co`=coil, `di`=discrete input, `hr`=holding register, `ir`=input register. `hr0` = holding register 0. |
| `elem_count=1` | Number of consecutive registers. |

---

## 2. Arrays and multi-dimensional indexing

### Whole-array read

```
protocol=ab-eip&gateway=127.0.0.1&path=1,0&cpu=LGX&elem_count=10&name=TestBigArray
```
Source: `src/examples/simple.c`

`elem_count=10` with no index on `name` reads the first 10 elements of the array tag as one block.

### Single indexed element (1-D)

```
protocol=ab-eip&gateway=127.0.0.1&path=1,0&cpu=LGX&elem_count=1&name=Test_Array_1[%d]
```
Source: `src/tests/test_indexed_tags/test_indexed_tags.c` (`%d` filled in at runtime, e.g. `Test_Array_1[42]`)

### Whole 1-D array

```
protocol=ab-eip&gateway=127.0.0.1&path=1,0&cpu=LGX&elem_count=1000&name=Test_Array_1
```

### 2-D array, single element

```
protocol=ab-eip&gateway=127.0.0.1&path=1,0&cpu=LGX&elem_count=1&name=Test_Array_2x3[%d][%d]
```
e.g. `name=Test_Array_2x3[1][2]` — row/column indices, both zero-based, matching how the tag is dimensioned in the PLC.

### Whole 2-D array

```
protocol=ab-eip&gateway=127.0.0.1&path=1,0&cpu=LGX&elem_count=6&name=Test_Array_2x3
```
`elem_count` = total element count across all dimensions (2×3=6).

### 3-D array, single element / whole array

```
protocol=ab-eip&gateway=127.0.0.1&path=1,0&cpu=LGX&elem_count=1&name=Test_Array_2x3x4[%d][%d][%d]
protocol=ab-eip&gateway=127.0.0.1&path=1,0&cpu=LGX&elem_count=24&name=Test_Array_2x3x4
```
All from `src/tests/test_indexed_tags/test_indexed_tags.c`. `elem_count=24` = 2×3×4.

### Bit-in-array-element access

```
protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&name=TestINTArray[0].13
```
```
protocol=ab_eip&gateway=10.206.1.40&path=1,4&cpu=LGX&elem_size=4&elem_count=1&name=TestDINTArray[3].17
```
Sources: `src/tests/scripts/run_hardware_tests.sh`, `src/examples/toggle_bit.c`

`.n` after an array index addresses a single bit within that element (bit 13 of `TestINTArray[0]`, bit 17 of `TestDINTArray[3]`). Contrast with PCCC's `/n` bit suffix used on `B3:0`, `N7:0`, etc.

---

## 3. Strings

### Standard AB string (default Logix string layout)

```
protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=88&elem_count=11&name=barcodes
```
Source: `src/examples/string_standard.c`

Uses the library's built-in default string layout for Logix (`str_count_word_bytes=4`, `str_total_length=88`, etc. — no need to specify since they're the Logix defaults).

### Standard PLC/5 string

```
protocol=ab_eip&gateway=10.206.1.38&plc=plc5&elem_count=2&name=ST18:0
```
Source: `src/examples/string_standard.c`. `ST` = PCCC string file type.

### Non-standard / custom UDT string

```
protocol=ab-eip&gateway=10.206.1.39&path=1,0&plc=ControlLogix&elem_size=20&elem_count=228&name=CB_Rpt&str_count_word_bytes=4&str_is_byte_swapped=0&str_is_counted=1&str_is_fixed_length=1&str_is_zero_terminated=0&str_max_capacity=16&str_pad_bytes=0&str_total_length=20
```
Source: `src/examples/string_non_standard_udt.c` — required whenever a tag's string type isn't the built-in Rockwell `STRING` UDT (e.g. a user-defined UDT with a differently-sized character array).

| Attribute | Meaning |
|---|---|
| `str_count_word_bytes=4` | Leading length-count field is 4 bytes wide. |
| `str_is_byte_swapped=0` | Characters are not byte-swapped within 16-bit words. |
| `str_is_counted=1` | String has an explicit count word (vs. relying only on termination/fixed length). |
| `str_is_fixed_length=1` | String always occupies `str_total_length` bytes regardless of content length. |
| `str_is_zero_terminated=0` | Not NUL-terminated C-style. |
| `str_max_capacity=16` | Max characters storable (here, smaller than a standard 82-byte Rockwell STRING). |
| `str_pad_bytes=0` | No trailing padding bytes after the character data. |
| `str_total_length=20` | Total bytes per string field in the tag buffer (4-byte count + 16-byte capacity). |

### Indexed string element with resize support

```
protocol=ab-eip&gateway=10.206.1.40&path=1,0&plc=ControlLogix&name=CB_Txt[0,0]&str_is_counted=1&str_count_word_bytes=4&str_is_fixed_length=0&str_max_capacity=16&str_total_length=0&str_pad_bytes=0&allow_field_resize=1
```
Source: `src/tests/test_string/test_string.c`

| Attribute | Meaning |
|---|---|
| `name=CB_Txt[0,0]` | Comma-separated multi-dimension index (2-D array), alternate to `[0][0]`. |
| `str_is_fixed_length=0` | Variable-length string field — grows/shrinks in the buffer as content changes. |
| `allow_field_resize=1` | Lets the library resize the tag's data buffer in place when this variable-length field changes size on a read or write. |

---

## 4. UDTs, tag listing, and type discovery

libplctag doesn't need special syntax to read a UDT — a UDT tag is read as
raw bytes into the tag buffer, laid out exactly as Studio 5000 lays out the
struct in the PLC (respecting BOOL-packing, alignment, etc.). Individual
fields of interest are usually accessed with `Program:X.MyUdt.Field` naming
or by reading the whole UDT and indexing into the returned byte buffer
per the layout learned from `@tags`/`@udt/nnn`.

### Whole-UDT / many-BOOL-field tag

```
protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_count=1&name=TestManyBOOLFields
```
Source: `src/tests/test_tag_type_attribute/test_tag_type_attribute.c` — reads an entire UDT instance (with several packed BOOL members) as one tag.

### `@tags` — enumerate all tags in the PLC

```
protocol=ab-eip&gateway=<gw>&path=<path>&plc=ControlLogix&name=@tags
```
Used by `src/tools/list_tags_logix/list_tags_logix.c` (built from `TAG_STRING_TEMPLATE "protocol=ab-eip&gateway=%s&path=%s&plc=ControlLogix&name="` + `@tags`). Returns a packed array of tag descriptors (name, type code, dimensions). The tool then prints, for each tag found, a ready-to-use tag string, e.g.:

```
protocol=ab-eip&gateway=<gw>&path=<path>&plc=ControlLogix&elem_size=<n>&elem_count=<n>&name=<TagName>
```

### `@tags` on Micro800 (no `path`)

```
protocol=ab-eip&gateway=<gw>&plc=Micro800&name=@tags
```
Source: `src/tools/list_tags_micro8x0/list_tags_micro8x0.c`

### `@udt/nnn` — fetch a UDT definition by instance ID

```
name=@udt/42
```
`nnn` (here `42`) is the UDT template instance ID discovered from a tag's type info returned by `@tags`. Response payload is field names (NUL-terminated), types, offsets, and dimensions — see `Special-Tags.md` in the wiki.

---

## 5. DH+ bridging (EtherNet/IP → DH+ via a DHRIO module)

```
protocol=ab_eip&gateway=10.206.1.39&path=1,2,A:27:1&cpu=plc5&elem_count=1&elem_size=2&name=N7:0&debug=4
```
Source: `src/examples/multithread_plc5_dhp.c`; also exercised in `run_hardware_tests.sh` ("basic DH+ bridging").

| Attribute | Meaning |
|---|---|
| `path=1,2,A:27:1` | Routing path: port `1` (backplane) → slot `2` (DHRIO module in the local chassis) → `A:27:1` (DH+ address: channel `A`, our DH+ ID `27`, the remote DH+ ID "1" — the remote PLC/5's node number on the DH+ link). |
| `cpu=plc5` | The target across the bridge is a PLC/5 (PCCC protocol), even though the local hop is CIP/EtherNet-IP. |
| `name=N7:0` | PCCC address on the far side of the bridge. |

A second DH+ example, addressing a bit through the same bridge:

```
protocol=ab_eip&gateway=10.206.1.40&path=1,2,A:27:1&cpu=plc5&elem_count=1&elem_size=2&name=B3:0/10
```

---

## 6. EtherNet/IP (CIP) bridging — routing through another module/PLC

```
protocol=ab_eip&gateway=10.206.1.37&path=1,4,18,10.206.1.39,1,0&plc=lgx&name=TestBigArray[0]
```
Source: `src/tests/scripts/run_hardware_tests.sh` ("basic CIP bridging")

| Path segment | Meaning |
|---|---|
| `1,4` | First hop: port 1 (backplane), slot 4 (EtherNet/IP module). |
| `18,10.206.1.39` | Second hop: CIP port type 18 (Ethernet link) to IP address `10.206.1.39` — routes onto a second EtherNet/IP network/module. |
| `1,0` | Third hop: port 1, slot 0 — reaches the CPU in the second chassis. |

This is the general form for multi-hop CIP routing: each `type,address` pair
in `path` is one more hop through a router/bridge module before reaching the
target CPU. `gateway` is only the first device the library opens a TCP
connection to; everything after that is routed by the PLC's own backplane/
network hardware using the `path` value.

---

## 7. Special pseudo-tags

### `@identity` — device identity object

```
protocol=ab_eip&gateway=192.168.1.42&plc=generic&name=@identity
```
```
protocol=ab_eip&gateway=192.168.1.42&plc=generic&path=1,2&name=@identity
```
Source: `src/examples/get_identity.c`

`plc=generic` skips PLC-family-specific tag-name parsing since `@identity` isn't addressed like a data tag.

### `@connection` — session/connection lifecycle monitor

```
protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=@connection
```
```
protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&connection_inactivity_timeout_ms=5000&name=@connection
```
```
protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&name=@connection
```
Sources: `src/tests/test_connection_tag/test_connection_tag.c`, `run_simulator_tests_parallel.py`

| Attribute | Meaning |
|---|---|
| `name=@connection` | No PLC data — fires callback events on connect/disconnect/error instead. |
| `connection_inactivity_timeout_ms=5000` | Disconnect the session after 5s of no traffic (protocol-specific default is otherwise ~30s). |
| `io_events=0` | (Optional, not shown above) suppress the read/write-started/completed events this tag would otherwise also fire when sibling data tags on the same session do I/O. |

### `@raw` — direct CIP passthrough

```
protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&name=@raw
```
Source: `src/tests/test_raw_cip/test_raw_cip.c`. Bytes written with `plc_tag_set_raw_bytes()` are sent as-is as a CIP request; the response lands back in the buffer for `plc_tag_get_raw_bytes()`. No validation — malformed or malicious CIP payloads reach the PLC unchecked.

---

## 8. Other tag-level attributes seen in examples/tests

| Attribute | Example | Meaning |
|---|---|---|
| `read_cache_ms` | `...&read_cache_ms=100` (`multithread_cached_read.c`) | Cache read results for 100ms; repeated reads within that window return cached data instead of re-polling the PLC. |
| `auto_sync_read_ms` / `auto_sync_write_ms` | `...&auto_sync_read_ms=600&auto_sync_write_ms=20` (`test_auto_sync.c`) | Background auto-read every 600ms; buffered writes flushed 20ms after a local value change. |
| `connection_group_id` | `...&name=TestBigArray&connection_group_id=%d` (`stress_rc_mem.c`) | Tags sharing the same `connection_group_id` (and same connection-identifying attributes) share one underlying PLC session/connection instead of opening a new one each. |
| `use_connected_msg` | (AB-specific attrs, wiki) | `1`=CIP connected messaging, `0`=unconnected (UCMM). Required `1` for Micro800 and DH+ bridged links. |
| `allow_packing` | (AB-specific attrs, wiki) | `1` (default on Logix-class PLCs) lets the library bundle multiple tags' CIP requests into one packet (`Multiple Service Packet`). |
| `debug` | `...&debug=4` (many examples) | `0`=none .. `5`=spew; `4` (`PLCTAG_DEBUG_DETAIL`) is the common troubleshooting level. |
| `max_requests_in_flight` | (Modbus-specific attrs, wiki) | Pipeline up to 16 outstanding Modbus requests instead of the default of 1. |

---

## Sources scanned

- `src/examples/*.c`
- `src/tests/*/*.c`
- `src/tools/list_tags_logix`, `list_tags_micro8x0`, `tag_rw2`, `tag_rw_deprecated`
- `src/tests/scripts/run_hardware_tests.sh`
- `src/tests/scripts/run_simulator_tests_parallel.py`
- `~/Projects/libplctag.wiki/Tag-String-Attributes.md`, `All-Attributes.asciidoc`, `Special-Tags.md`
- `src/external_docs/PLC_TAG_CREATE_FROM_TAG.md`

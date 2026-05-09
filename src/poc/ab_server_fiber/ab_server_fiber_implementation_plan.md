# Plan: Fiber-based AB Server (`ab_server_fiber`)

Rewrite `src/tools/ab_server` as a fiber-based server in `src/poc/ab_server_fiber/`,
reusing `fiber_net`, `arena`, `bytes`, `log`, `err`, `args`, `utils` from
modbus_server3.  Replace all `slice_get`/`slice_set` calls with direct
`bytes_pack()`/`bytes_unpack()` calls.  Replace threads + mutexes with fibers.
Follow modbus_server3 patterns for main, listener fiber, client fiber.

---

## Decision: Rewrite, Not Mechanical Port

The existing ab_server uses threads, mutexes, 65 KB stack buffers,
`slice_get_*`/`slice_set_*` at raw offsets, and a callback-based TCP server.
Porting mechanically would require touching 85+ slice calls in `cip.c` alone
while preserving fragile offset arithmetic.  A rewrite starting from the
modbus_server3 template is cleaner because:

- Fiber lifecycle (listener + client fibers) is already proven.
- Arena per-request eliminates buffer management.
- `bytes_unpack()` returns remaining data, enabling incremental parsing naturally.
- Single-threaded model eliminates all mutex code.
- Stats / instrumentation come free from `fiber_net`.

---

## Phases

### Phase 1 — Scaffold & Infrastructure

1. Create `src/poc/ab_server_fiber/` directory.
2. Create `CMakeLists.txt` modeled on modbus_server3 (yafl, copied sources from
   `modbus_server/` for `err`, `args`, `utils`).
3. Wire into `src/poc/CMakeLists.txt`.
4. Copy `arena.c/h`, `args.c/h`, `bytes.c/h`, `err.c/h`, `log.c/h`, `fiber_net.c/h`, `utils.c/h` from modbus_server3.
5. Create `plc.h` — three structs:
   - **`tag_def_t`** — from ab_server `tag_def_s` minus `mutex_p` and atomics.
   - **`plc_config_t`** — global read-only after init: `plc_type`, `path[]`,
     `tags` list, `port_str`, `reject_fo_count`, `response_delay`.
   - **`eip_session_t`** — per-fiber connection state: `session_handle`,
     `sender_context`, connection IDs / sequences, packet size limits,
     `pccc_seq_id`.
6. Create `ab_server_fiber.c` skeleton: `main()` + `listener_fiber()` +
   `client_fiber()`.

*Depends on: nothing.*

### Phase 2 — EIP + CPF Layer

7. **`eip.h` / `eip.c`** (~120 lines) — rewrite from `ab_server/eip.c`:
   - Unpack 24-byte header:
     ```c
     Bytes rest = bytes_unpack(pkt, "<HHIIQI",
                               &cmd, &len, &session, &status, &context, &options);
     ```
   - Switch on command -> `RegisterSession` (0x0065), `UnregisterSession` (0x0066),
     `UnconnectedSend` (0x006F), `ConnectedSend` (0x0070).
   - Pack response:
     ```c
     Bytes hdr = bytes_pack(a, "<HHIIQI", cmd, resp_len, session, 0, context, 0);
     return bytes_join(a, hdr, payload);
     ```
8. **`cpf.h` / `cpf.c`** (~160 lines) — rewrite from `ab_server/cpf.c`:
   - Parse CPF item array with `bytes_unpack()`, route payload to CIP dispatch.
   - Rewrap response in CPF envelope with `bytes_pack()`.

*Depends on: Phase 1.*

### Phase 3 — CIP Protocol (Largest Phase)

9. **`cip.h` / `cip.c`** (~1200 lines) — rewrite from `ab_server/cip.c`:
   - **`cip_dispatch_unconnected()`/`cip_dispatch_connected()`**: split service byte + path + body, switch on service.  Try to unify dispatch so that connected vs. unconnected is handled at the CPF layer.
   - **`parse_tag_path()`**: parse symbolic segment `0x91` + name length + name
     bytes via `bytes_unpack("<BB", &seg_type, &name_len)` then
     `bytes_slice(rest, 0, name_len)`.  Parse numeric index segments
     `0x28`/`0x29`/`0x2A`.  Linear-search tag list by name.
   - **`handle_cip_read()`**:
     ```c
     Bytes rest = bytes_unpack(body, "<H", &count);
     if (fragmented) rest = bytes_unpack(rest, "<I", &frag_offset);
     // compute element byte offset from tag dimensions
     Bytes data = bytes_from_buf(tag->data + offset, copy_len);
     Bytes hdr = bytes_pack(a, "<BBBB", service | 0x80, 0, CIP_OK, 0);
     return bytes_join(a, hdr, data);
     ```

     Fragmentation: when response exceeds `max_packet`, set status `0x06`, return
     partial `bytes_slice()` of tag data.
   - **`handle_cip_write()`**: similar, copy request data into tag.
   - **`handle_forward_open()` / `handle_forward_close()`**:
     `bytes_unpack()` the 42+ byte structure, store connection state in
     `eip_session_t`, `bytes_pack()` response.
   - **`handle_multi_service()`**: `bytes_unpack()` offset table, recursively call
     `cip_dispatch()` per sub-request, `bytes_concat()` all responses,
     `bytes_pack()` offset table.
   - **`cip_error()`**:
  
     ```c
     // No extended status:
     bytes_pack(a, "<BBBB", service | 0x80, 0, status, 0);
     // With extended status:
     bytes_pack(a, "<BBBB", service | 0x80, 0, status, ext_status_word_count);
     ```

*Depends on: Phase 2 (dispatch routing).  Internally parallelizable — read/write,
forward open, multi-service, error helpers are independent.*

### Phase 4 — PCCC Protocol

10. **`pccc.h` / `pccc.c`** (~500 lines) — rewrite from `ab_server/pccc.c`:
    - **`pccc_dispatch()`**: check prefix bytes, extract seq ID with
      `bytes_unpack()`, route by `plc_type` + command byte.
    - **6 handlers** (plc5 read/write/rmw, slc read/write/rmw):
      `bytes_unpack()` request fields, find tag by `data_file_num`,
      `bytes_from_buf()` tag data, `bytes_pack()` response header,
      `bytes_join()` header + data.
    - **RMW handlers**: `bytes_slice()` AND/OR masks, apply bitwise in-place.

*Depends on: Phase 2 (dispatch routing).  Parallel with Phase 3.*

### Phase 5 — Main + Client Fiber

11. **`ab_server_fiber.c` `main()`**: `args_parse()` for `--plc`, `--path`,
    `--port`, `--tag`, `--delay`, `--reject_fo`.  Parse + allocate tags.
    `fiber_net_create()` + `fiber_net_run()` + shutdown.
12. **`listener_fiber()`**: accept loop -> spawn `client_fiber()` (copy
    modbus_server3 pattern).
13. **`client_fiber()`**:
    - 16 KB per-request arena.  Reset before receiving.  Reset before encoding response.
    - Allocate `eip_session_t` once outside the loop.
    - Loop:
      ```c
      Bytes hdr_buf = bytes_alloc(&arena, 24);
      fiber_socket_recv(net, sock, hdr_buf, IO_TIMEOUT_MS);
      uint16_t payload_len;
      bytes_unpack(hdr_buf, "<2xH", &payload_len);
      Bytes payload_buf = bytes_alloc(&arena, payload_len);
      fiber_socket_recv(net, sock, payload_buf, IO_TIMEOUT_MS);
      Bytes pkt = bytes_join(&arena, hdr_buf, payload_buf);
      Bytes response = eip_dispatch(&arena, pkt, session, config);
      fiber_socket_send(net, sock, response, IO_TIMEOUT_MS);
      arena_reset(&arena);
      ```
    - Support `response_delay` via fiber suspend loop (not blocking sleep).
14. **`print_statistics()`**: throughput, histogram, per-tag fairness,
    `fiber_net_get_loop_stats()` + CPU usage.

*Depends on: Phases 2-4 for protocol handlers.*

### Phase 6 — Verification

All the tests run in src/tests/scripts/run_simulator_tests.sh that take a tag string parameter must pass.

15. `make ab_server_fiber` — clean build, zero warnings with `-Wall -Wextra`.
16. ASAN / UBSAN clean under debug build.
17. Functional test: ControlLogix read/write with `tag_rw2`.
18. Functional test: PCCC with Micrologix mode.
19. Multi-service: test with libplctag client that sends batched requests.
20. ForwardOpen rejection: `--reject_fo=2` should reject first 2, accept 3rd.
21. Fragmentation: tag large enough to require fragmented reads.
22. Graceful shutdown: Ctrl-C prints statistics without crash.

---

## File Inventory

### New Files (~3000 Lines Total)

| File | Est. Lines | Source of Logic |
|---|---|---|
| `CMakeLists.txt` | ~60 | modbus_server3 template |
| `ab_server_fiber.c` | ~500 | modbus_server3.c pattern + ab_server/main.c tag parsing |
| `plc.h` | ~100 | ab_server/plc.h minus threads |
| `eip.c` / `eip.h` | ~150 | ab_server/eip.c rewritten with bytes_pack/unpack |
| `cpf.c` / `cpf.h` | ~200 | ab_server/cpf.c rewritten |
| `cip.c` / `cip.h` | ~1300 | ab_server/cip.c rewritten (**critical path**) |
| `pccc.c` / `pccc.h` | ~550 | ab_server/pccc.c rewritten |

### Copied from modbus_server3/ (Independent Evolution)

- `args.c` / `args.h`
- `arena.c` / `arena.h`
- `bytes.c` / `bytes.h`
- `err.c` / `err.h`
- `fiber_net.c` / `fiber_net.h`
- `log.c` / `log.h`
- `utils.c` / `utils.h`

### Eliminated (Not Ported)

| Old File | Replaced By |
|---|---|
| `slice.h` | `bytes_pack()` / `bytes_unpack()` / `bytes_slice()` |
| `socket.h` / `socket.c` | `fiber_socket_*()` |
| `tcp_server.h` / `tcp_server.c` | `fiber_net_*()` lifecycle |
| `thread.h` / `thread.c` | eliminated (single-threaded) |
| `mutex.h` / `mutex.c` | eliminated (single-threaded) |
| `memory.h` / `memory.c` | `arena` or direct `malloc()` / `free()` calls. |
| `compat.h` / `compat.c` | handled by `fiber_net` |

---

## Key Decisions

- **`bytes_pack()` / `bytes_unpack()` directly** in every handler — no wrapper
  layer, no slice compatibility shim.
- **No mutexes or atomics** — single-threaded fiber model.
- **`eip_session_t` per-fiber** (connection state); **`plc_config_t` shared
  read-only** by all fibers.
- **`random_utils.h`** replaced with deterministic counter for connection IDs
  (only used in ForwardOpen).
- **Response delay** via fiber suspend loop, not blocking `sleep()`.
- **16 KB arena per client fiber**, reset per request.
- **Copy shared files** into `ab_server_fiber/` for independent evolution (same
  approach as modbus_server3).

---

## Further Considerations

1. **Phase 3 is the critical path** — `cip.c` multi-service + fragmentation is
   ~60% of the effort.  Consider implementing read-only first, then writes +
   multi-service + fragmentation incrementally.
2. **CIP tag names** are length-prefixed binary, not C-strings.  Use
   `bytes_slice()` for the name bytes after unpacking the length byte — the
   `s` format char in `bytes_unpack()` is for NUL-terminated strings.
3. **Test coverage** — the existing `src/tests/` directory has scripts and test
   programs that exercise `ab_server`.  Those tests should work against
   `ab_server_fiber` with only a port/path change.

---

## Performance Measurements

Use or translate all the performance measurements from the modbus_server3 _and_ ab_server code.  It is critical to get detailed statistics about performance in this proof of concept.

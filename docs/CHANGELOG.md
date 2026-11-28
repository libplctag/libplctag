# Changelog

## 2.7.3 (changes since 2.7.2)

### New

- `find_or_create_tag=1` returns the existing tag for the same tag string instead of creating a new one.  A create with a callback that finds a tag registers the callback on it, or fails with `PLCTAG_ERR_DUPLICATE` if the tag already has one.
- `destroy_tag_after_idle_sec` destroys a tag after it goes this many seconds without a read or write.
- `connection_retry_max_delay_ms` caps the wait between reconnect attempts.
- `plc_tag_get_attribute_size()` returns the buffer size an attribute needs.
- `plc_tag_get_int64_attribute()` reads 64-bit integer attributes.
- `@connection` tags report connection statistics as attributes: connect, failure and idle disconnect counts, time connected, retry wait, tag count, requests, replies, bytes and tag operations (in total and since the latest connect), timeouts, error replies, requests in flight and response times.  AB and Omron also report the Forward Open's connection size and both connection IDs.  Reading them never waits on the connection's IO thread.  They are for information only and may change or be removed in any release.
- The `size` attribute can be set: `plc_tag_set_int_attribute(tag, "size", n)` resizes the tag's buffer, as `plc_tag_set_size()` does.
- Attributes set in the tag string can be read back after create: `plc`, `gateway`, `gateway_port`, `path`, `use_connected_msg`, `allow_packing`, `connection_group_id`, `find_or_create_tag`, `conn_only_use_old_forward_open`, the `str_*` attributes and the byte orders.
- Attribute names ignore case and `_`, in tag strings and in the attribute functions: `elem_count`, `ElemCount` and `ELEM_COUNT` are the same.
- `plc` is optional for CIP devices: without it the library reads the Identity object and picks the PLC type from the product name.
- `@identity` tag works on all CIP PLC types.
- `@tags` and `@udt/N` listing on Omron NJ/NX.
- `plc_tag_create_from_tag()` copies the source tag's data encoding (`str_*`, byte orders, `write_pad_alignment_bytes`); the new tag's string can override any of it.
- `write_pad_alignment_bytes` replaces `str_pad_to_multiple_bytes_EXPERIMENTAL`. It pads any write that fits in one request, not just strings.

### Changed

- Connection settings take the latest value given. `connection_inactivity_timeout_ms`, `connection_retry_max_delay_ms` and Modbus `max_requests_in_flight` are applied by every tag that joins a shared connection and can be set at any time through any of its tags or an `@connection` tag. Out-of-range values are clamped with a warning at create, and refused with `PLCTAG_ERR_OUT_OF_BOUNDS` by a set.
- Lowering `max_requests_in_flight` at runtime lets requests already in flight finish.
- `allow_packing` (the tag's own flag: 0 sends its requests alone), `io_events` (on `@connection` tags) and `conn_only_use_old_forward_open` can be set at any time. `conn_only_use_old_forward_open` is a connection setting like the ones above, used from the next reconnect from scratch.
- `use_connected_msg` is part of shared connection matching when `plc` is given: connected and unconnected tags no longer share a connection.
- `allow_packing` works on unconnected sessions.
- Forward Open negotiates its size: it accepts a size the PLC offers and steps down on refusal or "out of connections".
- A permanent Forward Open failure fails the tags waiting on that connection instead of retrying forever.
- Reconnects use one backoff with jitter for CIP and Modbus.
- Timeout defaults and limits are the same for CIP and Modbus.
- Tag IDs use the full range up to `INT32_MAX`.
- Omron NJ/NX support is merged into the AB/CIP code; `share_conn` is gone (use `connection_group_id`).

### Deprecated

- `share_session`: logs a warning; use `connection_group_id`.
- `elem_type`: logs a warning when given in a tag string; to be removed.  PCCC tags take the type from the data file in the tag name, and CIP tags get it from the PLC or the byte order attributes.
- `raw_tag_type_bytes.length`: use `plc_tag_get_attribute_size(tag, "raw_tag_type_bytes")`.

### Removed

- `str_pad_to_multiple_bytes_EXPERIMENTAL`: use `write_pad_alignment_bytes`.
- MinGW x86 (32-bit) build.

### Fixed

- #672: Omron fragment handling.
- #673: session create race.
- #675: livelock when the peer closes the socket during Forward Open.
- #677: sockets were inheritable by child processes (Windows `WSA_FLAG_NO_HANDLE_INHERIT`, POSIX `SOCK_CLOEXEC`/`FD_CLOEXEC`), so a destroyed tag's connection stayed open while a child ran.
- #679: the reconnect retry counter overflowed and was not reset after a good connection, giving near-zero retry waits.
- #680: on POSIX, `select()` with a socket at or above `FD_SETSIZE` crashed or corrupted memory; now `poll()`, with `EINTR` retried.
- #681: `plc_tag_create()` failed with `PLCTAG_ERR_NO_RESOURCES` after the tag ID counter wrapped onto long-lived tags; the highest ID could not be destroyed; `@connection` status was not updated.
- #682: ab_server PCCC replies did not echo the request's requestor ID.
- #683: `read_cache_ms` was never filled by async reads, and every cache hit restarted the window so the PLC was never read again.
- #684: a `plc_tag_create_ex()` callback could run after create returned an error.
- #686: Modbus writes that took several requests dropped the tail but reported success.
- #687: after `plc_tag_abort()` of a read in flight, the next read completed at once with `PLCTAG_STATUS_PENDING`.
- #688: PCCC named fields (`T4:0.ACC`, `C5:0.DN`, `R6:0.LEN` ...) failed with `PLCTAG_ERR_BAD_PARAM`.
- #689: `connection_inactivity_timeout_ms` range and clamping clarified; the setting was silently ignored on a tag joining a shared connection.
- #690: `@identity` failed with CIP status 0x04 when it shared a session with connected ControlLogix tags.
- #691: a session created by an `@connection` tag did not use connected messaging, so tags created after it timed out.  A comment in #681 reported this earlier.
- #692: the static library marked the `plc_tag_*` functions for export, so a DLL or shared library linking it re-exported them; on Windows it also defined `DllMain`, which could conflict with the `DllMain` of the program or DLL linking it.
- Race when `plc_tag_shutdown()` runs while a tag is being created.
- Race between a callback and tag destruction when create fails.
- Use-after-free and event handling for callbacks called from within callbacks.
- Modbus: dropped events, request/response payload mismatch, idle tag leak, double free of a request slot.
- Windows: closed sockets were not detected the same way as on POSIX.
- Reply data is read before a closed socket is handled.
- musl builds link `libatomic` when needed.

### ab_server

- Tag listing (`@tags`) and UDT definitions (`@udt/N`) for Logix and Omron; `--udt` defines UDT types for tags.
- ListIdentity over TCP and UDP, and the Identity object.
- Fault injection: `--close-every`, `--startup-close-ms`, `--dup-conn-id-tries`, `--link_offline`, `--no_large_fo`, `--no_reply`, `--out_of_connections`.
- `--delay-ms` replaces `--delay` and takes fractions of a millisecond.
- `--random-fill` fills tags with random data.
- `--reject_fo` is removed.

### modbus_server

- `--max-in-flight`, `--delay-ms`, `--reorder`, `--close-after-bytes`, `--startup-close-ms` and `--random-fill`.

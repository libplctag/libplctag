> **DEPRECATED.** This document predates the current EtherNet/IP design and is
> retained for historical reference only. The authoritative design is
> [ENIP-SESSION-DESIGN.md](../src/libplctag/protocols/enip/ENIP-SESSION-DESIGN.md). Do not use this document to guide new work.

# PLC Access Library — New C API Design

**Date:** 2026-06-07
**Status:** Design. A **new, standalone** PLC-communication library: a small,
path-based C API, usable directly and from wrapper languages.
**Relationship to libplctag:** *not* layered on it. libplctag and the ENIP
execution plans (`docs/ENIP-IMPLEMENTATION-EXECUTION-PLAN*.md`) are **reference
material** for protocol decoding, the per-connection thread model, metadata/shape
blocks, the generic type vocabulary, reference-counting, and the platform
`socket_wait_event`/wake-pipe construct — borrowed ideas and code, not a
dependency.

All public types and functions are `plc_`-prefixed.

---

## 1. Ergonomic principle

- **Path parsed once, inside the call.** `plc_read_int(dev, "my_array[42].field1.4", 0, t)`
  does the whole descent internally; the app never makes per-level calls for a path
  it knows, and never parses paths itself.
- **`path` + `index`.** Every accessor takes a `path` and an `int index` that
  addresses a child of the node at `path` (an array element or a struct field).
  Pass `index = 0` when `path` already names the node. Iterating an array is just
  `plc_read_int(dev, "arr", i, t)` — no string building.
- **No out-pointer parameters.** Reads return the value; writes take the value
  inline and return `plc_status_t`. The one exception is `plc_get_path`, which
  returns a constructed string the caller frees with `plc_free` (§3).
- **Timeout selects sync vs async** (§6): `0` = async, `> 0` = block.

Non-goals (this version): cascade/whole-structure writes (PLC multi-field writes
aren't atomic), timed auto-write, server-push transport, transactional batches.

---

## 2. Types

- `plc_dev_handle_t` — opaque device/connection handle from `plc_open`.
  Generational, ABA-safe; `PLC_INVALID_HANDLE` is 0; no arithmetic.
- `plc_status_t` — `PLC_STATUS_OK`, `PLC_STATUS_PENDING`, and `PLC_STATUS_ERR_*`.
- `plc_value_type_t` — the coarse, protocol-neutral type:

| `plc_value_type_t` | kind     | access  | notes                              |
|--------------------|----------|---------|------------------------------------|
| `PLC_VAL_STRUCT`   | composite| navigate| children are named fields          |
| `PLC_VAL_ARRAY`    | composite| navigate| children are indexed; nested per rank |
| `PLC_VAL_BOOL`     | scalar   | r/w     | may be a bit within a word (§5)    |
| `PLC_VAL_INT`      | scalar   | r/w     | `int64_t` (see unsigned note §5)   |
| `PLC_VAL_DOUBLE`   | scalar   | r/w     | `double`                           |
| `PLC_VAL_STRING`   | scalar   | r/w     | library-owned string               |
| `PLC_VAL_BYTES`    | scalar   | r/w     | opaque blob                        |
| `PLC_VAL_UNKNOWN`  | —        | —       | unmapped native type               |

The **protocol driver** maps native codes to these (`DINT`→`PLC_VAL_INT`, Logix
`STRING`→`PLC_VAL_STRING`, `BYTE/WORD/…`→`PLC_VAL_INT`); the precise native width
is kept internally for range-checking writes. The app never sees native names.

---

## 3. Lifetime and ownership

- `plc_open` → `plc_dev_handle_t`; `plc_close(dev)` tears down the connection and
  frees everything cached under it.
- **Metadata is cached for the device lifetime** (tag list, type trees, dims,
  offsets, UDTs); fetched lazily, invalidated by the reconnect/generation gate.
  This makes the `plc_get_*` calls and path resolution fast after first touch.
- **Interned, library-owned (do not free):** `plc_get_name` (a finite, stable set
  of names), valid for the device lifetime.
- **Library-owned read data:** `plc_read_string` / `plc_read_bytes` return pointers
  valid **until the next read of the same path, the next `plc_flush` that refetches
  it, or `plc_close`**. Copy if you need them longer.
- **Caller-owned (must free with `plc_free`):** `plc_get_path` constructs a child
  path of unbounded cardinality (`arr[0]`, `arr[1]`, …), so it is **not** interned;
  the library allocates it and the caller returns it via `plc_free` (allocator-safe
  across a DLL boundary; the only caller-frees function in the API).
- **Writes copy the app's input before returning**, so the app may free its buffer
  immediately.
- Threaded use is internally locked (the libplctag `api_mutex` pattern).

---

## 4. Reading and writing

```c
/* synchronous: one call does the I/O and returns */
int64_t      v = plc_read_int (dev, "my_array", 42, t);     /* my_array[42], t > 0 */
plc_status_t s = plc_write_int(dev, "my_array", 42, 7, t);

/* a bit is addressed in the path */
bool b = plc_read_bool(dev, "flags.3", 0, t);
```

Reads return the value; on error or not-yet-ready they return a **sentinel**
(`INT64_MIN` / `NAN` / `false` / `NULL`). There is no per-value status; validate a
path up front with `plc_get_type` (`PLC_VAL_UNKNOWN` means unknown/invalid), and
get async outcomes from `plc_poll_events` (§6).

**Bulk / batched access uses async staging + flush** (no array-bulk calls; staging
covers them and coalesces the wire traffic):

```c
for (int i = 0; i < n; i++) plc_write_int(dev, "arr", i, vals[i], 0);  /* stage (0 = async) */
plc_flush(dev, "arr", t);                                              /* one+ coalesced writes */

for (int i = 0; i < n; i++) plc_read_int(dev, "arr", i, 0);            /* stage reads (return sentinel) */
plc_flush(dev, "arr", t);                                              /* one batched read into cache  */
for (int i = 0; i < n; i++) vals[i] = plc_read_int(dev, "arr", i, 0);  /* from cache */
```

`plc_flush` coalesces contiguous array runs into single element-count requests and
multi-service-packs the rest. Writing disparate struct fields stays separate
writes — **no cascade / no false atomicity.**

### Enumeration

The device root is the container at path `""`. The same calls walk tags, struct
fields, and array elements:

```c
for (size_t i = 0; i < plc_get_count(dev, "", 0, t); i++) {
    const char *name = plc_get_name(dev, "", i, t);   /* tag/field name; NULL for array elems */
    char       *cp   = plc_get_path(dev, "", i, t);   /* "TagName"; caller frees */
    plc_value_type_t ty = plc_get_type(dev, cp, 0, t);
    /* recurse on cp for structs/arrays … */
    plc_free(cp);
}
```

Array shape is discovered by recursion: `plc_get_count(dev, "arr", 0, t)` is the
outermost dimension; build `arr[0]` with `plc_get_path` and `plc_get_count` it for
the next, down to scalars (count 0). No separate dimensions call.

---

## 5. Scalars in detail

- **Sentinels:** `read_int`→`INT64_MIN`, `read_double`→`NAN`, `read_bool`→`false`,
  `read_string`/`read_bytes`→`NULL` on error/not-ready.
- **Unsigned 64-bit:** the API exposes signed `int64_t` only (wrapper languages
  such as Java have no `uint64`). A 64-bit unsigned PLC value (`ULINT`, `LWORD`) is
  returned **bit-for-bit** in the `int64_t`; values ≥ 2⁶³ read as negative —
  reinterpret as unsigned where your language allows. No data is lost. (≤32-bit
  unsigned fits normally.)
- **Range-checked writes:** values are checked against the native width; out of
  range → error, value unchanged. Over-length string → error, unchanged.
- **Bits** are addressed in the path (`"tag.3"`); a bit write is a transparent
  read-modify-write of the containing element.
- **Arrays are recursive** (CIP `a[4][3][9]` = array of 4 of 3 of 9): `index`
  selects one level; deeper nesting rides the path (build it with `plc_get_path`).
- **`bytes` length** comes from `plc_get_size`.

---

## 6. Async, status, events

**Timeout convention (uniform across the API):**
- `timeout == 0` — **async / non-blocking**: a read returns the **cached** value if
  one exists (kept fresh by a subscription or a prior flush), otherwise it stages a
  fetch and returns a sentinel; a write is buffered (staged). Staged ops hit the
  wire on `plc_flush`, which (with `timeout == 0`) itself kicks and returns,
  completion via `plc_poll_events`.
- `timeout > 0` — **synchronous**: block up to that many ms. Use `INT_MAX` for
  "effectively unlimited."

There is no "default" and no "infinite" sentinel — `0` is async, `INT_MAX` is
forever.

**Device status:** `plc_status(dev)` returns the current connection state — one of
the `PLC_STATUS_CONN_*` values (`UP`, `DOWN`, `DISCONNECTING`, `CONNECTING`,
`IDLE_WAIT`, `ERR_WAIT`), mirroring libplctag's set. There is no per-path status —
a path's validity is checked with `plc_get_type`, a synchronous read's failure
shows as the sentinel, and per-operation async outcomes come from events.

**Waiting:** `plc_poll_events` blocks until staged ops complete (or timeout),
filling an event array. Each event carries the path, index, type, and **status**
(so failures are reported, not just completions). One drain loop per device; the
app dispatches/filters the small returned array itself (no filter argument — on a
shared queue a filter would drop events other consumers need). The event `path`
field is a fixed 256-byte buffer; paths longer than that are truncated.

**Device/connection events.** Connection state changes are not tied to a tag, so
they arrive as events with an **empty `path` (`""`)** — the same convention that
makes `""` the device root for enumeration. For such an event `index == 0`,
`type == PLC_VAL_UNKNOWN`, and `status` is the new `PLC_STATUS_CONN_*` value. The
app distinguishes a device event by testing `event.path[0] == '\0'`.

**Subscriptions.** `plc_subscribe(dev, path, read_interval_ms)` turns on background
polling of `path`: the library re-reads it every interval, updates the cache, and
emits a `plc_poll_events` event on each refresh. Retrieve the data with a
non-blocking read — `plc_read_*(dev, path, index, 0)` returns the latest cached
value (no I/O). A subscriber loop is: `plc_subscribe(...)`, then repeatedly
`plc_poll_events` → on an event for that path, `plc_read_*(..., 0)`.
`plc_unsubscribe` stops it. A push transport (EtherNet/IP Class 1, …) would later
replace the poll behind the same API.

Internally a value caches its data with a staleness/generation gate; only the
device runs a metadata timer, and composites re-resolve lazily on a generation
bump.

---

## 7. Full C API (`plc.h`)

```c
/* plc.h — new PLC access library. Path-based; opaque handles; no unions on the ABI.
 * Threaded use is internally locked. Handles are opaque (no arithmetic).
 *
 * timeout_ms:  0  = async (stage; complete via plc_flush + plc_poll_events)
 *             >0  = block up to N ms (use INT_MAX for effectively unlimited)
 * index: addresses a child of the node at `path` (array element or struct field);
 *        pass 0 when `path` already names the node. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t plc_dev_handle_t;
#define PLC_INVALID_HANDLE ((uint64_t)0)

typedef enum {
    /* operation results (returned by reads/writes/flush and carried in events) */
    PLC_STATUS_OK = 0, PLC_STATUS_PENDING,
    PLC_STATUS_ERR_INVALID_HANDLE, PLC_STATUS_ERR_EXPIRED, PLC_STATUS_ERR_NOT_FOUND,
    PLC_STATUS_ERR_TYPE, PLC_STATUS_ERR_RANGE, PLC_STATUS_ERR_TIMEOUT, PLC_STATUS_ERR_IO,
    PLC_STATUS_ERR_BAD_CONNECTION, PLC_STATUS_ERR_UNSUPPORTED, PLC_STATUS_ERR_NO_MEM,
    PLC_STATUS_ERR_BAD_PARAM, PLC_STATUS_ERR_CLOSED,

    /* connection/device states: returned by plc_status(dev) and carried in device
     * events (event.path == "").  Mirrors libplctag's connection-status set. */
    PLC_STATUS_CONN_UP = 100,        /* connected and ready                         */
    PLC_STATUS_CONN_DOWN,            /* not connected                               */
    PLC_STATUS_CONN_DISCONNECTING,   /* tearing down                                */
    PLC_STATUS_CONN_CONNECTING,      /* establishing                                */
    PLC_STATUS_CONN_IDLE_WAIT,       /* waiting to reconnect after idle disconnect  */
    PLC_STATUS_CONN_ERR_WAIT,        /* waiting to reconnect after error            */
} plc_status_t;

typedef enum {
    PLC_VAL_UNKNOWN = 0, PLC_VAL_STRUCT, PLC_VAL_ARRAY,
    PLC_VAL_BOOL, PLC_VAL_INT, PLC_VAL_DOUBLE, PLC_VAL_STRING, PLC_VAL_BYTES,
} plc_value_type_t;

/* ---- device ---- */
plc_dev_handle_t plc_open (const char *dev_url, int timeout_ms);
plc_status_t     plc_close(plc_dev_handle_t dev);
plc_status_t     plc_status(plc_dev_handle_t dev);   /* current PLC_STATUS_CONN_* state */

/* free a string the library handed you with explicit ownership (currently only
 * plc_get_path). Do NOT free interned/lib-owned returns. */
void             plc_free(void *p);

/* ---- metadata / enumeration (cached for the device lifetime) ---- */
size_t           plc_get_count(plc_dev_handle_t dev, const char *path, int index, int timeout_ms); /* children; 0 scalar */
plc_value_type_t plc_get_type (plc_dev_handle_t dev, const char *path, int index, int timeout_ms);
size_t           plc_get_size (plc_dev_handle_t dev, const char *path, int index, int timeout_ms); /* data bytes; 0 unknown */
const char      *plc_get_name (plc_dev_handle_t dev, const char *path, int index, int timeout_ms); /* field name; NULL for array elem/scalar; interned */
char            *plc_get_path (plc_dev_handle_t dev, const char *path, int index, int timeout_ms); /* "parent.field"/"parent[i]"; CALLER FREES via plc_free */

/* ---- read by path: return the value; sentinel on error/not-ready ---- */
int64_t        plc_read_int   (plc_dev_handle_t dev, const char *path, int index, int timeout_ms); /* INT64_MIN on error */
double         plc_read_double(plc_dev_handle_t dev, const char *path, int index, int timeout_ms); /* NAN on error       */
bool           plc_read_bool  (plc_dev_handle_t dev, const char *path, int index, int timeout_ms); /* false on error     */
const char    *plc_read_string(plc_dev_handle_t dev, const char *path, int index, int timeout_ms); /* lib-owned; NULL    */
const uint8_t *plc_read_bytes (plc_dev_handle_t dev, const char *path, int index, int timeout_ms); /* lib-owned; NULL; len via plc_get_size */

/* ---- write by path: value inline (copied before return); status out ---- */
plc_status_t plc_write_int   (plc_dev_handle_t dev, const char *path, int index, int64_t v, int timeout_ms);
plc_status_t plc_write_double(plc_dev_handle_t dev, const char *path, int index, double  v, int timeout_ms);
plc_status_t plc_write_bool  (plc_dev_handle_t dev, const char *path, int index, bool    v, int timeout_ms);
plc_status_t plc_write_string(plc_dev_handle_t dev, const char *path, int index, const char *s, int timeout_ms);
plc_status_t plc_write_bytes (plc_dev_handle_t dev, const char *path, int index, const uint8_t *b, size_t len, int timeout_ms);

/* ---- flush staged (timeout 0) reads & writes under `path`; coalesces contiguous
 *      array runs. timeout 0 kicks and returns (completion via plc_poll_events). ---- */
plc_status_t plc_flush(plc_dev_handle_t dev, const char *path, int timeout_ms);

/* ---- subscriptions: background polling that keeps the cache fresh ---- */
/* After this the library reads `path` every read_interval_ms; retrieve with a
 * non-blocking read plc_read_*(dev, path, index, 0), and each refresh emits a
 * plc_poll_events event for `path`. */
plc_status_t plc_subscribe  (plc_dev_handle_t dev, const char *path, int read_interval_ms);
plc_status_t plc_unsubscribe(plc_dev_handle_t dev, const char *path);

/* ---- events ---- */
typedef struct {
    char             path[256];   /* tag path that triggered the event (truncated if longer);
                                   * "" (empty) => a device/connection event                   */
    int              index;       /* element/field index (0 if N/A)                            */
    plc_value_type_t type;        /* value type; PLC_VAL_UNKNOWN for device events             */
    plc_status_t     status;      /* op result, or a PLC_STATUS_CONN_* state for device events */
} plc_event_t;

/* Wait for staged-op completions; fills events[], returns count (0 timeout, -1 error). */
int          plc_poll_events(plc_dev_handle_t dev, plc_event_t *events, size_t max_events, int timeout_ms);

#ifdef __cplusplus
}
#endif
```

---

## 7a. Example — connect, list tags, read a DINT array

```c
#include "plc.h"
#include <stdio.h>

int main(void) {
    plc_dev_handle_t dev = plc_open("eip://10.206.1.40:44818/1/0", 5000);
    if (dev == PLC_INVALID_HANDLE) { fprintf(stderr, "open failed\n"); return 1; }

    /* 1. list the controller's tags (children of the device root "") */
    for (size_t i = 0, n = plc_get_count(dev, "", 0, 5000); i < n; i++) {
        const char      *name = plc_get_name(dev, "", i, 5000);  /* interned; do not free */
        char            *path = plc_get_path(dev, "", i, 5000);  /* caller frees           */
        plc_value_type_t ty   = plc_get_type(dev, path, 0, 5000);
        if (ty == PLC_VAL_ARRAY)
            printf("%-24s array[%zu]\n", name, plc_get_count(dev, path, 0, 5000));
        else
            printf("%-24s type=%d\n", name, (int)ty);
        plc_free(path);
    }

    /* 2. read a DINT array in one batched round trip (stage async, then flush) */
    const char *arr = "TestBigArray";
    size_t cnt = plc_get_count(dev, arr, 0, 5000);             /* outermost dimension */

    for (size_t i = 0; i < cnt; i++)
        plc_read_int(dev, arr, (int)i, 0);                     /* stage each element  */

    if (plc_flush(dev, arr, 5000) != PLC_STATUS_OK) {          /* one coalesced ReadTag */
        fprintf(stderr, "read failed\n");
        plc_close(dev);
        return 1;
    }

    for (size_t i = 0; i < cnt; i++)                           /* now served from cache */
        printf("%s[%zu] = %lld\n", arr, i, (long long)plc_read_int(dev, arr, (int)i, 0));

    plc_close(dev);
    return 0;
}
```

A subscription version replaces step 2 with `plc_subscribe(dev, arr, 100)`, then a
loop of `plc_poll_events(...)` → on an event for `arr`, read each element with
`plc_read_int(dev, arr, i, 0)` (served from the auto-refreshed cache).

---

## 8. Decisions and remaining notes

Settled:
- **Unsigned 64-bit:** signed `int64_t` only (Java/others lack `uint64`); 64-bit
  unsigned values are returned bit-for-bit (§5). No `uint` variants.
- **Array-bulk convenience:** not added; stage+flush covers it. Revisit only if the
  read double-loop proves to hurt ergonomics in practice.
- **Status:** device-only `plc_status(dev)`; no per-path status (path validity via
  `plc_get_type`, sync failure via sentinel, async via event status).
- **Event path cap:** fixed `path[256]`, documented as truncating.
- **Naming:** all types and functions `plc_`-prefixed.

- **Connection states:** `plc_status_t` carries `PLC_STATUS_CONN_*`
  (UP/DOWN/DISCONNECTING/CONNECTING/IDLE_WAIT/ERR_WAIT, offset 100), returned by
  `plc_status(dev)` and delivered as device events (empty `path`).

- **Subscriptions:** poll-based via `plc_subscribe`/`plc_unsubscribe`; the cache is
  kept fresh and read back with a non-blocking (`timeout == 0`) read; refreshes
  emit `plc_poll_events` events. Push transport (Class 1, …) deferred behind the
  same API.

Still open:
- **Change-only vs every-interval events** for subscriptions (report-by-exception
  vs report-on-poll). Lean change-only to cut event noise.

---

**End of design.**

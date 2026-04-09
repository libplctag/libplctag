# Plan: Type-safe `bytes_pack` / `bytes_pack_into`

## Context

The current format-string API (`bytes_pack(Arena*, const char *fmt, ...)`) has two problems:

1. **No compile-time safety** — wrong format strings (wrong type specifier, wrong argument count) silently produce garbage at runtime.
2. **Double-pass overhead** — because `va_list` is not rewindable, the implementation must walk the argument list twice: once to compute the total byte count, then a second time to write the bytes.

The replacement uses C11 `_Generic` to detect each argument's C type at the call site and inject a type-tag integer before each value in the variadic argument list. The implementation does a **single pass**, writing directly into the arena via `arena_current()` / `arena_commit()`. Byte order is one leading enum parameter. Arrays are passed as `Bytes` structs (raw `memcpy`, no per-element endian conversion).

The old `bytes_pack` / `bytes_pack_into` are renamed to `bytes_pack_fmt` / `bytes_pack_into_fmt`. All callers are updated.

---

## Files changed

| File | What changes |
|------|-------------|
| `bytes.h` | Add `BytesEndian` / `BytesPackType` enums, `BYTES_TYPE_OF` `_Generic` macro, `BYTES_FOREACH` (up to 32 args), `bytes_pack` / `bytes_pack_into` macros, `bytes_pack_impl` / `bytes_pack_into_impl` declarations; rename old `bytes_pack` / `bytes_pack_into` decls to `*_fmt` |
| `bytes.c` | Add `write_typed_args` (static), `bytes_pack_impl`, `bytes_pack_into_impl`; rename old implementations to `*_fmt` |
| `cip.c`, `cpf.c`, `eip.c`, `pccc.c` | Rename call sites `bytes_pack(…)` → `bytes_pack_fmt(…)` and `bytes_pack_into(…)` → `bytes_pack_into_fmt(…)` |

---

## New types (`bytes.h`)

```c
typedef enum {
    BYTES_LE =  1,
    BYTES_BE = -1,
} BytesEndian;

typedef enum {
    BYTES_TYPE_END   = 0,   /* sentinel — always the last argument */
    BYTES_TYPE_U8,
    BYTES_TYPE_U16,
    BYTES_TYPE_U32,
    BYTES_TYPE_U64,
    BYTES_TYPE_I8,
    BYTES_TYPE_I16,
    BYTES_TYPE_I32,
    BYTES_TYPE_I64,
    BYTES_TYPE_F32,         /* float   — passed as double via vararg promotion */
    BYTES_TYPE_F64,         /* double  */
    BYTES_TYPE_BYTES,       /* Bytes struct — raw memcpy, no endian conversion */
    BYTES_TYPE_ARRAY,       /* BytesArray struct — per-element endian conversion */
} BytesPackType;

/*
 * Typed array descriptor passed via varargs for BYTES_TYPE_ARRAY.
 * elem_type must be one of the scalar types (U8..F64).
 */
typedef struct {
    void         *data;
    size_t        count;
    BytesPackType elem_type;
} BytesArray;
```

---

## `_Generic` type-tag macro (`bytes.h`)

```c
/*
 * Resolve a C expression to its BytesPackType tag at compile time.
 * Unrecognised types fall through to BYTES_TYPE_BYTES (raw copy).
 */
#define BYTES_TYPE_OF(x) _Generic((x),   \
    uint8_t:    BYTES_TYPE_U8,           \
    uint16_t:   BYTES_TYPE_U16,          \
    uint32_t:   BYTES_TYPE_U32,          \
    uint64_t:   BYTES_TYPE_U64,          \
    int8_t:     BYTES_TYPE_I8,           \
    int16_t:    BYTES_TYPE_I16,          \
    int32_t:    BYTES_TYPE_I32,          \
    int64_t:    BYTES_TYPE_I64,          \
    float:      BYTES_TYPE_F32,          \
    double:     BYTES_TYPE_F64,          \
    Bytes:      BYTES_TYPE_BYTES,        \
    BytesArray: BYTES_TYPE_ARRAY,        \
    default:    BYTES_TYPE_BYTES         \
)

/* Expand one user argument to a (type-tag, value) pair. */
#define BYTES_WRAP(x)  (int)BYTES_TYPE_OF(x), (x)

/*
 * Wrap any typed array pointer + element count into a BytesArray.
 * Element type is derived at compile time from sizeof(*ptr_) via _Generic.
 * Endian conversion is applied per-element in write_typed_args.
 *
 * Example: bytes_pack(a, BYTES_LE, (uint8_t)cmd, BYTES_ARRAY(my_u32_arr, 5))
 */
#define BYTES_ARRAY(ptr_, count_) \
    ((BytesArray){ \
        .data      = (void *)(ptr_), \
        .count     = (count_), \
        .elem_type = BYTES_TYPE_OF(*(ptr_)) \
    })
```

The `_Generic` dispatch is **compile-time only** — the selected branch is the only branch that survives to the binary (the others are discarded by the compiler). Zero runtime overhead for type dispatch.

---

## `BYTES_FOREACH` macro (`bytes.h`, up to 32 arguments)

```c
#define BYTES_FOREACH_1(_1)       BYTES_WRAP(_1)
#define BYTES_FOREACH_2(_1,...)   BYTES_WRAP(_1), BYTES_FOREACH_1(__VA_ARGS__)
#define BYTES_FOREACH_3(_1,...)   BYTES_WRAP(_1), BYTES_FOREACH_2(__VA_ARGS__)
/* … */
#define BYTES_FOREACH_32(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_31(__VA_ARGS__)

/* Count up to 32 arguments. */
#define BYTES_NARGS32_(_1,_2,_3,_4,_5,_6,_7,_8,          \
                       _9,_10,_11,_12,_13,_14,_15,_16,    \
                       _17,_18,_19,_20,_21,_22,_23,_24,   \
                       _25,_26,_27,_28,_29,_30,_31,_32,N,...) N
#define BYTES_NARGS32(...) \
    BYTES_NARGS32_(__VA_ARGS__,                            \
        32,31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,  \
        16,15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)

#define BYTES_FOREACH_CAT_(a,b)  a##b
#define BYTES_FOREACH_CAT(a,b)   BYTES_FOREACH_CAT_(a,b)
#define BYTES_FOREACH(...)  BYTES_FOREACH_CAT(BYTES_FOREACH_, BYTES_NARGS32(__VA_ARGS__))(__VA_ARGS__)
```

---

## Public macros (`bytes.h`)

```c
extern Bytes bytes_pack_impl(Arena *a, int endian, ...);
extern Bytes bytes_pack_into_impl(Bytes buf, int endian, ...);

/*
 * Type-safe pack into a fresh arena allocation.
 * Usage: bytes_pack(arena, BYTES_LE, (uint8_t)cmd, (uint16_t)len, someBytes)
 */
#define bytes_pack(a_, endian_, ...) \
    bytes_pack_impl((a_), (int)(endian_), BYTES_FOREACH(__VA_ARGS__), (int)BYTES_TYPE_END)

/*
 * Type-safe pack into an existing Bytes buffer.
 * Returns the remaining unfilled slice; use bytes_filled(buf, rest) to recover what was written.
 */
#define bytes_pack_into(buf_, endian_, ...) \
    bytes_pack_into_impl((buf_), (int)(endian_), BYTES_FOREACH(__VA_ARGS__), (int)BYTES_TYPE_END)

/* Format-string variants (old API, still available) */
extern Bytes bytes_pack_fmt(Arena *a, const char *fmt, ...);
extern Bytes bytes_pack_into_fmt(Bytes buf, const char *fmt, ...);
```

---

## Core writer — `write_typed_args` (`bytes.c`, `static`)

Single pass. Writes directly into caller-supplied memory. Returns bytes written, or `SIZE_MAX` on overflow (nothing has been committed to the arena at that point).

```c
static size_t write_typed_args(uint8_t *dst, size_t cap, int endian, va_list args) {
    size_t off = 0;

    for(;;) {
        int tag = va_arg(args, int);
        if(tag == (int)BYTES_TYPE_END) { break; }

        switch((BytesPackType)tag) {
            case BYTES_TYPE_U8: {
                if(off + 1 > cap) { return SIZE_MAX; }
                dst[off++] = (uint8_t)va_arg(args, unsigned int);
                break;
            }
            case BYTES_TYPE_I8: {
                if(off + 1 > cap) { return SIZE_MAX; }
                dst[off++] = (uint8_t)(int8_t)va_arg(args, int);
                break;
            }
            case BYTES_TYPE_U16:
            case BYTES_TYPE_I16: {
                if(off + 2 > cap) { return SIZE_MAX; }
                uint16_t v = (uint16_t)va_arg(args, unsigned int);
                if(endian == (int)BYTES_LE) {
                    dst[off]   = (uint8_t) v;
                    dst[off+1] = (uint8_t)(v >> 8);
                } else {
                    dst[off]   = (uint8_t)(v >> 8);
                    dst[off+1] = (uint8_t) v;
                }
                off += 2;
                break;
            }
            case BYTES_TYPE_U32:
            case BYTES_TYPE_I32: {
                if(off + 4 > cap) { return SIZE_MAX; }
                uint32_t v = (uint32_t)va_arg(args, unsigned int);
                if(endian == (int)BYTES_LE) {
                    dst[off]   = (uint8_t) v;
                    dst[off+1] = (uint8_t)(v >>  8);
                    dst[off+2] = (uint8_t)(v >> 16);
                    dst[off+3] = (uint8_t)(v >> 24);
                } else {
                    dst[off]   = (uint8_t)(v >> 24);
                    dst[off+1] = (uint8_t)(v >> 16);
                    dst[off+2] = (uint8_t)(v >>  8);
                    dst[off+3] = (uint8_t) v;
                }
                off += 4;
                break;
            }
            case BYTES_TYPE_U64:
            case BYTES_TYPE_I64: {
                if(off + 8 > cap) { return SIZE_MAX; }
                uint64_t v = va_arg(args, uint64_t);
                if(endian == (int)BYTES_LE) {
                    for(int i = 0; i < 8; i++) { dst[off + (size_t)i] = (uint8_t)(v >> (i * 8)); }
                } else {
                    for(int i = 0; i < 8; i++) { dst[off + (size_t)i] = (uint8_t)(v >> ((7 - i) * 8)); }
                }
                off += 8;
                break;
            }
            case BYTES_TYPE_F32: {
                if(off + 4 > cap) { return SIZE_MAX; }
                double   d = va_arg(args, double);
                float    f = (float)d;
                uint32_t bits;
                memcpy(&bits, &f, 4);
                /* reuse U32 logic inline */
                if(endian == (int)BYTES_LE) {
                    dst[off]   = (uint8_t) bits;
                    dst[off+1] = (uint8_t)(bits >>  8);
                    dst[off+2] = (uint8_t)(bits >> 16);
                    dst[off+3] = (uint8_t)(bits >> 24);
                } else {
                    dst[off]   = (uint8_t)(bits >> 24);
                    dst[off+1] = (uint8_t)(bits >> 16);
                    dst[off+2] = (uint8_t)(bits >>  8);
                    dst[off+3] = (uint8_t) bits;
                }
                off += 4;
                break;
            }
            case BYTES_TYPE_F64: {
                if(off + 8 > cap) { return SIZE_MAX; }
                double   d = va_arg(args, double);
                uint64_t bits;
                memcpy(&bits, &d, 8);
                if(endian == (int)BYTES_LE) {
                    for(int i = 0; i < 8; i++) { dst[off + (size_t)i] = (uint8_t)(bits >> (i * 8)); }
                } else {
                    for(int i = 0; i < 8; i++) { dst[off + (size_t)i] = (uint8_t)(bits >> ((7 - i) * 8)); }
                }
                off += 8;
                break;
            }
            case BYTES_TYPE_BYTES: {
                Bytes b = va_arg(args, Bytes);
                if(b.data && b.len > 0) {
                    if(off + b.len > cap) { return SIZE_MAX; }
                    memcpy(dst + off, b.data, b.len);
                    off += b.len;
                }
                break;
            }
            case BYTES_TYPE_ARRAY: {
                BytesArray ba = va_arg(args, BytesArray);
                if(!ba.data || ba.count == 0) { break; }
                for(size_t i = 0; i < ba.count; i++) {
                    switch(ba.elem_type) {
                        case BYTES_TYPE_U8:
                        case BYTES_TYPE_I8: {
                            if(off + 1 > cap) { return SIZE_MAX; }
                            dst[off++] = ((uint8_t *)ba.data)[i];
                            break;
                        }
                        case BYTES_TYPE_U16:
                        case BYTES_TYPE_I16: {
                            if(off + 2 > cap) { return SIZE_MAX; }
                            uint16_t v;
                            memcpy(&v, (uint8_t *)ba.data + i * 2, 2);
                            if(endian == (int)BYTES_LE) {
                                dst[off]   = (uint8_t) v;
                                dst[off+1] = (uint8_t)(v >> 8);
                            } else {
                                dst[off]   = (uint8_t)(v >> 8);
                                dst[off+1] = (uint8_t) v;
                            }
                            off += 2;
                            break;
                        }
                        case BYTES_TYPE_U32:
                        case BYTES_TYPE_I32: {
                            if(off + 4 > cap) { return SIZE_MAX; }
                            uint32_t v;
                            memcpy(&v, (uint8_t *)ba.data + i * 4, 4);
                            if(endian == (int)BYTES_LE) {
                                dst[off]   = (uint8_t) v;
                                dst[off+1] = (uint8_t)(v >>  8);
                                dst[off+2] = (uint8_t)(v >> 16);
                                dst[off+3] = (uint8_t)(v >> 24);
                            } else {
                                dst[off]   = (uint8_t)(v >> 24);
                                dst[off+1] = (uint8_t)(v >> 16);
                                dst[off+2] = (uint8_t)(v >>  8);
                                dst[off+3] = (uint8_t) v;
                            }
                            off += 4;
                            break;
                        }
                        case BYTES_TYPE_U64:
                        case BYTES_TYPE_I64: {
                            if(off + 8 > cap) { return SIZE_MAX; }
                            uint64_t v;
                            memcpy(&v, (uint8_t *)ba.data + i * 8, 8);
                            if(endian == (int)BYTES_LE) {
                                for(size_t j = 0; j < 8; j++) { dst[off + j] = (uint8_t)(v >> (j * 8)); }
                            } else {
                                for(size_t j = 0; j < 8; j++) { dst[off + j] = (uint8_t)(v >> ((7 - j) * 8)); }
                            }
                            off += 8;
                            break;
                        }
                        case BYTES_TYPE_F32: {
                            if(off + 4 > cap) { return SIZE_MAX; }
                            uint32_t bits;
                            memcpy(&bits, (uint8_t *)ba.data + i * 4, 4);
                            if(endian == (int)BYTES_LE) {
                                dst[off]   = (uint8_t) bits;
                                dst[off+1] = (uint8_t)(bits >>  8);
                                dst[off+2] = (uint8_t)(bits >> 16);
                                dst[off+3] = (uint8_t)(bits >> 24);
                            } else {
                                dst[off]   = (uint8_t)(bits >> 24);
                                dst[off+1] = (uint8_t)(bits >> 16);
                                dst[off+2] = (uint8_t)(bits >>  8);
                                dst[off+3] = (uint8_t) bits;
                            }
                            off += 4;
                            break;
                        }
                        case BYTES_TYPE_F64: {
                            if(off + 8 > cap) { return SIZE_MAX; }
                            uint64_t bits;
                            memcpy(&bits, (uint8_t *)ba.data + i * 8, 8);
                            if(endian == (int)BYTES_LE) {
                                for(size_t j = 0; j < 8; j++) { dst[off + j] = (uint8_t)(bits >> (j * 8)); }
                            } else {
                                for(size_t j = 0; j < 8; j++) { dst[off + j] = (uint8_t)(bits >> ((7 - j) * 8)); }
                            }
                            off += 8;
                            break;
                        }
                        default: break;
                    }
                }
                break;
            }
            default: break;
        }
    }

    return off;
}
```

---

## `bytes_pack_impl` (`bytes.c`)

```c
Bytes bytes_pack_impl(Arena *a, int endian, ...) {
    uint8_t *dst = arena_current(a);
    size_t   cap = arena_remaining(a);

    va_list args;
    va_start(args, endian);
    size_t written = write_typed_args(dst, cap, endian, args);
    va_end(args);

    if(written == SIZE_MAX) { return (Bytes){NULL, 0}; }
    arena_commit(a, written);
    return (Bytes){dst, written};
}
```

On overflow, `arena_commit` is never called — the arena cursor is unchanged and `{NULL, 0}` is returned.

---

## `bytes_pack_into_impl` (`bytes.c`)

```c
Bytes bytes_pack_into_impl(Bytes buf, int endian, ...) {
    if(!buf.data) { return (Bytes){NULL, 0}; }

    va_list args;
    va_start(args, endian);
    size_t written = write_typed_args(buf.data, buf.len, endian, args);
    va_end(args);

    if(written == SIZE_MAX) { return (Bytes){NULL, 0}; }
    return bytes_slice(buf, written, buf.len - written);  /* remaining unfilled */
}
```

Return semantics match the existing `bytes_pack_into`: the **remaining** slice is returned. Use the existing `bytes_filled(original, rest)` inline helper to recover the written portion.

---

## Type-safe unpack (`bytes.h` + `bytes.c`)

### Design

`_Generic` dispatches on pointer types, which are all distinct:

```c
#define BYTES_OUT_TYPE_OF(ptr_) _Generic((ptr_),  \
    uint8_t*:    BYTES_TYPE_U8,                   \
    uint16_t*:   BYTES_TYPE_U16,                  \
    uint32_t*:   BYTES_TYPE_U32,                  \
    uint64_t*:   BYTES_TYPE_U64,                  \
    int8_t*:     BYTES_TYPE_I8,                   \
    int16_t*:    BYTES_TYPE_I16,                  \
    int32_t*:    BYTES_TYPE_I32,                  \
    int64_t*:    BYTES_TYPE_I64,                  \
    float*:      BYTES_TYPE_F32,                  \
    double*:     BYTES_TYPE_F64,                  \
    Bytes*:      BYTES_TYPE_BYTES,                \
    BytesArray*: BYTES_TYPE_ARRAY,                \
    default:     BYTES_TYPE_BYTES                 \
)

/* Expand one output pointer to a (type-tag, void*) pair. */
#define BYTES_UNWRAP(ptr_)  (int)BYTES_OUT_TYPE_OF(ptr_), (void *)(ptr_)
```

`BYTES_FOREACH_OUT` mirrors `BYTES_FOREACH` but uses `BYTES_UNWRAP`:

```c
#define BYTES_FOREACH_OUT_1(_1)       BYTES_UNWRAP(_1)
#define BYTES_FOREACH_OUT_2(_1,...)   BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_1(__VA_ARGS__)
/* … up to 32 */
#define BYTES_FOREACH_OUT_32(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_31(__VA_ARGS__)

#define BYTES_FOREACH_OUT(...)  BYTES_FOREACH_CAT(BYTES_FOREACH_OUT_, BYTES_NARGS32(__VA_ARGS__))(__VA_ARGS__)
```

Padding (`x` in format strings) is replaced by `bytes_skip`:

```c
/* Advance past n bytes; returns remaining slice or {NULL,0} if out of range. */
static inline Bytes bytes_skip(Bytes b, size_t n) {
    return bytes_slice(b, n, b.len >= n ? b.len - n : 0);
}
```

`Bytes*` output: caller pre-sets `ptr->len`; unpack assigns a zero-copy slice into the source buffer (`ptr->data` points into `src`, no copy).

`BytesArray*` output: caller pre-sets `count`, `elem_type`, and `data` (pre-allocated storage); unpack reads `count` elements with per-element endian conversion.

### Public API (`bytes.h`)

```c
extern Bytes bytes_unpack_impl(Bytes data, int endian, ...);

/*
 * Type-safe unpack from a Bytes source.
 * Each output argument must be a typed pointer: uint16_t*, uint32_t*, etc.
 * For Bytes*: pre-set ptr->len; a zero-copy slice is assigned to ptr->data.
 * For BytesArray*: pre-set count, elem_type, and data (pre-allocated).
 * For padding: use bytes_skip(data, n) before the next bytes_unpack call.
 * Returns the remaining unread slice, or {NULL,0} on underflow.
 */
#define bytes_unpack(data_, endian_, ...) \
    bytes_unpack_impl((data_), (int)(endian_), BYTES_FOREACH_OUT(__VA_ARGS__), (int)BYTES_TYPE_END)

/* Format-string variant (old API). */
extern Bytes bytes_unpack_fmt(Bytes data, const char *fmt, ...);
```

### Core reader — `read_typed_args` (`bytes.c`, `static`)

```c
static size_t read_typed_args(const uint8_t *src, size_t len, int endian, va_list args) {
    size_t off = 0;

    for(;;) {
        int   tag = va_arg(args, int);
        if(tag == (int)BYTES_TYPE_END) { break; }
        void *ptr = va_arg(args, void *);

        switch((BytesPackType)tag) {
            case BYTES_TYPE_U8: {
                if(off + 1 > len) { return SIZE_MAX; }
                *(uint8_t *)ptr = src[off++];
                break;
            }
            case BYTES_TYPE_I8: {
                if(off + 1 > len) { return SIZE_MAX; }
                *(int8_t *)ptr = (int8_t)src[off++];
                break;
            }
            case BYTES_TYPE_U16: {
                if(off + 2 > len) { return SIZE_MAX; }
                uint16_t v = (endian == (int)BYTES_LE)
                    ? ((uint16_t)src[off] | ((uint16_t)src[off+1] << 8))
                    : (((uint16_t)src[off] << 8) | (uint16_t)src[off+1]);
                *(uint16_t *)ptr = v;
                off += 2;
                break;
            }
            case BYTES_TYPE_I16: {
                if(off + 2 > len) { return SIZE_MAX; }
                uint16_t v = (endian == (int)BYTES_LE)
                    ? ((uint16_t)src[off] | ((uint16_t)src[off+1] << 8))
                    : (((uint16_t)src[off] << 8) | (uint16_t)src[off+1]);
                *(int16_t *)ptr = (int16_t)v;
                off += 2;
                break;
            }
            case BYTES_TYPE_U32: {
                if(off + 4 > len) { return SIZE_MAX; }
                uint32_t v = (endian == (int)BYTES_LE)
                    ? ((uint32_t)src[off] | ((uint32_t)src[off+1] << 8) | ((uint32_t)src[off+2] << 16) | ((uint32_t)src[off+3] << 24))
                    : (((uint32_t)src[off] << 24) | ((uint32_t)src[off+1] << 16) | ((uint32_t)src[off+2] << 8) | (uint32_t)src[off+3]);
                *(uint32_t *)ptr = v;
                off += 4;
                break;
            }
            case BYTES_TYPE_I32: {
                if(off + 4 > len) { return SIZE_MAX; }
                uint32_t v = (endian == (int)BYTES_LE)
                    ? ((uint32_t)src[off] | ((uint32_t)src[off+1] << 8) | ((uint32_t)src[off+2] << 16) | ((uint32_t)src[off+3] << 24))
                    : (((uint32_t)src[off] << 24) | ((uint32_t)src[off+1] << 16) | ((uint32_t)src[off+2] << 8) | (uint32_t)src[off+3]);
                *(int32_t *)ptr = (int32_t)v;
                off += 4;
                break;
            }
            case BYTES_TYPE_U64: {
                if(off + 8 > len) { return SIZE_MAX; }
                uint64_t v = 0;
                if(endian == (int)BYTES_LE) {
                    for(size_t i = 0; i < 8; i++) { v |= ((uint64_t)src[off + i] << (i * 8)); }
                } else {
                    for(size_t i = 0; i < 8; i++) { v |= ((uint64_t)src[off + i] << ((7 - i) * 8)); }
                }
                *(uint64_t *)ptr = v;
                off += 8;
                break;
            }
            case BYTES_TYPE_I64: {
                if(off + 8 > len) { return SIZE_MAX; }
                uint64_t v = 0;
                if(endian == (int)BYTES_LE) {
                    for(size_t i = 0; i < 8; i++) { v |= ((uint64_t)src[off + i] << (i * 8)); }
                } else {
                    for(size_t i = 0; i < 8; i++) { v |= ((uint64_t)src[off + i] << ((7 - i) * 8)); }
                }
                *(int64_t *)ptr = (int64_t)v;
                off += 8;
                break;
            }
            case BYTES_TYPE_F32: {
                if(off + 4 > len) { return SIZE_MAX; }
                uint32_t bits = (endian == (int)BYTES_LE)
                    ? ((uint32_t)src[off] | ((uint32_t)src[off+1] << 8) | ((uint32_t)src[off+2] << 16) | ((uint32_t)src[off+3] << 24))
                    : (((uint32_t)src[off] << 24) | ((uint32_t)src[off+1] << 16) | ((uint32_t)src[off+2] << 8) | (uint32_t)src[off+3]);
                memcpy(ptr, &bits, 4);
                off += 4;
                break;
            }
            case BYTES_TYPE_F64: {
                if(off + 8 > len) { return SIZE_MAX; }
                uint64_t bits = 0;
                if(endian == (int)BYTES_LE) {
                    for(size_t i = 0; i < 8; i++) { bits |= ((uint64_t)src[off + i] << (i * 8)); }
                } else {
                    for(size_t i = 0; i < 8; i++) { bits |= ((uint64_t)src[off + i] << ((7 - i) * 8)); }
                }
                memcpy(ptr, &bits, 8);
                off += 8;
                break;
            }
            case BYTES_TYPE_BYTES: {
                Bytes *bp = (Bytes *)ptr;
                if(off + bp->len > len) { return SIZE_MAX; }
                bp->data = (uint8_t *)src + off;  /* zero-copy slice */
                off += bp->len;
                break;
            }
            case BYTES_TYPE_ARRAY: {
                BytesArray *bap = (BytesArray *)ptr;
                if(!bap->data || bap->count == 0) { break; }
                for(size_t i = 0; i < bap->count; i++) {
                    switch(bap->elem_type) {
                        case BYTES_TYPE_U8:
                        case BYTES_TYPE_I8: {
                            if(off + 1 > len) { return SIZE_MAX; }
                            ((uint8_t *)bap->data)[i] = src[off++];
                            break;
                        }
                        case BYTES_TYPE_U16:
                        case BYTES_TYPE_I16: {
                            if(off + 2 > len) { return SIZE_MAX; }
                            uint16_t v = (endian == (int)BYTES_LE)
                                ? ((uint16_t)src[off] | ((uint16_t)src[off+1] << 8))
                                : (((uint16_t)src[off] << 8) | (uint16_t)src[off+1]);
                            memcpy((uint8_t *)bap->data + i * 2, &v, 2);
                            off += 2;
                            break;
                        }
                        case BYTES_TYPE_U32:
                        case BYTES_TYPE_I32: {
                            if(off + 4 > len) { return SIZE_MAX; }
                            uint32_t v = (endian == (int)BYTES_LE)
                                ? ((uint32_t)src[off] | ((uint32_t)src[off+1] << 8) | ((uint32_t)src[off+2] << 16) | ((uint32_t)src[off+3] << 24))
                                : (((uint32_t)src[off] << 24) | ((uint32_t)src[off+1] << 16) | ((uint32_t)src[off+2] << 8) | (uint32_t)src[off+3]);
                            memcpy((uint8_t *)bap->data + i * 4, &v, 4);
                            off += 4;
                            break;
                        }
                        case BYTES_TYPE_U64:
                        case BYTES_TYPE_I64: {
                            if(off + 8 > len) { return SIZE_MAX; }
                            uint64_t v = 0;
                            if(endian == (int)BYTES_LE) {
                                for(size_t j = 0; j < 8; j++) { v |= ((uint64_t)src[off + j] << (j * 8)); }
                            } else {
                                for(size_t j = 0; j < 8; j++) { v |= ((uint64_t)src[off + j] << ((7 - j) * 8)); }
                            }
                            memcpy((uint8_t *)bap->data + i * 8, &v, 8);
                            off += 8;
                            break;
                        }
                        case BYTES_TYPE_F32: {
                            if(off + 4 > len) { return SIZE_MAX; }
                            uint32_t bits = (endian == (int)BYTES_LE)
                                ? ((uint32_t)src[off] | ((uint32_t)src[off+1] << 8) | ((uint32_t)src[off+2] << 16) | ((uint32_t)src[off+3] << 24))
                                : (((uint32_t)src[off] << 24) | ((uint32_t)src[off+1] << 16) | ((uint32_t)src[off+2] << 8) | (uint32_t)src[off+3]);
                            memcpy((uint8_t *)bap->data + i * 4, &bits, 4);
                            off += 4;
                            break;
                        }
                        case BYTES_TYPE_F64: {
                            if(off + 8 > len) { return SIZE_MAX; }
                            uint64_t bits = 0;
                            if(endian == (int)BYTES_LE) {
                                for(size_t j = 0; j < 8; j++) { bits |= ((uint64_t)src[off + j] << (j * 8)); }
                            } else {
                                for(size_t j = 0; j < 8; j++) { bits |= ((uint64_t)src[off + j] << ((7 - j) * 8)); }
                            }
                            memcpy((uint8_t *)bap->data + i * 8, &bits, 8);
                            off += 8;
                            break;
                        }
                        default: break;
                    }
                }
                break;
            }
            default: break;
        }
    }

    return off;
}
```

### `bytes_unpack_impl` (`bytes.c`)

```c
Bytes bytes_unpack_impl(Bytes data, int endian, ...) {
    if(!data.data) { return (Bytes){NULL, 0}; }

    va_list args;
    va_start(args, endian);
    size_t consumed = read_typed_args(data.data, data.len, endian, args);
    va_end(args);

    if(consumed == SIZE_MAX) { return (Bytes){NULL, 0}; }
    return bytes_slice(data, consumed, data.len - consumed);
}
```

---

## Caller updates

All call sites in `cip.c`, `cpf.c`, `eip.c`, `pccc.c`:

```
bytes_pack(a, "<...")        →  bytes_pack_fmt(a, "<...")
bytes_pack_into(buf, "<...")  →  bytes_pack_into_fmt(buf, "<...")
bytes_unpack(buf, "<...")    →  bytes_unpack_fmt(buf, "<...")
```

No logic changes — strictly mechanical name substitution.

---

## Verification

```sh
# Must compile clean (warnings as errors)
cmake --build build --target ab_server_fiber

# Must pass all 30 performance test configurations
src/tests/scripts/run_perf_ab_server_fiber.sh
```

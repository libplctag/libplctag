# Attribute Handling Redesign

Status: design, not yet implemented.

## 1. Problem

Runtime attributes are reached through three public functions (`libplctag.h:585-588`), each
of which walks a hand-written if-else chain in the core and then, only if nothing matched,
calls a per-protocol function that walks another hand-written if-else chain.

Chains in the core: `lib.c:2366` (library-scope get), `lib.c:2391` (tag get), `lib.c:2443`
(library-scope set), `lib.c:2472` (tag set). Chains in the protocol modules:
`ab_common.c:1010,1069,1118`, `omron_common.c:747,788,837`, `modbus.c:3685,3721,3883`,
`connection_tag.c:272`, `omron_connection_tag.c:234`.

Consequences:

- **Core attributes cannot be overridden.** `lib.c:2391` and `lib.c:2472` reach the vtable
  only in the final `else`. A protocol has no way to replace `size` or to refuse an
  attribute that makes no sense for it.
- **Adding one attribute touches many files.** The name is a string literal repeated in
  every chain that must see it, with no table anywhere.
- **Type is encoded in the function.** A new value type costs a public function, a vtable
  slot, and a new chain in every module. That is why the library has no string attribute
  API, and why `plc_tag_get_byte_array_attribute()` has no library-scope (`id == 0`) path
  at all (`lib.c:2550` goes straight to `lookup_tag()`).
- **Variable-length payloads need a hand-written second name.** The size of
  `raw_tag_type_bytes` is fetched as a separate int attribute, `raw_tag_type_bytes.length`,
  implemented by hand in each module (`ab_common.c:1051`, `omron_common.c:777`). Nothing in
  the API asks an attribute how large its value is.
- **Error reporting differs by direction.** A getter returns the caller's `default_value`
  on failure and hides the reason in `tag->status`, so a failure is indistinguishable from
  a successful read whose value happens to equal the default. A setter returns the rc. At
  library scope an unsupported get returns the default while an unsupported set returns
  `PLCTAG_ERR_UNSUPPORTED`.
- **Nothing can enumerate the attributes.** Not the documentation, not tag-string
  validation, not a language binding. The wiki `All-Attributes` table is maintained by hand
  and drifts from the source.

Creation attributes (`attr_get_int(attribs, ...)`, 99 call sites, 43 of them in `lib.c`)
are a second, unrelated namespace that shares the word "attribute".

## 2. Goals and non-goals

Goals:

- One descriptor table per layer; lookup instead of dispatch chains.
- Protocol tables resolved before the core table, so a protocol can override or suppress
  a core attribute.
- Adding an attribute is a table entry, and a new value type is one enum value.
- A single place that decides what an unsupported access returns.
- A machine-readable source for the runtime half of the documentation table.

Non-goals:

- One new public function, `plc_tag_get_attribute_size()`. No change to the three existing
  functions or their signatures.
- Creation attributes are not unified with runtime attributes here. That is a separate
  project and nothing below depends on it.
- Library scope (`id == 0`) is not converted here. It is two chains and five names with
  nothing to override, so it is a later, independent commit.

## 3. Design

### 3.1 Descriptor

```c
typedef enum {
    ATTR_TYPE_INT,
    ATTR_TYPE_BYTES
} attr_val_type_t;

struct attr_def_t {
    const char *name;
    attr_val_type_t type;
    const char *description;   /* source for the generated documentation table */

    union {
        int32_t (*get_int)(plc_tag_p tag, int32_t *result);
        int32_t (*get_bytes)(plc_tag_p tag, uint8_t *buffer, int32_t buffer_length);
    };

    union {
        int32_t (*set_int)(plc_tag_p tag, int32_t value);
        int32_t (*set_bytes)(plc_tag_p tag, const uint8_t *buffer, int32_t buffer_length);
    };

    int32_t (*get_bytes_size)(plc_tag_p tag);
};
```

Anonymous unions are standard C11, which is what the project builds at
(`CMakeLists.txt:48`).

A table is an array of `attr_def_t` terminated by an entry whose `name` is NULL. Lookup is
a linear scan; the tables hold around ten entries each, so a hash would cost more code than
it saves.

### 3.2 The key is the name

Lookup is keyed on the name alone. The entry declares its type, and that type selects the
live branch of each union, so a union can never be read on the wrong branch. Reaching an
attribute through the wrong public entry point -- asking `plc_tag_get_int_attribute()` for
a byte array, or the reverse -- fails rather than guessing.

### 3.3 Asking an attribute how big its value is

```c
LIB_EXPORT int plc_tag_get_attribute_size(int32_t tag, const char *attrib_name);
```

A byte-array attribute is one entry. Its length is not an attribute of its own; the core
answers this call from the entry's `get_bytes_size`. An integer attribute reports the width
of an integer, which is four for as long as the integer API is `int`-shaped. A negative
return is an error code.

The sequence for a variable-length value is then: call `plc_tag_get_attribute_size()`,
allocate that many bytes, call `plc_tag_get_byte_array_attribute()` with the same name.

The alternative considered was two table entries sharing one name, an integer one reporting
the length and a byte-array one carrying the data, with lookup keyed on the pair (name,
type). It needs no new public symbol, which is its whole advantage. It was rejected because
an integer getter that returns a length for a name whose value is a byte array is
surprising, and because keying the table on a pair is machinery that exists only to let two
entries share a name. One function is the clearer trade.

`raw_tag_type_bytes.length` predates all of this. It survives as an ordinary integer entry
of its own, marked deprecated in its description, so that `tag_rw2.c:856` and
`test_tag_type_attribute.c:54` keep working. No new attribute gets one.

### 3.4 Policy is the pointers

There is no permission field. A NULL accessor is the whole policy:

| Entry state | Meaning |
| --- | --- |
| Setter NULL | Read-only |
| Getter NULL | Write-only |
| Both NULL | Suppressed: shadows the core entry of the same name |
| No entry in either table | Unsupported |

Every one of those returns `PLCTAG_ERR_UNSUPPORTED` from a single place in the core.

Nothing describes create-time writes, because nothing can call them through the public API.
A value that is only settable at tag creation is read out of the tag string by the protocol
constructor, which assigns it directly or calls the same static function the table would
have pointed at. The table describes the public surface only.

### 3.5 Accessors, not offsets

Every entry supplies functions. There is no `offset` field and no declarative field access.

The declarative form was considered and dropped. Making it safe needs a type enum wide
enough to name every field width, a switch over that enum on both the read and the write
path, and a range check driven by the declared type -- roughly fifty lines of subtle code,
with a silent memory bug available whenever an `offsetof` names the wrong struct. What it
would replace is seven two-line functions, because `size`, `bit_num`, `read_cache_ms`,
`auto_sync_read_ms`, `auto_sync_write_ms`, `connection_group_id` and `allow_field_resize`
are the entire population of plain fields, and most of them need a behavioral setter
regardless. Setting `auto_sync_read_ms` must also recompute the next automatic read deadline
from the new period and the current time; a bare field store would be wrong.

Range checking therefore lives in the setter that already has to exist.

### 3.6 Resolution order

```c
static const attr_def_t *attr_find(plc_tag_p tag, const char *name) {
    const attr_def_t *def = attr_table_find(tag->vtable->attribs, name);

    if(!def) { def = attr_table_find(core_attribs, name); }

    return def;
}
```

Protocol table first, core table second. This is the inversion that makes overriding
possible, and the suppression entry in section 3.3 depends on it.

### 3.7 Vtable

The three accessor pointers in `tag_vtable_t` (`tag.h:88-90`) are replaced by one:

```c
const attr_def_t *attribs;   /* NULL-name-terminated table */
```

### 3.8 Byte array semantics

`plc_tag_get_byte_array_attribute()` is unchanged. It still rejects a NULL buffer and a zero
length, a positive return still means "this many bytes were copied", and a buffer that is
too small still returns `PLCTAG_ERR_TOO_SMALL`. The size comes from
`plc_tag_get_attribute_size()`, not from passing NULL to the data accessor.

`set_bytes` has no public entry point today. The union branch exists so that adding one
later is a new public function and nothing else.

### 3.9 Error reporting

`get_int` returns a status and delivers the value through an out-parameter. That separates a
failed read from a successful read that returned the caller's default. The core substitutes
`default_value` when the status is bad, so the public signature does not change.

## 4. What this enables

- **A string attribute type** is one enum value, one pair of union branches and one public
  function, with no change to any module. Strings are a thin wrapper over byte arrays.
  Convention, fixed here so the first string attribute does not set it by accident: the
  stored bytes are raw and unterminated, `plc_tag_get_attribute_size()` reports the byte
  count *without* a terminator -- the same number `strlen()` would give -- and the string
  wrapper appends the NUL itself.
  This removes the only remaining reason for the `name=version` system tag, which exists
  because three integer attributes cannot return a version string.
- **Generated documentation** for the runtime half of the wiki `All-Attributes` page, from
  `name`, `type`, `description` and which accessors are NULL. The create-only half stays
  hand-written until the creation-attribute project lands.

## 5. Migration

Three steps, each shippable on its own.

1. Add `attr_def_t`, `attr_table_find()`, `attr_find()` and `core_attribs[]`. Keep the
   existing vtable accessors as a four-line fallback for any name that is in no table. Move
   the core attributes into `core_attribs[]`. No module changes. Behavior is identical apart
   from the resolution order.

   `plc_tag_get_attribute_size()` sees only what is in a table, so until a module is
   converted its attributes return `PLCTAG_ERR_UNSUPPORTED` from it. `raw_tag_type_bytes`
   keeps reporting its length through `raw_tag_type_bytes.length` until step 2 reaches the
   AB and Omron modules.
2. Convert one protocol module per commit, deleting that module's chains as its table lands.
3. Remove the three vtable function pointers and the fallback once the last module is
   converted.

## 6. Risk

Inverting the resolution order changes the meaning of any name that a module and the core
both handle. Today they are disjoint: the core owns `size`, `read_cache_ms`,
`auto_sync_read_ms`, `auto_sync_write_ms`, `bit_num`, `connection_group_id` and
`allow_field_resize`; the modules own `elem_size`, `elem_count`, `elem_type`,
`connection_status`, `connection_inactivity_timeout_ms` and `raw_tag_type_bytes`. The
inversion is therefore safe to make now, and gets harder with every attribute added.

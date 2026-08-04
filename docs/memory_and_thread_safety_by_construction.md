# Memory and Thread Safety By Construction

Notes from a review of `src/libplctag`, `src/libplctag/protocols`, and `src/utils`.
Captured for later; nothing here is implemented yet.

Spatial safety (slice/cursor buffers for the protocol marshalling code) is **out of
scope for this document** — it is covered by the separate slice-buffer rewrite plan.
What remains below is temporal safety, thread safety, and the parts of spatial safety
that live outside the wire-marshalling code.

C gives three levers for "by construction":

1. make the unsafe primitive unreachable (wrap it, delete the raw one),
2. make the compiler reject it (Clang attributes),
3. make it abort in CI (rank asserts).

Everything below is ranked by safety gained per line of diff.

---

## 1. Lock-rank asserts — do this first

**Problem.** Five lock families exist (`inst->tag_lookup_mutex`, `tag->api_mutex`,
`tag->ext_mutex`, `session->session_mutex`, `plc->mutex`) with no documented
acquisition order, and both directions of the same pair are taken in practice:

- `lib.c:1307/1314/1320/1356/1376` — `critical_block(tag->api_mutex)` then
  `tag->vtable->activate/status/abort(tag)`, which reaches protocol code that takes
  `session_mutex` / `plc->mutex`.
- `pccc.c` (8 sites) and `ab_common.c:799/805/864/1313` — take `tag->api_mutex` from
  inside protocol threads that already hold PLC state.

`modbus.c:1663` carries a hand-written `/* Phase 2 -- outside plc->mutex */` comment,
i.e. one instance of this was already found and worked around by hand.

**Fix.** Add `int rank` to `struct mutex_t`, a `mutex_create_ranked()`, and a
thread-local stack check in `mutex_lock_impl()`. Debug/CI builds only.

```c
/* platform.c, guarded by #ifdef LIBPLCTAG_LOCK_ORDER_CHECK */
static THREAD_LOCAL mutex_p held_mutexes[16];
static THREAD_LOCAL int held_ranks[16];
static THREAD_LOCAL int held_depth;

int mutex_lock_impl(const char *func, int line_num, mutex_p m) {
    /* same-pointer exemption: mutexes are recursive, re-locking one we already
     * hold is legal and must not trip the check. See note below. */
    if(held_depth > 0 && m != held_mutexes[held_depth - 1]
                      && m->rank <= held_ranks[held_depth - 1]) {
        pdebug(DEBUG_MODULE_PLATFORM, DEBUG_ERROR, 0,
               "Lock order violation at %s:%d: taking rank %d while holding rank %d",
               func, line_num, m->rank, held_ranks[held_depth - 1]);
        abort();
    }
    /* ... existing lock ... */
    held_mutexes[held_depth] = m;
    held_ranks[held_depth++] = m->rank;
}
```

**The same-pointer exemption is not optional.** `platform.c:600` creates every mutex
with `PTHREAD_MUTEX_RECURSIVE`; Windows uses `CRITICAL_SECTION`, also recursive. The
codebase relies on this — `add_session_unsafe()` and friends are called both from inside
`critical_block(session_mutex)` and from paths that take it themselves. Without the
pointer comparison, a rank-equal re-acquisition of the *same* mutex aborts, and §1 fires
false positives on the first run.

Recursive mutexes are also why this problem is invisible today: a callee can silently
re-enter a lock its caller holds, so nesting never announces itself at any single point
in the source. The assert is what makes it announce.

Proposed ranks — chosen to match how the code *wants* to flow; the assert then reports
where it does not:

| rank | lock                                |
|-----:|-------------------------------------|
|   10 | lib instance                        |
|   20 | `tag_lookup_mutex`                  |
|   30 | `session_mutex` / `plc->mutex`      |
|   40 | `tag->api_mutex`                    |
|   50 | `tag->ext_mutex`                    |

Under this ranking `lib.c:1307` aborts immediately. That is the point: a latent
deadlock becomes a CI failure on hardware we already run. Enable in every sanitizer job.

~40 lines.

### 1a. Two cheaper-looking alternatives, and why neither replaces this

**Multi-lock `critical_block`.** Extend the macro to take several mutexes, sort them by
rank internally, and lock in rank order — correct by construction instead of asserted
after the fact. Recursion makes the nesting safe. The premise holds; the reach does not.
The macro can only order locks named at one lexical point, and the edges that deadlock
here are not named anywhere:

```c
/* lib.c:1307 */
critical_block(tag->api_mutex) { tag->vtable->activate(tag); }
```

`activate()` reaches `session_mutex` (ab), `plc->mutex` (modbus), or `conn->mutex`
(omron), three frames down. Hoisting that into a multi-lock block means `lib.c` writing
the second mutex's name — but it lives in the protocol-specific struct, not
`TAG_BASE_STRUCT`. Generic code behind a vtable structurally cannot see it; that is what
the vtable is for.

Of the 7 lexically nested lock sites, exactly 2 could use it (`lib.c:1770` and
`lib.c:1812`, `api_mutex` + `ext_mutex`). The other 4 are vtable calls. So: ~20 lines of
varargs macro for 2 sites, leaving the 4 that actually bite. The assert inspects the
order a thread actually performed, at any call depth, and costs the same.

Worth keeping from the idea: the `rank` field on `struct mutex_t` is needed either way,
and if a third genuine two-lock site ever appears, the macro becomes worth its 20 lines.

**More atomics instead of locks.** Holding a reference to a tag or session and touching a
single field atomically is sound, and the codebase already does it correctly
(`atomic_get_int32(&tag->session->conn_status_ring_write_idx)`, `connection_tag.c:148`;
`modbus.c:3844`). It just does not reach far. Of 157 `critical_block` sites in
`src/libplctag`:

| body                                                                    | count | atomic-able |
|-------------------------------------------------------------------------|------:|-------------|
| calls a function (`hashtable_*`, `vector_*`, `*_unsafe`, `vtable->*`)   |  ~135 | no          |
| single scalar field, no invariant with a sibling                        |    ~8 | **yes**     |
| two fields with an invariant between them                               |    ~6 | no          |
| `req = rc_inc(tag->req)`                                                |     6 | no          |

~5% of sites, and **0 of the 7 nesting sites** — those are calls, not field accesses.

The rule holds for stale-tolerant scalars, not for pointers into refcounted objects.
`req = rc_inc(tag->req)` is the trap: it looks like one field, but load-then-increment is
two operations, and a concurrent `tag->req = new` plus `rc_dec(old)` frees the object
between them. Making `tag->req` atomic converts a mutex into a use-after-free.

---

## 2. Delete `src/utils/handle_system.{c,h}`

Dead code, and a second competing lifetime model. It is in no `CMakeLists.txt`, has zero
callers outside itself, and calls `mutex_create()` / `cond_create()` with no arguments
against `int mutex_create(mutex_p *m)` — it cannot compile.

If anyone ever revived it, it also has:

- `handle_acquire()` reads `handle_array.slots` / `num_slots` unsynchronized while
  `handle_alloc()` can `mem_free()` and replace that array — use-after-free.
- `handle_alloc()` returns `slot->handle` *after* leaving the critical block, through a
  pointer into a reallocatable array.
- `handle_destroy()` frees `header` while another thread may be blocked in
  `mutex_lock(header->mutex)`.
- The design holds a lock across arbitrary caller code (`handle_scoped`), which is the
  same hazard §1 exists to catch.

−545 lines.

---

## 3. Clang `-Wthread-safety`

The genuine correct-by-construction option for data races: annotate the mutex type as a
capability and every field with `guarded_by`, and the compiler refuses to build code
that touches a field without holding its lock.

```c
struct __attribute__((capability("mutex"))) mutex_t { /* ... */ };

#define TAG_BASE_STRUCT                                                   \
    /* ... */                                                             \
    mutex_p api_mutex;                                                    \
    int32_t  size    __attribute__((guarded_by(api_mutex)));              \
    uint8_t *data    __attribute__((guarded_by(api_mutex)));              \
    int8_t   status  __attribute__((guarded_by(api_mutex)));              \
    /* ... */
```

This codebase suits it unusually well: the field-to-lock mapping is already 1:1 and
already written down in prose. The comment on `skip_tickler` in `tag.h:186` explains
exactly which lock domain it lives in and why it had to become an atomic — annotations
make that comment executable, and would have caught the bitfield-packing race it
describes at compile time.

Clang-only. Run as a dedicated warnings job; cannot break GCC/MSVC builds.
Cost: annotate ~5 structs, then fix the fallout.

---

## 4. Clang `-Wconsumed` on refcounted pointers

`rc_inc()` / `rc_dec()` take `void *`, so the compiler cannot tell you that you released
the session where you meant the tag, that you forgot a release, or that you used the
pointer afterward.

Clang's consumable-type analysis is exactly this check — warning-only, zero runtime cost,
and it fits `rc.c` as already written:

```c
#if defined(__clang__)
#  define RC_TYPE      __attribute__((consumable(unconsumed)))
#  define RC_CONSUMES  __attribute__((set_typestate(consumed)))
#  define RC_PRODUCES  __attribute__((return_typestate(unconsumed)))
#else
#  define RC_TYPE
#  define RC_CONSUMES
#  define RC_PRODUCES
#endif

struct RC_TYPE plc_tag_t { /* ... */ };
extern void *rc_dec_impl(const char *func, int line_num, void *ref) RC_CONSUMES;
extern plc_tag_p lookup_tag(int32_t id) RC_PRODUCES;
```

Use-after-`rc_dec` becomes a build warning, as does a `lookup_tag()` result that is never
consumed on some path.

---

## 5. Scoped references and scoped locks

`lib.c` has 46 `lookup_tag()` sites, each hand-pairing its own `rc_dec()` across every
return path. `plc_tag_get_uint32()` relies on `break`-inside-`critical_block` falling
through to the `rc_dec` below it — correct, fragile.

```c
#define with_tag(id, var)                                                  \
    for(plc_tag_p var = lookup_tag(id); var != NULL; rc_dec(var), var = NULL)
```

46 hand-written acquire/release pairs become 46 uses of one construct.

`critical_block`'s doc comment says *"Do not use break, return, goto or continue"* —
safety by comment. On GCC/Clang, `__attribute__((cleanup))` makes release unconditional
on scope exit, including `return` and `goto`:

```c
static inline void rc_dec_cleanup(void *p) { rc_dec(*(void **)p); }
#define SCOPED_REF __attribute__((cleanup(rc_dec_cleanup)))
```

MSVC has no `cleanup`; keep the current macro as the MSVC path and let §1's rank assert
catch leaks there.

---

## 6. Fix `refcount_drain()`

It polls `sleep_ms(5)` on queue length plus a `cleanup_processing` flag, and on timeout
returns `PLCTAG_ERR_TIMEOUT` — after which shutdown proceeds to tear down the instance
anyway, with destructors possibly still running against it.

Have the cleanup thread signal a condvar when the queue drains, wait on that, and on
timeout log at `DEBUG_ERROR` and *refuse* to free the instance rather than racing it.
Correctness by polling interval is what intermittent shutdown-path CI flakiness looks
like from the inside.

~30 lines.

---

## 7. Remaining spatial items outside the slice rewrite

Two things the slice-buffer plan will not cover on its own:

**7a. Collapse the 46 `lib.c` scalar accessors.** They differ only in type, width, and
byte-order array, and each re-types the same check:

```c
if((offset >= 0) && (offset + ((int)sizeof(uint32_t)) <= tag->size)) { /* ... */ }
```

One X-macro table generating getter and setter from a single bounds-checked body means
one check in one place, and 46 call sites that cannot diverge. Roughly −900 lines from
`lib.c`.

**7b. Fold `tag->data` + `tag->size` into one slice.** They travel as an independent
pointer/length pair while `resize_tag_buffer_unsafe()` reallocates the pointer — the
`_unsafe` suffix is the entire enforcement mechanism. Worth coordinating with the slice
rewrite rather than doing separately.

**7c. Free CI flags.** `-D_FORTIFY_SOURCE=2`, `-Warray-bounds`, and one job with
`-fsanitize=bounds,object-size`. Flag additions to an existing matrix.

---

## Suggested order

| # | Change                                   | Diff        | Gets you                                            |
|---|------------------------------------------|-------------|-----------------------------------------------------|
| 1 | Lock-rank asserts (§1)                   | ~40 lines   | Finds real `api ↔ session` cycles today, on CI      |
| 2 | Delete `handle_system.*` (§2)            | −545 lines  | Removes a broken parallel lifetime model            |
| 3 | `-Wthread-safety` annotations (§3)       | ~5 structs  | Compiler rejects unlocked field access              |
| 4 | `-Wconsumed` on rc pointers (§4)         | 1 header    | Compiler flags use-after-`rc_dec`                   |
| 5 | `refcount_drain()` condvar (§6)          | ~30 lines   | Removes a poll-and-give-up shutdown race            |
| 6 | X-macro'd `lib.c` accessors (§7a)        | −900 lines  | One bounds check instead of 46                      |

Items 1, 2, and 5 are self-contained and independent of the slice rewrite. Items 3 and 4
are Clang-only warning jobs and cannot break the GCC/MSVC builds. Item 6 should probably
wait until the slice rewrite settles the shape of `tag->data`.

## Explicitly not proposed

- Generating marshalling code from a packet description — right long-term answer, a
  quarter of work. Revisit after the slice rewrite lands.
- Replacing `rc.c` with hazard pointers or epoch reclamation — the current refcount is
  sound (the CAS in `rc_inc` correctly refuses resurrection at count ≤ 0); the gaps are
  in *checking* its use, which §4 and §5 address for far less.

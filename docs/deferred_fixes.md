# Deferred Fixes and Outstanding Work

Running ledger of things found, deliberately not done, and why. Each entry says what
it is, where it is, and what it is waiting on.

Status key: **OPEN** = not started · **BLOCKED** = waiting on another item · **PARKED** = intentional, revisit later

---

## 1. Correctness bugs found

Fixed entries stay here as the record of what changed and what is still unverified.

### 1.1 `critical_block` unlocks a mutex it never acquired — OPEN

When `mutex_lock()` fails, the macro correctly skips the body but still calls
`mutex_unlock()`. Unlocking an unowned mutex is undefined behavior on POSIX; on Windows
it corrupts the CRITICAL_SECTION recursion count.

- Where: `critical_block` in both `platform.h` files (moving to `src/utils/mutex.h`)
- Blast radius: 159 call sites, both platforms
- Verified by instrumented harness: `lockfail` yields `LOCKFAIL1 U1`; nested inner
  failure yields `L1 B1 LOCKFAIL2 U2 U1` — the `U2` is the bug.
- Only fires when `mutex_lock` actually fails (destroyed mutex, `EINVAL`, `EAGAIN`).
- Fix is written and proven equivalent on 7 other scenarios (simple, nested, break,
  sequential, in-loop, loop+break, continue). Held back deliberately so the mutex
  file-move lands as a reviewable no-op first.

### 1.2 `atomic_set_bool` was not atomic on the non-C11 path — FIXED 2026-09-09

`src/utils/atomic_utils.c` non-C11 path was a plain read followed by a plain write, not an
exchange. Now `InterlockedExchange16` (Windows) / `__atomic_exchange_n` (GCC/Clang).

Demonstrated before the fix: a spinlock built on the old primitive **deadlocks** — a lost
update on release leaves the lock permanently held and every thread spins forever. The
harness hung until killed.

Extended in the same change to `atomic_set_int32` and `atomic_set_int64`, which had the
identical defect. `src/utils/debug.c:139` reads the return of `atomic_set_int32`, so that
one had a real consumer.

### 1.3 `atomic_compare_and_set_bool` returned the wrong thing — FIXED 2026-09-09

Both non-C11 branches returned a success flag instead of the original value, contrary to
the header contract and to the `int32`/`int64` versions in the same file. (The original
ledger entry said MSVC-only; the GCC branch was wrong too.)

This fixes `src/utils/debug.c:223` on the non-C11 path, where the thread that *won* the
race skipped `setvbuf()` while every loser called it.

**Still unverified:** the `Interlocked*` branches, including the `(SHORT)` casts and
`!= 0` conversions, need a Windows build. The `__atomic_*` branches are covered by the
harness described below.

**Verification:** `src/tests/unit/test_atomic_utils.c`, built twice —
`test_atomic_utils` (whatever path this compiler selects) and `test_atomic_utils_no_c11`
(`-D__STDC_NO_ATOMICS__`, forcing the fallback that is otherwise dead code wherever C11
atomics exist). Both registered with CTest and in the `unit_tests` target. Confirmed to
fail against the pre-fix code.

Note the failure mode is order-dependent: the compare-and-set assertion aborts first, so a
regression there fails fast. A regression in the *exchange* alone would instead hang
`test_set_bool_is_atomic_exchange`, since a lost update on release leaves the spin lock
permanently held.

---

## 2. Consolidation still to do

Original scope was threads, mutexes, condition variables, sockets out of `platform.h`
into `src/utils/`. Threads are done.

### 2.1 Mutexes — OPEN
Plan agreed: `src/utils/mutex.[ch]`, pure movement plus the `LINE_ID` macro fix
(proven behavior-identical). API unchanged. `mutex_try_lock` stays — 4 real call sites,
load-bearing in the Modbus response handler.

### 2.2 Spinlocks — OPEN (1.2 cleared, no longer blocked)
`src/utils/spinlock.[ch]`, built on `utils/atomic_utils.h` + `thread_yield()`, which makes
it the first of these with **zero** platform `#ifdef`s. 19 `spin_block` sites, 6 `lock_t`
variables. Cannot be correct on MSVC until 1.2 is fixed.

Note: `session.c:3415` does `res->lock = LOCK_INIT;` as a runtime assignment;
`ATOMIC_BOOL_STATIC_INIT` is an initializer form, so that becomes
`atomic_init_bool(&res->lock, false)`.

### 2.3 Condition variables — OPEN, needs a design decision
`platform.h`'s `cond_p` and `compat_utils.h`'s `compat_cond_t` are **different primitives
with the same name**:

- `cond_wait(c, timeout_ms)` — a self-contained auto-reset event, owns its mutex, has
  `cond_clear`, no broadcast.
- `compat_cond_wait(&cond, &mutex)` — a real condition variable requiring a caller-held
  mutex it atomically releases, plus `compat_cond_timedwait` and `compat_cond_broadcast`.

Not mechanically convertible: the compat form's atomic release-and-sleep is what prevents
lost wakeups. A wrong conversion produces a rare CI hang, not a compile error.
Recommendation on the table: expose both shapes from `condvar.h` rather than force one
onto the other.

### 2.4 Sockets — OPEN, larger than a consolidation
Three mutually incompatible APIs, with `socket_read`/`socket_write`/`socket_close`/
`socket_accept` colliding by name with different signatures and return conventions:

| API | shape | covers |
|---|---|---|
| `platform.h` | opaque `sock_p`, nonblocking connect, event loop, wake channel | client only |
| `tools/ab_server/socket.h` | raw `SOCKET` + `slice_s` | server |
| `{poc,tests}/utils/socket.h` | `socket_t` + `buf_t` + `util_err_t` | server, TCP + UDP |

`platform.h` has no bind/listen/accept and no UDP, so it cannot absorb the servers; the
`poc/tests` API has no nonblocking-connect state machine or wake channel, so it cannot
absorb the library. Suggested split: first de-duplicate the three byte-identical
`{poc,tests}/utils/socket.c` copies (−2,018 lines, zero API change), then treat
"unify client and server socket APIs" as its own project.

### 2.5 `compat_utils.*` cannot be deleted yet — BLOCKED on 2.3
Four copies remain (3 byte-identical + the `tests/` fork). Blocked by:
- `test_event.c` and `test_omron_destroy.c` use `compat_mutex_t` as the companion lock to
  `compat_cond_wait`/`compat_cond_timedwait` — mutex and condvar must convert together.
- Leftovers with no home yet: `compat_atomic_*`, `compat_time_ms`, `compat_sleep_ms`,
  `compat_set_interrupt_handler`, `compat_fprintf`, and (tests fork only)
  `compat_wait_for_listener`, `compat_cpu_count`.

### 2.6 Standalone servers keep their own shims — PARKED
`src/tools/ab_server/thread.[ch]` (321 lines) and `mutex.[ch]` (334 lines) are copies of
the platform API; `src/poc/modbus_server2/modbus_server2.c:85-111` has its own inline
static shims. Both targets deliberately do **not** link `plctag_static`
(`ab_server/CMakeLists.txt:4` documents the independent-sanitizer reason), and
`utils/thread.c`/`mutex.c` need `pdebug`/`mem_alloc`/`time_ms`. Folding them in means
either undoing that separation or importing library internals into standalone POCs.
Properly belongs to "unify the server support layer" (see 2.4).

---

## 3. Verification gaps

### 3.1 No Windows build — OPEN
Everything below compiles clean on POSIX and is unexercised on Windows:
- `src/utils/thread.c` — every `#ifdef _WIN32` branch
- 26 `THREAD_FUNC`/`THREAD_RETURN` conversions across 18 test files (wrong calling
  convention compiles clean on POSIX, fails on MSVC)
- the inlined `InterlockedCompareExchange` Winsock guard that replaced
  `compat_thread_once` in `src/tests/utils/compat_utils.c`
- fixes 1.2 and 1.3 when made — both are MSVC-only paths

### 3.2 `test_emulator_performance` now reports real numbers — DONE, watch it
Previously summed a local `int result` that was never assigned, so its iteration total
was always 0. Now routed through `thread_iterations[]`. Expect this test to start
reporting a non-zero figure where it used to report zero.

---

## 4. Dead code and cleanup (from the repo audit)

Ranked by size. All OPEN.

| what | where | approx |
|---|---|---|
| 4 identical copies of the server-utils set (args/buf/coro_net/err/log/socket/utils) | `poc/utils`, `poc/modbus_server`, `poc/ab_server_fiber`, `tests/utils` | −12,953 |
| `ab_server_fiber` duplicates `tools/ab_server` — fiber rewrite that never replaced it | `src/poc/ab_server_fiber` | −6,769 |
| three Modbus test servers; `modbus_server3` differs from `modbus_server` by 977 lines | `src/poc/modbus_server{,2,3}` | −3,966 |
| `list_tags_micro8x0` is `list_tags_logix` with 37 lines changed | `src/tools/list_tags_micro8x0` | −980 |
| `multithread_plc5.c` and `_dhp` differ by 6 lines; `multithread.c` and `_cached_read` same shape | `src/examples/multithread*.c` | −550 |
| `atomic_*_int64` family + `atomic_init_bool` — no callers (**but see 2.2: `atomic_init_bool` gains one**) | `src/utils/atomic_utils.h` | −60 |
| `attr.h` `find_entry` declared, never called outside `attr.c` — make it `static` | `src/utils/attr.h` | small |
| 4 sanitizer flags where 2 would do; `USE_SERVER_*` default to the client values | `CMakeLists.txt:76-81` | small |
| empty tool dirs holding only `__pycache__`/`CMakeFiles` | `src/tools/{capture,replay,device_sim}` | — |
| `hashtable.h` is the last header using `#ifndef` guards instead of `#pragma once` | `src/utils/hashtable.h` | 1 line |
| stale comment: "Module name lookup table is now defined in debug_generated.h" — it is defined in `debug_module_names.c`; the header only declares it | `src/utils/debug.c:116` | 1 line |
| dead serial-port block, marked `/* FIXME - either implement this or remove it. */` | both `platform.h`/`platform.c` | ~200 |
| untracked `ab_server_*.log` (2.4 MB) and `build/` sitting in the repo root — `.gitignore` fodder | repo root | — |

### Investigated and cleared — do not re-flag

- **`src/utils/hash.c`** — live. `hashtable.c:265` calls `hash()`; 7 callers total.
- **`src/contrib/cli`** — parked, not dead. Moving to `src/tools` once cleaned up.
- **`compat_random_u64`** — was a declaration with no definition in 3 headers; removed.
  The real `random_u64` in `src/utils/random_utils.c` is live with ~15 callers.
- **`src/utils/debug_module_names.c`** — live, despite the stale comment above.
- **`src/utils/handle_system.[ch]`** — was genuinely dead; deleted.
- **`src/unported`** — abandoned; deleted. Was a third-party contribution, recoverable at
  `7712ed7f:src/unported/`.

---

## 5. Settled decisions

- **2026-09-09** — `atomic_utils.c` fixed first (1.2, 1.3) so `spinlock.c` can be built on
  the atomic operations as intended, rather than carrying its own private test-and-set.

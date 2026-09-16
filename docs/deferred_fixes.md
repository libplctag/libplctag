# Deferred Fixes and Outstanding Work

Running ledger of things found, deliberately not done, and why. Each entry says what
it is, where it is, and what it is waiting on.

Status key: **OPEN** = not started · **BLOCKED** = waiting on another item · **PARKED** = intentional, revisit later

---

## 1. Correctness bugs found

Fixed entries stay here as the record of what changed and what is still unverified.

### 1.1 `critical_block` unlocked a mutex it never acquired — FIXED 2026-09-09

When `mutex_lock()` failed, the macro correctly skipped the body but still called
`mutex_unlock()`. Unlocking an unowned mutex is undefined behavior on POSIX; on Windows
it corrupts the recursion count.

The lock now happens in the outer loop's *initializer* rather than the inner loop's, so
the outer condition gates on the result. A `for` loop runs its increment only after a
completed iteration, so a failed lock never reaches the unlock.

- Where: `critical_block` in `src/utils/mutex.h`
- Blast radius: 160 call sites, both platforms, none edited
- Verified by instrumented harness across 10 scenarios: identical on all 8 success paths
  (simple, nested, break, sequential, in-loop, loop+break, continue, nested+break) and
  differing only on the two failure paths, where the spurious unlock is gone:
  `LOCKFAIL1 U1` → `LOCKFAIL1`, and `L1 B1 LOCKFAIL2 U2 U1` → `L1 B1 LOCKFAIL2 U1`.
- Verified in tree: 11/11 ctest, 181/181 simulator under ASan and under TSan, no races.

Found while auditing for the escapes the macro cannot protect against — `return` and
`goto` still leak the lock, and two real ones existed in `session_add_request()`
(`session.c`). Both are fixed; see 1.6. The tree is now clean at 0 of 160 sites and the
macro comment says to keep it that way.

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

### 1.6 `session_add_request()` returned from inside a `critical_block` — FIXED 2026-09-09

Two error paths in `session_add_request()` (`src/libplctag/protocols/ab/session.c`)
returned from inside `critical_block(session->session_mutex)`. `return` escapes both
loops of the macro, so `session_mutex` stayed locked forever — the session thread would
then block on it permanently. No macro shape can protect against this; the escape has to
be removed at the call site.

The null-request path now sets `rc` and `break`s, with the check moved after the block so
the tail does not dereference the null `req`.

The first of the two was worse than a leak: `if(!session)` was tested *inside*
`critical_block(session->session_mutex)`, which had already dereferenced `session` to get
the mutex. The check now happens before the block, where it can do some good.

Found by brace-matching every `critical_block` body in the tree. The audit is now clean:
0 of 160 sites contain `return` or `goto`. Worth re-running after any large merge.

---

### 1.7 AB leaked a request on ten "add request failed" paths — FIXED 2026-09-12

Found by measuring `ab/eip_cip.c` against `omron/omron_standard_tag.c` for the next port.

`session_add_request()` takes **its own** reference (`req = rc_inc(req)` inside the critical
block, `session.c`), so it does not consume the caller's. On failure the caller must release
what it holds. Ten sites did not:

| file | sites |
|---|---|
| `ab/eip_cip.c` | 6 |
| `ab/eip_cip_special.c` | 4 |

Each called `ab_tag_abort_request(tag)` and returned. That looks like cleanup but is not: it
releases `tag->req`, and the freshly built request has **not been stored there yet** --
`tag->req = req` happens only after a successful add. So `abort` released the previous
request (usually none) while the new one leaked.

The tree already contained the proof this is a bug rather than a convention. Of seventeen
sites that report "Unable to add request to session":

- Three pass `tag->req` itself, so `abort` does release them. Correct as they stand.
- Two, in `ab/eip_lgx_pccc.c`, do `req = rc_dec(req);` before aborting. Correct.
- Two, in `ab/pccc.c`, `break` to a shared `do{}while(0)` tail that releases on the failure
  path. Correct. (My first scan called these leaks; the six-line window missed the tail.)
- The remaining ten returned immediately with the reference outstanding.

And **Omron, which is the copy, gets it right in all four of its sites** -- `tag->req =
rc_dec(req)` -- which is how the asymmetry showed up in a similarity diff at all.

Fixed by inserting `req = rc_dec(req);` before the abort, matching what `eip_lgx_pccc.c`
already did. Both files gained `#include <utils/rc.h>`; they had been using `rc_inc`-free
code until now.

Reachability is narrow -- `session_add_request` fails only on a null session or on `rc_inc`
returning NULL for an object already being deleted, which is a shutdown race -- so this
leaks a request buffer per failed add during teardown rather than in steady state. That also
means the suites cannot be expected to show it.

Verification: clean build both configurations, 0 warnings, 0 errors. Simulator suite
**182/182** under ASan, 178s reported against 178s wall clock (15:42:25 to 15:45:24).

### 1.8 Two Forward Open fixes that existed on only one side — FIXED 2026-09-13

Found by diffing the four Forward Open functions ahead of merging them. Each module had a fix
the other lacked.

**Omron accepted a connection payload below the usable floor.** AB rejects a PLC-reported
supported size under `MIN_PAYLOAD_SIZE` with `PLCTAG_ERR_TOO_SMALL`, its comment noting that a
payload below the per-request overhead is "where the underflows live" -- the size arithmetic
downstream ends up with an overhead larger than the space. Omron had no floor at all and took
whatever the PLC offered. It now applies the same check.

**AB published the connection IDs outside the lock.** On a successful Forward Open, Omron
commits `targ_connection_id`, `orig_connection_id` and `max_payload_size` together inside
`critical_block(conn->mutex)`, with a comment that `conn_create_request()` reads
`max_payload_size` under the same lock. AB set the two connection IDs outside
`session_mutex` and took it only for `max_payload_size`, so a reader could see a committed
payload size beside a stale connection ID. AB now commits all three together.

Both were verified as one-sided by comparing the two implementations, not by reasoning about
either alone -- which is the argument for doing the comparison even where the merge itself is
deferred.

### 1.9 PCCC payload-size arithmetic — one bug introduced and reverted, 2026-09-14

**Nothing in released libplctag was wrong here.** This entry records a bug I introduced while
looking for one, because the reasoning that produced it is easy to repeat.

The claim was that `pccc_dhp_tag_write_start` omitted the 46-byte connected CPF header from
its overhead estimate, making `data_per_packet` too large and letting an over-long frame onto
the wire. It does omit the header, and that is correct.

`session_get_available_cip_payload_space()` returns
`GET_MAX_PAYLOAD_SIZE(conn) - sizeof(cpf_connected_data_item)`, and `cpf_connected_data_item`
is `{cpf_cdi_item_type, cpf_cdi_item_length, cpf_conn_seq_num}` -- six bytes. So its answer is
the budget for the bytes **after** `cpf_conn_seq_num`, and an overhead figure measured against
it counts only those bytes. Adding `sizeof(eip_cpf_co_header)` counts the 44-byte prefix a
second time. This is exactly what `EIP_CIP_PREFIX_SIZE` in `cip/conn.h` documents, and the
CAUTION comment there says not to substitute a struct size for it -- which is what I did, one
level up.

Three independent checks agree on 20 bytes of overhead for a DH+ PLC5 word write with a
3-byte encoded name:

- `pccc_dhp_tag_read_start` counts `pccc_dhp_routing_header + cmd + name + 1` and no CPF
  header. Same connection, same frame layout.
- The builder writes `cpf_cdi_item_length = cip_request_size + sizeof(cpf_conn_seq_num)` with
  `cip_request_size` measured from `(cip_req + 1)`, past the whole header. On a 244-byte
  connection: 2 + 20 + 218 = 240, item total 244.
- The request buffer is `available_payload + EIP_CIP_PREFIX_SIZE` = 282, and 46 + 20 + 216
  fills it with nothing to spare.

Measured on a lab PLC5 over a DH+ bridge, `N9:0`, INT elements, 238 bytes available:

| overhead | data per packet | elements |
|---|---|---|
| 20 (released, restored) | 218 | 109 |
| 66 (mine) | 172 | 86 -- refusal confirmed on hardware |

The probe in `src/tests/scripts/probe_pccc_limit.py` was written to settle this and did: it
walks the element count upward and separates a library refusal from a PLC refusal, so an
inflated estimate cannot hide behind a failure that looks like a hardware limit.

**The two DH+ bit writes did carry the double-count**, and still do not matter: a bit write
sends two masks, so `tag->size` is one element and the guard compares about two bytes against
two hundred. It can never fire. `sizeof(eip_cpf_co_header)` was removed from both for
consistency, not for behaviour. `pccc_dhp_rmw_cmd_req` already contains the DH+ routing
header, so that site does not add it separately.

**The direct (unconnected) PCCC path was examined and is not the same shape.** `cip_pccc_req`
is 13 bytes -- the Execute PCCC service header and the vendor request-id block -- not an
encapsulated frame, so `sizeof(cip_pccc_req)` in the overhead at `pccc.c:2101` is counting real
payload bytes. Direct PCCC builds an `eip_cpf_uc_header` frame with no Unconnected_Send
wrapper and no routing path, and its overhead of 26 against 238 gives 106 elements. A
100-element write was confirmed on hardware; the ceiling itself is unmeasured because the
only file long enough is exactly 100 elements.

Two loose ends on that path, neither a size bug:

- The `+ 1` at the end of the direct overhead is unexplained. It is conservative by one byte.
- `available_payload_unsafe()` subtracts `conn_path_size + 2` on the unconnected branch, for
  a routing path that the direct PCCC frame never emits. Harmless when no path is configured.
  When one is, the budget shrinks for a path that is not there -- and the more interesting
  question is whether a routed direct-PCCC tag reaches its target at all, since the builder
  appends no route. **Not verified.** DH+ bridging is the tested way to reach a PLC5 behind a
  ControlLogix, and it takes a different code path entirely.

**Hardware tests.** `run_hardware_tests.py` 30-34 cover this: whole-file (100 element) read and
write of `N101` both direct and over the DH+ bridge, plus a 115-element read that the library
must refuse locally on the 226-byte read ceiling. An earlier version of test 34 asserted that
a 100-element DH+ write was refused -- it encoded the bug as expected behaviour. The write
ceiling of 109 has no isolated test: `tag_rw2` reads before it writes, and a read past 109
elements needs a data file longer than any known one on the lab PLC5.

**Confirmed on hardware 2026-09-14: 45/45.** The two whole-file writes are the ones that
matter -- 200 bytes against the restored 218-byte limit, which the 172-byte limit refused.
Simulator suite 184/184 alongside.

**The lesson worth keeping.** This is the second size error of the same kind in this work. The
first was in the direct-write variant table: the two bit-write variants estimate with the
corresponding *word*-write command size rather than `sizeof(pccc_rmw_cmd_req)`, and
"correcting" them to the struct they actually put on the wire made the guard more permissive
than any released version. That one was caught in review; this one reached hardware. Both came
from comparing a number against a struct size instead of against what the number is measured
from. An offset is not a size, and neither is a conservative estimate. When a payload budget
and an overhead estimate meet, the only safe question is "from which byte is each one
counted?"

## 2. Consolidation still to do

Original scope was threads, mutexes, condition variables, sockets out of `platform.h`
into `src/utils/`. Threads are done.

### 2.1 Mutexes — DONE 2026-09-09

`src/utils/mutex.[ch]` (111 + 296 lines). Pure movement plus the `LINE_ID` macro fix
(proven behavior-identical). API unchanged, `int32_t` returns, `mutex_try_lock` kept.
Platform code confined to five static wrappers (`mutex_init`, `mutex_acquire`,
`mutex_acquire_try`, `mutex_release`, `mutex_close`) under one `#ifdef` at the bottom
of the file; every null check, `mem_alloc` and `pdebug` is shared.

`LINE_ID`/`PLCTAG_CAT`/`PLCTAG_CAT2` now live in `utils/mutex.h`; the Windows shim was
the only place that had them. `spinlock.h` deliberately keeps its own `SPIN_LINE_ID`
rather than depend on `mutex.h` — spin locks must not pull in the mutex layer.

Verified: clean build with **zero call-site edits** across 168 `critical_block` uses in
15 files, 11/11 ctest, 181/181 simulator under ASan, 181/181 under TSan with no race
reports. Cut posix `platform.c` 610–738 and windows 679–803.

Folded-in fix: POSIX `mutex_destroy` returned `PLCTAG_ERR_MUTEX_DESTROY` *before*
`mem_free`, leaking the handle whenever `pthread_mutex_destroy` failed. Both platforms
now always free. No caller ever tested for that code — the only references were the
producer itself and the error-to-string table at `lib.c:978`.

Still unverified: Windows. `CreateMutex` is a kernel object and recursive for the owning
thread, which matches the POSIX `PTHREAD_MUTEX_RECURSIVE` setup, but the moved code has
only been compiled on macOS/clang. The `mutex_acquire` `INFINITE` wait loop keeps its
original FIXME — it can still hang forever.

### 2.2 Spinlocks — DONE 2026-09-09

`src/utils/spinlock.[ch]`. `lock_t` is now `atomic_bool`, `LOCK_INIT` is
`ATOMIC_BOOL_STATIC_INIT`, and the implementation is built on `utils/atomic_utils.h` plus
`thread_yield()` — **zero preprocessor conditionals**, since those two headers already
carry the platform split. `lock_acquire()` returns `void` (it spins until held and cannot
fail), so `spin_block` has no failure path and none of the `critical_block` trouble in 1.1.

`spin_block` rewritten with a `LINE_ID()` form; proven identical to the old macro across
8 scenarios (simple, nested, break, continue, sequential, in-loop, loop+break, goto-out).
19 call sites and 6 `lock_t` variables compiled with **no edits**.

Two invariants captured while moving it:
- Nothing in `spinlock.c` may call `pdebug()`. `debug.c` guards its logger callback with a
  spin lock *because* these are logging-free; a `pdebug()` here re-enters `pdebug_impl()`
  from inside the lock it is taking. Now stated in a comment in `spinlock.c`.
- `goto`/`return` inside a `spin_block` leaks the lock, in the old macro and the new one
  alike. Audited: no site does this. The one apparent hit (`debug.c:360`) has the word
  "return" in a comment and correctly uses `break`.

`session.c:3415` changed from `res->lock = LOCK_INIT;` to `atomic_init_bool(&res->lock, false)`
— the right form for initializing an atomic object at runtime, and it gives
`atomic_init_bool` its first caller.

### 2.3 Condition variables — DONE 2026-09-09, renamed to `nap`

`src/utils/nap.[ch]` (99 + 443 lines). Movement out of `platform.h` plus a rename, because
the old name described the wrong primitive.

**What it really is.** Every one of the six objects follows the same pattern: a thread
sleeps until its own deadline, and anyone who gives it work cuts the sleep short. The
timeout is the *normal* path (`session.c:1746`, `lib.c:786` treat `ERR_TIMEOUT` as "nothing
happened, go around again"), the signal carries no data, and state lives elsewhere behind
the object's mutex. Stickiness exists only to close the race where work is queued just
before the waiter sleeps. Many signals during one sleep collapse into one wake up, which
suits state machines that re-read all their state on waking.

That is an interruptible sleep, not a condition variable. `cond_p` and friends became:

| was | now |
|---|---|
| `cond_p` | `nap_p` |
| `cond_create` / `cond_destroy` | `nap_create` / `nap_destroy` |
| `cond_wait(c, ms)` | `nap_wait(n, ms)` |
| `cond_signal(c)` | `nap_interrupt(n)` |
| `cond_clear(c)` | `nap_clear(n)` |
| `session->session_wait_cond`, `conn->wait_cond`, `tag->tag_cond_wait`, `inst->tag_tickler_wait`, `cleanup_cond`, `plc_cleanup_cond` | `session->session_nap`, `conn->nap`, `tag->tag_nap`, `inst->tag_tickler_nap`, `cleanup_nap`, `plc_cleanup_nap` |

Names considered and rejected: `event` (overloaded — callbacks, `SOCK_EVENT_*`), `signal`
(libc), `latch` (means manual-reset one-shot everywhere else), `wakeup`/`trigger` (34 hits
each; `socket_wake` is an adjacent but different mechanism), `notifier`, `binsem`, `parker`,
`idler`.

**The move itself.** Platform code confined to six static wrappers (`nap_init`, `nap_lock`,
`nap_unlock`, `nap_sleep`, `nap_signal`, `nap_close`) under one `#ifdef` at the bottom; the
null checks, `mem_alloc`, the spurious-wakeup loop, the timeout arithmetic and every
`pdebug` are now shared. `nap_sleep()` takes a *relative* millisecond count and absorbs the
one real platform difference: POSIX needs an absolute `struct timespec` deadline, Windows
takes a relative `DWORD`. `time_ms()` is `gettimeofday()`-based, the same clock the default
condition variable attributes use, so the computed deadline is identical to the old code's.

Verified: clean build, 11/11 ctest, 181/181 simulator under ASan, 181/181 under TSan with no
race reports. The move itself was proven first with **zero call-site edits** across all 80
uses; the rename was applied on top of that green build and re-verified. Cut posix
`platform.c` 608–842 and windows 678–858.

Folded-in fixes:
- **Windows `cond_destroy` never called `DeleteCriticalSection`**, freeing the struct with
  the critical section still initialized and leaking its wait resources. Same class as the
  `mutex_destroy` leak in 2.1. `nap_close()` now deletes it.
- The pending-interrupt flag is `bool` rather than `int`, and returns are `int32_t`, per the
  guidelines.

One deliberate behavior change, approved before implementation: `nap_interrupt` wakes **with
the lock held** on both platforms. POSIX already did; Windows woke after leaving the critical
section, carrying a comment (`/* Windows does this outside the critical section? */`) showing
the original author was unsure. Both orders are legal and neither loses a wakeup — the flag
is set under the lock either way — so the unified form leaves the 46 POSIX-tested call sites
bit-identical and changes only the Windows path.

Still unverified: Windows, as with 2.1 and 2.2. `CONDITION_VARIABLE` + `CRITICAL_SECTION`
must be used as a pair, which is why this keeps raw platform primitives instead of building
on `mutex_p` — a `CreateMutex` kernel handle cannot be passed to `SleepConditionVariableCS`.

### 2.3a `compat_cond_*` — DONE 2026-09-12

`compat_cond_t` was a real condition variable -- caller-held mutex atomically released on
wait, plus `compat_cond_timedwait` and `compat_cond_broadcast` -- and the earlier note here
said it was not convertible to `nap_p`. That judgment was about the primitive in the
abstract. Against the only two callers it does not hold.

`nap_wait()` checks a **pending** `interrupted` flag before it sleeps (`src/utils/nap.c`).
That is precisely what the caller-held mutex was buying: protection against a signal that
arrives between the predicate check and the sleep. Neither caller broadcasts and neither has
more than one waiter, so nothing else the condvar offered was in use.

`test_event.c`: the `compat_cond_t` and its companion `compat_mutex_t` became one `nap_p`.
The callback's `lock / signal / unlock` is now `nap_interrupt()`; the waiter's
`lock / wait / unlock` is `nap_wait(..., READ_EVENT_TIMEOUT_MS)` (5000 ms -- the old wait was
untimed, and a read that has not landed by then has failed anyway, which the status check
right after the wait already reports). A `nap_clear()` was added before each `plc_tag_read`:
the callback fires on *every* completed read, including one that completes before
`plc_tag_read` returns, so an interrupt could otherwise be left pending into the next pass.
**The old code could lose a wakeup** between `plc_tag_read` returning `PLCTAG_STATUS_PENDING`
and the wait starting; the conversion closes that.

`test_omron_destroy.c`: mutex and condvar became one `nap_p`, and `volatile int done` became
an `atomic_bool`. The destroy thread sets the flag and interrupts; main does a single
`nap_wait(..., DESTROY_TIMEOUT_MS)` and then tests the flag. No predicate loop -- one
signaller, one waiter, one shot, and the pending interrupt covers the thread finishing before
the wait starts.

**No condition variable was added to `src/utils/`.** With both callers converted,
`compat_cond_init/signal/broadcast/wait/timedwait/destroy` had no caller anywhere in the tree
and were deleted from all four `compat_utils` copies (62 lines each), along with the
`compat_cond_t` typedefs. Add a real condvar only when a caller turns up that needs one: more
than one waiter, or a predicate the waiter must re-check under the same lock the signaller
writes it under. `nap` is not that primitive and should not grow into it. The note in
`nap.h` that pointed at `compat_cond_*` now says this instead.

`test_fairness.c` still uses only `compat_mutex_t`, so it can convert to `utils/mutex.h` at
any time. It also has a `compat_mutex_init` with no matching destroy.

`test_event` was then wired into both suites, which needed it to behave like a test first.
Three things were wrong with it:

- It ran until `^C`. It now takes an optional second positional argument, the run duration in
  seconds; omit it and the interactive `^C` behaviour is unchanged.
- It returned 0 unconditionally -- bad arguments, tag creation failure and a dead PLC all
  exited 0. It now counts completed reads and thread failures in two atomics and exits 1 if
  any thread failed or no read ever completed, printing the pair either way.
- Its tag string was a hardcoded `TAG_PATH` naming the lab ControlLogix at `10.206.1.40`
  path `1,4`, so it could only ever run against real hardware. It now takes `--tag=`, the
  same idiom as `test_shutdown`, and its `DEFAULT_TAG_ATTRIBS` points at the local simulator
  (`127.0.0.1` path `1,0`). The `%d` is still filled in per thread.

Added to `run_simulator_tests_parallel.py` (4 threads, 5 s) and to `run_hardware_tests.py`
(4 threads, 20 s, `--tag=` overriding to `logix_gw`/`logix_path`), and to the
required-executables list in each. It went into `run_hardware_tests.sh` too, which was
deleted later the same day -- see 2.19.

Verification: clean build both configurations, 0 warnings, 0 errors.
`test_omron_destroy` run directly against `ab_server`, PASS. `test_event` run against a local
`ab_server` -- 25700 reads across 4 threads in 8 s, 0 thread failures, exit 0 -- and against
nothing, where it reports `0 reads completed` and exits 1 as intended. Its callback and nap
wait path is therefore covered in the sandbox, not only on hardware.
The three non-test `compat_utils.c` copies are still byte-identical to each other.
(Superseded by 2.5: two of the three are deleted and only `src/examples/` keeps a copy.)

### 2.4 Sockets — OPEN, design written up in `docs/socket_layering_design.md`

Three mutually incompatible APIs, with `socket_read`/`socket_write`/`socket_close`/
`socket_accept` colliding by name with different signatures and return conventions:

| API | shape | covers |
|---|---|---|
| `platform.h` | opaque `sock_p`, nonblocking connect, event mask, per-socket wake channel | library client only |
| `tools/ab_server/socket.h` | raw `SOCKET` + `slice_s` | `ab_server`, thread per connection |
| `poc/utils/socket.h` | `socket_t` + `buf_t` + `util_err_t` | `modbus_server` (TCP), `scan_eip_network` (UDP) |

**See `docs/socket_layering_design.md` for the full design.** Summary of what it settles:

- Only three things are genuinely shared: the EAGAIN+`select()` retry loop, the socket
  setup boilerplate, and the self-connected-TCP wake pair. The buffer and error types
  differ for real reasons and are *not* unified.
- The target model — a few threads each driving many sockets, per-connection state
  machines — inverts the obvious answer. Waiting must move *out* of the transfer calls, so
  the EAGAIN+select core is the compatibility shim, not the primitive to extract. The wake
  channel belongs to a per-thread poller, not to each socket.
- Proposed layers: `utils/socket_fd.[ch]` (non-blocking, no timeouts, the only file with a
  `#ifdef _WIN32`), `utils/poller.[ch]` (`poll()`/`WSAPoll()` backed, carries the thread's
  one wake pair, hands back a `void *context` per ready fd), then one thin adapter per
  program keeping its own types.
- `socket_wait_event()` should **not** move into `utils/` unchanged — it is `poller_wait()`
  over one socket, and its five call sites are all in `modbus.c`. The `wake_plc` vtable slot
  is `NULL` in every AB and Omron vtable; those protocols never wake out of a socket wait,
  yet `socket_create()` builds a wake pair for every socket, so the library carries one per
  connection and all but the modbus ones go unused.

Unblocked prerequisites — **step 1 DONE 2026-09-10**:
- `src/tests/utils/socket.[ch]` and `coro_net.[ch]` were compiled by **nothing**. Deleted.
- `src/poc/modbus_server` held byte-identical copies of seven `src/poc/utils` files
  (`socket`, `coro_net`, `buf`, `err`, `log`, `args`, `utils`). All eleven files deleted and
  the build pointed at `../utils`. Pointing at the shared `socket.c` alone was not possible:
  every one of these headers uses `#pragma once`, so one translation unit reaching `err.h`
  through both `modbus_server/` and `utils/` redefines every enumerator. The duplicate set
  had to go as a unit or not at all.
- `src/poc/modbus_server/log_modules.def` was a strict superset of the `utils` one (three
  extra entries: `FIBER_NET`, `MODBUS3_CLIENT`, `MODBUS3_LISTENER`). Merged into
  `src/poc/utils/log_modules.def` and the local copy deleted.
- `modbus_server2` and `modbus_server3` pulled those same files through
  `${MODBUS_SERVER_SRC}`; they now use a new `${MODBUS_UTILS_SRC}` for the shared ones and
  keep `${MODBUS_SERVER_SRC}` for `modbus_protocol.c`/`register_storage.c`.
- `ab_server`'s `socket_open_tcp_client()` had no caller outside its own `socket.c`. Deleted
  from both `socket.c` and `socket.h`.
- The UDP half of `poc/utils/socket.c` stays there for `scan_eip_network`; `modbus_server`
  does not use it.

Verification: clean build, 0 warnings, 0 errors; simulator suite 181/181.

That separate audit is done — see 2.20. `args`, `buf`, `err`, `log`, `utils`, `bitarray.h`
and `hashtable/` were dead and are deleted; `compat_utils` and `stats` are live and stay.
(2.5 replaced `compat_utils` there with the smaller `test_utils.[ch]`; `stats` is unchanged.)

Then round 6 moves the library's sockets to `src/utils/socket.[ch]` as pure movement, which
empties `platform.h`. The L0/poller work waits until the first state machine is about to be
written, so the poller is shaped by a real caller.

### 2.5 `compat_utils.*` — DONE 2026-09-16

Four copies went to one. `src/tools/utils/compat_utils.[ch]` and
`src/tests/utils/compat_utils.[ch]` are deleted; `src/examples/compat_utils.[ch]` stays.

**Why the examples copy stays.** Only `libplctag.h` is installed. The examples are template
code a user copies out of the tree and builds against an installed library, so an example
that includes `<utils/str.h>` would not compile for them. The shim there is doing the job it
exists for. The other three copies had no such constraint: tools, POCs and tests all build
inside the tree, link `plctag_static`, and already have `src/` on the include path from the
top-level `CMakeLists.txt:142`.

**What replaced what.** Every `compat_*` symbol outside `examples/` now resolves to the
library's own utility, except three that have no internal equivalent:

| `compat_*` | replacement | where |
|---|---|---|
| `compat_strcasecmp` | `str_cmp_i` | `utils/str.h` |
| `compat_strdup` | `str_dup` | `utils/str.h` |
| `compat_sscanf` | `sscanf` | stdlib |
| `compat_time_ms` | `time_ms` | `utils/time.h` |
| `compat_sleep_ms(x, NULL)` | `sleep_ms(x)` | `utils/time.h` |
| `compat_atomic_load_*` | `atomic_get_*` | `utils/atomic_utils.h` |
| `compat_atomic_store_*` | `atomic_set_*` | `utils/atomic_utils.h` |
| `compat_atomic_add_*` | `atomic_add_*` | `utils/atomic_utils.h` |
| `compat_atomic_inc_int32(a)` | `atomic_add_int32(a, 1)` | `utils/atomic_utils.h` |
| `compat_mutex_*` | `mutex_create`/`mutex_lock`/`mutex_unlock` | `utils/mutex.h` |
| `compat_fprintf` | `fprintf` | stdlib |

`compat_fprintf` was a `vsnprintf` into a fixed buffer followed by `fputs` — a silently
truncating `fprintf` with no caller that wanted truncation. Plain `fprintf` replaces it.

`compat_atomic_inc_int32` returned the pre-increment value, the same as
`__atomic_fetch_add`, and `atomic_add_int32` is also a fetch-add, so the two call sites that
use the return value (`test_connection_tag.c:280` and `:293`) are unchanged in meaning.
`compat_atomic_store_*` returned `void` where `atomic_set_*` returns the old value; every
call site discards it.

**The three survivors** moved to `src/tests/utils/test_utils.[ch]` (197/76 lines, down from
449/170) under `test_`-prefixed names: `test_set_interrupt_handler` (signals on POSIX,
`SetConsoleCtrlHandler` on Windows — the library has no equivalent),
`test_wait_for_listener` and `test_cpu_count`. `test_utils.h` also includes
`utils/atomic_utils.h`, `utils/mutex.h`, `utils/str.h` and `utils/time.h`, so the 37 test
programs kept one include line each instead of gaining four.

**Two name collisions** surfaced once `sleep_ms` and `time_ms` became the real names:
- `test_idle_disconnect.c:132` had a local `int64_t sleep_ms` — renamed `sleep_time_ms`.
- `test_reconnect_after_outage_sync.c:493` had a parameter `int64_t time_ms` shadowing the
  function it then tried to call — renamed `target_time_ms`. This one was a compile error,
  not a silent shadow: the body called `time_ms()` on the parameter.

**Incidental find, fixed.** All four copies guarded the Windows `vsscanf_s` branch of
`compat_sscanf` with `#ifdef _MSVC_VER`. MSVC's macro is `_MSC_VER`, so that branch has
never compiled anywhere and every platform has always used plain `vsscanf`. Fixed in the
surviving `src/examples/compat_utils.h:93`; the other three files are gone. It is only
reachable on Windows, which remains unbuilt (see 3.1).

**Also removed:** `src/poc/test_alternate_tag_listing.c` included `compat_utils.h` and used
no symbol from it; the include is gone. Seven `CMakeLists.txt` files stopped compiling
`compat_utils.c` (3 in `tools/`, 2 in `poc/`, and 37 in `tests/` switched to
`test_utils.c`), and the five `target_include_directories(... /utils)` lines that existed
only to find that header are gone with it.

**Verified:** ASan and TSan builds clean at `-Wall -Wextra -Wconversion -pedantic`, zero new
warnings; simulator suite 184/184 in 179s reported against 179s wall.

### 2.6 Standalone servers keep their own shims — PARKED
`src/tools/ab_server/thread.[ch]` (321 lines) and `mutex.[ch]` (334 lines) are copies of
the platform API; `src/poc/modbus_server2/modbus_server2.c:85-111` has its own inline
static shims. Both targets deliberately do **not** link `plctag_static`
(`ab_server/CMakeLists.txt:4` documents the independent-sanitizer reason), and
`utils/thread.c`/`mutex.c` need `pdebug`/`mem_alloc`/`time_ms`. Folding them in means
either undoing that separation or importing library internals into standalone POCs.
Properly belongs to "unify the server support layer" (see 2.4).

### 2.7 Sockets moved out of the platform shims — DONE 2026-09-10 (round 6)

`src/utils/socket.[ch]` now holds the library's socket API. `platform.c` lost
`posix:610-1741` (1,132 lines) and `windows:679-1787` (1,111 lines); both `platform.h`s
lost the socket block and pick the API back up through `#include <utils/socket.h>`.

**This round deliberately did not use the merge recipe.** Threads, spin locks, mutexes and
nap were folded into shared public functions over a thin static platform seam. Sockets were
not: only ~56% of the two bodies' lines match, and the divergence runs through the middle of
every function (`select()`/`fd_set` vs `WSAPoll`, `errno` vs `WSAGetLastError`,
`socketpair()` vs a self-connected TCP pair). Merging them would be a rewrite, not a move,
and it would be thrown away by the L0 `socket_fd` layer that does the same job better. So
`socket.c` carries both bodies whole under one top-level `#ifdef _WIN32`.

Verified at zero call-site edits: clean build with none of the 43 uses in `ab/session.[ch]`,
`omron/conn.[ch]` or `mb/modbus.c` touched, then 181/181 on the simulator suite *before* the
two changes below were applied on top.

New debug module: `PLCTAG_MODULE_SOCKET = 27`, appended to `plctag_debug_module_t`. The 310
`pdebug` calls in the moved code now use `DEBUG_MODULE_SOCKET` instead of
`DEBUG_MODULE_PLATFORM`. `DEBUG_MODULE_UTILS` was the standing precedent for `src/utils/`
files, but sockets are the noisiest thing in the library and folding them into `UTILS` would
have made socket logging impossible to request on its own. The addition is purely additive;
no existing module value moved.

Two fixes folded in, both pre-existing:
- **`socket_create()` leaked on wake-channel failure** (both platforms). It returned `rc`
  without freeing the allocation, leaving the caller holding a non-NULL pointer to a socket
  that was never created. Now frees and sets `*s = NULL`.
- **`sock_create_event_wakeup_channel()` was forward-declared `static` but defined without
  it** on POSIX. Legal C, but Windows had it right. Now `static` in both.

Verification: clean build (0 warnings) on the ASan and TSan builds; ASan suite 181/181;
TSan suite 181/181 with zero race reports.

### 2.8 Memory functions moved out of the platform shims — DONE 2026-09-10 (round 7)

`src/utils/mem.[ch]` now holds `mem_alloc`, `mem_realloc`, `mem_free`, `mem_set`,
`mem_copy`, `mem_move` and `mem_cmp`. `platform.c` lost `posix:74-232` (159 lines) and
`windows:79-237` (159 lines, including the dead `ssize_t` typedef below).

**There is no `#ifdef` in `mem.c`.** The two shim copies were the same code. The complete
diff between them was three `// NOLINTNEXTLINE` comments present only on POSIX, a cast
spelled `(size_t)(unsigned int)` on POSIX and `(size_t)(ssize_t)` on Windows, and a missing
`extern` on the POSIX `mem_cmp`. This was a deduplication, not a consolidation: ~314
duplicated lines became 205.

Casts standardized on `(size_t)(uint32_t)`. `-Wconversion` is on in every build, which is
why the double cast exists at all; each one sits behind a check that has already proven the
size positive. That made `typedef ptrdiff_t ssize_t;` at `windows/platform.c:80` dead --
the four memory casts were its only users -- so it went with the section.

Logging uses `DEBUG_MODULE_UTILS`, not a new module. Twelve `pdebug` calls, all
`DEBUG_WARN` on bad arguments. Sockets earned `PLCTAG_MODULE_SOCKET` because 310 calls of
connection chatter would have swamped `UTILS`; twelve warnings do not.

Done in two verified phases:
1. Pure move, signatures untouched. Clean build with none of the 302 call sites in 25 files
   edited; ASan 181/181; TSan 181/181.
2. `int` -> `int32_t` on all seven signatures. **Zero new warnings** across those same 302
   call sites, so nothing in the tree was passing a wider type into these. ASan 181/181;
   TSan 181/181.

Left behind deliberately: `WINDOWS_REQUESTED_TIMER_PERIOD_MS` at `windows/platform.c:77` is
now referenced only from commented-out code inside `utils/socket.c`. Dead, but deleting it
would strand that comment block; not worth a Windows-only edit that cannot be compiled here.

### 2.9 Serial port code deleted — DONE 2026-09-10

The serial API was declared in both `platform.h`s, implemented only on Windows
(`windows/platform.c:521-723`, 203 lines), and called by **nothing** — not the library, not
the tools, not the tests, not the POC servers. POSIX never had an implementation at all, so
those four declarations had no definition anywhere on that platform. The header carried
`/* FIXME - either implement this or remove it. */` since before this refactor started.

Deleted: the Windows `Serial Port` section, and from both headers the `serial_port_p`
typedef, the `PLC_SERIAL_PORT_NULL` macro and the four `plc_lib_*serial*` declarations.
Never tested, never exercised, and the user's call: remove rather than implement.

Note `PLC_SERIAL_PORT_NULL` expanded to `((plc_serial_port)NULL)` — a cast to a type
(`plc_serial_port`) that does not exist anywhere in the tree. Any use of that macro would
never have compiled, on either platform. Good evidence it never had one.

Left behind: `windows/platform.[ch]` still include `<tchar.h>` and `<strsafe.h>`, which the
serial code was the only user of. Removing them is a Windows-only edit that cannot be
compiled here, and `platform.h` reaches every file in the library, so it waits for the first
real Windows build (3.1).

Verification: clean build, 0 warnings; simulator suite 181/181.

### 2.10 String functions moved out of the platform shims — DONE 2026-09-10 (round 8)

`src/utils/str.[ch]` now holds the twelve string helpers. `platform.c` lost `posix:74-450`
(377 lines) and `windows:80-520` (441 lines).

**This one did use the merge recipe**, unlike sockets. The two copies differed in exactly
six places, all of them a single primitive:

| function | POSIX | Windows |
|---|---|---|
| `str_cmp_i` | `strcasecmp` | `_stricmp` |
| `str_cmp_i_n` | `strncasecmp` | `_strnicmp` |
| `str_str_cmp_i` | `strcasestr` | 30-line vendored public-domain `stristr` |
| `str_copy` | `strncpy` | `strncpy_s(..., _TRUNCATE)` |
| `str_dup` | `strdup` | `_strdup` |
| `str_to_float` | `strtof` | `strtod` + cast (Windows has no `strtof`) |

Everything else was drift: comment wording, one missing `return`, `int` vs `size_t` locals
in `str_split`, a `pdebug` commented out on one side. So the file is the shared bodies plus
six `platform_*` static wrappers under one `#ifdef` at the bottom. 818 duplicated lines
became 564.

The refuse-rather-than-truncate check in `str_copy` stays in the shared body rather than
moving into the wrapper, so both platforms refuse identically regardless of what their copy
primitive would have done alone. POSIX `strncpy` and Windows `strncpy_s(..., _TRUNCATE)` do
*not* agree on what happens when the source exactly fills the destination; the shared check
means neither ever gets the chance to disagree.

`str.h` picks up `COUNT_NARG` from `utils/macros.h`, which already had an identical copy of
that macro family. Both `platform.h`s carried their own `#ifndef`-guarded duplicate; those
are deleted, about 25 lines each.

Fixes folded in, both pre-existing:
- **Unreachable `return strncasecmp(...)` at the end of POSIX `str_cmp_i_n`** — every branch
  of the if/else above it already returns. Windows did not have the line. Deleted.
- **POSIX `str_split` returned NULL on a failed allocation without logging.** Windows logged.
  Now both do.

Verification: clean build with none of the 181 call sites in 14 files edited; ASan 181/181;
TSan 181/181.

### 2.11 `ZLA_SIZE` and its dead comments deleted — DONE 2026-09-10

`ZLA_SIZE` was defined as `0` on POSIX and as nothing on Windows, to spell a zero-length
trailing array portably. **No live code expanded it.** All 30 references were inside
comments -- commented-out trailing members in `ab/defs.h` and `omron/defs.h`:

```c
// uint8_t conn_path[ZLA_SIZE];    /* connection path as above */
```

They are commented out because the code stopped addressing payloads through flexible array
members and now indexes offsets into a byte buffer instead. The macro existed only so those
comments would compile if anyone uncommented them, which would have reintroduced an
abandoned design. Both definitions and all 30 comment lines removed, along with the
`/* VS C++ uses foo[] to denote a zero length array. */` note that explained the Windows
spelling.

Six of the 30 sat inside struct definitions that are *entirely* commented out
(`pccc_dhp_co_req`, `pccc_req` and four more). Those blocks remain -- they are a separate
dead-code question, not part of this.

Verification: clean build, 0 warnings; simulator suite 181/181.

### 2.12 Time moved out of the platform shims; `platform.c` deleted — DONE 2026-09-10 (round 9)

`src/utils/time.[ch]` now holds `time_ms()`, `time_us()` and `sleep_ms()`. **Both
`platform.c` files are gone** — after the time functions left there was nothing in them but
a license header, includes and dead macros. Removed from `libplctag_SRCS`; only
`${PLATFORM_SHIM_PATH}/platform.h` remains in the build.

`time_us()` was never in the shims. It had been hand-rolled twice, privately, in files with
no other reason to know the platform: `utils/debug.c:196` and `protocols/mb/modbus.c:77/89`.
Both copies are deleted and both `#ifdef _WIN32` blocks with them; `time_us()` is now a real
entry point.

Three fixes folded in, all pre-existing, all Windows-only and therefore unverified here:

- **`modbus.c`'s microsecond clock was ~15 ms granular on Windows.** Its private copy used
  `GetSystemTimeAsFileTime()`, which ticks at the scheduler interval (~15.6 ms by default),
  to measure per-cycle tickle, wait and send times *in microseconds*. Those numbers were
  noise. `debug.c`'s copy already used `GetSystemTimePreciseAsFileTime()` (Windows 8+);
  the shared implementation uses that.
- **`sleep_ms()` had two different contracts.** POSIX returned `PLCTAG_STATUS_OK` and
  rejected a negative; Windows returned `1` — which is `PLCTAG_STATUS_PENDING` — and passed
  a negative straight into `Sleep((DWORD)ms)`, sleeping about 49 days. None of the 12
  callers checks the return, so this was latent. The negative check now sits in the shared
  body so both platforms refuse identically.
- **`time_ms()` on Windows was a second, independent transcription of the FILETIME epoch
  math.** It is now `time_us() / 1000`, so the two clocks cannot disagree.

`localtime_r` went the other way. One caller (`debug.c:275`), and the shim defined the
*POSIX name*, so on Windows the library exported a public symbol called `localtime_r`. It is
now a static helper inside `debug.c` under `#ifdef _WIN32`. The exported name is gone.

Note the library already required Windows 8 or later before this round: `debug.c` called
`GetSystemTimePreciseAsFileTime()` unconditionally. Sharing that implementation does not add
a constraint, it makes an existing one apply consistently.

`WINDOWS_REQUESTED_TIMER_PERIOD_MS` went with `windows/platform.c`. Its only reference was
inside a commented-out block in `utils/socket.c`; anyone reviving that block needs to define
it again.

Done in two verified phases:
1. Move plus the three fixes. Clean build; the only files touched beyond the shims were
   `debug.c` and `modbus.c`, both deletions of code the new module replaces. 94 `time_ms`
   and 12 `sleep_ms` call sites untouched. ASan 181/181; TSan 181/181.
2. `int` -> `int32_t` on `sleep_ms`, and the POSIX wrapper's `done` flag to `bool` per the
   guidelines. Zero new warnings. ASan 181/181; TSan 181/181.

This run also closes out 2.11 (`ZLA_SIZE`) and the `snprintf_platform` removal below, which
had no passing suite behind them at the time.

### 2.13 `snprintf_platform` removed — DONE 2026-09-10

Two call sites, both in `utils/attr.c`, and the macro mapped to `snprintf` on POSIX but
`sprintf_s` on Windows. Those are not equivalent: `snprintf` truncates on overflow,
`sprintf_s` invokes the invalid-parameter handler, which by default terminates the process.
Neither buffer can overflow at either site (`%d` of an `int` is at most 11 characters, `%f`
of a `float` at most 46, into `char buf[64]`), so it was latent.

The macro existed because MSVC before 2015 had no conforming `snprintf`. It does now, and
the library already called plain `snprintf` in three other places, so the indirection was
not even applied consistently. Both sites now call `snprintf`; both definitions deleted.

`attr_set_float()` has no callers anywhere in the tree and was left in place at the user's
direction. It is the only reason `buf` needs to be 64 bytes.

### 2.14 Remaining platform macros dispersed — DONE 2026-09-10

`platform.h` held five macros after round 9. They went three different ways:

| macro | uses outside `platform/` | disposition |
|---|---|---|
| `START_PACK` / `END_PACK` | 77 each, in 7 files | moved to `utils/macros.h` |
| `MSG_NOSIGNAL` | 6, **all in `utils/socket.c`** | moved into that file |
| `USE_GNU_VARARG_MACROS` | 0 | deleted |
| `USE_STD_VARARG_MACROS` | 0 | deleted |
| `__PRETTY_FUNCTION__` | 0 | deleted |

`START_PACK`/`END_PACK` earned a shared home: `ab/defs.h` uses them 29 times, `omron/defs.h`
27, `ab/pccc.c` 13. `utils/byteorder.h` is also a user, which meant a `utils/` header was
including `<platform.h>` just to declare its own packed types; it now includes
`<utils/macros.h>` directly.

`MSG_NOSIGNAL` did **not** go to `macros.h`. Every use is in one file, it is a socket
concept rather than a general macro, and four other socket implementations in the tree
(`poc/utils/socket.c`, `poc/utils/coro_net.c`, both `fiber_net.c` copies) already define it
locally. Putting it in `utils/socket.c` matches that rather than adding a fifth shared
definition.

The three deleted macros had no users anywhere. `__PRETTY_FUNCTION__` is the interesting
one: the shim defined it for MSVC and MinGW, but nothing in the tree ever referenced it.

Both `platform.h` files now contain no code of their own -- only the include list that
forwards to `utils/`, plus on Windows the winsock include block and the `ssize_t` typedef.
POSIX is 60 lines, of which 32 are the license block.

### 2.15 `<platform.h>` includes swept; shim now unreferenced — DONE 2026-09-11 (round 10, steps 1-2)

Every file now includes the `utils/` headers it actually uses. **Nothing in the tree includes
`<platform.h>` any more.** Both `platform.h` files still exist and are still on the include
path -- deleting them is step 3 and is deliberately deferred, see below.

Note the count: 35 files included `<platform.h>`, not the ~180 claimed earlier in this
refactor (the wrong figure reached a comment inside `platform.h` itself before being caught).

**`ssize_t` is gone from the library.** Only `ab/cip.c` (15) and `omron/cip.c` (16) used it,
and every single use was a `-Wconversion` silencing cast -- `(int)(ssize_t)path_index` and
one `(size_t)(ssize_t)` -- never a declaration. All 31 became `ptrdiff_t` from `<stddef.h>`,
which is standard C99 and the same width. That removed the last reason for the
MSVC-only `typedef SSIZE_T ssize_t` in `windows/platform.h`.

**The sweep was done leaf-first**, `.c` files while the headers still forwarded, then the
headers. That ordering mattered: `utils/rc.h` includes `<platform.h>` and is itself included
by 12 files, so stripping it first would have broken all of them at once.

Transitive dependencies the sweep exposed, each a file using something it never included:

- `utils/hash.h` declared a `size_t` parameter; `utils/hashtable.c` used `NULL` throughout.
  Both had been getting `<stddef.h>` through `<platform.h>`.
- `ab/session.h` and `omron/conn.h` declare `sock_p`, `thread_p`, `mutex_p` and `nap_p`
  members. Neither included `<platform.h>` directly -- they reached it through
  `utils/rc.h`. Both now name the five headers they need.
- `ab/eip_lgx_pccc.c` called `mem_copy()` without including anything that declared it.
- `ab/defs.h` and `omron/defs.h` use `START_PACK`/`END_PACK` on every wire struct and had
  been relying on `<platform.h>` for them; both now include `<utils/macros.h>`.

Six files needed nothing at all and simply lost the include: `ab/error_codes.c`,
`ab/pccc.h`, `system/system.h`, `system/tag.h`, `utils/hash.c`, `utils/rc.h`.

The checkpoint used by rounds 1-9 -- a clean build at zero call-site edits -- does not apply
here, because this round is nothing but call-site edits. What replaced it: a script that
derives, from actual symbol usage with comments stripped, which `utils/` header each file
needs, and compares that against what it includes. It now reports **0 missing, 1 unused**,
the one being `utils/nap.h`'s deliberate include of `utils/mutex.h` for the MSVC `__func__`
shim. That check caught a spurious `utils/mutex.h` the sweep had added to `utils/debug.c`
from the word "mutex" appearing in a comment.

Verification: clean from-scratch build both configs; ASan 181/181; TSan 181/181, all under
`caffeinate -i`.

**Step 3 -- deleting `src/platform/` -- is NOT done, deliberately.** See 3.1: the shim is
the last thing insulating ~35 files from missing-include errors that only MSVC will report.
`windows/platform.h` pulls in `<io.h>`, `<process.h>`, `<malloc.h>`, `<tchar.h>`,
`<strsafe.h>`, `<string.h>`, `<stdlib.h>`, `<errno.h>`, `<math.h>`, `<time.h>` and
`<stdio.h>`; the POSIX header supplies none of those, so any Windows file leaning on one of
them compiles here and breaks there. The equivalent POSIX breakage was found and fixed in
this round because the compiler reported it immediately. Delete the shim after a Windows
build, not before -- it is five minutes of work then, and a remote debugging session now.

---

### 2.16 Callback types made public; extraneous `libplctag.h` includes dropped — DONE 2026-09-11

`tag_callback_func` and `tag_extended_callback_func` were declared in the **private**
`lib/tag.h` while appearing in four **public** `libplctag.h` declarations, so an external
caller could not name the type of the function they were required to pass. Both now live in
`libplctag.h` as `tag_callback_func_t` and `tag_extended_callback_func_t`.

The `_t` suffix is what made the change cheap. The old names collided with the parameter
name `tag_callback_func` used at 23 sites; with the suffix the parameter keeps its name
(`tag_extended_callback_func_t tag_callback_func`) and not one function body needed editing.
All 23 longhand spellings across 17 files were replaced by the typedef.

Seven `utils/` headers included `libplctag/lib/libplctag.h` and used nothing from it --
`async_io.h`, `mem.h`, `nap.h`, `socket.h`, `str.h`, `thread.h`, `time.h`. Four of the seven
mentioned a `PLCTAG_*` constant only inside a doc comment, which is why the include looked
justified. All seven lost it. A from-scratch build is clean, so nothing had been reaching
`libplctag.h` through them.

Two `utils/` headers keep the include and are not defects: `debug.h` uses
`PLCTAG_DEBUG_DETAIL` in the default value of `PLCTAG_COMPILE_DEBUG_LEVEL`, and `mutex.h`
uses `PLCTAG_STATUS_OK` inside the `critical_block` macro body.

Two headers are now documented in `coding_guidelines.md` as deliberate exceptions to the
"include only what you need" rule: `debug_generated.h` is never included directly (everything
that logs includes `debug.h`), and `libplctag.h` holds the types external callers need.

Verification: clean from-scratch build both configs; ASan 181/181; TSan 181/181, no race
reports, all under `caffeinate -i`.

### 2.17 AB vtable headers trimmed; every `.c` now includes its own header — DONE 2026-09-11

The seven `ab/eip_*.h` headers each included `ab_common.h`, and the audit had them down as
also *missing* `lib/tag.h` for `tag_vtable_t`. The second half of that finding was wrong.
`tag_vtable_t` is only ever a **struct tag** -- `struct tag_vtable_t { ... };` at `lib/tag.h:59`,
with no typedef name -- so `extern struct tag_vtable_t plc5_vtable;` declares an object of
incomplete type, which C permits for external linkage and which needs no include whatsoever.
It is the same rule that makes an opaque `typedef struct X *Y;` need none.

What actually drives these includes is `tag_byte_order_t` (a real typedef, `lib/tag.h:151`)
and `ab_tag_p` (`ab_common.h:41`):

| header | uses | action |
|---|---|---|
| `eip_cip_special.h` | `ab_tag_p`, `tag_byte_order_t` | none; already correct |
| `eip_cip.h` | `ab_tag_p`, `tag_byte_order_t` | added `lib/tag.h` |
| `eip_plc5_pccc.h` | `tag_byte_order_t` | swapped `ab_common.h` for `lib/tag.h` |
| `eip_slc_pccc.h` | `tag_byte_order_t` | swapped `ab_common.h` for `lib/tag.h` |
| `eip_lgx_pccc.h` | nothing | include deleted |
| `eip_plc5_dhp.h` | nothing | include deleted |
| `eip_slc_dhp.h` | nothing | include deleted |

Three of the seven now include nothing at all. No `.c` file needed a compensating include.

**Every `.c` file in the tree now includes its own header.** Five did not: the four PCCC/DH+
vtable files, which included only `ab/tag.h` and reached everything else through it, and
`utils/hash.c`, which *defines* `hash()` and had never seen the `extern uint32_t hash(uint8_t
*k, size_t length, uint32_t initval);` in `utils/hash.h`. Nothing was checking those
definitions against their declarations; a drifted signature would have linked and failed at
run time. Both builds stayed clean, so no mismatch was actually present -- the check simply
was not being made. The rule is now in `coding_guidelines.md`.

Still open in the four PCCC/DH+ `.c` files: each is an 18-line vtable initializer whose
remaining symbols (`ab_tag_abort_request`, `ab_get_int_attrib`, the sibling
`pccc_*_tag_read_start` functions) all arrive transitively through `ab/tag.h`. They define
`struct tag_vtable_t`, which needs the complete type, so `ab/tag.h` is doing real work there
-- but it is doing it as a funnel, not as a named dependency.

Verification: clean build both configs, 0 warnings, 0 errors. The `.c` own-header change is
itself a compile-time check and it passed -- no definition had drifted from its declaration.

Three suite attempts during this round were lost to host suspend (147s of work across 88
minutes, 99s across 4h39m, 106s across 4h) and were abandoned; all nine distinct failures in
them were timing-class (12, 18, 68, 69, 112, 114, 171, 172), with every functional test,
every stress test and every TSan race check clean. **The simulator and hardware suites were
subsequently re-run by the user on a host that stayed awake and both passed with no errors**,
covering 2.16, 2.17 and 2.18. See 3.3 for the host-suspend detector.

### 2.18 `rc.h` dropped from the session headers; `attr` and `plc_type_t` named where used — DONE 2026-09-12

Three audit items, all in the protocol headers.

**Extraneous `utils/rc.h` removed** from `ab/session.h` and `omron/conn.h`. Neither header
names a single `rc_*` symbol. Both were acting as a funnel: a tree-wide scan -- comments and
string literals stripped, then `rc_(alloc|inc|dec|weak_inc|deref)(` call sites counted --
found **eight `.c` files** taking the declarations transitively through them:

| file | `rc_*` call sites |
|---|---|
| `omron/omron_common.c` | 14 |
| `ab/ab_common.c` | 14 |
| `omron/conn.c` | 11 |
| `ab/session.c` | 11 |
| `ab/pccc.c` | 10 |
| `omron/omron_standard_tag.c` | 4 |
| `omron/omron_raw_tag.c` | 2 |
| `ab/eip_lgx_pccc.c` | 2 |

Each gained `#include <utils/rc.h>` in alphabetical position. The scan was written rather
than iterating on compiler errors, so the list is the whole set and not just what the first
build happened to reach.

**Missing `utils/attr.h` added** to `ab/session.h` (`attr` at line 201), `omron/conn.h`
(line 194) and `ab/ab_common.h` (lines 100-101, `get_plc_type` and `check_cpu`).
`omron/omron_common.h` names no `attr` and correctly got none.

**Missing protocol `defs.h` added** for `plc_type_t`: `ab/tag.h` gained
`<libplctag/protocols/ab/defs.h>` (field at line 74, type at `ab/defs.h:226`) and
`omron/tag.h` gained `<libplctag/protocols/omron/defs.h>` (field at line 72, type at
`omron/defs.h:209`).

**Fallout from the previous round.** `src/tests/unit/test_atomic_utils.c` uses
`PLCTAG_STATUS_OK` at lines 199 and 203 and had been getting it through `utils/thread.h`,
which lost its `libplctag.h` in 2.16. It gained `#include <libplctag/lib/libplctag.h>`.
**This surfaced only in the TSan build**: `unit_tests` is not in the ASan `all` target, so
`cmake --build build` alone never compiles the unit tests. Build both configurations, or
name the target, before calling an include sweep clean.

Verification: clean build both configurations, 0 warnings, 0 errors. Simulator and hardware
suites re-run by the user, both passed with no errors.

### 2.19 Hardware runners reconciled; `run_hardware_tests.sh` deleted — DONE 2026-09-12

The two hardware runners had silently drifted apart while `run_hardware_tests.py` still
claimed in its docstring to be a port with the "same test list". It was not. Neither file was
a superset, so "just use the Python one" would have quietly dropped four tests:

| test | was only in |
|---|---|
| ST data file PLC5 tag write | `.sh` |
| ST data file PLC5 tag read | `.sh` |
| extended callbacks with async tag creation (ControlLogix) | `.sh` |
| shutdown with reads and writes in flight (ControlLogix) | `.sh` |
| PCCC-mapped Logix tag read/write (slot 5) | `.py` |

The PCCC one matters most: `plc=lgxpccc` against a ControlLogix slot 5 is the only thing in
either suite that reaches `eip_lgx_pccc.c`.

All five were ported across, in matching order. Both files now run the same 39 tests with the
same gateways and tag strings -- verified mechanically, not by eye: the extracted test-name
lists diff clean, and so do the extracted `--tag=` strings once the Python constants
(`logix_gw` and friends) are substituted and both sides are sorted under the same collation.
`test_callback_ex_async` and `test_shutdown` were added to the Python required-executables
list, which had never listed them.

`run_hardware_tests.sh` was then **deleted**: nothing outside the repo invoked it, and two
lists that have to be kept in step by hand is how they drifted in the first place. The five
tests were ported across first so nothing was lost with it. `run_hardware_tests.py` is the
only hardware runner. This mirrors the simulator side, where the `.sh` runners are already
outdated and `run_simulator_tests_parallel.py` is what gets used.

Aside from the list, the two differed in kind. The `.sh` was 38 copy-pasted ten-line blocks with
the lab IPs inlined about forty times, a `$VALGRIND` prefix hook, hand-inverted `if [ $? == 0 ]`
for the one expected-failure test, and counts-only output. The `.py` is a declarative
`Test`/`Result` manifest with the addresses named once, an `expect_failure` flag, opt-in
`--workers` parallelism (default 1, because these are shared lab devices of unknown
concurrent-connection tolerance), per-test timing, and a failed-test list with log paths. It
shares its model with `run_simulator_tests_parallel.py`.

Verification: the two lists were diffed mechanically before the deletion, and the Python
manifest builds and resolves all 39 executables against `build/bin_dist`. The suite was not
run -- no lab network from here.

### 2.20 `src/tests/utils/` audited; ten dead files deleted — DONE 2026-09-12

The directory held eight units. Two are live: `compat_utils.c/.h`, compiled into 37 test
targets, and `stats.c/.h`, compiled into `test_fairness`, `perf_benchmark` and
`test_auto_read`. (An earlier note claiming no CMakeLists referenced the directory at all was
wrong.)

The other ten files -- `args.c/.h`, `bitarray.h`, `buf.c/.h`, `err.c/.h`, `log.c/.h`,
`log_modules.def`, `utils.c/.h` and `hashtable/test_hashtable.c` -- were deleted. **2233
lines.** No CMakeLists compiled any of them and nothing in the tree included them.

Two things settled it:

- Every dead `.c`/`.h` was **byte-identical** to its `src/poc/utils/` counterpart, except
  `log_modules.def`, which was the stale one: it lacked `FIBER_NET`, `MODBUS3_CLIENT` and
  `MODBUS3_LISTENER`. An abandoned fork that had stopped tracking.
- `hashtable/test_hashtable.c` included `../../util/debug.h` and `../../util/hashtable.h`.
  There is no `src/util/` -- it could not compile.

This directory is also where the `scan_eip_network.c` bug in 2.17 came from: `poc/utils/` and
`tests/utils/` carried the same filenames, so a wrong include prefix resolved to a plausible
path instead of failing loudly. Half that trap is now gone.

Verification: clean build both configurations, 0 warnings, 0 errors.

### 2.21 CIP error decoding extracted to `protocols/cip/` — DONE 2026-09-12

First step of the layer extraction agreed on 2026-09-12: transport / protocol / devices, flat
modules with composition expressed in code rather than in the directory tree. Layers are
ranked for extraction by **how many independent callers already exist**, since a layer with
one caller is speculation. Error decoding ranked first: two callers, no state, and the two
copies measured **100% identical** -- 291 lines each, zero divergence.

- `src/libplctag/protocols/ab/error_codes.[ch]` -- 382 lines, deleted.
- `src/libplctag/protocols/omron/cip.c` lines 1165-1455 -- the same 291 lines inline, deleted.
- New `src/libplctag/protocols/cip/error_codes.[ch]`, with its own `CMakeLists.txt`
  exporting `CIP_PROTOCOL_SOURCES` the way `ab/` exports `AB_PROTOCOL_SOURCES`.

Converted to the guidelines on the way across: `int` to `int32_t` through the table struct,
the lookup and `decode_cip_error_code`; explicit `extern` on the three entry points; the `.c`
includes its own header first. `omron/cip.h`'s `CIP` vtable slot changed to
`int32_t (*decode_cip_error_code)(...)` to match.

Callers repointed: five in `ab/` (`pccc.c`, `session.c`, `eip_cip.c`, `eip_cip_special.c`,
`eip_lgx_pccc.c`) and two in `omron/` (`conn.c`, `omron_standard_tag.c`), the latter two
having reached `cip_error_data_size` through `omron/cip.h` before.

**Two findings.**

The audit item that motivated this -- "`omron/cip.h` calls AB's `decode_cip_error_*`" -- was
a **false positive**. No Omron file includes anything from `ab/`; Omron had its own full
copy. There was never a layering violation, only duplication. Recorded here so it is not
re-flagged.

Omron's copy of the `cip_error_data_size` inline helper was **stale and carried a bug AB had
already fixed**: it compared pointers directly (`data < buf_end`), which is UB when the two
do not point into the same object, where AB's compares the integer values. Two copies, one
patched, and nothing to make them disagree loudly. That is the argument for the whole
exercise in one line.

Verification: clean build both configurations, 0 warnings, 0 errors. Simulator suite
**182/182**, 184s reported against 184s of wall clock -- no host suspend.

### 2.22 EIP and CIP constants split out of the two `defs.h` copies — DONE 2026-09-12

Second extraction step. `ab/defs.h` and `omron/defs.h` were 965/904 lines at 92% identical
once the `AB_`/`OMRON_` prefixes were normalised. This step moves **constants only**; the 56
wire structs stay where they are (see below).

- New `protocols/eip/defs.h` -- 12 macros: encapsulation commands, version, default port and
  timeout, and the four CPF item types.
- New `protocols/cip/defs.h` -- 63 macros: CIP service codes, status and error codes, the 35
  data types, connection parameters, transport class, tick/timeout/vendor/RPI values.
- 1002 call sites renamed across both modules. Every one is a value used in C code, so a
  missed rename is a compile error, not a silent zero.

The old names were wrong in a way the split exposed: **`AB_EIP_CMD_CIP_READ` is CIP service
0x4C, not an EIP command.** Everything under `AB_EIP_CMD_CIP_*` and the Forward Open/Close
and Unconnected Send service codes were EIP-prefixed CIP. They are `CIP_CMD_*` now.

**What the split discovered.**

- Omron had renamed the EIP *session* handle to *conn* throughout -- `OMRON_EIP_REGISTER_CONN`
  for encapsulation command 0x0065, `encap_conn_handle` for the field. The EtherNet/IP
  specification calls it a session handle. The spec name won for the two commands; the struct
  field rename is deferred with the structs.
- **22 dead macros in `omron/defs.h`**: every `OMRON_PCCC_DATA_*`, `OMRON_EIP_PLC5_*`,
  `OMRON_EIP_SLC_*` and `OMRON_EIP_PCCCLGX_*`. Omron inherited AB's entire PCCC constant set
  and never used one of them. Deleted.
- Omron lacks `CMD_CIP_READ_FRAG`/`WRITE_FRAG` (0x52/0x53) -- a real difference, not a naming
  one. AB uses them in 12 places in `eip_cip.c`.
- `AB_EIP_SLC_RANGE_WRITE_MASK_FUNC` and `OMRON_EIP_SLC_RANGE_BIT_WRITE_FUNC` are the same
  value (0xAB) under different names. Both PCCC, both now AB-only.

**Deliberately not moved.** PCCC constants stay in `ab/defs.h`: with Omron's dead copies gone
PCCC has exactly one caller, and the rule for this whole exercise is that a layer with one
caller is speculation. `protocols/pccc/` waits for a second.

**Deferred to a later step: the 56 wire structs.** They are composites -- an EIP encapsulation
header wrapping a CPF wrapping a CIP request -- so they do not belong wholly to either module,
and moving them requires the `encap_conn_handle` to `encap_session_handle` rename through
Omron. Two things found while measuring them, to be dealt with then:

- `eip_cpf_uc_header` and `eip_cpf_co_header` (53 lines) exist only in AB.
- The AB and Omron copies of the *same* Forward Open struct disagree in a comment:
  AB says `cm_service_code` is `ALWAYS 0x5B Extended Forward Open`, Omron says
  `ALWAYS 0x54 Forward Open`. One comment is wrong about the struct it sits in.

Verification: clean build both configurations, 0 warnings, 0 errors. Simulator suite
**182/182**, 183s reported against 183s wall clock.

### 2.23 EIP and CIP wire structs unified — DONE 2026-09-12

Completes 2.22. The 56 packed wire structs were the deferred half; they are now 19 shared
definitions plus what is genuinely one-sided.

Three things had to happen before the structs could be compared at all:

- **Omron's `encap_conn_handle` renamed to `encap_session_handle`** (26 sites), along with
  `eip_conn_reg_req` to `eip_session_reg_req`. Omron had renamed the EtherNet/IP *session*
  handle to *conn* throughout; the specification calls it a session handle, so the spec name
  won, matching the decision already taken for the two encapsulation commands in 2.22.
- **Eight dead PCCC structs deleted from `omron/defs.h`** -- `eip_pccc_req_old`,
  `eip_pccc_resp_old`, `pccc_req`, `pccc_resp`, `pccc_dhp_req`, `pccc_dhp_resp`,
  `pccc_dhp_co_req`, `pccc_dhp_co_resp`. Not one had a use anywhere in `omron/`, and three of
  them were already commented out. Same story as the 22 dead PCCC macros in 2.22: Omron
  inherited AB's whole PCCC surface and used none of it.

With that done the 19 shared structs differed only in whitespace and comments -- except one:

**`eip_forward_open_request_ex_t` carried contradictory comments.** AB said `cm_service_code`
is `ALWAYS 0x5B Extended Forward Open Request`; Omron said `ALWAYS 0x54 Forward Open Request`
on the same field of the same extended struct. Omron's *code* sets it to
`CIP_CMD_FORWARD_OPEN_EX` (0x5B) at `omron/conn.c:2794`, so the comment was simply wrong.
AB's text was kept.

The move:

| target | structs |
|---|---|
| `protocols/eip/defs.h` | `eip_encap`, `eip_session_reg_req`, and the four `cpf_*` address and data items |
| `protocols/cip/defs.h` | `cip_header`, the two `cip_multi_*` headers, Forward Open (plain and extended) and Forward Close request/response, `eip_cip_co_generic_response`, and the four `eip_cip_{co,uc}_{req,resp}` frames |

The CIP frames are composites -- an EIP encapsulation header wrapping a CPF wrapping a CIP
request -- so they belong wholly to neither layer. They are in `cip/` because the payload is
what varies and the encapsulation is a fixed prefix; `cip/defs.h` is "CIP as carried over
EtherNet/IP", which is what this library actually speaks.

**Still one-sided, deliberately left in `ab/defs.h`:** `eip_cpf_uc_header` and
`eip_cpf_co_header` (AB-only, 53 lines) and the PCCC structs, which now have exactly one
caller. `protocols/pccc/` waits for a second, same rule as 2.22.

`omron/defs.h` is **1060 lines down to 111** and contains no wire structs at all -- only
`plc_type_t`, the Omron connection parameters and a handful of device constants. That is what
an Omron device module should have been from the start.

Verification: clean build both configurations, 0 warnings, 0 errors. Simulator suite
**182/182**, 181s reported against 181s of wall clock (10:38:48 to 10:41:49). Packed wire
layouts are exactly what the simulator suite exercises end to end.

### 2.23a `cip.c` split: path/tag-name encoding and the type table shared — DONE 2026-09-12

Third extraction step, and the first to move real logic rather than declarations.
`ab/cip.c` and `omron/cip.c` held the same ten functions and the same 260-entry CIP type
table under different names. Normalising the debug module name and the tag type, **every one
of those functions was character-identical** apart from clang-format line wrapping --
`DEBUG_MODULE_OMRON_CIP` is two characters longer than `DEBUG_MODULE_AB_CIP`, so the
formatter broke different lines.

New `protocols/cip/cip.[ch]`, 1043 lines: `match_numeric_segment`, `match_ip_addr_segment`,
`match_dhp_addr_segment`, `skip_whitespace`, `parse_bit_segment`, `parse_symbolic_segment`,
`parse_numeric_segment`, `cip_encode_tag_name`, `cip_lookup_encoded_type_size`,
`cip_lookup_data_element_size`, and the type table. `ab/cip.c` is 1166 lines down to 170 and
`omron/cip.c` 1455 down to 146.

**The seam turned out to be six fields, not a vtable.** The tag-name encoder touched exactly
`tag_id`, `elem_count`, `encoded_name`, `encoded_name_size`, `is_bit` and `bit` -- the same
six in both modules. So it takes a `cip_tag_name_t` holding those instead of a vendor tag
pointer, and each module keeps a six-line adapter that fills one in and copies the three
outputs back. `is_bit` is a bitfield in `TAG_BASE_STRUCT`, so its address cannot be taken;
the context carries values, not pointers. No dialect vtable was needed or written.

**`cip_generic_t CIP` deleted.** Omron's struct-of-function-pointers had exactly one
implementation, and once error decoding (2.21) and everything above had moved, every slot but
`encode_path` was gone with them. Its call sites now call the shared functions directly and
`encode_path` is `omron_encode_path`. One implementation is not an interface.

**`encode_path` stays in both modules and is the one genuine behavioural difference.** AB's
handles DH+ routing for PLC5/SLC/MicroLogix and returns `PLCTAG_ERR_BAD_PARAM` for a DH+ path
on any other PLC type; Omron's is AB's with that whole branch deleted, so it needs no
`plc_type` argument. Worth noting: **Omron still calls `match_dhp_addr_segment` and sets
`is_dhp`, then ignores it** -- a DH+ path that AB rejects will silently produce a wrong
connection path on Omron. Not touched here; it is a behaviour change, not a move.

Also moved: `MAX_TAG_NAME`, `MAX_CONN_PATH` and `MAX_IP_ADDR_SEG_LEN` to `cip/defs.h`, from
four separate definitions in `ab/{tag,session}.h` and `omron/{tag,conn}.h`. New public
`PLCTAG_MODULE_CIP = 28` so the shared layer logs under its own module; the enum is a
sequential index and this is appended, so it is additive.

**One real regression, caught by the suite.** My word-level `tag` to `ctx` substitution
rewrote the word inside string literals and comments as well as code, so
`"Encoded tag name is too long"` became `"Encoded ctx name is too long"`. Tests 38 and 91
match on that text and failed -- 180/182. Fixed by restoring `tag` in 18 places inside
literals and comments, then diffing every string literal in `cip/cip.c` against
`HEAD:ab/cip.c`: the only ones absent are the eleven belonging to `cip_encode_path`, which
stayed behind. Nothing else was altered.

Verification: clean build both configurations, 0 warnings, 0 errors. Simulator suite
**182/182**, 183s reported against 183s wall clock (11:13:17 to 11:16:20).

### 2.23b Table-driven test for `cip_encode_path` — DONE 2026-09-12

`src/tests/unit/test_cip_path_encode.c`, wired into CTest as `unit_test_cip_path_encode` and
into the `unit_tests` target. Byte-exact expectations for 22 paths plus an over-long case.

This exists because the next step on `cip.c` is a **rewrite, not a move**. Every extraction
so far was safe because the code was provably identical before and after; a recursive descent
parser over port/node pairs has no such guarantee, and path encoding is close to untested --
the simulator suite runs `1,0` and little else, and the DH+ and CIP-bridging paths appear only
against real hardware. Every expectation in the table was read out of the current
implementation rather than derived from the CIP specification, so a failure after the rewrite
means behaviour changed. That may be a fix; it will not be silent.

The table covers the backplane paths (`1,0` connected and unconnected, `1,4`, `1,5`), odd
length padding, spaces, the empty path, the extended-address IP form
(`18,10.206.1.39` and the full bridge path `1,4,18,10.206.1.39,1,0` from the hardware suite),
the legacy DH+ item in every accepted spelling, and eight rejections.

**Three things the table pinned that were not previously written down.**

- **The `'2'` and `'3'` aliases in `match_dhp_addr_segment` are unreachable.** The driver
  tries `match_numeric_segment` first, which consumes the leading digit as a one-byte segment
  and leaves `:27:1`, which matches nothing. I expected `2:27:1` to encode as channel A; it
  returns `PLCTAG_ERR_BAD_PARAM`. The alias arms are dead code. Pinned as the rejection it is.
- **Ports 20 and 21 (channels A2 and B2) have no spelling at all.** `match_ip_addr_segment`
  accepts only 18 and 19, and `match_numeric_segment` caps at 0x11, so `20,10.206.1.39` is
  rejected twice and reported as a parse failure.
- **A numeric segment above 0x11 reports a parse failure, not a range failure.** The driver
  treats `PLCTAG_ERR_OUT_OF_BOUNDS` from the numeric matcher exactly like `NOT_FOUND` and
  falls through to the remaining matchers, so `1,99` ends as
  "Unable to parse remaining path string".

The DH+ source node is also pinned as accepted, range checked and then ignored -- `A:99:1`
encodes identically to `A:27:1`, because nothing ever reads the value and the wire always
carries zero (`pccc.c`, four sites).

Verification: passes under both the ASan and TSan builds and under CTest. Mutation checked --
changing the DH+ logical segment class from 0xA6 to 0xA7 fails the table with a byte-level
diff, and the library file was restored afterwards.

### 2.24 `utils/byte_buf.h`: a length-carrying byte buffer — DONE 2026-09-12

Groundwork for safer wire encoding and decoding, and the output side of the path rewrite.
Header-only, passed by value, 395 lines.

The length is **signed**. A negative length means the buffer carries an error rather than
data: the length is the `PLCTAG_ERR_*` code and the data pointer is a static message saying
where it happened. Every operation checks that first and passes it through untouched, so a
chain of encodes needs one check at the end instead of one per field. Decoding returns the
data that is left; encoding returns the space that is left.

**Why not `ab_server/slice.h`**, which does something similar and has sixteen users:

- An out-of-bounds `slice_get_uint8` returns `UINT8_MAX` -- a legitimate data byte. A
  truncated response decodes to plausible `0xFF` values instead of an error.
- Out-of-bounds writes are silent no-ops. The header says so itself: *"FIXME - these probably
  should not just fail silently."*
- Its error slice is `data == NULL` with the code stuffed in `len` as a `size_t`, so
  `slice_len()` on a failure returns a huge positive number.
- It is random access -- every call takes an explicit offset -- which is the index
  bookkeeping the path rewrite exists to delete.
- It carries `slice_set_uin64_le`, misspelled and uncalled.

`byte_buf` keeps the good idea (a length travelling with the pointer) and fixes each of
those. It is not shared with `ab_server`; per the standing rule that code gets copied and
converted when its turn comes.

**Splitting.** `byte_buf_split_front()` returns everything before the split index and
`byte_buf_split_back()` returns the split index to the end -- two calls rather than one
returning a struct, so no result type is needed. They exist so an outer header can be written
*after* the payload it describes: a CIP packet's EIP header carries a length that is not known
until the payload exists. Unlike `byte_buf_slice()` they do **not** clamp -- a split index past
the end returns `PLCTAG_ERR_TOO_SMALL`, because a reservation that silently came back short
would then be filled past its end. `byte_buf_written()` recovers how much a chain consumed
without the caller doing the subtraction by hand.

Both byte orders at 8/16/32/64 bits, plus bulk `encode_bytes`/`decode_bytes`. The multi-byte
paths funnel through one generic LE and one generic BE helper so the bounds check and the
byte ordering each live in exactly one place; the fixed-width entry points are generated from
those by macro.

Bounds checks are written as `(len - offset) >= count` rather than `offset + count <= len`,
so the addition cannot overflow.

Three defects were found by compiling the design sketch before writing it up: the multi-byte
peeks still called the two-argument `peek_uint8` from `slice.h` and used its `bool` return as
a data byte (a hard compile error in three functions), the multi-byte peeks took `size_t`
offsets against `intptr_t` everywhere else, and one had an unused local.

`src/tests/unit/test_byte_buf.c`, wired in as `unit_test_byte_buf`: six tests covering the
round trip with byte order actually checked in the raw bytes, error propagation through a
chain including the message and the untouched output value, bounds refusal at every width
plus the exactly-enough cases, the reserve-then-fill split, over-reservation, and bulk bytes.

Verification: clean under both configurations, 0 warnings, 0 errors; 6/6 under ASan and TSan
and under CTest.

**Not yet done:** `cip/path.c` is written against this next. `cip/path.h` and the buffer and
cursor primitives drafted before this existed are discarded -- `byte_buf` replaces
`cip_path_buf_t` outright, and the cursor stays separate because a path is text, not bytes.

### 2.25 `cip/path.c`: path encoding rewritten as a port/node parser — DONE 2026-09-12

The first genuine rewrite in this sequence rather than a move. `cip_encode_path()` was a
106-line loop that tried three matchers in turn and took whichever succeeded; it is now a
recursive descent parser over the grammar it was always implementing, in
`protocols/cip/path.[ch]` (422 + 117 lines), with AB's parts behind hooks.

```
path      := [ item ( ',' item )* ]
item      := port ( port_tail | separator node | <nothing> )
port      := number | letter [ '2' ]
node      := ipv4 | number
separator := ',' | '/'
```

One character of lookahead after the port picks between a device specific tail and a node, so
only the node backtracks -- and the cursor is an index, so that is one assignment.

**`match_ip_addr_segment` is gone.** 159 lines of special case existed only because the
pair abstraction was missing. An IP address is a link address whose length is greater than
one; `emit_port_node()` handles `1,4` and `18,10.206.1.39` in the same six lines.

**What a numeric port means is now written down.** A numeric port is the *encoded port byte*,
not a logical port number -- 18 is 0x12, the extended-link-address bit 0x10 plus port 2 --
so nothing is added to it. A letter port names a channel (A=1, B=2, A2=3, B2=4) and only
becomes an extended-address byte when the node turns out to be an IP address, by adding 0x11.
That offset is empirical, not derived from the specification, and the header says so, because
it looks like a plain bit set and is not.

**`needs_connection` left the encoder.** It was in/out: read to decide whether to append the
router path, written when a DH+ segment appeared. Path encoding does not decide connection
policy. It is now entirely inside AB's `finish` hook, and the generic encoder never sees it.

Functions are small by construction: the cursor is five one-liners, `parse_port_letter` is
20 lines, `parse_ipv4` 30, `parse_node` 25, `emit_port_node` 15, `parse_item` 30. The old
`cip_encode_path` alone was 106.

**Three deliberate behaviour changes**, each recorded in the pinned table from 2.23b:

- **Channels A2 and B2 can be written.** The old encoder took only 18 and 19 as
  extended-address ports, so `20,<ip>` was rejected twice and reported as a parse failure.
  Both `20,10.206.1.39` and `A2,10.206.1.39` now encode to `14 0B "10.206.1.39" 00`.
- **A node address takes the full byte.** Every number used to go through one matcher that
  capped at 0x11, applying a *port's* range to node addresses, so a DeviceNet or ControlNet
  node above 17 could not be written. `1,99` encoded nothing and now encodes `01 63`; `1,256`
  is refused as out of bounds.
- **`/` separates a port from its node** as well as `,` does.

All three widen what is accepted, so no path that used to work has changed meaning.

Two error-reporting fixes fell out. `PLCTAG_ERR_NOT_FOUND` is how the sub-parsers say "not
one of mine" and used to leak out of the public call; it becomes `PLCTAG_ERR_BAD_PARAM` at
the item level. And a write that runs out of room is relabelled `PLCTAG_ERR_TOO_LARGE` at the
three sites where a write can fail, rather than globally -- relabelling globally rewrote a
genuine range error, which the `1,256` case caught.

One bug found by the table during the rewrite: `finish` was running after the path had
already failed, and its relabel then overwrote the original error. It is guarded now.

**Still on the old code: Omron.** `omron_encode_path` is the same loop with the DH+ branch
deleted, and converting it is the next increment; until then `match_numeric_segment`,
`match_ip_addr_segment` and `match_dhp_addr_segment` stay in `cip/cip.c` for it alone.

Verification: clean build both configurations, 0 warnings, 0 errors. The 2.23b table passes
with the three changed expectations updated and eight cases added for what is newly
reachable. Simulator suite **182/182**, 182s reported against 182s wall clock (15:07:47 to
15:10:49). DH+ and CIP bridging remain hardware-only.

### 2.26 Omron converted to the port/node encoder; the old matchers deleted — DONE 2026-09-12

Completes 2.25. `omron_encode_path` now calls `cip_path_encode` with a finish-only hook, and
the three `match_*` functions that were kept alive for it alone are gone:
`match_numeric_segment`, `match_ip_addr_segment` (159 lines) and `match_dhp_addr_segment`.
`cip/cip.c` is 1043 lines down to 681; `omron/cip.c` is 146 down to 110, and its only
remaining content is the path wrapper.

Omron needs no `parse_port_tail` hook -- NJ/NX has no DH+ bridging -- so its hooks are a
single `finish` that appends the message router path when the connection needs one. The whole
device-specific part of Omron path encoding is now 20 lines.

**A latent bug is fixed rather than carried over.** The old Omron encoder *did* parse the DH+
form `A:src:dest` and set `is_dhp` from it, and then ignored the result: the routing AB
appends for such a path was never emitted. A DH+ path that AB rejects outright produced a
silently wrong connection path on Omron. It is now an error, because only a device that
supplies the hook can parse that form.

**Two dead fields deleted.** `conn->is_dhp` and `conn->dhp_dest` in `omron/conn.h` were
written once from that ignored result and read nowhere in the module, so the two out
parameters went with them: `omron_encode_path` is down from six arguments to four.

Also retired with the matchers: the `'2'` and `'3'` channel aliases recorded as unreachable in
2.23b. Only a channel letter introduces a DH+ item now, which keeps the rejection the pinned
table expects, for a reason that can be stated in one line instead of an accident of matcher
ordering.

**Hardware coverage for the channel-letter port.** `A,<ip>` is new capability from 2.25 and
the `+0x11` offset behind it is empirical, so `run_hardware_tests.py` gained
`CIP bridging with a channel letter port`: the existing bridge path with `18` replaced by `A`
(`path=1,4,A,10.206.1.39,1,0`). The two spellings must put byte-identical traffic on the
wire, and the unit table now pins them as encoding to the same 18 bytes, so if the letter
test fails while the numeric one passes the offset is wrong rather than the path. That is the
only check of the offset anywhere -- the table pins the bytes, not the PLC's opinion of them.
The hardware suite is 39 tests to 40.

Verification: clean build both configurations, 0 warnings, 0 errors. The pinned path table
passes with the two bridge spellings asserted equal. Simulator suite **182/182**, 179s
reported against 179s wall clock (15:29:11 to 15:32:10). Omron path encoding is exercised by
every Omron test in the suite; AB's DH+ and CIP bridging paths remain hardware-only and the
letter-port test is unrun.

### 2.27 AB's connected/unconnected status checks collapsed — DONE 2026-09-12

Groundwork for the cross-module merge. A function-level similarity map across both whole
modules -- rather than pairing files, which was the wrong unit -- showed each module
duplicating its own connected and unconnected paths as heavily as the two modules duplicate
each other:

| pair | similarity | lines |
|---|---|---|
| AB `check_read_status_connected` / `_unconnected` | 91% | 167 / 164 |
| AB `check_write_status_connected` / `_unconnected` | 93% | 42 / 42 |
| Omron `check_read_status_connected` / `_unconnected` | 89% | 157 / 178 |
| Omron `check_write_status_connected` / `_unconnected` | 85% | 49 / 45 |

So `check_read_status` existed in **four** near-copies of about 160 lines. Collapsing each
module's pair first halves what the cross-module merge then has to reconcile.

AB's two pairs are done. `eip_cip.c` is 1955 lines down to 1724.

**The write pair differed only by a cast.** Both used just `reply_service` and `status`, and
those four bytes are laid out identically in `eip_cip_co_resp` and `eip_cip_uc_resp` -- the
same sequence `cip_header` already describes. The shared check takes a `cip_header *` and two
four-line entry points say where it is.

**The read pair needed a trace.** Connected and unconnected differ in the EIP and CPF headers
ahead of the CIP reply, so both the reply header position and the payload start are passed
in. They had also drifted in the tail:

```
/* connected */                        /* unconnected */
if(rc != PENDING) {                    if(rc != OK && rc != PENDING) {
    if(rc != OK) { log; }                  log;
    ab_tag_abort_request(tag);             ab_tag_abort_request(tag);
}                                      }
```

The connected form aborts on success as well. The worry was the pre-write read: that branch
calls `tag_write_start()`, which builds and registers a new request, and aborting afterwards
would release it. Tracing both entry points settles it -- **neither `tag_read_start()` nor
`tag_write_start()` ever returns `PLCTAG_STATUS_OK`**; each returns `PLCTAG_ERR_BUSY`, an
error, or `PLCTAG_STATUS_PENDING`. So `rc == OK` at the tail means the read finished and
started nothing else, and by then `ab_tag_abort_request_only()` has released the request and
cleared the flags and the branch above has zeroed the offset. The extra call is a no-op and
the two tails are equivalent. AB's connected form was kept: more exercised, and it reports
the error rather than logging a bare "Error received!".

For the record, since it was the thing that made this look risky: the creation-time read
(`ab_common.c:619`) is what normally clears `first_read`, in the tickler. The pre-write read
only happens when a write is issued before any read has completed.

**Omron's two pairs followed**, and `omron_standard_tag.c` is 1830 lines down to 1666. The
write pair was the same cast-only difference, plus a null check in the connected form that
dereferenced `tag->tag_id` *inside* its own `if(!tag)` branch -- it would have crashed before
it could return `PLCTAG_ERR_NULL_PTR`. Neither the unconnected form nor AB has one, so it is
gone.

Omron's read pair carried a real asymmetry: the **unconnected** form validated the EIP
encapsulation header and the connected form did not. Resolved by splitting it in two.

- The `encap_command` test is **redundant** and was dropped. `conn.c:2548` already rejects any
  response whose EIP command does not answer the command that was sent -- the same central
  check AB has at `session.c:2826`, which is what `test_eip_response_validation` covers. A
  mismatched command cannot reach the tag layer.
- The `encap_status` test is **not** redundant and was kept, now covering both frames. Nothing
  in either module checks the encapsulation status of an ordinary response; only this one
  branch did. **This is a deliberate behaviour change**: an Omron *connected* read that
  receives a non-zero encapsulation status now fails with `PLCTAG_ERR_REMOTE_ERR` instead of
  being parsed as CIP. AB has no equivalent check on either path and was left alone -- worth
  revisiting when the two modules merge.

Verification: clean build both configurations, 0 warnings, 0 errors. Simulator suite
**182/182** after each of the four collapses -- 179s reported against 179s wall clock every
time.

### 2.28 Three of the five identical cross-module pairs shared — DONE 2026-09-12

Working the similarity map from 2.27, starting with the five pairs that were identical once
the debug module name and tag type were normalised. Three shared cleanly; two did not, and
why is the useful part.

**Shared, in new `protocols/cip/request.[ch]`:**

- `request_destroy` -- the `rc_alloc` destructor.
- `session_request_increase_buffer` / `conn_request_increase_buffer`, now
  `cip_request_increase_buffer`.

Both touch nothing but the request, and the request struct was already unified in 2.26, so
these moved with no seam at all.

**Shared, in new `protocols/cip/tag.c`:** `default_status`, now `cip_default_tag_status`.
It uses only `TAG_BASE_STRUCT` fields. Both copies logged `tag->tag_id` *before* testing
`tag` for null, so a null tag would have crashed on the way to reporting itself; the test
comes first now. Also worth noting both were declared `static` at the top of their file and
defined without it -- legal, C keeps the internal linkage from the visible prior declaration,
and `nm` confirms no external symbol, but it reads like a duplicate definition and is not.

**Not shared, deliberately:** `ab_tag_status` / `omron_tag_status`, and `raw_tag_write_start`.

Both are character-identical. Both are blocked by the same thing: they read
`read_in_progress`, `write_in_progress` and `use_connected_msg`, **none of which are in
`TAG_BASE_STRUCT`** -- they live in the module-specific tag structs. A shared
`cip_tag_status` would need four parameters to save twelve lines, and a shared
`raw_tag_write_start` would need three field parameters plus two builder callbacks. Each is
left with a comment saying so.

**That is the finding.** Identical text is not what makes a function shareable; whether its
*data surface* is already shared is. The three that moved touch only the request or only the
tag base. Everything else in the map is gated on the two tag structs, which differ by three
fields and already have `CIP_TAG_STRUCT` waiting for them in `cip/tag.h`. Unifying them is
the next step and it unblocks the rest of the list at once.

Verification: clean build both configurations, 0 warnings, 0 errors. Simulator suite
**182/182**, 179s reported against 179s wall clock (16:56:56 to 16:59:56).

### 2.29 The two tag structs unified behind `CIP_TAG_STRUCT` — DONE 2026-09-12

The gate identified in 2.28. `ab_tag_t` and `omron_tag_t` had the same twenty-odd fields in
the same order, differing by three, so both now expand a shared macro and declare only what
is genuinely theirs:

```c
struct ab_tag_t {
    TAG_BASE_STRUCT;
    CIP_TAG_STRUCT;

    /* AB specific from here on. */
    plc_type_t plc_type;
    ab_session_p conn;
    uint16_t req_pccc_seq_num;
    pccc_file_t file_type;
};
```

Omron's is the same with `omron_conn_p conn` and nothing else. `struct cip_tag_t` in
`cip/tag.h` is `TAG_BASE_STRUCT` plus `CIP_TAG_STRUCT`, which is the common initial sequence
both structs begin with, so shared code takes a `cip_tag_p` and each module casts -- the same
pattern the tree already uses to cast `plc_tag_p` to a module tag.

**Two fields stayed out of the macro**, for the same reason: their types are module specific.
`conn` is `ab_session_p` or `omron_conn_p`, and `plc_type_t` is a *different enum in each
module*. Nothing shared reads either -- the connection arrives as a parameter, and the
similarity map showed `eip_cip.c` has no live PLC-type branch at all (its only three are
commented out, and all three test `AB_PLC_OMRON_NJNX`).

**`elem_type_t` was shared too**, as `cip_elem_type_t` with `CIP_TYPE_*` names -- the same
eighteen entries under two prefixes, 58 uses renamed. AB's list had one extra,
`TAG_IDENTITY`, which is now simply an enumerator Omron does not use. Care was needed on the
first value: neither original had an `UNKNOWN = 0`, so `CIP_TYPE_BOOL = 0` is pinned with a
comment. An inserted zero would have shifted every value.

**AB gained `supports_fragmented_operations`, set to 1.** Only Omron's packing logic reads it and
AB's ignores it entirely. The value is right but the reasoning first recorded here was not:
the field is not a statement about fragmentation capability, it is **Omron's guard for not
having one**. Omron cannot recover from a packed response that overflows, so it refuses to
pack past the remaining space; AB never needs that arithmetic because a PLC that runs out of
room answers with CIP status `0x06` and AB asks for the next fragment
(`eip_cip.c:1376`). Setting the flag to 1 therefore means "AB does not need this guard",
which is correct -- see 2.31.

**The two pairs 2.28 had to decline now share.** `ab_tag_status` / `omron_tag_status` became
`cip_tag_status(cip_tag_p, void *conn)` with a one-line vtable wrapper each, because
`read_in_progress` and `write_in_progress` are in the macro now. That was the whole argument
for doing this first.

Verification: clean build both configurations, 0 warnings, 0 errors. Simulator suite
**182/182**, 179s reported against 179s wall clock (17:06:22 to 17:09:21).

### 2.30 `raw_tag_write_start` and the common-file block shared — DONE 2026-09-13

Items 1 and 2 from the re-measured map. `protocols/cip/tag.c` now holds five functions.

**`raw_tag_write_start`** -- the last of the five identical pairs from 2.28, unblocked by the
struct unification. The only thing that varies is which builder runs, so it takes the two as
`cip_build_request_func` callbacks and each module keeps a three-line vtable entry.

**From `ab_common.c` / `omron_common.c`:**

- `encode_tag_name` to `cip_fill_tag_name` -- identical but for `static`. It touches only
  shared fields, being the adapter written in 2.23a.
- `check_cpf_unconnected` to `cip_check_cpf_unconnected` -- identical apart from a parameter
  name.
- `check_cpf_connected` to `cip_check_cpf_connected` -- identical apart from dereferencing
  `tag->conn->orig_connection_id` and `->targ_connection_id`, which are in the module's own
  connection struct, so those two arrive as parameters.

**One constant deduplicated.** The idle-disconnect timeout was defined **four times** --
`ab/session.{c,h}` and `omron/conn.{c,h}` -- as the same expression
`(CIP_CONN_TIMEOUT_MS - 1000)` under two names. It is `CIP_DISCONNECT_TIMEOUT` in
`cip/defs.h` now. That also removes the only difference between `ab_set_int_attrib` and
`omron_set_int_attrib`, which can share whenever their `tag->conn` dereference is handled.

**Four pairs from that block were deliberately left alone**, and the reasons are worth
keeping:

| pair | why not |
|---|---|
| `check_cpu` | dispatches on `plc_type_t`, a different enum in each module |
| `ab_init` / `omron_init` | calls `session_startup()` vs `conn_startup()`; 16 lines |
| `tag_abort_request` | calls the module's own `_only` variant; 15 lines |
| `get_byte_array_attrib` | **a real behavioural difference**: AB gates the attribute on `plc_type` being LGX or Micro800 and reports "Unsupported PLC type" otherwise, Omron has no such gate |

The last one is the only one that matters. It is not a naming difference and should not be
resolved by whichever side a merge happens to take.

Also noted while measuring: the connection fields both modules reach through `tag->conn` have
**the same names on both sides** -- `connection_inactivity_timeout_ms`, `connection_status`,
`orig_connection_id`, `targ_connection_id`, with only AB's `is_dhp` extra. The session and
conn structs look as alignable as the tag structs were, which is encouraging for the
`session.c` / `conn.c` block.

Verification: clean build both configurations, 0 warnings, 0 errors. Simulator suite
**182/182**, 184s reported against 184s wall clock (07:05:12 to 07:08:16).

### 2.31 Fragmentation has two levels, and the capability flag was misnamed — 2026-09-13

Recorded because the plan after 2.30 rested on a wrong reading, twice over.

**Two independent things were being conflated.**

*Within one tag's operation* -- a read or write too large for one packet. Each vendor has its
own mechanism, and the flag now called `supports_fragmented_operations` says only that one
exists:

| family | mechanism | state |
|---|---|---|
| AB CIP | fragmented read/write services with a byte offset; CIP status `0x06` says more follows | implemented |
| Omron NJ/NX | a `0x80` data segment carrying a 4-byte offset and a 2-byte element count | **not implemented** |
| PCCC | none in the protocol; the client would have to chop the operation up | **not implemented** -- `pccc_offset` is `h2le16(0)` at all four sites and a transfer that does not fit returns `PLCTAG_ERR_TOO_LARGE` |

*Across different tags* -- CIP service `0x0A`, Multiple Service Packet, combining requests
from several tags into one packet. That is `allow_packing`, gated per PLC type in
`ab_common.c`: only `AB_PLC_LGX` reads the attribute, while Micro800, GENERIC, LGX_PCCC and
the PCCC families are each forced to 0 with a comment. Omron supports it and reads the
attribute.

**The flag is not dead generality.** An earlier version of this entry said it was. It is a
*level-one capability consulted by a level-two decision*, which is correct reasoning: packing
several responses into one packet risks overflow, and only a PLC that can fragment can
recover, so the flag relaxes the packing size guard. It reads as constant today only because
Omron's mechanism is unimplemented, which pins it to 0 on the one side that consults it.

**Renamed** from `supports_fragmented_read` to `supports_fragmented_operations`: no PLC is
known that fragments reads but not writes, and the flag is about the operation, not the
direction. It stays a bool -- it asserts that a mechanism exists, not which one, because the
mechanism is vendor specific and selected in the builder.

**`multiple_requests` is not a capability either.** In
`*data = (multiple_requests) ? CIP_CMD_WRITE_FRAG : CIP_CMD_WRITE;` it means "this operation
needs more than one request", so the fragmented service is selected because fragmentation is
being *used*. It says nothing about support.

**Re-ranking.** `session.c` / `conn.c` was put ahead of the `build_*` block on the idea that
`pack_requests` would settle the fragmentation question first. It settles nothing of the kind.

**CORRECTION (2026-09-14):** the function carrying the response-space accounting and the
`supports_fragmented_operations` guard is **`process_requests`**, the caller -- not
`pack_requests`. This entry drew its conclusion against the wrong function and on that basis
called `pack_requests` a pair whose difference is real. It is 98% identical between the
modules, differing only in a debug line, and was shared in 2.39. It is pure assembly: whether
requests may be packed at all, and how much room remains, are decided by `process_requests`,
which does still differ.

**Micro800 -- checked, and the code is correct.** It fragments a single operation exactly as
ControlLogix does, and it cannot pack multiple operations into one packet. That is what the
code already does: the CIP builders send `CIP_CMD_READ_FRAG` unconditionally, which is right
for Micro800, and `ab_common.c` forces `allow_packing = 0` for it with the comment "Micro800
cannot pack requests". The commented-out per-PLC selection at `eip_cip.c:521` is therefore
not a missing gate for Micro800; the only PLC it ever excluded was Omron, before Omron was
forked into its own module.

**The two levels are independent, and Micro800 is the proof.** A PLC can fragment a single
operation and still be unable to pack several, so a merged implementation must keep the two
flags separate rather than deriving one from the other.

### 2.32 `cip_plc_config_t`: PLC capabilities as data — DONE 2026-09-13

The Forward Open code differs between the modules only in per-PLC numbers, so those numbers
became a struct the shared code can read instead of asking which vendor it is talking to.

```c
typedef struct {
    int fo_conn_size;                    /* old Forward Open max payload */
    int fo_ex_conn_size;                 /* extended; zero selects the old form */
    int min_payload_size;                /* floor below which no request fits */
    bool supports_fragmented_operations; /* level one: split one operation */
    bool supports_packed_requests;       /* level two: CIP 0x0A, several tags per packet */
} cip_plc_config_t;
```

Every field is a property of the **PLC model, not the vendor** -- Micro800 fragments a single
operation exactly as a ControlLogix does and cannot pack several, and the PCCC families can do
neither. That is why the struct is keyed off nothing: the module fills one in when it works
out the PLC type, and the shared code just reads it.

The two size fields were replaced in `ab_session_t` and `omron_conn_t` by this struct, and the
per-PLC values now sit beside the capability flags in the same place:

| PLC | old FO | extended FO | floor | fragment | pack |
|---|---|---|---|---|---|
| ControlLogix / CompactLogix | 504 | 4000 | 500 | yes | yes |
| Micro800 | 504 | 4000 | 500 | yes | **no** |
| PLC5 / SLC / MicroLogix / lgxpccc | 244 | none | 92 | no | no |
| Omron NJ/NX | 502 | 1990 | 500 | not yet | yes |

`MIN_PAYLOAD_SIZE_PCCC` and `MIN_PAYLOAD_SIZE_CIP` moved from `ab/session.c` to `cip/defs.h`,
and AB's `MIN_PAYLOAD_SIZE(session)` macro -- which re-derived the floor by testing
`plc_type` against four PCCC families on every use -- is gone; the floor is a field now.

**The Omron payload constants are opening bids, not limits.** `ab/session.c` carried a
commented-out Omron pair reading 1994 against Omron's live 1990; the dead pair is deleted and
the live one now says what it is. A PLC that cannot manage the requested size rejects the
Forward Open and reports what it does support, and `receive_forward_open_response()` clamps
the guess to that and retries (the `PLCTAG_ERR_TOO_LARGE` arm of the connection state
machine). So asking too high costs one extra round trip at connect time, while asking too low
is silent, permanent, and costs throughput for the life of the connection -- which is why the
numbers err high.

Neither 1990 nor 1994 is derived from a specification or checked against hardware, and an
NJ/NX is reported to top out nearer **1892**. One connection against a real PLC settles it:
the "unsupported size" branch logs the size the PLC reports. Worth checking whether that
figure is the CIP payload or the whole frame -- `EIP_CIP_PREFIX_SIZE` is 44, so a 1892-byte
frame would mean a payload constant of 1848 -- and whether the limit varies by model, in which
case a single constant is the wrong shape and the negotiation is doing the real work anyway.

**Still to do:** the four Forward Open functions themselves. With the config in place their
remaining differences are the `session_handle`/`conn_handle` field rename, AB's DH+
`AB_EIP_PLC5_PARAM` override for a PCCC PLC reached over DH+, and whitespace.

Verification: clean build both configurations, 0 warnings, 0 errors. Simulator suite
**182/182**, 183s reported against 183s wall clock (10:03:55 to 10:06:59).

### 2.33 The two connection structs unified behind `CIP_CONN_STRUCT` — DONE 2026-09-13

Prerequisite for sharing the Forward Open code, which touches most of the connection struct.
Same treatment as the tags in 2.29.

First, 91 sites in AB were renamed so the two modules agree on field names:
`session_mutex` to `mutex`, `session_nap` to `nap`, `session_handle` to `conn_handle`,
`session_seq_id` to `conn_seq_id`, `session_seq_id_lock` to `conn_seq_id_lock`. After that the
structs held the **same thirty-nine fields** and differed only in what is genuinely each
module's own:

| AB only | Omron only |
|---|---|
| `plc_type` (different enum in each) | `plc_type` |
| `dhp_dest`, `is_dhp` -- DH+ routing | |
| `connection_status_reason` | |
| `conn_status_ring[]` + write index -- status ring with reason codes | `conn_event_ring[]` + write index -- event ring |

The two ring buffers are the interesting residue: both publish connection state to the
module's tags, with different element types, different sizes and different names. That is a
real design divergence, not naming, and it stays until someone decides which shape is right.

New `protocols/cip/conn.h` holds `CIP_CONN_STRUCT`; both structs paste it and append their
own. `plc_type` stays out for the same reason it did in `CIP_TAG_STRUCT` -- two different
enums.

**Found while comparing: the sequence-ID accessors differ in three ways**, and one is a
behaviour difference, not a style one:

```c
/* AB */                                     /* Omron */
spin_block(&session->conn_seq_id_lock) {     critical_block(conn->mutex) {
    if((++session->conn_seq_id) == 0) {          res = (uint16_t)conn->conn_seq_id++;
        session->conn_seq_id = 1;            }
    }
    res = (uint16_t)session->conn_seq_id;
}
```

- AB takes a dedicated spinlock; Omron serialises on the whole connection mutex.
- AB pre-increments and returns the new value; Omron post-increments and returns the old.
- **AB skips zero on rollover; Omron does not.** The sequence ID is what matches a response to
  its request, so a zero could collide with an unset context -- which is presumably why AB
  guards against it.

Both modules also bypass their own accessor in the Forward Open path, incrementing
`conn_seq_id` directly. That is on the connection's own thread during setup, so it is not a
race, but it means the rollover guard does not apply there even on AB.

**Resolved: one shared implementation, AB's logic on a mutex.** `cip_conn_get_new_seq_id()`
in the new `protocols/cip/conn.c` pre-increments, skips zero, and takes `conn->mutex` rather
than a dedicated spinlock. Both modules keep a one-line wrapper because their own names are
what callers use. Consequences:

- **Omron gains the zero guard.** Its sequence IDs could previously roll over to zero, which
  could collide with an unset sender context.
- **AB loses the dedicated spinlock.** `conn_seq_id_lock` had exactly one user and is deleted
  from `CIP_CONN_STRUCT`; the sequence ID is guarded by the connection mutex on both sides
  now, which is coarser but consistent and one lock fewer to reason about.
- **Both `_unsafe` variants are gone.** `session_get_new_seq_id_unsafe` had **no callers at
  all**; `conn_get_new_seq_id_unsafe` had exactly one, the accessor that now shares.

`struct cip_conn_t` and `cip_conn_p` were added alongside `CIP_CONN_STRUCT` for shared code
to take, the same arrangement as `cip_tag_t`.

Verification: clean build both configurations, 0 warnings, 0 errors. Simulator suite
**182/182**, 182s reported against 182s wall clock (10:18:08 to 10:21:10).

### 2.34 Forward Open shared — DONE 2026-09-13

The four Forward Open functions plus a helper now live once, in `protocols/cip/conn.c`.
`ab/session.c` is 3282 lines down to 3094 and `omron/conn.c` 2969 down to 2807.

After the field renames of 2.33 the four were 84%, 91%, **100%** and 92% identical. Four
things had to be dealt with:

- **The module's socket.** `send_eip_request` and `recv_eip_response` belong to whichever
  module owns the socket, so they arrive as a two-entry `cip_conn_io_t` and each module keeps
  one `static const` instance. Both keep the signature the modules already used.
- **AB's DH+ parameter override.** A PCCC PLC reached over a DH+ bridge needs a fixed
  connection parameter word instead of `CIP_CONN_PARAM | max_payload_guess`. That is now
  `cip_plc_config_t.conn_params_override`, worked out once where the session learns it is a
  DH+ route, so the Forward Open code contains no PLC types at all.
- **Two more lock asymmetries, both resolved AB's way.** Omron updated `max_payload_guess`
  outside the mutex in `send_forward_open_request` and again in the size-negotiation branch of
  `receive_forward_open_response`; AB held it in both. The value is read by the request
  builders, so AB's locking is kept and Omron gains it.
- **`next_conn_serial_number` was byte-identical** in both and fell out with the rest. It
  skips zero for the same reason the sequence ID does: a Forward Open uses the serial number
  to tell one connection attempt from another, and a repeated zero could look like a
  duplicate.

`send_extended_forward_open_request` was **character-identical** between the modules once the
names matched -- 59 lines with nothing to reconcile at all.

That is the fourth and fifth one-sided locking or validation fix this block has produced: the
payload floor (Omron lacked it), the connection-ID commit (AB lacked the lock), the sequence
ID rollover guard (Omron lacked it), and now two `max_payload_guess` updates (Omron lacked the
lock). None was visible from inside either module alone.

Verification: clean build both configurations, 0 warnings, 0 errors. Simulator suite
**182/182**, 183s reported against 183s wall clock (11:16:02 to 11:19:06). The DH+ override
path is **hardware only** -- no simulator test reaches it -- so `basic DH+ bridging` and
`basic DH+ bridging bit change` in the hardware suite are the checks that matter for that
change.

### 2.35 Forward Close, response unpacking and payload accounting shared — DONE 2026-09-13

The rest of the `session.c` / `conn.c` block bar one function. `ab/session.c` is 3094 lines
down to 2746 and `omron/conn.c` 2807 down to 2458; `cip/conn.c` holds 784.

| moved | similarity |
|---|---|
| `perform_forward_close` | **100%** |
| `send_forward_close_req` | **100%** |
| `recv_forward_close_resp` | 95% -- line wrapping only |
| `unpack_response` | 97% -- line wrapping and a missing debug argument |
| `get_available_cip_payload_space` | see below |

`EIP_CIP_PREFIX_SIZE` and the `GET_MAX_PAYLOAD_SIZE` macro were defined identically in both
modules and moved to `cip/conn.h` with them.

**One real difference, resolved AB's way.** In the unconnected branch of
`get_available_cip_payload_space`, AB subtracts `conn_path_size + 2` -- its comment says
"encoded path size plus two bytes for length and padding" -- while Omron subtracted only
`conn_path_size`. Omron was therefore under-reserving by two bytes on every unconnected
request. AB's version is the one that shipped.

**`create_request` shared too, on AB's expression, after working out what the constant
means.** The two differed in the buffer capacity:

```c
/* AB */    request_capacity = (size_t)(available_payload + EIP_CIP_PREFIX_SIZE);
/* Omron */ request_capacity = (size_t)(max_payload_size + EIP_CIP_PREFIX_SIZE);
```

`EIP_CIP_PREFIX_SIZE` is 44 while `sizeof(eip_cip_co_req)` is 46 and `sizeof(eip_cip_uc_req)`
is 50, which makes AB's look two to six bytes short. It is not. **44 is
`offsetof(eip_cip_co_req, cpf_conn_seq_num)`** -- an offset, not a frame size -- and the
builders measure a request's payload from that same field onwards
(`data - (uint8_t *)(&cip->cpf_conn_seq_num)`). So prefix plus payload is the bytes written,
by construction, and AB's capacity is exactly the worst case:

```
AB capacity = available + 44 = (max - 6) + 44 = max + 38
written     = 44 + payload, payload <= available = max - 6
            = max + 38                                        <- exact, no slack
```

Omron's `max + 44` over-allocated by the six bytes of CPF data item. AB's is the tight and
correct one, so it is what the shared version uses.

The constant now carries a comment saying it is an offset rather than a struct size, with the
two struct sizes spelled out and a warning not to "correct" it -- 44 sitting next to 46 and 50
invites exactly that mistake. The calculation was also split so that
`cip_conn_create_request()` can compute the available payload while already holding the
connection mutex, rather than taking it twice.

Verification: clean build both configurations, 0 warnings, 0 errors. Simulator suite
**182/182**, 182s reported against 182s wall clock (11:23:57 to 11:27:00).

### 2.36 The seven request builders shared — DONE 2026-09-13

The `build_*` quadrant, AB's code for both. `ab/eip_cip.c` is 1724 lines down to 1049 and
`omron/omron_standard_tag.c` 1666 down to 973; `cip/tag.c` holds the seven functions.

| function | was |
|---|---|
| `build_write_bit_request_connected` | 98% |
| `build_write_bit_request_unconnected` | 97% |
| `build_write_request_connected` | 90% |
| `build_write_request_unconnected` | 92% |
| `calculate_write_data_per_packet` | 91% |
| `build_read_request_connected` | 70% |
| `build_read_request_unconnected` | 81% |

**The fragmentation seam is the flag, and it is now live.** Both directions reduce to one
test on `cip_plc_config_t.supports_fragmented_operations`:

```c
/* writes: a PLC with no mechanism cannot split the operation at all */
if(multiple_requests && !conn->plc_config.supports_fragmented_operations) {
    return PLCTAG_ERR_TOO_LARGE;
}
*data = (multiple_requests) ? CIP_CMD_WRITE_FRAG : CIP_CMD_WRITE;

/* reads: the fragmented service carries a byte offset, the plain one does not */
read_cmd = conn->plc_config.supports_fragmented_operations ? CIP_CMD_READ_FRAG : CIP_CMD_READ;
```

AB has the flag set, so the write check never fires and both services are chosen exactly as
before. Omron has it clear, so the write check fires precisely where its tautological
`plc_type == OMRON_PLC_OMRON_NJNX` test used to, and reads use the plain service with no
offset, as before. Neither module's behaviour changes, and the flag stopped being decorative.

**Omron gains three things it was missing**, all from AB and all in paths it either never
reached or never checked:

- The **payload-space check** before a read request's size is set -- AB compares the built
  payload against the available space and returns `PLCTAG_ERR_TOO_LARGE`; Omron had none.
- The **unconnected write overhead accounting** -- AB counts the 10-byte Unconnected_Send
  header, Omron counted the connection path instead, which `available_payload` already
  subtracts.
- The **request-reference release** on a failed add, which AB gained in 1.7.

**AB gains nothing behaviourally, but the request now carries Omron's packing inputs.**
`response_size`, `first_read` and `supports_fragmented_operations` are set on every read
request. Omron's `pack_requests` reads them; AB's ignores them. Setting them unconditionally
keeps Omron working and costs AB three stores.

Three per-module wrappers fell out as dead once the shared write builder called the shared
bit builders and the shared `calculate_write_data_per_packet` directly.

**The unconnected builders are covered by the suite.** `ab_common.c:318` has ControlLogix read
`use_connected_msg` from the tag attributes, and three simulator tests set it to zero --
including a 1000-element `TestBigArray` read, which is the case that fragments. So the shared
unconnected path runs on every suite run, not only on hardware.

What is *not* reachable is narrower and Omron-specific: `omron_common.c:271` and `:341` set
`tag->use_connected_msg = 1` and then `attr_set_int()` writes that back over whatever the
caller asked for, so `use_connected_msg=0` on an Omron tag is **silently ignored**. Omron
could do unconnected messaging; it was simply never enabled. Every AB PLC type except
`AB_PLC_LGX` is hard-set the same way (`ab_common.c:297-334`). Worth a warning when the
attribute is present and about to be overridden -- silently discarding a caller's setting is
the kind of thing that costs someone an afternoon.

Verification: clean build both configurations, 0 warnings, 0 errors. Simulator suite
**182/182** after the write group (186s/186s) and again after the read group (183s/183s wall
clock, 19:13:41 to 19:16:45).

### 2.37 Unconnected messaging enabled for Omron and Micro800 — DONE 2026-09-14

`use_connected_msg` is a tag attribute, but only `AB_PLC_LGX` ever read it. Every other PLC
type hard-set the field and then called `attr_set_int()` to write that back over whatever the
caller had asked for, so the setting was **silently discarded**. Two of those were wrong.

**Omron NJ/NX** can do unconnected messaging exactly as a ControlLogix can. It now reads the
attribute, defaulting to connected as before.

Enabling it immediately failed, and the reason is the point of this entry:

```
[OMRON_CONN] WARN process_requests:1686 First request size 2147483647 exceeds remaining space 498,
                                        cannot process any requests.
```

repeated until the tag create timed out. **Omron's `get_payload_size()` had no
`EIP_UNCONNECTED_SEND` branch** and fell through to `INT_MAX` for every unconnected request,
so the packing loop could never schedule one. The forced `use_connected_msg = 1` was not a
protocol restriction at all -- it was covering for a missing branch one layer down.

AB's version handles both encapsulation commands, cross-checks the computed size against the
declared one, and guards a null or empty request. It is shared now as
`cip_get_payload_size()`, which pulled `eip_cpf_uc_header` and `eip_cpf_co_header` out of
`ab/defs.h` into `eip/defs.h` -- the two AB-only structs left behind in 2.23.

**Micro800** also supports unconnected messaging, with the same ~500 byte default payload as
the other CIP PLCs. Its `"Micro800 needs connected messaging."` comment was wrong. It reads
the attribute now too. It still cannot pack several operations into one packet, which is the
other, independent capability -- Micro800 remains the clearest example that the two levels do
not imply each other.

Two simulator tests added, both reading with `use_connected_msg=0`: **Omron unconnected
messaging** and **Micro800 unconnected messaging**. Before this, the only coverage of the
unconnected request builders was AB's three ControlLogix tests, so the paths merged in 2.36
are now exercised on three PLC families rather than one.

Still hard-set, and correctly so: the PCCC families and `AB_PLC_GENERIC`.

Verification: clean build both configurations, 0 warnings, 0 errors. Simulator suite
**184/184**, 183s reported against 183s wall clock (07:50:27 to 07:53:31) -- two tests more
than the 182 this work started from.

### 2.38 Duplicate servers deleted; `modbus_server` moved to `tools` — DONE 2026-09-14

**-17,132 lines across 75 files.** Neither main suite referenced any of it; only standalone
`.sh` scripts did, and those went with it.

| deleted | lines |
|---|---|
| `src/poc/ab_server_fiber` -- a fiber rewrite that never replaced `tools/ab_server` | 8,510 |
| `src/poc/modbus_server3` | 3,610 |
| `src/poc/modbus_server2` | 990 |
| six `run_*_fiber_*.sh` / `run_*modbus[23]*.sh` scripts | -- |

`src/poc/modbus_server` moved to `src/tools/modbus_server`: the simulator suite depends on it,
so it is not a proof of concept.

**`src/poc/utils` merged into `src/tools/utils`** rather than left behind, since
`modbus_server` needs seven of its files. `compat_utils.[ch]` existed in both and was
byte-identical, so that copy went too -- the fourth of the set tracked in section 4.
`src/poc` is down to three programs, all of which now reach across to `tools/utils`.

### 2.39 `pack_requests` shared — DONE 2026-09-14

98% identical, differing only in a debug line: AB null-guards the request pointer and logs an
extra `new_req=%p`, Omron indexes `requests[0]` unconditionally. AB's version is shared.

The pair had been written off in 2.31 as having a real difference. That was wrong, and 2.31 is
corrected: the measurement behind it came from **`process_requests`**, the caller, which is
where the response-space accounting and the fragmentation guard live. `pack_requests` only
concatenates already-built requests into a Multiple Service Packet and holds no policy at all.

Verification: clean build both configurations, 0 warnings, 0 errors. Simulator suite
**184/184** after the deletions (182s/182s) and again after the merge (182s/182s wall clock,
08:25:14 to 08:28:17).

### 2.40 Response-size accounting for every PLC family — FUTURE WORK, not started

Noted 2026-09-14 as the intended direction. **Deliberately not started.**

Today the three families handle an operation that will not fit in one packet in three
different ways, and only one of them plans ahead:

| family | approach |
|---|---|
| Omron NJ/NX | computes the expected response size per request and refuses to pack past the remaining space -- `process_requests` in `omron/conn.c` |
| AB CIP | no accounting at all; sends it and reacts to CIP status `0x06` by asking for the next fragment |
| PCCC | neither; a transfer that does not fit returns `PLCTAG_ERR_TOO_LARGE` |

**The intent is to move everything to the Omron model**: know the size in advance rather than
discover it from the PLC. That is the last real difference between `session.c:process_requests`
and `conn.c:process_requests`, which is why those two are still not shared -- see 2.31.

**What it needs first**, and why it is not a small job: the library must know the total size of
each tag *and the size of each element of an array tag*, ahead of the read that would otherwise
tell it. Today a first read is exactly how the size becomes known, which is what
`request->first_read` exists to signal and why the packing guard treats an unknown size as
unpackable. Getting sizes up front means somewhere to hold per-tag type information across
operations, and a way to obtain it that is not "do the read and see".

The same applies to PCCC operations, which additionally have no protocol fragmentation at all,
so the client would have to split a transfer itself -- see the mechanism table in 2.31.
`pccc_offset` is `h2le16(0)` at all four sites today.

Related pieces already in place: `cip_plc_config_t` carries `supports_fragmented_operations`
and `supports_packed_requests` per PLC model (2.32), and the request carries `response_size`,
`first_read` and `supports_fragmented_operations` (2.36). Those are the inputs such a scheme
would use; nothing else is built.

### 2.41 Section 4 cleanup — DONE 2026-09-14

The rest of the dead-code list, after 2.38 took the big items.

**`list_tags_micro8x0` deleted, −1,002 lines.** It was `list_tags_logix` with two constants
changed and the path argument removed. The path *is* the difference: a path names a CPU in a
chassis, so a PLC that takes one is a Logix and a Micro800 is not. `list_tags_logix` now takes
the path as optional and picks the PLC type from its presence, printing reusable tag strings
in whichever form matches. It had no callers outside its own `CMakeLists.txt`.

**`multithread_plc5_dhp.c` deleted, −186 lines.** It differed from `multithread_plc5.c` by
three `#define`s and demonstrated nothing the other did not. The surviving example carries a
comment showing the DH+ tag string, since that was the entire content of the difference.

Small items: `find_entry` is `static` and out of `attr.h`; `hashtable.h` uses `#pragma once`,
the last header in the tree that did not; `debug.c`'s comment names
`debug_module_names.c` rather than the header that only declares the table; the three empty
tool directories holding nothing but `__pycache__` and `CMakeFiles` are gone; `build/`,
`build_*/` and `ab_server_*.log` are in `.gitignore` and the 3.7 MB of stray logs removed.

Two entries were examined and deliberately **not** actioned -- see the table above for the
reasons: the `atomic_*_int64` family, and the four sanitizer flags.

A measurement error worth recording: the first pass at the int64 atomics reported zero users,
because the grep filtered paths containing `atomic_utils` and that also matched
`src/tests/unit/test_atomic_utils.c`. The exclusion has to name the implementation file, not a
substring of it.

Verification: clean build both configurations, 0 warnings, 0 errors. Simulator suite
**184/184**, 189s reported against 189s wall clock (08:53:33 to 08:56:42).

### 2.42 The DH+ PCCC write trio merged — DONE 2026-09-15

`pccc_dhp_tag_write_start`, `plc5_dhp_tag_write_bit_start` and `slc_dhp_tag_write_bit_start`
collapsed into one skeleton and a four-entry variant table. **−480 lines, +155, net −325.**

Held back until §1.9 was settled, on the reasoning that three functions whose overhead
formulas disagree should not be merged until it is known which one is right. That was the
correct order: the merge would have frozen a wrong formula into the shared skeleton.

**The frame is uniform once one struct stops being special.** All three build

```
eip_cpf_co_header | pccc_dhp_routing_header | <command> | <body>
```

but the two bit writes reached it through `pccc_dhp_rmw_cmd_req`, which welded the routing
header to a five-byte command. Split into its two parts it is the same bytes in the same
order as the word write, so one pointer chain and one routing fill now serve all three and
that struct is deleted. Each variant carries a command size, a FNC code, an optional
`validate`, a `required_payload`, a `finish_command` and a `write_body`.

**Nothing new was written for the bodies.** All six hooks are the direct trio's existing
functions from the `pccc_write_variant_t` table -- `plc5_write_finish_command`,
`slc_write_finish_command`, `rmw_finish_command`, `plc5_write_body`, `plc5_write_bit_body`,
`slc_write_bit_body`. The DH+ mask builders were the same logic spelled out again; the diff
that deleted them is the evidence.

**The guards were swept, not eyeballed.** Old and new thresholds compared over 600 payload
sizes x 11 encoded-name lengths x 6 element sizes x 6 tag sizes, for all four variants: zero
differences. The sweep earned its keep -- it caught `cmd_req_size` set to the five bytes the
SLC bit write actually emits, where the estimate has always used the six-byte SLC word-write
struct. One byte more permissive, and invisible by inspection. Restored, with the same note
the direct table carries on the same quirk.

The three overhead formulas were preserved exactly rather than normalised, including the
double-counted masks in both bit writes. Restated against a common base of routing + command
+ name they demand:

| variant | required | body actually writes | slack |
|---|---|---|---|
| word write | `tag->size` | `tag->size` | exact |
| PLC5 bit | `4 x elem_size` | `2 x elem_size` | `2 x elem_size` |
| SLC bit | `3 x elem_size + 2` | `2 x elem_size + 1` | `elem_size + 1` |

At the two-byte element size a bit tag uses, that is eight bytes demanded of roughly two
hundred available. The guard cannot fire, so the excess has never refused a real write, and
tightening it would be shrinking a size estimate for tidiness -- which is what §1.9 is about.

**Two format bugs went out with the code that held them.** `%zu` applied to an `int`
`data_per_packet` in both bit writes. On arm64 the conversion reads eight bytes where four
were pushed. Unreachable in practice, both in the same dead guard, and invisible to
`-Wall -Wextra` for the reason in §3.4.

Verified: both configs clean, simulator **184/184** (183s reported, 182s wall), hardware
**45/45**. DH+ has no simulator coverage, so tests 28, 29, 32, 33 and 34 are the whole check.

## 3. Verification gaps

### 3.1 No Windows build — OPEN
Everything below compiles clean on POSIX and is unexercised on Windows:
- `src/utils/thread.c` — every `#ifdef _WIN32` branch
- 26 `THREAD_FUNC`/`THREAD_RETURN` conversions across 18 test files (wrong calling
  convention compiles clean on POSIX, fails on MSVC)
- the inlined `InterlockedCompareExchange` Winsock guard that replaced
  `compat_thread_once`, now in `src/tests/utils/test_utils.c` (2.5)
- the Windows halves of `test_set_interrupt_handler` and `test_cpu_count`, moved verbatim
  in 2.5
- **`compat_sscanf`'s `vsscanf_s` branch in `src/examples/compat_utils.h`** — its guard was
  `_MSVC_VER`, so it has never once compiled; 2.5 corrected it to `_MSC_VER`, which means a
  Windows build will reach that code for the first time
- fixes 1.2 and 1.3 when made — both are MSVC-only paths
- **`<tchar.h>` and `<strsafe.h>` in `windows/platform.h`** — unused since the serial
  delete (2.9), but `platform.h` reaches every file in the library, so the removal wants a
  real compiler behind it.
- **`src/platform/*/platform.h` still exists and is still on the include path, though
  nothing includes it (2.15).** It is deliberately kept as the insulation layer until there
  has been a Windows build; the POSIX-side transitive breakage that the round-10 sweep
  exposed had a Windows counterpart that no compiler here can see.
- **The three Windows fixes in round 9 (2.12)** — the `GetSystemTimePreciseAsFileTime`
  switch in `modbus.c`'s timing path, the `sleep_ms` negative guard and return-code change,
  and `time_ms()` now deriving from `time_us()`. All three are corrections, but none has
  been through a Windows compiler.
- **`ab/cip.c` uses `ssize_t` about 30 times, but `windows/platform.h` only typedefs it
  under `_MSC_VER`.** A MinGW build of that file should fail outright. Pre-existing and
  unrelated to any round; found while removing the last `ssize_t` use from the shims.
- **`src/utils/socket.c`'s entire `#ifdef _WIN32` half (1,111 lines)** — moved verbatim from
  `windows/platform.c`, so it is no more broken than it was, but the new file's include block
  and the `socket_create()` leak fix inside that branch have never been through a compiler.
  This is now the largest unverified Windows surface in the tree.

### 3.2 `test_emulator_performance` now reports real numbers — DONE, watch it
Previously summed a local `int result` that was never assigned, so its iteration total
was always 0. Now routed through `thread_iterations[]`. Expect this test to start
reporting a non-zero figure where it used to report zero.

### 3.3 `test_idle_disconnect` (AB ControlLogix) is flaky under TSan — WATCH

Failed once during the 2.3 rename verification with `ERROR: Connection should be UP after
near-max wait, but is IDLE_WAIT`, then passed on an immediate re-run (181/181, no race
reports). It is a timing-class test that waits to just under the inactivity timeout and
expects the connection to still be up; TSan's instrumentation overhead can push the wake
past the deadline. Nothing in the diff that ran between the two runs was more than a
rename. Re-flag only if it fails twice in a row.

Two more sightings during round 6 verification, neither counted against the round:
- The Modbus variant (`test 172`) produced the identical message on the **ASan** build, so
  this is not TSan-specific — it is the near-max-wait pattern itself. Passed on re-run.
- `test 68`/`test 69` (reconnect after PLC outage, 100-second wall-clock tests) failed in a
  run where the host slept mid-suite: the runner reported 174s of work across 39 minutes of
  wall clock. Both passed on a run with no suspend. **When a timing test fails, check wall
  clock against the runner's reported total before believing it.**
- `test 11` (AB/ControlLogix tag scheduling fairness) failed once under TSan at CV 20.64%
  against a 20.00% threshold, with zero race reports. A statistical margin, not a race.
  Passed on re-run. If this recurs, the threshold is the thing to look at, not the code.

**Run the suites under `caffeinate -i`.** The host idle-slept through six runs on
2026-09-10, one reporting 124s of work across 38 minutes of wall clock and failing four
timing tests (18, 112, 68, 69); another reported 153s across 44 minutes. Every failure
passed on a caffeinated re-run. The command is:

```
caffeinate -dimsu python3 src/tests/scripts/run_simulator_tests_parallel.py <bindir> <logdir> --max-stress=200
```

**No `caffeinate` flag combination has proved reliable.** `-i` lost the machine on
2026-09-11 (146s of work across 32 minutes, failing 68, 69, 112, 115). `-dimsu` -- display,
idle, disk, system, user-active -- then lost it twice in a row the same evening: 147s across
88 minutes, and 99s across 4 hours 39 minutes. One `-dimsu` run in between was clean at 178s
reported against 178s wall clock. So the flags are worth using and are not a guarantee;
something outside `caffeinate`'s assertions is suspending this host.

**The wall-clock check is the part that actually works.** Every one of these was identified
as a host problem, not a regression, by comparing the runner's reported total against elapsed
wall clock. Always print `date` before and after the run. Treat a timing failure as real only
when the two agree.

Keep the wall-clock-versus-reported-total check as a cross-check: if the two disagree, the
machine slept and the timing results mean nothing. That check is what identified every one
of these as a host problem rather than a regression.

---

### 3.4 `pdebug_impl` has no `format` attribute — OPEN

`pdebug_impl` takes a `printf` template and a `...`, and is declared without
`__attribute__((format(printf, 6, 7)))`. Every `pdebug` call in the tree is therefore
unchecked: `-Wall -Wextra -Wformat` cannot see a mismatched conversion.

Two live examples were found by reading, not by the compiler -- `%zu` applied to an `int` in
both DH+ bit writes (§2.42). Both are gone, but they were found by accident.

Adding the attribute is one line. It is filed rather than done because nobody knows how many
call sites it lights up, and a build that will not compile is a poor thing to hand over
mid-refactor. Worth doing at a quiet point, with the count measured first:

```
/* on the declaration in src/utils/debug.h */
__attribute__((format(printf, 6, 7)))
```

MSVC has no equivalent, so the attribute needs the usual GCC/Clang guard -- which means the
Windows build (§3.1) would still be unchecked.

## 4. Dead code and cleanup (from the repo audit)

Ranked by size.

| what | where | approx | state |
|---|---|---|---|
| ~~4 identical copies of the server-utils set~~ | -- | −6,000 est | **DONE** -- `poc/modbus_server` and the `tests/utils` socket/coro_net pair 2026-09-10 (2.4 step 1); the rest of `tests/utils` in 2.20; `poc/utils` merged into `tools/utils` in 2.38 |
| ~~`ab_server_fiber` duplicates `tools/ab_server`~~ | -- | −8,510 | **DONE** 2.38 |
| ~~three Modbus test servers~~ | -- | −4,600 | **DONE** 2.38 -- `modbus_server2` and `3` deleted, `modbus_server` moved to `tools` |
| ~~`list_tags_micro8x0` is `list_tags_logix` with 37 lines changed~~ | -- | −1,002 | **DONE** 2.41 |
| ~~`multithread_plc5.c` and `_dhp` differ by 6 lines~~ | -- | −186 | **DONE** 2.41 |
| `multithread.c` and `_cached_read` same shape | `src/examples/multithread*.c` | −184 | **KEPT** -- `_cached_read` demonstrates `read_cache_ms` and is read-only, which is a different thing to show |
| `atomic_*_int64` family | `src/utils/atomic_utils.h` | −30 | **KEPT** -- its only user is `test_atomic_utils.c`, which exercises both the C11 and the fallback implementation. Deleting a symmetric API family to save 30 lines would cost that |
| ~~`attr.h` `find_entry` declared, never called outside `attr.c`~~ | -- | small | **DONE** 2.41 -- now `static` |
| ~~4 sanitizer flags where 2 would do~~ | `CMakeLists.txt:75` | small | **CLEARED** -- the comment above them explains the design: the server flags are deliberately independent so CI can tell which side a sanitizer report belongs to |
| ~~empty tool dirs~~ | -- | -- | **DONE** 2.41 |
| ~~`hashtable.h` `#ifndef` guard~~ | -- | 1 line | **DONE** 2.41 -- `#pragma once` |
| ~~stale comment in `debug.c`~~ | -- | 1 line | **DONE** 2.41 |
| ~~untracked `ab_server_*.log` and `build/`~~ | -- | -- | **DONE** 2.41 -- `.gitignore`d and removed |

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

# Unified Library Lifecycle Design

Status: design, not yet implemented.

## 1. Problem

Library-scoped infrastructure (`tags` hashtable, `tag_lookup_mutex`, `tag_tickler_wait`,
tickler thread) is created and destroyed once per init cycle. Application threads can be
inside an API call at any instant, entering through `lookup_tag()` or directly through
`critical_block(tag_lookup_mutex)`.

`library_state` synchronizes construction against construction, and construction against
destruction. It never sees the API calls: `plc_tag_read()`, `plc_tag_destroy()`,
`plc_tag_get_int32()` etc. never call `initialize_modules()`. They are gated only by
`lib_active`, or at seven sites by nothing at all.

Consequences observed in CI (macOS TSan, `test_shutdown_restart`):

- **TOCTOU in `lookup_tag()`** — `lib_active` is a hint, not a lock. It can go false and
  the mutex and hashtable can be freed between the check and the `critical_block()`.
- **Ungated direct uses** — `lib.c:582` (tickler scan), `1197`/`1233`/`1253` (create error
  paths), `1306`/`1311` (shutdown scan), `1713` (`plc_tag_destroy`), `4583`
  (`add_tag_lookup`). `plc_tag_destroy` has no gate whatsoever.
- **ABA on restart** — `mutex_create()` returns a new mutex at the address a just-freed one
  occupied; TSan reports the write in `mutex_create` racing the read in `mutex_lock_impl`
  from a still-running `lookup_tag()`.
- **Non-atomic shutdown guard** — `if(!tags || !atomic_get_bool(&lib_active))` is two loads;
  two threads can both pass and both run the whole tag-destroy loop.
- **Create racing shutdown** — `plc_tag_shutdown()` sets `lib_active=false` and destroys
  every tag *before* `destroy_modules()` moves the state to SHUTTING_DOWN. Throughout that
  window the state is still RUNNING, so a concurrent `plc_tag_create()` takes the
  `initialize_modules()` fast path and creates a tag into a table being drained.

## 2. Goals and non-goals

Goals:

- No use-after-free of library-scoped infrastructure by in-flight API calls.
- One source of truth for "may I start new work", with the phase information the threads
  need kept separate and clearly subordinate.
- Preserve concurrent tag creation from multiple threads at startup.
- Preserve shutdown-then-restart.
- Preserve orderly protocol close (ForwardClose / UnregisterSession / socket close).
- Preserve callback delivery for tags destroyed during shutdown.
- No public API or ABI change.

Non-goals:

- **Generation bits in the tag handle.** `next_tag_id` (`lib.c:68`) is file-scope static,
  initialized once at load, and is *not* reset by `lib_init()`. It already counts
  monotonically across instances, so a stale handle from a previous instance simply misses
  the table and returns `NOT_FOUND` — which is the required semantics. The margin is the
  full 28 bits (~268M creations). The handle has only 3 spare bits (bit 31 must stay clear
  for error codes), giving 8 generations before the generation itself wraps; widening it
  means stealing from the ID space and making *both* wrap hazards worse. Not worth it.
  `next_tag_id` must stay outside the instance and must never be reset.
- Changes to per-tag locking (`tag->api_mutex`) or protocol-internal locking.
- Refcounting the lookup mutex on its own. Correct but poorly targeted: it buys the ability
  to free ~200 bytes at a cost of an acquire/release pair per lookup. Refcount the instance
  instead.

## 3. Core invariant

> **A live tag implies a live library instance.**

A tag takes a reference to the instance at creation and drops it in its destructor.
Therefore any code path that starts from a valid `plc_tag_p` may use `inst->tags` and
`inst->tag_lookup_mutex` without further checks — the objects cannot outlive the tag
holding them. This is what collapses the 47 `lookup_tag()` call sites in `lib.c` to a
single change site.

## 4. Data structures

```c
typedef struct lib_instance_t *lib_instance_p;

struct lib_instance_t {
    hashtable_p tags;
    mutex_p     tag_lookup_mutex;
    cond_p      tag_tickler_wait;
    thread_p    tag_tickler_thread;
};
```

Allocated with `rc_alloc()`. Globals in `lib.c`:

```c
static lib_instance_p current_instance = NULL;   /* the acquisition gate */
static lock_t         instance_lock = LOCK_INIT; /* immortal; guards the pair below */
```

`library_state` (`init.c:75`) stays, and stays authoritative for *phase*.

### 4.1 Division of responsibility

Two variables, disjoint jobs, one direction of dependency:

| | `current_instance` | `library_state` |
|---|---|---|
| Answers | "may I start new work?" | "what phase are we in?" |
| Values | NULL / non-NULL | UNINITIALIZED / INITIALIZING / RUNNING / SHUTTING_DOWN |
| Read by | `lib_instance_acquire()` only | tickler, RC thread, IO threads, diagnostics |
| Written | only under `instance_lock`, atomically with the state | only under `instance_lock` |

`current_instance` is non-NULL **exactly while** `library_state == LIB_STATE_RUNNING`. The
pair is always updated together inside one `spin_block(&instance_lock)`, so an acquire only
ever needs to test the pointer. `lib_active` is deleted.

Note there is no `LIB_STATE_SHUTDOWN`. Shutdown ends by returning to UNINITIALIZED;
a distinct terminal state would make the library unable to restart, which
`test_shutdown_restart` explicitly requires.

### 4.2 What stays outside the instance

- `next_tag_id` — must remain monotonic across instances (see §2).
- Refcount cleanup infrastructure (`refcount_startup()` / `refcount_teardown()`) — it must
  outlive the instance, because tag destructors run on it and instance destruction happens
  after it is joined. Keeping it outside breaks the circularity.
- Protocol module globals (AB / Modbus / Omron). Out of scope for this change; they keep
  their existing `*_init()` / `*_teardown()` pairing.

## 5. Acquire and release

```c
static lib_instance_p lib_instance_acquire(void) {
    lib_instance_p inst = NULL;

    spin_block(&instance_lock) {
        if(current_instance) { inst = rc_inc(current_instance); }
    }

    return inst;   /* NULL means "not running" -- caller must punt */
}
```

**The spinlock is load-bearing and cannot be replaced by an atomic load plus `rc_inc`.**
`rc_inc_impl()` (`rc.c:133`) computes `rc = ((refcount_p)data) - 1;` and then reads
`rc->count`. It dereferences the header *before* it can discover the count is zero. So
relying on "`rc_inc` returns NULL when the count hits zero" is a weak-reference upgrade that
is only sound when the memory is still valid:

```
thread A:                              shutdown:
inst = atomic_load(&current);   /* non-NULL */
                                       current = NULL
                                       rc_dec(inst) -> 0 -> destructor -> free
rc_inc(inst)   <-- reads rc->count out of freed memory
```

Under `instance_lock`, either A observes non-NULL and raises the count to >= 2 before
releasing — so shutdown's `rc_dec` is not the last — or A observes NULL and bails. No
window, and no ABA, because A never holds a bare pointer across an unlocked gap.

An atomic load of `current_instance` is still fine as a *fast negative check* ("NULL, punt")
ahead of the spinlock, but the acquire itself must be locked.

Release is plain `rc_dec(inst)`.

## 6. Startup

`plc_tag_create_impl()` is the only production path that starts the library.

```c
inst = lib_instance_acquire();
if(!inst) {
    rc = lib_instance_start();          /* see below */
    if(rc != PLCTAG_STATUS_OK) { return rc; }
    inst = lib_instance_acquire();
    if(!inst) { return PLCTAG_ERR_NOT_ALLOWED; }   /* raced a shutdown */
}
```

`lib_instance_start()`:

1. CAS `library_state` UNINITIALIZED -> INITIALIZING.
2. On failure, branch on the observed state:
   - **RUNNING** — someone else finished. Return OK; the caller re-acquires.
   - **INITIALIZING** — someone else is building. Sleep 10 ms, retry.
   - **SHUTTING_DOWN** — sleep 10 ms, retry, bounded by `LIB_START_WAIT_TIMEOUT_MS`;
     on expiry return `PLCTAG_ERR_TIMEOUT`.
3. On success (we won): `refcount_startup()`, build the instance (hashtable, lookup mutex,
   tickler condvar, tickler thread), then the protocol `*_init()` calls, then publish:

```c
spin_block(&instance_lock) {
    current_instance = inst;
    atomic_set_int32(&library_state, LIB_STATE_RUNNING);
}
```

4. On any construction failure: tear down what was built, set state back to UNINITIALIZED,
   return the error.

**The RUNNING and INITIALIZING branches must not be errors.** `initialize_modules()` runs on
*every* `plc_tag_create()`, and `test_shutdown_restart` spawns ten threads that each call
`plc_tag_create()` simultaneously: one wins the CAS, the other nine must wait and then
succeed. Making a non-UNINITIALIZED state an error fails nine of ten creations.

The tickler thread is handed the instance at `thread_create()` and holds a reference for its
whole lifetime, so it never acquires per iteration.

## 7. Tag creation and destruction

`plc_tag_create_impl()` **transfers** its acquired reference into the tag:

```c
tag->instance = inst;    /* transfer -- do NOT rc_dec(inst) on the success path */
```

and registers the tag via `add_tag_lookup()` under `inst->tag_lookup_mutex`. Every error
path after the acquire must `rc_dec(inst)`.

The tag destructor drops `tag->instance` **last**, after all other tag teardown.

Because of this, `plc_tag_destroy()` (`lib.c:1713`), the create error paths
(`1197`/`1233`/`1253`) and anything else holding a tag use `tag->instance` directly and need
no acquire.

## 8. `lookup_tag()`

```c
plc_tag_p lookup_tag(int32_t tag_id) {
    plc_tag_p tag = NULL;
    lib_instance_p inst = lib_instance_acquire();

    if(!inst) { return NULL; }   /* not running -- callers already handle NULL */

    critical_block(inst->tag_lookup_mutex) {
        tag = hashtable_get(inst->tags, (int64_t)tag_id);
        if(tag && tag->tag_id == tag_id) {
            tag = rc_inc(tag);
        } else {
            tag = NULL;
        }
    }

    rc_dec(inst);
    return tag;
}
```

The instance reference here is per-call and released before return, because `lookup_tag()`
does not hand the instance to its caller. This is unavoidable: `lookup_tag()` must touch
`inst->tags` and `inst->tag_lookup_mutex` *before* it has a tag to borrow a reference from.
Cost is one uncontended spinlock plus two atomic RMWs per API call, against a mutex
acquisition the call was about to make anyway.

All 47 existing call sites are unchanged — they already handle a NULL return.

## 9. Shutdown

`plc_tag_shutdown()` stays `LIB_EXPORT void` — it is registered with `atexit()`, which
requires `void(void)`, and changing it would break ABI. A failed precondition is logged, not
returned. If a status is wanted later, add `plc_tag_shutdown_ex()` returning `int32_t` and
keep the void wrapper.

### 9.1 Sequence

1. **Close the gate, atomically.** One critical section does the precondition check, the
   state transition and the pointer clear together:

   ```c
   spin_block(&instance_lock) {
       if(atomic_get_int32(&library_state) == LIB_STATE_RUNNING) {
           atomic_set_int32(&library_state, LIB_STATE_SHUTTING_DOWN);
           inst = current_instance;
           current_instance = NULL;
       }
   }
   if(!inst) { /* warn: not running, or another thread is already shutting down */ return; }
   ```

   This is the fix for both the non-atomic double-shutdown guard and the create-racing-
   shutdown window: no new acquisition is possible from this instant, and exactly one thread
   proceeds.

2. **Wake the IO threads** so they observe the phase promptly (`socket_wake`,
   `wake_plc_thread`, `cond_signal`). A thread parked in a long socket wait would otherwise
   not notice.

3. **Destroy all tags.** Walk `inst->tags` under `inst->tag_lookup_mutex`, `rc_inc` each,
   call `plc_tag_destroy()` outside the lock, `rc_dec`. Unchanged in shape from today.

4. **Drain and join the tickler.** It exits on its own once the table is empty (§10), which
   is what delivers the `PLCTAG_EVENT_DESTROYED` / `PLCTAG_EVENT_ABORTED` callbacks raised
   by step 3. Bounded by `SHUTDOWN_DRAIN_TIMEOUT_MS`, then joined.

5. **Protocol teardowns** — `ab_teardown()`, `mb_teardown()`, `omron_teardown()`. These join
   the IO threads, which by now have closed their connections cleanly (§11).

6. **Drain the RC queue to quiescence**, with the RC thread still running. This runs every
   tag destructor and the full transitive closure of what they release (session / plc / conn
   destructors, and whatever those release in turn). Bounded by
   `SHUTDOWN_DRAIN_TIMEOUT_MS`.

7. **Destroy the instance.** `rc_dec(inst)` with the count now at 1, queued and run by the
   RC thread like every other object. The destructor destroys the tickler condvar, the
   lookup mutex and the (now empty) hashtable. Wait for quiescence again.

8. **`refcount_teardown()`** — final drain and join the RC thread.

9. **`library_state = LIB_STATE_UNINITIALIZED`** — ready to restart.

### 9.2 Why this order

Three constraints, jointly satisfiable only in the order above:

- The tickler `rc_inc`s tags during its scan (`lib.c:588`) and `rc_dec`s them at the end of
  the body (`lib.c:658`). Those final `rc_dec`s queue work onto the RC cleanup thread.
  Therefore **the tickler must be joined before the RC thread** (4 before 8).
- Tag destructors touch the tag table and lookup mutex, which the instance destructor
  destroys. Therefore **every tag destructor must complete before the instance destructor
  runs** (6 before 7). This is the constraint documented at `init.c:172-175`.
- The instance is an ordinary rc object and is destroyed through the normal queue, so
  **the instance must be destroyed before the RC thread is joined** (7 before 8).

The middle constraint is *not* satisfied by FIFO ordering alone, even though the queue is
FIFO (`vector_insert` at the tail `rc.c:200`, `vector_remove(cleanup_queue, 0)` at the head
`rc.c:259`). Destructors queue transitively: a tag destructor releasing the last reference
to its session appends the session destructor *after* anything already queued, including a
prematurely-queued instance destructor. Draining to quiescence first removes the need to
audit which destructors touch the table — step 6 guarantees nothing is left that could.

An earlier draft of this design had the instance destroyed *after* `refcount_teardown()`,
relying on `rc_dec_impl()`'s fallback to immediate inline cleanup when
`cleanup_thread_running` is clear (`rc.c:195`, cleared at `rc.c:343`). That works, but it
makes the instance the one object that does not go through the normal path, and it depends
on a subtle property of `rc_dec`. Draining and destroying through the queue is uniform and
does not special-case anything.

Relative to today's `refcount_teardown()` -> `lib_teardown()` order, the new elements are the
tickler drain and join ahead of everything (the tickler now runs through the SHUTTING_DOWN
phase), and the explicit drain-to-quiescence separating tag destruction from instance
destruction.

### 9.2.1 Required new helper

Step 6 needs a "wait until the cleanup queue is idle" primitive, which `rc.c` does not
currently expose. Queue-length-zero is not sufficient: a destructor may be mid-run and about
to queue more. The RC thread must publish an idle indicator — queue empty *and* not
currently inside `refcount_cleanup()` — and the helper waits on that with a timeout:

```c
extern int refcount_drain(int timeout_ms);   /* new, in rc.c */
```

### 9.3 Bounded waits

Every drain gets a deadline and a loud warning on expiry. An unbounded wait is the failure
mode called out at `init.c:295-316` for the Windows DLL at-exit path, where the loader has
already killed the threads being waited on. Suggested single constant:

```c
#define SHUTDOWN_DRAIN_TIMEOUT_MS (10000)
```

## 9.4 Safe to call at any point

`plc_tag_shutdown()` must be safe to call at any time, from any thread, any number of times.
The atomic gate close in §9.1 step 1 provides most of that: exactly one caller wins the
RUNNING -> SHUTTING_DOWN transition and proceeds; every other caller — concurrent, repeated,
or on a never-initialized library — observes a non-RUNNING state, logs, and returns
immediately. There is no partial-shutdown path and no double-drain.

There is one case it does **not** make safe, and it is pre-existing rather than introduced
here: **calling `plc_tag_shutdown()` from a tag callback self-deadlocks.** Callbacks run on
the tickler thread (`plc_tag_generic_handle_event_callbacks`, `lib.c:658`, invoked from
`tag_tickler_func`), and shutdown joins the tickler thread at step 4. A callback that calls
shutdown therefore joins its own thread.

Two options, in preference order:

1. **Detect and refuse.** Record the tickler thread id at creation; if
   `plc_tag_shutdown()` is entered on that thread, log an error and return without
   shutting down. Cheap, and turns a hang into a diagnosable message.
2. Document the restriction only.

Option 1 is recommended — a hang inside a user callback is close to undiagnosable from the
outside, and this is exactly the failure mode already called out for the Windows DLL at-exit
path (`init.c:295-316`).

Note that `plc_tag_destroy()` on the last tag does not hit this: it tears down the session
and its handler thread (§9.5, layer 1), but never the tickler, so the common "destroy my
last tag from a callback" pattern stays safe.

## 9.5 Two teardown layers

Resource release happens at two independent layers. Only the second needs an explicit call,
and this design changes only the second.

### Layer 1 — protocol resources, released automatically by refcounting

A tag holds a reference to its session / plc / conn (`tag->session = rc_inc(...)`,
`ab_common.c:322`/`328`/`341`), and the tag destructor releases it (`ab_common.c:942`).
When the last tag referencing a session drops it, `session_destroy()` runs and performs the
complete orderly teardown:

- `remove_session()` — unlink from the module's session list
- set `terminating`, signal the wait condvar, `thread_join()` the handler thread
- `perform_forward_close()` — ForwardClose for connected CIP sessions
- `session_unregister()` — UnregisterSession
- `session_close_socket()`
- release all queued requests

Modbus (`plc`) and Omron (`conn`) follow the same pattern. **So destroying the last tag does
release the connections, sockets and IO threads**, with a proper on-the-wire close, and it
does so with no involvement from `library_state` at all. This behaviour is correct, is
relied upon, and is untouched by this design.

Note this is the same shape the instance uses in §7: an owner holds a reference, the
destructor does the orderly close. Layer 2 is this pattern applied one level up.

### Layer 2 — the library instance, released explicitly

The tag table, lookup mutex, tickler thread, and the module-level globals (session list and
its mutex, the Modbus and Omron equivalents) are *not* released when the last tag goes. The
library stays RUNNING with an empty tag table and the tickler spinning per §10.

Layer 2 is torn down only by an explicit `plc_tag_shutdown()`, by the `atexit()` handler
(`init.c:315`), or by `DllMain` on `FreeLibrary` (`lib.c:131`). An application that never
calls shutdown still gets layer 2 cleaned up by `atexit()` on POSIX and on the Windows
static library — which is the "on most platforms" caveat; the Windows DLL case is excluded
for the loader-lock reason documented at `init.c:295-316`.

### Why layer 2 should not become implicit

Extending auto-teardown to layer 2 — shutting the library down when the tag count reaches
zero — would break in ways layer 1 does not:

- **Self-deadlock.** Per §9.4, `plc_tag_destroy()` is routinely called from tag callbacks,
  which run on the tickler thread. Layer 1 is safe there because `session_destroy()` joins
  the *session* handler thread, never the tickler. Layer 2 must join the tickler itself.
- **Racy trigger.** Between observing an empty table and closing the gate, another thread
  can create a tag. Layer 1 has no such window — the refcount reaching zero *is* the
  trigger, atomically.
- **Thrash.** Layer 1 churn (session closed and reopened across a create/destroy cycle) is
  bounded and already accepted. Layer 2 churn would additionally rebuild the tag table,
  lookup mutex and tickler thread and re-run `ab_init()` / `mb_init()` / `omron_init()` on
  every cycle.

### Consequence for the drain bound

Because layer 1 destructors run on the RC cleanup thread and perform blocking network I/O
(ForwardClose and UnregisterSession round-trips, each with its own timeout) plus a
`thread_join()`, a single queue entry can occupy the RC thread for seconds. Two implications:

- `SHUTDOWN_DRAIN_TIMEOUT_MS` (§9.3) must accommodate several sessions closing in sequence,
  not just queue-processing overhead. 10 s may be too tight when many sessions are open.
- The idle indicator required by `refcount_drain()` (§9.2.1) must distinguish "queue empty"
  from "queue empty and no destructor currently running", since a long-running
  `session_destroy()` with an empty queue behind it is exactly the state that must not be
  mistaken for quiescence.

## 10. Tickler and RC cleanup loop conditions

Both loops use the same rule: **keep running while there is work, or while the library is
running.**

Tickler:

```c
while(1) {
    int entries = 0;

    critical_block(inst->tag_lookup_mutex) { entries = hashtable_entries(inst->tags); }

    if(entries == 0 && atomic_get_int32(&library_state) != LIB_STATE_RUNNING) { break; }

    /* ... existing scan / tickle / raise events / callbacks ... */
}
```

RC cleanup thread:

```c
while(queue_not_empty || atomic_get_int32(&library_state) == LIB_STATE_RUNNING) { ... }
```

Notes:

- Use `hashtable_entries()` (`hashtable.c:189`, already exists), **not**
  `hashtable_capacity()`, which is what the tickler scan currently uses.
- The state load is `atomic_get_int32()` on `library_state`, not `_Atomic` — consistent with
  the rest of the codebase.
- This is what makes shutdown deliver callbacks correctly: the tickler keeps going after the
  phase leaves RUNNING because tags still exist, and exits only once the table drains.
- The tickler holds an instance reference for its lifetime, so `inst` is valid throughout
  and no acquire is needed in the loop.
- `library_state` must be readable from `lib.c` and the protocol modules — export an
  accessor (`lib_state_get()`) rather than the variable.

## 11. IO threads

`session_handler` (`session.c:1355`), `modbus_plc_handler` (`modbus.c:1040`) and the Omron
conn handler (`conn.c:1154`) currently have `&& atomic_get_bool(&lib_active)` in the loop
condition. That is removed.

Instead, in the **IDLE state** — the quiescent point, no request in flight — check the
phase:

```c
if(lib_state_get() != LIB_STATE_RUNNING) {
    /* set the existing per-connection terminate flag and let the state machine
     * walk its normal close path */
}
```

**The check feeds the existing terminate flag; it does not break the loop.** These are
protocol state machines with real teardown states — `SESSION_DISCONNECT` (ForwardClose),
`SESSION_UNREGISTER`, `SESSION_CLOSE_SOCKET` (`session.c:1295-1297`). Exiting the loop
directly skips all of them: no ForwardClose for connected CIP sessions, no
UnregisterSession, no orderly socket close, and the PLC holds the connection until its own
timeout expires. That is observable on the wire and is covered by the idle-disconnect /
reconnect tests, the `@connection` tag tests and issue #625.

Each session / plc / conn holds an instance reference for its lifetime, so the phase check
is purely advisory and costs one atomic load per idle iteration.

### 11.1 The phase check is a signal, not a guard

`while(state == RUNNING)` has the same TOCTOU shape as the `lib_active` check it replaces:
the load happens at the top of the iteration and the body then touches shared state.
Swapping the variable name changes nothing about safety. **All safety comes from the
reference**; the phase check exists only to make shutdown prompt.

## 12. Splitting deferred destruction into two phases

### 12.1 The observation

Anything on the RC cleanup queue is already logically dead — its refcount reached zero and
no new reference can be taken. Only the memory is still live. But the destructor currently
does two very different jobs in one serial step on the single RC thread:

| | work | cost | parallelizable |
|---|---|---|---|
| (a) | tell the owned IO thread to stop; close the connection on the wire | seconds (ForwardClose + UnregisterSession round-trips, each with its own timeout) | yes — each connection is independent |
| (b) | `thread_join()`, free memory | microseconds *if (a) already finished* | no, and no need |

With N sessions open, shutdown pays roughly `N * (a + b)` serially on one thread. If (a) is
started for all N at the moment each refcount hits zero, they wind down concurrently on
their own threads and the RC thread's serial work collapses to `N * b`.

### 12.2 The mechanism: a two-phase destructor

Rather than a second function pointer — which costs a field in every refcount header and
splits one object's teardown across two functions that must be kept in agreement — keep a
single destructor and give it a phase argument and a return value:

```c
/* pre_queue: true  -> called synchronously on the releasing thread, before queueing.
 *            false -> called on the RC cleanup thread, deferred.
 *
 * returns:   true  -> queue me; call again with pre_queue == false, then free.
 *            false -> teardown is complete; free now, do not call again.
 *
 * The return value is only meaningful on the pre_queue == true call and is
 * ignored on the deferred call. */
typedef bool (*rc_cleanup_func)(void *data, bool pre_queue);
```

`rc_dec_impl()` at count zero:

```c
bool needs_deferred_cleanup = true;

if(rc->cleanup_func) { needs_deferred_cleanup = rc->cleanup_func(data, true); }

if(needs_deferred_cleanup && cleanup_thread_available) {
    /* queue; the RC thread later calls cleanup_func(data, false), then mem_free(rc) */
} else {
    mem_free(rc);   /* NOTE: no second cleanup_func() call -- phase 1 said it was done */
}
```

The important detail is that **`false` means "already fully torn down"**, so the deferred
call is skipped entirely rather than issued a second time. A destructor that returns `false`
must have completed all its work in the phase-1 call.

`refcount_cleanup()` becomes `if(rc->cleanup_func) { rc->cleanup_func(data, false); }` and
then `mem_free(rc)` as today.

### 12.3 The three destructor shapes

**1. Migration / unaudited (all 13 existing sites start here).** Behaviourally identical to
today:

```c
bool foo_destroy(void *arg, bool pre_queue) {
    if(pre_queue) { return true; }   /* nothing to do early; defer as before */
    ... existing destructor body ...
    return false;                    /* ignored */
}
```

**2. Owns a thread and a connection** (session / plc / conn) — the case this is for:

```c
bool session_destroy(void *arg, bool pre_queue) {
    ab_session_p session = arg;

    if(pre_queue) {
        /* fast, non-blocking: start the wind-down now, in parallel with every
         * other session being released at about the same time. */
        atomic_set_int32(&session->terminating, 1);
        cond_signal(session->session_wait_cond);
        socket_wake(session->sock);
        return true;                 /* still need the join */
    }

    /* deferred: the handler has walked its close states and exited (§12.4) */
    thread_join(session->handler_thread);      /* bounded -- see §12.5 */
    thread_destroy(&session->handler_thread);
    if(!session->close_completed) { ...fallback close... }
    ... free fields ...
    return false;
}
```

**3. Pure memory** — no owned thread, no internal mutex, nothing blocking:

```c
bool buf_destroy(void *arg, bool pre_queue) {
    (void)pre_queue;
    ... free fields ...
    return false;                    /* free now; never touches the queue */
}
```

Shape 3 is a net win over today, where *every* rc object round-trips through the queue and
the single RC thread regardless of how trivial its teardown is.

### 12.3.1 Constraints on the phase-1 (`pre_queue == true`) call

It runs on whatever thread dropped the last reference — an application thread inside
`plc_tag_destroy()`, the tickler thread inside a callback, the RC thread during a transitive
release, or an IO thread. So it must be:

- **Non-blocking.** Flag stores, `cond_signal`, `socket_wake`. No network I/O, no joins,
  no waits.
- **Lock-free with respect to the object's own mutexes.** `rc_dec` is called from inside
  `critical_block(session->session_mutex)` in places; taking that mutex in phase 1 would
  self-deadlock.
- **Non-recursive.** No `rc_dec` calls from phase 1.

### 12.3.2 Hazard: returning `false` frees on the caller's thread

This is the sharp edge of the design and the reason shape 1 is the default.

Returning `false` frees the object **synchronously, on the releasing thread**. If any
`rc_dec(X)` call site holds a lock that lives *inside* X — the pattern
`critical_block(session->session_mutex) { ... rc_dec(session); }` — then the synchronous
free destroys the mutex that `critical_block` is about to unlock on scope exit. Immediate
use-after-free.

Unconditional deferral currently hides this, and that is precisely what the comment at
`rc.c:240-247` means by "destructors don't run in arbitrary user threads ... avoiding
complex thread synchronization issues."

A destructor cannot determine this for itself — it is a property of its **call sites**, not
of the type. So returning `false` requires a per-type audit: *no `rc_dec` of this type may
be called while holding a lock owned by this object.* The conservative rule:

> Return `false` only for leaf objects with no internal mutex, no owned thread and no
> condvar. Everything else returns `true`.

Migrate everything as shape 1 first; convert to shape 2 or 3 only with the audit done.

### 12.3.3 Incidental benefit: three fewer function-pointer casts

Three call sites currently launder the destructor through a cast —
`(rc_cleanup_func)ab_tag_destroy` (`ab_common.c`), `(rc_cleanup_func)system_tag_destroy`
(`system.c`), `(rc_cleanup_func)omron_tag_destroy` (`omron_common.c`) — because those
functions are declared taking a concrete tag pointer rather than `void *`. That is undefined
behaviour under `-fsanitize=function` and is part of why the check is currently suppressed
in `cmake_toolchains/clang_or_gcc.cmake`.

Since every site has to be touched for the signature change anyway, declare these three as
`bool f(void *, bool)` and cast internally. This does not let the suppression be dropped —
the tag vtable dispatch is the larger source — but it removes three instances and does not
add new ones.

### 12.4 The prerequisite: `terminating` must drive the close states

Today `session_handler()` is `while(!atomic_get_int32(&session->terminating) && ...)`
(`session.c:1355`), so setting `terminating` exits the loop *immediately, without closing*.
That is why `session_destroy()` performs `perform_forward_close()`, `session_unregister()`
and `session_close_socket()` itself, after the join.

For the split to pay off, `terminating` must instead route the handler into its existing
teardown states — `SESSION_DISCONNECT`, `SESSION_UNREGISTER`, `SESSION_CLOSE_SOCKET`
(`session.c:1295-1297`) — which are already reachable and exercised by the idle-disconnect
path (`session.c:1538-1540`). The handler then does its own orderly close and exits, and the
destructor is reduced to join-and-free.

This is the same change §11 requires for the library-shutdown case. Unifying them means
**one** close path, driven by `terminating`, regardless of whether the trigger was the last
tag being destroyed (§9.5 layer 1) or a library shutdown (§9.5 layer 2). Today those are two
separate code paths that must be kept in agreement.

The destructor keeps the close sequence as a **fallback**, guarded by a
`close_completed` flag set by the handler: if the handler already exited — on error, or
because it was never started — the destructor still closes properly.

### 12.5 Consequences elsewhere in this design

- **§9.1 step 3** (destroy all tags) now starts every session closing in parallel as each
  tag's last reference drops, so by step 6 most are already done.
- **§9.5's drain-bound problem largely dissolves.** The RC thread no longer performs
  blocking network I/O per entry, so `SHUTDOWN_DRAIN_TIMEOUT_MS` covers concurrent
  wind-down rather than sequential closes. The bound is still required, and the idle
  indicator in §9.2.1 still must distinguish "no destructor running" from "queue empty",
  but the expected drain time drops from `N * seconds` to roughly one connection's close.
- **Existing hazard worth fixing alongside:** `thread_join(session->handler_thread)` in
  `session_destroy()` is unbounded. A wedged handler hangs the RC thread and therefore all
  subsequent cleanup. It should be a bounded join with a loud warning.

### 12.6 Why not have the RC thread run both phases

An alternative is two passes on the RC thread: call every queued entry with
`pre_queue == true`, then make a second pass with `false`. That avoids running library code
on arbitrary threads and sidesteps §12.3.2 entirely. It is rejected because phase 1 is then
delayed until the RC thread reaches the queue — and during shutdown the RC thread may be
occupied by a slow entry — which loses most of the head start. Running phase 1 on the
releasing thread starts the wind-down at the earliest possible instant, which is the entire
point.

## 13. Implementation order

Two orderings matter here and are worth keeping distinct: **dependency order** (what must
exist before what will compile or behave correctly) and **which steps fix the races
currently failing in CI** versus which are correctness/latency improvements the redesign
motivates but that TSan is not currently red on. Conflating them risks treating "more
thorough" as a precondition for "CI is green," when it isn't.

The failures in CI are all one thing: `lookup_tag()` and the direct `tag_lookup_mutex`
users touching library-scoped state while shutdown frees it. Steps 1-4 below are what fix
that. Steps 5-7 are required for the redesign to be a *correct and complete* replacement for
today's shutdown (orderly protocol close, callback delivery during teardown, parallel
session wind-down) — genuinely worth doing, but not what the failing tests are asking for.

1. **`lib_instance_t`, `instance_lock`, `lib_instance_acquire()`, `lib_state_get()`**
   (§4-5). Pure addition — nothing calls any of it yet, so nothing currently passing can
   regress. Unit-testable in complete isolation: concurrent acquire/release against a fake
   create/destroy cycle, no protocol code involved. Everything downstream trusts the
   acquire semantics in §5, so get those right here rather than patching them later.

2. **`lib_instance_start()` and the `plc_tag_create_impl()` acquire/transfer**; `tag->instance`
   and the destructor release (§6-7). First step that changes real behaviour. Exercise
   against the existing concurrent-create case: `test_shutdown_restart`'s ten threads
   racing `plc_tag_create()` — one wins the CAS, the other nine must still succeed.
3. **`lookup_tag()` rewrite** (§8), and sweep the remaining direct
   `critical_block(tag_lookup_mutex)` sites onto `tag->instance`. This is the actual fix
   for the TOCTOU that CI is failing on.
4. **`refcount_drain()`** in `rc.c` (§9.2.1) — small and self-contained, unit-testable
   against the RC queue alone before it is wired into anything — then
   **`plc_tag_shutdown()` resequencing** (§9.1): the atomic gate close and the
   tickler-thread reentrancy guard (§9.4). This is the fix for the create-racing-shutdown
   and non-atomic double-shutdown races.

   **Checkpoint: run the failing CI suite (TSan + ASan/UBSan, both platforms) here.** This
   is where the race conditions are actually resolved. It tells you whether the rest of this
   list is needed before you spend time on it, rather than after.

5. **Tickler and RC loop conditions** (§10) — switch to `hashtable_entries()` plus a state
   check, so tag-destroy callbacks raised during shutdown are still delivered. This is a
   shutdown-*correctness* fix, not a race fix, and it is a prerequisite for step 6: routing
   real protocol teardown through the handler requires the tickler still be alive to see it
   through.
6. **IO thread phase checks + routing `terminating` through the close states** (§11 and
   §12.4, unified — the same change makes the handler walk its own teardown either way).
   This is where "shuts down without crashing" becomes "shuts down cleanly" — ForwardClose,
   UnregisterSession, orderly socket close all preserved. Test specifically against the
   idle-disconnect/reconnect and `@connection` suites, since those are what would regress if
   this were done carelessly.
7. **RC two-phase destructor** (§12.2-12.3):
   - **7a.** `rc_cleanup_func` signature change plus mechanical shape-1 migration of all 13
     `rc_alloc()` sites. Behaviour-preserving, and does not depend on anything above — this
     half can land on its own, any time, including before step 1, as a self-contained PR.
   - **7b.** Convert session / plc / conn to shape 2, with the bounded join (§12.5) and the
     `close_completed` fallback guard (§12.4). This half is pointless before step 6 exists —
     there is no orderly-close path yet for the phase-1 disposer call to kick off, so
     landing 7b early buys nothing. Convert leaf types to shape 3 only where the §12.3.2
     audit (no `rc_dec` of the type under its own lock) is done.
8. **Delete `lib_active` and the old `tag_lookup_mutex` / `tags` globals.** Cleanup, last,
   once nothing references them.

Net: 1-4 is the race fix and should be scoped, landed, and checkpointed against CI as its
own unit. 5-6 is the shutdown-correctness follow-up. 7a can be pulled forward and done
whenever convenient since it stands alone; 7b waits on 6 regardless of when 7a lands.

## 14. Verification

- `test_shutdown_restart`, `test_shutdown_cip`, `test_shutdown_modbus` under TSan and
  ASan/UBSan on macOS and Ubuntu — these are the jobs currently failing.
- The idle-disconnect / reconnect and `@connection` tests, to confirm orderly protocol close
  is preserved.
- Callback tests, to confirm `DESTROYED` / `ABORTED` delivery during shutdown.
- A new stress test: repeated shutdown/restart cycles, asserting no per-cycle leak (the
  instance must actually be freed each cycle, not leaked).
- `plc_tag_shutdown()` called concurrently from several threads, and called repeatedly —
  exactly one should do work, the rest should return immediately (§9.4).
- `plc_tag_shutdown()` called from a tag callback — must log and return, not hang (§9.4).
- Destroying the last tag and then continuing to create and use new tags: layer 1 must close
  the session cleanly (ForwardClose / UnregisterSession observable at the emulator) and the
  library must stay RUNNING and serve the new tags (§9.5).
- Shutdown with several sessions open: all should close concurrently, and total shutdown
  time should be close to a single connection's close rather than N times it (§12.1). The
  emulator should observe N ForwardClose / UnregisterSession exchanges overlapping in time,
  not serialized.
- A wedged handler thread (emulator stops responding mid-close) must not hang shutdown —
  the bounded join warns and proceeds (§12.5).

# Tag schedule rework for 2.7.3

What changed in the 2026-11-08/09 session, why, and what it measured. The release checklist
(`release_2_7_3_must_do.md`) has the raw tables; this is the overview.

Status: everything below "Uncommitted" is in the working tree, not committed. The ASan
simulator suite passes 331 of 331, unit tests 14 of 14.

---

## 1. Committed earlier in the session

| Item | Change |
|---|---|
| #1 | MicroLogix `L19:0` test writes one value (`--write=7 --check`) |
| #2 | `test_auto_read` fails when a tag reads less than half its expected count |
| #5 | `test_many_tags --idle-s` and `--max-idle-cpu-pct`, off by default |
| #7 | `test_many_tags --passes --read-s`, memory lines; new `run_tag_ramp.py` |
| #9 | `run_perf_compare.py`: machine CPU and busiest client and server thread |
| #7 fix | Tickle walk capped at 512 queued; expire walk skipped in `SESSION_IDLE` |

---

## 2. Uncommitted: the tag list is a treap

The handler's list of tags was a vector sorted by `op_time`. Insert used a binary search
and then a memmove, and remove used a linear search and then a memmove. At 200000 tags the
memmove was 44% of the client.

Two replacements were trialled:

| Structure | Ramp 200000 | Mixed periods, 7000 tags |
|---|---|---|
| Array (HEAD) | 459 ms per pass | 11% CPU, passes |
| Sorted list + `due_tail` | 137 ms | 99% CPU, fails |
| Treap | 152 ms | 6% CPU, passes |

The list is O(1) when an insert lands at either end, as in the ramp. With mixed auto-sync
periods, every re-arm lands in the middle, and finding the place is O(N). The treap is
O(log N) in every case.

The treap (`lib/tag_schedule.c`):

- a binary search tree on `op_time`, and a max-heap on a random 64-bit priority from
  splitmix64;
- 32 bytes per tag: `sched_left`, `sched_right`, `sched_parent`, `sched_priority`;
- equal times keep insertion order: an equal key goes right, and rotations keep the in-order
  sequence;
- `tag_schedule_first()` and `tag_schedule_next()` walk it in time order.

---

## 3. Every scheduled tag stays on the treap

Every tag except `@connection` and the system tags is on its connection's treap from
activation until it is destroyed. A move (re-arm, wake, fragment, abort) unlinks and
reinserts in one step under the connection mutex, so no other thread ever sees a tag off the
treap. An abort drops the tag's reply and moves it to due now in that one step
(`cip_conn_abort_tag()`, Modbus `requeue_aborted_unsafe()`). A tag with no work is re-armed
instead of taken off.

### Op states (`lib/tag.h`)

| State | Meaning | `op_time` |
|---|---|---|
| `IDLE` | nothing due | now + 1 h (`TAG_DRIVER_IDLE_OFFSET_MS`) |
| `SYNC_WAIT` | auto-sync, next read or write | its next time |
| `DESTROY` | idle, auto-destroy armed | now + `destroy_idle_sec` |
| `READ_REQUEST` / `WRITE_REQUEST` | queued, waiting to be packed | now |
| `READ_RESPONSE` / `WRITE_RESPONSE` | on the wire | unchanged; keeps its place |

`SYNC_WAIT`, `READ_RESPONSE` and `WRITE_RESPONSE` are new. `tag_op_busy()` covers the
REQUEST and RESPONSE states.

### Transitions

| From | Event | To |
|---|---|---|
| IDLE, SYNC_WAIT, DESTROY | read or write starts | REQUEST |
| REQUEST | packed | RESPONSE, in place |
| RESPONSE | fragment, failed exchange | REQUEST |
| RESPONSE | done | SYNC_WAIT, DESTROY or IDLE |
| any | abort | due now, then re-armed |
| IDLE | reaches the head | IDLE, an hour out |
| DESTROY | reaches the head | removed and destroyed |

`arm_unsafe()` (`lib/tag_driver.c`) decides the state and time. A due IDLE or DESTROY tag
needs no connection, so it doesn't count for `has_due` (no reconnect for a re-arm). The
expire walk re-arms it in every handler state.

---

## 4. Reply matching without slots

The exchange slot tables are gone. A packed tag is in a RESPONSE state with a due time in
the past, so it is at the front of the treap.

| | CIP | Modbus |
|---|---|---|
| At send | `tag->reply_index` = its request's index in the packet | its transaction id goes into `plc->in_flight[16]` |
| Reply | walk from the head; each RESPONSE tag reads its own sub-reply by index | id not in the array → dropped; else walk to the RESPONSE tag with that `seq_id` |
| Stop | after the number of requests sent, or at the first tag not yet due | at the first tag not yet due, or the end |
| Abort or destroy | `reply_index = -1`; its reply is dropped | its entry is emptied (`0xFFFFFFFF`) |
| Failed exchange, reconnect | RESPONSE → REQUEST, in place | same; the array is emptied |

The CIP walk stops on the number of requests *sent*, not the count in the reply, so a PLC
that answers short still gets every packed tag failed rather than stranded in RESPONSE. The
request sizes for framing the packet are a local array in `process_requests()`.

---

## 5. Handler overhead

| Change | Where |
|---|---|
| Notify only tags with events (`event_count > 0`), or an auto-destroy | `tickle_tags_unsafe()`, `tag_driver_done_unsafe()` |
| Drain the notify list by index, then reset once: O(n), not O(n²) | `tag_driver_notify()` |
| Tickle walk stops after one packet of queued requests (200) | `TAG_DRIVER_TICKLE_QUEUED` |
| `tickle_tag()` uses the caller's `now`: no clock reads per tag | `tickle_tag()` |
| Skip an `@connection` watcher that has read the whole ring | `connection_tag_notify_watchers()` |

Callbacks still run outside `session_mutex`, through the notify list, so a callback may
call the library on any tag without risking a lock-order deadlock.

---

## 6. Auto-destroy and special tags

| Change | Effect |
|---|---|
| `scheduled` bit in the base tag, set by `cip_tag_alloc()` and Modbus `create_tag_object()` | marks tags that live on a handler's treap |
| `destroy_tag_after_idle_sec` needs `scheduled` | create → `BAD_PARAM`, setter → `NOT_ALLOWED` |
| `@connection` and system tags | refused; before, the attribute was accepted and never fired |
| CIP tag still identifying its device | allowed; the bit doesn't depend on its vtable |
| System tag names | only `version` and `debug`; anything else (`@connection`, `@raw`, …) is `BAD_PARAM` at create |

---

## 7. Bug fixed: Modbus aborted create

Before, when a Modbus create read was aborted, CREATED reported the abort but
`plc_tag_status()` returned OK, and reads and writes worked. `mb_tag_status()` and
`mb_submit()` now return `create_result`, as CIP does, so the create fails for good. This
bug is present on HEAD as well.

---

## 8. Tests

| Test | New or changed |
|---|---|
| `test_tag_schedule` (unit) | treap structure check; randomized ops against a reference model, including tie order |
| `test_auto_read --periods=…` | mixed auto-sync periods; per-period table; suite runs 700 tags |
| `test_write_before_read --abort-create-read`, Modbus | suite entry; found the bug in §7 |
| `test_auto_destroy`, no `plc=` | suite entry: auto-destroy on tags created before the device is identified |
| `test_auto_destroy` | sets data on an auto-destroy tag; it must neither go early nor live longer |
| `test_connection_tag` | auto-destroy refused; `find_or_create_tag` shares one handle |
| `test_special` | system-tag refusals; now runs in the simulator suite |

---

## 9. Results

Final code: the single-step abort, the Modbus in-flight array and everything before them.
Linux, 4 cores, deep idle blocked, Release build. Run 2026-11-10: two runs each for the ramp
and the 7000-70000 mixed tests, one run each at 200000 tags.

### Ramp, median read pass

| Tags | Array (HEAD) | Final |
|---|---|---|
| 200 | 1.20 ms | 1.21 ms |
| 2000 | 2.27 ms | 1.57 ms |
| 20000 | 16.8 ms | 18.3 ms |
| 200000 | 459 ms | 186 ms |

At 200000 tags: 1.08 M tags/s, against 0.43 M for the array.

### Mixed auto-sync periods, 50 to 5000 ms

| Tags | Array CPU | Final CPU | Final reads |
|---|---|---|---|
| 7000 | 11% | 5.6% | all on time, worst gap 1 ms late |
| 35000 | 98%, starved | 21.9% | all on time, worst 3 ms late |
| 70000 | 99%, starved | 43.6% | all on time, worst 3 ms late |
| 200000 | — | 82% | saturated, see below |

### 200000 tags

| Periods | CPU | Result |
|---|---|---|
| 250 to 10000 ms, 30 s | 31.6% | every read on time, worst gap 4 ms late |
| 50 to 5000 ms, 10 s | 82.3% | saturated: asks 1.08 M reads/s, delivers about 0.79 M |

Saturated, only the shortest periods lose reads: 50 ms tags average 99.9 of 200 and 100 ms
tags 98.9 of 100; 250 ms and longer keep their counts. The 50 ms tags sit at the test's pass
floor (half the expected count), so a single run passes or fails on a few percent: the run
before averaged 103 and passed, this one had 1626 tags just under the floor.

Memory: 32 bytes more per tag than the array (1029 against 997 at 200000 tags).

---

## 10. Known limits and open items

| Item | Note |
|---|---|
| Ramp 20000 is 9% slower than the array | each finished read is re-armed into the treap, an hour out, instead of leaving it |
| Abort while a CIP exchange is out | the reply walk can't reach its count and runs to the first tag not yet due; one long walk with a large backlog |
| Modbus abort frees its entry at once | the device can briefly hold one more request than `max_requests_in_flight` |
| Modbus lost reply | holds its entry until an abort; deferred, see the release checklist |
| Modbus abort before activation | tested only through the queued create read; the exact window isn't testable |
| Modbus perf | not measured: the ramp and mixed tests are CIP only (release item #8) |

---

## 11. Files

| Area | Files |
|---|---|
| Schedule | `lib/tag_schedule.c/.h`, `lib/tag_driver.c/.h`, `lib/tag.h` |
| CIP | `modules/cip/conn.c/.h`, `cip.c`, `tag.c/.h` |
| Modbus | `modules/modbus/modbus.c` |
| Other library | `api/lib.c`, `api/tag_attribs.c`, `lib/connection_tag.c`, `modules/system/system.c` |
| Tests | `unit/test_tag_schedule.c`, `test_auto_read`, `test_auto_destroy`, `test_connection_tag`, `test_special`, `run_simulator_tests_parallel.py` |
| Docs | `release_2_7_3_must_do.md` (item 16, deferred table), this file |

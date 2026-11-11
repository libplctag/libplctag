# Must do for the 2.7.3 release

Consolidated 2026-11-08 from `ephemeral_docs/deferred_fixes.md` (DF),
`docs/scale_and_performance_plan.md` (SP), `docs/performance.md` (PF) and
`ephemeral_docs/static_tag_table_plan.md` (ST). Items already fixed and tested are left out.

## Machines

| Machine | Use |
|---|---|
| MacBook Air M1 (`docs/perf/MacBook-Air-M1-16GB`) | **Functional checks only.** Is the test correct, does it finish, are the logs right. macOS cannot block deep idle and the machine has no fan, so its numbers are not release numbers. |
| Linux, Precision Tower 3420 (`docs/perf/Precision-Tower-3420-i5-6500-32GB`) | **All final measurements**, with deep idle blocked. |
| ControlLogix chassis (L81 at 10.206.1.40, `path=1,4`) | Hardware tests through `run_hardware_tests.py`. |

## Summary

| # | Item | Source | Decision | Status |
|---|---|---|---|---|
| 1 | MicroLogix `L19:0` test writes 4 values to a 1-element tag | DF | Write one value | Done |
| 2 | `test_auto_read` passes with zero reads | DF | Fail when reads fall short of the expected count | Done |
| 3 | `PLCTAG_MAX_TAGS` default | ST §7 | 262144 | Done in code; record only |
| 4 | Pad slot table entries to 64 bytes | ST §7 | Keep unpadded; revisit if cache effects show in testing | Decided |
| 5 | Idle CPU phase in `test_many_tags` | SP 1.1 | Add it, off by default (`--idle-s`) | Done |
| 6 | Connection count ramp on the L81, 1-8 connections | SP 1.2 | Short hardware test | To do |
| 7 | CIP tag ramp, 200 to 200000 tags, zero delay | SP 1.3/1.4 | `run_tag_ramp.py` + `test_many_tags --passes --read-s` | Built; 200000 is client-bound, see #16 |
| 8 | Modbus tag ramp, 50 to 50000 tags | SP 2.3 | New ramp test, stop at 100% server CPU | To do |
| 9 | Machine-total and per-thread CPU monitoring | SP 3.4 | Added to `run_perf_compare.py`; macOS path untested | Done (Linux) |
| 10 | v2.7.2 vs HEAD: no regressions | SP 3.5 | Keep the pairwise comparison; no 2.7.0/2.7.1 table | Run on Linux |
| 11 | Fairness gate thresholds | SP 3A | Functional on M1, final on Linux | To do |
| 12 | Release regression gate | SP 4 | Covered by 5-11 | Follows from them |
| 13 | Allocation counting by interposing | SP 5 | macOS and Linux | To do |
| 14 | Re-run Linux fairness and Modbus scale matrices, deep idle blocked | PF | User runs | To run |
| 15 | Final performance numbers | PF | Linux machine | To run |
| 16 | Tag list insert and remove cost | PF | Array, list and treap trialled | Treap committed (c4753f13) |
| 17 | Small first packet of an async cycle | PF | Deferred: only a tight poll loop shows it | Deferred 2026-11-08 |
| 18 | Backoff unit tests on Windows and ARM in CI | DF | Not reviewed | Check the next CI run |

Deferred or closed are listed at the end.

---

## 1. MicroLogix `L19:0` test writes 4 values to a 1-element tag

- **Where:** `src/tests/scripts/run_simulator_tests_parallel.py:1378`, the "L data file
  Micrologix tag read/write" case: `tag_rw2 --type=sint32 ... name=L19:0 --write=0,1,2,3`.
- **Problem:** the tag has no `elem_count`, so it holds one element. `update_values()` writes
  while `offset < len`, so only the first value goes out and three are dropped silently. The test
  claims more than it checks, and `tag_rw2 --check` would report it as an error, which is why
  this case never got `--check`.
- **Decision:** write one value. Then add `--check`, as the lgxpccc `L19:0` case at line 1480
  already does (`--write=7 --check`).

## 2. `test_auto_read` passes with zero reads

- **Where:** `src/tests/test_auto_read/test_auto_read.c`. The result is
  `stats_assess_fairness()` alone (about line 373, `return fair ? 0 : 1`).
- **Problem:** found 2026-10-08. A run with `reads=0` printed `Result: PASS`, because one tag that
  never reads is perfectly fair.
- **Decision:** the test knows its auto-sync period and its recording time, so the read count per
  tag is predictable: about recording time / `auto_sync_read_ms`. Fail if any tag has zero reads,
  and fail if a tag's count is far below the expected count. Set the floor loose (for example half
  the expected count) so CI stalls do not trip it; the point is to catch a tag that stopped, not
  to measure the rate.

## 3. `PLCTAG_MAX_TAGS` default

- **Decision:** 262144. This is already the value in `src/libplctag/lib/tag_registry.c:63`.
- Cost: 16 bytes per entry on 64-bit (12 on 32-bit), so 4 MB (3 MB) of BSS. BSS costs no pages
  until touched on a system with virtual memory. On a target without it, all of it is real RAM;
  such targets override the define.
- The 2026-11-07 benchmarks in `docs/performance.md` ran with 65536 slots. The final Linux runs
  (item 15) must use 262144 so the release numbers match the release code.

## 4. Pad slot table entries to 64 bytes

**Which entry:** `tag_entry_t` in `src/libplctag/lib/tag_registry.c`, the element of the static
`tag_table[PLCTAG_MAX_TAGS]`:

```c
typedef struct {
    lock_t lock;    /* guards tag_id and tag */
    int32_t tag_id; /* == tag->tag_id while tag is set */
    plc_tag_p tag;  /* holds the registry's reference; NULL when free */
} tag_entry_t;
```

- Unpadded it is 16 bytes on 64-bit, so four entries share one 64-byte cache line.
- Tag ids are handed out in order, and the slot is `tag_id % PLCTAG_MAX_TAGS`, so tags created
  one after another sit in neighbouring entries. Two threads looking up neighbouring tags take
  different spin locks on the same cache line, and the line moves between cores (false sharing).
- Padding each entry to 64 bytes removes that, at 16 MB of BSS at 262144 entries instead of 4 MB.
- Evidence so far: the unpadded table already cut `lookup_tag` from 41.6% to 6.4% of client CPU
  (async read 1000 × 8) and was the same or better than the hashtable in all 384 cells.
- **Decision (2026-11-08):** keep unpadded, as now. Revisit if cache effects show in testing,
  for example `lookup_tag` cache misses as a hot spot with many threads on neighbouring tags.
  One A/B run on Linux (8 threads, async, 1000 tags per thread, padded vs unpadded) would then
  settle it.

## 5. Idle CPU phase in `test_many_tags`

- **Where:** `src/tests/test_many_tags/test_many_tags.c`. Phases today: create, write, read,
  destroy (`phase_t`, line 76).
- **Plan (SP 1.1 phase 4):** after the read phase and before destroy, leave every tag created
  with no operations for a fixed time (`--idle-s=S`, default 0 = off, so the simulator suite
  is not slowed; the ramps pass `--idle-s=10`) and measure the
  process CPU over that time with `getrusage(RUSAGE_SELF)` (macOS and Linux; Windows
  `GetProcessTimes`). Log CPU seconds and percent of one core.
- **Why:** it catches work that grows with the tag count instead of with activity, for example
  a scan over all tags on every I/O thread wake.
- **Check:** a limit option (`--max-idle-cpu-pct`) so the ramp tests can fail on it. Default off
  until there is data from the Linux machine.

## 6. Connection count ramp on the L81

- **Goal:** find where adding connections stops helping and starts hurting. Not exact numbers.
- **Hardware only.** Wired into `src/tests/scripts/run_hardware_tests.py`, not the simulator
  suite.
- **Target:** `gateway=10.206.1.40&path=1,4&plc=ControlLogix`, `TestBigArray` (at least 1000
  DINT elements on the L81).
- **Runs:** async read of 1000 one-element tags, `--groups` 1 through 8, a few seconds each
  (about 3 s run, short pause between), with `perf_benchmark` (it already takes `--groups`,
  `--group-by=tag`, `--mode=async`, `--duration`). Reads only, so it is safe on the shared PLC.
- **Output:** ops/s per connection count in the log, and the count after which throughput
  drops. The test fails only on errors or a run that does no reads; the curve is reported, not
  judged, because the PLC is shared.
- Background (SP 1.2): N is the smallest connection count within 5% of the peak. The simulator
  ramp to 128 groups is dropped; the PLC limit is the one that matters.

## 7. CIP tag ramp, 200 to 200000 tags, zero delay

- **Goal:** the fastest time to read every tag once, at each tag count. This is the baseline for
  the shortest sustainable auto-sync period.
- **Server:** `ab_server` with no delay.
- **Ramp:** 200, 2000, 20000, 200000 tags.
- **Per step:**
  1. Create every tag async (timeout 0, then wait for all). **Not timed as part of the result.**
  2. Read every tag, async, and wait for all. Repeat for a fixed time or pass count.
  3. Destroy every tag. **Not timed as part of the result.**
- **Report per step**, in the log: fastest pass (ms to read all tags), median pass, maximum read
  rate (tags/s), client CPU and `ab_server` CPU during the read passes. Setup and teardown times
  are logged separately.
- **Server tags:** `ab_server` and libplctag allow at most 65536 elements per array, so 200000
  tags need several arrays: `perfTag1` .. `perfTagK`, `DINT[200000/K]`, K at least 4 (SP 1.1).
  One array per connection also keeps connections off each other's server mutex.
- **Expected limit:** the `data_mutex` per tag in `ab_server` (`cip.c`). At 2026-10-31 it was
  about 17% of busy server samples at 8 connections with no delay (SP 3B). With every tag on a
  few arrays it is the likely bottleneck. Record server CPU per thread so this shows directly.
- **Tool:** extend `test_many_tags` (it already creates async, times phases and spreads tags over
  groups) with a repeated read phase and a `--ramp` list, plus a runner script that starts
  `ab_server` with the arrays. The script also sets `ulimit -n 1024`.
- **Machines:** functional check on the M1 (does every step finish, are the logs right); the
  numbers come from the Linux machine.
- **Built 2026-11-08:** `src/tests/scripts/run_tag_ramp.py --bin-dir=<Release bin_dist>`.
  `test_many_tags` gained `--passes` and `--read-s` (repeat reads for at least R passes and S
  seconds), logs every pass, then fastest/median/slowest pass, rates, client CPU%, and peak RSS
  before create, after create (bytes per tag) and after reads. The runner samples `ab_server`
  CPU, busiest thread and RSS, and the machine total, over the read passes, and ends with a table.
- **First run 2026-11-08** (Linux, Release HEAD + this work, 1 connection, 1 thread, no
  delay, at least 10 passes and 3 s per step):

  | Tags | Median pass | Median tags/s | Client CPU | Server CPU | Client B/tag | Create |
  |---|---|---|---|---|---|---|
  | 200 | 1.2 ms | 167k | 22% | 7% | (page noise) | 2 ms |
  | 2000 | 2.3 ms | 874k | 79% | 24% | 1100 | 15 ms |
  | 20000 | 20.5 ms | 977k | 121% | 25% | 1003 | 147 ms |
  | 200000 | 1870 ms | 107k | 103% | 3% | 997 | 1500 ms |

  20000 to 200000 tags is 10 times the tags and 91 times the pass time, with the client CPU
  bound and the server idle: something in the client is O(N) per tag.
- **Profiled 2026-11-08:** not the search (`vector_find_index` under 0.5%). 27% `memmove` (sorted
  vector insert and remove) and 32% two walks per packet, `tag_driver_tickle_due_unsafe` and
  `tag_driver_expire_due_unsafe`. Both walked the whole due prefix, which after 200000 async
  `plc_tag_read` calls is 200000 queued, not yet sent, requests, looking for non-request
  entries. One packet takes about 165.
- **Fixed 2026-11-08:** the tickle walk stops after 512 queued requests (no packet holds more;
  a due tag behind them would queue behind them anyway), and the expire walk is skipped in
  `SESSION_IDLE`, where the tickle walk already takes due DESTROY tags.

  | Tags | Median pass before | After | Median tags/s after |
  |---|---|---|---|
  | 20000 | 20.5 ms | 16.8 ms | 1.19 M |
  | 200000 | 1870 ms | 457 ms | 438 k |

  `memmove` is now 44% of client CPU at 200000 tags and is what is left of the O(N) per tag
  (item 16).

## 8. Modbus tag ramp, 50 to 50000 tags

- Same shape as item 7 against `modbus_server`, holding registers `hr0` up to `hr49999` (one unit
  has 65536, so the unit-ID FIXME does not block this).
- **Ramp:** 50, 500, 5000, 50000 tags.
- **Stop rule:** `modbus_server` is single threaded. Sample its CPU during each step; when it
  reaches about 100% of one core, log that the server is the limit and stop the ramp. Steps
  beyond that measure the server, not the library.
- Known server cost: `log_is_enabled()` (`src/poc/utils/log.c:130`) loops over all 64 modules
  on every log call, about 8% of busy samples. Left alone (deferred).
- Server CPU at 2026-10-31: 78% of its one core at 8 connections, no delay. Expect the stop rule
  to trigger.

## 9. Machine-total and per-thread CPU monitoring

- **Today:** `run_perf_compare.py` samples client and server CPU with `ps -o time=` and RSS with
  `ps -o rss=`.
- **Add, only if small:**
  - Machine total: sum of `ps -A -o %cpu=` during the run. Same command on macOS and Linux.
  - Per-thread client CPU: Linux `ps -L -o tid=,pcpu= -p <pid>` (or `/proc/<pid>/task/*/stat`),
    macOS `ps -M -p <pid>`. Keeps application threads and I/O threads apart.
  - Record which ran out first: client, server or machine (machine near cores × 100%, or one
    process at its thread count × 100%).
- The ramp tests in items 7 and 8 use the same sampling for server CPU.
- **Done 2026-11-08:** `run_perf_compare.py` writes `machine_cpu_pct`, `client_max_thread_pct`,
  `server_max_thread_pct` and `limit` (machine, server, client or blank) as the last CSV
  columns, and prints them in the verdict table. Linux reads `/proc`: `ps -o time=` there has
  1 s resolution, so the old server CPU% moved in 10% steps on a 10 s run; it now has 10 ms.
  macOS uses `ps -M` and `ps -A`. Per-thread end samples are taken 0.5 s before the window
  ends, since client and server connection threads exit right after it. Machine total is
  not `ps -A -o %cpu`, which on Linux is a lifetime average.
- **Still to do:** functional check of the macOS path on the M1.

## 10. v2.7.2 vs HEAD: no regressions

- **Decision:** the release criterion is no regression from v2.7.2 to HEAD. The table of every
  release from 2.7.0 (SP 3.5) is dropped.
- "Better" needs higher ops/s, and lower or equal CPU per op and memory (see
  `docs/performance.md`, Metrics). A cell is worse on throughput if B is more than 5% lower and
  the ranges do not overlap.
- Async throughput depends on the phase between the 1 ms poll sleep and the server delay, ±20%
  either way. An async loss that comes with lower CPU and a longer p95 is that effect until a
  `--poll-ms=0` run says otherwise.
- **State:** on Linux with deep idle blocked, the ControlLogix scale matrix shows no loss that is
  not the poll effect (2026-11-07). The fairness and Modbus matrices still need that run
  (item 14).

## 11. Fairness gate thresholds

- **Rules (SP 3A):** tag fairness within one connection, ±1 for `test_fairness` and for one
  thread; tag CV for several threads. Thread fairness as the worst 1 s window min/max across
  threads.
- **Built:** `perf_benchmark` window columns (`window_min_max`, `window_cv`,
  `conn_window_min_max`, `tag_spread`, `window_s`), `--group-by=tag`, `test_fairness --groups=C`,
  `run_perf_compare.py --fairness-matrix`.
- **Known:** the 8 × 8 async ControlLogix tag spread (12 against 4 on v2.7.2) is the thread speed
  gap over more cycles: 1.3% relative on HEAD against 2.9% on v2.7.2.
- **To do:**
  1. Functional check on the M1: the matrix runs, the columns fill, the summary prints a
     verdict.
  2. Final on Linux with deep idle blocked: 5 HEAD runs, then write the thresholds for tag CV and
     thread min/max into `docs/performance.md`.
- Not in CI: CI hardware is overcommitted and stalls.

## 12. Release regression gate

Covered by items 5-11: the ramps give create, read and idle CPU at large tag counts, item 10
gives throughput, CPU per op and memory, item 11 gives fairness. Nothing extra to build.

## 13. Allocation counting by interposing

- **Goal:** allocations per operation in steady-state reads (target 0) and per tag created, for
  v2.7.2 and HEAD alike, with no library change.
- **Feasible on both:**
  - Linux: `LD_PRELOAD` of a small shared library defining `malloc`, `calloc`, `realloc`, `free`,
    forwarding through `dlsym(RTLD_NEXT, ...)`. `dlsym` can call `calloc` itself, so the first
    calls need a small static bootstrap buffer.
  - macOS: `DYLD_INSERT_LIBRARIES` with `DYLD_INTERPOSE` entries in the `__DATA,__interpose`
    section. Works on our own test binaries; SIP strips the variable for system binaries such as
    `/usr/bin/python3`, so run the test executable directly, not through a system interpreter.
- libplctag allocates only through `mem_alloc`/`mem_realloc`/`mem_free`, which call `calloc`,
  `realloc` and `free` (`src/platform/platform_common.c:62-77`), so the interposer sees every
  allocation. It also sees libc's own (thread creation, stdio buffers); counting from a marker
  after setup keeps those out of the steady-state number.
- **Output:** counts and bytes, printed at exit and at a marker the test can trigger (a signal
  or an environment variable naming a phase).
- Relates to the no-runtime-allocation goal: this is the measurement that shows progress.

## 14. Re-run Linux fairness and Modbus scale matrices

- Linux machine, deep idle blocked. The user runs these.
- The ControlLogix scale matrix was done 2026-11-07. The Modbus run is still wanted for the
  release record.

## 15. Final performance numbers

- From the Linux machine, with the release code (262144 slots, item 3).
- The M1 is not a reliable platform for large-scale performance; it is for functional checks.

## 16. Tag list insert and remove cost

- **Background (PF):** the tag list on each connection is a vector, and `vector_find_index` is a
  linear search. At auto-sync with 10000 tags it is what is left of the per-exchange cost
  (248 µs per exchange in a scratchpad benchmark against 9 µs for a red-black tree). Polling
  is cheaper with the vector (6 against 16 µs).
- **Decision:** before changing the structure, measure how long it takes to insert a tag into
  the list, remove it, and delete it, at the tag counts of the ramps (items 7, 8).
- The M1 can show where time goes (profiles, call counts). The Linux machine gives the numbers.
- The red-black tree stays optional and depends on this measurement.

### Result (2026-11-09): sorted doubly linked list

The vector became a sorted doubly linked list through the tags (`tag_list_t`: head, tail,
`due_tail`). Remove is O(1). An insert at or after the tail appends, and a due insert goes
after `due_tail`, the last due tag inserted. Only a future time before the tail walks.

Ramp (item 7), Linux, deep idle blocked, median of 10+ passes, two runs each:

| Tags | Array ms | List ms | Change |
|---|---|---|---|
| 200 | 1.20 | 1.20 | same |
| 2000 | 2.27 | 2.25 | same |
| 20000 | 16.8 | 13.8 | -18% |
| 200000 | 465 / 524 | 137 | -71% |

- The list scales linearly: 20000 to 200000 tags is 10× the time.
- 200000 tags: 1.46 M tags/s against 0.43 M. The client uses 133% CPU against 112%.
- Memory: 16 more bytes per tag (two pointers).

### Mixed auto-sync periods (2026-11-09): the list is worse than the array

`test_auto_read --periods=50,100,250,500,1000,2000,5000`, tags spread evenly, 10 s, Linux,
deep idle blocked. Each re-arm lands in the middle of the list, so the insert walks.

| Tags | Array CPU | List CPU | Array result | List result |
|---|---|---|---|---|
| 7000 | 11% | 99.5% | pass | 50 ms tags read 49 of 200 |
| 35000 | 98% | 99.9% | 50 ms tags read 69 of 200 | almost no reads |
| 70000 | 99% | 100% | about 10 reads per tag | creates time out |

- Profile at 7000 tags: 94% in `tag_schedule_insert` (the walk in `find_prev`).
- The array saturates too, at 35000 tags: its middle insert is a memmove.
- Status: needs an index for middle inserts before the list can replace the array.

### Treap trial (2026-11-09)

A treap on op_time with a random 64-bit heap priority (splitmix64), linked through the tags:
left, right, parent and priority, 32 bytes per tag. Equal times keep insertion order (an
equal key goes right). Same runs as above, two of each, Linux, deep idle blocked.

Ramp, median pass:

| Tags | Array ms | List ms | Treap ms |
|---|---|---|---|
| 200 | 1.20 | 1.20 | 1.20 |
| 2000 | 2.27 | 2.25 | 2.30 |
| 20000 | 16.8 | 13.7 | 14.7 |
| 200000 | 459 | 137 | 152 |

Mixed periods, client CPU and 50 ms tag reads of 200:

| Tags | Array | List | Treap |
|---|---|---|---|
| 7000 | 11%, 200 | 99%, 63 | 6%, 200 |
| 35000 | 98%, 69 | 100%, 0 | 26%, 200 |
| 70000 | 99%, 10 | 100%, 0 | 52%, 199 |

- The treap passes every mixed run, and its CPU grows linearly with the read rate.
- On the ramp it is 11% slower than the list and 3× faster than the array.
- Memory: 32 more bytes per tag than the array (1029 against 997).

### Every tag on the treap (2026-11-09)

New op state `TAG_OP_SYNC_WAIT`. A tag with no op is re-armed instead of taken off: SYNC_WAIT
at its next auto-sync time, DESTROY at its idle destroy time, otherwise IDLE an hour out. Only
destroy takes a tag off. The
handler notifies only tags with events. One run, deep idle blocked:

| Test | Treap before | Every tag on treap |
|---|---|---|
| Ramp 2000, median ms | 2.30 | 1.54 |
| Ramp 20000, median ms | 14.7 | 18.0 |
| Ramp 200000, median ms | 152 | 185 |
| Mixed 7000, CPU | 6% | 6% |
| Mixed 35000, CPU | 26% | 24% |
| Mixed 70000, CPU | 52% | 49% |

- The ramp cost is the re-arm: a finished read goes back in an hour out instead of coming off.
- Then `TAG_OP_READ_RESPONSE` and `TAG_OP_WRITE_RESPONSE`: a packed tag stays on the treap in
  place. Same runs: ramp 200000 184 ms, 20000 18.0 ms; mixed 70000 49% CPU, all reads on time.

### Mixed periods at 200000 tags (2026-11-09)

| Periods, ms | Asked reads/s | Client CPU | Result |
|---|---|---|---|
| 50 to 5000 (7 groups), 10 s | 1.08 M | 85% | saturated: 50 ms tags 87 of 200, 100 ms 76 of 100, the rest on time |
| 250 to 10000 (6 groups), 30 s | 0.26 M | 35% | all on time, worst gap 3 ms late |

- Saturated, the client delivered about 0.69 M reads/s, against 1.1 M on the ramp: each
  auto-sync read also costs a tickle and a re-arm.
- Overload falls on the shortest periods only; longer periods keep their schedule.

### Reply matching without slots (2026-11-09)

The tickle stops after one packet of queued requests (200), the notify list drains by index, and
`tickle_tag()` takes the caller's time. Then the exchange slots went: a packed CIP tag keeps its
request's index (`reply_index`) and stays in RESPONSE at the front of the list, so the reply walk
reads each tag's sub-reply directly. Modbus keeps a count of requests in flight and matches a
reply to the due RESPONSE tag with its transaction id. Two runs each, deep idle blocked:

| Test | Before these changes | After |
|---|---|---|
| Ramp 200000, median ms | 182 | 186 |
| Mixed 70000, client CPU | 49% | 44% |
| Mixed 200000, 50 to 5000 ms, CPU | 85% | 82% |
| … 50 ms tags, reads of 200 | 87 | 103 (passes) |
| … delivered reads/s | 0.69 M | 0.80 M |
| Mixed 200000, 250 to 10000 ms, CPU | 35% | 31% |

## 17. Small first packet of an async cycle

- **Background (PF, 2026-11-03):** an async cycle took three round trips: the first
  `plc_tag_read` wakes the idle I/O thread, which sends the 3-6 tags started so far, then a full
  packet, then the rest. Two round trips would save one server delay per cycle.
- **Code today:** `cip_conn_queue_tag()` (`modules/cip/conn.c:1716`) wakes the I/O thread
  unless `rebuild_pending` is set, so the first tag queued on an idle connection still wakes it.
  The settle wait that could have batched it was removed 2026-11-03.
- **May already be fixed** by later changes. Check: async read, 250 tags, 1 × 1, count tags per
  request packet with the `write()` shim used before. If the first packet still carries only a
  few tags, it is still open; decide then.
- **Checked 2026-11-08** (HEAD `ef836b68`, Release, Linux, deep idle not blocked; functional
  only): async read, 250 tags, 1 × 1, `ab_server` 1 ms and 0.3 ms, 3 s. An `LD_PRELOAD` shim
  counted the CIP services in each request packet; cycles are split at every 250 tags.

  | Delay | Poll | Cycles | 2 packets | 3 packets | First packet < 20 tags | First packet median |
  |---|---|---|---|---|---|---|
  | 1 ms | 1 ms | 752 | 735 | 17 | 0 | 86 |
  | 1 ms | 0 | 801 | 2 | 799 | 742 | 13 |
  | 0.3 ms | 1 ms | 1114 | 1059 | 55 | 2 | 87 |
  | 0.3 ms | 0 | 1906 | 10 | 1896 | 1729 | 14 |

  At most about 165 reads fit in one packet, so 2 packets is the minimum for 250 tags.
  With `--poll-ms=1` cycles already take 2. With `--poll-ms=0` (the application starts the next
  cycle the moment the last one ends) the woken I/O thread still sends the 1-20 tags queued so
  far, and the cycle takes 3 round trips. **Still open** for applications that queue reads in a
  tight loop.
- **Decision (2026-11-08):** defer. Only a caller that spins shows it, and it still gets more
  ops/s than with the 1 ms poll.

## 18. Backoff unit tests on Windows and ARM

Not covered in the 2026-11-08 review. The Debug gate is gone (`src/tests/CMakeLists.txt`) and
`ci.yml` runs `ctest -R unit_test` on Windows (lines 907-911) and under QEMU for the ARM cross
jobs (1089-1093). Whether those jobs pass has not been seen yet. Check the next CI run.

---

## Deferred or closed

| Item | Source | Decision |
|---|---|---|
| `ab_server` RMW rejection paths have no test | DF | Accepted gap; the library never sends a malformed RMW |
| Q1: second machine as the server | SP | Deferred |
| P core vs E core gap (`taskpolicy -b`) | SP 3.3 | Dropped; final runs are on Linux with deep idle blocked |
| Mutex wait timer in the servers | SP 3B | Deferred; `sample` already shows the waits |
| `modbus_server` `log_is_enabled()` loop | SP 3B | Deferred |
| User performance guide (wiki) | SP 3C | After solid data from the Linux machine |
| Several network modules to one CPU | SP 3D | Covered by the hardware tests |
| M1 ControlLogix async 5-15% loss, timing runs | PF | Answered by the Linux runs with deep idle blocked |
| Re-check M1 losses with `--poll-ms=0` | PF | Dropped; the M1 is functional only |
| Modbus lost reply fills the in-flight table | 2026-11-09 | Deferred; see below |

### Modbus lost reply fills the in-flight table (deferred 2026-11-09)

Each Modbus request on the wire holds an entry in `plc->in_flight` (16 at most) until its reply
comes or its tag is aborted. A reply that never comes keeps the entry. Today only an abort frees
it: a `plc_tag_read`/`plc_tag_write` timeout aborts its op, but an auto-sync op has no timeout.
Enough lost replies fill the table and the PLC sends nothing more; a filled entry also keeps the
inactivity disconnect from firing (`modbus.c`, "continuing to wait"). The same held with the old
slots. A lost reply means the device or network is at fault. Options when it is taken up: a send
time per entry and a reply deadline, or a reconnect once the oldest request is older than the
inactivity timeout.

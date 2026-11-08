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
| 1 | MicroLogix `L19:0` test writes 4 values to a 1-element tag | DF | Write one value | To do |
| 2 | `test_auto_read` passes with zero reads | DF | Fail when reads fall short of the expected count | To do |
| 3 | `PLCTAG_MAX_TAGS` default | ST §7 | 262144 | Done in code; record only |
| 4 | Pad slot table entries to 64 bytes | ST §7 | Keep unpadded; revisit if cache effects show in testing | Decided |
| 5 | Idle CPU phase in `test_many_tags` | SP 1.1 | Add it | To do |
| 6 | Connection count ramp on the L81, 1-8 connections | SP 1.2 | Short hardware test | To do |
| 7 | CIP tag ramp, 200 to 200000 tags, zero delay | SP 1.3/1.4 | New ramp test | To do |
| 8 | Modbus tag ramp, 50 to 50000 tags | SP 2.3 | New ramp test, stop at 100% server CPU | To do |
| 9 | Machine-total and per-thread CPU monitoring | SP 3.4 | Add if not invasive; macOS and Linux | To do |
| 10 | v2.7.2 vs HEAD: no regressions | SP 3.5 | Keep the pairwise comparison; no 2.7.0/2.7.1 table | Run on Linux |
| 11 | Fairness gate thresholds | SP 3A | Functional on M1, final on Linux | To do |
| 12 | Release regression gate | SP 4 | Covered by 5-11 | Follows from them |
| 13 | Allocation counting by interposing | SP 5 | macOS and Linux | To do |
| 14 | Re-run Linux fairness and Modbus scale matrices, deep idle blocked | PF | User runs | To run |
| 15 | Final performance numbers | PF | Linux machine | To run |
| 16 | Tag list insert and remove cost | PF | Measure; M1 finds where, Linux gives numbers | To do |
| 17 | Small first packet of an async cycle | PF | May already be fixed; verify | To check |
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
  with no operations for a fixed time (10 s by default, an option to change it) and measure the
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

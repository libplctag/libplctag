# Scale and performance plan

Status: plan, nothing built.

Goal: prove 200k tags work on CIP and 50k on Modbus, and track performance and fairness per
release so a release cannot get slower or less fair unnoticed.

## What exists

- `src/tests/perf_benchmark/perf_benchmark.c`: one configuration per run, one CSV line out.
  - Modes: `sync`, `async`, `auto` (auto-sync read, reports achieved period).
  - Columns include ops/s, client `cpu_load_pct`, `cpu_us_per_op`, p50/p95/p99/max latency,
    fairness, `rss_growth_kb`, `create_ms`.
  - Public API only, `REQUIRED_VERSION 2.6.0`, so it builds against 2.7.0 and later.
  - `--groups` is capped at 100.
- `src/tests/scripts/run_perf_compare.py`:
  - A/B of two benchmark binaries in A B B A order, `--rounds` rounds, one server set from HEAD.
  - Verdict: medians differ by more than 5% and min-max ranges do not overlap. When the server
    is over 90% of a core, it compares client CPU per op instead.
  - `--thread-sweep` runs 1-5, 8 and 12 threads, shared connection vs one per thread.
  - `--delay` passes ab_server `--delay=<ms>`.
- `ab_server --delay=<ms>`: whole milliseconds. It sleeps in the session's thread after each
  request, so connections overlap but requests on one connection are serial.
- `modbus_server` (`src/poc/modbus_server`): protothread based (one OS thread), no delay
  option. It ignores the unit ID (`modbus_server.c:429` FIXME), so all unit IDs share one
  register map.
- `run_hardware_tests.sh:40` and `run_modbus_tests.sh:156` set `ulimit -n 1024`. The macOS
  default is 256.
- `docs/perf/MacBook-Air-M1-16GB/`: earlier CSVs and plots, plus `host_info.txt`
  (MacBookAir10,1, 4 P + 4 E cores, 16 GB).
- `connection_group_id` accepts 0-32767.
- Each connection group has its own client I/O thread.

## Decisions

| # | Decision | Result |
|---|---|---|
| D1 | Headline metric for the table | Async read, 1 DINT per tag, one connection per thread. |
| D2 | Modbus tag count | 50k holding registers (65536 per unit). |
| D3 | Where the 200k tests run | The local development machine, started by hand. |
| D4 | Aggregation of repeated runs | Median of 5 interleaved runs, min-max shown. |

## Open questions

| # | Question | Recommendation |
|---|---|---|
| Q1 | Second host as the server (step 3.2): what CPU, OS and link? Does the baseline table use it? | Open. Look at it later. |
| Q2 | Step 3D: .40 vs .39 changed buffer size and bandwidth together. | **Decided:** run .40 with `conn_only_use_old_forward_open=1`, which clamps it to the ENBT size. |
| Q3 | `test_thread_fairness` in the simulator suite (CI)? | **Decided:** no. CI hardware is overcommitted and stalls, so fairness runs only on the local machine. |

## Step 0: tidy `docs/perf` (15 min)

- Delete the stray untracked trees `docs/perf/~` and `docs/perf/docs/perf` (shell path accidents).
- Move the loose `docs/perf_*.csv` and `docs/perf/perf_logix_*.csv` into
  `docs/perf/MacBook-Air-M1-16GB/<protocol>/`.
- Fix `modbus/perf_logix_results_20260417_123330.csv`: it is Logix data in the Modbus folder.

## Step 1: 200k tags, CIP (about 1 day)

### 1.1 Correctness tool: `src/tests/test_many_tags`

The usual CLI style: `--tag=<base attrs without name>` plus:

- `--tags=N`.
- `--threads=T`, default 1. Thread j owns tags j, j+T, j+2T, and so on, and runs every phase on
  its own tags at the same time as the other threads.
- `--groups=G`, default 1. Tag i gets `connection_group_id=(i mod G)+1`, so the default puts every
  tag on group 1, one connection.
- **Server tags.** libplctag addresses at most 64k elements per array, so `DINT[200000]` must be
  refused (the tool and ab_server both check this). There must also be one array per
  connection, so that no two connections share a server mutex (step 3B).
  - K = the smallest multiple of G that is at least 4, so that 200k/K ≤ 50000.
  - ab_server: `--tag=perfTag1:DINT[200000/K]` up to `--tag=perfTagK:...`.
  - Tag i is `perfTag((i mod K)+1)[i / K]`. G divides K, so every array belongs to exactly one
    connection.
- Phases, each timed and logged:
  1. Create all tags with timeout 0, then poll until all are done (sync create of 200k would
     take minutes).
  2. Write a distinct value to each tag (`i ^ seed`).
  3. Read every tag back and compare. Exit nonzero on any mismatch or error, and print the
     first 10 failures.
  4. Measure idle CPU for 10 s with all tags created and no operations. This catches work that
     grows with tag count instead of with activity.
  5. Destroy all, and time it.
- The step 3.2 script sets `ulimit -n 1024` before any run.

### 1.2 Find N (the connection count)

- Ramp `--groups` one at a time from 1 to 16, then 20, 24, 28, 32, 40, 48, 64, 96 and 128.
  Use 200k tags, async read and ab_server `--delay-ms=1`.
- Plot ops/s and ops/s per connection against G. Linear scaling shows as a flat per-connection
  line, so a peak at 13 shows instead of falling between 8 and 16.
- N = the smallest group count within 5% of peak throughput.
- Raise `perf_benchmark`'s 100-group cap to 128.
- Record why it flattens: client CPU, server CPU, delay-bound, or too many I/O threads (one per
  group, so 128 groups means 128 client threads).
- About 26 points × 5 runs × (10 s + 30 s cool-down), or about 1.5 h.

### 1.3 The two scenarios

- One connection, and N connections, both with `test_many_tags` (correctness) and
  `perf_benchmark --mode=async` (numbers).

### 1.4 Auto-sync read period

- `perf_benchmark --mode=auto --tags=200000 --auto-ms=P --groups=1|N`.
- Lower P until the achieved median period exceeds 1.1 P, or the max period shows a starved tag.
- Report the lowest sustainable P for each scenario, with p99 and max periods.

## Step 2: 50k tags, Modbus (about 1 day)

### 2.1 Address space

- 50k holding registers `hr0`-`hr49999`, one tag each. One unit has 65536, so the unit-ID FIXME
  does not block this.

### 2.2 Server delay (done 2026-10-31)

- `modbus_server --delay-ms=<ms>`, fractions allowed, measured from when the request is read.
  At exit it logs the same "Response delay: requested ... applied mean ..." line as ab_server.
- `coro_net` gained per-task deadlines: `coro_wait_until(task, deadline_us)` takes the task out of
  the poll set until the deadline. The loop polls no longer than the earliest deadline and waits
  out the last part of a millisecond with `select()`, which still wakes on socket activity.
  (Windows: whole milliseconds.) `CORO_EVENT_ALWAYS` was no use for this: an ALWAYS task's socket
  is polled with every event bit set, so the loop never sleeps.
- Measured (async read, 10 tags per connection, sanitized build):

  | Delay | Connections | ops/s | Applied mean | Server CPU |
  |---|---|---|---|---|
  | 1 ms | 1 | 659 | 1266 µs | 6% |
  | 1 ms | 8 | 5,645 | 1138 µs | 22% |
  | 0.3 ms | 1 | 1,607 | 410 µs | 10% |
  | 0.3 ms | 8 | 16,160 | 340 µs | 28% |

  ab_server gives 1259 µs and 390 µs for the same settings.
- `run_perf_compare.py` passes `--delay-ms` to modbus_server too, and the fairness matrix runs
  every server at the same delay.

### 2.3 Repeat 1.1-1.4

Repeat steps 1.1-1.4 with `protocol=modbus-tcp` and 50k tags.

## Step 3: local performance baseline (about 2 days, then about 2 h per release)

### 3.1 Bring the harness up to date (done 2026-10-31)

- `perf_benchmark.c` from HEAD builds unchanged against v2.7.0, v2.7.1 and v2.7.2. Build each
  release's `plctag_dyn` Release, put its `libplctag.h` at `<inc>/libplctag/api/`, and compile
  `perf_benchmark.c`, `utils/compat_utils.c` and `utils/stats.c` against it.
- **HEAD must be built the same way (Release, no sanitizers).** `build/` is Debug + ASan, so a
  comparison against it is meaningless: a trial showed HEAD 15% "worse" for that reason alone.
- `run_perf_compare.py --thread-sweep`: `--threads=` (default 1-8 and 12) and
  `--tags-per-thread=` (default 100; the full 1, 10, 100, 1000 set is about 9 h).
- **Delay.** ab_server `--delay` is now `--delay-ms`, fractions allowed (`--delay-ms=0.3`). No
  alias. The callers in `run_perf_compare.py` and `run_simulator_tests_parallel.py` are updated.
  - Each sleep is measured on a monotonic clock. On SIGINT the server logs the mean delay it
    applied, and `run_perf_compare.py` writes it to the CSV as a `# server delay` line.
  - Measured on the M1: 0.3 ms gives about 390 µs, and 1 ms gives about 1260 µs.
- Servers always come from HEAD and their commit is recorded. Only the library varies.

### 3.2 Machine and conditions

- MacBook Air M1 (MacBookAir10,1, this machine), on mains power.
- **It has no fan.** Sustained load throttles it. Drop any run not at "Nominal" thermal pressure,
  and wait 30 s between runs.
- **`src/tests/scripts/perf_env.sh <out_dir> <command...>` (done 2026-10-31).**
  - It refuses to start on battery, in Low Power Mode, or under a thermal warning (exit 2).
  - It sets `ulimit -n 1024` and runs the command.
  - It writes `<out_dir>/perf_env.txt`: command, commit, `host_info.sh` output, Wi-Fi state, and
    thermal state before and after. It warns if a thermal warning appeared during the run.
  - Thermal state comes from `pmset -g therm` (no sudo), which only reports warning levels.
    Switch to `powermetrics` if runs throttle without a warning.
- **Second host as server (Q1).** It has a slower CPU, so it is closer to a PLC, and its load
  does not touch the client.
  - Record ping RTT, and set the server delay so RTT plus delay is still about 1 ms.
  - The server host gets its own `host_info.sh` record.

### 3.3 P and E cores

macOS has no thread affinity API, so pinning is impossible.

- Default-QoS threads prefer P cores. Above 4 busy threads, E cores join, and so do the server's
  threads. A remote server (Q1) removes the server's share.
- Measure the P/E gap directly: run the same configuration with the client under
  `taskpolicy -b` (E cores only), and compare with a default run at T ≤ 4 (P cores in
  practice). The ratio says how unfair a mixed run can be from core type alone (step 3A).
- Noise: 5 runs per configuration, interleaved across library versions (A B B A, as
  `run_perf_compare.py` does).
  - Report the median and the min-max range (D4).

### 3.4 CPU monitoring

- Already per run: client `cpu_load_pct` and server CPU (in `run_perf_compare.py`).
- Add the machine total from `ps` or `top -l` samples during the run. "Out of CPU" means the total
  is near 800%, or one process is at its thread count × 100%.
- Per-thread client CPU (`ps -M`), so the application threads and the I/O threads show apart.
- Record which one ran out first: client, server or machine.

### 3.5 `docs/performance.md` (new)

Async read, 1 DINT, one connection per thread, 1 ms delay. Each cell is the median reads/s.

| Version | AB 1 thread | AB best | Micro800 1 thread | Micro800 best | Modbus 1 thread | Modbus best |
|---|---|---|---|---|---|---|
| 2.7.0 | | | | | | |
| 2.7.1 | | | | | | |
| 2.7.2 | | | | | | |
| 2.7.3 | | | | | | |

- "Best" cells note threads × connections × tags per thread.
- AB is `plc=ControlLogix`, which packs requests. Micro800 never packs (`ab_common.c:223`). The
  AB/Micro800 ratio is the packing gain, and Micro800 is the unpacked maximum to compare with
  Modbus. Both use the same ab_server and delay.
- Micro800 vs Modbus also compares two server designs: ab_server runs one OS thread per session,
  and modbus_server runs every client on one protothread loop. A gap at one connection is
  protocol and client cost. A gap that grows with connections is the server model, so check
  server CPU (step 3B) before calling it a library difference.
- ponytail: `plc=ControlLogix&allow_packing=0` is the cleaner control, since only packing
  changes. Run it once to check it matches Micro800, then drop it if it does.

Fairness, at the "best" configuration of each protocol (definitions in step 3A):

| Version | AB tag spread | AB thread min/max | Micro800 tag spread | Micro800 thread min/max | Modbus tag spread | Modbus thread min/max |
|---|---|---|---|---|---|---|
| 2.7.0 | | | | | | |
| 2.7.1 | | | | | | |
| 2.7.2 | | | | | | |
| 2.7.3 | | | | | | |

- The step 3A matrix cells that fall below the thread threshold are listed under the table, with
  threads, connections and tags per thread.
- Under both tables: the machine, conditions and server commit, plus a link to the raw CSVs in
  `docs/perf/MacBook-Air-M1-16GB/<version>/`.
- A version's row is measured once, when it is released, on the same machine. Earlier versions
  are re-measured only when the machine or the method changes, and the whole table is then redone.

## Step 3A: fairness (critical, a release gate)

Fairness has two parts, with different rules.

1. **Tag fairness, within one thread or connection.** Every tag gets the same number of finished
   requests, ±1. This is a hard rule.
   - `test_fairness` caught tags starving other tags on one connection in earlier releases. It
     stays as it is.
   - Add a `--groups=C` option that spreads its tags round-robin over C connections, and check
     ±1 within each connection.
   - `perf_benchmark` reports "tag CV" and "tag std dev" instead (2026-11-01): the spread of
     whole-run finished requests across one connection's tags, for the worst connection. With
     several threads, ±1 is not realistic: the tags of one connection belong to threads moving at
     different speeds, and the absolute gap grows with the cycle count (8 × 8: a 1.3% thread gap
     is 12 requests at 910 cycles per tag). CV is relative, so it does not.
   - ±1 stays the rule for `test_fairness` and for one thread.
2. **Thread fairness, across threads.** Threads doing the same operation get about the same
   requests and responses per unit of time.
   - The OS and the core type limit this. A thread on a P core may finish more than one on an E
     core, or may not. Measure first, then set what is reasonable.
   - Step 3.3 measures the P/E gap on its own. A mixed run whose spread is within that gap is
     the OS. Spread beyond it is the library.
   - Reported as "thread min/max": the worst 1 s window's min/max ratio across threads.

- **Measure per window.** A run total hides a thread that starves for a while and then catches
  up.
  - `perf_benchmark` counts, per thread and per tag, the requests started and the responses
    received in each 1 s window.
  - It reports the worst window as well as the run total.
- **Isolate the library.** Each connection gets its own server tag (`perfTag1`, `perfTag2`, ...),
  so the server's per-tag mutex cannot cause unfairness (step 3B). A second run, with every
  thread on one shared array, shows the server's contribution.
- **Modes.** Every cell runs sync and async, read and write, with a 1 ms delay, on CIP
  (`plc=ControlLogix` and `plc=Micro800`) and on Modbus.
- **Matrix over threads, connections and tags per thread.** Fix the total at 10000 tags and vary
  three things:

  | Axis | Values |
  |---|---|
  | Threads T | 1, 2, 4, 8 |
  | Connections C | 1, 2, 3, 4, 8 (16 once, as a probe) |
  | Tags per thread | 10000 / T |

  - Each connection group gets its own client I/O thread, so a cell runs T application threads
    plus C I/O threads. On 8 cores, cells where T + C > 8 measure scheduling, not the library.
    Record both counts for each cell.
  - `perf_benchmark` today ties connections to threads in sync and async modes: each thread's
    tags share one group. Add `--group-by=tag`, which spreads tags round-robin over the C groups
    whatever thread owns them (auto mode already does this).
  - **New case:** T = 1 with C = 2 to 8, that is one thread with its tags spread over several
    connections. Commercial PLC monitoring software works this way, usually with 3 to 8
    connections because PLCs have few connection resources.
  - All 20 cells × 2 modes × 2 ops × 3 PLC types × 5 runs × 10 s, plus cool-downs, comes to
    about 5 h. It runs by hand before a release (D3).
  - C stops at 8 because most PLCs cannot spare more connections. The step 1.2 ramp still goes
    higher, to find where the library itself stops scaling.
- **Gate.**
  - Tag CV: threshold set from measured runs, as for thread min/max. ±1 with one thread.
  - Thread min/max: measure 5 runs on HEAD with the P/E gap from step 3.3, then set the
    threshold from them.
  - Not in CI (Q3): CI hardware is overcommitted and stalls. The gate runs by hand on the local
    machine.

### 3A status (2026-10-31)

Built:
- `perf_benchmark`:
  - Per-tag completion counts in 1 s windows.
  - New columns `window_min_max`, `window_cv` (worst full window across threads, or tags in auto),
    `conn_window_min_max` (worst window across connections) and `tag_spread` (largest max - min of
    whole-run ops among one connection's tags).
  - `--group-by=tag`, repeated `--tag` (group g uses base g-1), up to 10000 tags and 128 groups.
- `test_fairness --groups=C`: tags round-robin over C connections, and fairness is assessed per
  connection as well as overall.
- `run_perf_compare.py --fairness-matrix`: the matrix above for ControlLogix, Micro800 and Modbus,
  each CIP connection on its own `perfTagN` array, with worst-window min/max and tag spread in the
  summary.

Found in a trial run (2 threads, 3 connections, 10000 tags, async read, 3 s, sanitized build):
- Micro800 at 1 ms does about 2,100 ops/s, so one pass over a thread's 5,000 tags takes about
  2.4 s. A 1 s window then lands mid-pass, and thread min/max reads 0.78-0.90 even though nothing
  starved. Modbus shows the same at 0.71-0.95. The window must be several pass times long, or the
  unpacked protocols need fewer tags.
- Creating 10000 CIP tags one at a time takes 16 s per run (one create read per tag at the 1 ms
  delay), which would add hours to the matrix.

Fixed (2026-10-31):
- The window is now `window_s` = 4 passes over a unit's tags, rounded up to whole seconds
  (pass = tags × duration / total ops), and is a new CSV column. `perf_benchmark` warns when a
  run holds fewer than two windows.
- Tags are created with timeout 0 and waited for together: 10000 CIP tags in 5.4 s (was 16 s),
  Modbus in 0.24 s.
- Re-run, 20 s: Micro800 thread min/max 0.95-0.97 (was 0.78-0.90) with a 20 s window, so still
  only one window. Modbus 0.89-0.90 with a 3 s window: a real gap to check on a Release build.

Cost: Micro800 at 1 ms does about 800 ops/s per connection, so a 4-pass window over 10000 tags is
about 50/C s and two windows take about 100/C s. Across the matrix that is about 1 h per side
per round, or about 10 h for Micro800 alone at 5 rounds A and B. ControlLogix and Modbus fit in
10 s runs.

Matrix sizing (decided 2026-10-31): ControlLogix 10000 tags, 10 s runs; Micro800 and Modbus
1000 tags, 15 s runs, every server at 1 ms. Each protocol is 84 configurations. Estimated time
for A and B: at 5 rounds, ControlLogix 2.8 h, Micro800 4.0 h, Modbus 4.0 h (10.8 h); at 3
rounds, 6.5 h. The estimate adds 2 s per run for create and destroy, which is low for 10000 CIP
tags (5.4 s on the sanitized build).

### Found: create is about 160x slower than v2.7.2 for many tags (2026-10-31)

Async create of 10000 ControlLogix tags at 1 ms delay, Release builds:

| Connections | v2.7.2 | HEAD |
|---|---|---|
| 1 | 93 ms | 14,933 ms |
| 2 | 58 ms | 7,668 ms |
| 4 | 59 ms | 3,580 ms |
| 8 | 79 ms | 1,771 ms |

- HEAD time is about 1.5 ms per tag per connection: one round trip per create read.
- Cause: `standard_tag.c:315` makes a create read unpackable
  (`plan->packable = allow_packing && create_read_status != PLCTAG_STATUS_PENDING`), because
  its reply size is unknown and so cannot be budgeted. v2.7.2 packed first reads
  (`eip_cip.c:458`, `req->allow_packing = tag->allow_packing`, whatever `first_read` was).
- **Fixed (dbe7ff1e), verified 2026-11-01:** simulator suite 325/325 on the sanitized build.
  Release, 10000 tags, 1 ms, 3 rounds: 1 connection 98-103 ms (v2.7.2 100-104 ms), 16
  connections 66-70 ms (v2.7.2 59-65 ms), 8 threads × 8 connections 58-60 ms (v2.7.2 56-57 ms).
  Needs a hardware check (user).
- The fix: a create read is packed with a guess of 4 bytes per element, only on families with the fragmented read
  service. A reply that does not fit comes back partial and the read goes on in fragments. After
  an empty fragment the retry goes alone. OMRON (no fragmented read) keeps create reads unpacked.
- Tests: new `src/tests/test_many_tags` (step 1.1: create async, write, read back, destroy, all
  async, with `--threads`, `--groups`, `--max-create-ms`). Three simulator cases: 10000 DINT
  tags created in under 5 s at 1 ms delay; 2000 LINT tags (elements bigger than the guess); 1000
  tags with `--empty_frag=3`.

### Matrix results, v2.7.2 (A) vs HEAD 9ae0c94e (B), 3 rounds (2026-10-31 to 11-01)

Release builds, 1 ms delay, 252 configurations, 8.5 h, no thermal warnings. HEAD is better in
78, the same in 162 and worse in 12. Data: `docs/perf` candidate, currently in the scratchpad
`fairness_matrix_20261031/compare.csv`.

- Sync, all protocols: HEAD is the same or up to 33% faster.
- Async ControlLogix at 4-8 threads × 4-8 connections: HEAD is up to 6x faster (v2.7.2 collapses
  to 110-300k ops/s).
- **Regression, open:** async ControlLogix with 1-2 threads on many connections. 1 thread × 16
  connections: HEAD 206-298k ops/s at 526-597% client CPU, v2.7.2 690-704k at 271-273%. At 1 × 8
  connections it is -21%, at 2 × 8 connections -16%, at 1 × 1 connection -14% (HEAD uses half the
  CPU there). Re-checked after the create fix: unchanged.
  - Cause (2026-11-01, debug log, 625 tags on 1 connection): exchanges alternate 181 tags and 1
    tag. The app restarts its first tag about 84 us after a reply; that wakes the idle IO thread,
    which sends it alone, and the rest wait a round trip. Every tag queued during an exchange also
    woke the IO thread (write, poll, read per op).
  - Fixed (uncommitted): `cip_conn_queue_tag` skips the wake while `rebuild_pending` (under
    `session_mutex`) says the handler will look again before it waits. `settle_due_tags` waits,
    yielding, when the last exchange had 8 or more tags and under a quarter of that is due: until
    the count is quiet 50 us, reaches the last batch, or 300 us pass. New `time_us()`.
  - After, Release, 1 round, 10000 tags: 1 × 16 1,124k ops/s at 1.34 us/op (v2.7.2 643k at
    4.06); 1 × 1 98k (102k); 4 × 4 470k (309k); 8 × 8 909k (134k). Sync 1 × 1 686 ops/s,
    unchanged (v2.7.2 542). Simulator suite 325/325. Needs the full matrix re-run.
- **Fairness, open:** tag spread fails the ±1 rule on HEAD for async ControlLogix on 8
  connections with 2-8 threads (10-22; v2.7.2 2-5). Worst-window thread min/max stays at 0.83 or
  above. Micro800 and Modbus spread is 1-2 everywhere.

## Step 3B: server bottlenecks (about 1 day)

The servers must not limit what the library can do, or the numbers measure the server.

- **ab_server.**
  - One thread per session (`tcp_server.c:133`). The session gets a copy of the PLC context, and
    the tag list is shared.
  - Each tag has its own `data_mutex` (`cip.c:809`, `1013`, `1115`). Tags on one array share one
    mutex, so N threads on `TestBigArray[i]` contend.
  - The delay sleep runs outside the lock (`main.c:1342`).
  - Fix by test design: one array per connection (`perfTag1`..`perfTagK`, step 1.1).
- **modbus_server.** A single-threaded protothread loop, so every client shares one core, and
  that is the expected ceiling. Check `register_storage` for locking too.
- **Method.**
  - Profile each server under the 8-thread, connection-per-thread load with `sample <pid> 10` or
    Instruments (macOS), and with `perf` on Linux.
  - Measure mutex wait time once by wrapping the server's `mutex_lock` with a timer.
  - A server is the bottleneck if its CPU is at its thread limit (100% for modbus_server) or its
    lock wait is above 10% of run time.
- **Outcome.** Either fix the server (per-unit storage, more worker threads) or record its
  ceiling beside every result that hits it.

### 3B results (2026-10-31)

Release builds without sanitizers, mains power, async read of one array element, 8 threads,
8 connections, 800 tags, no delay. `sample <pid> 4` on the server partway through each run.

| Server | ops/s | Server CPU | Client CPU | Client µs/op |
|---|---|---|---|---|
| ab_server, ControlLogix (packed) | 657,530 | 60% | 410% | 6.2 |
| ab_server, Micro800 (unpacked) | 74,907 | 109% | 399% | 53.2 |
| modbus_server | 58,452 | 78% (1 thread) | 459% | 78.6 |

- **Neither server is the bottleneck yet.**
  - ab_server's session threads sit in `select` for about 95% of samples.
  - modbus_server's single thread is 22% idle in `poll`.
  - With the 1 ms delay, server CPU is 2-14% (step 3.1 check).
- **ab_server mutex.** The waits are in `read_tag` on the shared array's `data_mutex`: about 1% of
  all thread samples, but about 17% of busy samples. Small now, and it grows as the client gets
  faster, so one array per connection (step 1.1) stays.
- **modbus_server will be the first ceiling.** It runs on one core and was already at 78%.
  `log_is_enabled()` (`src/poc/utils/log.c:130`) loops over all 64 modules on every log call, and
  takes about 8% of the server's busy samples. ponytail: not fixed. Fix (keep the highest enabled
  level, return early) when modbus_server reaches 100%.
- **The client limits zero-delay runs, and the harness inflates its CPU.** `perf_benchmark
  --poll-ms=0` yields in a loop. With `--poll-ms=1`:
  - Micro800: 74,907 → 83,738 ops/s, client CPU 399% → 219%.
  - Modbus: 58,452 → 76,724 ops/s, client CPU 459% → 322%.
  - So `cpu_us_per_op` with `--poll-ms=0` measures the spin, not the library. Decide the harness
    default before the baseline (step 3.5).
- **One connection, no delay:** Micro800 36,887 ops/s and Modbus 30,656 ops/s, about one client
  core each. Eight connections give only 2.3-2.5× that, with the machine about half busy.
  Something in the client limits scaling. Profile the client next (step 3A needs it anyway).
- Not done: the mutex wait timer from the method above. `sample` already attributes the waits.
- **`sample` slows the run.** The table's ops/s were taken while sampling at 1 ms and are low:
  Micro800 at 8 connections gave 75k under `sample` and 121k without it. Use them only to see
  where time goes, never as throughput.

### Client scaling (2026-10-31)

`perf_benchmark` now defaults to `--poll-ms=1`, and so does `run_perf_compare.py`. Micro800, no
delay, one thread per connection, 100 tags per thread, 5 s, no sampling:

| Connections | ops/s | Per connection | Client CPU | Server CPU | Client µs/op |
|---|---|---|---|---|---|
| 1 | 34,359 | 34,359 | 36% | 28% | 10.4 |
| 2 | 52,604 | 26,302 | 62% | 47% | 11.7 |
| 4 | 95,567 | 23,892 | 144% | 103% | 15.1 |
| 6 | 111,541 | 18,590 | 239% | 156% | 21.4 |
| 8 | 120,599 | 15,075 | 292% | 177% | 24.2 |

- The client samples are almost all waits (sleep, `poll`, condition variables). No library
  function stands out; the busiest, `hashtable_get` and `lookup_tag`, are well under 1%.
- Scaling is near linear up to 4 connections and bends after about 6. That is where client plus
  server CPU passes the 4 P cores (2.5 cores at 4 connections, 4.0 at 6, 4.7 at 8).
- Client cost per op rises from 10 to 24 µs. That fits threads moving to E cores and the
  machine filling up. It does not fit a library lock: mutex waits are under 0.2% of samples.
- **Conclusion:** on one machine, the bend at 6 connections is the machine, not the library.
  A separate server host (Q1) would show whether the library goes further. The `taskpolicy -b`
  run (step 3.3) will give the E-core cost per op directly.


## Step 3C: user performance guide (after 3A and 1.2)

A user-facing recommendation, published on the wiki as a "Performance" page and linked from
`docs/performance.md`.

- **The question it answers:** "I have X tags and a PLC that can spare Y connections. How many
  threads and connections, which mode, and what auto-sync period?"
- **Built from the step 3A matrix:** for each tag count, the lowest C that reaches 90% of the
  best throughput, and whether more threads help at that C.
- **Sync vs async with few tags per thread (side test).** Sync has been seen to beat async at
  small tag counts per thread.
  - T = 1, 2, 4 and 8, with 1, 2, 4 and 8 tags per thread, sync and async, at 1 ms and 0 delay.
  - Report ops/s, `cpu_us_per_op` and RSS, since every sync thread costs a stack.
  - The guide states where the crossover is, if there is one.
- **Content:**
  - Start with one connection, then add connections up to the PLC's spare budget (3-8) before
    adding threads.
  - Async or auto-sync for many tags. Sync only below the measured crossover.
  - The lowest auto-sync period that is sustainable for a given tag count and connection count.
  - The cost of each extra connection to the PLC.
- **Evidence for each claim:** the measured numbers, the machine, and "simulator, 1 ms delay".
  Hardware confirmation on the ControlLogix is a separate line.
- **Kept current:** redone when step 4 shows a change in the shape of the curves, not every
  release.

## Step 3D: several network modules, one CPU (hardware, about half a day)

The ControlLogix chassis has three Ethernet modules. All three reach the L81 CPU in slot 4:

| Gateway | Module | Link | Connection buffer |
|---|---|---|---|
| 10.206.1.37 | ENBT | 100 Mbps | Likely 504 bytes (no Extended Forward Open) |
| 10.206.1.39 | ENBT | 100 Mbps | Likely 504 bytes |
| 10.206.1.40 | L81 front port | 1 Gbps, direct to the CPU | 4002 bytes |

- **Question:** does spreading connections over two network modules raise throughput to one CPU,
  and by how much?
- **Fixed for every run:** `path=1,4` (the L81), the same tags, the same CPU load.
- **Record the negotiated size** per connection from the debug log ("Session payload
  calculation", `conn.c:217`), so "likely" in the table becomes measured.
- **Runs** (async read, 1000 and 10000 tags, C = 2, 4, 8 in total):
  1. .37 alone.
  2. .39 alone.
  3. .37 and .39 together, C/2 connections each.
  4. .40 alone with `conn_only_use_old_forward_open=1`, so the buffer matches the ENBTs.
  5. .40 alone with the default (Extended Forward Open, 4002 bytes), as the ceiling.
- **Reading:**
  - Runs 1-3 have the same buffer size and bandwidth per module, so they are a clean test. If run
    3 is about twice runs 1 and 2, the modules were the limit. If it is close to them, the CPU's
    communication time slice is. Record the controller's system overhead time slice setting.
  - Run 4 against run 2 changes only the module and bandwidth (Q2).
  - Run 5 against run 4 changes only the buffer size. A smaller buffer also means fewer
    requests packed per frame.
- **Harness change:** `perf_benchmark` accepts `--tag` more than once and spreads tags
  round-robin across the base strings.
- **Conditions:** no other hardware tests running, because the PLCs are shared. Use 3 rounds, not
  5, because of the time on shared hardware.
- **Feeds step 3C:** the guide gains "a second network module to the same CPU gives X" as
  measured, not assumed.

## Step 4: regression gate per release

- Before each release, run `run_perf_compare.py` with A = the previous release tag and B = the
  candidate, using the 3.5 configurations plus the 200k CIP and 50k Modbus runs.
- The release fails on any configuration where B is worse by the existing rule (more than 5%,
  ranges not overlapping).
- Metrics beyond ops/s:
  - Fairness: the step 3A gate. A drop fails the release even when throughput rises.
  - `cpu_us_per_op`: the most stable, and the one that matters when server-bound.
  - p99 and max latency.
  - Create and destroy time for 200k tags.
  - RSS per tag.
  - Idle CPU with 200k tags.
- Optional, when Mac noise blocks a verdict: a Linux box with `taskset` pinning, the performance
  governor and SMT off. Use it for trends only; the table stays on the Mac.
- Optional: one hardware run per release against the ControlLogix (`--logix-attrs`), 1-8
  threads, because simulator numbers are not production numbers.

## Step 5: memory and allocations (later, about half a day)

- RSS: `rss_growth_kb` exists. Add RSS per tag after the 200k create and after destroy, so a
  leak shows as a remainder.
- Allocation counts: a small interposer library that counts
  `malloc`/`calloc`/`realloc`/`free` and bytes, and prints totals at exit
  (`DYLD_INSERT_LIBRARIES` on macOS, `LD_PRELOAD` on Linux).
  - It needs no library change, so it measures 2.7.0 the same way as HEAD.
  - Counting inside `mem_alloc()` would miss older versions and put test code in the product.
- Report allocations per operation for steady-state reads (target: 0) and per tag created.
- For deeper analysis by hand: `heaptrack` or `valgrind --tool=massif` on Linux, `leaks` and
  Instruments on macOS.

## Order

1. Step 0, 3.1 and the 3.2 script, so the harness works on every version.
2. Steps 3B and 3A, so the server limits are known and the fairness gate exists.
3. Step 1 (CIP 200k), so N is known before the baseline.
4. Steps 3.3-3.5 (the baseline tables).
5. Step 3D (hardware, needs you at the PLCs), then step 3C (user guide).
6. Step 2 (Modbus 50k), then steps 4 and 5.

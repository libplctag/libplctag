# libplctag performance

How libplctag performance and fairness are measured, what each release is compared against, and
the results so far.

## What is compared

- **Library versions, nothing else.** Each comparison is release N-1 (A) against the candidate
  (B). The servers, the machine, the benchmark source and the load are the same for both sides.
  - The current comparison is v2.7.2 (A) against the 2.7.3 branch `2_7_3_performance` (B).
  - Earlier releases (2.7.0, 2.7.1) build with the same benchmark and can be added as rows.
- **Three protocol paths**, all on local simulators:

  | Name | Server | What it exercises |
  |---|---|---|
  | ControlLogix | `ab_server --plc=ControlLogix` | CIP with request packing (many tags per packet). |
  | Micro800 | `ab_server --plc=Micro800` | CIP without packing (one tag per packet). |
  | Modbus | `modbus_server` | Modbus TCP, one register per request. |

  - ControlLogix / Micro800 is the packing gain.
  - Micro800 vs Modbus is the unpacked maximum of two protocols. It also compares two server
    designs (ab_server: one thread per session; modbus_server: one protothread loop), so a gap
    that grows with connections must be checked against server CPU before it is blamed on the
    library.
- **Two properties:**
  1. Throughput and cost: operations per second, client CPU per operation, latency.
  2. Fairness: every tag and every thread gets its share of the connection (see below).
- HEAD must beat v2.7.2 on all three client measures: higher `ops_per_sec`, lower
  `cpu_us_per_op` and lower `peak_rss_kb`. A cell that loses on any one of them is a regression,
  even when it wins on the other two.

## Test setup

### Machine

- MacBook Air M1 (MacBookAir10,1), 4 performance + 4 efficiency cores, 16 GB, macOS.
- Client and server run on the same machine. Results are relative (A vs B on the same day), not
  absolute capacity figures.
- The machine has no fan and throttles under sustained load. Conditions are enforced by
  `src/tests/scripts/perf_env.sh <out_dir> <command...>`:
  - It refuses to start on battery, in Low Power Mode, or under a thermal warning.
  - It sets `ulimit -n 1024`.
  - It writes `<out_dir>/perf_env.txt`: command, commit, host info, Wi-Fi state, and thermal state
    before and after the run.
- Runs are started under `caffeinate -i` so the machine does not sleep.
- Linux: every run needs deep CPU idle states blocked. `perf_env.sh` refuses to start otherwise,
  and records `deep_idle_before` and `deep_idle_after` in `perf_env.txt`. A wake from deep idle
  costs 50-200 µs, which favors a client that spins over one that blocks (see "Deep idle states
  blocked" below). To block them for 24 h, which needs root:

  ```sh
  sudo sh -c 'exec 3>/dev/cpu_dma_latency; printf "\0\0\0\0" >&3; exec sleep 86400' &
  ```

  The block ends when that `sleep` exits or is killed. The check counts entries into idle states
  with an exit latency over 10 µs for 1 s; there must be none. macOS gives no such control.
- macOS has no thread affinity API. Above 4 busy threads, efficiency cores join and per-thread
  speed differs by core type. Thread fairness thresholds must allow for that.

### Builds

- Every library version is a **Release** build of `plctag_dyn` with no sanitizers. A Debug or ASan
  build is never compared: in a trial that alone made HEAD look 15% slower.
- `src/tests/perf_benchmark/perf_benchmark.c` uses only the public API (`REQUIRED_VERSION 2.6.0`),
  so the same source is compiled against each version's `libplctag.h` and library:

  ```sh
  cc -O2 -Wall -I src/tests/utils -I <version>/inc \
      src/tests/perf_benchmark/perf_benchmark.c src/tests/utils/compat_utils.c src/tests/utils/stats.c \
      -L<version>/lib -lplctag -Wl,-rpath,<version>/lib -lm -o <version>/perf_benchmark
  ```

- Servers (`ab_server`, `modbus_server`) always come from HEAD, and their commit is recorded.

### Server delay

- Every server answers after a 1 ms delay (`--delay-ms=1`), so the network round trip looks like a
  PLC's rather than a loopback's. Measured on the M1, 1 ms requested gives about 1.26 ms applied.
- The server measures each sleep and logs the mean on exit. `run_perf_compare.py` writes it into
  the CSV as a `# server delay` line.
- Each connection reads its own server array (`perfTag1`, `perfTag2`, ...), so ab_server's
  per-tag mutex cannot cause contention or unfairness between connections.
- Neither server is the bottleneck at these loads: ab_server's session threads are idle in
  `select` about 95% of the time, and server CPU with the 1 ms delay is 2-14%. modbus_server runs
  on one core and will be the first server ceiling.

## Methodology

### One run

`perf_benchmark` runs one configuration and prints one CSV line.

1. Create all tags with timeout 0, then wait for them together. The time is `create_ms`.
2. Prime: one read of each tag, so every tag has its size. The time is `prime_ms`.
3. Run the load for `--duration` seconds (10 s by default).
   - `--mode=sync`: each thread calls `plc_tag_read` with a timeout, one tag after another.
   - `--mode=async`: each thread starts an operation on each of its tags and polls
     `plc_tag_status` every `--poll-ms` (1 ms by default) for the ones that finished.
   - `--op=read` or `--op=write`, one DINT per tag.
4. Destroy the tags and report.

Load shape:

- `--threads=T` application threads, each owning a separate slice of `--tags`.
- `--groups=C` connections (`connection_group_id`). Each connection has its own client I/O thread.
- `--group-by=tag` spreads tags round-robin over the connections, whatever thread owns them, so
  one thread can drive several connections. Monitoring software (HMI, SCADA) works this way,
  usually with 3-8 connections because PLCs have few connection resources.

### Comparing two versions

`src/tests/scripts/run_perf_compare.py --a=<A perf_benchmark> --b=<B perf_benchmark>`:

- Runs every configuration in **A B B A** order for `--rounds` rounds, so drift (thermal, other
  load) affects both sides equally.
- Starts a fresh server for each run and records server CPU.
- Per configuration, takes the **median** of each side and its min-max range.
- Verdict:
  - **same** if the medians differ by 5% or less, or the A and B ranges overlap.
  - Otherwise **better** or **WORSE**.
  - When the server uses more than 90% of a core, the run measures the server, so the verdict
    uses client CPU per operation instead of throughput.

### The matrix

`run_perf_compare.py --fairness-matrix --delay-ms=1` runs 252 configurations:

| Axis | Values |
|---|---|
| Protocol | ControlLogix (10000 tags, 10 s), Micro800 (1000 tags, 15 s), Modbus (1000 tags, 15 s) |
| Mode | sync, async |
| Operation | read, write |
| Threads T × connections C | 1, 2, 4, 8 threads × 1, 2, 3, 4, 8 connections, and 1 × 16 (21 cells) |

- The total tag count is fixed per protocol, and tags per thread is total / T.
- A cell runs T application threads plus C I/O threads. On 8 cores, cells where T + C > 8 also
  measure the OS scheduler.
- Micro800 and Modbus use 1000 tags: unpacked at 1 ms they do about 800 ops/s per connection, and
  10000 tags would need runs of minutes to hold enough fairness windows.
- At 3 rounds the full matrix takes about 8.5 h. `--only="<text>"` runs the configurations whose
  key contains the text, for example `--only="logix d1ms async"`.

Example (one side of the matrix, exact paths elided):

```sh
caffeinate -i src/tests/scripts/perf_env.sh out/ \
    python3 src/tests/scripts/run_perf_compare.py \
        --a=v2.7.2/perf_benchmark --b=head/perf_benchmark --server-dir=head/build/bin_dist \
        --fairness-matrix --delay-ms=1 --rounds=3 --out=out/compare.csv
```

### The release matrix

`run_perf_compare.py --release-matrix=logix` runs the configurations tracked for each release.
The server delay defaults to 0.3 ms, a fast PLC on a fast network.

| Axis | Values |
|---|---|
| Connections | 1, 2, 3, 4, 5 |
| Threads | 1, 2, 4, ... up to 4 × the CPU count (1-16 on 4 CPUs, 1-32 on 8) |
| Tags | 1, 50, 100, 150, 200, 250 |
| Mode | sync, async (read) |

- Configurations with more threads or connections than tags are skipped: 252 on 4 CPUs.
- One ControlLogix packet holds about 180 of the harness's tags (`perfTagN[0]`). Tags spread
  round-robin over the connections, so only 200 and 250 tags on one connection need a second
  packet.
- Tracked: ops per tag per second, client CPU % and client peak memory. Server CPU % and memory
  are tracked only to catch a server regression.
- At 3 rounds it takes about 5 h on 4 CPUs.

`run_perf_compare.py --auto-matrix=logix` measures auto-sync reads: tags 1000, 2500, 5000 and
10000 × `auto_sync_read_ms` 1, 2, 5, 10, 20, 50 and 100 × 1 and 5 connections, 56
configurations, about 1.2 h at 3 rounds. Compare the achieved period (`p50_us`, the interval
between completions on a tag) with the requested one, and client CPU against both. The point
where the achieved period falls behind the requested one, or CPU climbs steeply, sets the
practical lower limit on the period.

`--summary=<dir>/summary.csv` writes the medians of every configuration and side (A, B), and
one matrix CSV per client metric: `summary_ops_per_tag_s.csv`, `summary_cpu_load_pct.csv` and
`summary_peak_rss_kb.csv`. Each row is a configuration without its connection count. The columns
are A at each connection count, B at each, then B ÷ A. With colour scales in a spreadsheet, each
is a heatmap of two axes at once. These files are the synopsis kept in `docs/perf/<host>/`.

## Metrics

### Throughput and cost

| Column | Meaning |
|---|---|
| `ops_per_sec` | Finished operations per second, all threads. |
| `cpu_load_pct` | Client process CPU, 100 = one core. |
| `cpu_us_per_op` | Client CPU time per operation. This is the library's cost. |
| `p50_us` ... `max_us` | Operation latency, start to finish. |
| `create_ms`, `prime_ms` | Time to create all tags, and to read each once. |
| `rss_growth_kb` | Client memory growth during the run. |
| `peak_rss_kb` | Client peak memory at the end of the run. |
| `mutex_waits_per_op` | `pthread_mutex_lock()` calls per operation that had to block, measured window only. -1 without the shim. |
| `mutex_wait_us_per_op` | Total time blocked in those calls, all threads, per operation. -1 without the shim. |
| `server_cpu_pct` | Server CPU, from the harness. |
| `server_rss_kb` | Server memory after the run, from the harness. One server serves a whole protocol and delay, so this grows across runs. |

- With `--poll-ms=0` the async loop spins, and `cpu_us_per_op` measures the spin, not the library.
  All comparisons use `--poll-ms=1`.
- The summary prints client CPU (`cli%`), client and server memory (MB), and mutex wait (µs/op) for A and B
  next to the verdict. They do not change the verdict.
- Mutex wait needs `--mutex-shim=<build>/bin_dist/libmutex_wait_shim.so`. It works on Linux only, with
  perf_benchmark linked against `plctag_dyn`. The shim works with any library version, so v2.7.2 and HEAD
  are measured the same way. A lock that `trylock` gets at once is not counted. Spin locks, atomics and the
  relock inside `pthread_cond_wait()` are not seen.
- Linux memory before 2026-11-02: `rss_growth_kb` came from `ru_maxrss`, which keeps the RSS of the
  process that started the harness (about 20 MB for the Python runner). Growth below that showed as 0.
  The harness now reads `VmHWM`, which counts only its own memory.

### Fairness

Fairness has two parts.

1. **Tag fairness:** every tag on a connection gets the same number of finished operations.
   - `tag_cv` and `tag_std_dev`: the coefficient of variation (%) and standard deviation of
     whole-run finished operations across one connection's tags. The connection with the largest
     CV is reported.
   - With one thread, the rule is ±1 operation per tag, and `src/tests/test_fairness` enforces it
     in the simulator suite.
   - With several threads, ±1 is not realistic. One connection's tags belong to threads moving at
     different speeds, and the absolute gap grows with the number of cycles. Example: at 8 threads
     × 8 connections, a 1.3% thread speed gap is 12 operations at 910 operations per tag. CV is
     relative, so it does not grow with throughput.
2. **Thread fairness:** threads doing the same work get about the same operations per unit of
   time.
   - Counted per 1 s bucket, per thread and per tag, so a thread that starves for a while and then
     catches up still shows.
   - `window_s`: the window length, 4 passes over a thread's tags rounded up to whole seconds, so a
     window does not end in the middle of a pass. The benchmark warns when a run holds fewer than
     two windows.
   - `window_min_max`: the worst window's min/max ratio of operations across threads (1.0 =
     equal). `window_cv`: its CV.
   - `conn_window_min_max`: the same across connections.

Thresholds for `tag_cv` and `window_min_max` are set from measured runs on HEAD, allowing for the
performance/efficiency core gap. Proposed values are under Results.

## Results

### v2.7.2 vs HEAD 9ae0c94e, full matrix (2026-10-31)

3 rounds, 252 configurations, 8.5 h, no thermal warnings. HEAD better in 78, same in 162, worse
in 12.

- Sync, all protocols: HEAD the same or up to 33% faster.
- Async ControlLogix at 4-8 threads × 4-8 connections: HEAD up to 6× faster. v2.7.2 falls to
  110-300k ops/s there.
- Async ControlLogix with 1-2 threads on many connections: HEAD slower. 1 thread × 16
  connections: HEAD 206-298k ops/s at 526-597% client CPU, v2.7.2 690-704k at 271-273%. Fixed
  by 5101b388: 1.34 µs/op against 4.06 (see "Fixes since").
- Tag create, 10000 ControlLogix tags: HEAD about 160× slower (14.9 s against 93 ms on 1
  connection), because create reads were not packed.

### Fixes since

- **Create reads are packed again** (dbe7ff1e). 10000 tags, 3 rounds:

  | Connections | v2.7.2 | HEAD |
  |---|---|---|
  | 1 | 100-104 ms | 98-103 ms |
  | 16 | 59-65 ms | 66-70 ms |
  | 8 threads × 8 | 56-57 ms | 58-60 ms |

- **Packet packing no longer alternates** (5101b388). The I/O thread sent one tag, then all the
  rest, in turn: the first tag restarted after a reply woke it before the others were queued.
  It now skips redundant wakes and waits up to 300 µs for a batch to settle (the settle wait was
  removed later, see "Settle wait removed"). Async ControlLogix
  read, 10000 tags, 1 round:

  | Threads × connections | v2.7.2 | HEAD |
  |---|---|---|
  | 1 × 1 | 102k ops/s | 98k ops/s |
  | 1 × 16 | 643k ops/s, 4.06 µs/op | 1,124k ops/s, 1.34 µs/op |
  | 4 × 4 | 309k ops/s | 470k ops/s |
  | 8 × 8 | 134k ops/s | 909k ops/s |

  Sync 1 × 1 read: 686 ops/s (v2.7.2 542).

- **Fairness at 8 × 8:** HEAD's per-tag range is 12 operations against 4 on v2.7.2. That is the
  thread speed gap at 7× the cycle count: relative gap 1.3% on HEAD, 2.9% on v2.7.2. Connection
  windows are 0.9986 (HEAD) and 0.9988 (v2.7.2).

### v2.7.2 vs HEAD 5b148252, async ControlLogix (2026-11-01)

The async ControlLogix part of the matrix (`--only="logix d1ms async"`), 42 configurations, 3
rounds, 10000 tags, no thermal warnings, server delay applied mean 1.34 ms.

Read, median ops/s:

| Threads × connections | v2.7.2 | HEAD | Change |
|---|---|---|---|
| 1 × 1 | 109k | 90k | −18% |
| 1 × 2 | 217k | 200k | −8% |
| 1-8 × 3-4 | 285-411k | 281-426k | same |
| 1 × 8 | 619k | 781k | +26% |
| 1 × 16 | 689k | 1,134k | +65% |
| 4 × 8 | 317k | 894k | +182% |
| 8 × 8 | 103k | 973k | +847% |

- **Regression, open:** HEAD is 6-18% slower on 1-2 connections with 1-4 threads (9
  configurations WORSE, all there). On 1 connection HEAD uses half the client CPU per operation
  (6.9 against 13.5 µs) but its cycle is longer: median latency 56 ms against 46 ms. Either HEAD
  packs fewer tags per packet, or the settle wait (up to 300 µs per exchange, about 40 exchanges
  per cycle) adds the 10 ms. The earlier matrix showed −14% at 1 × 1, so the settle wait is a
  suspect, not proven. Ruled out 2026-11-02 (see "Settle wait off" below).
- **Tag CV:** HEAD at most 0.59% anywhere (0.36% at 8 × 8). v2.7.2 up to 1.97%, above 1% at
  4 × 8, 8 × 4 and 8 × 8, where its throughput collapses.
- **Thread window min/max:** HEAD worst 0.857 (8 threads × 1 connection, write), v2.7.2 0.798.
  On one shared connection both are about 0.86-0.93.

Proposed thresholds, from these runs:

- `tag_cv` ≤ 1% (HEAD's worst is 0.59%).
- `window_min_max` ≥ 0.80 (HEAD's worst is 0.857). To be confirmed against the
  performance/efficiency core gap.

### v2.7.2 vs HEAD 5b148252, tags × threads × connections, Linux (2026-11-01)

A second host, for trends only: Dell Precision Tower 3420, i5-6500, 4 cores, no SMT, 32 GB,
Debian, `powersave` governor, no pinning. Client and server share the 4 cores.

- `run_perf_compare.py --scale-matrix=logix|modbus --delay-ms=1 --rounds=3 --duration=5`: 10,
  100 and 1000 tags × 1, 2, 4 and 8 threads × 1, 2, 4 and 8 connections, sync and async, read
  and write, tags round-robin over the connections. 192 configurations per protocol, about 1.8 h
  each.
- Both sides link `libplctag_static.a`, not `plctag_dyn` (built Release, no sanitizers).
- 5 s runs hold fewer than two fairness windows in the slow cells, so only throughput is used.
- Server delay applied mean 1.11 ms (ControlLogix), 1.14 ms (Modbus). No cell was server bound.

| Protocol | Better | Same | WORSE |
|---|---|---|---|
| ControlLogix | 41 | 124 | 27 |
| Modbus | 8 | 177 | 7 |

- **Modbus:** the same almost everywhere. The WORSE cells are 5-7%, at 1000 tags and 4-8
  threads.
- **ControlLogix, 10 and 100 tags:** the same or better. Async at 100 tags is up to 27% faster
  (2 threads × 1-2 connections, 4 threads × 4-8 connections).
- **ControlLogix, 1000 tags, 1-2 threads:** sync is 14-20% faster on every connection count
  except 2 × 1. v2.7.2 spends 1253 µs of client CPU per op at 1 × 1 sync, HEAD 247 µs.
- **Regression, ControlLogix, 1000 tags, 4-8 threads:** 22 of the 27 WORSE cells.

  | Cell | v2.7.2 | HEAD | Change |
  |---|---|---|---|
  | async read 1 × 1 | 91k ops/s | 78k | −14% |
  | async read 4 × 4 | 318k | 222k | −30% |
  | async read 8 × 8 | 353k | 317k | −10% |
  | sync read 8 × 8 | 6,216 | 5,148 | −17% |

  - HEAD uses less client CPU in each of these (4 × 4 async: 196% against 210%; 1 × 1 async:
    49% against 116%), and its latency is longer: sync 8 × 8 p50 1568 µs against 1248 µs.
  - The extra 320 µs per sync op is close to the settle wait's 300 µs limit, but the settle wait
    is not the cause (see "Settle wait off" below).

### v2.7.2 vs HEAD 5b148252, full matrix, Linux (2026-11-01 to 11-02)

The same Linux host. `--fairness-matrix --delay-ms=1 --rounds=3`, both sides on `plctag_dyn`
(Release, no sanitizers). 252 configurations, 9.1 h, no failed runs. Server delay applied mean
1.12 ms (ControlLogix), 1.10 ms (Micro800), 1.14 ms (Modbus).

| Protocol | Better | Same | WORSE |
|---|---|---|---|
| ControlLogix | 60 | 17 | 7 |
| Micro800 | 20 | 19 | 45 |
| Modbus | 6 | 71 | 7 |

- **ControlLogix sync:** HEAD 14-69% faster everywhere (1 thread: 620 against 415 ops/s).
- **ControlLogix async, 8 connections or more:** v2.7.2 collapses on 4 cores, HEAD does not.

  | Threads × connections | v2.7.2 | HEAD |
  |---|---|---|
  | 1 × 8 | 338k | 750k |
  | 1 × 16 | 325k | 816k |
  | 4 × 8 | 83k | 845k |
  | 8 × 8 | 24k | 851k |

- **ControlLogix async, 1-3 connections, 1 thread:** HEAD 8-19% slower (1 × 1: 94k against
  114k). The same regression as on the M1. HEAD uses less client CPU (83% against 201%) and its
  p50 latency is longer (56 ms against 36 ms).
- **Regression, Micro800, open:** HEAD 5-16% slower in 45 cells.
  - Async on every cell with 3 connections or more, and sync with 4-8 threads (8 × 8 sync:
    5,194 against 6,206 ops/s, p50 1504 µs against 1248 µs).
  - Single-thread sync is 12-20% faster.
  - Not the settle wait: Micro800 does not pack, so the last exchange is one tag and
    `settle_due_tags` returns at once.
  - ab_server CPU per op is about 3.5× higher with HEAD (1 × 8 async: 53% at 5.9k ops/s against
    18% at 7.0k). `strace -c` on ab_server shows the same syscalls per op for both sides (about one
    `recvfrom`, one `sendto` and two `pselect6`), so HEAD does not send more requests. Under strace
    the order reverses (HEAD 25.0k ops against 21.1k), so it is timing sensitive. On 4 cores, a
    busier server takes CPU from the client.
- **Modbus:** the same almost everywhere. WORSE cells are 5-8%.
- **Fairness:**
  - v2.7.2 starves threads: `window_min_max` is 0 (a thread with no finished operation in a
    whole window) in async ControlLogix at 2-8 threads × 8 connections. HEAD's worst is 0.857 on
    ControlLogix and 0.80 on Micro800 and Modbus, the same as v2.7.2 there.
  - `tag_cv` is meaningless for 1-thread sync ControlLogix: 10 s at 620 ops/s does not finish one
    pass over 10000 tags, so each tag has 0 or 1 operations (HEAD 79%, v2.7.2 120%). Elsewhere
    HEAD's worst is 5.3% (Micro800, Modbus), the same as v2.7.2.

### Settle wait off, Linux (2026-11-02)

HEAD 5b148252 with `SETTLE_MIN_BATCH` set to 2^30, so `settle_due_tags` returns at once (B),
against HEAD (A) and v2.7.2 (A). ControlLogix, 1 ms delay, 3 rounds, `plctag_dyn`.

| Cell | v2.7.2 | HEAD | Settle off | Settle off vs HEAD |
|---|---|---|---|---|
| async read, 10000 tags, 1 × 1 | 115k | 94k | 97k | +2.9%, same |
| async read, 10000 tags, 1 × 2 | 233k | 194k | 195-201k | +3.5%, same |
| async read, 10000 tags, 8 × 8 | 53k | 872k | 861-871k | −0.1%, same |
| sync read, 1000 tags, 8 × 8 | 6,204 | 5,279 | 5,145-5,181 | −2.5%, same |
| async read, 1000 tags, 4 × 4 | 312k | 235k | 230-236k | −2.3%, same |

- The settle wait is not the cause of any regression: without it HEAD is still 16-24% slower
  than v2.7.2 in the four regressed cells.
- It is not the cause of the 8 × 8 gain either. That gain comes from elsewhere (the skipped
  wakes in 5101b388, or earlier).
- It changes nothing measurable here, and it spends CPU yielding. Removed 2026-11-03 (see
  "Settle wait removed").

### Packing count, Linux (2026-11-02)

An `LD_PRELOAD` shim counts the client's `send()` calls: on a TCP socket each is one exchange,
and on the wake socket each is one I/O thread wake. ControlLogix async read, 10000 tags, 1 × 1,
1 ms delay, `plctag_dyn`. Exchanges in the measured window are the difference between a 6 s and
a 2 s run (1 run each), so create and prime drop out.

| Version | Tags per exchange | Exchanges/s | ms per exchange | Server delay applied | Rest of the exchange |
|---|---|---|---|---|---|
| v2.7.2 | 176 | 592 | 1.69 | 1.09 ms | 0.60 ms |
| HEAD | 178 | 528 | 1.89 | 1.13 ms | 0.76 ms |

- Packing is the same. The 1 × 1 loss is about 160 µs more per exchange outside the server's
  sleep: reading the reply, finishing about 178 tags, building and sending the next request.
- One exchange at a time per connection on both sides (`process_requests` sends, then waits for
  the reply), so that time adds directly to the cycle.
- Prime is one sync read per tag, one tag per exchange: 10000 exchanges and 15.7 s (HEAD) or
  24 s (v2.7.2) per ControlLogix run, before the measured window. It is most of each run's wall
  time and is not in the matrix time estimates.
- Sync, 1000 tags, 8 × 8: one tag per exchange on both sides, as expected. HEAD sends 0.62-0.64
  wakes per operation, v2.7.2 none. A wake is a syscall and a context switch, and every
  unpacked operation pays it. This is a lead for the sync and Micro800 regressions on 4 cores.
  Not proven.
- A first count that divided all exchanges by the window length was wrong: it counted the
  prime's 10000 single-tag exchanges.

### Exchange split, Linux (2026-11-02)

An `LD_PRELOAD` shim in both perf_benchmark and ab_server logs each `send()` and the first read
after it, with `CLOCK_MONOTONIC` times. Joined by TCP port, each exchange splits into four
parts: client send to server read (`to_srv`), server handling with its 1 ms sleep (`srv`),
server send to client read (`to_cli`), and client read to the next send (`cli`). `to_srv` and
`to_cli` are loopback plus the time for the receiving thread to wake. Async read, 1 thread,
`plctag_dyn`, a dynamically linked copy of the HEAD ab_server (the CMake one is static, and
`LD_PRELOAD` does not reach it). Medians of 2 runs, µs.

| Cell | Version | `to_srv` | `srv` | `to_cli` | `cli` |
|---|---|---|---|---|---|
| Micro800 1000 tags, 8 connections | v2.7.2 | 15 | 1062 | 17 | 5 |
| | HEAD | 52 | 1101 | 61 | 23 |
| Micro800 1000 tags, 1 connection | v2.7.2 | 82 | 1144 | 98 | 34 |
| | HEAD | 102 | 1162 | 104 | 55 |
| ControlLogix 10000 tags, 1 connection | v2.7.2 | 45 | 1147 | 46 | 254 |
| | HEAD | 67 | 1206 | 73 | 507 |

- ControlLogix: `cli` doubles on HEAD, 254 to 507 µs per exchange: reply dispatch and building
  the next packed exchange. This is the 1-2 connection regression, and it is client code.
- Micro800: HEAD is slower in the two wake parts, not in its own code. The cause is idle-state
  wake latency (next section).

### Deep idle states blocked, Linux (2026-11-02)

v2.7.2's client busy-polls: 78% CPU on Micro800 async 1 × 1, against 28% for HEAD (fairness
matrix, `cpu_load_pct`). That keeps the cores out of deep idle states. HEAD blocks, and on this
host each wake from C3-C8 takes 70-200 µs (`cpuidle` latencies) under the `powersave` governor.
A re-run with `/dev/cpu_dma_latency` held at 0, which keeps the cores out of deep idle states:

```sh
sudo sh -c 'exec 3>/dev/cpu_dma_latency; printf "\0\0\0\0" >&3; sleep 900' &
```

| Cell | Deep idle | v2.7.2 ops/s | HEAD ops/s | `to_srv` / `to_cli` µs, v2.7.2 → HEAD |
|---|---|---|---|---|
| Micro800 1000 tags, 8 connections | allowed | 6797 | 6065 (−11%) | 15/17 → 52/61 |
| | blocked | 7134 | 7249 (+1.6%) | 9/10 → 10/11 |
| Micro800 1000 tags, 1 connection | allowed | 720 | 701 (−3%) | 82/98 → 102/104 |
| | blocked | 901 | 906 (+0.6%) | 10/10 → 9/9 |

- The Micro800 regression is the host's idle states, not HEAD's code. With deep idle blocked,
  HEAD matches v2.7.2 or beats it, at about a third of the client CPU.
- Linux runs before this one allowed deep idle. In latency-bound cells, v2.7.2's spinning wins
  by keeping the CPU awake. Those verdicts measure wake latency, not library work.

### Settle wait × server delay, Linux, deep idle blocked (2026-11-02)

ControlLogix read, each HEAD settle variant against v2.7.2 in its own A B B A pair, 3 rounds,
`plctag_dyn`, deep idle blocked for every run (`perf_env.txt`). Settle variants are
`SETTLE_QUIET_US` / `SETTLE_MAX_US` in `conn.c`: off (`SETTLE_MIN_BATCH` = 2^30), 25/100,
50/300 (HEAD), 100/600. ab_server applied 158-165 µs for 0.1 ms, 359-370 µs for 0.3 ms and
1055-1076 µs for 1 ms, so the fast end is a PLC that answers in about 160 µs. The full pivot of all
36 cells is `docs/perf/Precision-Tower-3420-i5-6500-32GB/settle_sweep/pivot.txt`.

Ops/s, HEAD (50/300) against v2.7.2:

| Cell | 0.1 ms v2.7.2 | 0.1 ms HEAD | 0.3 ms v2.7.2 | 0.3 ms HEAD | 1 ms v2.7.2 | 1 ms HEAD |
|---|---|---|---|---|---|---|
| async 10000 1×1 | 387k | 395k | 275k | 253k (−7.5%) | 121k | 122k |
| async 10000 1×2 | 389k | 896k | 388k | 569k | 245k | 259k |
| async 10000 4×4 | 107k | 1042k | 116k | 914k | 398k | 508k |
| async 10000 8×8 | 36k | 1082k | 28k | 1118k | 26k | 892k |
| sync 10000 1×1 | 839 | 5244 | 715 | 2552 | 501 | 914 |
| sync 10000 8×8 | 6120 | 34574 | 5215 | 18483 | 3590 | 7032 |
| async 1000 4×4 | 435k | 715k | 453k | 705k | 337k | 367k |
| sync 1000 8×8 | 26.6k | 34.3k | 16.6k | 18.3k | 6.7k | 7.0k |

- With deep idle blocked, HEAD is better or the same in every cell but one (async 10000 1 × 1 is
  within 2% at 0.1 and 1 ms). The earlier Linux WORSE cells (1000 tags
  4-8 threads, 10000 tags 1 × 1 at 1 ms) were wake latency. The one loss left is async 10000 1 × 1
  at 0.3 ms, −7.5%.
- The faster the PLC, the bigger HEAD's lead. Sync 10000 tags is 6× v2.7.2 at 0.1 ms.
- Client CPU: v2.7.2 uses 100-310% in every cell, because it spins. HEAD sync uses 3-17% at 10000 tags.
- Client memory: v2.7.2 peaks at 0.9-1.6 GB in async 4 × 4 and 8 × 8 (10000 tags). HEAD stays at
  42 MB in every cell.
- The settle wait never helps. Across all 36 cells the four variants are within 2% of each
  other, except async 10000 1 × 1, where any settle setting costs throughput. Ranges do not
  overlap:

  | Delay | Settle off | 25/100 | 50/300 (HEAD) | 100/600 |
  |---|---|---|---|---|
  | 0.1 ms | 420958 | 389727 | 394943 | 394321 |
  | 0.3 ms | 266973 | 253273 | 253418 | 253114 |
  | 1 ms | 125532 | 122474 | 122406 | 122207 |

  With the settle off, the 0.3 ms loss against v2.7.2 shrinks from −7.5% to −2.8%.

### Packet sizes without the settle wait, Linux, deep idle blocked (2026-11-03)

The settle wait was added against packets that alternate 1 / 100+ / 1 tags. An `LD_PRELOAD`
shim logs each request's size in the measured window. ControlLogix with `perfTagN[0]`: a lone
tag is 66 bytes, and a packed exchange is 54 + 22 × tags, up to 181 tags. "Small" is 2 tags or
fewer. Async read, one run per cell, settle off against HEAD (50/300).

| Cell | Delay | Small, off / HEAD | P(small after big), off / HEAD | P(small after small), off / HEAD | ops/s, off / HEAD |
|---|---|---|---|---|---|
| 10000 1×1, 1×2 | all | 0-0.1% / 0% | 0-0.1% / 0% | 0% / 0% | within 5%, except one HEAD run (below) |
| 10000 4×4 | 0.1 ms | 1.2% / 0.2% | 1.2% / 0.2% | 3.5% / 15.5% | 1041k / 1059k |
| 1000 4×4 | 0.1 ms | 11.7% / 1.2% | 13.1% / 1.0% | 0.9% / 15.9% | 743k / 709k |
| 1000 4×4 | 0.3 ms | 4.2% / 0.3% | 4.4% / 0.3% | 0.1% / 3.8% | 663k / 648k |
| 1000 8×8 | 0.1 ms | 3.8% / 0.4% | 3.8% / 0.4% | 3.1% / 4.5% | 731k / 764k |
| 1000 4×4, 8×8 | 1 ms | 0.7-1.6% / 0% | 0.7-1.6% / 0% | 0% / 0% | within 1% |

- No steady alternation. A small exchange follows a big one at most 13% of the time
  (1000 4 × 4 at 0.1 ms, sample `62 64 61 62 1 62 63 62`), and another small one almost
  never follows it. These are isolated blips.
- At 10000 tags on 1-2 connections, packets are full (181 tags) with or without the settle.
- The settle cuts small packets 5-10×, but does not pay: settle off is as fast or faster in the
  settle sweep above.
- One HEAD run at 0.3 ms, 10000 1 × 1, gave 155k ops/s against 253k in the sweep, with half the
  exchanges. A single traced run; treat it as noise.

### Auto-sync saturation, Linux, deep idle blocked (2026-11-03)

Auto-sync read, 1 ms period, 1 connection, 0.3 ms server delay: the period is shorter than the
tags can be read, so every read is late. The spot check found HEAD 40-65% slower than v2.7.2
here. `perf` showed two causes:

- Every late read logged `WARN ... Skipping N periods` (`lib/tag_driver.c`), about 120000 lines a
  second. HEAD logs it on the I/O thread under the connection mutex, so each exchange waited
  for the log writes. v2.7.2 logs the same warning, but from its separate tickler thread.
  perf_benchmark runs at `PLCTAG_DEBUG_WARN`, so this counts in every benchmark of the case.
- `TAG_DRIVER_TICKLE_BATCH` = 64 capped every auto-sync packet at 64 tags.

Fixed:

- The message stays `DEBUG_WARN`, now with `DEBUG_RATE_LIMIT` (5eb4370f): it prints the first time,
  then at most once every 5 s (`DEBUG_LIMIT_MS`) with a count of the dropped lines.
- The cap is 512 now (4b5ce255).

The table below was measured with the earlier fix: the message at `DEBUG_DETAIL` and a cap of 256.
The re-run with the current code follows it. Two runs each:

| Tags | v2.7.2 ops/s (CPU) | HEAD before | HEAD after |
|---|---|---|---|
| 1000 | 176k (134%) | 98k (42%) | 326k (29%) |
| 10000 | 169k (152%) | 57k (66%) | 170k (62%) |

- 1000 tags: 1.9x v2.7.2 at about a fifth of its CPU. Achieved period 3.1 ms against 5.5 ms.
- 10000 tags: equal throughput at 40% of the CPU. What is left is the linear search in the
  tag list (`vector_find_index`); a tree keyed on (op_time, sequence) removes it (Pending).

Re-run with the current code (58ee20be: rate-limited warning, cap 512), 2026-11-04,
`--auto-matrix=logix`, `--rounds=2 --duration=5`. Summaries are in
`docs/perf/Precision-Tower-3420-i5-6500-32GB/v2.7.2_vs_58ee20be/`:

| Tags | Verdict | ops/s A / B | µs/op A / B | MB A / B | cli% A / B |
|---|---|---|---|---|---|
| 1000 | better | 155k / 301k (+94%) | 8.1 / 0.9 | 12.5 / 6.6 | 126 / 28 |
| 10000 | better | 140k / 171k (+22%) | 10.2 / 3.5 | 76.0 / 42.2 | 141 / 60 |

- HEAD wins on all three measures in both cells. HEAD's numbers match the earlier fix; v2.7.2 ran
  10-17% lower this time, and it still logs every late read.
- The 1000-tag cell is about 8% below the 326k measured with the warning at `DEBUG_DETAIL`. One run
  pair, so this may be noise; the rate-limit check costs one `time_ms()` per late read.

### Async 250 tags, 1 × 1, 1 ms polling, Linux, deep idle blocked (2026-11-03)

The spot check found HEAD 30% slower here. It is the harness's poll interval, not library cost:
HEAD uses 16% CPU against 80% for v2.7.2, and logging off changes nothing.

A cycle starts all tags, then the app checks status and sleeps 1 ms until all are done. Both
versions take three round trips per cycle: the first `plc_tag_read` wakes the I/O thread, which
sends the 3-6 tags started so far; then a full packet (161-181 tags); then the rest. Traced
cycle, HEAD (v2.7.2 in brackets), µs from the first `plc_tag_read`:

| Phase | µs |
|---|---|
| Start loop, 250 `plc_tag_read` calls | 320 (731) |
| Last reply | 1255 (1270) |
| App wakes after last reply | +145 (+571) |
| Cycle | 1453 (1910) |

The app wakes about start loop + 1 ms after the cycle starts. HEAD's start loop is faster, so its
wake lands within tens of µs of the last reply; when the reply is later, the cycle takes a second
1 ms sleep (2.3 ms). Even the trace shim's few ns per call moved HEAD to one sleep (165k ops/s).
Sweep, ops/s, one 3 s run each:

| Tags | v2.7.2 poll 1 | HEAD poll 1 | | v2.7.2 poll 0 | HEAD poll 0 | |
|---|---|---|---|---|---|---|
| 150 | 116k | 125k | +8% | 146k | 170k | +16% |
| 175 | 127k | 148k | +16% | 168k | 195k | +16% |
| 200 | 126k | 164k | +31% | 139k | 208k | +50% |
| 225 | 141k | 100k | -29% | 152k | 170k | +12% |
| 250 | 154k | 109k | -29% | 165k | 187k | +13% |
| 275 | 155k | 119k | -24% | 174k | 206k | +18% |
| 300 | 161k | 129k | -20% | 187k | 212k | +13% |

- HEAD's cliff sits between 200 and 225 tags, where three round trips (about 1.25 ms) pass the
  app's wake. v2.7.2 has the same cliff at a different count. Without the sleep, HEAD wins at
  every count.
- Library-side gain still open: the first 3-6 tag packet costs one server delay per cycle.
  Two round trips (181 + 69) would be about 0.85 ms and stay under the 1 ms wake.

### Settle wait removed (2026-11-03)

`settle_due_tags` and the `SETTLE_*` constants are gone from `conn.c`. The settle wait cost 3-7%
on one connection with one thread, and gained nothing anywhere in the sweep above.

### ControlLogix 1 × 1 regression gone, Linux, deep idle blocked (2026-11-03)

Exchange split, 10000 tags, async read, settle removed, medians of 2 runs:

  | Delay | Version | ops/s | `to_srv` | `srv` | `to_cli` | `cli` |
  |---|---|---|---|---|---|---|
  | 0.3 ms | v2.7.2 | 238k | 15 | 418 | 17 | 152 |
  | | HEAD | 259k (+9%) | 11 | 405 | 10 | 217 |
  | 1 ms | v2.7.2 | 114k | 19 | 1125 | 21 | 248 |
  | | HEAD | 122k (+7%) | 13 | 1111 | 11 | 277 |

- The earlier 254 to 507 µs `cli` gap was mostly the settle wait (up to 300 µs) plus deep idle.
- HEAD still spends 30-65 µs more client time per exchange, but wins it back on faster wakes and
  shorter server handling.

Hardware tests pass (2026-11-03) with the settle wait removed and the auto-sync fixes.

### v2.7.2 vs HEAD 4b5ce255, scale and fairness matrices, M1 (2026-11-03 to 11-04)

The core runs from the Linux host, on the M1: `--scale-matrix=logix`, `--scale-matrix=modbus`
(`--duration=5`) and `--fairness-matrix`, all `--delay-ms=1 --rounds=3`, both sides on
`plctag_dyn` (Release, clang, no sanitizers). 636 configurations, 12.9 h, on mains power, no
failed runs, no thermal warnings. Server delay applied mean 1.35-1.48 ms. macOS cannot block deep
idle, so these runs allow it. Summaries are in `docs/perf/MacBook-Air-M1-16GB/v2.7.2_vs_4b5ce255/`.
Judged on all three measures (`--from-csv`):

| Run | Better | Same | WORSE | CPU/op WORSE | Memory WORSE |
|---|---|---|---|---|---|
| Scale, ControlLogix | 156 | 6 | 30 | 0 | 0 |
| Scale, Modbus | 190 | 0 | 2 | 0 | 0 |
| Fairness matrix | 241 | 0 | 11 | 0 | 0 |

- **CPU per op:** HEAD lower or the same in every cell: 30-94% lower on ControlLogix in the
  fairness matrix, 25-88% on Micro800, 10-69% on Modbus.
- **Memory:** HEAD lower or the same in every cell. Async ControlLogix 10000 tags at 4 × 4:
  44 MB against 2.6 GB.
- **Ops/s:** every WORSE cell is a throughput loss of 5-15%, always with lower CPU and a longer
  p95:
  - ControlLogix async, 1000 tags (scale, 30 cells) and 10000 tags with 1-4 threads on 1-3
    connections (fairness, 8 cells). Worst: 10000 tags 2 × 1 write, −15.1% (73k against 86k
    ops/s, 12.0 against 18.5 µs/op). 1 × 1 read is −14.0%.
  - Modbus sync 1000 tags 8 × 8, −6 to −7%, at about half the CPU per op. In both runs.
  - Micro800 async write 1 × 16, −5.4%.
- **Gains:** async ControlLogix 8 × 8 is 7.3× v2.7.2 (907k against 124k ops/s), 1 × 16 +50%.
  Sync ControlLogix 1 × 16 +27%.
- **Micro800:** no regression on the M1 apart from the one cell above.
- The ControlLogix async loss is the cell set that went away on Linux once deep idle was blocked.
  v2.7.2 spins (84-180% client CPU in those cells against 32-127% for HEAD); HEAD blocks and pays
  the wake. macOS cannot block idle, so the M1 cannot test that directly.

### v2.7.2 vs hashtable vs slot table, ControlLogix scale matrix, Linux, deep idle blocked (2026-11-07)

Three versions: v2.7.2, b8b1cc32 (tag ids in a hashtable under `tag_lookup_mutex`) and the
slot table (7b17e21d, the slot index as a modulus, 65536 slots). The slot table is a static array
of `{lock, tag_id, tag}` entries indexed by tag id. A lookup takes only that entry's spinlock.
`--scale-matrix=logix --rounds=3 --duration=5`, two pairs per delay, all sides `plctag_dyn`
(Release) with the same `perf_benchmark.c`. Memory is client peak RSS.

| Delay | Pair | Better | Same | WORSE | CPU/op (median) | Memory (median) | ops/s (median) |
|---|---|---|---|---|---|---|---|
| 0.3 ms | v2.7.2 → b8b1cc32 | 192 | 0 | 0 | −55% | −12% | +3% |
| 0.3 ms | b8b1cc32 → slot | 67 | 120 | 5 | −33% async 1000 tags | 0 | 0 |
| 1 ms | v2.7.2 → b8b1cc32 | 179 | 0 | 13 | −53% | −10% | +1.5% |
| 1 ms | b8b1cc32 → slot | 60 | 125 | 7 | −41% async 1000 tags | 0 | 0 |

- **Slot table CPU:** −33 to −41% per op at async 1000 tags, −15 to −18% at async 100 tags,
  the same for sync. Under `perf record` (async read 1000 × 8, 1 ms), `lookup_tag` was 41.6% of
  client CPU on b8b1cc32 and 6.4% on the first slot table version (6e4645b2).
- **Slot table memory:** the same in all 384 cells. RSS does not show the fixed cost: 16 bytes
  per slot of `.bss`, 1 MB at 65536 slots, 4 MB at the default 262144. On a target without
  virtual memory that is all real RAM.
- **Slot table WORSE cells:** only throughput, async 1000 tags. They are the benchmark, not the
  library (next section).
- **v2.7.2 → b8b1cc32 at 0.3 ms:** async 1000 tags +40% ops/s at −72% CPU per op and −47%
  memory. Every WORSE cell at 1 ms is async 100 tags on 1-2 connections, the same benchmark
  effect.

### Async throughput and the 1 ms poll sleep (2026-11-07)

Async `perf_benchmark` sleeps `--poll-ms` (1 ms by default) between status passes. That sleep
beats against the server delay, so a client whose round trip changes by tens of µs can land on a
worse phase and lose 5-20% throughput while it uses less CPU. Slot table against b8b1cc32, async
1000 tags, 5 rounds:

| Cell | Delay | 1 ms poll | 0 ms poll | 2 ms poll |
|---|---|---|---|---|
| read 1000 × 1 g1 | 1 ms | −7% | 0% | +4% |
| read 1000 × 1 g1 | 0.3 ms | +7.5% | −1% | +5% |
| write 1000 × 2 g4 | 0.3 ms | −20% | +24% | |
| read 1000 × 4 g4 | 0.3 ms | −2% | +41% | |
| write 1000 × 4 g4 | 0.3 ms | −5% | +43% | |

- Request packets carry the same number of tags on both sides (about 142, counted with a
  `write()` shim). The slower cells make fewer exchanges, not smaller ones.
- The losing cells move when the delay changes, and the sign flips with the poll interval.
- At 0 ms the app thread spins, so only throughput compares there, not CPU.
- Read an async throughput loss that comes with lower CPU and a longer p95 as this effect until a
  0 ms poll run says otherwise. The M1's ControlLogix async 5-15% losses above have that shape.

### Pending

- ControlLogix async 5-15% slower on the M1 (above). Find out whether it is wake latency or
  the 30-65 µs more client time per exchange measured on Linux. Two timing runs, both on the M1,
  ControlLogix async read, 10000 tags, 1 × 1, 1 ms delay, HEAD only:
  1. **Client code per exchange.** Time `dispatch_replies_unsafe` and `build_exchange_unsafe`
     (`modules/cip/conn.c`) with `CLOCK_MONOTONIC` on entry and exit; report the median and p95
     of each, µs per exchange. Their sum against the 30-65 µs gap says whether the client code
     is the loss.
  2. **Wake latency per exchange.** The exchange split ("Exchange split, Linux") on both
     v2.7.2 and HEAD: `to_srv`, `srv`, `to_cli`, `cli`. On macOS the shim loads with
     `DYLD_INSERT_LIBRARIES` instead of `LD_PRELOAD`. If HEAD's `to_srv` + `to_cli` is larger by
     about the ops/s loss, the loss is wake latency, which macOS cannot block.
- Re-run the Linux fairness matrix and the Modbus scale matrix with deep idle blocked. The
  ControlLogix scale matrix is done (2026-11-07, above). Modbus only needs a run if the
  ControlLogix versions had differed.
- Re-check the M1 ControlLogix async losses with `--poll-ms=0` (see "Async throughput and the
  1 ms poll sleep").
- Hardware performance numbers.
- Optional: replace the tag list vector with an intrusive red-black tree keyed on (op_time,
  insertion sequence), every tag in it, idle tags at now + 1 year. Scratchpad benchmark,
  600-byte tags, 181 tags per exchange: auto-sync list cost at 10000 tags 248 µs to 9 µs per
  exchange; polling 6 to 16 µs (all tags in the tree cost two more tree operations per op).
  Needs the rightmost and finger insert shortcuts. Only auto-sync at 10000 tags gains, and there
  HEAD already matches v2.7.2 throughput at 40% of its CPU.
- Optional: send the first 3-6 tag packet of an async cycle with the full one (one server delay
  per cycle).

`docs/perf/<host>/` keeps only high-level data: `host_info.txt`, each run's verdict summary and
its `--summary` CSVs. Raw CSVs (`--out`) and `perf_env.txt` files stay out of the repository.

## Known limits

- Client and server share one machine. Above about 6 connections, client plus server CPU passes
  the 4 performance cores and scaling bends. A separate server host would show whether the
  library goes further.
- modbus_server is single threaded and reached 78% of its core at 8 connections with no delay.
- Fairness is not run in CI: CI hardware is overcommitted and stalls. It runs by hand before a
  release.
- Async throughput in the matrices depends on the phase between the 1 ms poll sleep and the
  server delay, ±20% in either direction (2026-11-07 above). CPU per op and memory do not.

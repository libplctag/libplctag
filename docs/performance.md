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
| `server_cpu_pct` | Server CPU, from the harness. |

- With `--poll-ms=0` the async loop spins, and `cpu_us_per_op` measures the spin, not the library.
  All comparisons use `--poll-ms=1`.

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
  connections: HEAD 206-298k ops/s at 526-597% client CPU, v2.7.2 690-704k at 271-273%.
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
  It now skips redundant wakes and waits up to 300 µs for a batch to settle. Async ControlLogix
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
  suspect, not proven.
- **Tag CV:** HEAD at most 0.59% anywhere (0.36% at 8 × 8). v2.7.2 up to 1.97%, above 1% at
  4 × 8, 8 × 4 and 8 × 8, where its throughput collapses.
- **Thread window min/max:** HEAD worst 0.857 (8 threads × 1 connection, write), v2.7.2 0.798.
  On one shared connection both are about 0.86-0.93.

Proposed thresholds, from these runs:

- `tag_cv` ≤ 1% (HEAD's worst is 0.59%).
- `window_min_max` ≥ 0.80 (HEAD's worst is 0.857). To be confirmed against the
  performance/efficiency core gap.

### Pending

- Find the 1-2 connection regression: count tags per exchange and settle waits on HEAD at 1 × 1.
- Full matrix re-run (sync, Micro800, Modbus) on the current HEAD, with `tag_cv`.
- Hardware confirmation of both fixes on a real ControlLogix.
- Raw CSVs go in `docs/perf/MacBook-Air-M1-16GB/<version>/` with their `perf_env.txt`.

## Known limits

- Client and server share one machine. Above about 6 connections, client plus server CPU passes
  the 4 performance cores and scaling bends. A separate server host would show whether the
  library goes further.
- modbus_server is single threaded and reached 78% of its core at 8 connections with no delay.
- Fairness is not run in CI: CI hardware is overcommitted and stalls. It runs by hand before a
  release.

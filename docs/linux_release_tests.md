# Linux tests for the 2.7.3 release

The release-gate tests that run on the Linux machine (Precision Tower 3420,
`docs/perf/Precision-Tower-3420-i5-6500-32GB`).  The M1 runs are functional checks only; these
give the release numbers.  Item numbers are from `docs/release_2_7_3_must_do.md`.

## Setup

1. Block deep CPU idle for 24 h (root).  `perf_env.sh` refuses to start without it:

   ```sh
   sudo sh -c 'exec 3>/dev/cpu_dma_latency; printf "\0\0\0\0" >&3; exec sleep 86400' &
   ```

2. Two Release builds of HEAD:

   ```sh
   cmake -S . -B build_rel -DCMAKE_BUILD_TYPE=Release && cmake --build build_rel -j
   cmake -S . -B build_dyn -DCMAKE_BUILD_TYPE=Release -DUSE_VALGRIND=1 && cmake --build build_dyn -j
   ```

   `build_dyn` is only for item 13: Release links the test binaries `-static`, where `LD_PRELOAD`
   does nothing.  `USE_VALGRIND=1` only turns that off.

3. v2.7.2 and HEAD `perf_benchmark` binaries, each linked against its own Release `plctag_dyn`, as
   in `docs/performance.md`, "Builds".  Below they are `v2.7.2/perf_benchmark` and
   `head/perf_benchmark`.

4. Start every run under `src/tests/scripts/perf_env.sh <out_dir> <command...>`.  It records the
   host, the commit and the idle state.

## Tests

| # | Test | About |
|---|---|---|
| 8 | Modbus tag ramp | Minutes |
| 13 | Allocation counts, HEAD and v2.7.2 | Minutes |
| 10, 14 | v2.7.2 vs HEAD: fairness and Modbus scale matrices | Fairness about 14 h at 5 rounds |
| 11 | Fairness thresholds | From the item 14 fairness run |
| 15 | Final performance numbers | Hours |

### 8. Modbus tag ramp

```sh
src/tests/scripts/perf_env.sh out/mb_ramp \
    python3 src/tests/scripts/run_tag_ramp.py --protocol=modbus --bin-dir=build_rel/bin_dist
src/tests/scripts/perf_env.sh out/mb_ramp8 \
    python3 src/tests/scripts/run_tag_ramp.py --protocol=modbus --bin-dir=build_rel/bin_dist --groups=8
```

- Ramp 50, 500, 5000, 50000 tags.  It stops when `modbus_server` reaches 95% of one core over a
  step's reads ("# server limit" line).
- Record: the final table, and the step at which the server limit was hit, if any.  On the M1 at
  one connection the client was the limit (126% client, 20% server at 50000).
- Fails on any tag error.

### 13. Allocation counts

HEAD:

```sh
python3 src/tests/scripts/run_tag_ramp.py --bin-dir=build_dyn/bin_dist \
    --alloc-count=build_dyn/bin_dist/liballoc_count.so --ramp=2000,20000
python3 src/tests/scripts/run_tag_ramp.py --protocol=modbus --bin-dir=build_dyn/bin_dist \
    --alloc-count=build_dyn/bin_dist/liballoc_count.so --ramp=500,5000
```

v2.7.2: build `test_many_tags` against v2.7.2's library the same way as `perf_benchmark`, add
`-ldl`, put it in a directory with HEAD's `ab_server` and `modbus_server`, and pass that
directory as `--bin-dir`.

- The Linux path of `liballoc_count` (`LD_PRELOAD`, `dlsym(RTLD_NEXT)`) has not run yet.  If
  the step prints "alloc_count did not load", the binary is static or ASan-built.
- Record: `allocs/create` and `allocs/read` for each step and version.  Expected on HEAD from the
  M1: CIP about 29 per create and 2 per read (known issue 5, fixed after 2.7.3); Modbus 19 per
  create and 0 per read.

### 10 and 14. v2.7.2 vs HEAD

```sh
src/tests/scripts/perf_env.sh out/fair \
    python3 src/tests/scripts/run_perf_compare.py \
        --a=v2.7.2/perf_benchmark --b=head/perf_benchmark --server-dir=build_rel/bin_dist \
        --fairness-matrix --delay-ms=1 --rounds=5 --out=out/fair/compare.csv --summary=out/fair/summary.csv
src/tests/scripts/perf_env.sh out/mb_scale \
    python3 src/tests/scripts/run_perf_compare.py \
        --a=v2.7.2/perf_benchmark --b=head/perf_benchmark --server-dir=build_rel/bin_dist \
        --scale-matrix=modbus --delay-ms=1 --rounds=3 --out=out/mb_scale/compare.csv
```

- The ControlLogix scale matrix was done 2026-11-07; it is not repeated.
- Pass: no WORSE verdict, except an async throughput loss that comes with lower CPU and a longer
  p95 (the poll-phase effect, `docs/release_2_7_3_must_do.md` item 10).  Rerun such a cell
  with `--poll-ms=0 --only="<cell key>"` to confirm.
- `--rounds=5` on the fairness matrix gives the 5 HEAD runs item 11 needs, so it is run once.

### 11. Fairness thresholds

- From the HEAD (B) side of the item 14 fairness run: tag CV and the worst 1 s thread min/max
  window per cell.
- Write the thresholds into `docs/performance.md`.

### 15. Final performance numbers

On the release code (262144 tag slots):

```sh
src/tests/scripts/perf_env.sh out/cip_ramp \
    python3 src/tests/scripts/run_tag_ramp.py --bin-dir=build_rel/bin_dist
src/tests/scripts/perf_env.sh out/release_matrix \
    python3 src/tests/scripts/run_perf_compare.py \
        --a=v2.7.2/perf_benchmark --b=head/perf_benchmark --server-dir=build_rel/bin_dist \
        --release-matrix=logix --out=out/release_matrix/compare.csv
```

- The CIP ramp is 200 to 200000 tags.  The 2026-11-08 run gave a median pass of 457 ms at
  200000.
- Item 8 gives the Modbus numbers.

## Not on this machine

- Item 6, the connection count ramp, runs against the L81 from `run_hardware_tests.py`.  The
  2026-11-18 run was a Debug build.  If the Linux machine reaches the lab network, a run from
  `build_rel/bin_dist` gives Release numbers:
  `python3 src/tests/scripts/run_connection_ramp.py build_rel/bin_dist/perf_benchmark --tag='protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&name=TestBigArray[0]'`
- Item 18, the backoff unit tests, runs in CI (Windows, and ARM under QEMU).  Check the next CI run.

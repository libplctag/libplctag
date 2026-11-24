# 2.7.3 release gate (Phase 9): Linux runs

This file is for the agent on the Linux machine (Dell Precision Tower 3420, i5-6500, 4 cores,
32 GB, Debian). It lists the Phase 9 steps that must run there, with commands.

Code state: branch `ready_next_release_v2_7_3`. The simulator suite (410/410) and the hardware
suite pass on macOS. Everything here measures performance; nothing here is a code change.

## Rules

- **Do not commit.** Leave every change uncommitted; the user commits.
- **GitHub is read-only.** `gh run view`, `gh pr view` and other reads are fine. Never comment,
  post, push, label or edit anything on GitHub.
- **Change as little as possible.** If a run shows a bug, a script error or a regression,
  report it with file:line and a one-line description, and **do not fix it**. Wait for the user.
- **Never rebuild a build directory while a run is using it.**
- **Never run `cmake --build <dir> --target clean`.** It deletes the test executables, and a plain
  rebuild does not bring them back. To start from scratch, delete the build directory instead.
- **Release builds only, with no sanitizers.** A Debug or ASan build is never measured.
- **Block deep idle states for every measured run.** `perf_env.sh` refuses to start otherwise.
  This needs root; ask the user to run it if you cannot:

  ```sh
  sudo sh -c 'exec 3>/dev/cpu_dma_latency; printf "\0\0\0\0" >&3; exec sleep 86400' &
  ```

  The block ends after 24 h or when that `sleep` is killed. Start every measured run under
  `src/tests/scripts/perf_env.sh <out_dir> <command...>`. It checks idle states, sets
  `ulimit -n 1024` and records the host and commit in `<out_dir>/perf_env.txt`.
- **One measured run at a time.** Do no builds and run nothing else heavy while a run is going.
  Client and server share the 4 cores.
- Long runs take hours. Run them under `nohup` or `tmux` so a lost shell does not kill them.
- Background docs:
  - `docs/release_2_7_3_must_do.md`: items by number.
  - `docs/performance.md`: method, verdict rules, earlier Linux results.
  - `ephemeral_docs/release_2_7_3_plan.md`: the Phase 9 table.

## 0. Builds

All paths below are relative to the repo root. `$V272` is a separate checkout of tag `v2.7.2`
(`7712ed7f`), for example a `git worktree add ../v2.7.2 v2.7.2`.

```sh
# HEAD, Release (servers, test_many_tags, test_fairness, perf_benchmark source)
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j4
cmake --build build-release -j4 --target ab_server modbus_server test_many_tags test_fairness perf_benchmark

# HEAD, Release, dynamic test binaries; only for allocation counting (item 13).
# Release links the tests -static on Linux, and LD_PRELOAD does nothing on a static binary.
cmake -S . -B build-release-dyn -DCMAKE_BUILD_TYPE=Release -DUSE_VALGRIND=1
cmake --build build-release-dyn -j4 --target test_many_tags alloc_count ab_server modbus_server

# v2.7.2, Release
cmake -S $V272 -B $V272/build-release -DCMAKE_BUILD_TYPE=Release
cmake --build $V272/build-release -j4
```

Build one `perf_benchmark` per library from the **HEAD** source. Both link the shared library
(`plctag_dyn`), so the only difference between A and B is the library:

```sh
mkdir -p perf/a perf/b
for side in "a:$V272/build-release/bin_dist" "b:build-release/bin_dist"; do
    out=${side%%:*}; lib=${side#*:}
    cc -O2 -Wall -I src/tests/utils -I "$lib" \
        src/tests/perf_benchmark/perf_benchmark.c src/tests/utils/compat_utils.c src/tests/utils/stats.c \
        -L"$lib" -lplctag -Wl,-rpath,"$(realpath "$lib")" -lm -lpthread -o perf/$out/perf_benchmark
done
ldd perf/a/perf_benchmark | grep plctag   # must point at the v2.7.2 bin_dist
ldd perf/b/perf_benchmark | grep plctag   # must point at the HEAD bin_dist
```

The servers always come from HEAD: `--server-dir=build-release/bin_dist`.

Check the scripts before the long runs:

```sh
python3 src/tests/scripts/run_perf_compare.py --self-check
python3 src/tests/scripts/run_tag_ramp.py --self-check
```

Put results under `docs/perf/Precision-Tower-3420-i5-6500-32GB/2.7.3_release/`, written `$OUT` below.
Record HEAD's short hash in each result.

## 1. CI: backoff unit tests on Windows and ARM (item 18)

Read only. Find the latest CI run on `ready_next_release_v2_7_3`:

```sh
gh run list --branch ready_next_release_v2_7_3 --limit 5
gh run view <id>
```

Report whether these steps passed:
- the Windows jobs' `ctest -R unit_test -C MinSizeRel` (`.github/workflows/ci.yml` about lines
  906-910);
- the ARM cross jobs' `ctest -R unit_test` under QEMU (about line 1092).

Report a failure with the job and test name. Do not rerun jobs.

## 2. Tag ramps (items 7, 8, 13; part of item 15)

These are short, about minutes each.

```sh
# CIP, 200 to 200000 tags, no server delay
src/tests/scripts/perf_env.sh $OUT/ramp_cip \
    python3 src/tests/scripts/run_tag_ramp.py --bin-dir=build-release/bin_dist --idle-s=10 \
    > $OUT/ramp_cip/ramp.log 2>&1

# Modbus, 50 to 50000 tags (item 8): run with 1 and with 8 connections
for g in 1 8; do
  src/tests/scripts/perf_env.sh $OUT/ramp_modbus_g$g \
      python3 src/tests/scripts/run_tag_ramp.py --protocol=modbus --groups=$g \
      --bin-dir=build-release/bin_dist --idle-s=10 \
      > $OUT/ramp_modbus_g$g/ramp.log 2>&1
done

# Allocation counting (item 13), dynamic build
for p in cip modbus; do
  src/tests/scripts/perf_env.sh $OUT/alloc_$p \
      python3 src/tests/scripts/run_tag_ramp.py --protocol=$p --bin-dir=build-release-dyn/bin_dist \
      --alloc-count=build-release-dyn/bin_dist/liballoc_count.so \
      > $OUT/alloc_$p/ramp.log 2>&1
done
```

- If `liballoc_count.so` is not in `bin_dist`, find it with `find build-release-dyn -name 'liballoc_count*'`.
- Each run ends with a table. Copy the tables into the result notes.
- Compare against `docs/release_2_7_3_must_do.md`:
  - items 7 and 16: 200000 tags had a median of about 185 ms;
  - item 8: the M1 functional run;
  - item 13: the M1 numbers were CIP 29 allocs per create, 2.0 per read; Modbus 19 per create, 0 per read.
- Item 8's stop rule: the ramp stops when `modbus_server` reaches 95% of a core. Report whether it did, and at which step.
- The runner fails an alloc step that prints no counts. That means the binary was static; check
  which build directory was used.

## 3. v2.7.2 vs HEAD matrices (items 10, 11, 14, 15)

Run them in this order. The times are estimates for 4 cores.

| Run | Covers | Approx. time |
|---|---|---|
| a. Modbus scale matrix | 10, 14 | 1.8 h |
| b. ControlLogix scale matrix | 10, 15 (the 2026-11-07 run is older than the final code) | 1.8 h |
| c. ControlLogix release matrix | 15 | 5 h |
| d. Fairness matrix, 5 rounds | 10, 11, 14 | about 14 h |

```sh
CMP="python3 src/tests/scripts/run_perf_compare.py --a=perf/a/perf_benchmark --b=perf/b/perf_benchmark --server-dir=build-release/bin_dist"

# a.
src/tests/scripts/perf_env.sh $OUT/scale_modbus \
    $CMP --scale-matrix=modbus --delay-ms=1 --rounds=3 --duration=5 \
    --out=$OUT/scale_modbus/compare.csv --summary=$OUT/scale_modbus/summary.csv \
    > $OUT/scale_modbus/run.log 2>&1

# b.
src/tests/scripts/perf_env.sh $OUT/scale_logix \
    $CMP --scale-matrix=logix --delay-ms=1 --rounds=3 --duration=5 \
    --out=$OUT/scale_logix/compare.csv --summary=$OUT/scale_logix/summary.csv \
    > $OUT/scale_logix/run.log 2>&1

# c.
src/tests/scripts/perf_env.sh $OUT/release_logix \
    $CMP --release-matrix=logix --rounds=3 \
    --out=$OUT/release_logix/compare.csv --summary=$OUT/release_logix/summary.csv \
    > $OUT/release_logix/run.log 2>&1

# d.
src/tests/scripts/perf_env.sh $OUT/fairness \
    $CMP --fairness-matrix --delay-ms=1 --rounds=5 \
    --out=$OUT/fairness/compare.csv --summary=$OUT/fairness/summary.csv \
    > $OUT/fairness/run.log 2>&1
```

- Using `--rounds=5` for run d gives the "5 HEAD runs" that item 11 asks for, so the fairness run
  is not done twice. If 14 h is too long, ask the user before cutting it.
- A run can be re-judged without rerunning it:
  `run_perf_compare.py --from-csv=$OUT/<run>/compare.csv`.

### Reading the results (`docs/performance.md`, "Comparing two versions")

- A cell is **WORSE** if the medians differ by more than 5%, the ranges do not overlap, and B
  loses on any one of:
  - `ops_per_sec`
  - `cpu_us_per_op`
  - `peak_rss_kb`
- A run with the server above 90% of a core is judged on CPU per op, not throughput.
- **Known effect, not a regression.** An async loss that comes with lower client CPU and a longer
  p95 is the 1 ms poll phase. Earlier runs measured it at ±20%. Confirm such a cell with
  `--poll-ms=0 --only="<cell key>"` before calling it a regression.
- List every WORSE cell, with its key, both medians and both ranges. Do not investigate or
  change code; report them.

### Item 11: fairness thresholds

From run d, HEAD side (B) only, over all cells:
- worst `tag_cv`, separately for one thread and for several threads;
- worst `window_min_max` (thread fairness across threads);
- worst `conn_window_min_max`.

The proposed thresholds are `tag_cv` ≤ 1% and `window_min_max` ≥ 0.80 (`docs/performance.md`,
"Proposed thresholds"). For `test_fairness` and for one thread, the tag rule is ±1 operation per
tag within one connection.

Report the measured worst values and the margin to each threshold. Then write the final
thresholds into `docs/performance.md`, but only after the user agrees with them.

## 4. Optional: connection ramp on the L81 (item 6)

Run this only if this machine can reach 10.206.1.40. The Debug run from 2026-11-18 already
passed; this run is for Release numbers. Reads only; it is safe on the shared PLC.

```sh
python3 src/tests/scripts/run_connection_ramp.py perf/b/perf_benchmark \
    --tag="protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&name=TestBigArray[0]"
```

This is the same tag string that `run_hardware_tests.py` uses (about line 575).

## 5. Report back

Write `$OUT/SUMMARY.md` with:
- HEAD hash, v2.7.2 hash, the `perf_env.txt` for each run (`deep_idle_*` must say blocked);
- per item (6, 7, 8, 10, 11, 13, 14, 15, 18): pass, fail or not run, and the numbers;
- every WORSE cell and every failure, with file:line where it applies;
- anything that looked wrong in the scripts, unfixed.

Then update the Status column of the summary table in `docs/release_2_7_3_must_do.md` and the
Phase 9 table in `ephemeral_docs/release_2_7_3_plan.md` with the date and the result. Change
nothing else. The user reviews and commits.

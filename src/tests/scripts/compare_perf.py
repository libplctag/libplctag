#!/usr/bin/env python3
"""
compare_perf.py - Compare two perf_benchmark CSV result files.

Usage:
    compare_perf.py <baseline.csv> <candidate.csv> [--threshold=PCT]

Joins on (mode, connection_groups, threads, tags) and reports:
- reads/sec change (%)
- CPU load change (absolute percentage points)
- Fairness CV change

Exit code 0 if no regressions beyond threshold, 1 otherwise.
"""

import csv
import sys
import os


def read_results(path):
    """Read a perf CSV, skipping comment lines. Returns dict keyed by (mode, groups, threads, tags)."""
    results = {}
    with open(path, newline="") as f:
        # Skip comment lines.
        lines = [line for line in f if not line.startswith("#")]
    reader = csv.DictReader(lines)
    for row in reader:
        key = (
            row["mode"],
            int(row["connection_groups"]),
            int(row["threads"]),
            int(row["tags"]),
        )
        results[key] = row
    return results


def pct_change(old, new):
    if old == 0:
        return float("inf") if new != 0 else 0.0
    return ((new - old) / old) * 100.0


def main():
    threshold = 10.0  # default regression threshold in %

    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = [a for a in sys.argv[1:] if a.startswith("--")]

    for flag in flags:
        if flag.startswith("--threshold="):
            threshold = float(flag.split("=", 1)[1])

    if len(args) != 2:
        print(f"Usage: {sys.argv[0]} <baseline.csv> <candidate.csv> [--threshold=PCT]")
        sys.exit(2)

    baseline_path, candidate_path = args

    if not os.path.isfile(baseline_path):
        print(f"Error: baseline file not found: {baseline_path}", file=sys.stderr)
        sys.exit(2)
    if not os.path.isfile(candidate_path):
        print(f"Error: candidate file not found: {candidate_path}", file=sys.stderr)
        sys.exit(2)

    baseline = read_results(baseline_path)
    candidate = read_results(candidate_path)

    # Print header info from files.
    for label, path in [("Baseline", baseline_path), ("Candidate", candidate_path)]:
        with open(path) as f:
            for line in f:
                if line.startswith("# "):
                    print(f"  {label}: {line.strip()[2:]}")
                    break

    all_keys = sorted(set(baseline.keys()) | set(candidate.keys()))

    if not all_keys:
        print("No data to compare.")
        sys.exit(0)

    # Table header.
    header = (
        f"{'mode':<6} {'grp':>4} {'thr':>5} {'tag':>5} "
        f"{'base rps':>12} {'cand rps':>12} {'rps Δ%':>8} "
        f"{'base cpu%':>10} {'cand cpu%':>10} {'cpu Δ':>7} "
        f"{'base cv':>8} {'cand cv':>8} {'cv Δ':>7} "
        f"{'flag':>6}"
    )
    print()
    print(header)
    print("-" * len(header))

    regressions = 0
    improvements = 0
    missing = 0

    for key in all_keys:
        mode, groups, threads, tags = key

        b = baseline.get(key)
        c = candidate.get(key)

        if not b or not c:
            missing += 1
            status = "MISS"
            if b and not c:
                print(f"{mode:<6} {groups:>4} {threads:>5} {tags:>5} {'(only in baseline)':>50} {status:>6}")
            elif c and not b:
                print(f"{mode:<6} {groups:>4} {threads:>5} {tags:>5} {'(only in candidate)':>50} {status:>6}")
            continue

        b_rps = float(b["reads_per_sec"])
        c_rps = float(c["reads_per_sec"])
        rps_delta = pct_change(b_rps, c_rps)

        b_cpu = float(b["cpu_load_pct"])
        c_cpu = float(c["cpu_load_pct"])
        cpu_delta = c_cpu - b_cpu

        b_cv = float(b["fairness_cv"])
        c_cv = float(c["fairness_cv"])
        cv_delta = c_cv - b_cv

        # Flag regressions: throughput dropped by more than threshold%.
        if rps_delta < -threshold:
            flag = "REGR"
            regressions += 1
        elif rps_delta > threshold:
            flag = "IMPR"
            improvements += 1
        else:
            flag = ""

        print(
            f"{mode:<6} {groups:>4} {threads:>5} {tags:>5} "
            f"{b_rps:>12.1f} {c_rps:>12.1f} {rps_delta:>+7.1f}% "
            f"{b_cpu:>9.1f}% {c_cpu:>9.1f}% {cpu_delta:>+6.1f} "
            f"{b_cv:>7.1f}% {c_cv:>7.1f}% {cv_delta:>+6.1f} "
            f"{flag:>6}"
        )

    print("-" * len(header))
    print(f"Compared {len(all_keys)} configurations. "
          f"Regressions: {regressions}, Improvements: {improvements}, "
          f"Missing: {missing}, Threshold: {threshold}%")

    if regressions > 0:
        print(f"\nFAILED: {regressions} regression(s) exceed {threshold}% threshold.")
        sys.exit(1)
    else:
        print("\nPASSED: No regressions detected.")
        sys.exit(0)


if __name__ == "__main__":
    main()

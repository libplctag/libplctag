#!/usr/bin/env python3
"""Connection count ramp (docs/release_2_7_3_must_do.md item 6).

Runs perf_benchmark, async reads of --tags one-element tags spread over 1 to --max-groups
connections (--group-by=tag), --duration seconds each with --pause seconds between, and logs
ops/s per connection count.  N is the smallest count within 5% of the peak; throughput drops
after the peak's count.  Reads only, so it is safe on a shared PLC.

Fails only on a perf_benchmark error, a read error, or a run with no reads: the curve is
reported, not judged, because the PLC is shared.

Usage:
  run_connection_ramp.py PERF_BENCHMARK --tag=BASE [--max-groups=8] [--tags=1000]
      [--duration=3] [--pause=2]
BASE is a full tag string, e.g. protocol=ab-eip&gateway=...&path=1,4&plc=ControlLogix&name=TestBigArray[0]
"""

import argparse
import subprocess
import sys
import time

# perf_benchmark's CSV line, columns used here
OPS = 6
OPS_PER_SEC = 7
ERRORS = 12


def parse_csv(output):
    """(total_ops, ops_per_sec, errors) from perf_benchmark's CSV line, or None."""
    for line in output.splitlines():
        cols = line.split(",")
        if cols[0] == "async" and len(cols) > ERRORS:
            return int(cols[OPS]), float(cols[OPS_PER_SEC]), int(cols[ERRORS])
    return None


def knee(rates):
    """(peak count, smallest count within 5% of the peak) from {count: ops/s}."""
    peak = max(rates, key=rates.get)
    return peak, min(g for g, r in rates.items() if r >= 0.95 * rates[peak])


def self_check():
    out = "# window start\n# window end\nasync,2,1,1000,1000.00,3000,9000,3000.0,1.0,0,1,read,0,1,2,3,4\n"
    assert parse_csv(out) == (9000, 3000.0, 0) and parse_csv("nothing") is None
    assert knee({1: 1000.0, 2: 1900.0, 3: 2000.0, 4: 1500.0}) == (3, 2)
    print("self-check ok")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("perf_benchmark", nargs="?")
    p.add_argument("--tag")
    p.add_argument("--max-groups", type=int, default=8)
    p.add_argument("--tags", type=int, default=1000)
    p.add_argument("--duration", type=int, default=3)
    p.add_argument("--pause", type=float, default=2.0)
    p.add_argument("--self-check", action="store_true")
    args = p.parse_args()
    if args.self_check:
        return self_check()
    if not args.perf_benchmark or not args.tag:
        p.error("PERF_BENCHMARK and --tag are required")

    rates = {}
    failed = False
    for groups in range(1, args.max_groups + 1):
        if groups > 1:
            time.sleep(args.pause)
        cmd = [args.perf_benchmark, "--mode=async", f"--groups={groups}", "--group-by=tag", "--threads=1",
               f"--tags={args.tags}", f"--duration={args.duration}", f"--tag={args.tag}"]
        print(f"=== {groups} connections: {' '.join(cmd)}", flush=True)
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        print(proc.stdout, end="", flush=True)
        result = parse_csv(proc.stdout)
        if proc.returncode != 0 or result is None or result[0] == 0 or result[2] != 0:
            print(f"FAIL: {groups} connections: rc={proc.returncode}, result={result}", flush=True)
            failed = True
            continue
        rates[groups] = result[1]

    print("\nconnections  ops/s")
    for groups, rate in rates.items():
        print(f"{groups:11d}  {rate:.1f}")
    if rates:
        peak, n = knee(rates)
        print(f"peak {rates[peak]:.1f} ops/s at {peak} connections; N = {n} (smallest within 5% of the peak)")
        if peak < max(rates):
            print(f"throughput drops after {peak} connections")
    print("FAIL" if failed else "PASS")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Compare two perf_benchmark builds (A = baseline, B = candidate).

Both binaries are the same harness source linked against different
libraries; one set of servers serves both.  Each configuration runs
A B B A ... for --rounds rounds so drift hits both sides equally.  Writes
every raw run to --out and prints one verdict line per configuration.

A difference counts only if the medians differ by more than 5 % and the
min-max ranges do not overlap.  When the server uses most of a core the
run is server-bound, so the verdict uses client CPU per op instead of
ops/sec.

Usage:
  run_perf_compare.py --a=perf_release --b=perf_head --server-dir=b_head/bin_dist
      [--rounds=5] [--duration=10] [--out=perf_compare.csv]
  Hardware (Logix only, no server, delay 0 only):
  run_perf_compare.py --a=... --b=... \\
      --logix-attrs="protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix" \\
      --logix-tag=TestBigArray
  run_perf_compare.py --self-check
"""

import argparse
import csv
import itertools
import socket
import statistics
import subprocess
import sys
import time
from pathlib import Path

THRESHOLD = 0.05
SERVER_BOUND_PCT = 90.0
LOGIX_PORT = 45100
MODBUS_PORT = 45102


def build_matrix(hardware):
    """The S4 preliminary set.  Modbus has no server delay option."""
    configs = []
    logix_delays = [0] if hardware else [0, 2]
    for delay, (tags, threads), elems, mode, op in itertools.product(
            logix_delays, [(1, 1), (100, 10)], [1, 2000], ["sync", "async", "auto"], ["read", "write"]):
        if mode == "auto" and op == "write":
            continue
        if elems == 2000 and op == "write":
            continue
        configs.append(dict(protocol="logix", delay=delay, tags=tags, threads=threads,
                            elems=elems, mode=mode, op=op))
    if not hardware:
        for (tags, threads), mode in itertools.product([(1, 1), (100, 10)], ["sync", "async", "auto"]):
            configs.append(dict(protocol="modbus", delay=0, tags=tags, threads=threads,
                                elems=1, mode=mode, op="read"))
    return configs


def config_key(c):
    return f"{c['protocol']} d{c['delay']} {c['mode']} {c['op']} {c['tags']}x{c['threads']} e{c['elems']}"


def tag_string(c, args):
    if c["protocol"] == "modbus":
        return f"protocol=modbus-tcp&gateway=127.0.0.1:{MODBUS_PORT}&path=1&name=hr0"
    attrs = args.logix_attrs or f"protocol=ab-eip&gateway=127.0.0.1:{LOGIX_PORT}&path=1,0&plc=ControlLogix"
    name = f"{args.logix_tag}[0]" if c["elems"] == 1 else f"{args.logix_tag}&elem_count={c['elems']}"
    return f"{attrs}&name={name}"


def wait_port(port, proc):
    for _ in range(100):
        if proc.poll() is not None:
            sys.exit(f"server exited early: {proc.args}")
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.1).close()
            return
        except OSError:
            time.sleep(0.05)
    sys.exit(f"server never listened on {port}")


def start_server(protocol, delay, server_dir):
    if protocol == "logix":
        cmd = [str(server_dir / "ab_server"), "--plc=ControlLogix", "--path=1,0", f"--port={LOGIX_PORT}",
               "--tag=TestDINTArray:DINT[2000]"]
        if delay:
            cmd.append(f"--delay={delay}")
        port = LOGIX_PORT
    else:
        cmd = [str(server_dir / "modbus_server"), f"--listen=127.0.0.1:{MODBUS_PORT}"]
        port = MODBUS_PORT
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wait_port(port, proc)
    return proc


def cpu_seconds(pid):
    """ps TIME is [[dd-]hh:]mm:ss.ss."""
    return cpu_seconds_text(subprocess.run(["ps", "-o", "time=", "-p", str(pid)],
                                           capture_output=True, text=True).stdout.strip())


def cpu_seconds_text(text):
    days, _, text = text.rpartition("-")
    total = 0.0
    for part in text.split(":"):
        total = total * 60 + float(part)
    return total + float(days or 0) * 86400


def run_one(binary, c, args, server):
    cmd = [binary, f"--mode={c['mode']}", f"--op={c['op']}", "--groups=1", f"--threads={c['threads']}",
           f"--tags={c['tags']}", f"--duration={args.duration}", f"--tag={tag_string(c, args)}"]
    cpu0, t0 = (cpu_seconds(server.pid), time.monotonic()) if server else (0, 0)
    res = subprocess.run(cmd, capture_output=True, text=True)
    server_pct = 100.0 * (cpu_seconds(server.pid) - cpu0) / (time.monotonic() - t0) if server else 0.0
    lines = [l for l in res.stdout.splitlines() if l.startswith(c["mode"] + ",")]
    if res.returncode != 0 or not lines:
        print(f"  FAILED rc={res.returncode}: {' '.join(cmd)}\n{res.stderr[-2000:]}", file=sys.stderr)
        return None
    return lines[-1].split(","), server_pct


HARNESS_COLUMNS = ("mode,connection_groups,threads,tags,tags_per_thread,duration_ms,total_ops,ops_per_sec,"
                   "cpu_load_pct,fairness_cv,fairness_min_max_ratio,op,errors,p50_us,p95_us,p99_us,max_us,"
                   "cpu_us_per_op,rss_growth_kb,create_ms,prime_ms").split(",")


def verdict(a, b, higher_better):
    """Return (change, word).  change is (median B - median A) / median A."""
    ma, mb = statistics.median(a), statistics.median(b)
    change = (mb - ma) / ma if ma else 0.0
    if abs(change) <= THRESHOLD or (min(b) <= max(a) and min(a) <= max(b)):
        return change, "same"
    return change, "better" if (change > 0) == higher_better else "WORSE"


def self_check():
    noise_a, noise_b = [100, 104, 97, 101, 99], [102, 98, 105, 100, 96]
    assert verdict(noise_a, noise_b, True)[1] == "same"
    drop = [90, 92, 89, 91, 90]
    assert verdict(noise_a, drop, True)[1] == "WORSE"
    assert verdict(noise_a, drop, False)[1] == "better"
    assert verdict([100, 100], [107, 130], True)[1] == "better"
    assert verdict([100, 106], [104, 110], True)[1] == "same"   # 6 % but overlapping
    assert abs(cpu_seconds_text("1-02:03:04.5") - (86400 + 7384.5)) < 1e-9
    print("self-check ok")


def summarize(results):
    print(f"\n{'configuration':38} {'metric':11} {'median A':>10} {'median B':>10} {'change':>8}  verdict"
          f"   p95 A/B us      srv% A/B")
    for key, runs in results.items():
        a = [r for r in runs if r["side"] == "A"]
        b = [r for r in runs if r["side"] == "B"]
        if len(a) < 2 or len(b) < 2:
            print(f"{key:38} too few successful runs")
            continue
        srv = statistics.median(r["server_cpu_pct"] for r in runs)
        metric, higher = ("cpu_us_per_op", False) if srv >= SERVER_BOUND_PCT else ("ops_per_sec", True)
        va, vb = [float(r[metric]) for r in a], [float(r[metric]) for r in b]
        change, word = verdict(va, vb, higher)
        p95 = lambda side: statistics.median(float(r["p95_us"]) for r in side)
        sp = lambda side: statistics.median(r["server_cpu_pct"] for r in side)
        errs = sum(int(r["errors"]) for r in runs)
        print(f"{key:38} {metric:11} {statistics.median(va):10.1f} {statistics.median(vb):10.1f} "
              f"{change * 100:+7.1f}%  {word:7}  {p95(a):6.0f}/{p95(b):<6.0f}  {sp(a):4.0f}/{sp(b):<4.0f}"
              + (f"  {errs} errors" if errs else ""))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--a")
    p.add_argument("--b")
    p.add_argument("--server-dir", type=Path)
    p.add_argument("--rounds", type=int, default=5)
    p.add_argument("--duration", type=int, default=10)
    p.add_argument("--out", default="perf_compare.csv")
    p.add_argument("--logix-attrs", help="hardware Logix attributes (without name); skips servers and Modbus")
    p.add_argument("--logix-tag", default="TestDINTArray")
    p.add_argument("--only", help="run only configurations whose key contains this text")
    p.add_argument("--self-check", action="store_true")
    args = p.parse_args()

    if args.self_check:
        self_check()
        return
    hardware = bool(args.logix_attrs)
    if not args.a or not args.b or (not hardware and not args.server_dir):
        p.error("--a, --b and --server-dir (or --logix-attrs) are required")

    configs = [c for c in build_matrix(hardware) if not args.only or args.only in config_key(c)]
    total_s = len(configs) * 2 * args.rounds * (args.duration + 1)
    print(f"{len(configs)} configurations, {args.rounds} rounds, about {total_s // 60} min", flush=True)

    results = {}
    with open(args.out, "w", newline="") as f:
        out = csv.writer(f)
        out.writerow(["# A=" + args.a, "B=" + args.b])
        out.writerow(["side", "round", "protocol", "delay_ms", "elems", "server_cpu_pct"] + HARNESS_COLUMNS)
        for (protocol, delay), group in itertools.groupby(configs, lambda c: (c["protocol"], c["delay"])):
            server = None if hardware else start_server(protocol, delay, args.server_dir)
            try:
                for c in group:
                    key = config_key(c)
                    print(key, end="", flush=True)
                    for rnd in range(args.rounds):
                        order = [("A", args.a), ("B", args.b)]
                        for side, binary in (order if rnd % 2 == 0 else order[::-1]):
                            got = run_one(binary, c, args, server)
                            if got is None:
                                continue
                            cols, srv = got
                            out.writerow([side, rnd, protocol, delay, c["elems"], f"{srv:.1f}"] + cols)
                            f.flush()
                            row = dict(zip(HARNESS_COLUMNS, cols), side=side, server_cpu_pct=srv)
                            results.setdefault(key, []).append(row)
                        print(".", end="", flush=True)
                    print()
            finally:
                if server:
                    server.terminate()
                    server.wait()
    summarize(results)


if __name__ == "__main__":
    main()

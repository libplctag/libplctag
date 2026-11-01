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
      [--rounds=5] [--duration=10] [--poll-ms=1] [--interleave] [--out=perf_compare.csv]
  Hardware (Logix only, no server, delay 0 only):
  run_perf_compare.py --a=... --b=... \\
      --logix-attrs="protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix" \\
      --logix-tag=TestBigArray
  Thread sweep (1-8 and 12 threads, 100 tags per thread, one shared connection vs
  one connection per thread) instead of the default set.  Logix runs as a
  ControlLogix, which packs requests with Multiple Service Packet (0x0A):
  run_perf_compare.py --a=... --b=... --server-dir=... --thread-sweep=modbus|logix
      [--threads=1,2,4] [--tags-per-thread=1,10,100,1000] [--delay-ms=0.3]
  ab_server logs the mean delay it actually applied; it is written to --out as a
  "# server delay" line after each server's runs.
  Fairness matrix (10000 ControlLogix tags, 1000 Micro800 and Modbus tags, which do not
  pack and so need far longer for the same windows; 1, 2, 4, 8 threads x 1, 2, 3, 4, 8 connections, plus
  1 x 16; tags spread round-robin over the connections; sync and async, read and
  write; ControlLogix, Micro800 and Modbus).  Each CIP connection reads its own
  server array, so ab_server's per-tag mutex does not mix into the result:
  run_perf_compare.py --a=... --b=... --server-dir=... --fairness-matrix [--delay-ms=1]
  run_perf_compare.py --self-check
"""

import argparse
import csv
import itertools
import re
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

THRESHOLD = 0.05
SERVER_BOUND_PCT = 90.0
LOGIX_PORT = 45100
MODBUS_PORT = 45102
SWEEP_THREADS = (1, 2, 3, 4, 5, 6, 7, 8, 12)
MATRIX_TAGS = {"logix": 10000, "micro800": 1000, "modbus": 1000}
# Micro800 and Modbus at 1 ms on one connection: a 1000-tag pass is about 1.3 s, so a 4-pass
# window is about 6 s and two windows need about 12 s.
MATRIX_DURATION = {"logix": 10, "micro800": 15, "modbus": 15}
MATRIX_CELLS = [(t, c) for t in (1, 2, 4, 8) for c in (1, 2, 3, 4, 8)] + [(1, 16)]
PER_CONN_TAGS = 16
DELAY_LINE = re.compile(r"Response delay: requested (\d+) us, applied mean (\d+) us over (\d+) responses")


def build_matrix(hardware):
    """The S4 preliminary set."""
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


def thread_sweep_matrix(protocol, delay_ms=0, thread_counts=SWEEP_THREADS, tags_per_thread=(100,)):
    """groups=1 shares one connection; groups=threads gives each thread its own."""
    configs = []
    for threads, per_thread_tags, per_thread, mode, op in itertools.product(
            thread_counts, tags_per_thread, [False, True], ["sync", "async"], ["read", "write"]):
        if per_thread and threads == 1:
            continue
        configs.append(dict(protocol=protocol, delay=delay_ms, tags=per_thread_tags * threads, threads=threads,
                            groups=threads if per_thread else 1, elems=1, mode=mode, op=op))
    return configs


def fairness_matrix(delay_ms=1.0):
    """Threads x connections at a fixed tag count, every server at the same delay."""
    configs = []
    for protocol, (threads, conns), mode, op in itertools.product(
            ["logix", "micro800", "modbus"], MATRIX_CELLS, ["sync", "async"], ["read", "write"]):
        configs.append(dict(protocol=protocol, delay=delay_ms, tags=MATRIX_TAGS[protocol], duration=MATRIX_DURATION[protocol],
                            threads=threads, groups=conns, group_by="tag", per_conn_tag=True, elems=1,
                            mode=mode, op=op))
    return configs


def int_list(text):
    return tuple(int(x) for x in text.split(","))


def config_key(c):
    return (f"{c['protocol']} d{c['delay']:g}ms {c['mode']} {c['op']} {c['tags']}x{c['threads']}"
            f" g{c.get('groups', 1)}{'t' if c.get('group_by') == 'tag' else ''} e{c['elems']}")


def tag_strings(c, args):
    """One base per connection group when per_conn_tag is set (perf_benchmark: group g uses base g-1)."""
    if c["protocol"] == "modbus":
        return [f"protocol=modbus-tcp&gateway=127.0.0.1:{MODBUS_PORT}&path=1&name=hr0"]
    if c["protocol"] == "micro800":
        attrs = f"protocol=ab-eip&gateway=127.0.0.1:{LOGIX_PORT}&plc=Micro800"
    else:
        attrs = args.logix_attrs or f"protocol=ab-eip&gateway=127.0.0.1:{LOGIX_PORT}&path=1,0&plc=ControlLogix"
    if c.get("per_conn_tag"):
        return [f"{attrs}&name=perfTag{g}[0]" for g in range(1, c["groups"] + 1)]
    name = f"{args.logix_tag}[0]" if c["elems"] == 1 else f"{args.logix_tag}&elem_count={c['elems']}"
    return [f"{attrs}&name={name}"]


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


def start_server(protocol, delay_ms, server_dir, log):
    if protocol in ("logix", "micro800"):
        cmd = [str(server_dir / "ab_server"), f"--port={LOGIX_PORT}", "--tag=TestDINTArray:DINT[2000]"]
        cmd += ["--plc=ControlLogix", "--path=1,0"] if protocol == "logix" else ["--plc=Micro800"]
        cmd += [f"--tag=perfTag{g}:DINT[10]" for g in range(1, PER_CONN_TAGS + 1)]
        if delay_ms:
            cmd.append(f"--delay-ms={delay_ms:g}")
        port = LOGIX_PORT
    else:
        cmd = [str(server_dir / "modbus_server"), f"--listen=127.0.0.1:{MODBUS_PORT}"]
        if delay_ms:
            cmd.append(f"--delay-ms={delay_ms:g}")
        port = MODBUS_PORT
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=log)
    wait_port(port, proc)
    return proc


def stop_server(proc, log):
    """SIGINT so ab_server exits cleanly and logs the delay it applied; returns that line or None."""
    proc.send_signal(signal.SIGINT)
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    log.seek(0)
    found = DELAY_LINE.findall(log.read().decode(errors="replace"))
    return found[-1] if found else None


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
    cmd = [binary, f"--mode={c['mode']}", f"--op={c['op']}", f"--groups={c.get('groups', 1)}", f"--threads={c['threads']}",
           f"--tags={c['tags']}", f"--duration={c.get('duration', args.duration)}", f"--poll-ms={args.poll_ms}"]
    cmd += [f"--tag={t}" for t in tag_strings(c, args)]
    cmd += ["--interleave"] if args.interleave else []
    cmd += [f"--group-by={c['group_by']}"] if c.get("group_by") else []
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
                   "cpu_us_per_op,rss_growth_kb,create_ms,prime_ms,window_min_max,window_cv,conn_window_min_max,"
                   "tag_cv,tag_std_dev,window_s").split(",")


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
    assert DELAY_LINE.findall("x Response delay: requested 300 us, applied mean 412 us over 9 responses.")[-1] == ("300", "412", "9")
    assert len(thread_sweep_matrix("logix", 0.3, (1, 2), (1, 10))) == 2 * 2 * (1 + 2) * 2
    assert config_key(thread_sweep_matrix("logix", 0.3, (1,), (1,))[0]).startswith("logix d0.3ms ")
    m = fairness_matrix()
    assert len(m) == 3 * 21 * 2 * 2 and max(c["groups"] for c in m) <= PER_CONN_TAGS
    class A: logix_attrs = None; logix_tag = "TestDINTArray"
    assert tag_strings(dict(protocol="micro800", groups=3, per_conn_tag=True, elems=1), A)[2].endswith("plc=Micro800&name=perfTag3[0]")
    print("self-check ok")


def summarize(results):
    print(f"\n{'configuration':41} {'metric':11} {'median A':>10} {'median B':>10} {'change':>8}  verdict"
          f"   p95 A/B us      srv% A/B   win min/max A/B  tag CV% A/B")
    for key, runs in results.items():
        a = [r for r in runs if r["side"] == "A"]
        b = [r for r in runs if r["side"] == "B"]
        if len(a) < 2 or len(b) < 2:
            print(f"{key:41} too few successful runs")
            continue
        srv = statistics.median(r["server_cpu_pct"] for r in runs)
        metric, higher = ("cpu_us_per_op", False) if srv >= SERVER_BOUND_PCT else ("ops_per_sec", True)
        va, vb = [float(r[metric]) for r in a], [float(r[metric]) for r in b]
        change, word = verdict(va, vb, higher)
        p95 = lambda side: statistics.median(float(r["p95_us"]) for r in side)
        sp = lambda side: statistics.median(r["server_cpu_pct"] for r in side)
        errs = sum(int(r["errors"]) for r in runs)
        win = lambda side: min(float(r["window_min_max"]) for r in side)
        tag_cv = lambda side: max(float(r["tag_cv"]) for r in side)
        print(f"{key:41} {metric:11} {statistics.median(va):10.1f} {statistics.median(vb):10.1f} "
              f"{change * 100:+7.1f}%  {word:7}  {p95(a):6.0f}/{p95(b):<6.0f}  {sp(a):4.0f}/{sp(b):<4.0f}"
              f"   {win(a):.3f}/{win(b):<.3f}      {tag_cv(a):.2f}/{tag_cv(b):<.2f}"
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
    p.add_argument("--thread-sweep", choices=["modbus", "logix", "micro800"], help="run the thread sweep instead")
    p.add_argument("--fairness-matrix", action="store_true", help="run the step 3A fairness matrix instead")
    p.add_argument("--poll-ms", type=int, default=1, help="async: sleep this long between status passes; 0 yields in a loop")
    p.add_argument("--delay-ms", type=float, default=0.0,
                   help="thread sweep and fairness matrix: server response delay in ms, fractions allowed")
    p.add_argument("--threads", type=int_list, default=SWEEP_THREADS, help="thread sweep: thread counts, comma separated")
    p.add_argument("--tags-per-thread", type=int_list, default=(100,), help="thread sweep: tags per thread, comma separated")
    p.add_argument("--interleave", action="store_true", help="create tags round-robin across threads")
    p.add_argument("--self-check", action="store_true")
    args = p.parse_args()

    if args.self_check:
        self_check()
        return
    hardware = bool(args.logix_attrs)
    if not args.a or not args.b or (not hardware and not args.server_dir):
        p.error("--a, --b and --server-dir (or --logix-attrs) are required")

    if args.fairness_matrix:
        matrix = fairness_matrix(args.delay_ms or 1.0)
    elif args.thread_sweep:
        matrix = thread_sweep_matrix(args.thread_sweep, args.delay_ms, args.threads, args.tags_per_thread)
    else:
        matrix = build_matrix(hardware)
    configs = [c for c in matrix if not args.only or args.only in config_key(c)]
    # ponytail: + 2 s per run for create, prime and destroy; a guess, not measured per protocol.
    total_s = sum(2 * args.rounds * (c.get("duration", args.duration) + 2) for c in configs)
    print(f"{len(configs)} configurations, {args.rounds} rounds, about {total_s // 60} min", flush=True)

    results = {}
    with open(args.out, "w", newline="") as f:
        out = csv.writer(f)
        out.writerow(["# A=" + args.a, "B=" + args.b])
        out.writerow(["side", "round", "protocol", "delay_ms", "elems", "server_cpu_pct"] + HARNESS_COLUMNS)
        for (protocol, delay), group in itertools.groupby(configs, lambda c: (c["protocol"], c["delay"])):
            log = tempfile.TemporaryFile()
            server = None if hardware else start_server(protocol, delay, args.server_dir, log)
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
                applied = stop_server(server, log) if server else None
                log.close()
                if applied:
                    line = f"# server delay: requested {applied[0]} us, applied mean {applied[1]} us over {applied[2]} responses"
                    f.write(line + "\n")
                    print(line)
    summarize(results)


if __name__ == "__main__":
    main()

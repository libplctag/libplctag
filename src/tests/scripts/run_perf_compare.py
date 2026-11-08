#!/usr/bin/env python3
"""Compare two perf_benchmark builds (A = baseline, B = candidate).

Both binaries are the same harness source linked against different
libraries; one set of servers serves both.  Each configuration runs
A B B A ... for --rounds rounds so drift hits both sides equally.  Writes
every raw run to --out and prints one verdict line per configuration.

A difference counts only if the medians differ by more than 5 % and the
min-max ranges do not overlap.  Each configuration is judged on ops/sec,
client CPU per op and client peak memory; B is WORSE if it loses on any
one of them.  When the server uses most of a core the run is server-bound,
so its ops/sec does not count.

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
  Scale matrix (10, 100, 1000 tags x 1, 2, 4, 8 threads x 1, 2, 4, 8 connections; tags
  spread round-robin over the connections; sync and async, read and write):
  run_perf_compare.py --a=... --b=... --server-dir=... --scale-matrix=logix|modbus|micro800 [--delay-ms=1]
  Mutex wait (Linux only; --a and --b must link the shared library, not plctag_static):
  run_perf_compare.py ... --mutex-shim=b_head/bin_dist/libmutex_wait_shim.so
  run_perf_compare.py --self-check

CPU over each client's measured window: server process, machine total (all cores, so up to
cores x 100 %), and the busiest client and server thread.  "limit" names what ran out first:
machine, server or client (busiest thread at 90 % of a core or more).  Linux reads /proc,
macOS uses ps.
"""

import argparse
import csv
import itertools
import os
import re
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

THRESHOLD = 0.05
SERVER_BOUND_PCT = 90.0
# B must beat or match A on all three client measures (docs/performance.md): (column, higher is
# better, divisor for display).  peak_rss_kb is shown in MB.
VERDICT_METRICS = (("ops_per_sec", True, 1), ("cpu_us_per_op", False, 1), ("peak_rss_kb", False, 1024))
LOGIX_PORT = 45100
MODBUS_PORT = 45102
SWEEP_THREADS = (1, 2, 3, 4, 5, 6, 7, 8, 12)
MATRIX_TAGS = {"logix": 10000, "micro800": 1000, "modbus": 1000}
# Micro800 and Modbus at 1 ms on one connection: a 1000-tag pass is about 1.3 s, so a 4-pass
# window is about 6 s and two windows need about 12 s.
MATRIX_DURATION = {"logix": 10, "micro800": 15, "modbus": 15}
MATRIX_CELLS = [(t, c) for t in (1, 2, 4, 8) for c in (1, 2, 3, 4, 8)] + [(1, 16)]
PER_CONN_TAGS = 16
SCALE_TAGS = (10, 100, 1000)
SCALE_THREADS = (1, 2, 4, 8)
SCALE_CONNS = (1, 2, 4, 8)
# Release matrix.  One ControlLogix packet holds about 180 of the harness's tags, so 200 and
# 250 tags on one connection need a second packet.  Threads double up to 4 x the CPU count.
RELEASE_TAGS = (1, 50, 100, 150, 200, 250)
RELEASE_CONNS = (1, 2, 3, 4, 5)
RELEASE_THREADS = tuple(sorted({min(1 << i, 4 * (os.cpu_count() or 1)) for i in range(16)}))
# Auto-sync CPU sweep: where the client stops keeping up with auto_sync_read_ms.
AUTO_TAGS = (1000, 2500, 5000, 10000)
AUTO_MS = (1, 2, 5, 10, 20, 50, 100)
AUTO_CONNS = (1, 5)
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


def scale_matrix(protocol, delay_ms=1.0):
    """Tags x threads x connections; connections are independent of threads."""
    configs = []
    for tags, threads, conns, mode, op in itertools.product(
            SCALE_TAGS, SCALE_THREADS, SCALE_CONNS, ["sync", "async"], ["read", "write"]):
        if conns > tags or threads > tags:
            continue
        configs.append(dict(protocol=protocol, delay=delay_ms, tags=tags, threads=threads, groups=conns,
                            group_by="tag", per_conn_tag=True, elems=1, mode=mode, op=op))
    return configs


def release_matrix(protocol, delay_ms=0.3):
    """Connections x threads x tags, sync and async, read.  Skips threads or connections with no tag."""
    configs = []
    for tags, threads, conns, mode in itertools.product(RELEASE_TAGS, RELEASE_THREADS, RELEASE_CONNS, ["sync", "async"]):
        if conns > tags or threads > tags:
            continue
        configs.append(dict(protocol=protocol, delay=delay_ms, tags=tags, threads=threads, groups=conns,
                            group_by="tag", per_conn_tag=True, elems=1, mode=mode, op="read"))
    return configs


def auto_matrix(protocol, delay_ms=0.3):
    """Auto-sync read: tags x period x connections, for client CPU against the period."""
    return [dict(protocol=protocol, delay=delay_ms, tags=tags, threads=1, groups=conns, group_by="tag",
                 per_conn_tag=True, elems=1, mode="auto", op="read", auto_ms=period)
            for tags, period, conns in itertools.product(AUTO_TAGS, AUTO_MS, AUTO_CONNS)]


def int_list(text):
    return tuple(int(x) for x in text.split(","))


def config_key(c):
    return (f"{c['protocol']} d{c['delay']:g}ms {c['mode']} {c['op']} {c['tags']}x{c['threads']}"
            f" g{c.get('groups', 1)}{'t' if c.get('group_by') == 'tag' else ''} e{c['elems']}"
            + (f" p{c['auto_ms']}ms" if c.get("auto_ms") else ""))


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


CLK_TCK = os.sysconf("SC_CLK_TCK") if hasattr(os, "sysconf") else 100
CORES = os.cpu_count() or 1
PS_TIME = re.compile(r"^(?:\d+-)?(?:\d+:)*\d+\.\d+$")


def stat_seconds(text):
    """utime + stime from a Linux /proc stat line.  The command name may hold spaces."""
    fields = text.rpartition(")")[2].split()
    return (int(fields[11]) + int(fields[12])) / CLK_TCK


def cpu_seconds(pid):
    """Linux /proc has 10 ms resolution; Linux ps TIME has 1 s.  macOS ps TIME is [[dd-]hh:]mm:ss.ss."""
    try:
        return stat_seconds(Path(f"/proc/{pid}/stat").read_text())
    except OSError:
        return cpu_seconds_text(subprocess.run(["ps", "-o", "time=", "-p", str(pid)],
                                               capture_output=True, text=True).stdout.strip())


def thread_seconds(pid):
    """CPU seconds per thread of pid."""
    task = Path(f"/proc/{pid}/task")
    if task.is_dir():
        out = {}
        for t in task.iterdir():
            try:
                out[t.name] = stat_seconds((t / "stat").read_text())
            except OSError:
                pass  # the thread exited
        return out
    # macOS: one line per thread, the first two m:ss.ss fields are STIME and UTIME.
    # shortcut: keyed by line order since ps -M shows no thread id; fine while threads are not
    # created or destroyed during the window, which perf_benchmark does not do.
    out = {}
    lines = subprocess.run(["ps", "-M", "-p", str(pid)], capture_output=True, text=True).stdout.splitlines()[1:]
    for i, line in enumerate(lines):
        times = [w for w in line.split() if PS_TIME.match(w)][:2]
        if len(times) == 2:
            out[i] = sum(cpu_seconds_text(w) for w in times)
    return out


def machine_seconds():
    """Busy CPU seconds summed over all cores since boot (Linux) or over all live processes (macOS)."""
    try:
        with open("/proc/stat") as f:
            ticks = [int(x) for x in f.readline().split()[1:9]]
        return (sum(ticks) - ticks[3] - ticks[4]) / CLK_TCK  # minus idle and iowait
    except OSError:
        out = subprocess.run(["ps", "-A", "-o", "time="], capture_output=True, text=True).stdout.split()
        return sum(cpu_seconds_text(w) for w in out)


def busiest_thread_pct(before, after, seconds):
    if seconds <= 0:
        return 0.0
    return max((100.0 * (v - before.get(k, 0.0)) / seconds for k, v in after.items()), default=0.0)


def limit(machine_pct, client_thread_pct, server_thread_pct):
    """What ran out first: the whole machine, one server thread or one client thread."""
    if machine_pct >= SERVER_BOUND_PCT * CORES:
        return "machine"
    if server_thread_pct >= SERVER_BOUND_PCT:
        return "server"
    if client_thread_pct >= SERVER_BOUND_PCT:
        return "client"
    return ""


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
    cmd += [f"--auto-ms={c['auto_ms']}"] if c.get("auto_ms") else []
    env = dict(os.environ, LD_PRELOAD=str(args.mutex_shim)) if args.mutex_shim else None
    # Server CPU over the client's measured window (its "# window" lines), so create and prime
    # are not counted.  A client without the markers falls back to its whole run.
    def sample(client=None):
        return dict(t=time.monotonic(), srv=cpu_seconds(server.pid) if server else 0.0,
                    srv_threads=thread_seconds(server.pid) if server else {},
                    cli_threads=thread_seconds(client.pid) if client else {}, machine=machine_seconds())
    start = end = None
    # Client threads, and the server threads serving them, exit right after "# window end", so
    # the per-thread end sample is taken 0.5 s early.
    thr_end = {}
    early = lambda: thr_end.update(t=time.monotonic(), cli_threads=thread_seconds(proc.pid),
                                   srv_threads=thread_seconds(server.pid) if server else {})
    timer = threading.Timer(max(c.get("duration", args.duration) - 0.5, 0.1), early)
    whole = sample()
    with tempfile.TemporaryFile(mode="w+") as err:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=err, text=True, env=env)
        sample = lambda s=sample: s(proc)
        stdout = []
        for line in proc.stdout:
            if line.startswith("# window start"):
                start = sample()
                timer.start()
            elif line.startswith("# window end"):
                end = sample()
            stdout.append(line)
        proc.wait()
        timer.cancel()
        err.seek(0)
        stderr = err.read()
    if not start or not end:
        start, end = whole, sample()
    dt = end["t"] - start["t"]
    server_pct = 100.0 * (end["srv"] - start["srv"]) / dt if server and dt > 0 else 0.0
    machine_pct = 100.0 * (end["machine"] - start["machine"]) / dt if dt > 0 else 0.0
    thr = thr_end if thr_end else end
    cli_thread = busiest_thread_pct(start["cli_threads"], thr["cli_threads"], thr["t"] - start["t"])
    srv_thread = busiest_thread_pct(start["srv_threads"], thr["srv_threads"], thr["t"] - start["t"])
    cpu = dict(machine_cpu_pct=machine_pct, client_max_thread_pct=cli_thread, server_max_thread_pct=srv_thread,
               limit=limit(machine_pct, cli_thread, srv_thread))
    lines = [l.rstrip("\n") for l in stdout if l.startswith(c["mode"] + ",")]
    if proc.returncode != 0 or not lines:
        print(f"  FAILED rc={proc.returncode}: {' '.join(cmd)}\n{stderr[-2000:]}", file=sys.stderr)
        return None
    return lines[-1].split(","), server_pct, server_rss_kb(server.pid) if server else 0, cpu


def server_rss_kb(pid):
    out = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    return int(out) if out else 0


CPU_COLUMNS = ("machine_cpu_pct", "client_max_thread_pct", "server_max_thread_pct", "limit")
HARNESS_COLUMNS = ("mode,connection_groups,threads,tags,tags_per_thread,duration_ms,total_ops,ops_per_sec,"
                   "cpu_load_pct,fairness_cv,fairness_min_max_ratio,op,errors,p50_us,p95_us,p99_us,max_us,"
                   "cpu_us_per_op,rss_growth_kb,create_ms,prime_ms,window_min_max,window_cv,conn_window_min_max,"
                   "tag_cv,tag_std_dev,window_s,mutex_waits_per_op,mutex_wait_us_per_op,peak_rss_kb").split(",")


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
    assert overall(["better", "better", "WORSE"]) == "WORSE"
    assert overall(["better", "same", "same"]) == "better"
    assert overall(["same", "same", "same"]) == "same"
    assert abs(cpu_seconds_text("1-02:03:04.5") - (86400 + 7384.5)) < 1e-9
    assert stat_seconds("12 (a b) S " + " ".join(["0"] * 10) + f" {3 * CLK_TCK} {CLK_TCK} 0") == 4.0
    assert busiest_thread_pct({"1": 1.0, "2": 0.0}, {"1": 1.5, "2": 0.9, "3": 0.2}, 1.0) == 90.0
    assert limit(SERVER_BOUND_PCT * CORES, 0, 0) == "machine" and limit(10, 95, 99) == "server"
    assert limit(10, 95, 10) == "client" and limit(10, 10, 10) == ""
    assert PS_TIME.match("0:00.01") and not PS_TIME.match("31T")
    assert DELAY_LINE.findall("x Response delay: requested 300 us, applied mean 412 us over 9 responses.")[-1] == ("300", "412", "9")
    assert len(thread_sweep_matrix("logix", 0.3, (1, 2), (1, 10))) == 2 * 2 * (1 + 2) * 2
    assert config_key(thread_sweep_matrix("logix", 0.3, (1,), (1,))[0]).startswith("logix d0.3ms ")
    m = fairness_matrix()
    assert len(m) == 3 * 21 * 2 * 2 and max(c["groups"] for c in m) <= PER_CONN_TAGS
    class A: logix_attrs = None; logix_tag = "TestDINTArray"
    assert tag_strings(dict(protocol="micro800", groups=3, per_conn_tag=True, elems=1), A)[2].endswith("plc=Micro800&name=perfTag3[0]")
    s = scale_matrix("logix")
    assert len(s) == 3 * 4 * 4 * 2 * 2 and max(c["groups"] for c in s) <= PER_CONN_TAGS
    r = release_matrix("logix")
    assert len(r) == 2 * (1 + 5 * len(RELEASE_THREADS) * 5) and max(c["groups"] for c in r) <= PER_CONN_TAGS
    assert config_key(auto_matrix("logix")[0]).endswith(" p1ms")
    rows = [dict(protocol="logix", delay=0.3, mode="sync", op="read", tags=100, threads=1, groups=g, auto_ms="", side=s,
                 runs=3, ops_per_sec=v, ops_per_tag_s=v / 100, cpu_load_pct=10.0, peak_rss_kb=1000.0, p50_us=1.0, p95_us=2.0,
                 server_cpu_pct=1.0, server_rss_kb=1.0)
            for g, s, v in ((1, "A", 1000.0), (1, "B", 2000.0), (2, "A", 1500.0))]
    with tempfile.TemporaryDirectory() as d:
        write_summary(rows, Path(d) / "summary.csv")
        lines = (Path(d) / "summary_ops_per_tag_s.csv").read_text().splitlines()
        assert lines[0].endswith("A g1,A g2,B g1,B g2,B/A g1,B/A g2") and lines[1].endswith("10.0,15.0,20.0,,2.00,"), lines
    print("self-check ok")


SUMMARY_FIELDS = ("protocol", "delay", "mode", "op", "tags", "threads", "groups", "auto_ms")
SUMMARY_METRICS = ("ops_per_sec", "ops_per_tag_s", "cpu_load_pct", "peak_rss_kb", "p50_us", "p95_us",
                   "server_cpu_pct", "server_rss_kb")


def summary_rows(results, configs):
    """Medians per configuration and side, with ops per tag per second."""
    rows = []
    for key, runs in results.items():
        c = configs[key]
        for side in ("A", "B"):
            got = [r for r in runs if r["side"] == side]
            if not got:
                continue
            med = lambda col: statistics.median(float(r[col]) for r in got)
            row = {f: c.get(f, "") for f in SUMMARY_FIELDS}
            row.update(side=side, runs=len(got), ops_per_sec=med("ops_per_sec"), ops_per_tag_s=med("ops_per_sec") / c["tags"],
                       cpu_load_pct=med("cpu_load_pct"), peak_rss_kb=med("peak_rss_kb"), p50_us=med("p50_us"),
                       p95_us=med("p95_us"), server_cpu_pct=med("server_cpu_pct"), server_rss_kb=med("server_rss_kb"))
            rows.append(row)
    return rows


def write_summary(rows, path):
    """summary.csv (one row per configuration and side), then one matrix per client metric: a row per
    configuration without its connection count, columns connections x side, then B/A."""
    path = Path(path)
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=("side", "runs") + SUMMARY_FIELDS + SUMMARY_METRICS)
        w.writeheader()
        for r in rows:
            w.writerow({k: (f"{v:.2f}" if isinstance(v, float) else v) for k, v in r.items()})
    conns = sorted({r["groups"] for r in rows})
    row_fields = [f for f in SUMMARY_FIELDS if f != "groups"]
    by_row = {}
    for r in rows:
        by_row.setdefault(tuple(r[f] for f in row_fields), {})[(r["groups"], r["side"])] = r
    for metric in ("ops_per_tag_s", "cpu_load_pct", "peak_rss_kb"):
        with open(path.with_name(f"{path.stem}_{metric}.csv"), "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(row_fields + [f"{side} g{g}" for side in ("A", "B") for g in conns] + [f"B/A g{g}" for g in conns])
            for rk in sorted(by_row, key=lambda k: tuple(str(x).zfill(8) for x in k)):
                cell = by_row[rk]
                val = lambda g, side: cell[(g, side)][metric] if (g, side) in cell else None
                vals = [f"{val(g, side):.1f}" if val(g, side) is not None else "" for side in ("A", "B") for g in conns]
                ratios = [f"{val(g, 'B') / val(g, 'A'):.2f}" if val(g, "A") and val(g, "B") is not None else "" for g in conns]
                w.writerow(list(rk) + vals + ratios)


def results_from_csv(path):
    """Rebuild summarize()'s input from an --out file."""
    results = {}
    with open(path, newline="") as f:
        rows = csv.reader(l for l in f if not l.startswith("#"))
        head = next(rows)
        for cols in rows:
            r = dict(zip(head, cols))
            r["server_cpu_pct"] = float(r["server_cpu_pct"])
            # ponytail: the CSV does not record group_by, so the key has no "t" suffix.
            c = dict(protocol=r["protocol"], delay=float(r["delay_ms"]), mode=r["mode"], op=r["op"], tags=r["tags"],
                     threads=r["threads"], groups=r["connection_groups"], elems=r["elems"])
            results.setdefault(config_key(c), []).append(r)
    return results


def overall(words):
    """B must win or tie on every measure: one WORSE makes the configuration WORSE."""
    if "WORSE" in words:
        return "WORSE"
    return "better" if "better" in words else "same"


def summarize(results):
    print(f"\n{'configuration':41} {'verdict':7}  {'ops/s A':>10} {'ops/s B':>10} {'change':>7} {'':6}"
          f"  {'us/op A':>8} {'us/op B':>8} {'change':>7} {'':6}  {'MB A':>6} {'MB B':>6} {'change':>7} {'':6}"
          f"  p95 A/B us      srv% A/B   win min/max A/B  tag CV% A/B   cli% A/B    srv MB A/B   mutex us/op A/B"
          f"  mach% A/B   cli thr% A/B  srv thr% A/B  limit A/B")
    for key, runs in results.items():
        a = [r for r in runs if r["side"] == "A"]
        b = [r for r in runs if r["side"] == "B"]
        if len(a) < 2 or len(b) < 2:
            print(f"{key:41} too few successful runs")
            continue
        srv = statistics.median(r["server_cpu_pct"] for r in runs)
        cols, words = [], []
        for metric, higher, scale in VERDICT_METRICS:
            va, vb = [float(r[metric]) / scale for r in a], [float(r[metric]) / scale for r in b]
            change, word = verdict(va, vb, higher)
            # A server-bound run's throughput measures the server, so it does not count.
            if metric == "ops_per_sec" and srv >= SERVER_BOUND_PCT:
                word = "srv"
            else:
                words.append(word)
            cols.append(f"{statistics.median(va):{10 if higher else 8}.1f} {statistics.median(vb):{10 if higher else 8}.1f}"
                        f" {change * 100:+6.1f}% {word:6}")
        p95 = lambda side: statistics.median(float(r["p95_us"]) for r in side)
        sp = lambda side: statistics.median(r["server_cpu_pct"] for r in side)
        errs = sum(int(r["errors"]) for r in runs)
        win = lambda side: min(float(r["window_min_max"]) for r in side)
        tag_cv = lambda side: max(float(r["tag_cv"]) for r in side)
        cli_pct = lambda side: statistics.median(float(r["cpu_load_pct"]) for r in side)
        mb = lambda side, col: statistics.median(float(r[col]) for r in side) / 1024
        # -1 when the run had no --mutex-shim.
        mutex = lambda side: statistics.median(float(r["mutex_wait_us_per_op"]) for r in side)
        # CSVs from before these columns have none of them.
        cpu_col = lambda side, col: statistics.median(float(r.get(col) or 0) for r in side)
        lim = lambda side: statistics.mode(r.get("limit") or "-" for r in side)
        print(f"{key:41} {overall(words):7}  " + "  ".join(cols) +
              f"  {p95(a):6.0f}/{p95(b):<6.0f}  {sp(a):4.0f}/{sp(b):<4.0f}"
              f"   {win(a):.3f}/{win(b):<.3f}      {tag_cv(a):.2f}/{tag_cv(b):<.2f}"
              f"    {cli_pct(a):4.0f}/{cli_pct(b):<4.0f}"
              f"  {mb(a, 'server_rss_kb'):5.1f}/{mb(b, 'server_rss_kb'):<5.1f}"
              f"  {mutex(a):6.2f}/{mutex(b):<6.2f}"
              f"  {cpu_col(a, 'machine_cpu_pct'):4.0f}/{cpu_col(b, 'machine_cpu_pct'):<4.0f}"
              f"   {cpu_col(a, 'client_max_thread_pct'):4.0f}/{cpu_col(b, 'client_max_thread_pct'):<4.0f}"
              f"     {cpu_col(a, 'server_max_thread_pct'):4.0f}/{cpu_col(b, 'server_max_thread_pct'):<4.0f}"
              f"     {lim(a)}/{lim(b)}"
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
    p.add_argument("--only", help="run only configurations whose key contains this text; comma separated for several")
    p.add_argument("--thread-sweep", choices=["modbus", "logix", "micro800"], help="run the thread sweep instead")
    p.add_argument("--fairness-matrix", action="store_true", help="run the step 3A fairness matrix instead")
    p.add_argument("--scale-matrix", choices=["modbus", "logix", "micro800"],
                   help="run the tags x threads x connections matrix instead")
    p.add_argument("--release-matrix", choices=["modbus", "logix", "micro800"],
                   help="run the release matrix (connections x threads x tags, sync and async) instead")
    p.add_argument("--auto-matrix", choices=["logix", "micro800"],
                   help="run the auto-sync sweep (tags x auto_sync_read_ms x connections) instead")
    p.add_argument("--summary", help="also write medians to this CSV, plus one matrix CSV per client metric")
    p.add_argument("--poll-ms", type=int, default=1, help="async: sleep this long between status passes; 0 yields in a loop")
    p.add_argument("--delay-ms", type=float, default=None,
                   help="server response delay in ms, fractions allowed; 0 is no delay; omitted uses the matrix default")
    p.add_argument("--threads", type=int_list, default=SWEEP_THREADS, help="thread sweep: thread counts, comma separated")
    p.add_argument("--tags-per-thread", type=int_list, default=(100,), help="thread sweep: tags per thread, comma separated")
    p.add_argument("--interleave", action="store_true", help="create tags round-robin across threads")
    p.add_argument("--mutex-shim", type=Path, help="LD_PRELOAD this mutex_wait_shim into the clients (Linux)")
    p.add_argument("--self-check", action="store_true")
    p.add_argument("--from-csv", type=Path, help="print the verdicts for an earlier --out file, without running anything")
    args = p.parse_args()

    if args.self_check:
        self_check()
        return
    if args.from_csv:
        summarize(results_from_csv(args.from_csv))
        return
    hardware = bool(args.logix_attrs)
    if not args.a or not args.b or (not hardware and not args.server_dir):
        p.error("--a, --b and --server-dir (or --logix-attrs) are required")

    if args.fairness_matrix:
        matrix = fairness_matrix(1.0 if args.delay_ms is None else args.delay_ms)
    elif args.release_matrix:
        matrix = release_matrix(args.release_matrix, 0.3 if args.delay_ms is None else args.delay_ms)
    elif args.auto_matrix:
        matrix = auto_matrix(args.auto_matrix, 0.3 if args.delay_ms is None else args.delay_ms)
    elif args.scale_matrix:
        matrix = scale_matrix(args.scale_matrix, 1.0 if args.delay_ms is None else args.delay_ms)
    elif args.thread_sweep:
        matrix = thread_sweep_matrix(args.thread_sweep, args.delay_ms or 0, args.threads, args.tags_per_thread)
    else:
        matrix = build_matrix(hardware)
    configs = [c for c in matrix if not args.only or any(o in config_key(c) for o in args.only.split(","))]
    # ponytail: + 2 s per run for create, prime and destroy; a guess, not measured per protocol.
    total_s = sum(2 * args.rounds * (c.get("duration", args.duration) + 2) for c in configs)
    print(f"{len(configs)} configurations, {args.rounds} rounds, about {total_s // 60} min", flush=True)

    results = {}
    with open(args.out, "w", newline="") as f:
        out = csv.writer(f)
        out.writerow(["# A=" + args.a, "B=" + args.b])
        # new columns go last so that older readers indexing by position still work.
        out.writerow(["side", "round", "protocol", "delay_ms", "elems", "server_cpu_pct"] + HARNESS_COLUMNS + ["server_rss_kb"]
                     + list(CPU_COLUMNS))
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
                            cols, srv, srv_rss, cpu = got
                            out.writerow([side, rnd, protocol, delay, c["elems"], f"{srv:.1f}"] + cols + [srv_rss]
                                         + [f"{v:.1f}" if isinstance(v, float) else v for v in cpu.values()])
                            f.flush()
                            row = dict(zip(HARNESS_COLUMNS, cols), side=side, server_cpu_pct=srv, server_rss_kb=srv_rss, **cpu)
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
    if args.summary:
        write_summary(summary_rows(results, {config_key(c): c for c in configs}), args.summary)


if __name__ == "__main__":
    main()

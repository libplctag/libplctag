#!/usr/bin/env python3
"""CIP and Modbus tag ramps (docs/release_2_7_3_must_do.md items 7 and 8).

For each tag count, test_many_tags creates every tag, writes them, reads every tag --passes
times and destroys them.  Only the read passes are the result; create, write and destroy are
logged.  One ab_server (ControlLogix, no delay unless --delay-ms) holds perfTag1..K, DINT, so
no array passes the 65535-element limit; K is at least 4 and a multiple of --groups, so each
array belongs to one connection.

--protocol=modbus runs item 8 instead: one modbus_server holding --ramp's largest count of
holding registers, tag i is hr<i> (test_many_tags --registers).  modbus_server is single
threaded, so when its CPU over a step's read passes reaches --server-limit-pct of one core the
ramp stops: later steps would measure the server, not the library.

Every line test_many_tags prints is echoed (every pass).  The server's CPU, busiest thread
and RSS, and the machine total, are sampled over the read passes ("# reads start/end").
A table of all steps ends the run.

--alloc-count=PATH loads liballoc_count into test_many_tags (item 13) and adds allocations per
tag created and per tag read to the table.  The read figure includes test_many_tags' own growth
of its pass-time array, at most log2(passes / --passes) reallocs per step.  On Linux the test
binary must be dynamic: Release links it -static unless configured with -DUSE_VALGRIND=1.

Usage:
  run_tag_ramp.py --bin-dir=build/bin_dist [--protocol=cip|modbus] [--ramp=200,2000,20000,200000]
      [--passes=10] [--read-s=3] [--groups=1] [--threads=1] [--delay-ms=0] [--idle-s=0]
      [--server-limit-pct=95] [--alloc-count=build/bin_dist/liballoc_count.dylib]
"""

import argparse
import math
import os
import re
import resource
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_perf_compare import (CORES, busiest_thread_pct, cpu_seconds, int_list, machine_seconds,  # noqa: E402
                              server_rss_kb, stop_server, thread_seconds, wait_port)

PORT = 44850
MODBUS_PORT = 50200
RAMPS = {"cip": (200, 2000, 20000, 200000), "modbus": (50, 500, 5000, 50000)}
MAX_ARRAY_ELEMS = 65535

# test_many_tags output lines this script reads; every other line is only echoed.
PATTERNS = {
    "create_ms": r"^create\s+\d+ tags in (\d+) ms",
    "write_ms": r"^write\s+\d+ tags in (\d+) ms",
    "destroy_ms": r"^destroy\s+\d+ tags in (\d+) ms",
    "fastest_ms,median_ms,slowest_ms,mean_ms":
        r"^reads\s+\d+ passes: fastest ([\d.]+) ms, median ([\d.]+) ms, slowest ([\d.]+) ms, mean ([\d.]+) ms",
    "fastest_rate,median_rate,overall_rate":
        r"^rate\s+fastest pass (\d+) tags/s, median pass (\d+) tags/s, overall (\d+) tags/s",
    "client_cpu_pct": r"^CPU\s+client .* ([\d.]+)% of one core",
    "rss_before_kb,rss_created_kb,bytes_per_tag": r"^memory\s+peak RSS (\d+) KB before create, (\d+) KB after create, (-?\d+) bytes",
    "rss_reads_kb": r"^memory\s+peak RSS (\d+) KB after reads",
    "passes_done": r"^reads\s+(\d+) passes:",
    "create_allocs": r"^alloc_count create end: (\d+) allocs",
    "read_allocs": r"^alloc_count reads end: (\d+) allocs",
}


def arrays_for(max_tags, groups):
    """At least 4, a multiple of groups, and enough that no array is over the limit."""
    k = max(4, math.ceil(max_tags / MAX_ARRAY_ELEMS))
    return math.ceil(k / groups) * groups


def parse(lines):
    out = {}
    for keys, pattern in PATTERNS.items():
        for line in lines:
            m = re.match(pattern, line)
            if m:
                out.update(zip(keys.split(","), (float(v) for v in m.groups())))
    return out


def sample(server):
    return dict(t=time.monotonic(), srv=cpu_seconds(server.pid), threads=thread_seconds(server.pid),
                machine=machine_seconds(), rss=server_rss_kb(server.pid))


def run_step(args, tags, arrays, server):
    if args.protocol == "modbus":
        cmd = [str(args.bin_dir / "test_many_tags"), "--tag=protocol=modbus-tcp&gateway=127.0.0.1:%d&path=1" % MODBUS_PORT,
               "--name=hr", "--registers"]
    else:
        cmd = [str(args.bin_dir / "test_many_tags"), "--tag=protocol=ab-eip&gateway=127.0.0.1:%d&path=1,0&plc=ControlLogix" % PORT,
               "--name=perfTag", f"--arrays={arrays}"]
    cmd += [f"--tags={tags}", f"--groups={args.groups}", f"--threads={args.threads}", f"--passes={args.passes}",
            f"--read-s={args.read_s}", f"--idle-s={args.idle_s}"]
    print(f"\n=== {tags} tags: {' '.join(cmd)}", flush=True)
    start = end = None
    lines = []
    env = None
    if args.alloc_count:
        env = dict(os.environ, **{"DYLD_INSERT_LIBRARIES" if sys.platform == "darwin" else "LD_PRELOAD": str(args.alloc_count)})
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    for line in proc.stdout:
        if line.startswith("# reads start"):
            start = sample(server)
        elif line.startswith("# reads end"):
            end = sample(server)
        print("  " + line, end="", flush=True)
        lines.append(line)
    proc.wait()
    if server.poll() is not None:
        sys.exit(f"server exited during the {tags}-tag step")
    r = parse(lines)
    r.update(tags=tags, rc=proc.returncode)
    add_alloc_rates(r)
    if args.alloc_count and "create_allocs" not in r:
        print("  # alloc_count did not load: a -static test binary, or a sanitizer build", flush=True)
        r["rc"] = r["rc"] or 1
    if start and end:
        dt = end["t"] - start["t"]
        r.update(server_cpu_pct=100.0 * (end["srv"] - start["srv"]) / dt,
                 server_thread_pct=busiest_thread_pct(start["threads"], end["threads"], dt),
                 machine_cpu_pct=100.0 * (end["machine"] - start["machine"]) / dt,
                 server_rss_kb=end["rss"])
    return r


COLUMNS = (("tags", "tags", "{:.0f}"), ("rc", "rc", "{:.0f}"),
           ("fastest_ms", "fastest ms", "{:.3f}"), ("median_ms", "median ms", "{:.3f}"),
           ("slowest_ms", "slowest ms", "{:.3f}"), ("fastest_rate", "best tags/s", "{:.0f}"),
           ("median_rate", "median tags/s", "{:.0f}"), ("client_cpu_pct", "cli %", "{:.1f}"),
           ("server_cpu_pct", "srv %", "{:.1f}"), ("server_thread_pct", "srv thr %", "{:.1f}"),
           ("machine_cpu_pct", "mach %", "{:.1f}"), ("rss_created_kb", "cli KB", "{:.0f}"),
           ("bytes_per_tag", "B/tag", "{:.0f}"), ("server_rss_kb", "srv KB", "{:.0f}"),
           ("create_ms", "create ms", "{:.0f}"), ("destroy_ms", "destroy ms", "{:.0f}"),
           ("allocs_per_create", "allocs/create", "{:.2f}"), ("allocs_per_read", "allocs/read", "{:.4f}"))


def table(rows):
    cells = [[h for _, h, _ in COLUMNS]]
    cells += [[f.format(r[k]) if k in r else "-" for k, _, f in COLUMNS] for r in rows]
    widths = [max(len(c[i]) for c in cells) for i in range(len(COLUMNS))]
    return "\n".join("  ".join(c.rjust(w) for c, w in zip(row, widths)) for row in cells)


def add_alloc_rates(row):
    if "create_allocs" in row:
        row["allocs_per_create"] = row["create_allocs"] / row["tags"]
    if "read_allocs" in row and row.get("passes_done"):
        row["allocs_per_read"] = row["read_allocs"] / (row["passes_done"] * row["tags"])


def server_limited(row, limit_pct):
    return row.get("server_cpu_pct", 0.0) >= limit_pct


def self_check():
    assert arrays_for(200000, 1) == 4 and arrays_for(200000, 3) == 6 and arrays_for(1000000, 1) == 16
    r = parse(["create   200 tags in 12 ms, 0 failures\n",
               "memory   peak RSS 100 KB before create, 300 KB after create, 1024 bytes per tag\n",
               "reads    3 passes: fastest 1.500 ms, median 2.000 ms, slowest 3.000 ms, mean 2.167 ms\n",
               "rate     fastest pass 133333 tags/s, median pass 100000 tags/s, overall 90000 tags/s\n",
               "CPU      client 5.0 ms over 7.0 ms of reads, 71.4% of one core\n"])
    assert r["create_ms"] == 12 and r["bytes_per_tag"] == 1024 and r["median_ms"] == 2.0
    assert r["fastest_rate"] == 133333 and r["client_cpu_pct"] == 71.4
    assert "-" in table([dict(tags=1)])
    assert server_limited(dict(server_cpu_pct=96.0), 95) and not server_limited(dict(server_cpu_pct=50.0), 95)
    assert not server_limited(dict(), 95)
    r = parse(["reads    4 passes: fastest 1.000 ms, median 1.000 ms, slowest 1.000 ms, mean 1.000 ms\n",
               "alloc_count create end: 300 allocs, 0 frees, 9000 bytes since last mark; 310 allocs, 2 frees, 9100 bytes total\n",
               "alloc_count reads end: 2 allocs, 1 frees, 64 bytes since last mark; 312 allocs, 3 frees, 9164 bytes total\n"])
    r["tags"] = 100
    add_alloc_rates(r)
    assert r["allocs_per_create"] == 3.0 and r["allocs_per_read"] == 0.005
    print("self-check ok")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--bin-dir", type=Path, help="holds test_many_tags and ab_server")
    p.add_argument("--protocol", choices=("cip", "modbus"), default="cip")
    p.add_argument("--ramp", type=int_list, help="default 200,2000,20000,200000 for cip, 50,500,5000,50000 for modbus")
    p.add_argument("--passes", type=int, default=10, help="at least this many read passes per step")
    p.add_argument("--read-s", type=int, default=3, help="and at least this long, so CPU sampling has a usable window")
    p.add_argument("--groups", type=int, default=1)
    p.add_argument("--threads", type=int, default=1)
    p.add_argument("--delay-ms", type=float, default=0.0)
    p.add_argument("--idle-s", type=int, default=0, help="idle CPU phase after the reads (test_many_tags --idle-s)")
    p.add_argument("--server-limit-pct", type=float, default=95.0, help="modbus: stop the ramp when the server reaches this CPU")
    p.add_argument("--alloc-count", type=Path, help="liballoc_count to load into test_many_tags (item 13)")
    p.add_argument("--self-check", action="store_true")
    args = p.parse_args()
    if args.self_check:
        return self_check()
    if not args.bin_dir:
        p.error("--bin-dir is required")

    # macOS starts at 256 open files.
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    want = 1024 if hard == resource.RLIM_INFINITY else min(1024, hard)
    if soft < want:
        resource.setrlimit(resource.RLIMIT_NOFILE, (want, hard))

    args.ramp = args.ramp or RAMPS[args.protocol]
    arrays = arrays_for(max(args.ramp), args.groups)
    elems = math.ceil(max(args.ramp) / arrays)
    if args.protocol == "modbus":
        if max(args.ramp) > 65535:
            p.error("modbus: at most 65535 tags, one unit's holding registers")
        port = MODBUS_PORT
        cmd = [str(args.bin_dir / "modbus_server"), f"--listen=127.0.0.1:{port}", f"--holding-registers={max(args.ramp)}",
               "--debug=WARN"]
    else:
        port = PORT
        cmd = [str(args.bin_dir / "ab_server"), "--plc=ControlLogix", f"--port={port}", "--path=1,0"]
        cmd += [f"--tag=perfTag{k}:DINT[{elems}]" for k in range(1, arrays + 1)]
    cmd += [f"--delay-ms={args.delay_ms:g}"] if args.delay_ms else []
    print(f"server: {' '.join(cmd)}\n{CORES} cores", flush=True)
    log = tempfile.TemporaryFile()
    server = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=log)
    rows = []
    try:
        wait_port(port, server)
        for tags in args.ramp:
            rows.append(run_step(args, tags, arrays, server))
            if args.protocol == "modbus" and server_limited(rows[-1], args.server_limit_pct):
                print(f"\n# server limit: modbus_server at {rows[-1]['server_cpu_pct']:.1f}% of one core at {tags} tags;"
                      " later steps would measure the server.  Ramp stopped.", flush=True)
                break
    finally:
        stop_server(server, log)
        log.close()

    print(f"\n{args.protocol}: {len(rows)} steps, at least {args.passes} read passes and {args.read_s} s each, {args.groups} connections, {args.threads} threads,"
          f" delay {args.delay_ms:g} ms\n" + table(rows))
    return 0 if all(r["rc"] == 0 for r in rows) else 1


if __name__ == "__main__":
    sys.exit(main())

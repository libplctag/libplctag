#!/usr/bin/env python3
"""CIP tag ramp (docs/release_2_7_3_must_do.md item 7).

For each tag count, test_many_tags creates every tag, writes them, reads every tag --passes
times and destroys them.  Only the read passes are the result; create, write and destroy are
logged.  One ab_server (ControlLogix, no delay unless --delay-ms) holds perfTag1..K, DINT, so
no array passes the 65535-element limit; K is at least 4 and a multiple of --groups, so each
array belongs to one connection.

Every line test_many_tags prints is echoed (every pass).  The server's CPU, busiest thread
and RSS, and the machine total, are sampled over the read passes ("# reads start/end").
A table of all steps ends the run.

Usage:
  run_tag_ramp.py --bin-dir=build/bin_dist [--ramp=200,2000,20000,200000] [--passes=10]
      [--read-s=3] [--groups=1] [--threads=1] [--delay-ms=0] [--idle-s=0]
"""

import argparse
import math
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
    cmd = [str(args.bin_dir / "test_many_tags"), "--tag=protocol=ab-eip&gateway=127.0.0.1:%d&path=1,0&plc=ControlLogix" % PORT,
           "--name=perfTag", f"--tags={tags}", f"--arrays={arrays}", f"--groups={args.groups}",
           f"--threads={args.threads}", f"--passes={args.passes}", f"--read-s={args.read_s}", f"--idle-s={args.idle_s}"]
    print(f"\n=== {tags} tags: {' '.join(cmd)}", flush=True)
    start = end = None
    lines = []
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    for line in proc.stdout:
        if line.startswith("# reads start"):
            start = sample(server)
        elif line.startswith("# reads end"):
            end = sample(server)
        print("  " + line, end="", flush=True)
        lines.append(line)
    proc.wait()
    if server.poll() is not None:
        sys.exit(f"ab_server exited during the {tags}-tag step")
    r = parse(lines)
    r.update(tags=tags, rc=proc.returncode)
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
           ("create_ms", "create ms", "{:.0f}"), ("destroy_ms", "destroy ms", "{:.0f}"))


def table(rows):
    cells = [[h for _, h, _ in COLUMNS]]
    cells += [[f.format(r[k]) if k in r else "-" for k, _, f in COLUMNS] for r in rows]
    widths = [max(len(c[i]) for c in cells) for i in range(len(COLUMNS))]
    return "\n".join("  ".join(c.rjust(w) for c, w in zip(row, widths)) for row in cells)


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
    print("self-check ok")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--bin-dir", type=Path, help="holds test_many_tags and ab_server")
    p.add_argument("--ramp", type=int_list, default=(200, 2000, 20000, 200000))
    p.add_argument("--passes", type=int, default=10, help="at least this many read passes per step")
    p.add_argument("--read-s", type=int, default=3, help="and at least this long, so CPU sampling has a usable window")
    p.add_argument("--groups", type=int, default=1)
    p.add_argument("--threads", type=int, default=1)
    p.add_argument("--delay-ms", type=float, default=0.0)
    p.add_argument("--idle-s", type=int, default=0, help="idle CPU phase after the reads (test_many_tags --idle-s)")
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

    arrays = arrays_for(max(args.ramp), args.groups)
    elems = math.ceil(max(args.ramp) / arrays)
    cmd = [str(args.bin_dir / "ab_server"), "--plc=ControlLogix", f"--port={PORT}", "--path=1,0"]
    cmd += [f"--tag=perfTag{k}:DINT[{elems}]" for k in range(1, arrays + 1)]
    cmd += [f"--delay-ms={args.delay_ms:g}"] if args.delay_ms else []
    print(f"server: {' '.join(cmd)}\n{CORES} cores", flush=True)
    log = tempfile.TemporaryFile()
    server = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=log)
    rows = []
    try:
        wait_port(PORT, server)
        for tags in args.ramp:
            rows.append(run_step(args, tags, arrays, server))
    finally:
        stop_server(server, log)
        log.close()

    print(f"\n{len(rows)} steps, at least {args.passes} read passes and {args.read_s} s each, {args.groups} connections, {args.threads} threads,"
          f" delay {args.delay_ms:g} ms\n" + table(rows))
    return 0 if all(r["rc"] == 0 for r in rows) else 1


if __name__ == "__main__":
    sys.exit(main())

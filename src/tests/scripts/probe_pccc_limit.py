#!/usr/bin/env python3
"""Find the real per-packet element limit for a PCCC tag, and say who enforced it.

The library refuses a transfer it thinks will not fit, using an overhead estimate
built from the frame layout.  A PLC refuses one it actually cannot handle.  Those
two limits should coincide; if they do not, the estimate is wrong in one direction
or the other:

  library stops first   -> the estimate is too conservative, throughput is lost
  the PLC stops first   -> the estimate is too permissive, an over-long frame went
                           on the wire, which is the dangerous direction

This walks the element count upward one at a time and reports the first refusal of
each kind, so the two can be compared.  It is a measurement tool, not a pass/fail
test: run it by hand against real hardware.

Every attempt's log is kept, one file per element count, in log_dir (the working
directory by default).  The summary names the two that matter -- the last success
and the first refusal -- but the whole walk is on disk, because the interesting
question is usually "what changed between these two frames?" and that needs both.

Examples:
  probe_pccc_limit.py build/bin_dist --gateway=10.206.1.38 --plc=plc5 --name=N101:0
  probe_pccc_limit.py build/bin_dist /tmp/probe_logs --gateway=10.206.1.40 \\
                      --path=1,2,A:27:1 --plc=plc5 --name=N101:0 --elem-size=2
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

# The message our own size check emits.  Seeing this means the library said no.
LIBRARY_REFUSAL = re.compile(r"write data per packet is (\d+)", re.I)
LIBRARY_OVERHEAD = re.compile(r"write overhead is (\d+)", re.I)

# Anything the library reports as too large, whoever decided it.
TOO_LARGE = re.compile(r"PLCTAG_ERR_TOO_LARGE", re.I)

# The PLC saying the transfer runs off the end of the data file.  This is an address
# refusal, not a size refusal: start + count exceeded the file length.  It looks like a
# limit but it is the wrong limit, and it is easy to mistake for one because it also
# arrives as a failure at a particular element count.
WRONG_SIZE = re.compile(r"response code: 240|File is wrong size", re.I)

# Lines worth showing from a failing run.  The tail of a debug=4 log is library teardown,
# which says nothing about why the run failed; the WARN and ERROR lines do.  Matched
# case-sensitively so that the level token is what matches -- a case-fold match also
# catches DETAIL lines like "No error, socket is connected".
INTERESTING = re.compile(r" WARN | ERROR |^ERROR:")

# Warnings every run emits on the way down, regardless of why it failed.
LOG_NOISE = re.compile(r"plc_tag_tickler_wake_impl")


def build_tag(args, count):
    parts = [f"protocol=ab-eip&gateway={args.gateway}"]
    if args.path:
        parts.append(f"path={args.path}")
    parts.append(f"plc={args.plc}")
    if args.elem_size:
        parts.append(f"elem_size={args.elem_size}")
    parts.append(f"elem_count={count}")
    parts.append(f"name={args.name}")
    return "&".join(parts)


def log_path(args, count):
    direction = "write" if args.write else "read"
    return Path(args.log_dir) / f"{direction}_{count:04}.log"


def attempt(args, count):
    """Run one transfer.  Returns (ok, who_refused, log_text)."""
    cmd = [str(Path(args.test_dir) / "tag_rw2"), f"--type={args.type}", f"--tag={build_tag(args, count)}", "--debug=4"]

    if args.write:
        cmd.append("--write=" + ",".join(str(i % 100) for i in range(count)))

    proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    log = proc.stdout + proc.stderr

    with open(log_path(args, count), "w") as log_file:
        log_file.write(" ".join(cmd) + "\n\n")
        log_file.write(log)
        log_file.write(f"\nexit code {proc.returncode}\n")

    if proc.returncode == 0:
        return True, None, log

    if LIBRARY_REFUSAL.search(log):
        return False, "library", log

    if TOO_LARGE.search(log):
        return False, "plc (too large)", log

    if WRONG_SIZE.search(log):
        return False, "plc (past end of file)", log

    return False, "other", log


def print_log_excerpt(log, path):
    """Show the WARN and ERROR lines, which is where the reason lives."""
    lines = [line for line in log.splitlines() if INTERESTING.search(line) and not LOG_NOISE.search(line)]

    print(f"\nWarnings and errors from {path}:")

    if not lines:
        print("    (none -- read the whole log)")
        return

    for line in lines[:15]:
        print("    " + line)

    if len(lines) > 15:
        print(f"    ... {len(lines) - 15} more, see the log")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("test_dir", help="directory holding tag_rw2")
    parser.add_argument("log_dir", nargs="?", default=".", help="one log per attempt is written here")
    parser.add_argument("--gateway", required=True)
    parser.add_argument("--path", default=None, help="omit for a PLC with no chassis path")
    parser.add_argument("--plc", required=True, help="plc5, slc500, micrologix, lgxpccc, ...")
    parser.add_argument("--name", required=True, help="e.g. N101:0")
    parser.add_argument("--type", default="sint16")
    parser.add_argument("--elem-size", type=int, default=None)
    parser.add_argument("--start", type=int, default=80)
    parser.add_argument("--max", type=int, default=250)
    parser.add_argument("--write", action="store_true", help="probe writes instead of reads")
    args = parser.parse_args()

    Path(args.log_dir).mkdir(parents=True, exist_ok=True)

    direction = "write" if args.write else "read"
    print(f"Probing {direction} limit for {args.name} on {args.plc} at {args.gateway}"
          f"{' path ' + args.path if args.path else ''}")
    print(f"Element counts {args.start} upward, stopping at the first refusal.")
    print(f"Logs in {Path(args.log_dir).resolve()}\n")

    last_ok = None
    overhead = None
    per_packet = None

    for count in range(args.start, args.max + 1):
        ok, who, log = attempt(args, count)

        if ok:
            last_ok = count
            print(f"  {count:4} elements  ok")
            continue

        print(f"  {count:4} elements  REFUSED by {who}")

        m = LIBRARY_OVERHEAD.search(log)
        if m:
            overhead = int(m.group(1))
        m = LIBRARY_REFUSAL.search(log)
        if m:
            per_packet = int(m.group(1))

        print()
        if last_ok is None:
            print(f"Nothing succeeded, even at {args.start}.  Lower --start, or check the tag exists.")
            print(f"See {log_path(args, count)}.")
            return 1

        print(f"Largest transfer that worked: {last_ok} elements  ({log_path(args, last_ok)})")
        print(f"First refusal:                {count} elements, by the {who}  ({log_path(args, count)})")

        if overhead is not None and per_packet is not None:
            print(f"\nThe library's own numbers at the refusal: overhead {overhead} bytes, "
                  f"{per_packet} bytes of data per packet.")
            if args.elem_size:
                print(f"That is {per_packet // args.elem_size} elements of {args.elem_size} bytes.")

        if who == "library":
            print("\nThe library stopped first.  If the PLC would have accepted more, the overhead\n"
                  "estimate is too conservative and throughput is being left on the table.")
        elif who == "plc (past end of file)":
            offset = args.name.split(":")[-1]
            print(f"\nThe transfer ran off the end of the data file, so the size check was never\n"
                  f"reached.  Starting at element {offset}, {count} elements needs a file of at least\n"
                  f"{count + (int(offset) if offset.isdigit() else 0)} elements.  This says nothing about the per-packet limit.\n"
                  "Point --name at a longer file, or start at a lower offset.")
            print_log_excerpt(log, log_path(args, count))
        elif who.startswith("plc"):
            print("\nThe PLC stopped first.  The library let an over-long frame onto the wire, so the\n"
                  "overhead estimate is too small.  That is the direction that corrupts transfers.")
        else:
            print("\nThe failure was not a size refusal, so the size check was never reached.")
            print_log_excerpt(log, log_path(args, count))

        return 0

    print(f"\nReached {args.max} elements with no refusal.  Raise --max.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

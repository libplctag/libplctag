#!/usr/bin/env python3
"""Hardware test runner -- Python port of run_hardware_tests.sh.

Same test list, same real-PLC gateway IPs/tag strings, same pass/fail
semantics, ported into the declarative Test/Result model used by
run_simulator_tests_parallel.py so the two runners look and behave alike.

Unlike the simulator runner, there is nothing to spawn: every test here talks
to a real, already-running PLC on the lab network, addressed directly by IP
in its own command line. There's no local server lifecycle, no port
allocation, nothing to tear down -- run_test() is just "run the command,
capture the log, check the exit code."

Sequential by default (like the shell script): these are real, shared lab
devices of unknown concurrent-connection tolerance (some, like Micrologix/
PLC5, are old hardware historically limited to a handful of simultaneous
sessions), so parallelism here is opt-in via --workers, not the default the
way it is for the local simulator.

Usage matches the bash script: run_hardware_tests.py TEST_DIR [LOG_DIR]
"""

import argparse
import dataclasses
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Optional


@dataclasses.dataclass(frozen=True)
class Test:
    id: int
    name: str
    cmd: list[str]
    log_file: str
    expect_failure: bool = False


@dataclasses.dataclass(frozen=True)
class LogixTarget:
    """One cell of the chassis matrix: an Ethernet entry point plus a CPU slot."""
    label: str
    gateway: str
    path: str
    #: whether the TestBigArray / TestDINTArray / TestINTArray family is loaded on this CPU.
    #: Cells without them run only the tests that name no tag.
    has_test_tags: bool


@dataclasses.dataclass
class Result:
    test: Test
    ok: bool
    start: float
    end: float

    @property
    def elapsed(self) -> float:
        return self.end - self.start


TEST_DIR: Path
LOG_DIR: Path
_next_id = [1]


def exe(name: str) -> str:
    for candidate in (TEST_DIR / name, TEST_DIR / f"{name}.exe"):
        if candidate.exists():
            return str(candidate)
    raise FileNotFoundError(f"{name} not found in {TEST_DIR}")


def test(name: str, cmd: list[str], *, expect_failure: bool = False) -> Test:
    tid = _next_id[0]
    _next_id[0] += 1
    slug = re.sub(r"[^a-zA-Z0-9]+", "_", name).strip("_").lower()
    log_file = str(LOG_DIR / f"{tid}_{slug}.log")
    return Test(id=tid, name=name, cmd=cmd, log_file=log_file, expect_failure=expect_failure)


REQUIRED_EXECUTABLES = [
    "tag_rw2", "test_special", "test_tag_attributes", "test_tag_type_attribute",
    "string_standard", "string_non_standard_udt", "test_string", "test_idle_disconnect",
    "test_raw_cip", "list_tags_logix", "get_identity", "test_connection_tag",
    "test_write_before_read", "thread_stress",
]


def check_executables_present() -> None:
    missing = [n for n in REQUIRED_EXECUTABLES if not (TEST_DIR / n).exists() and not (TEST_DIR / f"{n}.exe").exists()]
    if missing:
        print(f"Missing executables in {TEST_DIR}: {', '.join(missing)}")
        sys.exit(1)


# ---------------------------------------------------------------------------
# Manifest: real-hardware gateways, one test per line in run_hardware_tests.sh
# ---------------------------------------------------------------------------

def build_manifest() -> list[Test]:
    # Real lab devices -- see run_hardware_tests.sh for the same addresses.
    logix_gw = "10.206.1.40"
    logix_path = "1,4"
    mlgx_gw = "10.206.1.36"
    plc5_gw = "10.206.1.38"
    dhp_bridge_gw = "10.206.1.40"
    cip_bridge_gw = "10.206.1.37"
    cip_bridge_target = "10.206.1.39"

    tests = [
        test("special tags", [exe("test_special")]),
        test("tag attributes", [exe("test_tag_attributes")]),
        test("tag type byte array attributes", [exe("test_tag_type_attribute")]),
        test("test tag_rw2 to get tag metadata",
             [exe("tag_rw2"), "--type=metadata",
              f"--tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=logix&name=TestBOOLArray"]),
        test("basic large tag read/write",
             [exe("tag_rw2"), "--type=sint32",
              f"--tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&elem_count=1000&name=TestBigArray",
              "--debug=4", "--write=1,2,3,4,5,6,7,8,9"]),
        test("test standard strings", [exe("string_standard")]),
        test("idle disconnect and reconnect with runtime timeout change (ControlLogix)",
             [exe("test_idle_disconnect"),
              f"--tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&name=TestBigArray"]),
        test("Get INT bit",
             [exe("tag_rw2"), "--type=bit",
              f"--tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&name=TestINTArray[0].13",
              "--debug=4"]),
        test("Set INT bit",
             [exe("tag_rw2"), "--type=bit",
              f"--tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&name=TestINTArray[0].13",
              "--debug=4", "--write=1"]),
        test("Get LINT bit",
             [exe("tag_rw2"), "--type=bit",
              f"--tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&name=TestLINTArray[0].43",
              "--debug=4"]),
        test("Set LINT bit",
             [exe("tag_rw2"), "--type=bit",
              f"--tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&name=TestLINTArray[0].43",
              "--debug=4", "--write=1"]),
        test("test non-standard UDT strings", [exe("string_non_standard_udt")]),
        test("test non-standard string size", [exe("test_string")]),
        test("B data file Micrologix tag read/write",
             [exe("tag_rw2"), "--type=uint16", f"--tag=protocol=ab-eip&gateway={mlgx_gw}&plc=micrologix&name=B3:0",
              "--write=0", "--debug=4"]),
        test("B bit data file Micrologix tag read/write",
             [exe("tag_rw2"), "--type=bit", f"--tag=protocol=ab-eip&gateway={mlgx_gw}&plc=micrologix&name=B3:0/6",
              "--write=1", "--debug=4"]),
        test("N data file Micrologix tag read/write",
             [exe("tag_rw2"), "--type=sint16", f"--tag=protocol=ab-eip&gateway={mlgx_gw}&plc=micrologix&name=N7:0",
              "--write=42", "--debug=4"]),
        test("N bit data file Micrologix tag read/write",
             [exe("tag_rw2"), "--type=bit", f"--tag=protocol=ab-eip&gateway={mlgx_gw}&plc=micrologix&name=N7:0/10",
              "--write=1", "--debug=4"]),
        test("L data file Micrologix tag read/write",
             [exe("tag_rw2"), "--type=sint32",
              f"--tag=protocol=ab-eip&gateway={mlgx_gw}&plc=micrologix&elem_count=4&name=L10:0",
              "--write=0,1,2,3", "--debug=4"]),
        test("L bit data file Micrologix tag read",
             [exe("tag_rw2"), "--type=bit", f"--tag=protocol=ab-eip&gateway={mlgx_gw}&plc=micrologix&name=L10:0/23",
              "--debug=4"]),
        test("L bit data file Micrologix tag write",
             [exe("tag_rw2"), "--type=bit", f"--tag=protocol=ab-eip&gateway={mlgx_gw}&plc=micrologix&name=L10:0/23",
              "--write=1", "--debug=4"],
             expect_failure=True),  # this write should NOT succeed
        test("B data file PLC5 tag read/write",
             [exe("tag_rw2"), "--type=uint16", f"--tag=protocol=ab-eip&gateway={plc5_gw}&plc=plc5&elem_count=1&name=B3:0",
              "--debug=4", "--write=0"]),
        test("B bit data file PLC5 tag read/write",
             [exe("tag_rw2"), "--type=bit",
              f"--tag=protocol=ab-eip&gateway={plc5_gw}&plc=plc5&elem_count=1&name=B3:0/10", "--debug=4", "--write=1"]),
        test("N data file PLC5 tag read/write",
             [exe("tag_rw2"), "--type=sint16", f"--tag=protocol=ab-eip&gateway={plc5_gw}&plc=plc5&elem_count=1&name=N7:0",
              "--debug=4", "--write=0"]),
        test("N bit data file PLC5 tag read/write",
             [exe("tag_rw2"), "--type=bit",
              f"--tag=protocol=ab-eip&gateway={plc5_gw}&plc=plc5&elem_count=1&name=N7:0/10", "--debug=4", "--write=1"]),
        # PCCC-mapped Logix tag (plc=lgxpccc): talks PCCC file-based addressing
        # (N7:0 etc.) to a ControlLogix CPU instead of native CIP tag names --
        # exercises eip_lgx_pccc.c, which no other test in this suite reaches.
        # Only the slot 5 CPU module has a DF1/PCCC data file mapped; slots 0
        # and 4 do not and were removed rather than left as expected failures.
        test("PCCC-mapped Logix tag read/write (slot 5)",
             [exe("tag_rw2"), "--type=sint16",
              f"--tag=protocol=ab_eip&gateway={logix_gw}&path=1,5&plc=lgxpccc&elem_count=1&name=N7:0",
              "--debug=4", "--write=42"]),
        test("basic DH+ bridging",
             [exe("tag_rw2"), "--type=uint8",
              f"--tag=protocol=ab_eip&gateway={dhp_bridge_gw}&path=1,2,A:27:1&cpu=plc5&elem_count=1&elem_size=2&name=N7:0",
              "--debug=4", "--write=42"]),
        test("basic DH+ bridging bit change",
             [exe("tag_rw2"), "--type=uint8",
              f"--tag=protocol=ab_eip&gateway={dhp_bridge_gw}&path=1,2,A:27:1&cpu=plc5&elem_count=1&elem_size=2&name=B3:0/10",
              "--debug=4", "--write=0"]),
        test("basic CIP bridging",
             [exe("tag_rw2"), "--type=sint32",
              f"--tag=protocol=ab_eip&gateway={cip_bridge_gw}&path=1,4,18,{cip_bridge_target},1,0&plc=lgx&name=TestBigArray[0]",
              "--debug=4", "--write=5"]),
        # Raw CIP tags are the only tag type whose request is the bare CIP message, so
        # they are what exercises the library's own EIP and CPF framing. Cover every
        # combination that frames differently: connected vs unconnected, a service the
        # PLC answers in one reply vs one that answers with a partial transfer, and
        # several tags at once so their messages are bundled into one Multiple Service
        # Packet. Packing only ever happens on a connection, so there is no unconnected
        # bundled case.
        test("raw cip tag, connected, list tag instances", [exe("test_raw_cip")]),
        test("raw cip tag, unconnected, list tag instances",
             [exe("test_raw_cip"),
              f"--tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&use_connected_msg=0&name=@raw"]),
        test("raw cip tag, connected, read named tag",
             [exe("test_raw_cip"),
              f"--tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&name=@raw",
              "--read=TestBigArray"]),
        test("raw cip tag, unconnected, read named tag",
             [exe("test_raw_cip"),
              f"--tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&use_connected_msg=0&name=@raw",
              "--read=TestBigArray"]),
        test("raw cip tags bundled into one packet",
             [exe("test_raw_cip"),
              f"--tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&name=@raw",
              "--read=TestBigArray", "--count=4"]),
        test("tag listing", [exe("list_tags_logix"), logix_gw, logix_path]),
        test("generic CIP device identity query",
             [exe("get_identity"), f"--tag=protocol=ab_eip&gateway={logix_gw}&plc=generic&name=@identity&debug=3"]),
        # There is deliberately no connected-messaging identity test.  ab_common.c recognises
        # "@identity" only under the generic PLC type, which sets use_connected_msg to zero
        # unconditionally, so a connected identity request is unreachable -- asking for one
        # just reads a tag literally named "@identity" and gets "tag does not exist".
        # Several ordinary tags read together, so the requests are bundled into one Multiple
        # Service Packet and the reply budget decides how many fit.  The raw CIP bundling test
        # covers the assembly; this covers it for tags that carry a reply size.
        test("bundled reads of several ordinary tags",
             [exe("thread_stress"), "4",
              f"protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&name=TestBigArray"]),
        # Writing a tag the caller never read: the library fetches the type itself first.
        test("write before read",
             [exe("test_write_before_read"),
              f"--tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&elem_count=1&name=TestBigArray",
              f"--tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&elem_count=1000&name=TestBigArray"]),
        test("connection tag connection state transitions (ControlLogix)",
             [exe("test_connection_tag"),
              f"--tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&name=@connection"]),
        test("connection tag connection state transitions (Micrologix)",
             [exe("test_connection_tag"), f"--tag=protocol=ab-eip&gateway={mlgx_gw}&plc=Micrologix&name=@connection"]),
        test("connection tag connection state transitions (PLC5)",
             [exe("test_connection_tag"), f"--tag=protocol=ab-eip&gateway={plc5_gw}&plc=plc5&name=@connection"]),
    ]

    tests.extend(build_logix_matrix())

    return tests


# ---------------------------------------------------------------------------
# The 1756 chassis matrix
# ---------------------------------------------------------------------------

#: The cells worth running beyond the primary one.  The primary cell -- the L81 through its
#: own port -- already carries the full Logix set above.
#:
#: Confirmed by querying the Identity object of each slot through .37, so every entry point
#: reaches every CPU across the backplane:
#:
#:     slot 0  1756-L61/B  LOGIX5561   firmware 20.12
#:     slot 4  1756-L81E/B             firmware 31.11   (own Ethernet port, .40)
#:     slot 5  1756-L55/A  LOGIX5555   firmware 16.22   (with a 1756-M22/A)
#:
#: The cells below form a cross rather than a diagonal: three CPUs through one entry module
#: isolates the CPU, and one CPU through three entry modules isolates the entry module.
LOGIX_TARGETS = [
    # --- CPU axis: each slot through the oldest ENBT ---
    #
    # v16 predates the Extended Forward Open, so this is the only device in the building that
    # makes the library try service 0x5B, be refused, and fall back to 0x54 (conn.c:2930).  It
    # also negotiates the smallest connection here, which is what puts cip_conn_max_cip_payload()
    # near its limit and makes the reply budget actually decide bundle sizes -- neither binds at
    # the 4000 bytes the L8x port offers.
    LogixTarget("L55 v16.22 via ENBT 4.8", "10.206.1.37", "1,5", has_test_tags=True),
    # v20 sits either side of the Extended Forward Open depending on platform, so this cell is
    # worth having for the negotiation alone.
    LogixTarget("L61 v20.12 via ENBT 4.8", "10.206.1.37", "1,0", has_test_tags=True),
    # --- entry-module axis: the same CPU through each way in ---
    #
    # Holding the CPU and its data table constant makes these an A/B on the entry module
    # alone: the connection size it will agree to, and the fact that an ENBT is a separate
    # routing device applying the Unconnected Send route timeout, which going in via the L8x
    # port does not have in the path at all.  Reading these two against each other also
    # compares the two ENBT firmware revisions.
    LogixTarget("L81 v31.11 via ENBT 4.8", "10.206.1.37", "1,4", has_test_tags=True),
    LogixTarget("L81 v31.11 via ENBT 6.6", "10.206.1.39", "1,4", has_test_tags=True),
]


def build_logix_matrix() -> list[Test]:
    """
    The Logix tests worth repeating per chassis cell.

    Not the whole Logix set: this is the subset that the connection size, the Forward Open
    variant and the presence of a routing device actually change.  Running all fourteen
    against all four cells would quadruple a run that talks to shared lab hardware without
    testing four times as much.
    """
    tests: list[Test] = []

    for target in LOGIX_TARGETS:
        gw, path = target.gateway, target.path
        base = f"protocol=ab-eip&gateway={gw}&path={path}&plc=ControlLogix"

        # Needs no tag to exist, so every cell can run it.
        tests.append(test(f"{target.label}: identity",
                          [exe("get_identity"), f"--tag=protocol=ab_eip&gateway={gw}&plc=generic&name=@identity&debug=3"]))
        tests.append(test(f"{target.label}: tag listing and UDT definitions",
                          [exe("list_tags_logix"), gw, path]))
        # Raw CIP is what exercises the library's own EIP and CPF framing.  List Tag Instances
        # names no tag, so it runs everywhere; both messaging modes, because the framing is
        # the only thing that differs between them.
        tests.append(test(f"{target.label}: raw cip, connected",
                          [exe("test_raw_cip"), f"--tag={base}&name=@raw"]))
        tests.append(test(f"{target.label}: raw cip, unconnected",
                          [exe("test_raw_cip"), f"--tag={base}&use_connected_msg=0&name=@raw"]))
        tests.append(test(f"{target.label}: connection tag state transitions",
                          [exe("test_connection_tag"), f"--tag={base}&name=@connection"]))

        if not target.has_test_tags:
            continue

        # A large array is where a small connection forces many fragments; on the L8x port the
        # same read takes two.
        # TestBigArray is DINT[1000] -- 4000 bytes.  That is roughly one payload through the
        # L8x port and eight or nine fragments through an ENBT, which is the contrast.
        tests.append(test(f"{target.label}: large fragmented read",
                          [exe("tag_rw2"), "--type=sint32",
                           f"--tag={base}&elem_count=1000&name=TestBigArray", "--debug=4"]))
        tests.append(test(f"{target.label}: large fragmented write",
                          [exe("tag_rw2"), "--type=sint32",
                           f"--tag={base}&elem_count=1000&name=TestBigArray", "--debug=4",
                           "--write=" + ",".join(str(i % 97) for i in range(1000))]))
        # Unconnected messaging on an ordinary tag, which nothing covered before the connected
        # and unconnected builders were merged into one.
        tests.append(test(f"{target.label}: unconnected tag read/write",
                          [exe("tag_rw2"), "--type=sint32",
                           f"--tag={base}&use_connected_msg=0&elem_count=4&name=TestDINTArray",
                           "--debug=4", "--write=11,22,33,44"]))
        tests.append(test(f"{target.label}: raw cip, bundled into one packet",
                          [exe("test_raw_cip"), f"--tag={base}&name=@raw",
                           "--read=TestBigArray", "--count=4"]))

    return tests


# ---------------------------------------------------------------------------
# Execution
# ---------------------------------------------------------------------------

def run_test(t: Test) -> Result:
    start = time.monotonic()
    with open(t.log_file, "w") as log:
        proc = subprocess.run(t.cmd, stdout=log, stderr=subprocess.STDOUT)
    ok = (proc.returncode == 0) != t.expect_failure
    end = time.monotonic()
    return Result(test=t, ok=ok, start=start, end=end)


def _print_result(r: Result) -> None:
    status = "OK" if r.ok else "FAILURE"
    print(f"Test {r.test.id}: {r.test.name}... {status} ({r.elapsed:.0f}s)")


def main() -> int:
    global TEST_DIR, LOG_DIR

    parser = argparse.ArgumentParser()
    parser.add_argument("test_dir")
    parser.add_argument("log_dir", nargs="?", default=".")
    parser.add_argument("--workers", type=int, default=1,
                         help="concurrent tests (default 1: sequential, matching the shell script -- these "
                              "hit real, shared lab PLCs of unknown concurrent-connection tolerance)")
    args = parser.parse_args()

    TEST_DIR = Path(args.test_dir)
    LOG_DIR = Path(args.log_dir)
    LOG_DIR.mkdir(parents=True, exist_ok=True)

    if not TEST_DIR.is_dir():
        print(f"{TEST_DIR} is not a valid path for test executables!")
        return 1

    print("Settings:")
    print(f"  test_dir: {TEST_DIR}")
    print(f"  log_dir: {LOG_DIR}")
    print(f"  workers: {args.workers}")
    print()

    check_executables_present()

    # limit the number of open files, matching the shell script's ulimit -n 1024
    if os.name == "posix":
        try:
            import resource
            soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
            resource.setrlimit(resource.RLIMIT_NOFILE, (min(1024, hard), hard))
        except Exception:
            pass

    script_start = time.monotonic()
    tests = build_manifest()
    results: list[Result] = []

    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futs = {pool.submit(run_test, t): t for t in tests}
        for fut in as_completed(futs):
            r = fut.result()
            _print_result(r)
            results.append(r)

    total = time.monotonic() - script_start

    results.sort(key=lambda r: r.test.id)
    failures = sum(1 for r in results if not r.ok)

    print()
    print(f"{len(results)} tests.")
    print(f"{len(results) - failures} successes.")
    print(f"{failures} failures.")
    print(f"Total time: {total:.0f}s")

    if failures:
        print("\nFailed tests:")
        for r in results:
            if not r.ok:
                print(f"  test {r.test.id}: {r.test.name} (log: {r.test.log_file})")

    return failures


if __name__ == "__main__":
    sys.exit(main())

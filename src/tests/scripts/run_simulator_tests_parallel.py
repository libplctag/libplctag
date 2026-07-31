#!/usr/bin/env python3
"""Parallel-aware simulator test runner.

Rewrite of run_simulator_tests.sh with a different scheduling model:

  - Tests are declared as data (Section + Test), not as sequential script
    steps, so each test gets a stable global id at declare time regardless
    of what order things finish running in.
  - Tests are tagged with a Group: FUNCTIONAL (correctness, fast, order/
    timing doesn't matter), STRESS (many threads/tags -- parallel-safe but
    CPU-heavy), or TIMING (depends on hardcoded waits/timeouts, sensitive to
    CPU contention from neighbors). FUNCTIONAL+STRESS run together in one
    phase; TIMING runs alone in a second phase so stress tests can't stall
    its timers (see the ~300s stall in test 29's idle-disconnect wait during
    a run that overlapped with contention -- that's the failure mode this
    phase split avoids).
  - Every emulator/server gets its own port, allocated centrally, so
    sections can all start concurrently without colliding.
  - subprocess.Popen + .terminate() replace pidfiles/pkill entirely, which
    also makes this work on Windows without a separate code path.

Usage matches the bash script: run_simulator_tests_parallel.py TEST_DIR [LOG_DIR]
"""

import argparse
import dataclasses
import enum
import os
import re
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, Future
from pathlib import Path
from typing import Callable, Optional


# ---------------------------------------------------------------------------
# Test model
# ---------------------------------------------------------------------------

class Group(enum.Enum):
    FUNCTIONAL = "functional"
    STRESS = "stress"
    TIMING = "timing"


# Which groups are allowed to run concurrently with each other. Phases run
# in order; a phase doesn't start until the previous one has fully drained.
PHASES: list[set[Group]] = [
    {Group.FUNCTIONAL, Group.STRESS},
    {Group.TIMING},
]


@dataclasses.dataclass
class Test:
    id: int
    name: str
    section: str
    group: Group
    cmd: Optional[list[str]]
    log_file: str
    expect_failure: bool = False
    depends_on: Optional[int] = None
    check: Optional[Callable[[], tuple[bool, str]]] = None


@dataclasses.dataclass
class Result:
    test: Test
    ok: bool
    detail: str
    start: float
    end: float

    @property
    def elapsed(self) -> float:
        return self.end - self.start


@dataclasses.dataclass
class Section:
    name: str
    start: Optional[Callable[[], subprocess.Popen]]
    startup_wait_s: float = 0.0
    process: Optional[subprocess.Popen] = None
    tests: list[Test] = dataclasses.field(default_factory=list)


class Manifest:
    """Assigns stable global ids to tests as they're declared."""

    def __init__(self):
        self._next_id = 1
        self.sections: list[Section] = []

    def section(self, name: str, start=None, startup_wait_s: float = 0.0) -> "SectionBuilder":
        s = Section(name=name, start=start, startup_wait_s=startup_wait_s)
        self.sections.append(s)
        return SectionBuilder(self, s)

    def all_tests(self) -> list[Test]:
        return [t for s in self.sections for t in s.tests]


class SectionBuilder:
    def __init__(self, manifest: Manifest, section: Section):
        self.manifest = manifest
        self.section = section

    def test(self, name: str, cmd: Optional[list[str]], group: Group, *,
             expect_failure: bool = False, depends_on: Optional[int] = None,
             check: Optional[Callable[[], tuple[bool, str]]] = None) -> Test:
        tid = self.manifest._next_id
        self.manifest._next_id += 1
        slug = re.sub(r"[^a-zA-Z0-9]+", "_", name).strip("_").lower()
        log_file = str(LOG_DIR / f"{tid}_{slug}.log")
        t = Test(id=tid, name=name, section=self.section.name, group=group, cmd=cmd,
                 log_file=log_file, expect_failure=expect_failure, depends_on=depends_on,
                 check=check)
        self.section.tests.append(t)
        return t


# ---------------------------------------------------------------------------
# Platform / process helpers
# ---------------------------------------------------------------------------

TEST_DIR: Path
LOG_DIR: Path

_next_port = [44818]
_port_lock = threading.Lock()


def alloc_port() -> int:
    with _port_lock:
        p = _next_port[0]
        _next_port[0] += 1
        return p


def exe(name: str) -> str:
    for candidate in (TEST_DIR / name, TEST_DIR / f"{name}.exe"):
        if candidate.exists():
            return str(candidate)
    raise FileNotFoundError(f"{name} not found in {TEST_DIR}")


def raise_fd_limit(n: int = 1024) -> None:
    if os.name != "posix":
        return
    try:
        import resource
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        resource.setrlimit(resource.RLIMIT_NOFILE, (min(n, hard), hard))
    except Exception:
        pass


def spawn(cmd: list[str], log_path: Path) -> subprocess.Popen:
    # Long-lived servers must not share this script's session: ab_server (and
    # friends) install no SIGHUP handler, so if they inherit our controlling
    # terminal/session they die silently the moment it goes away -- observed
    # directly here as servers vanishing from `ps` mid-run with no error in
    # their own log. start_new_session (POSIX) / CREATE_NEW_PROCESS_GROUP
    # (Windows) detaches them so they survive independent of how this script
    # itself was launched (backgrounded, under a job runner, etc).
    log = open(log_path, "w")
    kwargs = {}
    if os.name == "posix":
        kwargs["start_new_session"] = True
    else:
        kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    return subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, **kwargs)


def stop_process(proc: Optional[subprocess.Popen]) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


REQUIRED_EXECUTABLES = [
    "ab_server", "modbus_server", "list_tags_logix", "string_non_standard_udt", "string_standard",
    "tag_rw2", "test_connection_stress", "test_create_from_tag", "test_connection_tag",
    "test_connection_tag_late_join", "test_fairness", "test_auto_sync", "test_callback",
    "test_callback_ex", "test_callback_ex_logix", "test_callback_ex_modbus", "test_idle_disconnect",
    "test_modbus_multiple", "test_omron_destroy", "test_raw_cip", "test_reconnect_after_outage_async",
    "test_reconnect_after_outage_sync", "test_shutdown_cip", "test_shutdown_modbus",
    "test_shutdown_restart", "test_special", "test_string", "test_tag_attributes",
    "test_tag_type_attribute", "thread_stress", "stress_rc_mem", "test_indexed_tags",
]


def check_executables_present() -> None:
    missing = [n for n in REQUIRED_EXECUTABLES if not (TEST_DIR / n).exists() and not (TEST_DIR / f"{n}.exe").exists()]
    if missing:
        print(f"Missing executables in {TEST_DIR}: {', '.join(missing)}")
        sys.exit(1)


def make_plc_count_check(log_path: str, expected: int = 2) -> Callable[[], tuple[bool, str]]:
    def check() -> tuple[bool, str]:
        pattern = re.compile(r"Creating new PLC connection\.")
        count = 0
        try:
            with open(log_path, "r", errors="replace") as f:
                count = sum(1 for line in f if pattern.search(line))
        except FileNotFoundError:
            return False, f"log file not found: {log_path}"
        if count == expected:
            return True, f"found {count} PLC creation entries in {log_path}"
        return False, f"expected {expected} PLC creation entries, found {count} in {log_path}"
    return check


# ---------------------------------------------------------------------------
# Manifest: sections + tests (mirrors run_simulator_tests.sh 1:1)
# ---------------------------------------------------------------------------

def build_manifest() -> Manifest:
    m = Manifest()
    F, S, T = Group.FUNCTIONAL, Group.STRESS, Group.TIMING

    # --- ControlLogix "fast" section ---------------------------------------
    port = alloc_port()
    gw = f"127.0.0.1:{port}"

    def start_fast(port=port):
        return spawn([exe("ab_server"), "--debug", "--plc=ControlLogix", f"--port={port}",
                      "--path=1,0", "--tag=TestBigArray:DINT[2000]", "--tag=Test_Array_1:DINT[1000]",
                      "--tag=Test_Array_2x3:DINT[2,3]", "--tag=Test_Array_2x3x4:DINT[2,3,4]"],
                     LOG_DIR / "logix_fast_emulator.log")

    sec = m.section("controllogix_fast", start=start_fast, startup_wait_s=3)
    sec.test("basic unconnected tag read/write",
              [exe("tag_rw2"), "--type=sint32",
               f"--tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&elem_count=10&name=TestBigArray&use_connected_msg=0",
               "--debug=4", "--write=1,2,3,4,5,6,7,8,9"], F)
    sec.test("basic unconnected large tag read/write",
              [exe("tag_rw2"), "--type=sint32",
               f"--tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&elem_count=1000&name=TestBigArray&use_connected_msg=0",
               "--debug=4", "--write=1,2,3,4,5,6,7,8,9"], F)
    sec.test("basic connected tag read/write",
              [exe("tag_rw2"), "--type=sint32",
               f"--tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&elem_count=10&name=TestBigArray",
               "--debug=4", "--write=1,2,3,4,5,6,7,8,9"], F)
    sec.test("basic connected large tag read/write",
              [exe("tag_rw2"), "--type=sint32",
               f"--tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&elem_count=1000&name=TestBigArray",
               "--debug=4", "--write=1,2,3,4,5,6,7,8,9"], F)
    sec.test("stress RC memory code", [exe("stress_rc_mem")], S)
    sec.test("CIP thread stress",
              [exe("thread_stress"), "20", f"protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&name=TestBigArray"], S)
    sec.test("auto sync", [exe("test_auto_sync")], T)
    sec.test("indexed tags", [exe("test_indexed_tags")], F)
    sec.test("AB/ControlLogix tag scheduling fairness",
              [exe("test_fairness"), f"--tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&name=TestBigArray[0]&auto_sync_read_ms=200",
               "--num-tags=200", "--test-duration-secs=10"], S)
    sec.test("idle disconnect and reconnect with runtime timeout change (AB ControlLogix)",
              [exe("test_idle_disconnect"), f"--tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&name=TestBigArray"], T)
    sec.test("hard library shutdown", [exe("test_shutdown_cip")], F)
    sec.test("library shutdown and restart",
              [exe("test_shutdown_restart"), f"--tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray"], F)
    sec.test("connection tag connection state transitions (ControlLogix)",
              [exe("test_connection_tag"), f"--tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&connection_inactivity_timeout_ms=5000&name=@connection"], T)
    sec.test("multiple simultaneous @connection tags on same session (ControlLogix)",
              [exe("test_connection_tag"),
               f"--tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&name=@connection",
               "--num-tags=3",
               f"--data-tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray[0]"], F)
    sec.test("@connection tag late join -- session already UP before tag created (ControlLogix)",
              [exe("test_connection_tag_late_join"),
               f"--data-tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray[0]",
               f"--tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&name=@connection",
               "--timeout=5000"], F)
    sec.test("@connection tag 2-cycle reconnect (5 s idle timeout, ControlLogix)",
              [exe("test_connection_tag"),
               f"--tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&name=@connection",
               "--cycles=2",
               f"--data-tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray[0]",
               "--idle-timeout-ms=5000"], T)
    sec.test("create-from-tag API (17 permutation tests with AB/EIP)",
              [exe("test_create_from_tag"),
               f"--src-tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray[0]",
               "--clone-attrib=name=TestBigArray[1]&elem_count=1",
               f"--connection-tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&name=@connection",
               "--timeout=10000"], F)

    # --- ControlLogix slot-16 path encoding section -------------------------
    port16 = alloc_port()
    gw16 = f"127.0.0.1:{port16}"

    def start_slot16(port=port16):
        return spawn([exe("ab_server"), "--debug", "--plc=ControlLogix", f"--port={port}",
                      "--path=1,16", "--tag=TestBigArray:DINT[2000]"],
                     LOG_DIR / "logix_slot16_emulator.log")

    sec = m.section("controllogix_slot16", start=start_slot16, startup_wait_s=1)
    sec.test("unconnected tag read/write through chassis slot 16 (path=1,16)",
              [exe("tag_rw2"), "--type=sint32",
               f"--tag=protocol=ab-eip&gateway={gw16}&path=1,16&plc=ControlLogix&elem_count=10&name=TestBigArray&use_connected_msg=0",
               "--debug=4", "--write=1,2,3,4,5,6,7,8,9"], F)
    sec.test("connected tag read/write through chassis slot 16 (path=1,16)",
              [exe("tag_rw2"), "--type=sint32",
               f"--tag=protocol=ab-eip&gateway={gw16}&path=1,16&plc=ControlLogix&elem_count=10&name=TestBigArray",
               "--debug=4", "--write=1,2,3,4,5,6,7,8,9"], F)

    # --- Stand-alone section: no shared server ------------------------------
    # ERR_WAIT needs a port nothing is listening on; async/sync manage their
    # own ab_server internally (they take the ab_server path + port as argv).
    unreachable_port = alloc_port()
    async_port = alloc_port()
    sync_port = alloc_port()

    sec = m.section("standalone", start=None)
    sec.test("@connection tag ERR_WAIT on unreachable host (no server running)",
              [exe("test_connection_tag"),
               f"--tag=protocol=ab-eip&gateway=127.0.0.1:{unreachable_port}&path=1,0&plc=ControlLogix&name=@connection",
               "--expect-err"], T)
    sec.test("Test async reconnect after PLC outage",
              [exe("test_reconnect_after_outage_async"), exe("ab_server"), str(async_port)], T)
    sec.test("Test sync reconnect after PLC outage",
              [exe("test_reconnect_after_outage_sync"), exe("ab_server"), str(sync_port)], T)

    # --- ControlLogix functional/slow (delay=100) section -------------------
    slow_port = alloc_port()

    def start_slow(port=slow_port):
        return spawn([exe("ab_server"), f"--port={port}", "--plc=ControlLogix", "--path=1,0",
                      "--tag=TestBigArray:DINT[2000]", "--tag=Test_Array_1:DINT[1000]",
                      "--tag=Test_Array_2x3:DINT[2,3]", "--tag=Test_Array_2x3x4:DINT[2,3,4]", "--delay=100"],
                     LOG_DIR / "logix_slow_emulator.log")

    sec = m.section("controllogix_slow", start=start_slow, startup_wait_s=1)
    sec.test("emulator test callbacks",
              [exe("test_callback"), f"--tag=protocol=ab-eip&gateway=127.0.0.1:{slow_port}&path=1,0&cpu=LGX&elem_count=10&name=TestBigArray"], F)
    sec.test("emulator test extended callbacks sync", [exe("test_callback_ex")], F)
    sec.test("emulator test extended callbacks async",
              [exe("test_callback_ex_logix"), f"--tag=protocol=ab-eip&gateway=127.0.0.1:{slow_port}&path=1,0&cpu=LGX&elem_count=10&name=TestBigArray"], F)

    # --- Micro800 section ----------------------------------------------------
    m800_port = alloc_port()

    def start_micro800(port=m800_port):
        return spawn([exe("ab_server"), "--debug", "--plc=Micro800", f"--port={port}", "--tag=TestDINTArray:DINT[10]"],
                     LOG_DIR / "micro800_emulator.log")

    sec = m.section("micro800", start=start_micro800, startup_wait_s=1)
    sec.test("basic Micro800 read/write",
              [exe("tag_rw2"), "--type=sint32", f"--tag=protocol=ab-eip&gateway=127.0.0.1:{m800_port}&plc=micro800&name=TestDINTArray",
               "--write=42", "--debug=4"], F)

    # --- Omron section ---------------------------------------------------------
    omron_port = alloc_port()
    ogw = f"127.0.0.1:{omron_port}"

    def start_omron(port=omron_port):
        return spawn([exe("ab_server"), "--debug", "--plc=Omron", f"--port={port}", "--tag=TestDINTArray:DINT[10]"],
                     LOG_DIR / "omron_emulator.log")

    sec = m.section("omron", start=start_omron, startup_wait_s=1)
    sec.test("basic Omron read/write",
              [exe("tag_rw2"), "--type=sint32", f"--tag=protocol=ab-eip&gateway={ogw}&path=18,127.0.0.1&plc=omron-njnx&name=TestDINTArray",
               "--write=42", "--debug=4"], F)
    sec.test("Omron thread stress",
              [exe("thread_stress"), "10", f"protocol=ab-eip&gateway={ogw}&path=18,127.0.0.1&plc=omron-njnx&name=TestDINTArray"], S)
    sec.test("idle disconnect and reconnect with runtime timeout change (Omron)",
              [exe("test_idle_disconnect"), f"--tag=protocol=ab-eip&gateway={ogw}&path=18,127.0.0.1&plc=omron-njnx&name=TestDINTArray"], T)
    sec.test("@connection tag connection state transitions (Omron)",
              [exe("test_connection_tag"), f"--tag=protocol=ab-eip&gateway={ogw}&path=18,127.0.0.1&plc=omron-njnx&connection_inactivity_timeout_ms=5000&name=@connection"], T)
    sec.test("multiple simultaneous @connection tags on same session (Omron)",
              [exe("test_connection_tag"),
               f"--tag=protocol=ab-eip&gateway={ogw}&path=18,127.0.0.1&plc=omron-njnx&name=@connection",
               "--num-tags=3",
               f"--data-tag=protocol=ab-eip&gateway={ogw}&path=18,127.0.0.1&plc=omron-njnx&elem_count=1&name=TestDINTArray[0]"], F)
    sec.test("@connection tag 2-cycle reconnect (5 s idle timeout, Omron)",
              [exe("test_connection_tag"),
               f"--tag=protocol=ab-eip&gateway={ogw}&path=18,127.0.0.1&plc=omron-njnx&name=@connection",
               "--cycles=2",
               f"--data-tag=protocol=ab-eip&gateway={ogw}&path=18,127.0.0.1&plc=omron-njnx&elem_count=1&name=TestDINTArray[0]",
               "--idle-timeout-ms=5000"], T)
    sec.test("@connection tag late join (Omron)",
              [exe("test_connection_tag_late_join"),
               f"--data-tag=protocol=ab-eip&gateway={ogw}&path=18,127.0.0.1&plc=omron-njnx&elem_count=1&name=TestDINTArray[0]",
               f"--tag=protocol=ab-eip&gateway={ogw}&path=18,127.0.0.1&plc=omron-njnx&name=@connection",
               "--timeout=5000"], F)
    sec.test("connection stress (multiple connections) Omron",
              [exe("test_connection_stress"), "--num-threads=200",
               f"--tag=protocol=ab-eip&gateway={ogw}&path=18,127.0.0.1&plc=omron-njnx&name=TestDINTArray"], S)
    sec.test("Omron tag scheduling fairness",
              [exe("test_fairness"), f"--tag=protocol=ab-eip&gateway={ogw}&path=18,127.0.0.1&plc=omron-njnx&name=TestDINTArray[0]&auto_sync_read_ms=200",
               "--num-tags=200", "--test-duration-secs=10"], S)
    sec.test("emulator test extended callbacks async (Omron)",
              [exe("test_callback_ex_logix"), f"--tag=protocol=ab-eip&gateway={ogw}&path=18,127.0.0.1&plc=omron-njnx&elem_count=10&name=TestDINTArray"], F)
    sec.test("create-from-tag API (17 permutation tests with Omron)",
              [exe("test_create_from_tag"),
               f"--src-tag=protocol=ab-eip&gateway={ogw}&path=18,127.0.0.1&plc=omron-njnx&elem_count=1&name=TestDINTArray[0]",
               "--clone-attrib=name=TestDINTArray[1]&elem_count=1",
               f"--connection-tag=protocol=ab-eip&gateway={ogw}&path=18,127.0.0.1&plc=omron-njnx&name=@connection",
               "--timeout=10000"], F)
    sec.test("plc_tag_destroy does not hang after Omron connection loss (issue #625)",
              [exe("test_omron_destroy"), exe("ab_server")], F)

    # --- Micrologix section ----------------------------------------------------
    mlgx_port = alloc_port()

    def start_micrologix(port=mlgx_port):
        return spawn([exe("ab_server"), "--debug", "--plc=Micrologix", f"--port={port}",
                      "--tag=B3[10]", "--tag=N7[10]", "--tag=L19[10]"],
                     LOG_DIR / "micrologix_emulator.log")

    sec = m.section("micrologix", start=start_micrologix, startup_wait_s=1)
    mgw = f"127.0.0.1:{mlgx_port}"
    sec.test("B data file Micrologix tag read/write",
              [exe("tag_rw2"), "--type=uint16", f"--tag=protocol=ab-eip&gateway={mgw}&plc=micrologix&name=B3:0", "--write=0", "--debug=4"], F)
    sec.test("B bit data file Micrologix tag read/write",
              [exe("tag_rw2"), "--type=bit", f"--tag=protocol=ab-eip&gateway={mgw}&plc=micrologix&name=B3:0/6", "--write=1", "--debug=4"], F)
    sec.test("N data file Micrologix tag read/write",
              [exe("tag_rw2"), "--type=sint16", f"--tag=protocol=ab-eip&gateway={mgw}&plc=micrologix&name=N7:0", "--write=42", "--debug=4"], F)
    sec.test("N bit data file Micrologix tag read/write",
              [exe("tag_rw2"), "--type=bit", f"--tag=protocol=ab-eip&gateway={mgw}&plc=micrologix&name=N7:0/10", "--write=1", "--debug=4"], F)
    sec.test("L data file Micrologix tag read/write",
              [exe("tag_rw2"), "--type=sint32", f"--tag=protocol=ab-eip&gateway={mgw}&plc=micrologix&name=L19:0", "--write=0,1,2,3", "--debug=4"], F)
    sec.test("L bit data file Micrologix tag read",
              [exe("tag_rw2"), "--type=bit", f"--tag=protocol=ab-eip&gateway={mgw}&plc=micrologix&name=L19:0/23", "--debug=4"], F)
    sec.test("L bit data file Micrologix tag write",
              [exe("tag_rw2"), "--type=bit", f"--tag=protocol=ab-eip&gateway={mgw}&plc=micrologix&name=L19:0/23", "--write=1", "--debug=4"],
              F, expect_failure=True)  # this write should NOT succeed

    # --- PLC5 section ------------------------------------------------------
    plc5_port = alloc_port()

    def start_plc5(port=plc5_port):
        return spawn([exe("ab_server"), "--debug", "--plc=PLC/5", f"--port={port}", "--tag=B3[10]", "--tag=N7[10]"],
                     LOG_DIR / "plc5_emulator.log")

    sec = m.section("plc5", start=start_plc5, startup_wait_s=1)
    pgw = f"127.0.0.1:{plc5_port}"
    sec.test("B data file PLC5 tag read/write",
              [exe("tag_rw2"), "--type=uint16", f"--tag=protocol=ab-eip&gateway={pgw}&plc=plc5&elem_count=1&name=B3:0", "--debug=4", "--write=0"], F)
    sec.test("B bit data file PLC5 tag read/write",
              [exe("tag_rw2"), "--type=bit", f"--tag=protocol=ab-eip&gateway={pgw}&plc=plc5&elem_count=1&name=B3:0/10", "--debug=4", "--write=1"], F)
    sec.test("N data file PLC5 tag read/write",
              [exe("tag_rw2"), "--type=sint16", f"--tag=protocol=ab-eip&gateway={pgw}&plc=plc5&elem_count=1&name=N7:0", "--debug=4", "--write=0"], F)
    sec.test("N bit data file PLC5 tag read/write",
              [exe("tag_rw2"), "--type=bit", f"--tag=protocol=ab-eip&gateway={pgw}&plc=plc5&elem_count=1&name=N7:0/10", "--debug=4", "--write=1"], F)

    # --- Modbus section ------------------------------------------------------
    modbus_port = alloc_port()
    modbus_port2 = alloc_port()

    def start_modbus(p1=modbus_port, p2=modbus_port2):
        return spawn([exe("modbus_server"), f"--listen=127.0.0.1:{p1}", f"--listen=127.0.0.1:{p2}", "--debug=DETAIL"],
                     LOG_DIR / "modbus_server.log")

    sec = m.section("modbus", start=start_modbus, startup_wait_s=3)
    mbgw = f"127.0.0.1:{modbus_port}"
    mbgw2 = f"127.0.0.1:{modbus_port2}"
    sec.test("connection tag connection state transitions (Modbus)",
              [exe("test_connection_tag"), f"--tag=protocol=modbus-tcp&gateway={mbgw}&path=0&connection_inactivity_timeout_ms=5000&name=@connection"], T)
    sec.test("multiple simultaneous @connection tags on same session (Modbus)",
              [exe("test_connection_tag"),
               f"--tag=protocol=modbus-tcp&gateway={mbgw}&path=0&name=@connection",
               "--num-tags=3",
               f"--data-tag=protocol=modbus-tcp&gateway={mbgw}&path=0&elem_count=2&name=hr10"], F)
    sec.test("@connection tag 2-cycle reconnect (5 s idle timeout, Modbus)",
              [exe("test_connection_tag"),
               f"--tag=protocol=modbus-tcp&gateway={mbgw}&path=0&name=@connection",
               "--cycles=2",
               f"--data-tag=protocol=modbus-tcp&gateway={mbgw}&path=0&elem_count=2&name=hr10",
               "--idle-timeout-ms=5000"], T)
    sec.test("test idle disconnect with Modbus",
              [exe("test_idle_disconnect"), f"--tag=protocol=modbus-tcp&gateway={mbgw}&path=0&elem_count=2&name=hr10"], T)
    sec.test("thread stress Modbus",
              [exe("thread_stress"), "10", f"protocol=modbus-tcp&gateway={mbgw}&path=0&elem_count=2&name=hr10"], S)
    sec.test("connection stress (multiple connections) Modbus",
              [exe("test_connection_stress"), "--num-threads=200",
               f"--tag=protocol=modbus-tcp&gateway={mbgw}&path=0&elem_count=2&name=hr10"], S)
    sec.test("callback events Modbus",
              [exe("test_callback_ex_modbus"), f"--tag=protocol=modbus-tcp&gateway={mbgw}&path=0&elem_count=10&name=hr1"], F)
    sec.test("@connection tag late join (Modbus)",
              [exe("test_connection_tag_late_join"),
               f"--data-tag=protocol=modbus-tcp&gateway={mbgw}&path=0&elem_count=2&name=hr10",
               f"--tag=protocol=modbus-tcp&gateway={mbgw}&path=0&name=@connection",
               "--timeout=5000"], F)
    sec.test("create-from-tag API (17 permutation tests with Modbus)",
              [exe("test_create_from_tag"),
               f"--src-tag=protocol=modbus-tcp&gateway={mbgw}&path=0&elem_count=2&name=hr10",
               "--clone-attrib=name=hr20&elem_count=2",
               f"--connection-tag=protocol=modbus-tcp&gateway={mbgw}&path=0&name=@connection",
               "--timeout=10000"], F)
    sec.test("hard library shutdown (Modbus)",
              [exe("test_shutdown_modbus"),
               f"--tag=protocol=modbus-tcp&gateway={mbgw}&path=0&elem_count=1&name=hr5&auto_sync_read_ms=200&auto_sync_write_ms=20"], F)
    sec.test("Modbus tag scheduling fairness",
              [exe("test_fairness"), f"--tag=protocol=modbus-tcp&gateway={mbgw}&path=1&name=hr10&auto_sync_read_ms=200",
               "--num-tags=200", "--test-duration-secs=10"], S)
    reconnect_test = sec.test("for Modbus reconnect bug",
              [exe("test_modbus_multiple"), f"--gateway1={mbgw}", f"--gateway2={mbgw2}"], T)
    # Depends on the reconnect test's own log; must run after it, not concurrently.
    sec.test("check for exactly 2 PLC creation entries in Modbus reconnect test log",
              None, T, depends_on=reconnect_test.id, check=make_plc_count_check(reconnect_test.log_file))

    return m


# ---------------------------------------------------------------------------
# Execution
# ---------------------------------------------------------------------------

print_lock = threading.Lock()

# Each STRESS test already opens up to 200 of its own connections/threads.
# Letting several run at once (as the wide FUNCTIONAL+STRESS pool otherwise
# would) starves everything else on modest-core / shared machines badly
# enough that plain reads start missing their own default timeouts -- this
# was observed directly (test_connection_stress hit PLCTAG_ERR_TIMEOUT
# creating one of 200 tags, and unrelated functional reads in the Micrologix/
# PLC5 sections timed out) the first time this script ran with no cap here.
stress_limiter: threading.BoundedSemaphore


def run_test(test: Test, futures_by_id: dict[int, "Future"]) -> Result:
    if test.depends_on is not None:
        futures_by_id[test.depends_on].result()

    start = time.monotonic()
    if test.cmd is None:
        ok, detail = test.check()
    else:
        limiter = stress_limiter if test.group == Group.STRESS else None
        if limiter is not None:
            limiter.acquire()
        try:
            with open(test.log_file, "w") as log:
                proc = subprocess.run(test.cmd, stdout=log, stderr=subprocess.STDOUT)
        finally:
            if limiter is not None:
                limiter.release()
        ok = (proc.returncode == 0) != test.expect_failure
        detail = ""
    end = time.monotonic()

    with print_lock:
        status = "OK" if ok else "FAILURE"
        suffix = f" ({detail})" if detail else ""
        print(f"  [{test.section}] test {test.id} ({test.group.value}): {test.name}... "
              f"{status} ({end - start:.0f}s){suffix}")

    return Result(test=test, ok=ok, detail=detail, start=start, end=end)


def run_phase(tests: list[Test], max_workers: int, futures_by_id: dict[int, "Future"]) -> tuple[list[Result], float]:
    if not tests:
        return [], 0.0
    phase_start = time.monotonic()
    results = []
    with ThreadPoolExecutor(max_workers=max_workers) as pool:
        futs: dict[int, Future] = {}
        for t in tests:
            fut = pool.submit(run_test, t, futures_by_id)
            futs[t.id] = fut
            futures_by_id[t.id] = fut
        for fut in futs.values():
            results.append(fut.result())
    phase_end = time.monotonic()
    return results, phase_end - phase_start


def main() -> int:
    global TEST_DIR, LOG_DIR

    parser = argparse.ArgumentParser()
    parser.add_argument("test_dir")
    parser.add_argument("log_dir", nargs="?", default=".")
    parser.add_argument("--max-workers", type=int, default=max(2, (os.cpu_count() or 4) // 2),
                         help="worker threads for the functional+stress phase")
    parser.add_argument("--timing-workers", type=int, default=8,
                         help="worker threads for the timing phase")
    parser.add_argument("--max-stress", type=int, default=2,
                         help="max STRESS-group tests allowed to run at once, regardless of --max-workers "
                              "(each one already opens up to 200 of its own connections/threads)")
    parser.add_argument("--server-stagger-s", type=float, default=2.0,
                         help="delay between starting each section's server (see comment at the call site)")
    args = parser.parse_args()

    global stress_limiter
    stress_limiter = threading.BoundedSemaphore(args.max_stress)

    TEST_DIR = Path(args.test_dir)
    LOG_DIR = Path(args.log_dir)
    LOG_DIR.mkdir(parents=True, exist_ok=True)

    if not TEST_DIR.is_dir():
        print(f"{TEST_DIR} is not a valid path for test executables!")
        return 1

    check_executables_present()
    raise_fd_limit(1024)

    script_start = time.monotonic()

    manifest = build_manifest()

    # Start every section's server up front; they're all on distinct ports
    # so there's no collision even though they all run concurrently now.
    #
    # Launching them back-to-back with no gap was observed to get all of them
    # killed (SIGTERM) in a single burst a few dozen seconds in, on this
    # machine -- reproduced down to a minimal repro of just spawning many
    # listening-socket processes at once (a plain `sleep` child launched the
    # same way was unaffected, and staggering the launches by a few seconds
    # made the problem disappear entirely). That points at host-level
    # security/monitoring tooling reacting to a burst of new listeners, not a
    # bug in this script or in ab_server -- the stagger below is a cheap,
    # harmless way to avoid tripping it.
    servers = [s for s in manifest.sections if s.start is not None]
    for s in servers:
        s.process = s.start()
        if s.process.poll() is not None:
            print(f"Server for section '{s.name}' exited immediately!")
            return 1
        time.sleep(args.server_stagger_s)

    if servers:
        time.sleep(max((s.startup_wait_s for s in servers), default=0))
        for s in servers:
            if s.process.poll() is not None:
                print(f"Server for section '{s.name}' exited during startup!")
                for other in servers:
                    stop_process(other.process)
                return 1

    all_tests = manifest.all_tests()
    futures_by_id: dict[int, Future] = {}
    all_results: list[Result] = []
    phase_times: list[float] = []

    for phase_groups in PHASES:
        phase_tests = [t for t in all_tests if t.group in phase_groups]
        workers = args.timing_workers if phase_groups == {Group.TIMING} else args.max_workers
        results, elapsed = run_phase(phase_tests, workers, futures_by_id)
        all_results.extend(results)
        phase_times.append(elapsed)

    for s in servers:
        stop_process(s.process)

    total = time.monotonic() - script_start

    all_results.sort(key=lambda r: r.test.id)
    failures = sum(1 for r in all_results if not r.ok)

    print("\nSection timing (span from first test start to last test end in that section):")
    for s in manifest.sections:
        section_results = [r for r in all_results if r.test.section == s.name]
        if not section_results:
            continue
        span = max(r.end for r in section_results) - min(r.start for r in section_results)
        print(f"  {s.name}: {span:.0f}s")

    print("\nPhase timing:")
    for phase_groups, elapsed in zip(PHASES, phase_times):
        names = "+".join(g.value for g in phase_groups)
        print(f"  {names}: {elapsed:.0f}s")

    print("\nResults:")
    print(f" - {len(all_results)} tests.")
    print(f" - {len(all_results) - failures} successes.")
    print(f" - {failures} failures.")
    print(f" - Total time: {total:.0f}s")

    if failures:
        print("\nFailed tests:")
        for r in all_results:
            if not r.ok:
                print(f"  test {r.test.id}: {r.test.name} (log: {r.test.log_file})")

    return failures


if __name__ == "__main__":
    sys.exit(main())

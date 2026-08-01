#!/usr/bin/env python3
"""Parallel-aware simulator test runner.

Each test is a fully self-contained unit: its own server instance (if it
needs one) plus its own client process, on its own dynamically-assigned
port(s). A pool of worker *processes* -- one per CPU by default -- pulls
tests off a flat list and runs each one's complete lifecycle (start server,
run client, stop server) before picking up the next. This replaces an
earlier design where one long-lived server was shared by every test in a
"section" via a thread pool; full per-test isolation costs an extra server
start per test but removes any chance of one test's server state leaking
into another's, and lets scheduling spread across all cores instead of a
handful of threads.

Tests are tagged with a Group: FUNCTIONAL (correctness, fast, order/timing
doesn't matter), STRESS (many threads/tags -- parallel-safe but CPU-heavy),
or TIMING (depends on hardcoded waits/timeouts, sensitive to CPU contention
from neighbors). FUNCTIONAL+STRESS run together in one phase; TIMING runs
alone in a second phase so stress tests can't stall its timers. STRESS tests
are additionally capped by a semaphore independent of worker-pool size,
since each one already opens up to 200 of its own threads/connections --
letting one run per CPU would starve everything else on modest-core
machines.

Usage matches the bash script: run_simulator_tests_parallel.py TEST_DIR [LOG_DIR]
"""

import argparse
import dataclasses
import enum
import os
import re
import socket
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from multiprocessing import get_context
from pathlib import Path
from typing import Optional


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

_UNSET = object()


@dataclasses.dataclass(frozen=True)
class ServerSpec:
    """A server to start for a test. args_template entries may contain the
    literal placeholders "{PORT}" / "{PORT2}", filled in per-test at run
    time once that test's worker has allocated its own port(s)."""
    exe_path: str
    args_template: list[str]
    startup_wait_s: float = 1.0


@dataclasses.dataclass(frozen=True)
class CheckSpec:
    """A post-hoc log check, run inline in the main process instead of a
    worker (no subprocess involved -- just counts regex matches in a file
    another test already produced)."""
    log_file: str
    pattern: str
    expected: int


@dataclasses.dataclass(frozen=True)
class Test:
    id: int
    name: str
    section: str
    group: Group
    cmd_template: Optional[list[str]]  # None for check-only tests
    log_file: str
    server: Optional[ServerSpec] = None
    server_log_file: Optional[str] = None
    ports_needed: int = 0
    expect_failure: bool = False
    # A handful of test binaries take no arguments and are hardcoded to
    # connect to the library's compiled-in default gateway (127.0.0.1 with
    # no port -> the default EtherNet/IP port, DEFAULT_LIB_PORT). Their
    # server must be pinned to that literal port instead of a dynamically
    # allocated one, and since that port can't be shared concurrently across
    # more than one such test, they're serialized via a dedicated semaphore.
    exclusive_default_port: bool = False
    # depends_on is metadata only: the one dependent test in this manifest is
    # a CheckSpec test, which always runs after its whole phase's pool has
    # drained, so the dependency is satisfied by construction.
    depends_on: Optional[int] = None
    check: Optional[CheckSpec] = None


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


class Manifest:
    """Assigns stable global ids to tests as they're declared."""

    def __init__(self):
        self._next_id = 1
        self.tests: list[Test] = []
        self.section_names: list[str] = []

    def section(self, name: str, server: Optional[ServerSpec] = None) -> "SectionBuilder":
        self.section_names.append(name)
        return SectionBuilder(self, name, server)


def _server_ports_needed(server: Optional[ServerSpec]) -> int:
    if server is None:
        return 0
    return 2 if any("{PORT2}" in a for a in server.args_template) else 1


class SectionBuilder:
    def __init__(self, manifest: Manifest, name: str, server: Optional[ServerSpec]):
        self.manifest = manifest
        self.name = name
        self.server = server

    def test(self, name: str, cmd_template: Optional[list[str]], group: Group, *,
              ports_needed: Optional[int] = None, server=_UNSET,
              expect_failure: bool = False, depends_on: Optional[int] = None,
              check: Optional[CheckSpec] = None, exclusive_default_port: bool = False) -> Test:
        tid = self.manifest._next_id
        self.manifest._next_id += 1
        slug = re.sub(r"[^a-zA-Z0-9]+", "_", name).strip("_").lower()
        log_file = str(LOG_DIR / f"{tid}_{slug}.log")

        srv = self.server if server is _UNSET else server
        if ports_needed is None:
            ports_needed = _server_ports_needed(srv)
        server_log_file = str(LOG_DIR / f"{tid}_{slug}_server.log") if srv else None

        t = Test(id=tid, name=name, section=self.name, group=group, cmd_template=cmd_template,
                  log_file=log_file, server=srv, server_log_file=server_log_file,
                  ports_needed=ports_needed, expect_failure=expect_failure,
                  exclusive_default_port=exclusive_default_port,
                  depends_on=depends_on, check=check)
        self.manifest.tests.append(t)
        return t


# ---------------------------------------------------------------------------
# Platform / process helpers (used both in the main process and in workers)
# ---------------------------------------------------------------------------

TEST_DIR: Path


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
    # Long-lived servers must not share their launching process's session:
    # ab_server (and friends) install no SIGHUP handler, so if they inherit a
    # controlling terminal/session they can die silently the moment it goes
    # away. start_new_session (POSIX) / CREATE_NEW_PROCESS_GROUP (Windows)
    # detaches them so they survive independent of how they were launched.
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


# ab_server/modbus_server processes left running from an earlier, unrelated
# invocation (e.g. an interrupted prior run, or manual debugging) can squat
# on a port this run's allocator later hands to one of its own tests. That
# test's own server then fails to bind, but the client still connects --
# to the stale process instead -- and gets served against whatever tag
# config that old process happened to be started with. That produces a
# confusing, mis-attributed protocol-level test failure instead of an
# obvious "port already in use" one. Best-effort sweep before and after a
# run closes that hole; it's a no-op if nothing stale is running.
def kill_stray_servers() -> None:
    names = ["ab_server", "modbus_server"]
    if os.name == "posix":
        for name in names:
            subprocess.run(["pkill", "-f", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        for name in names:
            subprocess.run(["taskkill", "/IM", f"{name}.exe", "/F"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


REQUIRED_EXECUTABLES = [
    "ab_server", "modbus_server", "list_tags_logix", "string_non_standard_udt", "string_standard",
    "tag_rw2", "test_connection_stress", "test_create_from_tag", "test_connection_tag",
    "test_connection_tag_late_join", "test_fairness", "test_auto_sync", "test_callback",
    "test_callback_ex", "test_callback_ex_async", "test_idle_disconnect",
    "test_modbus_multiple", "test_omron_destroy", "test_raw_cip", "test_reconnect_after_outage_async",
    "test_reconnect_after_outage_sync", "test_shutdown",
    "test_shutdown_restart", "test_special", "test_string", "test_tag_attributes",
    "test_tag_type_attribute", "thread_stress", "stress_rc_mem", "test_indexed_tags",
]


def check_executables_present() -> None:
    missing = [n for n in REQUIRED_EXECUTABLES if not (TEST_DIR / n).exists() and not (TEST_DIR / f"{n}.exe").exists()]
    if missing:
        print(f"Missing executables in {TEST_DIR}: {', '.join(missing)}")
        sys.exit(1)


def run_check(check: CheckSpec) -> tuple[bool, str]:
    pattern = re.compile(check.pattern)
    count = 0
    try:
        with open(check.log_file, "r", errors="replace") as f:
            count = sum(1 for line in f if pattern.search(line))
    except FileNotFoundError:
        return False, f"log file not found: {check.log_file}"
    if count == check.expected:
        return True, f"found {count} PLC creation entries in {check.log_file}"
    return False, f"expected {check.expected} PLC creation entries, found {count} in {check.log_file}"


# ---------------------------------------------------------------------------
# Manifest: tests, flat (mirrors run_simulator_tests.sh 1:1)
# ---------------------------------------------------------------------------

# A few test binaries take no arguments and are hardcoded to connect to
# "gateway=127.0.0.1" with no port, which the library resolves to its
# compiled-in default EtherNet/IP port. Their server must be pinned to this
# literal port (see Test.exclusive_default_port), so the general-purpose
# dynamic port counter starts one above it to avoid ever handing this port
# out to an unrelated test.
DEFAULT_LIB_PORT = 44818


def build_manifest() -> Manifest:
    m = Manifest()
    F, S, T = Group.FUNCTIONAL, Group.STRESS, Group.TIMING

    # --- ControlLogix "fast" section ---------------------------------------
    fast_server = ServerSpec(
        exe_path=exe("ab_server"),
        args_template=["--debug", "--plc=ControlLogix", "--port={PORT}", "--path=1,0",
                        "--tag=TestBigArray:DINT[2000]", "--tag=Test_Array_1:DINT[1000]",
                        "--tag=Test_Array_2x3:DINT[2,3]", "--tag=Test_Array_2x3x4:DINT[2,3,4]"],
        startup_wait_s=3,
    )
    gw = "127.0.0.1:{PORT}"

    sec = m.section("controllogix_fast", server=fast_server)
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
    # These two take no arguments and are hardcoded to the library's default
    # gateway/port (they're protocol-specific by nature, not consolidation
    # candidates -- see stress_rc_mem/test_indexed_tags in the manifest
    # design notes). Give each its own server pinned to that literal port,
    # serialized against each other and against the controllogix_slow default-
    # port test below via exclusive_default_port (see Test docstring).
    fast_default_port_server = ServerSpec(
        exe_path=exe("ab_server"),
        args_template=["--debug", "--plc=ControlLogix", f"--port={DEFAULT_LIB_PORT}", "--path=1,0",
                        "--tag=TestBigArray:DINT[2000]", "--tag=Test_Array_1:DINT[1000]",
                        "--tag=Test_Array_2x3:DINT[2,3]", "--tag=Test_Array_2x3x4:DINT[2,3,4]"],
        startup_wait_s=3,
    )
    sec.test("stress RC memory code", [exe("stress_rc_mem")], S,
              server=fast_default_port_server, ports_needed=0, exclusive_default_port=True)
    sec.test("CIP thread stress",
              [exe("thread_stress"), "20", f"protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&name=TestBigArray"], S)
    sec.test("auto sync",
              [exe("test_auto_sync"),
               f"--tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray[4]&auto_sync_read_ms=600&auto_sync_write_ms=20"], T)
    sec.test("indexed tags", [exe("test_indexed_tags")], F,
              server=fast_default_port_server, ports_needed=0, exclusive_default_port=True)
    sec.test("AB/ControlLogix tag scheduling fairness",
              [exe("test_fairness"), f"--tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&name=TestBigArray[0]&auto_sync_read_ms=200",
               "--num-tags=200", "--test-duration-secs=10"], S)
    sec.test("idle disconnect and reconnect with runtime timeout change (AB ControlLogix)",
              [exe("test_idle_disconnect"), f"--tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&name=TestBigArray"], T)
    sec.test("hard library shutdown",
              [exe("test_shutdown"),
               f"--tag=protocol=ab_eip&gateway={gw}&path=1,0&plc=ControlLogix&elem_type=DINT&elem_count=1&name=TestBigArray[%d]&auto_sync_read_ms=200&auto_sync_write_ms=20"], F)
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
    slot16_server = ServerSpec(
        exe_path=exe("ab_server"),
        args_template=["--debug", "--plc=ControlLogix", "--port={PORT}", "--path=1,16",
                        "--tag=TestBigArray:DINT[2000]"],
        startup_wait_s=1,
    )
    gw16 = "127.0.0.1:{PORT}"

    sec = m.section("controllogix_slot16", server=slot16_server)
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
    sec = m.section("standalone")
    sec.test("@connection tag ERR_WAIT on unreachable host (no server running)",
              [exe("test_connection_tag"),
               "--tag=protocol=ab-eip&gateway=127.0.0.1:{PORT}&path=1,0&plc=ControlLogix&name=@connection",
               "--expect-err"], T, ports_needed=1)
    sec.test("Test async reconnect after PLC outage",
              [exe("test_reconnect_after_outage_async"), exe("ab_server"), "{PORT}"], T, ports_needed=1)
    sec.test("Test sync reconnect after PLC outage",
              [exe("test_reconnect_after_outage_sync"), exe("ab_server"), "{PORT}"], T, ports_needed=1)

    # --- ControlLogix functional/slow (delay=100) section -------------------
    slow_server = ServerSpec(
        exe_path=exe("ab_server"),
        args_template=["--port={PORT}", "--plc=ControlLogix", "--path=1,0",
                        "--tag=TestBigArray:DINT[2000]", "--tag=Test_Array_1:DINT[1000]",
                        "--tag=Test_Array_2x3:DINT[2,3]", "--tag=Test_Array_2x3x4:DINT[2,3,4]", "--delay=100"],
        startup_wait_s=1,
    )
    sgw = "127.0.0.1:{PORT}"

    sec = m.section("controllogix_slow", server=slow_server)
    sec.test("emulator test callbacks",
              [exe("test_callback"), f"--tag=protocol=ab-eip&gateway={sgw}&path=1,0&cpu=LGX&elem_count=10&name=TestBigArray"], F)
    # Hardcoded to the library's default gateway/port like the four
    # controllogix_fast tests above -- reuses that same pinned-port server
    # config and serialization group (no need for --delay=100 here; this
    # test doesn't exercise that timing).
    sec.test("emulator test extended callbacks sync", [exe("test_callback_ex")], F,
              server=fast_default_port_server, ports_needed=0, exclusive_default_port=True)
    sec.test("emulator test extended callbacks async",
              [exe("test_callback_ex_async"), f"--tag=protocol=ab-eip&gateway={sgw}&path=1,0&cpu=LGX&elem_count=10&name=TestBigArray"], F)

    # --- Micro800 section ----------------------------------------------------
    micro800_server = ServerSpec(
        exe_path=exe("ab_server"),
        args_template=["--debug", "--plc=Micro800", "--port={PORT}", "--tag=TestDINTArray:DINT[10]"],
        startup_wait_s=1,
    )

    sec = m.section("micro800", server=micro800_server)
    sec.test("basic Micro800 read/write",
              [exe("tag_rw2"), "--type=sint32", "--tag=protocol=ab-eip&gateway=127.0.0.1:{PORT}&plc=micro800&name=TestDINTArray",
               "--write=42", "--debug=4"], F)

    # --- Omron section ---------------------------------------------------------
    omron_server = ServerSpec(
        exe_path=exe("ab_server"),
        args_template=["--debug", "--plc=Omron", "--port={PORT}", "--tag=TestDINTArray:DINT[10]"],
        startup_wait_s=1,
    )
    ogw = "127.0.0.1:{PORT}"

    sec = m.section("omron", server=omron_server)
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
              [exe("test_callback_ex_async"), f"--tag=protocol=ab-eip&gateway={ogw}&path=18,127.0.0.1&plc=omron-njnx&elem_count=10&name=TestDINTArray"], F)
    sec.test("create-from-tag API (17 permutation tests with Omron)",
              [exe("test_create_from_tag"),
               f"--src-tag=protocol=ab-eip&gateway={ogw}&path=18,127.0.0.1&plc=omron-njnx&elem_count=1&name=TestDINTArray[0]",
               "--clone-attrib=name=TestDINTArray[1]&elem_count=1",
               f"--connection-tag=protocol=ab-eip&gateway={ogw}&path=18,127.0.0.1&plc=omron-njnx&name=@connection",
               "--timeout=10000"], F)
    # Manages its own ab_server internally on a hardcoded port -- no shared
    # section server, no dynamically-allocated port.
    sec.test("plc_tag_destroy does not hang after Omron connection loss (issue #625)",
              [exe("test_omron_destroy"), exe("ab_server")], F, server=None, ports_needed=0)

    # --- Micrologix section ----------------------------------------------------
    micrologix_server = ServerSpec(
        exe_path=exe("ab_server"),
        args_template=["--debug", "--plc=Micrologix", "--port={PORT}",
                        "--tag=B3[10]", "--tag=N7[10]", "--tag=L19[10]"],
        startup_wait_s=1,
    )
    mgw = "127.0.0.1:{PORT}"

    sec = m.section("micrologix", server=micrologix_server)
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
    plc5_server = ServerSpec(
        exe_path=exe("ab_server"),
        args_template=["--debug", "--plc=PLC/5", "--port={PORT}", "--tag=B3[10]", "--tag=N7[10]"],
        startup_wait_s=1,
    )
    pgw = "127.0.0.1:{PORT}"

    sec = m.section("plc5", server=plc5_server)
    sec.test("B data file PLC5 tag read/write",
              [exe("tag_rw2"), "--type=uint16", f"--tag=protocol=ab-eip&gateway={pgw}&plc=plc5&elem_count=1&name=B3:0", "--debug=4", "--write=0"], F)
    sec.test("B bit data file PLC5 tag read/write",
              [exe("tag_rw2"), "--type=bit", f"--tag=protocol=ab-eip&gateway={pgw}&plc=plc5&elem_count=1&name=B3:0/10", "--debug=4", "--write=1"], F)
    sec.test("N data file PLC5 tag read/write",
              [exe("tag_rw2"), "--type=sint16", f"--tag=protocol=ab-eip&gateway={pgw}&plc=plc5&elem_count=1&name=N7:0", "--debug=4", "--write=0"], F)
    sec.test("N bit data file PLC5 tag read/write",
              [exe("tag_rw2"), "--type=bit", f"--tag=protocol=ab-eip&gateway={pgw}&plc=plc5&elem_count=1&name=N7:0/10", "--debug=4", "--write=1"], F)

    # --- Modbus section ------------------------------------------------------
    modbus_server = ServerSpec(
        exe_path=exe("modbus_server"),
        args_template=["--listen=127.0.0.1:{PORT}", "--listen=127.0.0.1:{PORT2}", "--debug=DETAIL"],
        startup_wait_s=3,
    )
    mbgw = "127.0.0.1:{PORT}"
    mbgw2 = "127.0.0.1:{PORT2}"

    sec = m.section("modbus", server=modbus_server)
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
              [exe("test_callback_ex_async"), f"--tag=protocol=modbus-tcp&gateway={mbgw}&path=0&elem_count=10&name=hr1"], F)
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
              [exe("test_shutdown"),
               f"--tag=protocol=modbus-tcp&gateway={mbgw}&path=0&elem_count=1&name=hr5&auto_sync_read_ms=200&auto_sync_write_ms=20"], F)
    sec.test("Modbus tag scheduling fairness",
              [exe("test_fairness"), f"--tag=protocol=modbus-tcp&gateway={mbgw}&path=1&name=hr10&auto_sync_read_ms=200",
               "--num-tags=200", "--test-duration-secs=10"], S)
    reconnect_test = sec.test("for Modbus reconnect bug",
              [exe("test_modbus_multiple"), f"--gateway1={mbgw}", f"--gateway2={mbgw2}"], T)
    # Depends on the reconnect test's own log; a CheckSpec test always runs
    # after its whole phase's worker pool has drained, so this is guaranteed
    # to see the finished log without needing any cross-process signaling.
    sec.test("check for exactly 2 PLC creation entries in Modbus reconnect test log",
              None, T, depends_on=reconnect_test.id, ports_needed=0,
              check=CheckSpec(log_file=reconnect_test.log_file,
                               pattern=r"Creating new PLC connection\.", expected=2))

    return m


# ---------------------------------------------------------------------------
# Execution
# ---------------------------------------------------------------------------

# Set via ProcessPoolExecutor's initializer in each worker process.
_port_counter = None
_port_lock = None
_stress_sema = None
_default_port_sema = None


def _worker_init(port_counter, port_lock, stress_sema, default_port_sema) -> None:
    global _port_counter, _port_lock, _stress_sema, _default_port_sema
    _port_counter = port_counter
    _port_lock = port_lock
    _stress_sema = stress_sema
    _default_port_sema = default_port_sema
    raise_fd_limit(1024)


def _port_is_free(port: int) -> bool:
    # Best-effort only: something else can still grab the port between this
    # check and the server's own bind() moments later. It closes the much
    # more common hole where the counter hands out a port some unrelated,
    # already-running process on the host happens to occupy (see: a stray
    # dev binary from a different project squatting on a low port number).
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind(("127.0.0.1", port))
            return True
        except OSError:
            return False


def _alloc_ports(n: int) -> list[int]:
    if n == 0:
        return []
    ports = []
    # Hold the lock for the whole probe-and-claim sequence so two workers
    # can't both pick the same still-free-looking port at once.
    with _port_lock:
        while len(ports) < n:
            candidate = _port_counter.value
            _port_counter.value += 1
            if _port_is_free(candidate):
                ports.append(candidate)
    return ports


def _fill(template: list[str], ports: list[int]) -> list[str]:
    out = []
    for arg in template:
        if ports:
            arg = arg.replace("{PORT}", str(ports[0]))
            if len(ports) > 1:
                arg = arg.replace("{PORT2}", str(ports[1]))
        out.append(arg)
    return out


def run_test_in_worker(test: Test) -> Result:
    """Runs entirely inside a worker process: allocate this test's own
    port(s), bring its server up if it needs one, run the client, tear the
    server back down. One test occupies this worker until all of that is
    done."""
    start = time.monotonic()
    stress_held = False
    default_port_held = False
    server_proc = None
    try:
        ports = _alloc_ports(test.ports_needed)

        if test.exclusive_default_port:
            _default_port_sema.acquire()
            default_port_held = True

        if test.group == Group.STRESS:
            _stress_sema.acquire()
            stress_held = True

        if test.server is not None:
            server_cmd = [test.server.exe_path] + _fill(test.server.args_template, ports)
            server_proc = spawn(server_cmd, Path(test.server_log_file))
            time.sleep(test.server.startup_wait_s)
            if server_proc.poll() is not None:
                end = time.monotonic()
                return Result(test=test, ok=False, detail="server exited during startup", start=start, end=end)

        cmd = _fill(test.cmd_template, ports)
        with open(test.log_file, "w") as log:
            proc = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT)
        ok = (proc.returncode == 0) != test.expect_failure
        detail = ""
    except Exception as e:
        # A crash here (e.g. a transient spawn failure under heavy load) must
        # not take down every other in-flight/pending test -- record it as
        # this one test's failure and let the rest of the run continue.
        ok = False
        detail = f"{type(e).__name__}: {e}"
    finally:
        stop_process(server_proc)
        if stress_held:
            _stress_sema.release()
        if default_port_held:
            _default_port_sema.release()
    end = time.monotonic()
    return Result(test=test, ok=ok, detail=detail, start=start, end=end)


def _print_result(r: Result) -> None:
    status = "OK" if r.ok else "FAILURE"
    suffix = f" ({r.detail})" if r.detail else ""
    print(f"  [{r.test.section}] test {r.test.id} ({r.test.group.value}): {r.test.name}... "
          f"{status} ({r.elapsed:.0f}s){suffix}")


def run_phase(pool: ProcessPoolExecutor, tests: list[Test]) -> tuple[list[Result], float]:
    if not tests:
        return [], 0.0
    phase_start = time.monotonic()
    results: list[Result] = []

    pool_tests = [t for t in tests if t.cmd_template is not None]
    check_tests = [t for t in tests if t.cmd_template is None]

    futs = {pool.submit(run_test_in_worker, t): t for t in pool_tests}
    for fut in as_completed(futs):
        r = fut.result()
        _print_result(r)
        results.append(r)

    # Check-only tests need no worker/subprocess: just grep a log file that's
    # now guaranteed complete, since every pool test in this phase has
    # finished above.
    for t in check_tests:
        cstart = time.monotonic()
        ok, detail = run_check(t.check)
        cend = time.monotonic()
        r = Result(test=t, ok=ok, detail=detail, start=cstart, end=cend)
        _print_result(r)
        results.append(r)

    phase_end = time.monotonic()
    return results, phase_end - phase_start


def main() -> int:
    global TEST_DIR, LOG_DIR

    parser = argparse.ArgumentParser()
    parser.add_argument("test_dir")
    parser.add_argument("log_dir", nargs="?", default=".")
    parser.add_argument("--max-workers", type=int, default=os.cpu_count() or 4,
                         help="worker processes for the functional+stress phase (one per CPU by default)")
    parser.add_argument("--timing-workers", type=int, default=max(2, (os.cpu_count() or 4) // 2),
                         help="worker processes for the timing phase")
    parser.add_argument("--max-stress", type=int, default=2,
                         help="max STRESS-group tests allowed to run at once, regardless of worker count "
                              "(each one already opens up to 200 of its own connections/threads)")
    args = parser.parse_args()

    TEST_DIR = Path(args.test_dir)
    LOG_DIR = Path(args.log_dir)
    LOG_DIR.mkdir(parents=True, exist_ok=True)

    if not TEST_DIR.is_dir():
        print(f"{TEST_DIR} is not a valid path for test executables!")
        return 1

    check_executables_present()
    raise_fd_limit(1024)
    kill_stray_servers()

    script_start = time.monotonic()
    manifest = build_manifest()
    all_tests = manifest.tests

    ctx = get_context("spawn")
    # Start one above DEFAULT_LIB_PORT so the general-purpose allocator can
    # never hand that reserved port out to an unrelated test.
    port_counter = ctx.Value("i", DEFAULT_LIB_PORT + 1)
    port_lock = ctx.Lock()
    stress_sema = ctx.BoundedSemaphore(args.max_stress)
    default_port_sema = ctx.BoundedSemaphore(1)

    all_results: list[Result] = []
    phase_times: list[float] = []

    for phase_groups in PHASES:
        phase_tests = [t for t in all_tests if t.group in phase_groups]
        workers = args.timing_workers if phase_groups == {Group.TIMING} else args.max_workers
        with ProcessPoolExecutor(max_workers=workers, mp_context=ctx, initializer=_worker_init,
                                  initargs=(port_counter, port_lock, stress_sema, default_port_sema)) as pool:
            results, elapsed = run_phase(pool, phase_tests)
        all_results.extend(results)
        phase_times.append(elapsed)

    kill_stray_servers()

    total = time.monotonic() - script_start

    all_results.sort(key=lambda r: r.test.id)
    failures = sum(1 for r in all_results if not r.ok)

    print("\nSection timing (span from first test start to last test end in that section):")
    for name in manifest.section_names:
        section_results = [r for r in all_results if r.test.section == name]
        if not section_results:
            continue
        span = max(r.end for r in section_results) - min(r.start for r in section_results)
        print(f"  {name}: {span:.0f}s")

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

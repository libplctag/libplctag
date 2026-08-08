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
import shlex
import socket
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from multiprocessing import get_context
from pathlib import Path
from typing import Optional


def _now_ts() -> str:
    t = time.time()
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t)) + f".{int((t % 1) * 1000):03d}"


_builtin_print = print


def print(*args, **kwargs) -> None:
    """Prefix every printed line with a timestamp, so a stalled/killed CI job's
    output shows exactly when things stopped progressing (e.g. which test was
    running right before a 1-hour job timeout fired). Overrides the builtin for
    this module only; splits on embedded '\\n' so a single print() with a blank
    spacer line or multi-line message still gets one timestamp per real line."""
    sep = kwargs.pop("sep", " ")
    text = sep.join(str(a) for a in args)
    stamped = "\n".join(f"{_now_ts()} {line}" if line else _now_ts() for line in text.split("\n"))
    _builtin_print(stamped, **kwargs)


# ---------------------------------------------------------------------------
# Test model
# ---------------------------------------------------------------------------

class Group(enum.Enum):
    FUNCTIONAL = "functional"
    STRESS = "stress"
    TIMING = "timing"


# A starved STRESS test (too many threads for too few cores) doesn't hang --
# it retries and backs off forever, one client at a time, and silently eats
# the whole CI job's 1-hour timeout before anyone finds out which test did it
# (observed: 38 minutes on a 4-CPU runner for a test that should take well
# under a minute). Kill it and fail it explicitly instead so the log names
# the actual culprit.
STRESS_TEST_TIMEOUT_S = 180

# A genuinely deadlocked FUNCTIONAL/TIMING test has the same failure mode:
# no output, no error, the whole CI job silently eats its 1-hour timeout
# instead of naming the stuck test (observed: macOS TSan x64 run stopped
# dead after the timing-phase "auto sync" test, no log for whatever ran
# next). Every group gets a budget; TIMING's is generous since several of
# those tests deliberately sleep tens of seconds already.
DEFAULT_TEST_TIMEOUT_S = 300


# Which groups are allowed to run concurrently with each other. Phases run
# in order; a phase doesn't start until the previous one has fully drained.
# STRESS gets its own phase: each stress test already opens up to 100
# connections/threads, and running one alongside unrelated FUNCTIONAL tests
# in sibling workers was starving those tests' own server-startup and
# protocol-round-trip timeouts on constrained CI hardware. Running STRESS
# alone (no FUNCTIONAL test sharing the host's CPU) removes that contention.
PHASES: list[set[Group]] = [
    {Group.STRESS},
    {Group.FUNCTIONAL},
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


CREATE_BREAKAWAY_FROM_JOB = 0x01000000


def spawn(cmd: list[str], log_path: Path) -> subprocess.Popen:
    # Long-lived servers must not share their launching process's session:
    # ab_server (and friends) install no SIGHUP handler, so if they inherit a
    # controlling terminal/session they can die silently the moment it goes
    # away. start_new_session (POSIX) / CREATE_NEW_PROCESS_GROUP (Windows)
    # detaches them so they survive independent of how they were launched.
    log = open(log_path, "w")
    if os.name == "posix":
        return subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)

    # On GitHub Actions' Windows runners, ab_server has been observed to vanish
    # mid-test with no shutdown log and no console-control-handler event fired --
    # the signature of the whole job object being torn down with
    # TerminateProcess, which bypasses SetConsoleCtrlHandler entirely.
    # CREATE_NEW_PROCESS_GROUP only protects against console Ctrl events, not
    # job-object membership, so also try to break the child out of the job
    # entirely. Some job objects don't permit breakaway, in which case
    # CreateProcess fails outright and we fall back to the old behavior.
    flags = subprocess.CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB
    try:
        return subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, creationflags=flags)
    except OSError:
        return subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT,
                                 creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)


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
    "test_lib_api_coverage",
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

# Base for dynamically allocated server ports.
#
# Deliberately BELOW Linux's default ephemeral range (net.ipv4.ip_local_port_range
# is 32768-60999). Allocating server ports inside that range means a client
# connection's ephemeral source port can take the number in the window between
# _port_is_free() probing it and the server actually calling bind() -- which shows
# up as an intermittent "Address already in use" on a port the probe just reported
# as free. SO_REUSEADDR does not help against a live socket.
#
# Note DEFAULT_LIB_PORT itself is inside the ephemeral range and cannot move: it is
# the standard EtherNet/IP port and is compiled into the library. The handful of
# exclusive_default_port tests therefore keep that small exposure.
ALLOC_PORT_BASE = 20000


def build_manifest() -> Manifest:
    m = Manifest()
    F, S, T = Group.FUNCTIONAL, Group.STRESS, Group.TIMING

    # --- ControlLogix "fast" section ---------------------------------------
    fast_server = ServerSpec(
        exe_path=exe("ab_server"),
        args_template=["--debug", "--plc=ControlLogix", "--port={PORT}", "--path=1,0",
                        "--tag=TestBigArray:DINT[2000]", "--tag=Test_Array_1:DINT[1000]",
                        "--tag=Test_Array_2x3:DINT[2,3]", "--tag=Test_Array_2x3x4:DINT[2,3,4]",
                        "--tag=TestReal:REAL[1]", "--tag=TestLReal:LREAL[1]",
                        "--tag=TestLint:LINT[1]", "--tag=TestSint:SINT[1]"],
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
    sec.test("lib.c public API coverage (float32/64, int64, int8, raw_bytes, lock, byte_order)",
              [exe("test_lib_api_coverage"),
               f"--real-tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&elem_type=REAL&elem_count=1&name=TestReal",
               f"--lreal-tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&elem_type=LREAL&elem_count=1&name=TestLReal",
               f"--lint-tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&elem_type=LINT&elem_count=1&name=TestLint",
               f"--sint-tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&elem_type=SINT&elem_count=1&name=TestSint",
               f"--byteorder-tag=protocol=ab-eip&gateway={gw}&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray"], F)
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

    # --- Error-path coverage section ----------------------------------------
    # Deliberately trigger client-side error branches that the rest of the
    # suite's happy-path tests never touch. The first three need nothing
    # special from ab_server -- a nonexistent tag name / out-of-bounds index /
    # oversized element count are naturally rejected by its own tag-path and
    # bounds validation. The rest use ab_server's two error-injection knobs
    # (--reject_fo=N, --delay=Nms) on their own dedicated server instances.
    error_server = ServerSpec(
        exe_path=exe("ab_server"),
        args_template=["--debug", "--plc=ControlLogix", "--port={PORT}", "--path=1,0",
                        "--tag=TestBigArray:DINT[2000]"],
        startup_wait_s=3,
    )
    egw = "127.0.0.1:{PORT}"

    sec = m.section("error_injection", server=error_server)
    sec.test("read nonexistent tag",
              [exe("tag_rw2"), "--type=sint32",
               f"--tag=protocol=ab-eip&gateway={egw}&path=1,0&plc=ControlLogix&elem_count=1&name=NoSuchTagAtAll",
               "--debug=4"], F, expect_failure=True)
    sec.test("read array index out of bounds",
              [exe("tag_rw2"), "--type=sint32",
               f"--tag=protocol=ab-eip&gateway={egw}&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray[9999]",
               "--debug=4"], F, expect_failure=True)
    sec.test("request more elements than the tag holds",
              [exe("tag_rw2"), "--type=sint32",
               f"--tag=protocol=ab-eip&gateway={egw}&path=1,0&plc=ControlLogix&elem_count=99999&name=TestBigArray",
               "--debug=4"], F, expect_failure=True)

    # ab_server's --reject_fo=N used to have a bug (fixed): reject_fo_count
    # lived in plc_s, which tcp_server.c copies fresh into every accepted TCP
    # connection, so the decrement never persisted across the client's
    # reconnect-per-retry behavior -- N>=1 rejected forever instead of N
    # times. Now fixed by sharing one atomic_int32_t (via pointer) across
    # every connection's copy, decremented with atomic_dec_int32. This test
    # confirms the counter genuinely persists: reject exactly once, then
    # succeed on the next attempt -- verified via log content (exit code
    # alone wouldn't catch a bug that skipped the rejection and "succeeded"
    # some other way, or one that rejected every time).
    #
    # Note: the client still doesn't take the PLCTAG_ERR_DUPLICATE-specific
    # retry branch in session.c:1498 for this rejection -- it recovers via a
    # different, outer session-recreation retry instead, treating it as
    # PLCTAG_ERR_REMOTE_ERR. That's a separate, still-open issue (see the
    # proposed byte-layout investigation) independent of this counter fix.
    reject_fo_server = ServerSpec(
        exe_path=exe("ab_server"),
        args_template=["--debug", "--plc=ControlLogix", "--port={PORT}", "--path=1,0",
                        "--tag=TestBigArray:DINT[2000]", "--reject_fo=1"],
        startup_wait_s=3,
    )
    reject_fo_test = sec.test("ForwardOpen rejection persists across reconnect, then succeeds",
              [exe("tag_rw2"), "--type=sint32",
               f"--tag=protocol=ab-eip&gateway={egw}&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray",
               "--debug=4"], T, server=reject_fo_server)
    sec.test("check ForwardOpen was rejected exactly once before succeeding",
              None, T, depends_on=reject_fo_test.id, ports_needed=0,
              check=CheckSpec(log_file=reject_fo_test.log_file,
                               pattern=r"Forward Open command failed", expected=1))

    # --delay= applied to every response (including session register and
    # ForwardOpen, not just reads -- see ab_server's request_handler) paired
    # with a short client timeout deterministically hits PLCTAG_ERR_TIMEOUT,
    # instead of relying on incidental resource contention the way most
    # accidental timeout failures elsewhere in this suite have been.
    big_delay_server = ServerSpec(
        exe_path=exe("ab_server"),
        args_template=["--debug", "--plc=ControlLogix", "--port={PORT}", "--path=1,0",
                        "--tag=TestBigArray:DINT[2000]", "--delay=5000"],
        startup_wait_s=3,
    )
    sec.test("deterministic timeout via injected response delay",
              [exe("tag_rw2"), "--type=sint32",
               f"--tag=protocol=ab-eip&gateway={egw}&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray",
               "--debug=4", "--timeout=1000"], T, server=big_delay_server, expect_failure=True)

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
    # Starts and stops its own ab_server internally, so there is no section server
    # for the harness to manage -- but it still takes an allocated port so its
    # private server cannot collide with anyone else's.
    sec.test("plc_tag_destroy does not hang after Omron connection loss (issue #625)",
              [exe("test_omron_destroy"), exe("ab_server"), "{PORT}"], F, server=None, ports_needed=1)

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
# Prefixed to every client command line (see --client-wrapper). Servers are
# deliberately left unwrapped: they are spawned separately below, and under a
# tool like Valgrind an instrumented server would slow the whole suite down a
# second time for no extra coverage of the library under test.
_client_wrapper: list[str] = []
# Multiplies the per-test kill budgets (see --timeout-scale).
_timeout_scale = 1.0


def _worker_init(port_counter, port_lock, stress_sema, default_port_sema,
                 client_wrapper, timeout_scale) -> None:
    global _port_counter, _port_lock, _stress_sema, _default_port_sema
    global _client_wrapper, _timeout_scale
    _port_counter = port_counter
    _port_lock = port_lock
    _stress_sema = stress_sema
    _default_port_sema = default_port_sema
    _client_wrapper = client_wrapper
    _timeout_scale = timeout_scale
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


def _wait_for_server_ready(port: int, proc: subprocess.Popen, timeout_s: float) -> bool:
    """Poll until something is actually listening on `port`, instead of a
    fixed sleep. A fixed sleep only reflects the *typical* time a server
    takes to bind and listen; on a slow/contended CI runner (e.g. observed on
    Windows ARM64 under 4-way parallel load) that can take longer than the
    sleep, and the client then fails with a spurious connect/read timeout
    even though the server would have come up fine given a bit more time.
    Returns False if the server process exits first or the timeout expires."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            return False
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(0.2)
            try:
                s.connect(("127.0.0.1", port))
                return True
            except OSError:
                pass
        time.sleep(0.1)
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

        # A server that vanishes mid-test with no shutdown log and no signal
        # handler firing (observed on Windows -- see test 36 investigation)
        # looks identical to a real client-side protocol failure. One retry
        # with a freshly spawned server tells the two apart cheaply, since a
        # real bug reproduces and an external one-off process kill doesn't.
        for attempt in range(2):
            if test.server is not None:
                server_cmd = [test.server.exe_path] + _fill(test.server.args_template, ports)
                server_proc = spawn(server_cmd, Path(test.server_log_file))
                ready_port = ports[0] if ports else DEFAULT_LIB_PORT
                # Generous upper bound (not just startup_wait_s) so a slow/contended CI
                # runner gets real headroom instead of a spurious timeout; a healthy
                # server still returns almost immediately once it's actually listening.
                ready_timeout = max(test.server.startup_wait_s * 10, 15.0)
                if not _wait_for_server_ready(ready_port, server_proc, ready_timeout):
                    end = time.monotonic()
                    detail = "server exited during startup" if server_proc.poll() is not None else "server did not start listening in time"
                    return Result(test=test, ok=False, detail=detail, start=start, end=end)

            cmd = _client_wrapper + _fill(test.cmd_template, ports)
            base_timeout = STRESS_TEST_TIMEOUT_S if test.group == Group.STRESS else DEFAULT_TEST_TIMEOUT_S
            timeout_s = base_timeout * _timeout_scale
            try:
                with open(test.log_file, "w") as log:
                    proc = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=timeout_s)
                ok = (proc.returncode == 0) != test.expect_failure
                detail = ""
            except subprocess.TimeoutExpired:
                ok = False
                detail = f"client exceeded {timeout_s}s budget, killed"

            server_vanished = test.server is not None and server_proc.poll() is not None
            stop_process(server_proc)
            server_proc = None
            if ok or not server_vanished or attempt == 1:
                if not ok and server_vanished:
                    detail = "server vanished mid-test (retried once, still failed)"
                break
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

    # CI captures stdout through a pipe, not a TTY, so Python block-buffers
    # by default -- output can sit unflushed for a long time, making a script
    # that's actually running fine look hung in the CI log viewer. Force line
    # buffering so every print() below is visible immediately.
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:
        pass

    parser = argparse.ArgumentParser()
    parser.add_argument("test_dir")
    parser.add_argument("log_dir", nargs="?", default=".")
    parser.add_argument("--max-workers", type=int, default=os.cpu_count() or 4,
                         help="worker processes for the functional+stress phase (one per CPU by default)")
    parser.add_argument("--timing-workers", type=int, default=max(2, (os.cpu_count() or 4) // 2),
                         help="worker processes for the timing phase")
    parser.add_argument("--max-stress", type=int, default=1,
                         help="max STRESS-group tests allowed to run at once, regardless of worker count "
                              "(each one already opens up to 200 of its own connections/threads)")
    parser.add_argument("--client-wrapper", default="",
                         help="command prefixed to every client invocation, e.g. "
                              "\"valgrind --tool=memcheck --error-exitcode=1\". Servers are not wrapped.")
    parser.add_argument("--timeout-scale", type=float, default=1.0,
                         help="multiplier for the per-test kill budgets; raise it when --client-wrapper "
                              "slows the client down (Valgrind costs roughly 20-50x)")
    parser.add_argument("--skip-groups", default="",
                         help="comma-separated groups to skip entirely (functional, stress, timing). "
                              "TIMING tests assert on wall-clock behaviour and STRESS tests spawn hundreds "
                              "of threads, so both give false failures under a heavy --client-wrapper.")
    args = parser.parse_args()

    client_wrapper = shlex.split(args.client_wrapper)

    try:
        skip_groups = {Group(g.strip()) for g in args.skip_groups.split(",") if g.strip()}
    except ValueError as e:
        print(f"--skip-groups: {e}")
        return 1

    TEST_DIR = Path(args.test_dir)
    LOG_DIR = Path(args.log_dir)
    LOG_DIR.mkdir(parents=True, exist_ok=True)

    if not TEST_DIR.is_dir():
        print(f"{TEST_DIR} is not a valid path for test executables!")
        return 1

    print("Settings:")
    print(f"  CPUs detected: {os.cpu_count()}")
    print(f"  test_dir: {TEST_DIR}")
    print(f"  log_dir: {LOG_DIR}")
    print(f"  max-workers (functional+stress phase): {args.max_workers}")
    print(f"  timing-workers (timing phase): {args.timing_workers}")
    print(f"  max-stress (concurrent STRESS-group tests): {args.max_stress}")
    if client_wrapper:
        print(f"  client-wrapper: {' '.join(client_wrapper)}")
    if args.timeout_scale != 1.0:
        print(f"  timeout-scale: {args.timeout_scale}")
    if skip_groups:
        print(f"  skip-groups: {', '.join(sorted(g.value for g in skip_groups))}")
    print()

    print("Checking for required executables...")
    check_executables_present()
    print("Raising file descriptor limit...")
    raise_fd_limit(1024)
    print("Killing any stray ab_server/modbus_server processes from a previous run...")
    kill_stray_servers()
    print("Done with startup checks.")

    script_start = time.monotonic()
    manifest = build_manifest()
    all_tests = manifest.tests
    print(f"Built manifest: {len(all_tests)} tests.")

    ctx = get_context("spawn")
    # ALLOC_PORT_BASE sits below the ephemeral range and below DEFAULT_LIB_PORT,
    # so the allocator can never hand out that reserved port either.
    port_counter = ctx.Value("i", ALLOC_PORT_BASE)
    port_lock = ctx.Lock()
    stress_sema = ctx.BoundedSemaphore(args.max_stress)
    default_port_sema = ctx.BoundedSemaphore(1)

    all_results: list[Result] = []
    phase_times: list[tuple[str, float]] = []

    for phase_groups in PHASES:
        phase_groups = phase_groups - skip_groups
        if not phase_groups:
            continue
        phase_tests = [t for t in all_tests if t.group in phase_groups]
        if phase_groups == {Group.TIMING}:
            workers = args.timing_workers
        elif phase_groups == {Group.STRESS}:
            workers = args.max_stress
        else:
            workers = args.max_workers
        names = "+".join(g.value for g in phase_groups)
        print(f"Starting phase '{names}': {len(phase_tests)} tests, {workers} workers...")
        with ProcessPoolExecutor(max_workers=workers, mp_context=ctx, initializer=_worker_init,
                                  initargs=(port_counter, port_lock, stress_sema, default_port_sema,
                                            client_wrapper, args.timeout_scale)) as pool:
            results, elapsed = run_phase(pool, phase_tests)
        all_results.extend(results)
        # Record the name alongside the time: with --skip-groups, phases can be
        # dropped, so zipping the timings back against PHASES would mislabel them.
        phase_times.append((names, elapsed))
        print(f"Phase '{names}' done in {elapsed:.0f}s.")

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
    for names, elapsed in phase_times:
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

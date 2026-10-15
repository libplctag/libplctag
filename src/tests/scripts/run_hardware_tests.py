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

Parallel, with locks on what the tests share.  Unlike the simulator, where every
test gets its own ab_server, these PLCs and their tags are shared, so:

- functional tests run together on every device, even on the same tag, except
  that a test which reads back what it wrote locks that tag exclusively, and one
  that only writes locks it shared -- writers run alongside each other but never
  between another test's write and its check;
- a stress test locks the whole device it talks to, as the simulator runs that
  group in a phase of its own;
- the MicroLogix is always locked whole: run in parallel it acknowledged a write
  to B3:0 and then read back the old value.

A test that crosses devices -- a CIP bridge to the MicroLogix, DH+ to the PLC/5
-- holds a lock on each.  Tests that want the same lock run in manifest order.

Usage matches the bash script: run_hardware_tests.py TEST_DIR [LOG_DIR]
"""

import argparse
import dataclasses
import os
import re
import subprocess
import sys
import time
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from pathlib import Path
from typing import Optional


@dataclasses.dataclass(frozen=True)
class Test:
    id: int
    name: str
    cmd: list[str]
    log_file: str
    #: the error the test must fail with; None means it must succeed.
    expect_error: Optional[str] = None
    group: str = "functional"
    #: (device, tag, exclusive) locks; tag None locks the whole device.
    resources: frozenset = frozenset()


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


# Real lab devices -- see run_hardware_tests.sh for the same addresses.
LOGIX_GW = "10.206.1.40"     # L81 Ethernet port, also the DH+ bridge
ENBT_48_GW = "10.206.1.37"   # ENBT 4.8 in the same chassis
ENBT_66_GW = "10.206.1.39"   # ENBT 6.6 in the same chassis
MLGX_GW = "10.206.1.36"
PLC5_GW = "10.206.1.38"

#: which device each address belongs to.  The three chassis entry points are one device.
DEVICE_OF_HOST = {LOGIX_GW: "logix", ENBT_48_GW: "logix", ENBT_66_GW: "logix", MLGX_GW: "micrologix", PLC5_GW: "plc5"}

#: executables that name their gateways and tags in their own source: devices, chassis tags.
TARGETS_OF_EXE = {
    "test_special": (set(), set()),
    "test_tag_attributes": ({"logix"}, {"TestDINTArray"}),
    "test_tag_type_attribute": ({"logix"}, {"TestBigSINTArray", "TestManyBOOLFields"}),
    "string_standard": ({"logix", "plc5"}, {"barcodes", "ST18"}),
    "string_non_standard_udt": ({"logix"}, {"CB_Rpt"}),
    "test_string": ({"logix"}, {"CB_Txt"}),
    "test_raw_cip": ({"logix"}, set()),
}


#: devices that run one test at a time whatever the test: with several sessions open the
#: MicroLogix 1100 acknowledged a write and did not keep it.
WHOLE_DEVICES = {"micrologix"}

#: executables that write a tag and read it back, so a concurrent writer breaks them.
CHECKING_EXES = {"test_bit_write", "test_write_before_read"}


def resources_of(cmd: list[str], group: str) -> frozenset:
    """
    The locks a command line needs.  A functional test locks the base name of each tag it
    names, on each device it talks to, so TestINTArray[0].13 and TestINTArray[0] contend, as
    do B3:0/6 and B3:0: exclusively if it checks what it wrote, shared if it only writes, not
    at all if it only reads.  Any other group locks its devices whole.  A DH+ route (A:node)
    ends at the PLC/5.
    """
    args = cmd[1:]
    if args:
        text = " ".join(args)
        devices = {dev for host, dev in DEVICE_OF_HOST.items() if re.search(rf"\b{re.escape(host)}\b", text)}
        if re.search(r"\bA:\d+:\d+", text):
            devices.add("plc5")
        tags = set(re.findall(r"(?:[&?]name=|^name=|--read=)([A-Za-z_][A-Za-z0-9_]*)", text.replace(" ", "\n"), re.M))
        checks = "--check" in text or Path(cmd[0]).stem in CHECKING_EXES
        if not checks and "--write" not in text:
            tags = set()
    else:
        devices, tags = TARGETS_OF_EXE[Path(cmd[0]).stem]
        checks = True

    if group != "functional":
        return frozenset((dev, None, True) for dev in devices)
    whole = set(devices) & WHOLE_DEVICES
    return frozenset({(dev, tag, checks) for dev in set(devices) - whole for tag in tags} | {(dev, None, True) for dev in whole})


def conflicts(a: frozenset, b: frozenset) -> bool:
    """Locks clash on one device and tag, or a whole device, unless both are shared."""
    for dev_a, tag_a, excl_a in a:
        for dev_b, tag_b, excl_b in b:
            if dev_a == dev_b and (tag_a is None or tag_b is None or tag_a == tag_b) and (excl_a or excl_b):
                return True
    return False


def test(name: str, cmd: list[str], *, expect_error: Optional[str] = None, group: str = "functional") -> Test:
    tid = _next_id[0]
    _next_id[0] += 1
    slug = re.sub(r"[^a-zA-Z0-9]+", "_", name).strip("_").lower()
    log_file = str(LOG_DIR / f"{tid}_{slug}.log")
    return Test(id=tid, name=name, cmd=cmd, log_file=log_file, expect_error=expect_error, group=group,
                resources=resources_of(cmd, group))


REQUIRED_EXECUTABLES = [
    "tag_rw2", "test_special", "test_tag_attributes", "test_tag_type_attribute",
    "string_standard", "string_non_standard_udt", "test_string", "test_idle_disconnect",
    "test_raw_cip", "list_tags_logix", "get_identity", "test_connection_tag",
    "test_write_before_read", "thread_stress", "test_bit_write",
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
    logix_gw = LOGIX_GW
    logix_path = "1,4"
    mlgx_gw = MLGX_GW
    plc5_gw = PLC5_GW
    dhp_bridge_gw = LOGIX_GW
    cip_bridge_gw = ENBT_48_GW
    cip_bridge_target = ENBT_66_GW
    cip_bridge_out = "1,4,18"  # backplane to the slot 4 Ethernet module, then out its port 2/A

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
              "--debug=4", "--write=1", "--check=1"]),
        test("Get LINT bit",
             [exe("tag_rw2"), "--type=bit",
              f"--tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&name=TestLINTArray[0].43",
              "--debug=4"]),
        test("Set LINT bit",
             [exe("tag_rw2"), "--type=bit",
              f"--tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&name=TestLINTArray[0].43",
              "--debug=4", "--write=1", "--check=1"]),
        # Clearing matters as much as setting: a mask that turns on too much still passes a
        # write-1 check.  This is the CIP read-modify-write path.
        test("Clear INT bit",
             [exe("tag_rw2"), "--type=bit",
              f"--tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&name=TestINTArray[0].13",
              "--debug=4", "--write=0", "--check=0"]),
        test("Clear LINT bit",
             [exe("tag_rw2"), "--type=bit",
              f"--tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&name=TestLINTArray[0].43",
              "--debug=4", "--write=0", "--check=0"]),
        # --check on a bit tag re-reads only that bit, so every test above passes even if the
        # mask also clobbered the neighbours in the same word.  test_bit_write stamps a known
        # alternating pattern on the whole word, flips the one bit through the bit tag, and
        # reads the word back: exactly one bit may differ.  It restores the original word on
        # the way out, including after a failure.
        #
        # The CIP read-modify-write (service 0x4E) is the case that only hardware can prove --
        # ab_server has no handler for it, so the simulator suite cannot reach this path at all.
        test("INT bit write changes only that bit (CIP read-modify-write)",
             [exe("test_bit_write"), "--bit=13", "--debug=4",
              f"--word-tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&elem_count=1&name=TestINTArray[0]",
              f"--bit-tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&name=TestINTArray[0].13"]),
        test("LINT bit write changes only that bit (CIP read-modify-write, 64 bit word)",
             [exe("test_bit_write"), "--bit=43", "--debug=4",
              f"--word-tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&elem_count=1&name=TestLINTArray[0]",
              f"--bit-tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&name=TestLINTArray[0].43"]),
        test("test non-standard UDT strings", [exe("string_non_standard_udt")]),
        test("test non-standard string size", [exe("test_string")]),
        # The MicroLogix program drives B3/0, so any value written to the whole word must
        # leave bit 0 set or the read-back disagrees with what we sent.
        test("B data file Micrologix tag read/write",
             [exe("tag_rw2"), "--type=uint16", f"--tag=protocol=ab-eip&gateway={mlgx_gw}&plc=micrologix&name=B3:0",
              "--write=1", "--check", "--debug=4"]),
        test("B bit data file Micrologix tag read/write",
             [exe("tag_rw2"), "--type=bit", f"--tag=protocol=ab-eip&gateway={mlgx_gw}&plc=micrologix&name=B3:0/6",
              "--write=1", "--check=1", "--debug=4"]),
        test("N data file Micrologix tag read/write",
             [exe("tag_rw2"), "--type=sint16", f"--tag=protocol=ab-eip&gateway={mlgx_gw}&plc=micrologix&name=N7:0",
              "--write=42", "--check", "--debug=4"]),
        test("N bit data file Micrologix tag read/write",
             [exe("tag_rw2"), "--type=bit", f"--tag=protocol=ab-eip&gateway={mlgx_gw}&plc=micrologix&name=N7:0/10",
              "--write=1", "--check=1", "--debug=4"]),
        # The SLC masked write: mask word then data word.  Getting the layout wrong was
        # accepted by the simulator and rejected by the PLC, so both polarities are checked.
        test("B bit data file Micrologix tag clear",
             [exe("tag_rw2"), "--type=bit", f"--tag=protocol=ab-eip&gateway={mlgx_gw}&plc=micrologix&name=B3:0/6",
              "--write=0", "--check=0", "--debug=4"]),
        test("N bit data file Micrologix tag clear",
             [exe("tag_rw2"), "--type=bit", f"--tag=protocol=ab-eip&gateway={mlgx_gw}&plc=micrologix&name=N7:0/10",
              "--write=0", "--check=0", "--debug=4"]),
        # The SLC masked write carries a mask word then a data word; a wrong width shows up
        # here and nowhere else.
        # The pattern keeps bit 0 set because the MicroLogix program drives B3/0.
        test("Micrologix B bit write changes only that bit (SLC masked write)",
             [exe("test_bit_write"), "--bit=6", "--pattern=0x5a5b", "--debug=4",
              f"--word-tag=protocol=ab-eip&gateway={mlgx_gw}&plc=micrologix&elem_count=1&name=B3:0",
              f"--bit-tag=protocol=ab-eip&gateway={mlgx_gw}&plc=micrologix&name=B3:0/6"]),
        test("Micrologix N bit write changes only that bit (SLC masked write)",
             [exe("test_bit_write"), "--bit=10", "--debug=4",
              f"--word-tag=protocol=ab-eip&gateway={mlgx_gw}&plc=micrologix&elem_count=1&name=N7:0",
              f"--bit-tag=protocol=ab-eip&gateway={mlgx_gw}&plc=micrologix&name=N7:0/10"]),
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
             expect_error="PLCTAG_ERR_UNSUPPORTED"),  # this write should NOT succeed
        # A data file the PLC does not have. This is the only way to reach the PCCC
        # remote-error path: the PLC answers with a PCCC status byte rather than data,
        # so the response checkers must report the error AND clear the tag's
        # in-progress flag. The DH+ case is the one that matters -- its checkers used
        # to clear that flag only on success, leaving the tag pending forever.
        test("missing data file Micrologix tag read",
             [exe("tag_rw2"), "--type=sint16", f"--tag=protocol=ab-eip&gateway={mlgx_gw}&plc=micrologix&name=N250:0",
              "--debug=4"],
             expect_error="PLCTAG_ERR_REMOTE_ERR"),  # N250 does not exist
        test("B data file PLC5 tag read/write",
             [exe("tag_rw2"), "--type=uint16", f"--tag=protocol=ab-eip&gateway={plc5_gw}&plc=plc5&elem_count=1&name=B3:0",
              "--debug=4", "--write=0", "--check"]),
        test("B bit data file PLC5 tag read/write",
             [exe("tag_rw2"), "--type=bit",
              f"--tag=protocol=ab-eip&gateway={plc5_gw}&plc=plc5&elem_count=1&name=B3:0/10", "--debug=4", "--write=1", "--check=1"]),
        # The PLC/5 read-modify-write: an AND mask and an OR mask, one element each.
        test("B bit data file PLC5 tag clear",
             [exe("tag_rw2"), "--type=bit",
              f"--tag=protocol=ab-eip&gateway={plc5_gw}&plc=plc5&elem_count=1&name=B3:0/10", "--debug=4", "--write=0",
              "--check=0"]),
        test("N data file PLC5 tag read/write",
             [exe("tag_rw2"), "--type=sint16", f"--tag=protocol=ab-eip&gateway={plc5_gw}&plc=plc5&elem_count=1&name=N7:0",
              "--debug=4", "--write=0", "--check"]),
        test("N bit data file PLC5 tag read/write",
             [exe("tag_rw2"), "--type=bit",
              f"--tag=protocol=ab-eip&gateway={plc5_gw}&plc=plc5&elem_count=1&name=N7:0/10", "--debug=4", "--write=1", "--check=1"]),
        # The PLC/5 read-modify-write is an AND mask and an OR mask, one element each.  An
        # inverted or over-wide mask passes the two tests above and fails this one.
        test("PLC5 B bit write changes only that bit (AND/OR mask pair)",
             [exe("test_bit_write"), "--bit=10", "--debug=4",
              f"--word-tag=protocol=ab-eip&gateway={plc5_gw}&plc=plc5&elem_count=1&name=B3:0",
              f"--bit-tag=protocol=ab-eip&gateway={plc5_gw}&plc=plc5&elem_count=1&name=B3:0/10"]),
        test("PLC5 N bit write changes only that bit (AND/OR mask pair)",
             [exe("test_bit_write"), "--bit=10", "--debug=4",
              f"--word-tag=protocol=ab-eip&gateway={plc5_gw}&plc=plc5&elem_count=1&name=N7:0",
              f"--bit-tag=protocol=ab-eip&gateway={plc5_gw}&plc=plc5&elem_count=1&name=N7:0/10"]),
        test("missing data file PLC5 tag read",
             [exe("tag_rw2"), "--type=sint16",
              f"--tag=protocol=ab-eip&gateway={plc5_gw}&plc=plc5&elem_count=1&name=N250:0", "--debug=4"],
             expect_error="PLCTAG_ERR_NOT_FOUND"),  # N250 does not exist
        # PCCC-mapped Logix tag (plc=lgxpccc): talks PCCC file-based addressing
        # (N7:0 etc.) to a ControlLogix CPU instead of native CIP tag names --
        # exercises eip_lgx_pccc.c, which no other test in this suite reaches.
        # Only the slot 5 CPU module has a DF1/PCCC data file mapped; slots 0
        # and 4 do not and were removed rather than left as expected failures.
        test("PCCC-mapped Logix tag read/write (slot 5)",
             [exe("tag_rw2"), "--type=sint16",
              f"--tag=protocol=ab_eip&gateway={logix_gw}&path=1,5&plc=lgxpccc&elem_count=1&name=N7:0",
              "--debug=4", "--write=42", "--check"]),
        # eip_lgx_pccc.c never looks at tag->is_bit, so a bit write here is a whole-word typed
        # write of the buffer the tag last read.  The test reads first, as an app must.
        test("PCCC-mapped Logix bit write changes only that bit (slot 5)",
             [exe("test_bit_write"), "--bit=10", "--debug=4",
              f"--word-tag=protocol=ab_eip&gateway={logix_gw}&path=1,5&plc=lgxpccc&elem_count=1&name=N7:0",
              f"--bit-tag=protocol=ab_eip&gateway={logix_gw}&path=1,5&plc=lgxpccc&elem_count=1&name=N7:0/10"]),
        test("basic DH+ bridging",
             [exe("tag_rw2"), "--type=uint8",
              f"--tag=protocol=ab_eip&gateway={dhp_bridge_gw}&path=1,2,A:27:1&cpu=plc5&elem_count=1&elem_size=2&name=N7:0",
              "--debug=4", "--write=42", "--check"]),
        test("basic DH+ bridging bit change",
             [exe("tag_rw2"), "--type=uint8",
              f"--tag=protocol=ab_eip&gateway={dhp_bridge_gw}&path=1,2,A:27:1&cpu=plc5&elem_count=1&elem_size=2&name=B3:0/10",
              "--debug=4", "--write=0"]),
        # The DH+ bridge rides the same PLC/5 mask pair over a different transport, so a
        # framing error that shifts the masks by the routing header shows up only here.
        test("DH+ bridged bit write changes only that bit",
             [exe("test_bit_write"), "--bit=10", "--debug=4",
              f"--word-tag=protocol=ab_eip&gateway={dhp_bridge_gw}&path=1,2,A:27:1&cpu=plc5&elem_count=1&elem_size=2&name=B3:0",
              f"--bit-tag=protocol=ab_eip&gateway={dhp_bridge_gw}&path=1,2,A:27:1&cpu=plc5&elem_count=1&elem_size=2&name=B3:0/10"]),
        test("missing data file DH+ bridged tag read",
             [exe("tag_rw2"), "--type=uint8",
              f"--tag=protocol=ab_eip&gateway={dhp_bridge_gw}&path=1,2,A:27:1&cpu=plc5&elem_count=1&elem_size=2&name=N250:0",
              "--debug=4"],
             expect_error="PLCTAG_ERR_NOT_FOUND"),  # N250 does not exist
        test("basic CIP bridging",
             [exe("tag_rw2"), "--type=sint32",
              f"--tag=protocol=ab_eip&gateway={cip_bridge_gw}&path=1,4,18,{cip_bridge_target},1,0&plc=lgx&name=TestBigArray[0]",
              "--debug=4", "--write=5", "--check"]),
        # A PCCC CPU reached through a ControlLogix chassis's Ethernet module instead of
        # directly.  The path holds no DH+ segment, so these are the direct rows carried
        # inside an Unconnected Send -- the PCCC request is routed, not reframed.
        test("CIP bridged MicroLogix tag read/write",
             [exe("tag_rw2"), "--type=sint16",
              f"--tag=protocol=ab-eip&gateway={cip_bridge_gw}&path={cip_bridge_out},{mlgx_gw}&plc=micrologix&name=N7:0",
              "--debug=4", "--write=42", "--check"]),
        test("CIP bridged MicroLogix bit write changes only that bit",
             [exe("test_bit_write"), "--bit=10", "--debug=4",
              f"--word-tag=protocol=ab-eip&gateway={cip_bridge_gw}&path={cip_bridge_out},{mlgx_gw}&plc=micrologix&elem_count=1&name=N7:0",
              f"--bit-tag=protocol=ab-eip&gateway={cip_bridge_gw}&path={cip_bridge_out},{mlgx_gw}&plc=micrologix&name=N7:0/10"]),
        test("CIP bridged PLC5 tag read/write",
             [exe("tag_rw2"), "--type=sint16",
              f"--tag=protocol=ab-eip&gateway={cip_bridge_gw}&path={cip_bridge_out},{plc5_gw}&plc=plc5&elem_count=1&name=N7:0",
              "--debug=4", "--write=42", "--check"]),
        test("CIP bridged PLC5 bit write changes only that bit",
             [exe("test_bit_write"), "--bit=10", "--debug=4",
              f"--word-tag=protocol=ab-eip&gateway={cip_bridge_gw}&path={cip_bridge_out},{plc5_gw}&plc=plc5&elem_count=1&name=N7:0",
              f"--bit-tag=protocol=ab-eip&gateway={cip_bridge_gw}&path={cip_bridge_out},{plc5_gw}&plc=plc5&elem_count=1&name=N7:0/10"]),
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
        # @identity with no path: the request is not routed onward, so it is answered by
        # whatever device owns the gateway address.  One per distinct entry point, which is
        # every way into the lab.  The routed form, which reaches a CPU in a chassis slot,
        # is the identity test in the Logix matrix below.
        test("identity, unrouted: L81 Ethernet port",
             [exe("get_identity"), f"--tag=protocol=ab_eip&gateway={logix_gw}&plc=generic&name=@identity&debug=3"]),
        test("identity, unrouted: ENBT 4.8",
             [exe("get_identity"), f"--tag=protocol=ab_eip&gateway={cip_bridge_gw}&plc=generic&name=@identity&debug=3"]),
        test("identity, unrouted: ENBT 6.6",
             [exe("get_identity"), f"--tag=protocol=ab_eip&gateway={cip_bridge_target}&plc=generic&name=@identity&debug=3"]),
        # The PLC/5 and the MicroLogix are single-box devices with no chassis to route into,
        # so unrouted is the only form they have.  Neither is a CIP-native PLC, which makes
        # them the test that Get_Attributes_All on the Identity object is answered by a
        # device whose data plane is PCCC.
        test("identity, unrouted: PLC/5",
             [exe("get_identity"), f"--tag=protocol=ab_eip&gateway={plc5_gw}&plc=generic&name=@identity&debug=3"]),
        test("identity, unrouted: MicroLogix 1100",
             [exe("get_identity"), f"--tag=protocol=ab_eip&gateway={mlgx_gw}&plc=generic&name=@identity&debug=3"]),
        # The L8x port is the one entry point the matrix below does not cover, so route
        # through it to the CPU behind it here.
        test("identity, routed: L81 v31.11 via L8x port",
             [exe("get_identity"),
              f"--tag=protocol=ab_eip&gateway={logix_gw}&path={logix_path}&plc=generic&name=@identity&debug=3"]),
        # @identity on each family's own PLC type, so it rides that family's connection.
        test("identity, Logix, connected",
             [exe("get_identity"),
              f"--tag=protocol=ab_eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&name=@identity&debug=3"]),
        test("identity, Logix, unconnected",
             [exe("get_identity"),
              f"--tag=protocol=ab_eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&use_connected_msg=0&name=@identity&debug=3"]),
        test("identity, PCCC-mapped Logix",
             [exe("get_identity"),
              f"--tag=protocol=ab_eip&gateway={logix_gw}&path={logix_path}&plc=lgx-pccc&name=@identity&debug=3"]),
        test("identity, PLC/5 direct",
             [exe("get_identity"), f"--tag=protocol=ab_eip&gateway={plc5_gw}&plc=plc5&name=@identity&debug=3"]),
        test("identity, MicroLogix direct",
             [exe("get_identity"), f"--tag=protocol=ab_eip&gateway={mlgx_gw}&plc=micrologix&name=@identity&debug=3"]),
        # A DH+ node has no CIP Identity object; the DHRIO bridge would answer in its place.
        test("identity, PLC/5 via DH+ is refused",
             [exe("get_identity"),
              f"--tag=protocol=ab_eip&gateway={dhp_bridge_gw}&path=1,2,A:27:1&plc=plc5&name=@identity&debug=3"],
             expect_error="PLCTAG_ERR_UNSUPPORTED"),
        # No plc: the connection reads the Identity object and picks the family from the
        # product name.  Read-only, so they disturb nothing the other tests write.
        test("no plc: L81 identified, read",
             [exe("tag_rw2"), "--type=sint32",
              f"--tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&elem_count=10&name=TestBigArray", "--debug=4"]),
        test("no plc: PLC/5 identified, read",
             [exe("tag_rw2"), "--type=sint16", f"--tag=protocol=ab-eip&gateway={plc5_gw}&name=N7:0", "--debug=4"]),
        test("no plc: MicroLogix identified, read",
             [exe("tag_rw2"), "--type=sint16", f"--tag=protocol=ab-eip&gateway={mlgx_gw}&name=N7:0", "--debug=4"]),
        # An ENBT answers Identity as a 1756 module that is not a CPU, so the create is refused.
        test("no plc: ENBT with no path is refused",
             [exe("tag_rw2"), "--type=sint32", f"--tag=protocol=ab-eip&gateway={cip_bridge_gw}&name=TestBigArray",
              "--debug=4"],
             expect_error="PLCTAG_ERR_BAD_PARAM"),
        # Identity cannot see past a DH+ bridge, so DH+ needs plc=.
        test("no plc: DH+ path is refused",
             [exe("tag_rw2"), "--type=sint16",
              f"--tag=protocol=ab-eip&gateway={dhp_bridge_gw}&path=1,2,A:27:1&name=N7:0", "--debug=4"],
             expect_error="PLCTAG_ERR_BAD_PARAM"),
        # Several ordinary tags read together, so the requests are bundled into one Multiple
        # Service Packet and the reply budget decides how many fit.  The raw CIP bundling test
        # covers the assembly; this covers it for tags that carry a reply size.
        test("bundled reads of several ordinary tags",
             group="stress", cmd=[exe("thread_stress"), "4",
              f"protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&name=TestBigArray"]),
        # Writing a tag the caller never read: the library fetches the type itself first.
        test("write before read",
             [exe("test_write_before_read"),
              f"--tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&elem_count=1&name=TestBigArray",
              f"--tag=protocol=ab-eip&gateway={logix_gw}&path={logix_path}&plc=ControlLogix&elem_count=1000&name=TestBigArray"]),
        # Each @connection test watches its own connection, so they run alongside anything.
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

        # Needs no tag to exist, so every cell can run it.  The path matters: with one the
        # request is routed to the CPU in that slot inside an Unconnected Send, without one
        # it stops at the entry module.  Those are the two branches of cip/identity_tag.c:184,
        # and this axis is what identifies each CPU.
        tests.append(test(f"{target.label}: identity",
                          [exe("get_identity"),
                           f"--tag=protocol=ab_eip&gateway={gw}&path={path}&plc=generic&name=@identity&debug=3"]))
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
        # No plc, so each CPU is identified through its own route.  @raw names no tag.
        tests.append(test(f"{target.label}: no plc, raw cip",
                          [exe("test_raw_cip"), f"--tag=protocol=ab-eip&gateway={gw}&path={path}&name=@raw"]))

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
    if t.expect_error is None:
        ok = proc.returncode == 0
    else:
        ok = proc.returncode != 0 and re.search(rf"^ERROR.*\b{t.expect_error}\b",
                                                 Path(t.log_file).read_text(errors="replace"), re.M) is not None
    end = time.monotonic()
    return Result(test=t, ok=ok, start=start, end=end)


def run_all(tests: list[Test], workers: int) -> list[Result]:
    """
    Start each test once its locks clash with no running test and no earlier waiting test, so
    tests that share a lock run one at a time and in manifest order.
    """
    pending = list(tests)
    running: dict = {}
    results: list[Result] = []

    with ThreadPoolExecutor(max_workers=workers) as pool:
        while pending or running:
            claimed = [t.resources for t in running.values()]
            for t in list(pending):
                if len(running) >= workers:
                    break
                blocked = any(conflicts(t.resources, c) for c in claimed)
                claimed.append(t.resources)
                if blocked:
                    continue
                pending.remove(t)
                running[pool.submit(run_test, t)] = t

            done, _ = wait(running, return_when=FIRST_COMPLETED)
            for fut in done:
                t = running.pop(fut)
                r = fut.result()
                _print_result(r)
                results.append(r)

    return results


def _print_result(r: Result) -> None:
    status = "OK" if r.ok else "FAILURE"
    print(f"Test {r.test.id}: {r.test.name}... {status} ({r.elapsed:.0f}s)")


def main() -> int:
    global TEST_DIR, LOG_DIR

    parser = argparse.ArgumentParser()
    parser.add_argument("test_dir")
    parser.add_argument("log_dir", nargs="?", default=".")
    parser.add_argument("--workers", type=int, default=8,
                         help="most tests at once (default 8).  The locks limit it further.  1 runs the whole "
                              "suite sequentially.")
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
    results = run_all(tests, args.workers)

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

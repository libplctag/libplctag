#!/usr/bin/env python3
"""
Capture PLC traffic for repeated test runs and generate replay dataset JSON.

Design goals:
- Python-only workflow (Scapy for capture + pcap parsing).
- Linux/macOS first.
- Focused EIP/CIP parsing sufficient for replay dataset generation.
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

try:
    from scapy.all import AsyncSniffer, IP, Raw, TCP, rdpcap, wrpcap  # type: ignore
except ModuleNotFoundError:
    AsyncSniffer = None
    IP = None
    Raw = None
    TCP = None
    rdpcap = None
    wrpcap = None


@dataclasses.dataclass
class WireMessage:
    direction: str  # "c2s" or "s2c"
    payload: bytes
    ts: float
    stream_key: str


@dataclasses.dataclass
class Transaction:
    request: bytes
    response: bytes
    req_key: str


def parse_hex_bytes(data: bytes) -> str:
    return data.hex()


def now_utc() -> str:
    return dt.datetime.now(tz=dt.timezone.utc).isoformat()


def ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def require_scapy() -> None:
    if AsyncSniffer is None or IP is None or Raw is None or TCP is None or rdpcap is None or wrpcap is None:
        raise RuntimeError("Scapy is required. Install with: python3 -m pip install scapy")


def run_capture(args: argparse.Namespace) -> int:
    require_scapy()

    out_dir = Path(args.out_dir).resolve()
    ensure_dir(out_dir)

    cmd = list(args.command)
    if cmd and cmd[0] == "--":
        cmd = cmd[1:]

    if not cmd:
        print("ERROR: capture requires a command after --", file=sys.stderr)
        return 2

    bpf = f"host {args.plc_ip} and tcp port {args.plc_port}"

    manifest: Dict[str, Any] = {
        "created_utc": now_utc(),
        "plc_ip": args.plc_ip,
        "plc_port": args.plc_port,
        "iface": args.iface,
        "runs": args.runs,
        "command": cmd,
        "bpf": bpf,
        "captures": [],
    }

    print(f"[capture] iface={args.iface} plc={args.plc_ip}:{args.plc_port} runs={args.runs}")
    print(f"[capture] bpf={bpf}")

    for run_idx in range(1, args.runs + 1):
        pcap_path = out_dir / f"run_{run_idx:03d}.pcapng"
        print(f"[capture] run {run_idx}/{args.runs}: starting sniffer")

        sniffer = AsyncSniffer(iface=args.iface, filter=bpf, store=True)
        sniffer.start()

        # Let capture settle before command starts.
        time.sleep(args.pre_command_delay)

        t0 = time.time()
        proc = subprocess.run(cmd, capture_output=True, text=True)
        elapsed = time.time() - t0

        # Capture a little after command exits to catch trailing packets.
        time.sleep(args.post_command_delay)

        packets = sniffer.stop()
        wrpcap(str(pcap_path), packets)

        run_meta = {
            "run": run_idx,
            "pcap": str(pcap_path),
            "captured_packets": len(packets),
            "command_rc": proc.returncode,
            "elapsed_sec": elapsed,
            "stdout": proc.stdout,
            "stderr": proc.stderr,
        }
        manifest["captures"].append(run_meta)

        print(
            f"[capture] run {run_idx}: packets={len(packets)} rc={proc.returncode} elapsed={elapsed:.2f}s -> {pcap_path}"
        )

        if proc.returncode != 0 and not args.keep_going:
            print("[capture] command failed and --keep-going is not set; stopping.", file=sys.stderr)
            break

    manifest_path = out_dir / "capture_manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(f"[capture] wrote manifest: {manifest_path}")

    return 0


def parse_eip_header(payload: bytes) -> Optional[Dict[str, Any]]:
    if len(payload) < 24:
        return None

    cmd = int.from_bytes(payload[0:2], "little")
    length = int.from_bytes(payload[2:4], "little")
    session = int.from_bytes(payload[4:8], "little")
    status = int.from_bytes(payload[8:12], "little")
    context = payload[12:20]
    options = int.from_bytes(payload[20:24], "little")

    return {
        "cmd": cmd,
        "length": length,
        "session": session,
        "status": status,
        "context": context,
        "options": options,
    }


def parse_cpf_items(eip_payload: bytes) -> List[Dict[str, Any]]:
    # EIP send data payload starts with interface handle (4), timeout (2), item count (2)
    if len(eip_payload) < 8:
        return []

    item_count = int.from_bytes(eip_payload[6:8], "little")
    off = 8
    items: List[Dict[str, Any]] = []

    for _ in range(item_count):
        if off + 4 > len(eip_payload):
            break
        item_type = int.from_bytes(eip_payload[off : off + 2], "little")
        item_len = int.from_bytes(eip_payload[off + 2 : off + 4], "little")
        off += 4
        if off + item_len > len(eip_payload):
            break
        data = eip_payload[off : off + item_len]
        off += item_len
        items.append({"type": item_type, "len": item_len, "data": data})

    return items


def parse_cpf_items_with_offsets(eip_payload: bytes, packet_offset: int = 24) -> List[Dict[str, Any]]:
    """Like parse_cpf_items, but includes absolute offsets into the full packet."""
    if len(eip_payload) < 8:
        return []

    item_count = int.from_bytes(eip_payload[6:8], "little")
    off = 8
    items: List[Dict[str, Any]] = []

    for _ in range(item_count):
        if off + 4 > len(eip_payload):
            break

        item_type = int.from_bytes(eip_payload[off : off + 2], "little")
        item_len = int.from_bytes(eip_payload[off + 2 : off + 4], "little")
        data_start = off + 4
        data_end = data_start + item_len
        if data_end > len(eip_payload):
            break

        items.append(
            {
                "type": item_type,
                "len": item_len,
                "data": eip_payload[data_start:data_end],
                "item_abs_offset": packet_offset + off,
                "data_abs_offset": packet_offset + data_start,
            }
        )
        off = data_end

    return items


def get_cip_view(payload: bytes) -> Optional[Dict[str, Any]]:
    hdr = parse_eip_header(payload)
    if not hdr:
        return None

    cmd = hdr["cmd"]
    if cmd not in (0x006F, 0x0070):
        return None

    eip_body = payload[24 : 24 + hdr["length"]]
    items = parse_cpf_items_with_offsets(eip_body, packet_offset=24)

    for item in items:
        if item["type"] == 0x00B2 and item["len"] >= 1:
            return {
                "service": item["data"][0],
                "cip": item["data"],
                "cip_abs_offset": item["data_abs_offset"],
                "connected": False,
                "connected_seq_abs_offset": None,
                "items": items,
            }

        if item["type"] == 0x00B1 and item["len"] >= 3:
            return {
                "service": item["data"][2],
                "cip": item["data"][2:],
                "cip_abs_offset": item["data_abs_offset"] + 2,
                "connected": True,
                "connected_seq_abs_offset": item["data_abs_offset"],
                "items": items,
            }

    return None


def extract_protocol_fields(payload: bytes) -> List[Dict[str, Any]]:
    """Extract dynamic protocol fields with absolute offsets for replay tooling."""
    fields: List[Dict[str, Any]] = []

    hdr = parse_eip_header(payload)
    if not hdr:
        return fields

    # EIP session handle.
    fields.append(
        {
            "name": "eip_session_handle",
            "offset": 4,
            "size": 4,
            "value_hex": payload[4:8].hex() if len(payload) >= 8 else "",
        }
    )

    # EIP sender context.
    if len(payload) >= 20:
        fields.append(
            {
                "name": "eip_sender_context",
                "offset": 12,
                "size": 8,
                "value_hex": payload[12:20].hex(),
            }
        )

    view = get_cip_view(payload)
    if not view:
        return fields

    # Connected CPF sequence and address IDs.
    if view["connected_seq_abs_offset"] is not None:
        seq_off = int(view["connected_seq_abs_offset"])
        if seq_off + 2 <= len(payload):
            fields.append(
                {
                    "name": "connected_sequence",
                    "offset": seq_off,
                    "size": 2,
                    "value_hex": payload[seq_off : seq_off + 2].hex(),
                }
            )

    for item in view["items"]:
        if item["type"] == 0x00A1 and item["len"] >= 4:
            off = int(item["data_abs_offset"])
            fields.append(
                {
                    "name": "cpf_connected_address_connection_id",
                    "offset": off,
                    "size": 4,
                    "value_hex": payload[off : off + 4].hex(),
                }
            )

    # Forward Open / Forward Open response (normal and extended).
    service = int(view["service"])
    cip = bytes(view["cip"])
    cip_abs = int(view["cip_abs_offset"])

    if len(cip) >= 2:
        path_bytes = int(cip[1]) * 2
        body_off = 2 + path_bytes

        # Forward Open request services: 0x54, 0x5B
        if service in (0x54, 0x5B) and body_off + 10 < len(cip):
            ot_off = cip_abs + body_off + 2
            to_off = cip_abs + body_off + 6
            if to_off + 4 <= len(payload):
                fields.append(
                    {
                        "name": "forward_open_originator_to_target_connection_id",
                        "offset": ot_off,
                        "size": 4,
                        "value_hex": payload[ot_off : ot_off + 4].hex(),
                    }
                )
                fields.append(
                    {
                        "name": "forward_open_target_to_originator_connection_id",
                        "offset": to_off,
                        "size": 4,
                        "value_hex": payload[to_off : to_off + 4].hex(),
                    }
                )

        # Forward Open response services: 0xD4, 0xDB
        if service in (0xD4, 0xDB) and len(cip) >= 4:
            ext_words = int(cip[3])
            fo_rsp_off = 4 + (ext_words * 2)
            ot_off = cip_abs + fo_rsp_off
            to_off = cip_abs + fo_rsp_off + 4
            if to_off + 4 <= len(payload):
                fields.append(
                    {
                        "name": "forward_open_response_target_to_originator_connection_id",
                        "offset": ot_off,
                        "size": 4,
                        "value_hex": payload[ot_off : ot_off + 4].hex(),
                    }
                )
                fields.append(
                    {
                        "name": "forward_open_response_originator_to_target_connection_id",
                        "offset": to_off,
                        "size": 4,
                        "value_hex": payload[to_off : to_off + 4].hex(),
                    }
                )

    return fields


def extract_cip_service(payload: bytes) -> Optional[int]:
    hdr = parse_eip_header(payload)
    if not hdr:
        return None

    cmd = hdr["cmd"]
    if cmd not in (0x006F, 0x0070):
        return None

    eip_body = payload[24 : 24 + hdr["length"]]
    items = parse_cpf_items(eip_body)

    # Unconnected data item: 0x00B2
    for item in items:
        if item["type"] == 0x00B2 and len(item["data"]) >= 1:
            return item["data"][0]

    # Connected data item: 0x00B1, first 2 bytes are sequence number.
    for item in items:
        if item["type"] == 0x00B1 and len(item["data"]) >= 3:
            return item["data"][2]

    return None


def transaction_key(req: bytes) -> str:
    hdr = parse_eip_header(req)
    if not hdr:
        return f"raw_len:{len(req)}"

    cmd = hdr["cmd"]
    cip = extract_cip_service(req)
    if cip is None:
        return f"eip_cmd:0x{cmd:04x}"
    return f"eip_cmd:0x{cmd:04x}|cip_srv:0x{cip:02x}"


def stream_key(src: str, sport: int, dst: str, dport: int) -> str:
    left = f"{src}:{sport}"
    right = f"{dst}:{dport}"
    return "|".join(sorted([left, right]))


def load_wire_messages(pcap_path: Path, plc_ip: str, plc_port: int) -> List[WireMessage]:
    require_scapy()

    pkts = rdpcap(str(pcap_path))
    out: List[WireMessage] = []

    for pkt in pkts:
        if IP not in pkt or TCP not in pkt or Raw not in pkt:
            continue

        ip = pkt[IP]
        tcp = pkt[TCP]
        raw = bytes(pkt[Raw].load)
        if not raw:
            continue

        if tcp.sport != plc_port and tcp.dport != plc_port:
            continue

        src = str(ip.src)
        dst = str(ip.dst)

        if dst == plc_ip and tcp.dport == plc_port:
            direction = "c2s"
        elif src == plc_ip and tcp.sport == plc_port:
            direction = "s2c"
        elif tcp.dport == plc_port:
            direction = "c2s"
        elif tcp.sport == plc_port:
            direction = "s2c"
        else:
            continue

        out.append(
            WireMessage(
                direction=direction,
                payload=raw,
                ts=float(pkt.time),
                stream_key=stream_key(src, int(tcp.sport), dst, int(tcp.dport)),
            )
        )

    out.sort(key=lambda m: m.ts)
    return out


def build_transactions(messages: List[WireMessage]) -> List[Transaction]:
    pending: List[WireMessage] = []
    txns: List[Transaction] = []

    for msg in messages:
        if msg.direction == "c2s":
            pending.append(msg)
            continue

        if msg.direction == "s2c" and pending:
            req = pending.pop(0)
            txns.append(Transaction(request=req.payload, response=msg.payload, req_key=transaction_key(req.payload)))

    return txns


def compute_static_mask(byte_seqs: Sequence[bytes]) -> Tuple[bytes, bytes]:
    """
    Compute template + mask by comparing every byte position across every run.

    mask byte semantics:
      0xFF = must match exactly
      0x00 = dynamic/ignored
    """
    if not byte_seqs:
        return b"", b""

    template = bytearray(byte_seqs[0])
    mask = bytearray([0xFF] * len(template))

    run_count = len(byte_seqs)
    for i in range(len(template)):
        values = []
        for seq in byte_seqs:
            if i < len(seq):
                values.append(seq[i])

        # If any run is missing this byte (length mismatch), treat as dynamic.
        if len(values) != run_count:
            mask[i] = 0x00
            continue

        # Any value disagreement across runs marks the byte dynamic.
        if len(set(values)) > 1:
            mask[i] = 0x00

    return bytes(template), bytes(mask)


def apply_protocol_dynamic_mask(packet: bytes, mask: bytearray) -> None:
    """
    Apply protocol-aware masking for fields that are semantically dynamic even
    when they do not vary in a small capture sample.
    """
    if len(packet) < 24 or len(mask) < 24:
        return

    hdr = parse_eip_header(packet)
    if not hdr:
        return

    cmd = hdr["cmd"]

    # Sender context is caller-provided and should not be required to match.
    for i in range(12, min(20, len(mask))):
        mask[i] = 0x00

    # Session handle is dynamic for post-register traffic.
    if cmd in (0x006F, 0x0070, 0x0066):
        for i in range(4, min(8, len(mask))):
            mask[i] = 0x00

    # Connected data item sequence number is dynamic.
    seq_off = parse_connected_sequence_offset(packet)
    if seq_off is not None:
        for i in range(seq_off, min(seq_off + 2, len(mask))):
            mask[i] = 0x00

    # Connected address item data (connection ID) is dynamic and should not be
    # treated as a stable match field even if identical across a few runs.
    if cmd == 0x0070:
        body = packet[24 : 24 + hdr["length"]]
        if len(body) >= 8:
            item_count = int.from_bytes(body[6:8], "little")
            off = 8
            for _ in range(item_count):
                if off + 4 > len(body):
                    break
                item_type = int.from_bytes(body[off : off + 2], "little")
                item_len = int.from_bytes(body[off + 2 : off + 4], "little")
                data_start = off + 4
                data_end = data_start + item_len
                if data_end > len(body):
                    break

                if item_type == 0x00A1 and item_len > 0:
                    abs_start = 24 + data_start
                    abs_end = 24 + data_end
                    for i in range(abs_start, min(abs_end, len(mask))):
                        mask[i] = 0x00

                off = data_end

    # Also apply masking based on extracted protocol field offsets so both
    # normal and extended Forward Open connection IDs are always dynamic.
    for field in extract_protocol_fields(packet):
        name = field.get("name", "")
        off = int(field.get("offset", -1))
        size = int(field.get("size", 0))
        if off < 0 or size <= 0:
            continue

        if (
            "session_handle" in name
            or "sender_context" in name
            or "connected_sequence" in name
            or "connection_id" in name
        ):
            for i in range(off, min(off + size, len(mask))):
                mask[i] = 0x00


def compute_masked_template(byte_seqs: Sequence[bytes]) -> Tuple[bytes, bytes]:
    template, mask = compute_static_mask(byte_seqs)
    if not template:
        return template, mask

    mutable_mask = bytearray(mask)
    apply_protocol_dynamic_mask(template, mutable_mask)
    return template, bytes(mutable_mask)


def parse_connected_sequence_offset(payload: bytes) -> Optional[int]:
    hdr = parse_eip_header(payload)
    if not hdr or hdr["cmd"] != 0x0070:
        return None

    body = payload[24 : 24 + hdr["length"]]
    if len(body) < 8:
        return None

    item_count = int.from_bytes(body[6:8], "little")
    off = 8
    for _ in range(item_count):
        if off + 4 > len(body):
            return None
        item_type = int.from_bytes(body[off : off + 2], "little")
        item_len = int.from_bytes(body[off + 2 : off + 4], "little")
        data_start = off + 4
        data_end = data_start + item_len
        if data_end > len(body):
            return None

        if item_type == 0x00B1 and item_len >= 2:
            # Return offset within full EIP packet.
            return 24 + data_start

        off = data_end

    return None


def infer_patch_rules(req_template: bytes, rsp_template: bytes) -> List[Dict[str, Any]]:
    rules: List[Dict[str, Any]] = []

    if len(rsp_template) >= 24:
        rules.append({"type": "recalc_eip_length", "offset": 2, "size": 2})
        rules.append({"type": "set_session_handle", "offset": 4, "size": 4})

    if len(req_template) >= 20 and len(rsp_template) >= 20:
        rules.append({"type": "copy_from_request", "src_offset": 12, "dst_offset": 12, "size": 8, "name": "sender_context"})

    seq_off = parse_connected_sequence_offset(rsp_template)
    if seq_off is not None:
        rules.append({"type": "set_connected_sequence", "offset": seq_off, "size": 2})

    return rules


def generate_dataset(input_dir: Path, plc_ip: str, plc_port: int) -> Dict[str, Any]:
    pcap_files = sorted(input_dir.glob("run_*.pcap*"))
    if len(pcap_files) < 1:
        raise RuntimeError(f"No capture files found in {input_dir}")

    runs: List[List[Transaction]] = []
    for pcap in pcap_files:
        msgs = load_wire_messages(pcap, plc_ip, plc_port)
        txns = build_transactions(msgs)
        runs.append(txns)

    min_txn = min(len(r) for r in runs)

    entries: List[Dict[str, Any]] = []
    for idx in range(min_txn):
        reqs = [runs[r][idx].request for r in range(len(runs))]
        rsps = [runs[r][idx].response for r in range(len(runs))]

        req_template, req_mask = compute_masked_template(reqs)
        rsp_template, rsp_mask = compute_masked_template(rsps)

        req_key = runs[0][idx].req_key

        entry = {
            "id": idx + 1,
            "key": req_key,
            "request": {
                "template_hex": parse_hex_bytes(req_template),
                "mask_hex": parse_hex_bytes(req_mask),
                "length": len(req_template),
                "dynamic_byte_count": sum(1 for b in req_mask if b == 0),
            },
            "response": {
                "template_hex": parse_hex_bytes(rsp_template),
                "mask_hex": parse_hex_bytes(rsp_mask),
                "length": len(rsp_template),
                "dynamic_byte_count": sum(1 for b in rsp_mask if b == 0),
                "patch_rules": infer_patch_rules(req_template, rsp_template),
            },
        }

        entries.append(entry)

    return {
        "version": 1,
        "generated_utc": now_utc(),
        "plc_ip": plc_ip,
        "plc_port": plc_port,
        "source_pcaps": [str(p) for p in pcap_files],
        "runs": len(runs),
        "aligned_transaction_count": min_txn,
        "entries": entries,
        "notes": [
            "Mask bytes use ff=must-match and 00=ignore/dynamic.",
            "Patch rules are intentionally conservative and can be extended for ForwardOpen connection ID mapping.",
            "This first version assumes TCP payload granularity is sufficient for these test captures.",
        ],
    }


def run_generate(args: argparse.Namespace) -> int:
    input_dir = Path(args.input_dir).resolve()
    out_file = Path(args.output).resolve()

    dataset = generate_dataset(input_dir, args.plc_ip, args.plc_port)

    ensure_dir(out_file.parent)
    out_file.write_text(json.dumps(dataset, indent=2), encoding="utf-8")

    print(f"[generate] wrote dataset: {out_file}")
    print(f"[generate] entries={dataset['aligned_transaction_count']} runs={dataset['runs']}")
    return 0


def run_analyze(args: argparse.Namespace) -> int:
    input_dir = Path(args.input_dir).resolve()
    pcap_files = sorted(input_dir.glob("run_*.pcap*"))
    if not pcap_files:
        print(f"ERROR: no run_*.pcap* files in {input_dir}", file=sys.stderr)
        return 2

    for pcap in pcap_files:
        msgs = load_wire_messages(pcap, args.plc_ip, args.plc_port)
        txns = build_transactions(msgs)
        print(f"[analyze] {pcap.name}: messages={len(msgs)} transactions={len(txns)}")

        # Show first few keys to validate alignment shape quickly.
        preview = [t.req_key for t in txns[:8]]
        if preview:
            print("  keys:", ", ".join(preview))

    return 0


def run_all(args: argparse.Namespace) -> int:
    print("[run] phase 1/3 capture")
    rc = run_capture(args)
    if rc != 0:
        return rc

    analyze_args = argparse.Namespace(
        input_dir=args.out_dir,
        plc_ip=args.plc_ip,
        plc_port=args.plc_port,
    )

    print("[run] phase 2/3 analyze")
    rc = run_analyze(analyze_args)
    if rc != 0:
        return rc

    generate_args = argparse.Namespace(
        input_dir=args.out_dir,
        plc_ip=args.plc_ip,
        plc_port=args.plc_port,
        output=args.output,
    )

    print("[run] phase 3/3 generate")
    return run_generate(generate_args)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Capture PLC traffic and generate replay dataset.")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_cap = sub.add_parser("capture", help="Capture traffic while running a test command multiple times.")
    p_cap.add_argument("--iface", required=True, help="Capture interface, e.g. en0 or eth0")
    p_cap.add_argument("--plc-ip", required=True, help="Real PLC IP")
    p_cap.add_argument("--plc-port", type=int, default=44818, help="PLC TCP port (default 44818)")
    p_cap.add_argument("--runs", type=int, default=3, help="How many repeated runs (default 3)")
    p_cap.add_argument("--out-dir", required=True, help="Output directory for captures and manifest")
    p_cap.add_argument("--pre-command-delay", type=float, default=0.25, help="Seconds to wait after sniffer start")
    p_cap.add_argument("--post-command-delay", type=float, default=0.50, help="Seconds to keep capturing after command")
    p_cap.add_argument("--keep-going", action="store_true", help="Continue remaining runs even if command fails")
    p_cap.add_argument("command", nargs=argparse.REMAINDER, help="Command to run, provide after --")
    p_cap.set_defaults(func=run_capture)

    p_an = sub.add_parser("analyze", help="Summarize captured transactions.")
    p_an.add_argument("--input-dir", required=True, help="Directory containing run_*.pcapng")
    p_an.add_argument("--plc-ip", required=True, help="Real PLC IP")
    p_an.add_argument("--plc-port", type=int, default=44818, help="PLC TCP port")
    p_an.set_defaults(func=run_analyze)

    p_gen = sub.add_parser("generate", help="Generate replay dataset JSON from repeated captures.")
    p_gen.add_argument("--input-dir", required=True, help="Directory containing run_*.pcapng")
    p_gen.add_argument("--plc-ip", required=True, help="Real PLC IP")
    p_gen.add_argument("--plc-port", type=int, default=44818, help="PLC TCP port")
    p_gen.add_argument("--output", required=True, help="Output replay dataset JSON path")
    p_gen.set_defaults(func=run_generate)

    p_run = sub.add_parser("run", help="Run capture, analyze, and generate in one command.")
    p_run.add_argument("--iface", required=True, help="Capture interface, e.g. en0 or eth0")
    p_run.add_argument("--plc-ip", required=True, help="Real PLC IP")
    p_run.add_argument("--plc-port", type=int, default=44818, help="PLC TCP port (default 44818)")
    p_run.add_argument("--runs", type=int, default=3, help="How many repeated runs (default 3)")
    p_run.add_argument("--out-dir", required=True, help="Output directory for captures and manifest")
    p_run.add_argument("--output", required=True, help="Output replay dataset JSON path")
    p_run.add_argument("--pre-command-delay", type=float, default=0.25, help="Seconds to wait after sniffer start")
    p_run.add_argument("--post-command-delay", type=float, default=0.50, help="Seconds to keep capturing after command")
    p_run.add_argument("--keep-going", action="store_true", help="Continue remaining runs even if command fails")
    p_run.add_argument("command", nargs=argparse.REMAINDER, help="Command to run, provide after --")
    p_run.set_defaults(func=run_all)

    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return int(args.func(args))
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

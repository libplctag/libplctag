# Capture And Replay Dataset Tool

This directory contains a Python-only toolchain for:

- capturing real PLC traffic while running an existing test command,
- comparing multiple captures of the same test,
- generating a replay dataset JSON for a future replay server.

The implementation is intentionally Linux/macOS focused and assumes capture can run with root/elevated privileges.

## Files

- `capture_replay_dataset.py` - main CLI

## Requirements

- Python 3.9+
- `scapy` (`pip install scapy`)
- root (or equivalent capture permissions)

## Quick Start

Run capture, analyze, and generate in one command:

```bash
sudo python3 src/tools/capture/capture_replay_dataset.py run \
  --iface en0 \
  --plc-ip 192.168.1.10 \
  --plc-port 44818 \
  --runs 3 \
  --out-dir /tmp/libplctag_capture \
  --output /tmp/libplctag_capture/replay_dataset.json \
  -- ./build/bin/test_raw_cip
```

If you want to run phases separately, use the subcommands below.

Capture three runs of the same test:

```bash
sudo python3 src/tools/capture/capture_replay_dataset.py capture \
  --iface en0 \
  --plc-ip 192.168.1.10 \
  --plc-port 44818 \
  --runs 3 \
  --out-dir /tmp/libplctag_capture \
  -- ./build/bin/test_raw_cip
```

Generate replay dataset JSON:

```bash
python3 src/tools/capture/capture_replay_dataset.py generate \
  --input-dir /tmp/libplctag_capture \
  --plc-ip 192.168.1.10 \
  --plc-port 44818 \
  --output /tmp/libplctag_capture/replay_dataset.json
```

## Notes

- Current matching logic aligns request/response pairs by observed order and protocol key.
- TCP stream reassembly is intentionally minimal in this first version and assumes request/response payloads are typically delivered as complete TCP payload units for the target tests.
- Generated patch directives include key dynamic fields (`session_handle`, `sender_context`, connected sequence), with room to add more rules over time.

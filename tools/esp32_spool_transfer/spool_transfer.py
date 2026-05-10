#!/usr/bin/env python3
"""
Host-side spool transfer tool for pqueue ESP32 diagnostics.

Usage:
    python3 spool_transfer.py [--port PORT] [--baud BAUD] [--out FILE]

Connects to the ESP32 running esp32_spool_transfer firmware, captures the
spool dump, saves it locally, then sends DONE to release the lock.
"""

import argparse
import sys
import time
import serial


DEFAULT_PORT = "/dev/ttyACM0"
DEFAULT_BAUD = 115200
DEFAULT_OUT = "build/pqueue-spools/pqueue_spool/pqueue.spool"
DUMP_TIMEOUT_S = 120
COMMAND_TIMEOUT_S = 10


def parse_args():
    p = argparse.ArgumentParser(description="Capture pqueue spool from ESP32 over serial.")
    p.add_argument("--port", default=DEFAULT_PORT, help=f"Serial port (default: {DEFAULT_PORT})")
    p.add_argument("--baud", type=int, default=DEFAULT_BAUD, help=f"Baud rate (default: {DEFAULT_BAUD})")
    p.add_argument("--out", default=DEFAULT_OUT, help=f"Output file (default: {DEFAULT_OUT})")
    p.add_argument("--no-reset", action="store_true", help="Don't toggle DTR to reset the device")
    return p.parse_args()


def log(msg):
    print(msg, flush=True)


def main():
    args = parse_args()

    log(f"Opening {args.port} at {args.baud} baud")
    s = serial.Serial(args.port, args.baud, timeout=1)

    if not args.no_reset:
        log("Resetting device via DTR...")
        s.dtr = False
        time.sleep(0.1)
        s.dtr = True
        time.sleep(2)
    else:
        log("Skipping reset (--no-reset)")

    chunks = {}
    spool_size = None
    deadline = time.time() + DUMP_TIMEOUT_S

    log("Waiting for dump...")

    while time.time() < deadline:
        raw = s.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").rstrip()

        if line.startswith("CHUNK:"):
            # CHUNK:<offset_hex>:<hexdata>
            parts = line.split(":", 2)
            if len(parts) != 3:
                log(f"  WARNING: malformed chunk line: {line[:64]}")
                continue
            offset = int(parts[1], 16)
            data = bytes.fromhex(parts[2])
            chunks[offset] = data
            print(f"  {offset:#010x} {len(data)} bytes\r", end="", flush=True)
        elif line.startswith("SPOOL_SIZE:"):
            spool_size = int(line.split(":", 1)[1])
            log(f"Spool size: {spool_size} bytes")
        elif line == "SPOOL_END":
            print()  # newline after progress
            log("Dump complete.")
            break
        elif line.startswith("ERROR:"):
            print()
            log(f"Device error: {line}")
            s.close()
            sys.exit(1)
        else:
            log(f"  [{line}]")
    else:
        print()
        log("ERROR: timed out waiting for SPOOL_END")
        s.close()
        sys.exit(1)

    if spool_size is None:
        log("ERROR: never received SPOOL_SIZE")
        s.close()
        sys.exit(1)

    # Reassemble
    total_received = sum(len(d) for d in chunks.values())
    if total_received != spool_size:
        log(f"ERROR: size mismatch — got {total_received} bytes, expected {spool_size}")
        s.close()
        sys.exit(1)

    spool_data = bytearray(spool_size)
    for offset, data in chunks.items():
        spool_data[offset:offset + len(data)] = data

    with open(args.out, "wb") as f:
        f.write(spool_data)
    log(f"Saved {spool_size} bytes to {args.out}")

    # Wait for READY then send DONE
    deadline = time.time() + COMMAND_TIMEOUT_S
    while time.time() < deadline:
        raw = s.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").rstrip()
        log(f"  [{line}]")
        if line == "READY":
            s.write(b"DONE\n")
            log("Sent DONE")
            break
    else:
        log("WARNING: timed out waiting for READY — lock may still be held on device")

    # Drain remaining output
    deadline = time.time() + 5
    while time.time() < deadline:
        raw = s.readline()
        if raw:
            log(f"  [{raw.decode('utf-8', errors='replace').rstrip()}]")

    s.close()
    log("Done.")


if __name__ == "__main__":
    main()

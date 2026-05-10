#!/usr/bin/env python3
"""
Host-side spool transfer tool for pqueue ESP32 diagnostics.

Usage:
    # Dump only (inspect locally, release lock immediately):
    python3 spool_transfer.py [--port PORT] [--out FILE]

    # Dump and hold lock until you are ready to upload or abort:
    python3 spool_transfer.py --hold-lock [--upload FILE]

    # Release a held lock (device still alive at READY prompt):
    python3 spool_transfer.py --release-lock

WARNING — lock and spool consistency:

  Without --hold-lock, the lock is released immediately after the dump.
  The app will resume and may modify the spool before you upload anything.
  Only use the dumped spool for read-only inspection in this mode.

  With --hold-lock, the lock is held for the lifetime of this process.
  If the device reboots while the lock is held — for any reason — the stale
  lock will be cleared on the next boot and the app will resume. If this
  happens, DO NOT upload. The spool you dumped is no longer current.
  Treat any unexpected disconnect or timeout as a signal to abort.
"""

import argparse
import sys
import time
import serial


DEFAULT_PORT = "/dev/ttyACM0"
DEFAULT_BAUD = 115200
DEFAULT_OUT = "build/pqueue-spools/pqueue_spool/pqueue.spool"
DUMP_TIMEOUT_S = 120
UPLOAD_CHUNK_TIMEOUT_S = 30
COMMAND_TIMEOUT_S = 10
CHUNK_SIZE = 256


def parse_args():
    p = argparse.ArgumentParser(description="Dump and optionally repair pqueue spool over serial.")
    p.add_argument("--port", default=DEFAULT_PORT, help=f"Serial port (default: {DEFAULT_PORT})")
    p.add_argument("--baud", type=int, default=DEFAULT_BAUD, help=f"Baud rate (default: {DEFAULT_BAUD})")
    p.add_argument("--out", default=DEFAULT_OUT, help=f"Where to save the dumped spool (default: {DEFAULT_OUT})")
    p.add_argument("--upload", metavar="FILE", help="Repaired spool file to upload back to device")
    p.add_argument("--no-reset", action="store_true", help="Don't toggle DTR to reset the device")
    return p.parse_args()


def log(msg):
    print(msg, flush=True)


def read_line(s, timeout_s):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        raw = s.readline()
        if raw:
            return raw.decode("utf-8", errors="replace").rstrip()
    return None


def do_dump(s):
    chunks = {}
    spool_size = None
    bad_lines = 0
    deadline = time.time() + DUMP_TIMEOUT_S

    log("Waiting for dump...")

    while time.time() < deadline:
        raw = s.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").rstrip()

        if line.startswith("CHUNK:"):
            parts = line.split(":", 2)
            if len(parts) != 3:
                bad_lines += 1
                log(f"WARNING: malformed chunk (skipped): {line[:64]}")
                s.write(b"ACK\n")
                continue
            try:
                offset = int(parts[1], 16)
                data = bytes.fromhex(parts[2])
            except ValueError:
                bad_lines += 1
                log(f"WARNING: unparseable chunk (skipped): {line[:64]}")
                s.write(b"ACK\n")
                continue
            chunks[offset] = data
            s.write(b"ACK\n")
            log(f"  chunk {offset:#010x} {len(data)} bytes")
        elif line.startswith("SPOOL_SIZE:"):
            spool_size = int(line.split(":", 1)[1])
            log(f"Spool size: {spool_size} bytes")
        elif line == "SPOOL_END":
            log("Dump complete.")
            break
        elif line.startswith("ERROR:"):
            log(f"Device error: {line}")
            return None, None
        else:
            log(f"  [{line}]")
    else:
        log("ERROR: timed out waiting for SPOOL_END")
        return None, None

    if spool_size is None:
        log("ERROR: never received SPOOL_SIZE")
        return None, None

    missing = []
    offset = 0
    while offset < spool_size:
        if offset not in chunks:
            missing.append(offset)
        offset += CHUNK_SIZE

    if missing or bad_lines:
        log(f"ERROR: transfer incomplete — {len(missing)} missing chunk(s), {bad_lines} bad line(s)")
        for m in missing[:10]:
            log(f"  missing offset {m:#010x}")
        if len(missing) > 10:
            log(f"  ... and {len(missing) - 10} more")
        return None, None

    spool_data = bytearray(spool_size)
    for off, data in chunks.items():
        spool_data[off:off + len(data)] = data

    return spool_size, bytes(spool_data)


def do_upload(s, upload_path):
    with open(upload_path, "rb") as f:
        upload_data = f.read()

    total = len(upload_data)
    log(f"Uploading {total} bytes from {upload_path}")
    s.write(f"UPLOAD {total}\n".encode())

    line = read_line(s, COMMAND_TIMEOUT_S)
    if line is None:
        log("ERROR: timed out waiting for READY_UPLOAD")
        return False
    log(f"  [{line}]")
    if line != "READY_UPLOAD":
        log(f"ERROR: expected READY_UPLOAD, got: {line}")
        return False

    offset = 0
    while offset < total:
        chunk = upload_data[offset:offset + CHUNK_SIZE]
        line_out = f"CHUNK:{offset:08x}:{chunk.hex()}\n"
        s.write(line_out.encode())
        log(f"  upload chunk {offset:#010x} {len(chunk)} bytes")
        offset += len(chunk)

    s.write(b"SPOOL_END\n")

    # Device writes and renames, then responds
    line = read_line(s, UPLOAD_CHUNK_TIMEOUT_S)
    if line is None:
        log("ERROR: timed out waiting for upload result")
        return False
    log(f"  [{line}]")
    return line.startswith("UPLOAD_OK")


def main():
    args = parse_args()

    log(f"Opening {args.port} at {args.baud} baud")
    s = serial.Serial(args.port, args.baud, timeout=5)

    if not args.no_reset:
        log("Resetting device via DTR...")
        s.dtr = False
        time.sleep(0.1)
        s.dtr = True
        time.sleep(2)
    else:
        log("Skipping reset (--no-reset)")

    spool_size, spool_data = do_dump(s)
    if spool_data is None:
        s.close()
        sys.exit(1)

    with open(args.out, "wb") as f:
        f.write(spool_data)
    log(f"Saved {spool_size} bytes to {args.out}")

    # Wait for READY
    line = read_line(s, COMMAND_TIMEOUT_S)
    if line is None:
        log("WARNING: timed out waiting for READY — lock may still be held on device")
        s.close()
        sys.exit(1)
    log(f"  [{line}]")
    if line != "READY":
        log(f"ERROR: expected READY, got: {line}")
        s.close()
        sys.exit(1)

    if args.upload:
        ok = do_upload(s, args.upload)
        if not ok:
            log("Upload failed.")
            s.write(b"DONE\n")
            s.close()
            sys.exit(1)
    else:
        s.write(b"DONE\n")
        log("Sent DONE")

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

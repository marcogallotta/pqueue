#!/usr/bin/env python3
"""
Receive pqueue doctor dump protocol and save files to disk.

Stdin mode (pipe from pqueue-doctor-dump):
    ./build/pqueue-doctor-dump --base-path /path/to/spool | \\
        python3 tools/pqueue_doctor_recv.py --out-dir /tmp/dump

Serial mode (live device):
    python3 tools/pqueue_doctor_recv.py \\
        --port /dev/ttyACM0 \\
        --target api_outbox:/pqueue_api_spool \\
        --out-dir /tmp/dump

    Multiple targets (each dumped into a named subdirectory):
    python3 tools/pqueue_doctor_recv.py \\
        --port /dev/ttyACM0 \\
        --targets pqueue_doctor.targets \\
        --out-dir /tmp/dump

    Targets file format (one target per line, # comments allowed):
        api_outbox /pqueue_api_spool
        other_queue /other_spool

Protocol:
    DUMP_BEGIN
    FILE_BEGIN name=<name> size=<bytes>
    <hex-encoded lines>
    FILE_END name=<name> crc=<crc32hex>
    FILE_ERROR name=<name> message=<error_code>
    DUMP_END files=<n> errors=<n>
"""

import argparse
import os
import re
import sys
import time


DEFAULT_PORT = "/dev/ttyACM0"
DEFAULT_BAUD = 115200
RECV_TIMEOUT_S = 60
READY_TIMEOUT_S = 30
MAX_FILE_SIZE = 1 * 1024 * 1024  # 1 MB -- any larger is clearly wrong for pqueue files

# Only accept names that look like pqueue data files.
_VALID_NAME = re.compile(r'^(manifest-[ab]\.bin|seg-[0-9a-f]{8}\.bin)$')


# CRC32 matching the firmware (polynomial 0xEDB88320).
def crc32(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def log(msg: str) -> None:
    print(msg, flush=True)


def parse_kv(line: str) -> dict:
    """Parse 'key=value key=value ...' tokens from a protocol line (after the verb)."""
    result = {}
    for part in line.split():
        if "=" in part:
            k, _, v = part.partition("=")
            result[k] = v
    return result


def validate_filename(name: str) -> bool:
    return bool(_VALID_NAME.match(name))


def parse_target_arg(s: str):
    """Parse 'name:path' into (name, path). Used as argparse type."""
    if ':' not in s:
        raise argparse.ArgumentTypeError(
            f"target must be name:path, got: {s!r}")
    name, _, path = s.partition(':')
    if not name or not path:
        raise argparse.ArgumentTypeError(
            f"target name and path must be non-empty, got: {s!r}")
    return (name, path)


def load_targets_file(path: str):
    """Load targets from a file with 'name path' lines (# comments allowed)."""
    targets = []
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split(None, 1)
            if len(parts) != 2:
                raise ValueError(
                    f"{path}:{lineno}: expected 'name path', got: {line!r}")
            targets.append((parts[0], parts[1]))
    return targets


class FileTransfer:
    def __init__(self, name: str, expected_size: int):
        self.name = name
        self.expected_size = expected_size
        self.data = bytearray()

    def feed_hex_line(self, line: str) -> bool:
        try:
            chunk = bytes.fromhex(line)
        except ValueError:
            log(f"  ERROR: invalid hex in {self.name}: {line[:40]!r}")
            return False
        self.data.extend(chunk)
        return True

    def verify(self, expected_crc_hex: str) -> bool:
        if len(self.data) != self.expected_size:
            log(f"  ERROR: {self.name}: size mismatch "
                f"(expected {self.expected_size}, got {len(self.data)})")
            return False
        try:
            expected_crc = int(expected_crc_hex, 16)
        except ValueError:
            log(f"  ERROR: {self.name}: bad CRC field: {expected_crc_hex!r}")
            return False
        actual_crc = crc32(bytes(self.data))
        if actual_crc != expected_crc:
            log(f"  ERROR: {self.name}: CRC mismatch "
                f"(expected {expected_crc:08x}, got {actual_crc:08x})")
            return False
        return True


def receive_lines(lines, out_dir: str) -> bool:
    """
    Process an iterable of protocol lines. Returns True if all files
    were received and verified without errors.
    """
    os.makedirs(out_dir, exist_ok=True)

    transfer = None
    discard_until = None  # name to silently discard until its FILE_END
    ok = True

    for raw_line in lines:
        line = raw_line.rstrip("\r\n")
        if not line:
            continue

        if discard_until is not None:
            if line.startswith("FILE_END ") and parse_kv(line[len("FILE_END "):]).get("name") == discard_until:
                discard_until = None
            continue

        if line == "DUMP_BEGIN":
            if transfer is not None:
                log(f"  ERROR: DUMP_BEGIN while {transfer.name} still open")
                ok = False
                transfer = None

        elif line.startswith("DUMP_END "):
            kv = parse_kv(line[len("DUMP_END "):])
            log(f"Dump complete: files={kv.get('files','?')} errors={kv.get('errors','?')}")
            if kv.get("errors", "0") != "0":
                ok = False
            break

        elif line.startswith("FILE_BEGIN "):
            if transfer is not None:
                log(f"  ERROR: FILE_BEGIN while {transfer.name} still open")
                ok = False
                transfer = None
            kv = parse_kv(line[len("FILE_BEGIN "):])
            name = kv.get("name", "")
            if not validate_filename(name):
                log(f"  ERROR: rejected unsafe filename: {name!r}")
                ok = False
                discard_until = name
                continue
            try:
                size = int(kv.get("size", ""))
            except ValueError:
                log(f"  ERROR: {name}: missing or non-integer size field")
                ok = False
                discard_until = name
                continue
            if size < 0 or size > MAX_FILE_SIZE:
                log(f"  ERROR: {name}: absurd size {size}")
                ok = False
                discard_until = name
                continue
            log(f"Receiving {name} ({size} bytes)...")
            transfer = FileTransfer(name, size)

        elif line.startswith("FILE_END "):
            if transfer is None:
                log(f"  ERROR: FILE_END without FILE_BEGIN: {line}")
                ok = False
                continue
            kv = parse_kv(line[len("FILE_END "):])
            end_name = kv.get("name", "")
            if end_name != transfer.name:
                log(f"  ERROR: FILE_END name={end_name!r} does not match "
                    f"open transfer {transfer.name!r}")
                ok = False
                transfer = None
                continue
            if not transfer.verify(kv.get("crc", "0")):
                ok = False
                transfer = None
                continue
            dest = os.path.join(out_dir, transfer.name)
            with open(dest, "wb") as f:
                f.write(transfer.data)
            log(f"  -> {dest} ({len(transfer.data)} bytes, CRC OK)")
            transfer = None

        elif line.startswith("FILE_ERROR "):
            kv = parse_kv(line[len("FILE_ERROR "):])
            log(f"  ERROR from device: name={kv.get('name','')} "
                f"message={kv.get('message','')}")
            ok = False
            transfer = None

        elif line.startswith("LIST_ERROR "):
            kv = parse_kv(line[len("LIST_ERROR "):])
            log(f"  ERROR: device list failed: {kv.get('message','')}")
            ok = False

        elif transfer is not None:
            if not transfer.feed_hex_line(line):
                ok = False
                transfer = None
        else:
            log(f"  [{line}]")

    if transfer is not None:
        log(f"  ERROR: {transfer.name}: transfer incomplete (no FILE_END)")
        ok = False

    return ok


def recv_stdin(out_dir: str) -> bool:
    return receive_lines(sys.stdin, out_dir)


def _wait_for_ready(s, timeout_s: float = READY_TIMEOUT_S) -> bool:
    """Read lines until READY is seen. Prints other lines as info. Returns False on timeout."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        raw = s.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
        if line == "READY":
            return True
        if line:
            log(f"  [{line}]")
    return False


def _dump_lines(s):
    """Yield lines from serial until DUMP_END (or timeout)."""
    deadline = time.time() + RECV_TIMEOUT_S
    while time.time() < deadline:
        raw = s.readline()
        if not raw:
            continue
        decoded = raw.decode("utf-8", errors="replace")
        yield decoded
        if decoded.rstrip().startswith("DUMP_END "):
            return
    log("WARNING: timed out waiting for DUMP_END")


def recv_serial(port: str, baud: int, out_dir: str, targets: list) -> bool:
    import serial  # only import when needed

    log(f"Opening {port} at {baud} baud")
    s = serial.Serial(port, baud, timeout=1)

    # Toggle DTR to reset the device (EN pin via auto-reset circuit).
    s.setDTR(False)
    time.sleep(0.1)
    s.setDTR(True)
    time.sleep(0.1)
    s.reset_input_buffer()

    log("Waiting for READY...")
    if not _wait_for_ready(s):
        log("ERROR: timed out waiting for READY after boot")
        s.close()
        return False

    all_ok = True
    multi = len(targets) > 1

    for name, path in targets:
        log(f"Selecting target {name!r} ({path})...")
        s.write(f"TARGET {name} {path}\n".encode())
        if not _wait_for_ready(s, timeout_s=10):
            log(f"ERROR: timed out waiting for READY after TARGET {name}")
            all_ok = False
            break

        target_out_dir = os.path.join(out_dir, name) if multi else out_dir
        log("Sending DUMP_ALL...")
        s.write(b"DUMP_ALL\n")

        if not receive_lines(_dump_lines(s), target_out_dir):
            all_ok = False

        # Consume the READY that follows DUMP_ALL
        _wait_for_ready(s, timeout_s=10)

    log("Sending DONE...")
    s.write(b"DONE\n")
    s.close()
    return all_ok


def main() -> None:
    p = argparse.ArgumentParser(
        description="Receive pqueue doctor dump protocol and save files to disk.")
    p.add_argument("--out-dir", required=True,
                   help="Directory to write received files into")
    p.add_argument("--port",
                   help="Serial port for live device mode "
                        "(omit to read from stdin)")
    p.add_argument("--baud", type=int, default=DEFAULT_BAUD,
                   help=f"Baud rate for serial mode (default: {DEFAULT_BAUD})")
    p.add_argument("--target", dest="targets", metavar="NAME:PATH",
                   type=parse_target_arg, action="append", default=[],
                   help="Target to dump as name:path (repeatable)")
    p.add_argument("--targets", dest="targets_file", metavar="FILE",
                   help="File listing targets, one 'name path' per line")
    args = p.parse_args()

    if args.port:
        targets = list(args.targets)
        if args.targets_file:
            targets.extend(load_targets_file(args.targets_file))
        if not targets:
            p.error("serial mode requires --target or --targets")
        ok = recv_serial(args.port, args.baud, args.out_dir, targets)
    else:
        ok = recv_stdin(args.out_dir)

    if ok:
        log("All files received successfully.")
    else:
        log("Completed with errors.")
        sys.exit(1)


if __name__ == "__main__":
    main()

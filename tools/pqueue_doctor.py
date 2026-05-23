#!/usr/bin/env python3
"""
pqueue doctor - on-device maintenance and dump tool.

Serial mode:
    python3 tools/pqueue_doctor.py \\
        --port /dev/ttyACM0 \\
        --target api_outbox:/pqueue_api_spool \\
        --validate

    python3 tools/pqueue_doctor.py \\
        --port /dev/ttyACM0 \\
        --target api_outbox:/pqueue_api_spool \\
        --dump-all --out-dir /tmp/pqdump

    Multiple targets (each dumps into out-dir/name/):
    python3 tools/pqueue_doctor.py \\
        --port /dev/ttyACM0 \\
        --targets pqueue_doctor.targets \\
        --validate --dump-all --out-dir /tmp/pqdump

    Targets file format (one target per line, # comments allowed):
        api_outbox /pqueue_api_spool reservedBytes=262144

Stdin mode (pipe from pqueue-doctor-dump POSIX tool):
    ./build/pqueue-doctor-dump --base-path /path/to/spool | \\
        python3 tools/pqueue_doctor.py --out-dir /tmp/dump

Commands run in this fixed order regardless of flag order:
    info, list, diag, validate,
    compact / compact-all, drop-front-if-corrupt, recover-stale-lock, format,
    dump-file, dump-all

Protocol (dump):
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
CMD_TIMEOUT_S = 30
MAX_FILE_SIZE = 1 * 1024 * 1024

_VALID_NAME = re.compile(r'^(manifest-[ab]\.bin|seg-[0-9a-f]{8}\.bin)$')
_VALID_TARGET_NAME = re.compile(r'^[A-Za-z0-9_-]+$')
_VALID_CONFIG_KEYS = frozenset({
    'reservedBytes', 'recordSizeBytes', 'maxSegmentBytes', 'minFreeBytes', 'maxSegments',
})


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
    result = {}
    for part in line.split():
        if "=" in part:
            k, _, v = part.partition("=")
            result[k] = v
    return result


def validate_filename(name: str) -> bool:
    return bool(_VALID_NAME.match(name))


def _validate_target_name(name: str) -> str:
    if not _VALID_TARGET_NAME.match(name):
        raise argparse.ArgumentTypeError(
            f"target name must match [A-Za-z0-9_-]+, got: {name!r}")
    return name


_CONFIG_RULES = {
    # key: (min_inclusive, max_inclusive)  None means no bound on that side
    'reservedBytes':   (1,   None),
    'recordSizeBytes': (1,   None),
    'maxSegmentBytes': (1,   None),
    'minFreeBytes':    (0,   None),
    'maxSegments':     (1,   255),
}


def _validate_config_kv(k: str, v: str, error_fn):
    """Validate a config key/value pair. error_fn(msg) raises the appropriate error."""
    if k not in _VALID_CONFIG_KEYS:
        error_fn(f"unknown config key {k!r}; valid keys: "
                 f"{', '.join(sorted(_VALID_CONFIG_KEYS))}")
    try:
        n = int(v)
    except ValueError:
        error_fn(f"config value for {k!r} must be an integer, got: {v!r}")
        return  # unreachable, but keeps type checker happy
    lo, hi = _CONFIG_RULES[k]
    if lo is not None and n < lo:
        error_fn(f"config {k!r} must be >= {lo}, got {n}")
    if hi is not None and n > hi:
        error_fn(f"config {k!r} must be <= {hi}, got {n}")


def parse_queue_config(s: str) -> tuple:
    if '=' not in s:
        raise argparse.ArgumentTypeError(
            f"--queue-config must be KEY=VAL, got: {s!r}")
    k, _, v = s.partition('=')

    def _err(msg):
        raise argparse.ArgumentTypeError(msg)

    _validate_config_kv(k, v, _err)
    return (k, v)


def parse_target_arg(s: str):
    if ':' not in s:
        raise argparse.ArgumentTypeError(
            f"target must be name:path, got: {s!r}")
    name, _, path = s.partition(':')
    if not name or not path:
        raise argparse.ArgumentTypeError(
            f"target name and path must be non-empty, got: {s!r}")
    _validate_target_name(name)
    return (name, path, {})


def load_targets_file(path: str):
    targets = []
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split()
            if len(parts) < 2:
                raise ValueError(
                    f"{path}:{lineno}: expected 'name path [key=val ...]', "
                    f"got: {line!r}")
            name = parts[0]
            if not _VALID_TARGET_NAME.match(name):
                raise ValueError(
                    f"{path}:{lineno}: invalid target name {name!r} "
                    f"(must match [A-Za-z0-9_-]+)")
            config = {}
            for kv in parts[2:]:
                if '=' not in kv:
                    raise ValueError(
                        f"{path}:{lineno}: expected key=val, got: {kv!r}")
                k, _, v = kv.partition('=')

                def _err(msg, _ln=lineno, _p=path):
                    raise ValueError(f"{_p}:{_ln}: {msg}")

                _validate_config_kv(k, v, _err)
                config[k] = v
            targets.append((name, parts[1], config))
    return targets


# ---------------------------------------------------------------------------
# Dump protocol
# ---------------------------------------------------------------------------

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
    """Process dump protocol lines. Returns True if all files received OK."""
    os.makedirs(out_dir, exist_ok=True)

    transfer = None
    discard_until = None
    saw_dump_end = False
    ok = True

    for raw_line in lines:
        line = raw_line.rstrip("\r\n")
        if not line:
            continue

        if discard_until is not None:
            if (line.startswith("FILE_END ")
                    and parse_kv(line[len("FILE_END "):]).get("name") == discard_until):
                discard_until = None
            continue

        if line == "DUMP_BEGIN":
            if transfer is not None:
                log(f"  ERROR: DUMP_BEGIN while {transfer.name} still open")
                ok = False
                transfer = None

        elif line.startswith("DUMP_END "):
            saw_dump_end = True
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

    if not saw_dump_end:
        log("  ERROR: dump ended without DUMP_END (timeout or truncated stream)")
        ok = False

    return ok


# ---------------------------------------------------------------------------
# Stdin mode
# ---------------------------------------------------------------------------

def recv_stdin(out_dir: str) -> bool:
    return receive_lines(sys.stdin, out_dir)


# ---------------------------------------------------------------------------
# Serial session
# ---------------------------------------------------------------------------

def _wait_for_ready(s, timeout_s: float = READY_TIMEOUT_S) -> bool:
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


def _send_cmd(s, cmd: str, timeout_s: float = CMD_TIMEOUT_S) -> tuple:
    """Send a command, collect output lines until READY. Returns (lines, timed_out, cmd_ok)."""
    s.write((cmd + "\n").encode())
    lines = []
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        raw = s.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
        if line == "READY":
            cmd_ok = not any(
                l.lower().startswith("error") or "failed" in l.lower()
                for l in lines
            )
            return lines, False, cmd_ok
        if line:
            lines.append(line)
    return lines, True, False


def _dump_lines(s):
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


def run_serial_session(port: str, baud: int, targets: list, args) -> bool:
    import serial

    log(f"Opening {port} at {baud} baud")
    s = serial.Serial(port, baud, timeout=1)

    if args.trigger:
        # Running app is already up; send trigger and wait for it to enter doctor mode.
        time.sleep(0.2)
        s.reset_input_buffer()
        log(f"Sending trigger: {args.trigger!r}")
        s.write((args.trigger + "\n").encode())
    elif not args.no_reset:
        s.setDTR(False)
        time.sleep(0.1)
        s.setDTR(True)
        time.sleep(0.1)
        s.reset_input_buffer()

    ready_timeout = READY_TIMEOUT_S * 4 if args.trigger else READY_TIMEOUT_S
    log("Waiting for READY...")
    if not _wait_for_ready(s, timeout_s=ready_timeout):
        log("ERROR: timed out waiting for READY")
        s.close()
        return False

    all_ok = True
    multi = len(targets) > 1

    for name, path, config in targets:
        log(f"\n=== Target: {name} ({path}) ===")
        target_cmd = " ".join(
            ["TARGET", name, path] + [f"{k}={v}" for k, v in config.items()])
        lines, timed_out, _ = _send_cmd(s, target_cmd)
        for l in lines:
            log(f"  {l}")
        if timed_out:
            log(f"ERROR: timed out after TARGET {name}")
            all_ok = False
            break

        # Read-only commands
        for flag, cmd, label in [
            (args.info,     "INFO",     "Info"),
            (args.list,     "LIST",     "List"),
            (args.diag,     "DIAG",     "Diagnostics"),
            (args.validate, "VALIDATE", "Validate"),
        ]:
            if not flag:
                continue
            log(f"--- {label} ---")
            lines, timed_out, cmd_ok = _send_cmd(s, cmd)
            for l in lines:
                log(f"  {l}")
            if timed_out:
                log(f"ERROR: timed out waiting for response to {cmd}")
                all_ok = False
            elif not cmd_ok:
                all_ok = False

        # Mutation commands
        if args.compact is not None:
            log(f"--- Compact ({args.compact} steps) ---")
            lines, timed_out, cmd_ok = _send_cmd(s, f"COMPACT {args.compact}", timeout_s=120)
            for l in lines:
                log(f"  {l}")
            if timed_out:
                log("ERROR: timed out waiting for COMPACT response")
                all_ok = False
            elif not cmd_ok:
                all_ok = False

        if args.compact_all is not None:
            log(f"--- Compact all (max {args.compact_all} steps) ---")
            lines, timed_out, cmd_ok = _send_cmd(s, f"COMPACT_ALL {args.compact_all}", timeout_s=300)
            for l in lines:
                log(f"  {l}")
            if timed_out:
                log("ERROR: timed out waiting for COMPACT_ALL response")
                all_ok = False
            elif not cmd_ok:
                all_ok = False

        if args.drop_front_if_corrupt:
            log("--- Drop front if corrupt ---")
            lines, timed_out, cmd_ok = _send_cmd(s, "DROP_FRONT_IF_CORRUPT")
            for l in lines:
                log(f"  {l}")
            if timed_out:
                log("ERROR: timed out waiting for DROP_FRONT_IF_CORRUPT response")
                all_ok = False
            elif not cmd_ok:
                all_ok = False

        if args.recover_stale_lock:
            log("--- Recover stale lock ---")
            lines, timed_out, cmd_ok = _send_cmd(s, "RECOVER_STALE_LOCK")
            for l in lines:
                log(f"  {l}")
            if timed_out:
                log("ERROR: timed out waiting for RECOVER_STALE_LOCK response")
                all_ok = False
            elif not cmd_ok:
                all_ok = False

        if args.format:
            log("--- Format ---")
            lines, timed_out, cmd_ok = _send_cmd(s, f"FORMAT CONFIRM {name}", timeout_s=60)
            for l in lines:
                log(f"  {l}")
            if timed_out:
                log("ERROR: timed out waiting for FORMAT response")
                all_ok = False
            elif not cmd_ok:
                all_ok = False

        # Dump commands last, after any mutations
        if args.dump_file:
            log(f"--- Dump file: {args.dump_file} ---")
            s.write(f"DUMP_FILE {args.dump_file}\n".encode())
            dump_out = os.path.join(args.out_dir, name) if multi else args.out_dir
            if not receive_lines(_dump_lines(s), dump_out):
                all_ok = False
            if not _wait_for_ready(s, timeout_s=10):
                log("ERROR: timed out waiting for READY after DUMP_FILE")
                all_ok = False

        if args.dump_all:
            log("--- Dump all ---")
            s.write(b"DUMP_ALL\n")
            dump_out = os.path.join(args.out_dir, name) if multi else args.out_dir
            if not receive_lines(_dump_lines(s), dump_out):
                all_ok = False
            if not _wait_for_ready(s, timeout_s=10):
                log("ERROR: timed out waiting for READY after DUMP_ALL")
                all_ok = False

    log("\nSending DONE...")
    s.write(b"DONE\n")
    s.close()
    return all_ok


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    p = argparse.ArgumentParser(
        description="pqueue doctor - on-device maintenance and dump tool.")

    # Connection
    p.add_argument("--port",
                   help="Serial port (omit to read dump protocol from stdin)")
    p.add_argument("--baud", type=int, default=DEFAULT_BAUD,
                   help=f"Baud rate (default: {DEFAULT_BAUD})")
    p.add_argument("--no-reset", action="store_true",
                   help="Do not toggle DTR to reset the device on connect")
    p.add_argument("--trigger", metavar="TEXT",
                   help="Send TEXT to wake a running app into doctor mode (implies --no-reset)")

    # Targets
    p.add_argument("--target", dest="targets", metavar="NAME:PATH",
                   type=parse_target_arg, action="append", default=[],
                   help="Target as name:path (repeatable)")
    p.add_argument("--targets", dest="targets_file", metavar="FILE",
                   help="File listing targets, one 'name path [key=val ...]' per line")
    p.add_argument("--queue-config", metavar="KEY=VAL",
                   type=parse_queue_config, action="append", default=[],
                   help=(f"Queue config override applied to --target targets only "
                         f"(not --targets file entries, which carry config inline). "
                         f"Repeatable. Valid keys: "
                         f"{', '.join(sorted(_VALID_CONFIG_KEYS))}"))

    # Read-only commands
    p.add_argument("--info",     action="store_true", help="Print filesystem stats and file list")
    p.add_argument("--list",     action="store_true", help="List segment files with sizes")
    p.add_argument("--diag",     action="store_true", help="Print AppendLog diagnostics")
    p.add_argument("--validate", action="store_true", help="Validate queue, print issues and repair hints")

    # Mutation commands
    p.add_argument("--compact", metavar="STEPS", type=int,
                   help="Run compactIdle(STEPS)")
    p.add_argument("--compact-all", metavar="MAX_STEPS", type=int,
                   help="Run compactIdle to completion, up to MAX_STEPS")
    p.add_argument("--drop-front-if-corrupt", action="store_true",
                   help="Drop front record only if corruption is proven")
    p.add_argument("--recover-stale-lock", action="store_true",
                   help="Remove a stale lock left by a dead process")
    p.add_argument("--format", action="store_true",
                   help="Destructively reinitialize the queue (sends FORMAT CONFIRM)")

    # Dump commands
    p.add_argument("--dump-file", metavar="NAME",
                   help="Transfer one named file off-device")
    p.add_argument("--dump-all", action="store_true",
                   help="Transfer all manifest and segment files")
    p.add_argument("--out-dir",
                   help="Directory to write received files into (required for dump)")

    args = p.parse_args()

    needs_out_dir = args.dump_all or args.dump_file
    if needs_out_dir and not args.out_dir:
        p.error("--out-dir is required when using --dump-all or --dump-file")

    if args.compact is not None and args.compact <= 0:
        p.error("--compact must be > 0")
    if args.compact_all is not None and args.compact_all <= 0:
        p.error("--compact-all must be > 0")
    if args.dump_file and not validate_filename(args.dump_file):
        p.error(f"--dump-file name must be manifest-[ab].bin or seg-[0-9a-f]{{8}}.bin, "
                f"got: {args.dump_file!r}")

    _SERIAL_ONLY = ['info', 'list', 'diag', 'validate', 'compact', 'compact_all',
                    'drop_front_if_corrupt', 'recover_stale_lock', 'format',
                    'dump_file', 'dump_all']

    if args.port:
        cli_config = dict(args.queue_config)
        targets = [(name, path, {**config, **cli_config})
                   for name, path, config in args.targets]
        if args.targets_file:
            targets.extend(load_targets_file(args.targets_file))
        if not targets:
            p.error("serial mode requires --target or --targets")

        any_command = any([
            args.info, args.list, args.diag, args.validate,
            args.compact is not None, args.compact_all is not None,
            args.drop_front_if_corrupt, args.recover_stale_lock,
            args.format, args.dump_file, args.dump_all,
        ])
        if not any_command:
            p.error("no commands specified; add --validate, --dump-all, etc.")

        ok = run_serial_session(args.port, args.baud, targets, args)
    else:
        # Stdin mode: only dump protocol is supported
        serial_flags_set = [f for f in _SERIAL_ONLY if getattr(args, f, None)]
        if serial_flags_set:
            p.error(f"stdin mode only supports dump protocol; "
                    f"these flags require --port: {', '.join('--' + f.replace('_', '-') for f in serial_flags_set)}")
        if not args.out_dir:
            p.error("--out-dir is required in stdin mode")
        ok = recv_stdin(args.out_dir)

    if ok:
        log("Done.")
    else:
        log("Completed with errors.")
        sys.exit(1)


if __name__ == "__main__":
    main()

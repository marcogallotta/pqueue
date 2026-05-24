# pqueue Doctor

`pqueue_doctor.py` is an on-device maintenance tool. It runs commands on a live
queue over Serial: read diagnostics, validate integrity, compact dead space, and
recover from corruption. All mutations run through `pqueue::Queue` on-device;
the host sends commands and reads results.

---

## Connecting

### Standalone maintenance firmware

Flash a dedicated maintenance image that calls `pqueue::doctor::runSession()`
directly. No production firmware changes needed.

```cpp
// tools/esp32_pqueue_doctor/main.cpp pattern
#include "pqueue/doctor/session.h"

void setup() {
    Serial.begin(115200);
    LittleFS.begin(false);
}

void loop() {
    pqueue::doctor::runSession(); // blocks until DONE
    ESP.restart();
}
```

Build and flash (from the repo root):

```
pio run -d tools/esp32_pqueue_doctor --target upload
```

Then connect without `--trigger`:

```
python3 tools/pqueue_doctor.py \
    --port /dev/ttyACM0 \
    --target api_outbox:/pqueue_api_spool \
    --queue-config reservedBytes=262144 \
    --validate
```

### Production firmware (trigger mode, no reflash needed)

Add a trigger check to your main loop or sleep handler:

```cpp
#include "pqueue/doctor/session.h"

// Inside your loop() or sleep poll:
if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line == "PQUEUE_DOCTOR") {
        pqueue::doctor::runSession(); // blocks until DONE
        ESP.restart();
    }
}
```

Then send the trigger string with `--trigger`:

```
python3 tools/pqueue_doctor.py \
    --port /dev/ttyACM0 \
    --trigger PQUEUE_DOCTOR \
    --target api_outbox:/pqueue_api_spool \
    --queue-config reservedBytes=262144 \
    --validate
```

Doctor mode is blocking: normal app sync, drain, and log output is paused until
`DONE`, then the device reboots.

---

## Targets

A target is a name and a LittleFS base path, plus optional queue config overrides.
Config overrides must match what the app passes to `pqueue::Config` -- if they
differ, validate/compact/format will behave differently from the app.

### Command line

```
--target NAME:PATH
--queue-config KEY=VAL   # applies to all --target targets; repeatable
```

Example:

```
--target api_outbox:/pqueue_api_spool --queue-config reservedBytes=262144
```

### Targets file

One target per line. Pass with `--targets FILE`. Config overrides are inline:

```
# name          path                    config
api_outbox      /pqueue_api_spool       reservedBytes=262144
```

Multiple targets are processed in order; each gets its own command run.

### Config keys

| Key              | Type     | Constraint | Default  |
|------------------|----------|------------|----------|
| reservedBytes    | uint32   | > 0        | 131072   |
| recordSizeBytes  | uint32   | > 0        | 492      |
| maxSegmentBytes  | uint32   | > 0        | 4096     |
| minFreeBytes     | uint32   | >= 0       | 32768    |
| maxSegments      | uint8    | 1-255      | 16       |

Defaults shown are pqueue library defaults, not necessarily the app's production
values. Always check the app config and pass overrides where they differ.

---

## Commands

Most commands emit a `RESULT command=X ok=0|1 [fields]` line before `READY`.
Python uses this line to determine success; human-readable lines are for display
only. Exceptions: dump commands use the `DUMP_BEGIN`/`DUMP_END` protocol
instead, and argument/validation errors emit `error: ...` with no `RESULT`.

### Read-only

**`--info`** -- filesystem free space and file list for the target path.

**`--list`** -- segment files with sizes, manifest slot existence and validity.

**`--diag`** -- AppendLog internals: mount status, manifest epochs, segment
ranges, dangling segment count. Returns `ok=0` if mount or list failed.

**`--validate`** -- runs `Queue::validate()`. Prints each issue with a
`repair_hint`. Result includes `issues=N`.

```
RESULT command=VALIDATE ok=0 issues=1
issue code=record_crc_mismatch message=... repair_hint=drop_front_if_corrupt
```

### Mutation

**`--compact N`** -- runs `compactIdle(N)`. Use for routine dead-space reclaim.
Result includes `steps`, `compactions`, `more_work`.

**`--compact-all N`** -- runs `compactIdle` in a loop until no more work or N
total steps are exhausted.

**`--drop-front-if-corrupt`** -- drops the front record only if it is proven
unreadable or corrupt (CRC mismatch or invalid record structure).
`changed=0 code=front_not_corrupt` means the queue is healthy; not an error.

**`--recover-stale-lock`** -- removes a stale `.pqueue.lock` left by a crashed
POSIX process. On ESP32/LittleFS the lock is an in-process FreeRTOS mutex with
no persistent state; this command always returns `changed=0 code=not_applicable`
there. `changed=0 code=lock_not_stale` means the POSIX lock file is held by a
live process; not an error.

**`--format`** -- destructively reinitializes the queue. Sends
`FORMAT CONFIRM <name>` to the device, which checks the name matches the active
target. Prints a dump recommendation before executing.

### Dump

**`--dump-file NAME`** -- transfers one file off-device (hex-encoded, CRC
verified). NAME must be `manifest-[ab].bin` or `seg-[0-9a-f]{8}.bin`.

**`--dump-all`** -- transfers all manifest and segment files in the target.
Requires `--out-dir`. Files land in `out-dir/` (single target) or
`out-dir/name/` (multiple targets).

Dump is a forensics path: take a snapshot before a destructive repair. The files
are raw binary; there is no restore path.

**Dump files are not backups. Do not upload them back to the device. Use them
for debugging only.**

---

## Common workflows

Examples below use trigger mode. Omit `--trigger` when using standalone firmware.

### Routine health check

```
python3 tools/pqueue_doctor.py \
    --port /dev/ttyACM0 \
    --trigger PQUEUE_DOCTOR \
    --targets pqueue_doctor.targets \
    --validate --diag
```

### Compact before a low-space incident

```
python3 tools/pqueue_doctor.py \
    --port /dev/ttyACM0 \
    --trigger PQUEUE_DOCTOR \
    --targets pqueue_doctor.targets \
    --compact-all 64
```

### Recover from a corrupt front record

```
# 1. Validate to confirm the issue and repair hint
python3 tools/pqueue_doctor.py ... --validate

# 2. Drop the corrupt record
python3 tools/pqueue_doctor.py ... --drop-front-if-corrupt

# 3. Validate again to confirm clean
python3 tools/pqueue_doctor.py ... --validate
```

### Format a queue that cannot be repaired

```
# 1. Dump first as forensic evidence
python3 tools/pqueue_doctor.py ... --dump-all --out-dir /tmp/pqdump

# 2. Format
python3 tools/pqueue_doctor.py ... --format
```

---

## Serial protocol

Commands are sent as plain text lines. The device responds with human-readable
output followed by a machine-readable `RESULT` line, then `READY`. Example
exchange:

```
host -> TARGET api_outbox /pqueue_api_spool reservedBytes=262144
dev  -> target: api_outbox path=/pqueue_api_spool reservedBytes=262144
dev  -> READY
host -> VALIDATE
dev  -> validate: OK
dev  -> records_checked=16
dev  -> RESULT command=VALIDATE ok=1 issues=0
dev  -> READY
host -> DONE
```

Dump commands use a different protocol -- no `RESULT` line. `DUMP_FILE` emits a
single file block; `DUMP_ALL` wraps multiple file blocks in `DUMP_BEGIN` /
`DUMP_END`:

```
host -> DUMP_ALL
dev  -> DUMP_BEGIN
dev  -> FILE_BEGIN name=manifest-a.bin size=64
dev  -> 0102030405060708...  (hex, 256 chars per line)
dev  -> FILE_END name=manifest-a.bin crc=a1b2c3d4
dev  -> FILE_BEGIN name=seg-00000000.bin size=4096
dev  -> ...
dev  -> FILE_END name=seg-00000000.bin crc=deadbeef
dev  -> DUMP_END files=2 errors=0
dev  -> READY
```

If a file cannot be read, `FILE_ERROR name=<name> message=<code>` appears in
place of its `FILE_BEGIN`/`FILE_END` block, and `errors` in `DUMP_END` is
non-zero.

The device reboots after `DONE`.

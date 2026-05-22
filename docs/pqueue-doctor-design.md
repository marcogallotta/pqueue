# pqueue Doctor: Maintenance Interface Design

**Editing rules:** ASCII only -- no Unicode symbols (no checkmarks, arrows, emoji). This file is compiled to PDF via LaTeX and non-ASCII characters cause build warnings or missing glyphs.

---

## Decision

Do not port `esp32_spool_transfer` into an append-log directory upload/restore tool.

The fixed-slot transfer tool was coherent because the entire queue state lived in one file (`pqueue.spool`). AppendLog state spans multiple files whose authority is the manifest. Copying a directory snapshot to POSIX, mutating it, and uploading it back requires a multi-file transaction protocol with temp names, per-file CRCs, upload verification, deletion ordering, manifest-last publication, and interrupted-upload recovery. That is complex and fragile. Many repairs are either handled automatically by mount or are format-class anyway.

The right model: **mutations run on-device through `pqueue::Queue`; off-device access is read-only dump for forensics.**

This mirrors the pattern of mature embedded management stacks (Zephyr MCUmgr): the host asks the device to perform controlled operations over a transport. The device remains the filesystem authority.

---

## Architecture

### Near-term: pqueue_doctor maintenance firmware

A single firmware image (`esp32s3-pqueue-maintenance`) that you flash once for an incident. On boot it runs automatic read-only diagnosis without waiting for input, then accepts a serial command loop.

The firmware compiles in the app's pqueue targets by name. The operator selects a target before issuing commands:

```
TARGET api_outbox
TARGET default_queue
```

Boot output:

```
PQUEUE_DOCTOR_START
fs_total=... fs_used=... fs_free=...
targets: api_outbox default_queue
files:
  manifest-a.bin size=...
  manifest-b.bin size=...
  seg-00000001.bin size=...
  ...
validate: OK
READY
```

or on failure:

```
validate: FAILED
issue code=metadata_corrupt message=... repair_hint=format_queue
READY
```

Automatic output is read-only. No auto-format, no auto-drop.

### Long-term: production maintenance console

Move the same command set into the production firmware, entered only by an explicit trigger: serial command during first N seconds after boot, GPIO strap, or debug build flag. No reflash needed for routine investigation.

---

## Command set

```
INFO                      -- print fs stats and file list
LIST                      -- list pqueue segment files with sizes
VALIDATE                  -- run Queue::validate(), print issues and repair hints
DIAG                      -- run AppendLog diagnostics (manifest contents, segment summary)
DUMP_FILE <name>          -- transfer one file off-device (read-only)
DUMP_ALL                  -- transfer manifests and all seg-*.bin files (read-only)
COMPACT <steps>           -- run compactIdle(steps)
COMPACT_ALL <max_steps>   -- run compactIdle to completion, up to max_steps
DROP_FRONT_IF_CORRUPT     -- run Queue::dropFrontIfCorrupt()
RECOVER_STALE_LOCK        -- run Queue::recoverStaleLock()
FORMAT CONFIRM            -- destructively reinitialize the queue
DONE                      -- exit maintenance mode / reboot
```

All mutations use `pqueue::Queue` on-device. `FORMAT` requires the literal argument `CONFIRM` to prevent accidental execution.

---

## Off-device dump protocol

`DUMP_FILE <name>` transfers one file. `DUMP_ALL` transfers all manifest and segment files in the active target. Both are read-only. Format per file:

```
FILE_BEGIN name=<name> size=<bytes> crc=<crc32hex>
<hex-encoded chunks>
FILE_END name=<name>
```

Use `pqueue_appendlog_diag` on the host to analyze dumped files. This is the forensics path: copy evidence out, inspect it, decide what command to run on-device.

---

## What is not built yet

Upload/restore of append-log directories is deferred. If a genuine field case arises where record-preserving salvage is worth the risk, design it as an explicit dangerous mode with session IDs, per-file verification, manifest-last publication, and a commit/rollback gate. Until then it is wasted complexity.

---

## Staging

1. Delete fixed-slot upload/restore semantics with the fixed-slot backend.
2. No append-log upload/restore path is built.
3. Build `pqueue_doctor` as the replacement read-only dump and on-device maintenance tool.
4. Delete `esp32_spool_transfer` once `pqueue_doctor` covers dump and diagnosis. If the old tool cannot compile after fixed-slot removal, delete it then and make the loss explicit.
5. Move the command set into production firmware once the interface is proven.

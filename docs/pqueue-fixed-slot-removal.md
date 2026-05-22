# pqueue Fixed-Slot Backend: Reference for Removal

**Editing rules:** ASCII only -- no Unicode symbols (no checkmarks, arrows, emoji). This file is compiled to PDF via LaTeX and non-ASCII characters cause build warnings or missing glyphs.

This document describes the fixed-slot (`FileStore`) backend so it can be safely deleted.
It covers the on-disk format, the shared/non-shared type boundary in `file_store.h`, the
migration stance, and the `Config` fields that disappear with it.

---

## On-disk format

The fixed-slot backend writes a single file: `pqueue.spool`. The file is pre-allocated
to a deterministic size on first mount and never resized. Layout from offset 0:

```
[0, checkpointBytes)             checkpoint region
[checkpointBytes, checkpointBytes+journalBytes)   journal region
[checkpointBytes+journalBytes, spoolBytes)        record region
```

Where:

```
slotSizeBytes      = kRecordHeaderBytes(20) + recordSizeBytes
capacityRecords    = reservedBytes / slotSizeBytes
checkpointBytes    = kCheckpointSlots(4) * kCheckpointRecordBytes(64)  = 256
spoolBytes         = 256 + journalBytes + capacityRecords * slotSizeBytes
```

All of these are derived from `FileStoreConfig` (equivalently from `Config`). The spool
size is always exactly `spoolBytes`; a size mismatch on mount returns `ConfigMismatch`.

### Checkpoint region (256 bytes)

Four 64-byte slots at offsets 0, 64, 128, 192. Magic `0x5051434B` ("PQCK").
On each write, the slot index is `generation % 4`. On mount, the slot with the
highest `generation` wins. Each slot carries:

- `capacityRecords`, `recordSizeBytes`, `reservedBytes`, `journalBytes` -- checked
  against the current config; mismatch returns `ConfigMismatch` without loading.
- `head`, `tail`, `count` -- the ring-buffer state at checkpoint time.
- `generation` -- monotonically increasing; used to choose the winning slot.
- `journalUsedBytes` -- stored but not used on load (journal is rescanned from 0).
- `crc` -- CRC32 over all preceding fields.

A spool file full of zeros (brand new or after `format()`) passes the all-zero check
and gets a fresh checkpoint written at generation 1.

### Journal region (default 4096 bytes)

Variable-length sequence of 32-byte entries. Magic `0x50514A4E` ("PQJN"). Ops:
`Enqueue(1)`, `Pop(2)`, `RewriteFront(3)`. Each entry carries a `sequence` (the
affected slot), a `generation` (must be contiguous from `checkpoint.generation + 1`),
and a CRC.

On load, entries are replayed in order on top of the winning checkpoint index.
Replay stops at the first invalid entry, CRC failure, or non-contiguous generation.

The journal is flushed (a new checkpoint is written and the journal region is
implicitly reset by advancing `journalUsedBytes = 0`) when:
- the next entry would exceed `journalBytes`, or
- `journalOps + 1 >= checkpointEveryOps` (default: 64 ops), or
- used bytes would exceed 75% of `journalBytes`.

An unknown index transition (anything other than enqueue +1 or pop -1) also
forces an immediate checkpoint.

### Record region

`capacityRecords` slots of `slotSizeBytes` bytes each. Slot for sequence `s` is
at `recordRegionOffset + (s % capacityRecords) * slotSizeBytes`. Each slot:

```
magic       4 bytes   0x50515243 ("PQRC")
version     2 bytes   kFormatVersion = 0
headerBytes 2 bytes   20 (kRecordHeaderBytes)
sequence    4 bytes   u32; validated on read
recordBytes 4 bytes   u32; actual payload length <= recordSizeBytes
crc         4 bytes   CRC32 over header + payload
payload     recordSizeBytes bytes (padded with zeros to slot boundary)
```

`removeRecord` zeros the entire slot. `rebuildMetadata` scans all slots, collects
valid sequence numbers, checks for gaps, and writes a fresh checkpoint.

---

## Shared type boundary in `file_store.h`

`file_store.h` is the home of both the fixed-slot-specific types and the shared
`Store` interface. Deleting `FileStore` requires splitting the file.

### Types that must survive (used by Queue, AppendLogStore, types.h, or the public API)

- `StorageBackend` enum -- used by both `FileStoreConfig`, `AppendLogConfig`, and `Config`.
- `StoreLayout` enum -- used by `makeStore()` in `queue.cpp` to dispatch backends; disappears once only one backend exists.
- `kDefaultBasePath` -- used in `types.h` `Config`.
- `FileStoreIndex` -- `{ head, tail, count }`; Queue's internal currency. `AppendLogStore` synthesises it from `records_` in `indexFromRecords()` and returns it from `readIndex`/`readIndexFromDisk`. `Queue` caches it as `index_`.
- `Store` abstract class -- the full virtual interface both stores implement.
- `CompactIdleResult` -- returned by both stores' `compactIdle`; also returned by `Queue::compactIdle`.
- `ValidationResult`, `ValidationOptions`, `ValidationIssue`, `ValidationIssueCode`, `ValidationRepairAction` -- used by `Queue::validate()` and both stores' `validateUnlocked`.

### Types that disappear entirely with FileStore

- `FileStoreConfig` -- only used by `FileStore` and the FixedSlot branch of `makeStore()`.
- `FileStoreRuntimeState` -- internal to `FileStore`; also referenced in `diagnostics.cpp`.
- `FileStore` class.

### ValidationIssueCode values used by AppendLogStore::validateUnlocked

These must survive:
`InvalidConfig`, `MetadataCorrupt`, `JournalCorrupt`, `SlotCrcMismatch`.

These are FixedSlot-only and disappear:
`SpoolMissing`, `SpoolSizeMismatch`, `InvalidRingState`, `SlotHeaderInvalid`,
`SlotReadFailed`, `MetadataMissing`.

---

## Files deleted entirely

- `src/pqueue/file_store.h` -- after extracting shared types to a new header.
- `src/pqueue/file_store.cpp`
- `src/pqueue/storage_common.h` -- all FixedSlot: `PQCK`/`PQRC`/`PQJN` magic,
  `CheckpointRecord`, `JournalEntry`, `RecordHeader`, and their serializers.
  `AppendLogStore` does not include this file.
- `src/pqueue/storage_common.cpp`
- `src/pqueue/diagnostics.h` -- FixedSlot-specific types (`CheckpointSlotDiagnostic`,
  `FileStoreLayoutDiagnostic`, `FileStoreDiagnostic`, `diagnoseFileStore`,
  `checkpointSlotStateName`) are removed. AppendLog diagnostic types
  (`AppendLogStoreDiagnostic`, `diagnoseAppendLogStore`, etc.) survive.
- `src/pqueue/diagnostics.cpp` -- FixedSlot-specific implementation removed;
  AppendLog diagnostic implementation survives.

---

## Config fields that disappear

In `src/pqueue/types.h` `Config`:

- `StoreLayout storeLayout` -- field and enum go away; only AppendLog exists.
- `uint32_t journalBytes` -- FixedSlot-only; ignored by AppendLog.
- `uint32_t checkpointEveryOps` -- FixedSlot-only; ignored by AppendLog.

In `makeStore()` (`queue.cpp`): the entire FixedSlot branch and `FileStoreConfig`
construction are removed. `makeStore` becomes a straight `AppendLogStore` construction.

---

## On-disk compatibility

The two backends never share a filename. FixedSlot owns `pqueue.spool`; AppendLog
owns `mf-a.bin`, `mf-b.bin`, and `seg-*.bin`. A device previously running FixedSlot
will have a dangling `pqueue.spool` that AppendLog ignores.

---

## Remaining removal steps

1. **Migrate the app.** Migration plan: `docs/outbox-appendlog-migration.md`.

2. **Clean tools and docs.** Remove or update simulator flags, profiling modes,
   and documentation sections that reference FixedSlot.

3. **Eliminate FixedSlot.** Delete the files listed above, strip the dead
   `Config` fields, collapse `makeStore()`, and remove `StoreLayout`.

One prerequisite still blocked: remove FixedSlot-only `ValidationIssueCode` values
(`SpoolMissing`, `SpoolSizeMismatch`, `InvalidRingState`, `SlotHeaderInvalid`,
`SlotReadFailed`, `MetadataMissing`) once dead test references are cleared.
See `docs/appendlog-validation-repair.md`.

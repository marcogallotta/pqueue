# pqueue Append-Log Implementation Notes

**Editing rules:** ASCII only — no Unicode symbols (no checkmarks, arrows, emoji). This file is compiled to PDF via LaTeX and non-ASCII characters cause build warnings or missing glyphs.

**Design target:** `pqueue-append-log-design.pdf` — dual-manifest, segment-file backend.

**Current code state:** Stages 0–7 complete; initial v1 pass done. Manifest-backed mount, slot election, rollover, compaction, and cleanup are live. Every segment rotation publishes a manifest; `RangeLimitExceeded` signals when compaction is required before the next rollover. `compactOneSegment()` and `compactFull()` are wired; `writeRecord` calls `compactOneSegment()` when `needsCompaction()` is true. `needsCompaction()` uses `activeGenerations_.size() > config_.maxSegments` as the segment-count trigger. After every successful `publishManifest()` and at the end of `scanSegments()`, `cleanupOneDanglingSegment()` deletes one unreferenced segment file (lazy, one file per pass, idempotent). `writeRecord` rejects sequence `UINT32_MAX` with `SequenceExhausted` to prevent wrap-around.

---

## Scan and torn-tail rules (exact, from current `scanSegments()`)

These rules must be preserved exactly in `AppendLogStore::scanSegments()`.

Each segment is scanned with two cursors: `offset` (current read position, starts at `kSegmentHeaderBytes`) and `lastGoodOffset` (end of the last fully validated event, also starts at `kSegmentHeaderBytes`). A `corrupt` flag is set when a complete but invalid record is found.

**Events that advance `lastGoodOffset`** (valid event fully consumed):

- ENQUEUE or REWRITE: `parseEnqueueHeader()` succeeds, `payloadBytes <= maxRecordBytes`, full payload + CRC + footer read, CRC matches `enqueueEventCrc()`, footer == `kFooterMagic`
- POP: full 20-byte event read, `parsePopEvent()` succeeds

**Events that set `corrupt = true` and break**:

- `parseEnqueueHeader()` returns false (bad magic, version, or header size)
- `payloadBytes > maxRecordBytes`
- CRC mismatch or footer != `kFooterMagic` on ENQUEUE/REWRITE
- `parsePopEvent()` returns false
- Unknown magic (not ENQUEUE, REWRITE, or POP)

**Events that break without setting `corrupt`** (treated as torn tail):

- Fewer than 4 bytes remaining (can't read magic)
- Read I/O failure at any point
- Header read returned fewer than `kEnqueueHeaderBytes` bytes
- `totalEventBytes > remaining` (payload + trailer would exceed file)
- Payload+trailer read returned a short buffer
- Fewer than `kPopEventBytes` remaining for a POP

**Post-loop policy:**

- If `corrupt && isLastSegment`: clear `corrupt` (treat as torn tail)
- If `corrupt`: return `DataCorrupt`
- If `offset < fileSize` (scan stopped before EOF):
  - If `!isLastSegment`: return `DataCorrupt`
  - If `isLastSegment`: call `f->resizeFile(name, lastGoodOffset)` (truncation failure is non-fatal — next remount re-discards the same tail); set `activeSegmentBytes_ = lastGoodOffset`
- If `offset == fileSize` (clean EOF): set `activeSegmentBytes_ = offset`

**`isLastSegment`** is determined by position in the manifest-derived generation list, not numeric generation value. After compaction, the tail may not be the numerically largest generation.

---

## What needs to be built

### Manifest binary format (`append_log_common.*`)

Add `ManifestData` struct and serialiser/parser. Binary layout (all fields little-endian, packed, CRC32 IEEE polynomial 0xEDB88320):

```
magic          4 bytes   0x464D5150 ("PQMF")
version        2 bytes   current = 1
headerBytes    2 bytes   total manifest size in bytes
epoch          4 bytes   u32, incremented on each publish
nextGeneration 4 bytes   u32, next generation to allocate
rangeCount     2 bytes   u16, number of full segment ranges
ranges         N × 8 bytes   startGen u32 + endGen u32 per range
tailGeneration 4 bytes   u32
crc            4 bytes   CRC32 over all preceding bytes
footer         4 bytes   0x214B4F50 ("POK!")
```

Fixed overhead: 30 bytes. With 4 ranges: 30 + 32 = 62 bytes. Hard limit: 64 bytes total (LittleFS inline file threshold). Maximum 4 ranges.

**Initial values on first publish:** magic = `PQMF`, version = 1, epoch = 1, nextGeneration = 1, rangeCount = 0, tailGeneration = 0 (no tail yet), footer = `POK!`.

**Epoch:** starts at 1. Epoch 0 means "never published" — a slot with epoch 0 that is otherwise valid is treated as lower priority than any slot with epoch >= 1. Overflow (u32 wrapping from 0xFFFFFFFF to 0) is a non-issue at realistic ESP32 write rates and requires no handling.

**tailGeneration = 0** means no tail segment exists (empty store). Generation 0 is never allocated. `parseManifest()` must reject a manifest where `tailGeneration == 0` and `rangeCount != 0` — a store with full ranges but no tail is malformed.

**rangeCount limit:** maximum 4. If a publish would require more than 4 ranges after merging, `publishManifest()` returns an error. The caller must run a compaction pass to reduce range count before retrying. This is never a silent skip.

**CRC:** covers all bytes from `magic` through the byte immediately before `crc`. Computed with IEEE polynomial 0xEDB88320, initial value 0.

`parseManifest()` returns false (invalid slot) on: wrong magic, version != 1, wrong headerBytes, CRC mismatch, wrong footer, rangeCount > 4. Invalid slot is not DataCorrupt — it is treated the same as a missing slot for election purposes.

### Manifest publish (`AppendLogStore::publishManifest()`)

Writes the manifest to the inactive slot (A or B). Slot selection and write procedure:

1. Identify the inactive slot: the one that is currently missing or has the lower epoch among valid slots. If both are missing, write slot A first. If epochs are equal, write slot B (tiebreaker).
2. Serialise the manifest with the new epoch (current winning epoch + 1).
3. Write to the inactive slot's file. A crash here leaves the active slot intact; on next mount the partially written slot fails CRC and is discarded.
4. Update `activeGenerations_` and `nextGeneration_` in RAM only after the write returns success.

### Manifest slot selection on mount

All four cases:

| Slot A | Slot B | Result |
|---|---|---|
| valid | valid | use higher epoch; equal epoch → use slot A |
| valid | missing or invalid | use slot A |
| missing or invalid | valid | use slot B |
| missing or invalid | missing or invalid | see bootstrap below |

A slot is "invalid" if its file exists but `parseManifest()` returns false. A slot is "missing" if its file does not exist. Both cases are treated identically for slot selection.

### Bootstrap: fresh store with no manifests

If both slots are missing and no segment files exist: treat as an empty store. Set `activeGenerations_` to empty, `nextGeneration_` to 1, `nextSequence_` to 0, mount succeeds. This matches the current behaviour for a store with no segment files and preserves the ability to mount without calling `format()` first.

If both slots are missing but segment files exist on disk: return `DataCorrupt`. A store cannot have segments without a manifest.

### Range merging and max-4 enforcement

The manifest stores full ranges compactly. Before any publish, merge adjacent contiguous ranges. Two ranges `[a, b]` and `[c, d]` are contiguous if `b + 1 == c`; they merge to `[a, d]`. Merge is applied in logical order after any range insertion.

If after merging the range count would exceed 4, `publishManifest()` returns failure. The caller must compact before publishing. Rollover that would overflow range capacity is a hard failure, not a silent skip.

### Rollover: manifest publish

After `createSegment()` writes the new segment header and before returning from `rotateSegment()`:

1. Promote the current tail generation to a full range `[tailGen, tailGen]`.
2. Try to merge with the last existing full range (if contiguous).
3. Set `tailGeneration = newGen` (the just-created segment).
4. Set `nextGeneration = newGen + 1`.
5. Check that rangeCount <= 4 after merge; if not, fail before writing.
6. Call `publishManifest()`. If publish fails, the old tail segment remains the tail (no manifest update = no layout change visible to mount).

### Compaction v1: single output segment only

`compactOneSegment()` for v1:

1. If `activeGenerations_.size() < 2`: return no-op (nothing to compact besides the tail).
2. Select the oldest full range: the first range in the manifest that does not include the tail generation.
3. Collect live records from `records_` whose `segmentGeneration` falls within `[oldStart, oldEnd]`.
4. If no live records: the range is fully dead. Publish a manifest that drops the range entirely (oldStart..oldEnd removed, no replacement). Update `activeGenerations_`. Leave old files on disk. Return.
5. Compute the total live bytes: sum of `payloadBytes + kEnqueueOverheadBytes` for each live record. If this exceeds `maxSegmentBytes`: return no-op. Do not attempt multi-segment output in v1.
6. **Build replacement RAM state before publishing:** compute a new `SegmentRecord` for each live record as if it were written at its position in the new compacted segment. Store these as a local `std::vector<SegmentRecord>` — do not modify `records_` yet.
7. Allocate a new generation from `nextGeneration_`. Write the compacted segment file with a segment header and one ENQUEUE event per live record (using original sequence numbers and current payloads). Flush and verify the output is parseable.
8. Build the new manifest: replace `[oldStart, oldEnd]` with `[newGen, newGen]` in the range list. Merge if contiguous with adjacent ranges. Enforce max-4 range limit.
9. Publish the manifest. If publish fails: delete the newly written segment file, discard the replacement RAM state, return failure. `records_` is unchanged.
10. After successful publish: swap the replacement RAM state into `records_` by updating the matching entries. Update `activeGenerations_`. Leave old segment files on disk.

`compactFull()` loops `compactOneSegment()` until it returns no-op or failure.

### Cleanup model

After each successful manifest publish, a cleanup pass may run. Cleanup lists all segment files on disk, identifies those whose generation is not referenced by any range or `tailGeneration` in the current manifest, and deletes one. Cleanup is idempotent: a crash during deletion leaves mount unaffected (unreferenced files are ignored). Referenced files are never deleted. Cleanup runs lazily — one file per idle window or startup — not in the hot path.

---

## Implementation order

Before starting any stage, read these files to ground yourself in the current code:

- `src/pqueue/append_log_common.h` / `.cpp` — binary format, serialisers, parsers
- `src/pqueue/append_log_store.h` / `.cpp` — main implementation
- `tests/posix/pqueue_append_log.cpp` — primary test file

Do not infer code structure from this document alone. The code is the source of truth for what currently exists.

After completing each stage, update the "Current implementation state" section to reflect the new code — rewrite it in place, not as a changelog. Then remove the completed stage entry from this list entirely. Completed stages leave no trace here; their details live in the code shape above. Keep known limitations and deferred decisions explicit in that section — they are load-bearing information for the next stage.

---

Tests are not optional at any stage. Each stage must be fully tested before the next begins — later stages build on earlier ones and will silently inherit any bugs left untested. The most dangerous bugs in this design are ordering bugs (RAM updated before manifest, manifest updated before segment file) and election bugs (wrong slot wins on mount). These cannot be caught by inspection alone; they require tests that simulate crash points and remount. Every stage below calls out its critical tests — the ones where a missing test is most likely to let a real bug through.

---

## Current implementation state

**`append_log_common.h/.cpp`** defines the binary format layer (`pqueue::append_log_detail` namespace): `ManifestRange { startGen, endGen }`, `ManifestData { epoch, nextGeneration, tailGeneration, ranges }`, `serialiseManifest` / `parseManifest`, and the segment/event serialisers and parsers. `parseManifest` returns false (not `DataCorrupt`) on any validation failure; the caller treats an invalid slot the same as a missing one. Segment files are named `seg-{08x}.bin`.

**`append_log_store.h/.cpp`** is the store layer. The manifest is authoritative for everything: which segments exist, in what order to replay them, and what generation to allocate next. All RAM state is derived from the manifest on mount and kept in sync on every write.

**Slot election.** Two anonymous-namespace helpers handle the A/B slots. `chooseInactiveSlot` selects where to write next: both missing → slot A; both exist but neither valid → nullptr (DataCorrupt); one missing/invalid → that slot; both valid → lower-epoch slot (equal epoch → B). `chooseWinningSlot` selects which slot to read: neither valid → false; one valid → that slot; both valid → higher epoch (equal epoch → A). `publishManifest` writes to the inactive slot, then calls `applyManifestToRam` only on success. `readManifest` applies `chooseWinningSlot` and returns the winner.

**`applyManifestToRam`** is the single RAM-reconstruction path. It sets `manifestRanges_` (mirrors `ManifestData::ranges`), rebuilds `activeGenerations_` (all generations from every range in manifest order, then the tail), and sets `nextGeneration_`. Called by both `publishManifest` after a successful write and by `scanSegments` after `readManifest` returns the winning slot.

**Mount (`scanSegments`).** Sets defaults, then calls `readManifest`. A valid manifest → `applyManifestToRam`. No valid manifest with segment files on disk → `DataCorrupt`. No valid manifest and no segment files → empty store, mount succeeds. After applying the manifest, a disk scan advances `nextGeneration_` past all segment files on disk (including dangling ones not referenced by the manifest). The replay loop iterates `activeGenerations_` in manifest order (not numeric generation order); a referenced segment missing or too small is `DataCorrupt`. Torn-tail handling and corruption detection rules are unchanged.

**Rollover (`rotateSegment`).** Promotes `activeGeneration_` to a closed range `{oldTailGen, oldTailGen}`, merges with the preceding range if contiguous, and checks the range count before any I/O — if adding this range would exceed `kManifestMaxRanges` it returns `RangeLimitExceeded` without touching the disk. If the count is acceptable it calls `createSegment` to write the new segment header, then publishes the updated manifest. A crash between `createSegment` and `publishManifest` leaves a dangling segment file; on remount the old manifest wins and the dangling file advances `nextGeneration_` without being replayed. `ensureActiveSegment` follows the same pattern for the first segment of a session.

**Compaction.** `chooseCompactionRange()` uses a HighestDeadRatio selector. It iterates `manifestRanges_` and for each range first checks whether any entry in `records_` maps to that generation span. If no live records exist in the range, the range is immediately returned as compaction-eligible (dead-range removal, no I/O needed). Otherwise, `segmentStats()` is used to compute dead bytes (totalBytes minus liveBytes) per generation and the ratio dead/total across the range. The range with the highest ratio is returned; nullopt is returned if no range has any dead bytes. The tail generation is never a candidate.

`collectLiveRecords(range, out)` iterates `records_` in FIFO order and reads current payloads from disk for every `SegmentRecord` whose `segmentGeneration` falls within the range; popped records are already absent from `records_`; rewritten records have their `segmentGeneration` and `payloadOffset` updated in-place by `appendRewriteEvent`, so `collectLiveRecords` automatically returns the rewritten payload.

`compactRange()` (called by `compactOneSegment`) orchestrates the full compaction step: if `collectLiveRecords` returns empty it publishes a manifest that drops the range entirely with no file I/O (dead-range removal). Otherwise it packs live records into as many output segments as needed. If `outputSegs.size() > inputSegCount`, or `outputSegs.size() == inputSegCount` with no dead bytes, it returns `Status::noOp()`. Otherwise it writes the output segments, then builds the replacement manifest: the old range entry is replaced with `[firstNewGen, lastNewGen]`, then a two-sided merge pass checks whether the replacement is contiguous with both the preceding and the following range. The updated manifest is published; on failure the new segment file is left on disk as a dangling file cleaned up later. After a successful publish, `applyManifestToRam` has already updated `manifestRanges_`, `activeGenerations_`, and `nextGeneration_`; the `records_` swap runs immediately after. `Status::noOp()` passes `.ok()` but also sets `.isNoOp() = true`.

`compactFull()` loops `compactOneSegment()` at most `manifestRanges_.size()` times (captured at entry), breaking early when `compactOneSegment` returns `Status::noOp()`. Intended for idle time or explicit maintenance — not the hot path. `writeRecord` calls `compactOneSegment()` directly (one bounded step per enqueue) when `needsCompaction()` is true. `needsCompaction()` triggers on segment count (`activeGenerations_.size() > maxSegments`) or low free space (`freeBytes < minFreeBytes`); it does not trigger on dead ratio alone. Dead-ratio compaction is maintenance-only, not a write-path obligation. Consequence: under segment-count pressure with all ranges fully live (zero reclaimable bytes), `needsCompaction()` returns true but `compactOneSegment()` returns no-op on every call. This is correct — there is nothing to reclaim — but it means the write path wastes one no-op compaction attempt per enqueue until live records are popped.

**Key invariants.** `manifestRanges_` is always ordered oldest→newest. Pop events carry the same rotation check as rewrite events — a sequence of pops can trigger a rotation and promote the current tail to a full range. Any test that pops records before calling `compactOneSegment` must account for this. A rewrite event moves the live payload offset for the rewritten sequence into the active segment, leaving the original enqueue bytes as dead bytes in their source segment; `segmentStats` counts them as dead.

**`canEnqueue()`** performs only the hard FS floor check: returns false if `freeBytes() < minFreeBytes`. It does not check `maxTotalBytes` or require free space for the record itself — those would block the write that triggers compaction. `writeRecord()` is authoritative for both.

**`maxTotalBytes`** is a per-queue soft cap on total on-disk footprint (live + dead bytes, including dangling segment files). `writeRecord()` is the enforcement point: before appending, it computes `totalOnDiskBytes() + appendGrowthBytes(recordSize)` — the tracked footprint plus the predicted growth of the write, including a segment header if the write will rotate or create the first segment. If this exceeds `maxTotalBytes`, it loops `compactOneSegment()` until the footprint fits or compaction returns no-op, at which point it returns `QueueFull`. After the `maxTotalBytes` loop, `writeRecord()` also enforces the hard FS floor: if `freeBytes() < minFreeBytes + appendGrowthBytes(recordSize)` it returns `QueueFull`. Compaction, cleanup, pop, and rewrite are all exempt. `maxTotalBytes = 0` disables the footprint cap; `minFreeBytes = 0` disables the FS floor. `Queue` maps `Config::reservedBytes` to `maxTotalBytes`.

**`FullQueuePolicy::DropOldest` with `maxTotalBytes`.** `Queue::enqueue()` calls `canEnqueue()` before `writeRecord()`. Since `canEnqueue()` no longer checks `maxTotalBytes`, a footprint-full queue returns `QueueFull` from `writeRecord()` rather than from `canEnqueue()`. `Queue::enqueue()` handles this: if `writeRecord()` returns `QueueFull` and the policy is `DropOldest`, it evicts the front record (creating dead bytes in the compacted ranges) and retries `writeRecord()` once. The retry's compaction loop then reclaims those dead bytes and proceeds.

**`totalOnDiskBytes_`** is a RAM counter kept in sync with all segment file I/O. Appended bytes are added directly: `appendEnqueueEventBytes`, `appendPopEvent`, and `appendRewriteEvent` each increment by the number of bytes written. File-level writes go through `writeSegmentFileTracked(name, data)`: it reads the old file size (zero if absent or stat fails), writes the file, then applies the delta `newSize - oldSize`. This delta approach is critical for correctness on failed-publish retries — if the same generation file is overwritten, the counter reflects only the net size change. `cleanupOneDanglingSegment` reads the file size before `removeFile` and subtracts on success. Initialised on mount by summing all seg-*.bin file sizes from disk (including dangling); `fileSize()` errors during this loop are returned as mount failures. `totalOnDiskBytes()` returns this field directly — no I/O.

**Known limitations.** `compactFull` may revisit a newly compacted range rather than advancing to the next original range (harmless but inefficient). No multi-range holistic compaction. No repair tooling for corrupted-but-non-fatal states. Cleanup drains one dangling file per publish pass; a store with many accumulated dangling files drains slowly under a quiet write rate. The `DropOldest` retry in `Queue::enqueue()` evicts and retries once; a second `QueueFull` (e.g., both compaction and eviction are insufficient) is returned as an error without further eviction.



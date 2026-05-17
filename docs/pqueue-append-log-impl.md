# pqueue Append-Log Implementation Notes

**Editing rules:** ASCII only — no Unicode symbols (no checkmarks, arrows, emoji). This file is compiled to PDF via LaTeX and non-ASCII characters cause build warnings or missing glyphs.

**Design target:** `pqueue-append-log-design.pdf` — dual-manifest, segment-file backend.

**Current code state:** Stages 0–4 and 5a complete. Manifest-backed mount, slot election, and rollover are live. Every segment rotation publishes a manifest; `RangeLimitExceeded` signals when compaction is required before the next rollover. Range selection for compaction is implemented. Live record collection and compaction write are not yet wired.

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

## Current implementation state (Stages 0–4)

### Code shape

**`append_log_common.h/.cpp`** — binary format layer (`pqueue::append_log_detail` namespace):
- `ManifestRange { startGen, endGen }`, `ManifestData { epoch, nextGeneration, tailGeneration, ranges }`
- `serialiseManifest(const ManifestData&, std::vector<uint8_t>& out)` — clears `out` then writes
- `parseManifest(const uint8_t* data, size_t size, ManifestData& out) → bool` — returns false (not DataCorrupt) on any validation failure; caller treats invalid same as missing
- Constants: `kManifestMagic`, `kManifestVersion`, `kManifestFixedBytes` (30), `kManifestMaxRanges` (4)
- Segment files: `seg-{08x}.bin`; `SegmentHeader::startSeq` (was `baseSequence`)

**`append_log_store.h/.cpp`** — store layer:
- `publishManifest(const ManifestData&)` — writes to inactive slot, updates RAM only on success; uses `chooseInactiveSlot()` / `chooseWinningSlot()` free functions
- `readManifest(ManifestData& out) → bool` — reads both slots, returns winning manifest or false
- `applyManifestToRam(const ManifestData&)` — single path for expanding manifest into `activeGenerations_`, `nextGeneration_`, and `manifestRanges_`; called by both `publishManifest` and `scanSegments`
- `rotateSegment()` — promotes current tail to a full range, range-merges, checks limit, creates segment, publishes manifest
- `ensureActiveSegment()` — publishes manifest after creating the first segment of a session
- `scanSegments()` — manifest-authoritative mount path (see below)
- `manifestRanges_` field mirrors `ManifestData::ranges` in RAM; updated on every `applyManifestToRam()` call

### Slot-election helpers (anonymous namespace)

`chooseInactiveSlot(existsA, validA, epochA, existsB, validB, epochB) → const char*`:
- Both missing → slot A (fresh store)
- Both exist but neither valid → nullptr (caller returns DataCorrupt)
- One missing/invalid → that slot
- Both valid → lower-epoch slot (equal epoch → B)

`chooseWinningSlot(validA, mdA, validB, mdB, out) → bool`:
- Neither valid → false
- One valid → that slot
- Both valid → higher epoch; equal epoch → slot A

### Mount path (`scanSegments`)

Preamble sets defaults (`activeGenerations_` empty, `manifestRanges_` empty, `activeSegmentBytes_ = 0`, `nextGeneration_ = 1`, `nextSequence_ = 0`), then:

1. `readManifest()` → if valid, `applyManifestToRam()` sets `activeGenerations_`, `manifestRanges_`, and `nextGeneration_`
2. If no valid manifest and segment files exist on disk → `DataCorrupt`
3. If no valid manifest and no segment files → empty store (mount succeeds)
4. Disk scan advances `nextGeneration_` past all segment files, including dangling ones

Replay loop iterates `activeGenerations_` in manifest order (not numeric order). A referenced segment missing or too small → `DataCorrupt`. Torn-tail and corruption rules unchanged.

### Rollover path (`rotateSegment`)

1. Save `oldTailGen = activeGeneration_`
2. Build `newRanges = manifestRanges_` + promote `{oldTailGen, oldTailGen}` (merge if contiguous with last range)
3. If `newRanges.size() > kManifestMaxRanges` → return `RangeLimitExceeded` (check before any I/O; no segment file created)
4. `createSegment(newGen, baseSeq)` — writes segment header, updates `activeGeneration_`, `activeSegmentBytes_`, `nextGeneration_`
5. Build `ManifestData{ranges=newRanges, tailGeneration=newGen, nextGeneration=nextGeneration_}`
6. `publishManifest()` — writes to inactive slot, calls `applyManifestToRam()` on success

Crash between steps 4 and 6: new segment file is dangling. On remount, old manifest wins; dangling file advances `nextGeneration_` (preventing reuse) but is not replayed. Stage 7 cleanup removes it.

### Compaction helpers (Stage 5a)

`CompactionRange { startGen, endGen }` — public struct on `AppendLogStore`.

`chooseCompactionRange() const -> std::optional<CompactionRange>`:
- Returns `manifestRanges_[0]` as a `CompactionRange` if `manifestRanges_` is non-empty.
- Returns nullopt if no full ranges exist (empty store or tail-only store).
- The tail generation is never eligible — it is excluded by design since only `manifestRanges_` (full ranges) is consulted; the tail lives in `activeGeneration_` separately.

**Invariant: `manifestRanges_` is always ordered oldest→newest.** `chooseCompactionRange()` relies on this — `[0]` is correct only if manifest order equals logical age. This is true by construction today: rollover always appends the promoted tail to the end, and `applyManifestToRam()` preserves manifest order. Stage 5c must maintain it when rebuilding the range list after compaction: replace `[oldStart, oldEnd]` in-place, then merge adjacent entries without resorting.

### Known temporary limitations

- **`needsCompaction()` uses a generation-span heuristic** instead of `activeGenerations_.size()` (Stage 6 TODO comment in code). Overestimates segment pressure before compaction is wired.
- **Cleanup not implemented** (Stage 7). Dangling segment files (from failed rotations or superseded compactions) accumulate on disk.
- **`compact()` for the no-live-records path does not publish an updated manifest** — next `ensureActiveSegment()` call will publish one, but a remount between compact and the next write would see a manifest pointing to deleted segments → DataCorrupt. Acceptable at this stage; addressed in Stage 5c when compaction is fully wired.


## Implementation order

### Stage 5b — Live record collection

Add `collectLiveRecords()`:

```cpp
Status collectLiveRecords(const CompactionRange& range,
                          std::vector<CompactionLiveRecord>& out) const;
```

Iterate `records_`, extract those whose `segmentGeneration` falls within the selected range, read current payloads, return in FIFO order. No file writes, no RAM mutation. Test: live records collected in order; popped records absent; rewritten records return current payload; empty result is success/no-op. **Critical test:** enqueue a record, rewrite it, then collect — the collected payload must be the rewritten value, not the original. A bug here would silently compact stale data.

### Stage 5c — compactOneSegment() v1

```cpp
Status compactOneSegment();
Status compactFull();
```

Wire range selection, live record collection, segment write, manifest publish, and RAM update into `compactOneSegment()`. Enforce step ordering: write segment file before publishing manifest, publish manifest before updating `records_` and `activeGenerations_`. Test: fully dead range removed; live records preserved in FIFO order; `rewriteFront()` result preserved; no-op when live bytes exceed `maxSegmentBytes`; manifest published only after new file written; `records_` updated only after manifest success; old segment files remain on disk; remount after compaction uses new segment. **Critical test:** simulate manifest publish failure after the new segment file is written — on remount the old segment must still be active, the new segment must be dangling, and all records must be intact. This is the core crash-safety guarantee of the whole compaction design.

### Stage 6 — Compaction trigger

Wire compaction into `writeRecord()`. Update `needsCompaction()` to use `activeGenerations_.size() > config_.maxSegments` as the segment-count trigger, replacing the current numeric generation span. Free-space pressure (`freeBytes() < config_.minFreeBytes`) remains a secondary trigger. Test: repeated enqueue/rotate eventually triggers compaction; queue behaviour correct before and after remount. **Critical test:** trigger compaction with a live queue, then remount — every record enqueued before compaction must still be peekable and poppable in the original FIFO order.

### Stage 7 — Cleanup

Implement lazy deletion of dangling segment files. Only delete files whose generation is absent from both the winning manifest's `ranges` and its `tailGeneration` — never anything the winning manifest references. A crash mid-delete is safe: the surviving file is re-evaluated on next mount and the manifest is unchanged, so cleanup is idempotent. Test: dangling files deleted; referenced files untouched; cleanup crash is safe. **Critical test:** run cleanup with a file that is referenced by the current manifest — it must not be deleted under any circumstances.

**Sequence exhaustion (low priority, address here or separately):** `nextSequence_` is a `uint32_t`. Wrap-around would silently corrupt FIFO ordering. Fix must fail closed: detect wrap at enqueue time and require `format()` or a repair path rather than continuing.


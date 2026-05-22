# pqueue Append-Log Implementation Notes

**Editing rules:** ASCII only -- no Unicode symbols (no checkmarks, arrows, emoji). This file is compiled to PDF via LaTeX and non-ASCII characters cause build warnings or missing glyphs.

The append-log store is a dual-manifest, segment-file backend. The manifest is
the authoritative record of which segment generations are live and in what order
to replay them. All RAM state is derived from the manifest on mount and kept in
sync on every mutation. The implementation is split across
`src/pqueue/append_log_store/`: `store.cpp` (mount, scan, public API),
`manifest.cpp` (slot election, publish, apply), and `compaction.cpp`
(compaction, cleanup).

**POSIX tests:** `tests/posix/pqueue_append_log_compaction.cpp` contains regression
coverage for `collectLiveRecords()` FIFO ordering after rewrite+compact+remount, plus a
transition-matrix section with one test per ugly operation sequence: pop→compact→remount,
rewrite→compact→remount, pop→rewrite→compact→remount, rewrite→pop→compact→remount,
rewrite-old/rotate-tail→compact→remount, subrange-compact→remount, compactFull→remount.
`tests/posix/pqueue_append_log_validate.cpp` covers validateUnlocked: fresh store, missing
segment, both slots corrupt, overlapping ranges, nextGeneration below max ref, wrong header
generation, corrupt CRC in sealed segment, torn tail in tail (ok), torn tail in sealed
(JournalCorrupt), dangling segment ignored. Tests call AppendLogStore::validateUnlocked
directly to bypass the queue lock (which requires a successful mount).
Run with `make -j12 test`.

---

## Scan and torn-tail rules

These rules govern `AppendLogStore::scanSegments()` and must be preserved exactly.

Each segment is scanned with two cursors: `offset` (current read position,
starts at `kSegmentHeaderBytes`) and `lastGoodOffset` (end of the last fully
validated event, also starts at `kSegmentHeaderBytes`). A `corrupt` flag is set
when a complete but invalid record is found.

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
  - If `isLastSegment`: call `f->resizeFile(name, lastGoodOffset)` (truncation failure is non-fatal -- next remount re-discards the same tail); set `activeSegmentBytes_ = lastGoodOffset`
- If `offset == fileSize` (clean EOF): set `activeSegmentBytes_ = offset`

**`isLastSegment`** is determined by position in the manifest-derived generation
list, not numeric generation value. After compaction, the tail may not be the
numerically largest generation.

---

## Binary format

**`append_log_common.h/.cpp`** defines the binary format layer (`pqueue::append_log_detail` namespace): `ManifestRange { startGen, endGen }`, `ManifestData { epoch, nextGeneration, tailGeneration, ranges }`, `serialiseManifest` / `parseManifest`, and the segment/event serialisers and parsers.

There are two format version fields: `kFormatVersion = 0` is used in segment
headers and all event types; `kManifestVersion = 1` is the manifest-specific
format version. These are versioned independently.

**Segment file header** (20 bytes, at offset 0 of each segment file, `kSegmentHeaderBytes = 20`):

```
magic       4 bytes   0x47535150 ("PQSG")
version     2 bytes   kFormatVersion = 0
headerBytes 2 bytes   20
generation  4 bytes   u32, matches the segment generation encoded in the filename; live sealed segments are referenced by manifest ranges, and the active tail is referenced by tailGeneration
startSeq    4 bytes   u32, informational: first enqueue sequence expected
headerCrc   4 bytes   CRC32 over preceding 16 bytes
```

**ENQUEUE / REWRITE event** (variable length, kEnqueueOverheadBytes = 24 fixed + payload):

```
magic       4 bytes   0x51455150 ("PQEQ") for ENQUEUE, 0x45525150 ("PQRE") for REWRITE
version     2 bytes   kFormatVersion = 0
headerBytes 2 bytes   16  (kEnqueueHeaderBytes)
sequence    4 bytes   u32
payloadBytes 4 bytes  u32
payload     N bytes   opaque bytes
crc         4 bytes   CRC32 over header + payload
footer      4 bytes   0x214B4F50 ("POK!")
```

**POP event** (20 bytes, kPopEventBytes = 20):

```
magic       4 bytes   0x45505150 ("PQPE")
version     2 bytes   kFormatVersion = 0
headerBytes 2 bytes   20
sequence    4 bytes   u32
eventCrc    4 bytes   CRC32 over magic + version + headerBytes + sequence
footer      4 bytes   0x214B4F50 ("POK!")
```

**Manifest binary layout** (all fields little-endian, packed, CRC32 IEEE polynomial 0xEDB88320):

```
magic          4 bytes   0x464D5150 ("PQMF")
version        2 bytes   kManifestVersion = 1
headerBytes    2 bytes   total manifest size in bytes
epoch          4 bytes   u32, incremented on each publish
nextGeneration 4 bytes   u32, next generation to allocate
rangeCount     2 bytes   u16, number of full segment ranges
ranges         N x 8 bytes   startGen u32 + endGen u32 per range
tailGeneration 4 bytes   u32
crc            4 bytes   CRC32 over all preceding bytes
footer         4 bytes   0x214B4F50 ("POK!")
```

Fixed overhead: 30 bytes. With 4 ranges: 30 + 32 = 62 bytes. Hard limit: 64
bytes total (LittleFS inline file threshold). Maximum 4 ranges (`kManifestMaxRanges`).

Segment files are named `seg-{08x}.bin`. `parseManifest` returns false (not
`DataCorrupt`) on any validation failure; the caller treats an invalid slot the
same as a missing one. `parseManifest` also actively rejects manifests where
any range has `startGen == 0`, enforcing the invariant that generation 0 is
never allocated.

**Epoch:** starts at 1. Epoch 0 means "never published" -- a slot with epoch 0
is lower priority than any slot with epoch >= 1. `tailGeneration = 0` means no
tail segment exists. Generation 0 is never allocated. A manifest where
`tailGeneration == 0` and `rangeCount != 0` is malformed.

---

## Slot election and manifest publish

Two anonymous-namespace helpers in `manifest.cpp` handle A/B slot selection.

`chooseInactiveSlot` (where to write next): both missing -> slot A; both exist
but neither valid -> nullptr (DataCorrupt); one missing/invalid -> that slot;
both valid -> lower-epoch slot (equal epoch -> B).

`chooseWinningSlot` (which slot to read): neither valid -> false; one valid ->
that slot; both valid -> higher epoch (equal epoch -> A).

`publishManifest` writes to the inactive slot, then calls `applyManifestToRam`
only on success. `readManifest` applies `chooseWinningSlot` and returns the
winner.

**Manifest slot cache.** After a successful `publishManifest`, the written slot
name and its epoch are stored in `cachedWrittenSlot_` and `cachedWrittenEpoch_`.
On subsequent calls, `publishManifest` uses the cache to skip the slow path
(2x `fileSize` + 2x `readFile` probe, roughly 4x ~35ms on device) and writes
directly to `otherSlot(cachedWrittenSlot_)`. The cache is cleared on `mount()`
and `format()` to force a fresh disk read. A crash between any two publish
calls is safe: on remount, `readManifest` rescans both slots from disk,
rebuilding the cache correctly.

**`applyManifestToRam`** is the single RAM-reconstruction path. It sets
`manifestRanges_` (mirrors `ManifestData::ranges`), rebuilds
`activeGenerations_` (all generations from every range in manifest order, then
the tail), and sets `nextGeneration_`. Called by both `publishManifest` after a
successful write and by `scanSegments` after `readManifest` returns the winning
slot.

---

## Mount

`scanSegments` sets defaults, calls `readManifest`, and applies the winning
manifest via `applyManifestToRam`. A valid manifest with no segment files on
disk is a fresh empty store; no valid manifest with segment files on disk is
`DataCorrupt`. After applying the manifest, a disk scan advances
`nextGeneration_` past all segment files on disk including dangling ones. The
replay loop iterates `activeGenerations_` in manifest order (not numeric
generation order); a referenced segment that is missing or too small is
`DataCorrupt`.

---

## Rollover

`rotateSegment` promotes `activeGeneration_` to a closed range
`{oldTailGen, oldTailGen}`, merges with the preceding range if contiguous, and
checks the range count before any I/O -- if adding the range would exceed
`kManifestMaxRanges` it returns `RangeLimitExceeded` without touching the disk.
It then calls `createSegment` to write the new segment header and publishes the
updated manifest. A crash between `createSegment` and `publishManifest` leaves
a dangling segment file; on remount the old manifest wins and the dangling file
advances `nextGeneration_` without being replayed.

`ensureActiveSegment` follows the same pattern for the first segment of a session.

---

## Compaction

### Strategy

`chooseCompactionRange()` uses HighestDeadRatio. For each range in
`manifestRanges_` it checks whether any entry in `records_` maps to that
generation span; a range with no live records is immediately returned as
compaction-eligible (dead-range fast path: selection needs no `fileSize` I/O,
just a RAM scan). The actual execution of a dead-range removal still publishes a
manifest and removes the retired segment files. Otherwise, `segmentStats()`
computes dead bytes (totalBytes minus liveBytes) per generation, and the range
with the highest dead/total ratio is returned. `nullopt` is returned if no range
has any dead bytes. The tail generation is never a candidate.

### collectLiveRecords

`collectLiveRecords(range, out)` iterates `records_` in FIFO order and reads
current payloads from disk for every `SegmentRecord` whose `segmentGeneration`
falls within the range. Popped records are absent from `records_`. Rewritten
records have their `segmentGeneration` and `payloadOffset` updated in-place by
`appendRewriteEvent`, so `collectLiveRecords` automatically returns the rewritten
payload. Reads are batched: records are grouped by segment generation and each
segment file is read once with `readFile`, slicing payloads by offset.

**`removeRecord()` is a no-op for AppendLog.** POP tombstones are written by
`appendPopEvent()` inside `writeIndex()`, not by `removeRecord()`. This is
intentional: the Store interface calls both, but for AppendLog the tombstone is
already committed by the time `removeRecord` is reached.

### compactRange

`compactRange()` (called by `compactOneSegment`) orchestrates the full
compaction step:

**1. Parent-range lookup and subrange classification.** `findParentRangeIdx`
scans `manifestRanges_` for the unique range containing `[startGen, endGen]`.
If none is found, returns `noOp`. The subrange is classified: prefix
(`startGen == parent.startGen`, `endGen < parent.endGen`), suffix
(`startGen > parent.startGen`, `endGen == parent.endGen`), middle (both sides
strictly inside), or exact (matches parent). `hasLeft = (startGen > parent.startGen)`,
`hasRight = (endGen < parent.endGen)`.

**Range-count gate.** Splitting a parent costs up to +2 manifest ranges.
The gate checks `manifestRanges_.size() + gateDelta > kManifestMaxRanges` before
any I/O. `gateDelta`: live middle = 2, live prefix/suffix = 1, exact = 0, dead
prefix/suffix = 0, dead middle = 1. Liveness is determined by a RAM scan over
`records_`. If the gate fires and `allowFallback == AllowFullRangeFallback::yes`,
the range expands to the full parent and continues. If `allowFallback::no`
(default for the write path), returns `noOp` immediately. `compactIdle` and
`compactFull` pass `yes`; the write path passes `no`.

**2. Rotate-before-compact preflight.** `wouldRotate` is true when all three
hold: `inputRange.endGen == manifestRanges_.back().endGen` (subrange reaches the
last manifest range's right endpoint), the active tail is non-empty and its
generation equals `manifestRanges_.back().endGen + 1`, and `tailDepsContained`
is satisfied. When `wouldRotate`, the hypothetical effective range extends to
include `activeGeneration_`. Live payload sizes and output segment counts are
estimated from RAM (`records_`, `sealedSegmentBytes_`, `activeSegmentBytes_`) --
no fileSize calls.

**Tail dependency guard (`tailDepsContained`).** The active tail may contain POP
and REWRITE tombstones for records whose source segments lie outside the range
being compacted. Rotating and discarding the tail would resurrect those records
on remount. `activeTailAffectedGenerations_` tracks the set of source segment
generations touched by POP/REWRITE events written to the current tail.
`tailDepsContained` is true only when `activeTailDependenciesTracked_` is true
and every generation in that set falls inside `[inputRange.startGen, activeGeneration_]`.
`activeTailDependenciesTracked_` is set false on mount (tail contents unknown)
and set true with an empty set whenever a fresh tail is created. After a
successful compaction, all generations in `[inputRange.startGen, inputRange.endGen]`
are erased from `activeTailAffectedGenerations_` -- those segments no longer
exist and their tombstones cannot cause resurrection.

**3. No-op gate.** Two conditions each return `noOp` without touching state:

- `hypoOutputSegs > hypoInputSegs`: compaction would increase segment count
  regardless of dead bytes; bail unconditionally.
- `hypoOutputSegs == hypoInputSegs && no dead bytes`: segment count unchanged
  and nothing to reclaim.

**4. Pre-rotate gate and rotate.** When `wouldRotate && hypoHasLive && hasLeft`,
the post-rotate manifest shape is precomputed to check whether the resulting
split would overflow `kManifestMaxRanges`. `hypoHasLive` is a RAM scan --
required because `tailCanMergeWithLastRange` only guarantees the tail has
events, not live payload records (a pop-only tail must not trigger the +1 gate).
If overflow and `allowFallback::yes`, `inputRange` expands to the full parent
before the preflight. If `allowFallback::no`, returns `noOp` before any
mutation. After all gates pass, `rotateSegment()` is called -- but only when
`hypoPayloadBytes` is non-empty (the hypothetical range contains live records).
If `wouldRotate` is true but the range is entirely dead, rotate is skipped and
dead-range removal handles cleanup without rotating. `RangeLimitExceeded`
is structurally unreachable here: rotating merges the contiguous tail into the
last manifest range, leaving range count unchanged. After rotate, parent,
`hasLeft`, and `hasRight` are re-derived from the updated `manifestRanges_`
(defensive reclassification), and `inputRange.endGen` is extended to the new
parent endpoint.

**5. Dead-range removal.** If `collectLiveRecords` returns empty, the parent
range is dropped and any non-empty remainders are re-inserted
(`[parent.startGen, inputRange.startGen-1]` and/or
`[inputRange.endGen+1, parent.endGen]`). No output file is written;
`cleanupInputSegments(inputRange)` removes the dead segment files.

**6. Live-range compaction.** Output segments are written at generations
starting from `nextGeneration_`. The manifest splice: erase the parent range,
insert `[left_remainder, output, right_remainder]` at the same index (omitting
empty sides). A two-sided merge pass checks contiguity with adjacent ranges.
After `publishManifest`, `cleanupInputSegments(inputRange)` removes only the
compacted input segments; remainder segment files are untouched. `records_`
entries for `inputRange` are updated in-place to point at the new segment
positions.

### Cleanup

`cleanupInputSegments(effectiveRange)` removes retired input segments directly
by name after a successful compaction manifest publish, using
`sealedSegmentBytes_` for size accounting -- no directory scan required.

`cleanupOneDanglingSegment` handles stragglers from interrupted compactions or
crashes. It is called once at the end of `scanSegments()` on mount. It is not
called after every manifest publish. Dangling segments that survive a single
mount pass are caught on subsequent remounts. Successful compaction removes the
retired input segments it just replaced, but it does not perform generic
dangling-output cleanup.

### compactOneSegment, compactFull, compactIdle

`compactOneSegment()` calls `chooseCompactionRange()`, then applies
`narrowRange()` to bound the compaction unit at `maxOutputSegments` (default 8)
predicted output segments. `narrowRange()` uses an O(n^2) sliding window over
per-segment stats to find the contiguous subrange with the highest dead ratio
that fits within the budget.

`compactFull()` delegates to `compactIdle(initialRangeCount)` where
`initialRangeCount = manifestRanges_.size()` at call time. This is a bounded
maintenance helper, not a guarantee that the store is fully compacted afterward:
if a range is large and `narrowRange()` narrows to a subrange, a single step
leaves residual work. Callers that need a fully clean store must loop until
`compactIdle` returns noOp.

`compactIdle(maxSteps)` runs up to `maxSteps` calls to `compactOneSegment`.
Each attempt counts as one step regardless of outcome. Stops early on noOp.
Returns `CompactIdleResult { status, stepsRun, compactions, noOps, moreWorkLikely }`
where `moreWorkLikely` is true only when the loop stopped by budget exhaustion
after at least one successful compaction. `Queue::compactIdle` is a lock-guarded
thin wrapper.

`writeRecord` calls `compactOneSegment()` directly (one bounded step per
enqueue) when `needsCompaction()` is true. `needsCompaction()` triggers on
segment count (`activeGenerations_.size() > maxSegments`) or low free space
(`freeBytes < minFreeBytes`); it does not trigger on dead ratio alone. Under
segment-count pressure with all ranges fully live, `needsCompaction()` returns
true but `compactOneSegment()` returns noOp on every call -- nothing to reclaim,
one wasted attempt per enqueue until records are popped.

---

## Capacity enforcement

**`maxTotalBytes`** caps total on-disk footprint (live + dead bytes, including
dangling files). `writeRecord` is the enforcement point: before appending, it
computes `totalOnDiskBytes() + appendGrowthBytes(recordSize)`. If this exceeds
`maxTotalBytes`, it loops `compactOneSegment()` until the footprint fits or
compaction returns noOp, then returns `QueueFull`. The hard FS floor
(`minFreeBytes`) is checked after the compaction loop. `maxTotalBytes = 0`
disables the footprint cap; `minFreeBytes = 0` disables the FS floor. `Queue`
maps `Config::reservedBytes` to `maxTotalBytes`.

**`totalOnDiskBytes_`** is a RAM counter kept in sync with all segment file I/O.
Appended bytes are added directly. File-level writes go through
`writeSegmentFileTracked(name, data)`: it reads the old file size (zero if
absent), writes the file, then applies the delta `newSize - oldSize`. This
delta approach ensures correctness on failed-publish retries where the same
generation file may be overwritten. `cleanupOneDanglingSegment` subtracts on
successful `removeFile`. Initialised on mount by summing all `seg-*.bin` file
sizes from disk including dangling files.

**`canEnqueue()`** performs only the hard FS floor check: returns false if
`freeBytes() < minFreeBytes`. It does not check `maxTotalBytes` or require free
space for the record itself -- `writeRecord` is authoritative for both.

**`FullQueuePolicy::DropOldest`** with `maxTotalBytes`: `Queue::enqueue()` calls
`canEnqueue()` before `writeRecord()`. Since `canEnqueue()` does not check
`maxTotalBytes`, a footprint-full queue returns `QueueFull` from `writeRecord`.
`Queue::enqueue()` handles this: if `writeRecord()` returns `QueueFull` and the
policy is `DropOldest`, it evicts the front record and retries `writeRecord()`
once. A second `QueueFull` is returned as an error without further eviction.

---

## Key invariants

`manifestRanges_` is always ordered oldest->newest. Pop events carry the same
rotation check as rewrite events -- a sequence of pops can trigger a rotation
and promote the current tail to a full range; tests that pop before calling
`compactOneSegment` must account for this. A rewrite event moves the live
payload offset for the rewritten sequence into the active segment, leaving the
original enqueue bytes as dead bytes in their source segment; `segmentStats`
counts them as dead. `Status::noOp()` passes `.ok()` and also sets
`.isNoOp() = true`.

---

## Limitations and future work

**`compactFull` range revisiting.** `compactFull` may revisit a newly compacted
range rather than advancing to the next original range. Harmless but
inefficient.

**Orphan tail after rotate-before-compact.** Rotate-before-compact leaves the
orphan tail generation numerically below the compaction output range. When the
orphan later receives live records and rotates, it forms a separate range that
cannot immediately merge with the output range. Range count stays bounded and
dead-range elimination reclaims it once popped, but the fragmentation is
inelegant.

**`activeTailDependenciesTracked_` suppression window.** After mount, tracking
is false until the first rotation creates a fresh tail. During that window
rotate-before-compact is suppressed entirely. Conservative but correct; a
mount-time tail rescan would close the gap.

**Cross-range REWRITE test gap.** No test covers the case where the tail
contains a REWRITE tombstone for a record in a different range (the analogous
cross-range POP test covers the same guard path and the production code handles
both identically, but a dedicated REWRITE test would close the gap).

**Configurable `kManifestMaxRanges`.** The current manifest format (30B fixed +
4x8B = 62B) fits within the LittleFS 64-byte inline threshold. For devices
with larger flash queueing megabytes of data, compaction latency scales with
queue size. More ranges (e.g. 8 ranges = 94B) would enable more subrange splits
and tighter per-step latency bounds. Deferred: touches the manifest binary
format, requires a version bump, and expands the test matrix significantly.

**`compactIdle` not exposed on Outbox.** `pqueue::Outbox` and
`pqueue::http::Outbox` do not expose `compactIdle`. Callers who need explicit
idle compaction must use `Queue` directly.

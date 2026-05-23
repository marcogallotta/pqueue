# Append-Log Implementation Notes

This document is internal design reference for contributors. It is not user-facing documentation; see `docs/usage.md` for operating guidance.

The append-log store is a dual-manifest, segment-file backend. The manifest is
the authoritative record of which segment generations are live and in what order
to replay them. All RAM state is derived from the manifest on mount and kept in
sync on every mutation. The implementation is split across
`src/pqueue/append_log_store/`: `store.cpp` (mount, scan, public API),
`manifest.cpp` (slot election, publish, apply), and `compaction.cpp`
(compaction, cleanup).

**POSIX tests:** `tests/posix/pqueue_append_log_compaction.cpp` contains regression
coverage for `collectLiveRecords()` FIFO ordering after rewrite+compact+remount, plus a
transition-matrix section with one test per ugly operation sequence: pop->compact->remount,
rewrite->compact->remount, pop->rewrite->compact->remount, rewrite->pop->compact->remount,
rewrite-old/rotate-tail->compact->remount, subrange-compact->remount, compactFull->remount.
`tests/posix/pqueue_append_log_validate.cpp` covers validateUnlocked: fresh store, missing
segment, both slots corrupt, overlapping ranges, nextGeneration below max ref, wrong header
generation, corrupt CRC in sealed segment, torn tail in tail (ok), torn tail in sealed
(JournalCorrupt), dangling segment ignored. Tests call AppendLogStore::validateUnlocked
directly to bypass the queue lock (which requires a successful mount).

`Queue::validate()` / `AppendLogStore::validateUnlocked()` is already a full-depth scan.
It reads every event in every referenced segment (sealed and tail), CRC-checks every
ENQUEUE/REWRITE payload, checks every footer magic, validates manifest structure and
range consistency, and verifies every referenced segment file exists and is large enough.
There is no shallower or deeper variant to add at the queue layer. The only validation
not covered is application-layer envelope decoding (e.g. whether a payload is a valid
`RequestEnvelope`), which is the responsibility of the Outbox layer via
`Outbox::validatePayloads()`.
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

**Orphaned REWRITE events:** If a REWRITE event references a sequence not yet in
`records_` (because its original ENQUEUE was in a dead range that was compacted
away), the REWRITE becomes authoritative: a new `SegmentRecord` is inserted in
sequence order. This handles the case where the dead-range fast path deleted the
source segment before the REWRITE event's segment was compacted.

Two invariants make this safe:

1. **No dangling REWRITEs at write time.** `appendRewriteEvent` validates that
   the sequence exists in `records_` before writing the event. A REWRITE for a
   non-existent or already-popped sequence is rejected. Therefore every REWRITE
   event on disk was written while the sequence was live, and POP-then-REWRITE
   for the same sequence is impossible in any valid store.

2. **Contiguity preserved.** The FIFO constraint ensures no gaps can appear after
   orphaned-REWRITE insertions are complete. If seq=X is alive (its REWRITE
   survives), no seq=Y with Y < X can have been popped without first popping X.
   Any sequence between X and the current head that was also in a deleted range
   was also REWRITE'd and will be inserted by the same scan pass. Temporary
   per-segment gaps during scan do not matter because `readRecord` is not called
   until `scanSegments` returns.

---

## Binary format

**`append_log_common.h/.cpp`** defines the binary format layer (`pqueue::append_log_detail` namespace): `ManifestRange { startGen, endGen }`, `ManifestData { epoch, nextGeneration, tailGeneration, ranges }`, `serialiseManifest` / `parseManifest`, and the segment/event serialisers and parsers.

There are two format version fields: `kFormatVersion = 1` is used in segment
headers and all event types; `kManifestVersion = 1` is the manifest-specific
format version. These are versioned independently.

**Segment file header** (20 bytes, at offset 0 of each segment file, `kSegmentHeaderBytes = 20`):

```
magic       4 bytes   0x47535150 ("PQSG")
version     2 bytes   kFormatVersion = 1
headerBytes 2 bytes   20
generation  4 bytes   u32, matches the segment generation encoded in the filename; live sealed segments are referenced by manifest ranges, and the active tail is referenced by tailGeneration
startSeq    4 bytes   u32, informational: first enqueue sequence expected
headerCrc   4 bytes   CRC32 over preceding 16 bytes
```

**ENQUEUE / REWRITE event** (variable length, kEnqueueOverheadBytes = 24 fixed + payload):

```
magic       4 bytes   0x51455150 ("PQEQ") for ENQUEUE, 0x45525150 ("PQRE") for REWRITE
version     2 bytes   kFormatVersion = 1
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
version     2 bytes   kFormatVersion = 1
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

### Workload characterisation

Enqueue is driven by backend failures: brief and intermittent, or a sustained
outage producing a burst. Pop is a rapid-fire drain after the backend recovers,
throttled by the caller. The two phases rarely overlap significantly.

During a pure enqueue burst there is no dead data (nothing has been popped), so
compaction has nothing to reclaim and should be a no-op. During a drain burst,
pops create dead data but no new enqueues arrive.

**Pops do not trigger compaction.** `needsCompaction()` is only checked in
`commitEnqueue`. The drain phase never stalls for compaction, which is correct
for latency. Dead data from a completed drain sits on flash until the next
enqueue triggers pressure or `compactIdle` runs explicitly.

**The clean-storage invariant.** If the queue is fully compacted before an
enqueue burst, the burst writes only new live data, stays within maxSegments,
and triggers no compaction. The write-path stall is a symptom of carrying dead
data into the burst from the previous drain. Fix the drain phase; the enqueue
phase fixes itself.

**Background compaction fits poorly here.** LittleFS is not concurrent: a
background compaction task still serialises against foreground I/O, adding
context-switch overhead without real parallelism. During an active burst or
drain there is no gain. The productive window is idle time between phases, which
the application already knows about. `compactIdle` called at that boundary is
the right mechanism.

### Strategy

**HighestDeadRatio** picks the range with the highest dead/total byte ratio,
skipping any range with no dead bytes. All evaluated alternatives either
produced true deadlocks under load or were mathematically identical to
HighestDeadRatio (dead/total and dead/live rank ranges identically -- both are
monotonically increasing functions of dead bytes given fixed total).

**Why usefulness-gating is required.** Any strategy that compacts live ranges
(OldestFirst, PressureWeightedHybrid, DeadByteThreshold) produces true
deadlocks under recoverable workloads. Compacting a fully-live range consumes a
compaction pass and a range slot without reclaiming anything, while dead data
accumulates elsewhere. HighestDeadRatio gates on dead bytes and refuses to touch
fully-live ranges.

**Capacity exhaustion is not a strategy failure.** When the queue fills with
live data and enqueue growth permanently exceeds drain rate, no compaction
strategy can help -- it is equivalent to any bounded queue hitting its size
limit. The right response is application-level backpressure or a larger queue.
The Deadlock/CapExhst metrics distinguish true deadlocks (compaction could have
helped but did not) from capacity exhaustion (no dead bytes to reclaim).

`chooseCompactionRange()` implements HighestDeadRatio. For each range in
`manifestRanges_` it checks whether any entry in `records_` maps to that
generation span; a range with no live records is immediately returned as
compaction-eligible (dead-range fast path: selection needs no `fileSize` I/O,
just a RAM scan). The actual execution of a dead-range removal still publishes a
manifest and removes the retired segment files. Otherwise, `segmentStats()`
computes dead bytes (totalBytes minus liveBytes) per generation, and the range
with the highest dead/total ratio is returned. `nullopt` is returned if no range
has any dead bytes. The tail generation is never a candidate.

**O(1) preflight sizing.** The preflight loop in `compactRange()` uses
`sealedSegmentBytes_.find(gen)` for O(1) size lookup instead of
`fs()->fileSize()` per segment (~21ms each on LittleFS). Prior to this
optimisation, the same workload (burst=500/pop=90%/rec=492B) produced 82580ms
max step latency; the bottleneck was per-segment fileSize calls on the hot path.

### collectLiveRecords

`collectLiveRecords(range, out)` iterates `records_` in FIFO order and reads
current payloads from disk for every `SegmentRecord` whose `segmentGeneration`
falls within the range. Popped records are absent from `records_`. Rewritten
records have their `segmentGeneration` and `payloadOffset` updated in-place by
`appendRewriteEvent`, so `collectLiveRecords` automatically returns the rewritten
payload. Reads are batched: records are grouped by segment generation and each
segment file is read once with `readFile`, slicing payloads by offset.

**`commitPop(expectedSequence)`** appends the POP tombstone via `appendPopEvent()`
and pops `records_.front()` in a single call. It fails closed with `InvalidIndex`
if `records_` is empty or `records_.front().sequence != expectedSequence`.

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

`compactFull()` runs `compactIdle(1)` in a loop until no compaction candidate
remains. On success, it leaves no compactable dead-data ranges, but may run many
steps and block for a proportionally long time. Use it only in offline tools and
maintenance firmware. In firmware loops, drive compaction with `compactIdle(n)`
instead.

`compactIdle(maxSteps)` runs up to `maxSteps` calls to `compactOneSegment`.
Each attempt counts as one step regardless of outcome. Stops early on noOp.
Returns `CompactIdleResult { status, stepsRun, compactions, noOps, moreWorkLikely }`
where `moreWorkLikely` is true only when the loop stopped by budget exhaustion
after at least one successful compaction. `Queue::compactIdle` is a lock-guarded
thin wrapper.

`commitEnqueue` calls `compactOneSegment()` directly (one bounded step per
enqueue) when `needsCompaction()` is true. `needsCompaction()` triggers on
segment count (`activeGenerations_.size() > maxSegments`) or low free space
(`freeBytes < minFreeBytes`); it does not trigger on dead ratio alone. Under
segment-count pressure with all ranges fully live, `needsCompaction()` returns
true but `compactOneSegment()` returns noOp on every call -- nothing to reclaim,
one wasted attempt per enqueue until records are popped.

---

## Simulator

### Correctness simulator

`tools/pqueue_compaction_sim.cpp`. Build: `make -j12 sim`. Run:
`./build/pqueue-compaction-sim`. Full sweep in ~2 seconds (in-memory FS, early
abort at 1000 failures).

Drives the real `AppendLogStore` API through two workload families:

**Random interleaved.** enqP in {0.55, 0.65, 0.80}, record size 19 bytes.

**Burst.** Models offline-consumer pattern: enqueue N, drain popRatio fraction,
repeat. burstSize in {12, 60, 250}, popRatio in {0.25, 0.5, 0.9}, recordSize
in {8, 19, 62} bytes. Parameters scaled ~1/8 from production values to keep
runs fast while preserving records-per-segment ratio.

Compaction trigger: rising-edge on segment count (new segment written), fires
if any range exceeds `deadRatioTrigger` or range count reaches
`rangePressureTrigger`. Segment count is the correct rising edge -- range count
stays at 1 with a single contiguous range.

---

## On-device validation

**esp32s3-littlefs** (`tests/arduino/test_pqueue_littlefs/test_main.cpp`): LittleFS
correctness suite. Covers basic FIFO, remount persistence, pop/rewrite/compact
persistence across reboots, capacity behaviour, validate, record size boundaries,
independent lock paths, outbox backlog persistence, retryable-failure semantics,
compactIdle survival across remount, and DropOldest eviction.
Run: `~/venvs/esp/bin/pio test -e esp32s3-littlefs`.

**esp32s3-littlefs-slow** (`tests/arduino/test_pqueue_littlefs_slow/test_main.cpp`):
Multi-reboot sequence tests. Each test triggers a deliberate reboot mid-operation
and verifies state after remount. Covers: fifo-many, pop-remaining, rewrite-front,
outbox-drain, compaction-reboot, and a churn pass without reboot.
Run: `~/venvs/esp/bin/pio test -e esp32s3-littlefs-slow`.

**esp32s3-littlefs-soak** (`tests/arduino/test_pqueue_littlefs_soak/test_pqueue_littlefs_soak_test_main.cpp`):
30-cycle enqueue/pop churn with segment rollover on real LittleFS. Verifies
remount correctness under sustained load. Does not call compactIdle (compaction
soak is covered by focused POSIX tests).
Run: `~/venvs/esp/bin/pio test -e esp32s3-littlefs-soak`.

**esp32s3-compaction** (`tests/arduino/test_pqueue_compaction/test_main.cpp`):
Compaction benchmarker. Build and upload only: `~/venvs/esp/bin/pio test -e esp32s3-compaction --without-testing`.

**Results** (ESP32S3, QSPI flash):

| Config | Compactions | NoOps | MaxOutSegs | MaxLatency | Deadlocks | CapExhausted |
|---|---|---|---|---|---|---|
| burst=100/pop=90%/rec=150B/5cy | 100 | 0 | 1 | 1090ms | 0 | 0 |
| burst=500/pop=90%/rec=492B/3cy | 17 | 6 | 8 | 4861ms | 0 | 0 |

Prior to O(1) preflight sizing, bulk collectLiveRecords reads, and targeted cleanup,
the heavy workload produced 82580ms MaxLatency; the bottlenecks were directory
scans and per-record fileSize calls on the hot path.

---

## Capacity enforcement

**`maxTotalBytes`** caps total on-disk footprint (live + dead bytes, including
dangling files). `commitEnqueue` is the enforcement point: before appending, it
computes `totalOnDiskBytes() + appendGrowthBytes(recordSize)`. If this exceeds
`maxTotalBytes`, it loops `compactOneSegment()` until the footprint fits or
compaction returns noOp, then returns `QueueFull`. The hard FS floor (`minFreeBytes`) is checked after the compaction loop:
`freeBytes() < minFreeBytes + appendGrowthBytes(recordSize)`, i.e. the write
is rejected if it would push remaining free space below the floor. `maxTotalBytes = 0`
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

**`canEnqueue(recordSize)`** performs only the hard FS floor check: returns false if
`freeBytes() < minFreeBytes`. It does not check `maxTotalBytes` or require free
space for the record itself -- `commitEnqueue` is authoritative for both.

**`FullQueuePolicy::DropOldest`** with `maxTotalBytes`: `Queue::enqueue()` calls
`canEnqueue()` before `commitEnqueue()`. Since `canEnqueue()` does not check
`maxTotalBytes`, a footprint-full queue returns `QueueFull` from `commitEnqueue`.
`Queue::enqueue()` handles this: if `commitEnqueue()` returns `QueueFull` and the
policy is `DropOldest`, it evicts the front record and retries `commitEnqueue()`
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

**Orphan tail after rotate-before-compact.** Rotate-before-compact leaves the
orphan tail generation numerically below the compaction output range. When the
orphan later receives live records and rotates, it forms a separate range that
cannot immediately merge with the output range. Range count stays bounded and
dead-range elimination reclaims it once popped, but the fragmentation is
inelegant.

**Configurable `kManifestMaxRanges`.** The current manifest format (30B fixed +
4x8B = 62B) fits within the LittleFS 64-byte inline threshold. For devices with
larger flash queueing megabytes of data, compaction latency scales with queue
size and can become a data-loss mechanism: if the store cannot compact fast
enough, QueueFull drops records. More ranges (e.g. 8 ranges = 94B) enable more
subrange splits and tighter per-step latency bounds at scale. Deferred: touches
the manifest binary format, requires a version bump, and expands the test matrix
significantly.

**Cost-aware compaction strategy.** Score ranges by
`bytes_reclaimed / estimated_compaction_ms`, where estimated cost is derived
from the latency model. Naturally avoids large nearly-live ranges when stall
budget is tight. Lower priority while HighestDeadRatio performs well in
practice.

**Pop-triggered compaction.** Pops currently never trigger compaction; dead data
accumulates until the next enqueue or explicit `compactIdle`. An idle-hint
callback -- fired after a drain empties the queue -- would let the application
know compaction is productive without coupling the timing to the write path.

---

## Design decisions

| Decision | Outcome |
|---|---|
| Manifest publish on rotation vs self-describing tail chain | Manifest publish on rotation. Tail chain complexity not justified by measured cost. |
| Manifest stores queue head/tail/count | No. Layout only. Head/tail/count come from replay. |
| Target segment size | 4 KB. Measured flush cost cliff at 4 KB -> 8 KB makes this the clear winner (see LittleFS timing reference below). |
| Manifest size target | <= 64 B. LittleFS inline file threshold -- staying below it saves ~7 ms per rollover. |
| Compaction journal over manifest-authority model | Rejected. With a journal over discovered segment files, the existence of a normal segment file becomes meaningful -- dangling compacted outputs can poison mount without a separate pending-intent mechanism. The "unreferenced files are garbage" invariant is lost. |
| Page/block preallocation | Rejected. LittleFS is COW with dynamic block allocation; preallocation does not deliver the expected flash-behavior wins. |
| Event frame format | Shared frame with a type field (ENQUEUE/POP/REWRITE). Exact layout is an implementation detail. |

---

## LittleFS timing reference (ESP32-S3)

Measured from prior on-device LittleFS timing runs. These are
the authoritative numbers for design decisions on the ESP32-S3. Other devices
will scale proportionally -- the ratios and the 4 KB block-size cliff hold
across LittleFS targets.

### Flush cost curve (128 B write, open/flush/close per iter)

| File size | Open (us) | Flush (us) | Total (us) | Flush w/ persistent handle |
|---|---|---|---|---|
| 512 B | 10,604 | 13,150 | 23,849 | 9,361 |
| 1 KB | 17,547 | 35,823 | 53,465 | 31,736 |
| 2 KB | 14,174 | 41,725 | 55,993 | 36,077 |
| 4 KB | 14,395 | 46,904 | 61,393 | 45,175 |
| 8 KB | 9,004 | 78,672 | 87,772 | 77,632 |
| 16 KB | 13,780 | 74,009 | 87,884 | 75,836 |
| 32 KB | 12,824 | 75,073 | 87,992 | 76,551 |

The cliff between 4 KB and 8 KB is the LittleFS block size boundary. It is
structural, not tunable.

### writeAt breakdown (512 B file, open/write/flush/close)

| Phase | Cost (us) |
|---|---|
| open | 10,844 |
| write | 60 |
| flush | 13,529 |
| close | 95 |
| total | 24,528 |

The write itself is trivially fast. Open and flush dominate.

### readAt breakdown (512 B file)

| Phase | Cost (us) |
|---|---|
| open | 4,628 |
| read | 32 |
| close | 71 |
| total | 4,732 |

### Rotation cost (20 B segment header + manifest publish, dual-manifest A/B)

| Manifest size | Seg create (us) | Manifest pub (us) | Rotation total (us) |
|---|---|---|---|
| 64 B | 19,358 | 25,732 | 45,090 |
| 128 B | 19,358 | 32,625 | 51,983 |
| 256 B | 19,358 | 31,940 | 51,298 |

The 64 B -> 128 B cost jump (~7 ms) is the LittleFS inline file threshold
(`LFS_INLINE_MAX`, default 64 B). Files <= 64 B are stored inline in the
directory entry with no separate data block allocation. The manifest is
designed to fit in 64 B to stay below this threshold.

### Amortized rollover overhead per enqueue (128 B manifest)

| Segment size | Records/seg | Amortized (us/enq) |
|---|---|---|
| 4 KB | 7 | 7,426 |
| 8 KB | 15 | 3,465 |
| 16 KB | 31 | 1,676 |
| 32 KB | 63 | 825 |

At 4 KB segments with a 64 B manifest (rollover = 45 ms), amortized overhead
= 45,090 / 7 ~ 6,441 us/enq -- about 14% of the 45 ms flush cost. Acceptable.

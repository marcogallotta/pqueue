# pqueue Append-only Design

## Purpose

pqueue is a crash-safe persistent FIFO queue for ESP32/LittleFS. The original storage backend used a fixed-size ring buffer: one spool file containing a checkpoint region, a journal region, and fixed-size record slots. Every enqueue writes the record payload into its slot and commits the new index into the journal — two `writeAt` calls per enqueue, both flushing the same large file.

On ESP32/LittleFS this is too slow to be useful. LittleFS uses a copy-on-write B-tree, and flush cost grows with file size because each flush must propagate changes through the metadata tree. A spool file sized for 20 records of 492 B is 18,688 B. Flushing it costs ~134 ms. Two flushes per enqueue puts the floor above 300 ms before accounting for open, mount, or read overhead — measurements confirm ~424 ms average.

There is no tuning path out of this. Shrinking the spool file means shrinking queue capacity. The flush cost is a property of LittleFS and the file size, not of the write size. The write itself takes 60 µs.

This document describes the append-log storage backend that replaces the fixed-slot design and brings enqueue/pop latency into an acceptable range.

## Goals

1. enqueue/pop/rewrite must be fast on ESP32/LittleFS.
2. In the default mode, every committed operation survives a power loss.
3. The queue must not exhaust flash over time.

## Design rules

1. Never flush a large file — keep tail segment size bounded.
2. Metadata writes only at the actual commit point, not as overhead.
3. Space reclaim must be incremental; never a full sweep that stalls the queue.
4. Old data stays valid until the new layout is durably committed.

## Invariants

These hold at all times. Violation is a bug, not a recoverable condition.

1. Only manifest-referenced segments are authoritative. Files on disk not named in the manifest are garbage.
2. Exactly one tail segment exists in the manifest at any time.
3. Full segments are immutable — never written to after rollover.
4. Manifest publish is the sole layout commit point. No layout change is visible to mount until a manifest is successfully written.
5. A segment generation number is never reused.
6. Tail truncation (discarding a torn write) is only legal on the manifest tail segment.
7. Corruption in a full segment is fatal — return DataCorrupt, do not attempt recovery.
8. Dangling segments are never scanned during normal mount.
9. The tail segment appears after all full ranges in logical replay order. No full range may be inserted after the tail.

## Storage model

The queue stores data in two file types: a dual-copy manifest and a set of segment files.

### Files

```text
spool/
├── manifest-a.bin        — manifest, slot A
├── manifest-b.bin        — manifest, slot B
├── seg-00000001.bin      — full segment
├── seg-00000002.bin      — full segment
└── seg-0000000c.bin      — tail segment
```

Segment generation numbers are monotonically increasing counters starting at 1, zero-padded to 8 hex digits, and never reused.

### Manifest

The manifest records the current queue layout: which segment files are referenced and which one is the tail. It contains:

```text
full segments: [10..11], [5..6], [3..4]
tail segment: generation 12
```

The non-monotonic range order reflects logical replay order after compaction — segments are listed in the order they must be scanned, not by generation number.

It does not store queue head, tail position, or count — those are derived by replaying the referenced segments on mount.

The manifest is only written on rollover and compaction. Normal enqueue, pop, and rewrite operations write only to the tail segment file.

**Size constraint:** the manifest must fit in <= 64 B to stay within the LittleFS inline file threshold and avoid the ~7 ms penalty of a separate block allocation. This is a hard constraint, not a preference.

**Binary layout** — all fields little-endian, packed (no padding), CRC32 using the IEEE polynomial (0xEDB88320). A reader encountering an unknown version returns DataCorrupt.

```
magic          4 B   — fixed sentinel
version        2 B   — format version; current = 1
headerBytes    2 B   — total manifest size in bytes
epoch          4 B   — u32, incremented on each publish
nextGeneration 4 B   — u32, next generation number to allocate
rangeCount     2 B   — number of full segment ranges
ranges         N × 8 B  (startGen u32, endGen u32)
tailGeneration 4 B   — u32
crc            4 B   — CRC32 over all preceding bytes
footer         4 B   — fixed sentinel
-----------------
fixed overhead: 30 B
room for ranges: 34 B → 4 ranges max
```

`tailGeneration` is encoded separately, not as a range entry. After each compaction publish, contiguous ranges are merged (e.g. `[10], [11], [12]` → `[10..12]`). With merging, 4 ranges is sufficient for normal operation — the range count only grows when compaction produces non-contiguous output, which is bounded by oldest-first selection. If a layout ever exceeds 4 ranges despite merging, publish is refused and a compaction pass must run first.

### Dual manifest slots

The manifest exists in two copies (slots A and B). Each carries an epoch counter. On publish, the inactive slot is overwritten with the new layout and a higher epoch — the active slot is never touched until the write is complete. On mount, each slot is validated independently by CRC and footer; only fully valid slots are considered. The valid slot with the higher epoch wins.

A crash mid-publish leaves the written slot with a bad CRC. It is discarded entirely — epoch is only compared between fully valid slots, so a partially written higher epoch cannot win. The surviving slot reflects the last successfully committed layout, and the segment files back it up. No committed data is lost.

This allows crash-safe manifest updates without depending on atomic file overwrite or rename.

## Segment model

A segment file is a sequential data blob:

```
+--------------------------------------------+
| segment header  (generation, startSeq, CRC)|
+--------------------------------------------+
| event 0         (length, type, data, CRC)  |
+--------------------------------------------+
| event 1         (length, type, data, CRC)  |
+--------------------------------------------+
| ...                                        |
+--------------------------------------------+
```

- **generation** — the segment's unique number, matching its filename (e.g. `seg-0000000c.bin` has generation 12).
- **startSeq** — the sequence number of the first event in this segment, allowing events to be positioned in the queue without scanning prior segments.

Segment files are not authoritative by existence. Mount asks what the manifest says is active, not which files happen to exist on disk.

### Segment states

Four terms describe the state of a segment:

- **tail segment** — the segment currently being appended to. There is exactly one at any time.
- **full segments** — complete segments referenced by the manifest. No longer written to.
- **referenced segments** — all segments in the manifest: full segments plus the tail segment.
- **dangling segments** — not referenced by the manifest. Ignored on mount; cleaned up lazily.

### Crash-safety invariant

Because segment authority lies in the manifest, compaction can write new output segments freely before committing. A crash before the manifest publish leaves the new segments dangling — they are ignored on the next mount with no effect on the active layout:

- current manifest: active = 1, 2, 3
- compaction writes new segment files 10, 11
- crash before manifest publish
- mount reads manifest: active = 1, 2, 3 — segments 10, 11 are ignored

After a successful publish:

- new manifest: active = 10, 11, 3
- mount scans 10, 11, 3
- old segments 1, 2 are dangling and ignored

## Rollover

Rollover occurs when the tail segment reaches `maxSegmentBytes`. The current tail becomes a full segment and a new tail takes its place:

1. Allocate the next generation number.
2. Create the new segment file and write its header (generation, startSeq, CRC).
3. Flush and close the new segment file.
4. Publish the manifest naming the new generation as the tail.

The new segment is written and flushed before the manifest is published. A crash between steps 3 and 4 leaves the new empty segment dangling — it is ignored on the next mount and cleaned up lazily. The old tail segment remains the tail.

Rollover cost has been measured on ESP32-S3 (see Appendix B). At 4 KB segments the total rollover cost is ~45 ms, amortized to ~6 ms per enqueue.

### Segment sizing

Measured on ESP32-S3 with LittleFS (see Appendix B). The flush cost curve has a hard cliff at the LittleFS block size boundary:

```
4 KB segment flush:   ~45 ms
8 KB segment flush:   ~78 ms   (+73%)
16 KB segment flush:  ~74 ms
```

The 4→8 KB jump is structural: LittleFS allocates blocks of 4096 B. A 4 KB file fits in one block; an 8 KB file needs two, which doubles the metadata tree work per flush. The ratio holds across LittleFS targets — absolute times will differ on other devices.

For 492 B records (516 B per event with overhead), effective average enqueue cost on ESP32-S3:

| Segment | Flush/enq | Recs/seg | Amort rollover | Effective avg |
|---------|-----------|----------|----------------|---------------|
| 4 KB    | ~45 ms    | 7        | ~6 ms          | **~51 ms**    |
| 8 KB    | ~78 ms    | 15       | ~3 ms          | ~81 ms        |
| 16 KB   | ~74 ms    | 31       | ~2 ms          | ~76 ms        |

4 KB segments have the lowest effective average cost on this device. Larger segments reduce rollover frequency but cost far more per flush. The rollover overhead at 4 KB is ~6–7 ms amortized per enqueue (~13% of the 45 ms flush cost), which is acceptable.

`maxSegmentBytes` must remain a config knob. Devices with slower flash or different block sizes will have different optimal values, but 4 KB is a good default for standard LittleFS configurations.

## Incremental compaction design

A full compaction pass risks multi-second latency spikes, excess flash wear, copying mostly-live data unnecessarily, and user-visible stalls. Compaction is therefore incremental by default — one full segment per call. The design goal is predictable bounded compaction, not globally optimal space reclaim.

Two APIs are provided:

- `compactOneSegment()` — compacts one segment and returns. Cost is bounded and predictable (~71 ms on ESP32-S3). Caller controls how often to invoke it.
- `compactFull()` — loops `compactOneSegment()` until no useful work remains. Intended for idle time or explicit maintenance (see open problem: compaction no-op loop).

### Useful compaction only

A compaction step runs only when useful — at least one of:

- a full segment can be removed from the layout
- dead bytes exceed threshold
- segment count pressure exists
- free-space pressure exists

Compacting mostly-live data just to make the log look clean is not useful.

## Compaction step

`compactOneSegment()`:

1. Choose the oldest full segment. Skip if its dead bytes are below a configurable threshold (a config knob alongside `maxSegmentBytes`).
2. Collect live records from that segment.
3. Build one compacted output segment buffer in RAM, sized to `maxSegmentBytes`. The buffer should be statically allocated — a dynamic allocation at compaction time may fail if the heap is fragmented by Wi-Fi, BLE, or TLS stacks. Validate during implementation that the static reservation fits alongside the live firmware footprint.
4. Write and verify the output segment file.
5. Publish new manifest replacing old segment/range with new segment/range.
6. Update RAM to match manifest.
7. Leave old files on disk — cleanup is separate.

## Mount model

Mount:

1. Read both manifest files.
2. Validate each slot (CRC + footer); pick the valid slot with the higher epoch. If neither slot is valid, return DataCorrupt.
3. Read only segments referenced by the manifest/layout.
4. Ignore all other segment files.
5. If a referenced segment is missing or corrupt in a non-recoverable way, return DataCorrupt.
6. If the tail segment has a torn write*, truncate/discard only where policy says safe.
7. Rebuild RAM live records from referenced segments.

This avoids the “do random files on disk imply active data?” ambiguity.

*A torn write is a partially written event at the end of the tail segment — the write was interrupted (e.g. power loss) before the full event, CRC, and footer were flushed. The event frame is incomplete and cannot be validated. This is distinct from a corrupt event, which is a fully written event with a bad CRC. Torn writes are only tolerated on the tail segment; corruption in a full segment is DataCorrupt.

## Cleanup model

After a successful manifest publish, superseded segment files become dangling — not referenced by the manifest and safe to delete.

Cleanup is not part of the compaction commit path. It runs incrementally outside the hot path: one file per idle window, one per startup, or one per explicit maintenance call.

Under low free space, cleanup runs eagerly before compaction or enqueue. It deletes only unreferenced files. If no garbage exists, enqueue fails.

Cleanup is idempotent: deletion failures are retried later, a crash during deletion leaves mount unaffected (unreferenced files are simply ignored), and referenced files are never deleted.

## Performance analysis framework

Estimated costs for each operation under the new design. All timings are from measurements on an ESP32-S3 (see Appendix B); other devices will differ.

| Operation | `writeAt`s | Manifest update | Est. cost |
|-----------|------------|-----------------|-----------|
| Enqueue (no rollover) | 1 | No | ~45 ms |
| Pop | 1 | No | ~45 ms |
| Rewrite front | 1 | No | ~45 ms |
| Rotation | 1 (header only) | Yes | ~45 ms total |
| Compaction step | 0 or 1 | Yes | ~26–71 ms |
| Cleanup | 1 (delete) | No | outside hot path |

Rotation cost is amortized over 7 records at 4 KB default — ~6.4 ms per enqueue overhead (~14% of normal enqueue cost).

Compaction step cost is ~71 ms when the selected segment has live records (one output segment write + one manifest publish), or ~26 ms for a fully dead segment (manifest publish only).

## Future work

Items are ordered by priority. The first three are required before a stable release.

### 1. Compaction strategy simulator *(required before stable release)*

Before redesigning the compaction strategy, data is needed. The space of possible algorithms is large, the right tradeoffs depend on workload, and the queue is intended for general use — not one known use case. A Posix-backed simulator drives the queue through configurable enqueue/pop patterns (rate, burst size, record size distribution) and evaluates candidate algorithms against a sweep of workload parameters. See `docs/pqueue-compaction-strategy.md` for the full simulator design, metrics, and candidate algorithm list. The best simulator candidates are then validated on-device to confirm real LittleFS costs match the simulator model.

### 2. Multi-segment compaction output *(required before stable release)*

v1 compaction writes a single output segment per call and returns no-op if live bytes exceed `maxSegmentBytes`. This creates a permanent dead-end: if the oldest range has more live bytes than fit in one segment, compaction never makes progress on it. Rollovers keep adding ranges until the manifest hits its 4-range limit and the queue deadlocks. The fix is allowing compaction to write multiple output segments per call, so it can always make progress regardless of range size. The manifest model already supports arbitrary range spans and non-monotonic generation ordering, so the publish semantics survive unchanged — the change is in the planner and RAM staging. The simulator (item 1) informs how to design this well.

### 3. Global compaction strategy *(required before stable release)*

The v1 compactor is greedy and local — always picks the oldest full range with no view of the others. This produces correct results but wastes flash: oldest-first ignores dead-byte ratios, and a newly compacted live range immediately becomes the oldest again, causing the next call to recompact it uselessly. The correct long-term direction is a two-phase model: an analysis pass that inspects all segment files across all ranges, scores them by reclaim efficiency (dead bytes reclaimed per live byte copied), and produces a prioritised compaction plan; an execution pass that acts on that plan. This also enables multi-input compaction — combining live records from multiple sparse ranges into fewer output segments — which v1 cannot do. The simulator (item 1) picks the winning strategy.

### 4. Dead-byte threshold

A quick interim improvement: skip ranges whose dead-byte ratio falls below a configurable threshold, with overrides for segment-count pressure and low free space. This prevents the most wasteful recompactions cheaply and can be wired in before or alongside the simulator work. It does not solve the global strategy problem but reduces churn while the full redesign is underway.

### 5. Deep invariant validator

As the compactor becomes more complex, debugging subtle ordering or sequence bugs becomes harder. A `validateDeep()` function that rescans every referenced segment and verifies manifest ordering, sequence monotonicity, no duplicate live sequences, tail uniqueness, range ordering, and no referenced-dangling overlap would be a valuable diagnostic tool. Not part of the normal mount path — a separate maintenance or debug call.

### 6. RAM write buffer

All operations currently write directly to flash. A RAM buffer sitting in front of disk ops would allow enqueue/pop/rewrite to return without a flush, batching writes and reducing per-operation flash cost. Deferred: adding a buffer makes durability semantics fuzzy, changes replay assumptions, and significantly increases testing complexity. The current ~45 ms durable write is already a large improvement over the original ~400 ms. Stabilise the storage engine and compaction strategy first.

### 7. Lazy tail segment creation

Rollover currently pre-creates the next tail segment before manifest publish, guaranteeing a referenced segment always exists on disk. An alternative is to create the tail lazily on first append, saving the flush of an empty segment. Deferred: the complexity cost to mount semantics and the "tail always exists" invariant is not justified by the speculative gain. Worth revisiting only if profiling shows it matters.

### 8. Compaction generations / levels

Once multi-input, multi-output compaction exists it may become useful to tag segments with compaction level metadata (original vs. compacted, level depth) to support hot/cold segregation or age-aware policies. Not needed now and not soon — the 4-range manifest constraint limits how far tiered compaction could go anyway. Revisit only if the simulator reveals a workload where level-aware policy consistently outperforms simpler strategies.

## Appendix A: Decisions log

| Decision | Outcome |
|----------|---------|
| Manifest publish on rotation vs self-describing tail chain | Manifest publish on rotation. Tail chain complexity not justified by measured cost. |
| Manifest stores queue head/tail/count | No. Layout only. Head/tail/count come from replay. |
| Target segment size | **4 KB.** Measured flush cost cliff at 4→8 KB makes this the clear winner. |
| Manifest size target | **<= 64 B.** LittleFS inline file threshold — stays below it saves ~7 ms per rollover. |
| Compaction journal over manifest-authority model | Rejected. With a journal over discovered segment files, the existence of a normal segment file becomes meaningful — dangling compacted outputs can poison mount without a separate pending-intent mechanism. The "unreferenced files are garbage" invariant is lost. |
| Page/block preallocation | Rejected. LittleFS is COW with dynamic block allocation; preallocation does not deliver the expected flash-behavior wins. |
| Event frame format | Shared frame with a type field (ENQUEUE/POP/REWRITE). Exact layout is an implementation detail. |

## Appendix B: LittleFS measurements (ESP32-S3)

Measured with the on-device profiler (`pqueue_profiling_main.cpp`). These are the authoritative numbers for design decisions on the ESP32-S3. Other devices will scale proportionally — the ratios and the 4 KB block-size cliff hold across LittleFS targets.

### Flush cost curve (128 B write, open/flush/close per iter)

| File size | Open (µs) | Flush (µs) | Total (µs) | Flush w/ persistent handle |
|-----------|-----------|------------|------------|---------------------------|
| 512 B     | 10,604    | 13,150     | 23,849     | 9,361                     |
| 1 KB      | 17,547    | 35,823     | 53,465     | 31,736                    |
| 2 KB      | 14,174    | 41,725     | 55,993     | 36,077                    |
| 4 KB      | 14,395    | 46,904     | 61,393     | 45,175                    |
| 8 KB      | 9,004     | 78,672     | 87,772     | 77,632                    |
| 16 KB     | 13,780    | 74,009     | 87,884     | 75,836                    |
| 32 KB     | 12,824    | 75,073     | 87,992     | 76,551                    |

The cliff between 4 KB and 8 KB is the LittleFS block size boundary. It is structural, not tunable.

### writeAt breakdown (512 B file, open/write/flush/close)

| Phase | Cost (µs) |
|-------|-----------|
| open  | 10,844    |
| write | 60        |
| flush | 13,529    |
| close | 95        |
| total | 24,528    |

The write itself is trivially fast. Open and flush dominate.

### readAt breakdown (512 B file)

| Phase | Cost (µs) |
|-------|-----------|
| open  | 4,628     |
| read  | 32        |
| close | 71        |
| total | 4,732     |

### Rotation cost (20 B segment header + manifest publish, dual-manifest A/B)

| Manifest size | Seg create (µs) | Manifest pub (µs) | Rotation total (µs) |
|-----------|-----------------|---------------|---------------------|
| 64 B      | 19,358          | 25,732        | 45,090              |
| 128 B     | 19,358          | 32,625        | 51,983              |
| 256 B     | 19,358          | 31,940        | 51,298              |

The 64 B → 128 B cost jump (~7 ms) is the LittleFS inline file threshold (`LFS_INLINE_MAX`, default 64 B). Files <= 64 B are stored inline in the directory entry with no separate data block allocation. **Design the manifest to fit in 64 B** to stay below this threshold.

### Amortized rollover overhead per enqueue (128 B manifest)

| Segment size | Records/seg | Amortized (µs/enq) |
|--------------|-------------|--------------------|
| 4 KB         | 7           | 7,426              |
| 8 KB         | 15          | 3,465              |
| 16 KB        | 31          | 1,676              |
| 32 KB        | 63          | 825                |

At 4 KB segments with a 64 B manifest (rollover = 45 ms), amortized overhead = 45,090 / 7 ~ 6,441 µs/enq — about 14% of the 45 ms flush cost. Acceptable.

# pqueue Append-only Design

## Purpose

The original pqueue storage backend used a fixed-size ring buffer: one spool file containing a checkpoint region, a journal region, and fixed-size record slots. Every enqueue writes the record payload into its slot and commits the new index into the journal — two `writeAt` calls per enqueue, both flushing the same large file.

On ESP32/LittleFS this is a dead end. LittleFS uses a copy-on-write B-tree, and flush cost grows with file size because each flush must propagate changes through the metadata tree. A spool file sized for 20 records of 492 B is 18,688 B. Flushing it costs ~134 ms. Two flushes per enqueue puts the floor above 300 ms before accounting for open, mount, or read overhead — and measurements confirm ~424 ms average.

There is no tuning path out of this. Shrinking the spool file means shrinking queue capacity. The flush cost is a property of LittleFS and the file size, not of the write size. The write itself takes 60 µs.

This document describes the append-log storage design that replaces the fixed-slot backend.

## Core constraints and objectives

1. Build append-log compaction that is crash-safe without turning into a write-amplification machine.
2. Normal enqueue/pop/rewrite must stay fast; compaction must not add hot-path writes unnecessarily.
3. Flash writes and flushes are expensive, so avoid metadata writes unless they are the actual commit point.
4. New compacted segment files can be written as dangling data; they become active only through an authoritative layout update.
5. Recovery should trust authoritative layout state, not blindly scan every segment file as active.
6. Compaction should be incremental and budgeted, not a large surprise sweep.
7. Do not write copied records one-by-one; batch into full-ish segment buffers.
8. Do not compact unless it meaningfully reduces dead data, active segment count, latency risk, or space pressure.
9. Old data must remain valid until the new layout is durably published.
10. Minimize flash wear while preserving clear fail-safe recovery semantics.

## Storage model

The design is:

```text
dual-copy authoritative root
+ append-only data segment files
+ incremental compaction
+ lazy/idempotent cleanup
```

### Files

```text
pqueue-root-a.bin
pqueue-root-b.bin
pqueue-seg-XXXXXXXX.bin
```

No temp or pending files. The root/commit model handles crash safety without them.

### Root

The root is the authority on which segment files are active. Recovery reads the root first and scans only the segments it references. A segment file not referenced by the root is unreferenced garbage — it is never treated as active merely because it exists on disk.

The root encodes active segments as ordered ranges:

```text
sealed active ranges: [10..11], [5..6], [3..4]
open tail: generation 12
```

The root does not store queue head/tail/count. Those come from replaying the referenced segments. Storing them in the root would create two sources of truth that can disagree after a crash.

### Dual root

LittleFS atomic rename under power loss is not verified. The root uses two slots:

```text
pqueue-root-a.bin
pqueue-root-b.bin
```

On publish: write the inactive slot with a higher epoch, flush, close. Recovery reads both, validates CRC and footer, picks the highest valid epoch. This avoids depending on overwrite atomicity.

## Segment model

Segments should be dumb data blobs.

A segment file contains:

```text
segment header
framed events / compacted records
crc/footer per record/event
```

Segment files are not authoritative by existence.

Recovery should ask:

```text
What does the root say is active?
```

not:

```text
Which segment files exist?
```

Unreferenced segment files are ignored.

This gives the intended crash-safe compaction behavior:

```text
current root: active = 1,2,3
compaction writes new segment files 10,11
crash before root publish
recovery reads root: active = 1,2,3
segments 10,11 are ignored
```

After publish:

```text
new root: active = 10,11,3
recovery scans 10,11,3
old 1,2 are ignored
```

## Rotation

Rotation happens when the active segment is full. On rotation:

```text
create next segment (write 20 B header, flush, close)
publish root with new active tail
```

On compaction:

```text
write compacted output segments
publish root replacing old active range with new range
```

The root is not updated on every enqueue — only on rotation and compaction. Normal append operations write only to the active segment.

Rotation cost has been measured (see measurements section). At 4 KB segments the rotation overhead is ~6 ms amortized per enqueue, which is acceptable.

### Segment sizing — RESOLVED: 4 KB default

Measured on ESP32-S3 with LittleFS (see measurements section). The flush cost curve has a hard cliff at the LittleFS block size boundary:

```
4 KB segment flush:   ~45 ms
8 KB segment flush:   ~78 ms   (+73%)
16 KB segment flush:  ~74 ms
```

The 4→8 KB jump is structural: LittleFS allocates blocks of 4096 B. A 4 KB file fits in one block; an 8 KB file needs two, which doubles the metadata tree work per flush. This ratio is device-independent — the absolute times scale with flash speed, but the block size boundary stays at 4 KB on all standard LittleFS configurations.

For 492 B records (516 B per event with overhead), effective average enqueue cost:

| Segment | Flush/enq | Recs/seg | Amort rotation | Effective avg |
|---------|-----------|----------|----------------|---------------|
| 4 KB    | ~45 ms    | 7        | ~6 ms          | **~51 ms**    |
| 8 KB    | ~78 ms    | 15       | ~3 ms          | ~81 ms        |
| 16 KB   | ~74 ms    | 31       | ~2 ms          | ~76 ms        |

**4 KB segments win.** Larger segments reduce rotation frequency but cost far more per flush. The rotation overhead at 4 KB is ~6–7 ms amortized per enqueue (~13% of the 45 ms flush cost), which is acceptable. The self-describing tail chain fallback is not needed.

`maxSegmentBytes` must remain a config knob. Devices with slower flash or different block sizes will have different optimal values, but 4 KB is the right default.

## Rejected directions

**Replacement journal over discovered segment files.** An earlier implementation used a compaction journal mapping `oldStart..oldEnd -> newStart..newEnd`, with recovery scanning segment files on disk and applying journal replacements. Rejected: the existence of a normal segment file becomes meaningful, so dangling compacted outputs can poison recovery without a separate pending-intent mechanism. Recovery becomes a mix of directory inference and journal interpretation, and the "unreferenced files are garbage" invariant is lost.

**Self-describing tail chain.** Rotation avoids a root write by embedding chain linkage in each segment header. Rejected for this design: measurement shows rotation cost at 4 KB segments is ~6 ms amortized per enqueue, which is acceptable. The added recovery complexity is not justified.

**Page/block preallocation.** Preallocate segment regions to write aligned full blocks. LittleFS is COW with dynamic block allocation, so preallocation does not deliver the expected flash-behavior wins.

## Incremental compaction design

Compaction should not be a full sweep.

Production compaction should be something like:

```text
compactStep(budget)
```

or:

```text
compactOneOldSegment()
```

It should process at most:

```text
one sealed old segment
or one small bounded logical range
or M bytes
or T milliseconds
```

Do not start with “compact all old segments.”

### Why

A full compaction pass risks:

- multi-second latency spikes;
- excess flash wear;
- copying mostly-live data unnecessarily;
- complex crash windows;
- user-visible stalls.

### Useful compaction only

A compaction step should run only if it is expected to do something useful:

```text
old active segment can be removed from active layout
dead bytes exceed threshold
segment count pressure exists
free-space pressure exists
```

Avoid compacting mostly-live data just to make the log look clean.

### Writer behavior

The compaction writer should:

1. Read old sealed segment(s).
2. Identify live records using current RAM/live index state.
3. Copy only live payloads into a RAM output buffer.
4. Write a new compacted segment file as one full-ish buffer.
5. Repeat only within budget.
6. Publish one root/layout update.

It must not:

```text
write one copied record
flush
write next copied record
flush
...
```

That would recreate the LittleFS problem.

## Compaction step

```text
compactStep():
  choose one sealed old segment/range worth compacting
  collect live records from that old segment/range
  if not useful: return no-op
  build one or more compacted output segment buffers in RAM
  write output segment file(s)
  verify written segment file(s)
  publish new root replacing old segment/range with new segment/range
  update RAM to match root
  leave old files on disk (cleanup is separate)
```

Write budget per step:

```text
N full segment writes (~45 ms each at 4 KB)
+ 1 root publish (~26 ms for <=64 B root)
+ 0 per-record flushes
+ 0 old deletes
```

## Recovery model

Recovery should be simple:

1. Read both root files.
2. Pick newest valid root by epoch + CRC + footer.
3. Read only segments referenced by the root/layout or reachable from the root-defined tail model.
4. Ignore all other segment files.
5. If a referenced segment is missing or corrupt in a non-recoverable way, return DataCorrupt.
6. If the open tail has a torn tail, truncate/discard only where policy says safe.
7. Rebuild RAM live records from referenced active segments.

This avoids the “do random files on disk imply active data?” ambiguity.

## Cleanup model

Cleanup should not be part of the compaction commit path.

After a successful root publish, old files become garbage.

Cleanup can delete unreferenced files:

```text
one file per idle window
one file per startup
one file per maintenance call
```

Cleanup must be idempotent:

- if deletion fails, try later;
- if crash happens during deletion, recovery still uses root and ignores missing unreferenced files;
- never delete referenced active files.

## LittleFS measurements (ESP32-S3)

Measured with the on-device profiler (`pqueue_profiling_main.cpp`). These are the authoritative numbers for design decisions. Other devices will scale proportionally — the ratios and the 4 KB block-size cliff hold across LittleFS targets.

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

### writeAt on 18,688 B spool file (fixed-slot reference)

Flush: 133,685 µs. Total: 147,610 µs per writeAt. Fixed-slot enqueue does 2 writeAts → ~295 ms from writes alone, explaining the ~424 ms observed enqueue average.

### readAt breakdown (512 B file)

| Phase | Cost (µs) |
|-------|-----------|
| open  | 4,628     |
| read  | 32        |
| close | 71        |
| total | 4,732     |

### Rotation cost (20 B segment header + root publish, dual-root A/B)

| Root size | Seg create (µs) | Root pub (µs) | Rotation total (µs) |
|-----------|-----------------|---------------|---------------------|
| 64 B      | 19,358          | 25,732        | 45,090              |
| 128 B     | 19,358          | 32,625        | 51,983              |
| 256 B     | 19,358          | 31,940        | 51,298              |

The 64 B → 128 B cost jump (~7 ms) is the LittleFS inline file threshold (`LFS_INLINE_MAX`, default 64 B). Files <= 64 B are stored inline in the directory entry with no separate data block allocation. **Design the root to fit in 64 B** to stay below this threshold.

### Amortized rotation overhead per enqueue (128 B root)

| Segment size | Records/seg | Amortized (µs/enq) |
|--------------|-------------|--------------------|
| 4 KB         | 7           | 7,426              |
| 8 KB         | 15          | 3,465              |
| 16 KB        | 31          | 1,676              |
| 32 KB        | 63          | 825                |

At 4 KB segments with a 64 B root (rotation = 45 ms), amortized overhead = 45,090 / 7 ~ 6,441 µs/enq — about 14% of the 45 ms flush cost. Acceptable.

## Performance analysis framework

Measured costs for each operation under the new design.

### Enqueue, no rotation

```text
1 writeAt to current segment (~45 ms at 4 KB segment)
```

No root update. This is the hot-path cost. At 4 KB segments the active file stays within one LittleFS block, keeping flush cost at ~45 ms.

### Pop

```text
1 writeAt POP event to current segment (~45 ms)
```

No root update. Same cost as enqueue.

### Rewrite front

```text
1 writeAt REWRITE event to current segment (~45 ms)
```

No root update.

### Rotation

```text
1 writeFile new segment header (20 B) — ~19 ms
1 root publish (<=64 B)            — ~26 ms
total                              — ~45 ms
```

Amortized over 7 records at 4 KB default = ~6.4 ms per enqueue overhead (~14% of normal enqueue cost).

### Compaction step

```text
N full compacted segment writes (~45 ms each at 4 KB)
1 root publish (<=64 B)            — ~26 ms
0 per-record flushes
0 old deletes
```

N is typically 1. Write budget per compaction step: ~71 ms for a single output segment.

### Cleanup

```text
delete one or few unreferenced files
outside hot path
```

Cleanup should not block enqueue unless free space pressure forces it.

## Flash wear principles

Do not compact unless useful.

Avoid:

- copying mostly-live segments;
- repeated compaction of the same data;
- full sweeps;
- per-record writes;
- immediate deletion churn;
- metadata writes that do not serve as a commit point.

Prefer:

- one full-buffer write per new compacted segment;
- one root publish as the commit;
- lazy cleanup;
- wear spread naturally by fresh generations;
- budgeted maintenance.

## Open problems

### Root format — partially resolved

**Size constraint:** root must fit in <= 64 B to stay within the LittleFS inline file threshold and avoid the ~7 ms penalty of a separate block allocation. This is a hard constraint, not a preference.

**Content resolved:** root stores layout only — no queue head/tail/count (those come from replay). Must include: magic, version, epoch, nextGeneration, active ranges, tail generation, CRC.

**Still open:** exact binary layout. Sketch fitting in 64 B:

```
magic         4 B
version       2 B
headerBytes   2 B
epoch         4 B
nextGeneration 4 B
rangeCount    2 B
ranges        N × 8 B  (startGen u32, endGen u32)
tailGeneration 4 B
crc           4 B
footer        4 B
-----------------
fixed overhead: 30 B
room for ranges: 34 B → 4 ranges max
```

4 active ranges is sufficient for normal operation (sealed compacted ranges + open tail). If a layout ever exceeds 4 ranges, a larger root is needed — this should be treated as a configuration or compaction policy constraint, not a format error.

**Still open:** whether tailGeneration is encoded separately or as part of ranges; whether a sealed marker is needed in the root or implied by not being tailGeneration.

### Open tail framing

Under the default model (root publish on rotation), the segment that is currently being appended to is "open" between rotations. Recovery must handle a torn write at the end of that segment. Need to define:

- segment header format (magic, generation, baseSequence, CRC of header);
- per-event framing (length, payload, per-event CRC);
- truncation rule on first bad CRC (truncate the open segment to the last good event boundary);
- whether the segment carries a "sealed" footer written at rotation time, or whether sealedness is implied by no-longer-being-the-tail-in-the-root.

This is the actual hard problem behind rotation, and it must be specified before code.

### Compaction selection policy

Need a first simple policy:

- one oldest sealed segment with enough dead bytes;
- or oldest safe range;
- never logical last segment;
- skip mostly-live segments;
- budget by bytes/time/files.

### Memory budget for compaction

Full-buffer writes require RAM. Still to decide:

- max compaction output buffer size;
- whether 4 KB fits on ESP32 alongside everything else;
- whether to stream into one segment buffer or allow multi-segment output;
- fallback behavior if RAM is unavailable.

### Cleanup under low free space

If cleanup is lazy, what happens when free space is low?

Possible answer:

- cleanup can run before compaction/enqueue when necessary;
- but it must delete only unreferenced files;
- if no garbage exists, enqueue may fail.

### Validation and repair

Normal mount should not salvage aggressively.

Repair tool may later:

- scan dangling files;
- recover from root corruption;
- inspect segment chains;
- rebuild root if possible.

But normal mount should be strict and simple.

## Decisions log

| Decision | Outcome |
|----------|---------|
| Root publish on rotation vs self-describing tail chain | Root publish on rotation. Tail chain complexity not justified by measured cost. |
| Root stores queue head/tail/count | No. Layout only. Head/tail/count come from replay. |
| Target segment size | **4 KB.** Measured flush cost cliff at 4→8 KB makes this the clear winner. |
| Root size target | **<= 64 B.** LittleFS inline file threshold — stays below it saves ~7 ms per rotation. |

Open, in priority order:

1. Open-tail framing: segment header format, per-event CRC, truncation rule, sealed marker.
2. Exact root binary layout fitting in 64 B (see root format section).
3. RAM budget for the compaction output buffer.
4. Compaction operates only on sealed segments? (Strong default: yes.)
5. Cleanup runs automatically or only under explicit maintenance / free-space pressure?
6. Is dual-root mandatory, or can LittleFS atomic rename be trusted after a targeted crash test?

## Recommended next step

Segment size (4 KB) and root size target (<= 64 B) are resolved by measurement.

Next task: specify the open-tail framing.

```text
Define segment header format (magic, generation, baseSequence, header CRC).
Define per-event framing (length prefix or fixed header, payload, per-event CRC, footer).
Define truncation rule: on first bad event in the open tail segment, truncate to last good offset.
Decide: sealed marker written at rotation time, or sealedness implied by not being tailGeneration in root?
```

Then prototype enqueue / pop / rotation / recovery without compaction and validate crash behavior. Only after that, implement compaction.

## Design warning

A design can be crash-safe and still bad if it creates too many writes.

The append-log backend is justified by performance. Therefore every safety mechanism must be judged by:

```text
What extra writes does it add?
Is this write the actual commit point?
Can it be delayed?
Can it be batched?
Can it be avoided by recovery semantics?
```

If a write does not serve as data write, root commit, or necessary cleanup, it is suspect.


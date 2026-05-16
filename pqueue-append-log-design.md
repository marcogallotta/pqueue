# pqueue Append-only Design

## Purpose

The original storage used for pqueue was a fixed-sized ring buffer. We 


This is a fresh design handoff for pqueue append-log compaction and layout management.

It intentionally steps back from the current working design/doc and asks:

> If the goal is a very efficient, crash-safe append-log queue on ESP32/LittleFS, how should the storage model be designed?

This document includes my proposed design ideas, clearly marked as such, plus alternatives and open problems. It should be used as a starting point for a new design track, not as a final implementation spec.

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

## The key design correction

The current generated design direction drifted toward a replacement journal model:

```text
oldStart..oldEnd -> newStart..newEnd
```

That model can work, but it is not the cleanest expression of the intended design.

The intended design is closer to this:

```text
The authoritative layout/root decides which segment files are active.
Segment files are just data blobs.
Any segment file not referenced by the layout/root is dangling garbage.
Compaction writes new dangling segment files first.
Then one authoritative layout/root publish makes them active.
```

This means recovery should not treat every `pqueue-seg-XXXXXXXX.bin` file as active merely because it exists.

## My recommended top-level model

My recommended model is:

```text
dual-copy authoritative layout/root
+ append-only data segment files
+ incremental compaction
+ lazy/idempotent cleanup
```

### Files

Possible file classes:

```text
pqueue-root-a.bin
pqueue-root-b.bin
pqueue-seg-XXXXXXXX.bin
```

I would avoid temp or pending files unless the root/commit model genuinely cannot handle a crash window.

### Root/layout file

The root/layout file is the authority.

It should include at least:

```text
magic
version
headerBytes
epoch / generation
active sealed layout
open tail information
next segment generation
queue head/tail summary if useful
crc
footer
```

The root may encode active segments as ordered ranges instead of listing every segment individually.

Example logical layout:

```text
sealed active ranges:
  [10..11]
  [5..6]
  [3..4]

open tail:
  generation 12
```

This is still a full logical active layout, but compact on disk.

### Dual root

Unless LittleFS atomic replace/rename is proven safe enough for power loss, I recommend dual root files:

```text
pqueue-root-a.bin
pqueue-root-b.bin
```

On publish:

1. Write the inactive root slot with a higher epoch.
2. Flush/close.
3. Recovery later picks the highest valid root by epoch + CRC + footer.

This costs one small root write per root publish, but avoids relying on uncertain overwrite semantics.

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

## Rotation cost

A fully authoritative root requires a root update when the active layout changes, and rotation changes the active layout. So naive full-root publishes one small root write per rotation.

This is only painful if rotation is frequent. The rotation cost has been measured on ESP32-S3 with LittleFS (see measurements below). The default model is therefore: **publish the root on rotation**. A self-describing tail chain is kept as a fallback if measurement shows rotation cost is genuinely unacceptable, not as a co-equal design path.

## Default design: full root, updated on rotation

### Shape

Every time a new segment becomes active, update the root.

Normal operation:

```text
append to current segment
if segment full:
  create next segment
  publish root with new active tail
```

Compaction:

```text
write compacted output segments
publish root replacing old active range with new range
```

### Pros

- Simple.
- Recovery is straightforward.
- No directory inference.
- Dangling files are always safe.
- One authority.

### Cons

- One extra root write per segment rotation.
- If segments are small, this may be too expensive.

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

### Why this is the default

It is the cleanest model, recovery is trivial, and the rotation cost is controlled by a single knob (segment size). Any alternative must beat it on measured cost, not on intuition.

## Fallback design: root + self-describing tail chain

Only consider this if measurement on target hardware shows the default's root-publish-on-rotation cost is genuinely unacceptable and segment sizing cannot bring it into budget.

### Shape

The root records:

```text
sealed active ranges
current tail generation
tail epoch / chain id
```

Normal append writes to current tail.

When rotating from tail N to N+1, the new segment contains enough linkage to be discovered from the previous tail or root.

Possible segment header fields:

```text
generation
previousGeneration
chainEpoch
baseSequence
state/open/sealed maybe
crc
```

Recovery starts from root and follows a tail chain.

### Trade-offs

Avoids root write on every rotation. In exchange:

- Recovery is significantly more complex.
- Must define how to distinguish a fully committed rotated tail from a partially created one.
- Needs a footer/commit marker in segment header or first event.
- Tail chain corruption rules become subtle.
- Requires careful crash tests.

The complexity is real and the bugs that come from it are the kind that show up months later in the field. Do not adopt this without a measured cost that justifies it.

## Rejected directions

The following directions were considered and discarded.

**Replacement journal over discovered segment files.** Scan segment files, read compaction journal records, apply replacements. This is the earlier generated design. It is rejected because the existence of a normal segment file becomes meaningful, so dangling compacted outputs can poison recovery unless special rules are added. Recovery becomes a mix of directory inference and journal interpretation, drifting away from the "unreferenced files are garbage" invariant. Not worth the ambiguity.

**Page/block preallocation.** Preallocate segment regions or files and write aligned full blocks. LittleFS is COW with dynamic block allocation, so preallocation does not deliver the flash-behavior wins it promises. Discarded.

## Recommended design direction

The design path is:

1. Adopt an authoritative root/layout model.
2. Treat all unreferenced segment files as garbage.
3. Use dual-root files with epoch + CRC unless LittleFS atomic replace is proven.
4. Do not use pending-intent files unless a specific crash window remains unsolved.
5. Make compaction incremental and budgeted.
6. Make the compaction writer build full segment buffers in RAM and write each new segment once.
7. Publish the root on rotation. Tune segment size so rotation is rare enough that this cost is invisible.
8. Hold the self-describing tail chain in reserve as a fallback only if measurement on target hardware proves rotation cost unacceptable.

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

## Proposed compaction step

Assuming an authoritative root exists:

```text
compactStep():
  choose one sealed old segment/range worth compacting
  collect live records from that old segment/range
  if not useful: return no-op
  build one or more compacted output segment buffers in RAM
  write output segment file(s)
  verify written segment file(s) cheaply
  publish new root/layout replacing old segment/range with new segment/range
  update RAM to match root
  leave old files on disk
```

Write budget:

```text
N full segment writes
+ 1 small root publish
+ 0 per-record writes
+ 0 pending-intent writes unless unavoidable
+ 0 old deletes in compaction path
```

Cleanup:

```text
later:
  delete unreferenced files gradually
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

## Root format sketch

This is only a sketch.

```text
RootHeader:
  magic
  version
  headerBytes
  epoch
  rootBytes
  flags
  nextGeneration
  layoutCrc
  footer

Layout:
  active range count
  repeated ordered active ranges:
    startGeneration
    endGeneration
  tail info:
    tailGeneration
    tailMode / chainEpoch / maybe previous pointer
  queue summary:
    headSequence
    tailSequence
    maybe count
```

Question: should the root store queue head/tail/count?

Decision: **no**. Replay of referenced segments is the single source of truth for queue head/tail/count. Storing them in the root creates two sources of truth that can disagree after a crash (root committed but a tail event did not, or vice versa). The root carries layout only. If a debug/observability summary is useful later, it can be added as advisory data with an explicit "not authoritative" flag, but not in the first version.

## Atomicity model

### If LittleFS atomic rename/replace is trusted

Root publish could be:

```text
write pqueue-root-new.bin
rename over pqueue-root.bin
```

But power-loss atomicity must be verified, not assumed.

### Safer model

Dual-root:

```text
pqueue-root-a.bin
pqueue-root-b.bin
```

Publish:

```text
choose older/inactive root slot
write complete new root with epoch+crc+footer
flush/close
```

Recovery:

```text
read both
validate crc/footer
pick highest epoch
```

This avoids depending on overwrite atomicity.

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

The 64 B → 128 B cost jump (~7 ms) is the LittleFS inline file threshold (`LFS_INLINE_MAX`, default 64 B). Files ≤ 64 B are stored inline in the directory entry with no separate data block allocation. **Design the root to fit in 64 B** to stay below this threshold.

### Amortized rotation overhead per enqueue (128 B root)

| Segment size | Records/seg | Amortized (µs/enq) |
|--------------|-------------|--------------------|
| 4 KB         | 7           | 7,426              |
| 8 KB         | 15          | 3,465              |
| 16 KB        | 31          | 1,676              |
| 32 KB        | 63          | 825                |

At 4 KB segments with a 64 B root (rotation = 45 ms), amortized overhead = 45,090 / 7 ≈ 6,441 µs/enq — about 14% of the 45 ms flush cost. Acceptable.

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
1 root publish (≤64 B)            — ~26 ms
total                              — ~45 ms
```

Amortized over 7 records at 4 KB default = ~6.4 ms per enqueue overhead (~14% of normal enqueue cost).

### Compaction step

```text
N full compacted segment writes (~45 ms each at 4 KB)
1 root publish (≤64 B)            — ~26 ms
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

## Important open problems

### Open problem 1: rotation durability — RESOLVED

Decision: root publish on rotation with 4 KB segments. Measured rotation cost ~45 ms (64 B root), amortized to ~6.4 ms per enqueue at 4 KB segments. Self-describing tail chain fallback is not needed.

### Open problem 2: root format — PARTIALLY RESOLVED

**Size constraint:** root must fit in ≤ 64 B to stay within the LittleFS inline file threshold and avoid the ~7 ms penalty of a separate block allocation. This is a hard constraint, not a preference.

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
─────────────────
fixed overhead: 30 B
room for ranges: 34 B → 4 ranges max
```

4 active ranges is sufficient for normal operation (sealed compacted ranges + open tail). If a layout ever exceeds 4 ranges, a larger root is needed — this should be treated as a configuration or compaction policy constraint, not a format error.

**Still open:** whether tailGeneration is encoded separately or as part of ranges; whether a sealed marker is needed in the root or implied by not being tailGeneration.

### Open problem 3: open tail framing

Under the default model (root publish on rotation), the segment that is currently being appended to is "open" between rotations. Recovery must handle a torn write at the end of that segment. Need to define:

- segment header format (magic, generation, baseSequence, CRC of header);
- per-event framing (length, payload, per-event CRC);
- truncation rule on first bad CRC (truncate the open segment to the last good event boundary);
- whether the segment carries a "sealed" footer written at rotation time, or whether sealedness is implied by no-longer-being-the-tail-in-the-root.

This is the actual hard problem behind rotation, and it must be specified before code.

### Open problem 4: compaction selection policy

Need a first simple policy:

- one oldest sealed segment with enough dead bytes;
- or oldest safe range;
- never logical last segment;
- skip mostly-live segments;
- budget by bytes/time/files.

### Open problem 5: memory budget

Full-buffer writes require RAM.

Need decide:

- max compaction output buffer size;
- whether 4KB is acceptable on ESP32;
- whether to stream into one segment buffer;
- fallback behavior if RAM unavailable.

### Open problem 6: cleanup under low free space

If cleanup is lazy, what happens when free space is low?

Possible answer:

- cleanup can run before compaction/enqueue when necessary;
- but it must delete only unreferenced files;
- if no garbage exists, enqueue may fail.

### Open problem 7: validation/repair

Normal mount should not salvage aggressively.

Repair tool may later:

- scan dangling files;
- recover from root corruption;
- inspect segment chains;
- rebuild root if possible.

But normal mount should be strict and simple.

## Questions for next design session

Resolved:

- Rotation publishes the root. Self-describing tail chain is fallback only.
- Root stores layout only; queue head/tail/count come from replay.
- Target segment size: **4 KB**. Measured flush cost cliff at 4→8 KB makes 4 KB the clear winner.
- Root size target: **≤ 64 B**. LittleFS inline threshold saves ~7 ms per rotation.

Still open, in order:

1. Open-tail framing: segment header format, per-event CRC, truncation rule, sealed marker.
2. Exact root binary layout fitting in 64 B (see open problem 2 sketch).
3. RAM budget for the compaction output buffer (does 4 KB fit on ESP32 alongside everything else).
4. Should compaction operate only on sealed segments? (Strong default: yes.)
5. Should cleanup run automatically or only under explicit maintenance / free-space pressure?
6. Is dual-root mandatory, or can LittleFS atomic rename be trusted after a targeted crash test?

## Recommended next step

Segment size (4 KB) and root size target (≤ 64 B) are resolved by measurement.

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

## Short handoff summary

Model:

```text
authoritative dual root (layout only, no head/tail/count)
segments as dumb blobs
unreferenced files ignored
normal appends avoid root writes
rotation publishes root; segment size tuned so this is invisible
compaction incremental, full-buffer writes, one root publish per step
old deletion deferred to lazy cleanup
```

Next work is specifying the open-tail framing and measuring rotation cost. Compaction comes after.

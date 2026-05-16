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

This is only painful if rotation is frequent. With reasonable segment sizes (say 8–16 KB on LittleFS) and typical event sizes, rotation is rare enough that one small root write per rotation amortizes invisibly into the workload. The rotation cost should be measured, not feared.

The default model is therefore: **publish the root on rotation**. A self-describing tail chain is kept as a fallback if measurement shows rotation cost is genuinely unacceptable, not as a co-equal design path.

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

### Segment sizing

Rotation cost is controlled by segment size. Larger segments mean fewer rotations and fewer root writes, at the cost of higher per-rotation flush work and larger per-segment buffers.

The right segment size is the one that, given the workload, makes rotation rare enough that root-publish-on-rotation is invisible. Target range to investigate: 4–16 KB, tuned to LittleFS block/page behavior. This must be measured on target hardware, not guessed.

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

## Performance analysis framework

The next design chat should estimate write costs by operation.

### Enqueue, no rotation

Expected writes:

```text
1 append/write to current segment
```

No root update.

If current implementation flushes every append, this remains the main hot-path cost.

### Pop

Expected writes:

```text
1 append/write POP event to current segment
```

No root update.

### Rewrite front

Expected writes:

```text
1 append/write REWRITE event to current segment
```

No root update.

### Rotation

Default model:

```text
1 write new segment header
1 root publish
```

Cost is amortized across all events written into the new segment. Tune segment size so this amortizes to invisibility.

### Compaction step

Target writes:

```text
N full compacted segment writes
1 root publish
0 old deletes
```

N should usually be small, often 1.

### Cleanup

Target writes/deletes:

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

Decision: root publish on rotation, with segment size tuned so rotation is rare enough that the cost is invisible. The self-describing tail chain is kept as a fallback only if measured rotation cost on target hardware proves unacceptable.

Outstanding work: measure rotation cost on target hardware, pick the segment size that makes it invisible.

### Open problem 2: root format

Need to decide whether root stores:

- full ordered list of active segments;
- ordered ranges;
- sealed layout plus open tail;
- queue head/tail/count;
- nextGeneration;
- cleanup watermark;
- epoch.

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

Still open, in order:

1. Target segment size given measured LittleFS rotation cost on target hardware.
2. RAM budget for the compaction output buffer (does 4 KB fit on ESP32 alongside everything else).
3. Open-tail framing: header, per-event CRC, truncation rule, sealed marker.
4. Should compaction operate only on sealed segments? (Strong default: yes.)
5. Should cleanup run automatically or only under explicit maintenance / free-space pressure?
6. Is dual-root mandatory, or can LittleFS atomic rename be trusted after a targeted crash test?

## Recommended next step

Do not implement compaction yet.

First nail down the open-tail framing and measure rotation cost on target hardware. Minimal next task:

```text
Specify segment framing (header, per-event CRC, truncation rule, sealed marker).
Measure root-publish-on-rotation cost on target hardware at candidate segment sizes.
Pick segment size.
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

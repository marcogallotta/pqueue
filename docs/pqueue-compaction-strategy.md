# pqueue Compaction Strategy Investigation

**Editing rules:** ASCII only -- no Unicode symbols (no checkmarks, arrows, emoji). This file is compiled to PDF via LaTeX and non-ASCII characters cause build warnings or missing glyphs.

## Background

v1 compaction is greedy and local: it always picks the oldest full range, reads all live records from it, and writes one output segment. This is correct but not globally optimal. The known failure mode: if the live bytes in the selected range exceed `maxSegmentBytes`, `compactRange()` returns `noOp`. Under sustained enqueue load, ranges routinely exceed one segment of live data. Once range count hits 4 and no range fits in a single output segment, the queue deadlocks permanently.

The goal of this investigation is to find a replacement strategy that eliminates deadlocks under realistic workloads, minimises write amplification, and has a simple enough trigger model to be reliable on-device.

## Simulator

Implementation: `tools/pqueue_compaction_sim.cpp`. Build with `make -j12 sim`, run as `./build/pqueue-compaction-sim`. The full sweep runs in ~2 seconds (7 strategies, in-memory FS, early abort at 1000 failures).

The simulator drives the real `AppendLogStore` API through configurable enqueue/pop patterns and records compaction behaviour. Storage I/O uses an in-memory `FileSystem` (`tools/memory_file_system.h`) -- no disk access occurs. Byte counts for flash wear measurement are tracked via a `CountingFileSystem` wrapper (`tools/counting_file_system.h`) around the memory backend.

A run aborts early once a strategy hits 1000 deadlocks: at that point the workload has already failed and further operations add no signal.

### Workloads

Two workload families are swept:

**Random interleaved.** Each operation is independently an enqueue or pop with probability `enqueueProb`. Three variants: enqP=0.55, 0.65, 0.80. Record size 19 bytes.

**Burst.** Models the primary real-world failure scenario: the device enqueues continuously while the server or network is unreachable, then drains when the consumer reconnects. Alternates between a pure-enqueue phase of `burstSize` ops and a pure-pop phase of `popRatio * queueSize` pops. Swept across burstSize in {12, 60, 250}, popRatio in {0.25, 0.5, 0.9}, and recordSize in {8, 19, 62} bytes.

All parameters are proportionally scaled to keep runs fast (scaling factor ~1/8 from real-world values):

- maxSegmentBytes: 4096 -> 512
- recordSizes: 64/150/492 -> 8/19/62
- burstSizes: 100/500/2000 -> 12/60/250
- numOps: 50000 -> 15000

This preserves the records-per-segment ratio and relative workload intensity. Before making a final strategy decision, revert to real-world sizes and rerun to confirm findings hold at production scale.

### Compaction trigger

The simulator uses a state-based, rising-edge trigger rather than a fixed operation-count cadence. Early runs with operation-count triggers (compact every N enqueues) showed that cadence dominates strategy quality: aggressive cadences produced up to 820 no-ops out of 827 attempts; relaxed cadences made all strategies look identical. The current trigger fires when a new full range is added (segment rotation) and at least one range exceeds the `deadRatioTrigger` threshold, or when range count reaches the emergency `rangePressureTrigger`. This collapses the no-op rate without over-firing.

### Metrics

Each run reports:

- **WriteAmp** -- live bytes copied per dead byte reclaimed. Lower is better.
- **Wear(KB)** -- total bytes written over the workload lifetime.
- **MaxRange** -- peak range count reached.
- **Compacts** -- times the strategy returned a range to compact.
- **NoOps** -- compaction attempts that returned no-op from the store.
- **Skips** -- times the strategy returned nullopt (declined to compact).
- **MaxOutSeg** -- maximum number of output segments written in a single compaction call. Directly maps to on-device stall latency: each LittleFS segment write is ~45ms, so MaxOutSeg=5 means up to 225ms stall. On passing workloads this peaks at 1-25; large values (up to 123) only appear on already-failed (capacity exhaustion) workloads.
- **Deadlock** -- enqueue failures where at least one range had >= 1% dead bytes at the time of failure. These are true strategy failures: compaction could have made progress but did not.
- **CapExhst** -- enqueue failures where no range had >= 1% dead bytes. These are capacity exhaustion: the queue is full of live data and no compaction strategy can help.

### Candidate strategies

Seven strategies were evaluated. All implement the same interface: given per-range stats (total bytes, live bytes, dead bytes) and the current range count, return a range to compact or nullopt.

1. **HighestDeadRatio** -- picks the range with the highest dead bytes / total bytes ratio. Skips if no range has dead bytes.
2. **CostBenefit** -- picks the range with the highest dead bytes / live bytes ratio. Minimises write amplification directly.
3. **RangeConsolidation** -- intended to prefer ranges whose compaction reduces range count by merging with an adjacent range. With current generation numbering, output segments get non-adjacent generations, so true merge is not achievable; falls back to dead-ratio selection. Eliminated: structurally identical to HighestDeadRatio.
4. **DeferUntilPressure** -- no-op until range count reaches 75% of the limit, then picks highest dead-ratio range. Eliminated: without proactive compaction, segment rotations extend a single range's endGen and range count stays at 1, so the pressure threshold is never triggered. In practice identical to HighestDeadRatio.
5. **MinLiveBytesCopied** -- among ranges with any dead bytes, picks the one with the fewest live bytes to copy. Shows occasional write-amp advantage on large-burst/fast-pop workloads but not consistently dominant.
6. **AgeWeightedDeadRatio** -- dead-ratio weighted by position in the range list (older ranges get higher weight). Mild advantage on a single workload (burst=60/pop=90%/rec=8) that does not persist across the sweep.
7. **LookaheadHeuristic** -- cost-benefit, but skips ranges below a 20% dead-ratio floor unless under pressure. Behaviour is identical to CostBenefit on almost all workloads.

### Eliminated strategies

Ten strategies were evaluated in total. Eight were eliminated; two remain.

**First-pass eliminations (true deadlocks):** Three strategies were eliminated because they produce true deadlocks (Deadlock > 0) on workloads that all remaining candidates handle without deadlocking. A strategy that deadlocks where another does not is strictly worse -- there is no tradeoff to weigh.

**OldestFirst** -- always picks the oldest range regardless of dead-byte content. Under any sustained enqueue load, it compacts fully-live ranges, copying all their data for zero reclaim. This wastes compaction passes and consumes range capacity without progress, eventually hitting the range limit while dead data is still available.

**PressureWeightedHybrid** -- normally uses cost-benefit, but falls back to oldest-first when range count reaches 75% of the limit. The fallback is triggered precisely when the queue is under the most pressure -- the worst time to compact a live range. The fallback cancels out any benefit from the normal-mode selection.

**DeadByteThreshold** -- oldest-first with a dead-ratio skip threshold, but the threshold is overridden to zero under emergency pressure. Same failure mode as PressureWeightedHybrid: the pressure override causes it to compact live ranges exactly when that is most harmful.

**Second-pass eliminations (redundancy):** After the three above were removed, the remaining seven were compared directly. Four are structurally equivalent to one of the two finalists and add no benefit.

**RangeConsolidation** -- falls back to dead-ratio selection because output segments receive non-adjacent generation numbers, making true range merging impossible. Identical in behaviour to HighestDeadRatio.

**DeferUntilPressure** -- designed to defer until range count pressure builds, but without proactive compaction, segment rotations extend a single range endGen and range count stays at 1. The pressure threshold is structurally never triggered. Identical in behaviour to HighestDeadRatio.

**AgeWeightedDeadRatio** -- adds an age weight on top of dead-ratio scoring. Shows a mild write-amp advantage on one workload (burst=60/pop=90%/rec=8: 0.94x vs 1.69x) that does not appear elsewhere in the sweep. Not consistently better than HighestDeadRatio.

**LookaheadHeuristic** -- cost-benefit with a 20% dead-ratio floor that is lifted under emergency pressure. Behaviour is identical to CostBenefit on almost all workloads. The floor adds complexity without measurable benefit.

**MinLiveBytesCopied** -- shows slightly better write amplification on some large-burst/fast-pop workloads (e.g. burst=250/pop=90%/rec=62: 8.15x vs 8.63x for HighestDeadRatio/CostBenefit) but the advantage is not consistent across the sweep and does not justify the added complexity over the two finalists.

## Findings

### Capacity exhaustion is not a strategy failure

When the queue fills with live data and enqueue growth permanently exceeds drain rate, the range limit is hit with no dead bytes available to reclaim. No compaction strategy can help in this state -- it is equivalent to any bounded queue hitting its size limit. The right response is backpressure or a larger queue, not a smarter compactor.

The Deadlock and CapExhst metrics distinguish the two cases. Ratio-based strategies on overloaded workloads show CapExhst=1000, Deadlock=0: the queue is genuinely full of live data. OldestFirst on the same workloads shows Deadlock=1000, CapExhst=0: it is hitting the range limit while dead data is still present, because it wastes compaction passes on live ranges.

### Usefulness-gating is a hard requirement

For recoverable workloads (burst with pop >= 90%, burst=12/pop=50%), the strategy split is binary:

The three eliminated strategies (OldestFirst, PressureWeightedHybrid, DeadByteThreshold) all show Deadlock=1000 on workloads the remaining candidates handle cleanly. See the Eliminated strategies section for the root cause analysis.

The seven remaining candidates all gate on dead-byte ratio and refuse to compact a mostly-live range. When they hit the limit on overloaded workloads, the CapExhst column confirms it is capacity exhaustion, not a strategy failure.

Usefulness-gating is therefore a first-order correctness requirement. Any strategy without it produces true deadlocks under sustained load.

### Multi-segment output is confirmed and bounded

`compactRange()` writes multiple output segments when live bytes in the selected range exceed `maxSegmentBytes`, gated on outputSegs < inputSegs (strictly reduces segment count). This was listed as a possible future feature in earlier work; it is already implemented and actively firing.

MaxOutSeg on passing workloads peaks at 1-25 (e.g. burst=250/pop=90%/rec=62 reaches 25). At 45ms per LittleFS segment write this is up to ~1.1 seconds of stall in the worst case on passing workloads. Large MaxOutSeg values (up to 123) appear only on capacity-exhaustion runs, which are not strategy failures. The stall risk is acceptable for the target use case, but should be monitored on-device.

### Final candidates

**HighestDeadRatio and CostBenefit are the two remaining candidates.** All other strategies were eliminated either for producing true deadlocks or for being structurally equivalent to one of these two. See the Eliminated strategies section for per-strategy reasoning.

The two candidates are functionally indistinguishable on all tested workloads: same Deadlock/CapExhst split, similar write amplification and flash wear. The difference is the scoring function: HighestDeadRatio maximises dead fraction (dead/total); CostBenefit maximises dead-to-live ratio (dead/live), which directly minimises write amplification. Either is a correct choice.

## Real-world scale rerun

Sim parameters reverted to production values (maxSegmentBytes=4096, recordSizes=64/150/492,
burstSizes=100/500/2000, numOps=50000). Results confirm and extend the scaled findings.

### Confirmed findings

HighestDeadRatio and CostBenefit remain identical across all 30 burst workloads and all 3
random workloads. The Deadlock/CapExhst classification is consistent: failures are either
all-deadlock or all-capacity-exhaustion with no mixed results.

### New finding: DeferUntilPressure diverges at real scale

The scaled run described DeferUntilPressure as structurally identical to HighestDeadRatio.
At real scale it deadlocks on enqP=0.55 (Deadlock=1000) while all other candidates pass.
The scaled description was wrong: at 4096-byte segments the structural equivalence breaks
down. DeferUntilPressure is now a confirmed elimination on correctness grounds, not just
redundancy grounds.

### MinLiveBytesCopied write-amp divergence is larger at real scale

The scaled run noted minor write-amp advantages for MinLiveBytesCopied on some workloads.
At real scale the wear gap is more pronounced on large-burst/slow-pop workloads:

- burst=2000/pop=25%/rec=64: 2132 KB vs 12080 KB (HighestDeadRatio/CostBenefit) -- ~6x less wear
- burst=500/pop=25%/rec=150: 1685 KB vs 4912 KB -- ~3x less wear

However it is worse on other workloads (burst=500/pop=25%/rec=492: WriteAmp 133 vs 67).
The advantage is concentrated in a specific regime (very large bursts, slow drain, small
records) and does not generalise across the sweep. MinLiveBytesCopied remains eliminated
as not consistently dominant.

### MaxOutSeg at real scale

On genuinely passing workloads (Deadlock=0, CapExhst=0) MaxOutSeg peaks at 60
(burst=500/pop=90%/rec=492). At 45ms/segment this is a ~2.7 second stall in the worst
passing case, higher than the 1-25 estimate from the scaled run. This must be verified
on-device; if stall latency is unacceptable the trigger thresholds or segment size will
need tuning.

## On-device validation

Test: `tests/arduino/test_pqueue_compaction/test_main.cpp`. Environment: `esp32s3-compaction`
in `platformio.ini`. Build and upload with `~/venvs/esp/bin/pio test -e esp32s3-compaction
--without-testing`.

Workload run: burst=100/pop=90%/rec=150B, 5 cycles, HighestDeadRatio strategy.

### Trigger fix

The sim uses a rising-edge trigger on range count (new range added). On-device, range count
stays at 1 throughout all cycles: all segment generations fall in one contiguous range and
compacted output stays in that same range. The range-count rising edge never fires.

Fix: track segment count instead of range count. A new segment file is the correct rising
edge -- it means new data has been written that may be worth compacting. The trigger then
checks dead ratio against kDeadRatioTrigger (0.25) and fires compaction if any range
qualifies. This is implemented in test_main.cpp; the store itself has no built-in trigger
and does not need to change.

### Results

  Workload: burst=100 pop=90% rec=150B cycles=5
  Compactions: 100  NoOps: 0  MaxOutSegs: 1
  MaxLatency: 1090 ms
  Deadlocks: 0  CapExhausted: 0  FinalQueueSize: 12

- MaxOutSegs=1 confirms the sim prediction for this workload.
- 0 deadlocks, 0 capacity exhaustion. All enqueued records readable after workload.
- outSegs=0 results (printed but not counted in MaxOutSegs) are ranges where all records
  were dead; compaction deleted the segments with no output written. This is correct.

### Latency model correction

The 45ms/segment estimate from the sim was a write-only approximation. Actual measured
latency per compactRange() call at 150B records and 1 output segment: 200-1090ms. The
dominant cost is reading all records in the range to find live ones, not just the write.
The 45ms model is wrong and should not be used for stall estimates. The correct figure
for this workload at 1 output segment is roughly 200-1100ms.

MaxOutSeg=60 (worst passing case from the real-world sim run, burst=500/pop=90%/rec=492)
at ~500-1100ms per single-segment compaction would imply a stall in the range of tens of
seconds in the absolute worst case. This needs a dedicated on-device run at those
parameters before shipping.

### Compaction frequency

With kDeadRatioTrigger=0.25 and segment-count rising edge, 100 compactions fired over
5 cycles x 100 enqueues. This is roughly once per segment rotation, which is aggressive.
Raising kDeadRatioTrigger (e.g. to 0.40 or 0.50) would reduce compaction frequency at
the cost of higher peak dead-byte accumulation. Tuning is deferred until the full
burst=500/pop=90%/rec=492 run is complete.

## Worst-case on-device run (burst=500/pop=90%/rec=492)

Test parameters: kBurstSize=500, kCycles=15, kRecordSize=492, kMaxTotalBytes=1MB,
kDeadRatioTrigger=0.25 (later 0.10). Environment: esp32s3-compaction.

### Result

The run did not complete normally. Within the first burst phase of cycle 0 the first
compactRange() call stalled for 30 seconds with outSegs=54:

  [compact] outSegs=54 latency=30602ms

This is not a measurement of worst-case stall under correct operation. It is a symptom
of a structural bug in how the trigger interacts with the compaction output generation
numbering. Lowering kDeadRatioTrigger from 0.25 to 0.10 did not help -- the stall
recurred at the same magnitude.

### Root cause: generation gap fragmentation

When compactRange([1,59]) runs, it writes output segments starting at nextGeneration_
(say gen 61), producing range [61,114]. The active segment at the time of compaction is
gen 60 -- it was open during compaction and is not included in the compacted range.

When gen 60 eventually fills and rotates, it seals as {60,60}. rotateSegment() checks
whether it is contiguous with the last manifest range: back().endGen+1 == 60? With back
= [61,114], that is 115 == 60, which is false. So {60,60} is added as a new range:
[{61,114},{60,60}].

The next new segment after compaction is gen 115 (nextGeneration_ was set to 115 after
compaction output). When gen 115 rotates, it checks back().endGen+1 == 115: back is
{60,60}, so 61 == 115, false. A third range is added: [{61,114},{60,60},{115,115}].

kManifestMaxRanges=4 and kRangePressureTrigger=3. With 3 ranges, the test trigger fires
compaction regardless of dead ratio. chooseHighestDeadRatio is called but with ~0% dead
bytes it returns nullopt -- no compaction fires. However the next rotation would push to
4 ranges, hitting RangeLimitExceeded in rotateSegment(). To avoid this, compaction must
reduce range count before then.

When dead bytes do exist (e.g. after pops in earlier cycles), pressure triggers
compaction on a range that may contain hundreds of live records. With 492B records and
maxSegmentBytes=4096, a range holding ~430 live records requires 54 output segments.
compactRange() allows this because outputSegs(54) <= inputSegs(59). The result is a
30-second stall.

The root problem is that compaction output generations are numerically above the active
generation, creating a gap that prevents range merging and causes permanent fragmentation
after every compaction pass.

### Current state

**Compaction usefulness gate (test_main.cpp).** The checkAndCompact trigger treats a
compaction as useful only if it produces a structural improvement:

  - the range is fully dead (all records popped), or
  - dead bytes > 0 and predicted outputSegs <= inputSegs, or
  - predicted outputSegs < inputSegs (structural consolidation, even at low dead ratio)

Predicted output segment count is estimated from segmentStats() liveBytes without reading
records: ceil(liveBytes / maxSegmentBytes). Compaction is skipped if predicted outputSegs >
inputSegs (would worsen range count) or predicted outputSegs == inputSegs with no dead bytes
(pure no-op). This gate lives entirely in test_main.cpp and does not touch the store.

**Rotate-before-compact (store).** When the selected range is the last manifest range and
the active tail is contiguous with it, compactRange() seals the tail into the compaction
input before writing output. This keeps output generations contiguous with whatever follows,
preventing the orphan-tail gap that caused the 30-second stall.

The design is preflight-before-rotate: usefulness is evaluated on the hypothetical extended
range (including the tail) using only in-memory record sizes and sealed-segment file sizes,
without mutating state. If the hypothetical compaction would be a no-op, the function returns
noOp immediately and the rotate does not fire. Only if useful does rotateSegment() run, after
which the effective range is re-resolved from the manifest to pick up the extended endpoint.

RangeLimitExceeded is structurally unreachable on the rotate path: rotateSegment() merges the
contiguous tail into the last manifest range before checking the limit, so range count is
unchanged.

After compaction, cleanupOneDanglingSegment() is called inputSegCount - 1 additional times
beyond the one implicit in publishManifest. This drains the former-tail segment (now dangling)
promptly; without it, the dangling file inflates totalOnDiskBytes_ and blocks subsequent writes.

**Residual fragmentation.** Rotate-before-compact prevents the catastrophic case where range
count hits kManifestMaxRanges=4 after every compact cycle. It does not prevent all
fragmentation: when the orphan tail (the sealed former-active at generation T+1, numerically
below the output range) later receives live records, it forms a range {T+1,T+1} that cannot
merge with the output range until it is itself compacted. Range count can reach 3 in a mixed
workload. This is bounded: dead-range elimination reclaims the orphan once its records are
popped, and range count never reaches 4.

## On-device run 2 (burst=500/pop=90%/rec=492, rotate-before-compact)

Run after rotate-before-compact landed. Observed:

  [compact] outSegs=54 latency=131384ms
  [compact] outSegs=24 latency=64853ms

Root cause: the hot-path selector (chooseHighestDeadRatio) returns an entire manifest
range as the compaction unit. A range spanning 54 output segments requires roughly 378
readAt() calls (54 segs x 7 records each) plus 54 writeFile() calls. Each LittleFS I/O
is 50-300ms. Total: ~130 seconds for one compaction call.

This is a selector policy problem, not a store problem. compactRange() is correct to
compact whatever range it is handed. The selector must not hand it a range that produces
more than a bounded number of output segments in the hot path.

Hot-path I/O improvements also landed during this run:
- checkAndCompact now uses logical segment count (sum of range widths + 1 for tail)
  instead of segmentStats().size(), eliminating per-generation fileSize() calls from
  the rising-edge check.
- SegmentWriteDisposition::MustBeNew added: createSegment() and compaction output writes
  skip the fileSize() probe that was hitting non-existent files at 2-3s each on LittleFS.
These helped latency of individual small compactions but did not address the 54-segment job.

## Bounded compaction window (test_main.cpp)

**Implementation.** narrowRange() in test_main.cpp caps the hot-path compaction unit at
kMaxOutputSegs=8 predicted output segments. If chooseHighestDeadRatio returns a range
whose predictedOutputSegs() exceeds the budget, narrowRange() retrieves per-segment stats
(store.segmentStats()), filters to the chosen range, and runs an O(n^2) sliding window
over the sorted generations. For each (i,j) window it accumulates liveBytes and stops
expanding when the next segment would push ceil(liveBytes/maxSegmentBytes) above the
budget. The window with the highest dead ratio within the budget is returned. A single
segment always fits (liveBytes <= maxSegmentBytes), so the function always returns a
valid subrange.

The original compaction target is passed to store.compactRange() unchanged when it
already fits within the budget; narrowRange() is a no-op in that case.

A [narrow] log line is printed when the range is actually shrunk, showing the original
and narrowed generation bounds.

The store and compactRange() are unchanged; narrowRange() is entirely in the test harness.

## On-device run 3 (burst=500/pop=90%/rec=492, bounded window)

Results:

  Workload: burst=500 pop=90% rec=492B cycles=15
  Compactions: 102  NoOps: 30  MaxOutSegs: 9
  MaxLatency: 82580 ms
  Deadlocks: 0  CapExhausted: 0  FinalQueueSize: 56
  PASS

The bounded window (kMaxOutputSegs=8) eliminates the original 131-second stall.
0 deadlocks, 0 capacity exhaustion across 15 cycles. The MaxOutSegs=9 result (above
kMaxOutputSegs=8) occurs when rotate-before-compact extends the effective range by 1
segment (the tail); the budget applies to the pre-rotate range. Acceptable.

MaxLatency=82580ms is dominated by cleanup_ms=77216ms, not by the compaction itself.
The breakdown for the worst-case compaction (75 input segs, 54 live records, 8 outputs):

  collect_ms=2689  write_ms=960  publish_ms=24
  cleanup_ms=77216  pre_size_ms=1601  total_ms=82557

Two O(n) bugs remain that were not caught in earlier sessions:

### Bug A: O(n^2) cleanup (cleanup_ms=77216ms, CRITICAL)

compactRange calls cleanupOneDanglingSegment() inputSegCount times (lines 312 and 413
of compaction.cpp). Each call to cleanupOneDanglingSegment() calls listFiles() on the
full directory -- this takes 1-1.5 seconds on LittleFS with ~75 files. Result:
75 calls x ~1.1s = ~82 seconds for the 75-seg compaction. The same pattern produces
cleanup_ms=9054ms for an 8-seg pressure compaction and cleanup_ms=1318ms for a 1-seg
compaction.

Fix: replace the N-call loop with a single batch delete that calls listFiles once,
identifies all non-active segments in one pass, and deletes them all. This is O(n_files)
instead of O(n_files x n_input_segs).

### Bug B: O(n) preflight fileSize (pre_size_ms=1601ms)

The preflight loop in compactRange (compaction.cpp lines 199-207) still calls
fs()->fileSize() for each sealed segment in the hypothetical range. With 75 sealed
segments at ~21ms per fileSize call, this totals ~1.6 seconds. The sealedSegmentBytes_
map was added to eliminate exactly this pattern in segmentStats(), but the preflight
loop was not updated.

Fix: replace fs()->fileSize(segmentName(gen), sz) with a sealedSegmentBytes_.find(gen)
lookup in the preflight loop.

### Other observations from run 3 logs

GC pressure resets after compaction: create_ms drops from 300-1709ms (late burst) back
to 20-35ms (immediately post-compaction), confirming compaction relieves LittleFS block
pressure by freeing the input segments. As the next burst accumulates segments, create_ms
climbs again. This is expected LittleFS behavior.

Manifest slot cache (probe_ms): probe_ms=0 on every rotation after the first, confirming
the cachedWrittenSlot_ fix is working.

noOp during burst: all compaction attempts during the burst phase return noOp(notFound)
because narrowRange produces sub-ranges (e.g. [1,1]) that are not manifest entries. This
is correct -- there is too much live data to compact during the burst. After pops reduce
live records to ~54, compaction fires and succeeds.

## Next steps

**Fix Bug A (cleanup O(n^2)).** Add cleanupAllDanglingSegments() that calls listFiles
once and deletes all non-active segments. Replace the loops at compaction.cpp lines 312
and 413. Expected: cleanup_ms for 75-seg compaction drops from ~77s to ~2s (1 listFiles
+ 75 removes at ~24ms each).

**Fix Bug B (preflight fileSize O(n)).** In compactRange preflight (lines 197-208),
replace fs()->fileSize(segmentName(gen), sz) with sealedSegmentBytes_.find(gen) lookup.
Expected: pre_size_ms drops from ~1600ms to ~0ms.

**Final strategy selection.** Deferred until the above fixes land and MaxLatency is
confirmed acceptable.

**Capacity exhaustion is out of scope.** The sim confirms workloads where ratio-based
strategies hit the range limit are pure capacity exhaustion (CapExhst=1000, Deadlock=0):
the queue is full of live data and no compaction strategy can help. The correct fix is
application-level backpressure or a larger queue. RangeLimitExceeded in this state is
correct and expected behaviour.

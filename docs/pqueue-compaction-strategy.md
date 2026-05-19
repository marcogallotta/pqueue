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

The simulator uses a state-based, rising-edge trigger rather than a fixed operation-count cadence. Early runs with operation-count triggers (compact every N enqueues) showed that cadence dominates strategy quality: aggressive cadences produced up to 820 no-ops out of 827 attempts; relaxed cadences made all strategies look identical. The current trigger fires when a new segment file is written and at least one range exceeds the `deadRatioTrigger` threshold, or when range count reaches the emergency `rangePressureTrigger`. This collapses the no-op rate without over-firing.

The rising edge is segment count (a new segment file), not range count. With a single contiguous range, range count stays at 1 throughout all cycles and the range-count edge never fires. Segment count is the correct signal.

### Metrics

Each run reports:

- **WriteAmp** -- live bytes copied per dead byte reclaimed. Lower is better.
- **Wear(KB)** -- total bytes written over the workload lifetime.
- **MaxRange** -- peak range count reached.
- **Compacts** -- times the strategy returned a range to compact.
- **NoOps** -- compaction attempts that returned no-op from the store.
- **Skips** -- times the strategy returned nullopt (declined to compact).
- **MaxOutSeg** -- maximum number of output segments written in a single compaction call. Directly maps to on-device stall latency.
- **Deadlock** -- enqueue failures where at least one range had >= 1% dead bytes at the time of failure. These are true strategy failures: compaction could have made progress but did not.
- **CapExhst** -- enqueue failures where no range had >= 1% dead bytes. These are capacity exhaustion: the queue is full of live data and no compaction strategy can help.

### Candidate strategies

Seven strategies were evaluated. All implement the same interface: given per-range stats (total bytes, live bytes, dead bytes) and the current range count, return a range to compact or nullopt.

1. **HighestDeadRatio** -- picks the range with the highest dead bytes / total bytes ratio. Skips if no range has dead bytes.
2. **CostBenefit** -- picks the range with the highest dead bytes / live bytes ratio. Minimises write amplification directly.
3. **RangeConsolidation** -- intended to prefer ranges whose compaction reduces range count by merging with an adjacent range. With current generation numbering, output segments get non-adjacent generations, so true merge is not achievable; falls back to dead-ratio selection. Eliminated: structurally identical to HighestDeadRatio.
4. **DeferUntilPressure** -- no-op until range count reaches 75% of the limit, then picks highest dead-ratio range. Eliminated: deadlocks on enqP=0.55 at real scale.
5. **MinLiveBytesCopied** -- among ranges with any dead bytes, picks the one with the fewest live bytes to copy. Shows occasional write-amp advantage on large-burst/fast-pop workloads but not consistently dominant.
6. **AgeWeightedDeadRatio** -- dead-ratio weighted by position in the range list (older ranges get higher weight). Mild advantage on a single workload (burst=60/pop=90%/rec=8) that does not persist across the sweep.
7. **LookaheadHeuristic** -- cost-benefit, but skips ranges below a 20% dead-ratio floor unless under pressure. Behaviour is identical to CostBenefit on almost all workloads.

## Findings

### Capacity exhaustion is not a strategy failure

When the queue fills with live data and enqueue growth permanently exceeds drain rate, the range limit is hit with no dead bytes available to reclaim. No compaction strategy can help in this state -- it is equivalent to any bounded queue hitting its size limit. The right response is backpressure or a larger queue, not a smarter compactor.

The Deadlock and CapExhst metrics distinguish the two cases. Ratio-based strategies on overloaded workloads show CapExhst=1000, Deadlock=0: the queue is genuinely full of live data. OldestFirst on the same workloads shows Deadlock=1000, CapExhst=0: it hits the range limit while dead data is still present, because it wastes compaction passes on live ranges.

### Usefulness-gating is a hard requirement

For recoverable workloads (burst with pop >= 90%, burst=12/pop=50%), the strategy split is binary. The three eliminated strategies (OldestFirst, PressureWeightedHybrid, DeadByteThreshold) all show Deadlock=1000 on workloads the remaining candidates handle cleanly.

Usefulness-gating is therefore a first-order correctness requirement. Any strategy without it produces true deadlocks under sustained load.

### Multi-segment output is confirmed and bounded

`compactRange()` writes multiple output segments when live bytes in the selected range exceed `maxSegmentBytes`, gated on outputSegs < inputSegs (strictly reduces segment count). This is already implemented and actively firing.

### Generation gap fragmentation

When `compactRange([1,59])` runs, output segments start at `nextGeneration_` (say gen 61), producing range [61,114]. The active segment at the time of compaction is gen 60. When gen 60 eventually fills and rotates, it seals as {60,60}. rotateSegment() checks contiguity with the last range: back().endGen+1 == 60? With back=[61,114], 115 != 60, so {60,60} is added as a new range. A subsequent rotation at gen 115 similarly fails contiguity with {60,60}, producing a third range. Range count climbs rapidly and the next compaction is forced on a range with large live data, causing multi-minute stalls.

Fix: rotate-before-compact (see Implementation below).

### Final candidates

**HighestDeadRatio and CostBenefit are the two remaining candidates.** All other strategies were eliminated either for producing true deadlocks or for being structurally equivalent to one of these two.

The two candidates are functionally indistinguishable on all tested workloads: same Deadlock/CapExhst split, similar write amplification and flash wear. The difference is the scoring function: HighestDeadRatio maximises dead fraction (dead/total); CostBenefit maximises dead-to-live ratio (dead/live), which directly minimises write amplification. Either is a correct choice.

Final selection is deferred until the worst-case on-device run (burst=500/pop=90%/rec=492) is complete with current fixes in place.

## Implementation

### Rotate-before-compact (store)

When the selected range is the last manifest range and the active tail is contiguous with it, `compactRange()` seals the tail into the compaction input before writing output. This keeps output generations contiguous with whatever follows, preventing the orphan-tail gap.

Design: preflight-before-rotate. Usefulness is evaluated on the hypothetical extended range (including the tail) using only in-memory record sizes and sealed-segment file sizes (`sealedSegmentBytes_`), without mutating state. If the hypothetical compaction would be a no-op, the function returns noOp immediately and the rotate does not fire. Only if useful does `rotateSegment()` run, after which the effective range is re-resolved from the manifest to pick up the extended endpoint.

RangeLimitExceeded is structurally unreachable on the rotate path: `rotateSegment()` merges the contiguous tail into the last manifest range before checking the limit, so range count is unchanged.

Tail dependency guard: rotate-before-compact is suppressed when the active tail contains POP or REWRITE tombstones for records whose source segments fall outside the range being compacted. Rotating and destroying those tombstones would cause those records to resurrect on remount. The guard is tracked in `activeTailAffectedGenerations_`; see the impl doc for details.

### Bounded compaction window (test_main.cpp)

`narrowRange()` in `test_main.cpp` caps the hot-path compaction unit at `kMaxOutputSegs=8` predicted output segments. If `chooseHighestDeadRatio` returns a range whose `predictedOutputSegs()` exceeds the budget, `narrowRange()` retrieves per-segment stats (`store.segmentStats()`), filters to the chosen range, and runs an O(n^2) sliding window over the sorted generations to find the subrange with highest dead ratio that fits within the budget. A single segment always fits, so the function always returns a valid subrange.

The original compaction target is passed to `store.compactRange()` unchanged when it already fits within the budget. The store and `compactRange()` are unchanged; `narrowRange()` is entirely in the test harness.

### O(1) cleanup and preflight (store)

`cleanupAllDanglingSegments()` calls `listFiles` once and deletes all segments whose generation is not referenced by the current manifest in a single pass. This replaced a loop of N calls to `cleanupOneDanglingSegment()`, each of which called `listFiles` independently (O(n_files x n_input_segs) total).

The preflight loop in `compactRange()` uses `sealedSegmentBytes_.find(gen)` instead of `fs()->fileSize()` per segment. `sealedSegmentBytes_` is kept in sync with all segment I/O and provides O(1) lookup; the previous fileSize() path was ~21ms per call on LittleFS.

## On-device validation

### Run 1: burst=100/pop=90%/rec=150B (5 cycles, HighestDeadRatio)

  Workload: burst=100 pop=90% rec=150B cycles=5
  Compactions: 100  NoOps: 0  MaxOutSegs: 1
  MaxLatency: 1090 ms
  Deadlocks: 0  CapExhausted: 0  FinalQueueSize: 12

MaxOutSegs=1 confirms sim prediction. 0 deadlocks, 0 capacity exhaustion.

Latency model note: the 45ms/segment estimate from the sim was write-only. Actual measured latency per `compactRange()` at 150B records and 1 output segment: 200-1090ms. The dominant cost is reading all records in the range, not the write.

### Run 2: burst=500/pop=90%/rec=492B (15 cycles, bounded window)

Run after rotate-before-compact and bounded window landed, before O(1) cleanup and preflight fixes.

  Compactions: 102  NoOps: 30  MaxOutSegs: 9
  MaxLatency: 82580 ms
  Deadlocks: 0  CapExhausted: 0  FinalQueueSize: 56
  PASS

0 deadlocks. MaxLatency=82580ms was dominated by cleanup (77216ms) and preflight fileSize (1601ms) -- both now fixed. A re-run with current code is needed to confirm acceptable latency.

## Outstanding

**Run 3 (worst-case on-device).** Re-run burst=500/pop=90%/rec=492B with O(1) cleanup and preflight fixes in place. Expected: cleanup_ms drops from ~77s to ~2s; pre_size_ms drops from ~1600ms to ~0ms. Confirm MaxLatency is acceptable.

**Final strategy selection.** Deferred until Run 3 confirms MaxLatency. Both HighestDeadRatio and CostBenefit are correct; pick one and remove the other from the sim.

**Capacity exhaustion is out of scope.** Workloads where ratio-based strategies hit the range limit are pure capacity exhaustion (CapExhst=1000, Deadlock=0): the queue is full of live data and no compaction strategy can help. RangeLimitExceeded in this state is correct and expected behaviour.

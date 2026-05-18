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

## Next steps

**1. On-device validation.**
Run HighestDeadRatio and CostBenefit against a representative burst workload on real
LittleFS hardware. Key things to confirm:
- Compaction call latency matches the ~45ms/segment estimate.
- MaxOutSeg on a real burst workload (expect up to ~60 in worst passing case per sim) stays
  within acceptable stall bounds. If not, tune deadRatioTrigger or rangePressureTrigger.
- No regressions in the broader queue behaviour (manifest integrity, correct live record
  replay after compaction).

**2. Final strategy selection.**
Pick one of HighestDeadRatio or CostBenefit. Both pass all recoverable workloads and are
behaviourally identical in the sim at both scaled and real-world parameters. Either is
acceptable; HighestDeadRatio is slightly simpler to reason about.

**Capacity exhaustion is out of scope.** The sim confirms workloads where ratio-based
strategies hit the range limit are pure capacity exhaustion (CapExhst=1000, Deadlock=0):
the queue is full of live data and no compaction strategy can help. The correct fix is
application-level backpressure or a larger queue. `RangeLimitExceeded` in this state is
correct and expected behaviour.

# pqueue Compaction Strategy Investigation

**Editing rules:** ASCII only -- no Unicode symbols (no checkmarks, arrows, emoji). This file is compiled to PDF via LaTeX and non-ASCII characters cause build warnings or missing glyphs.

## Background

v1 compaction is greedy and local: it always picks the oldest full range, reads all live records from it, and writes one output segment. This is correct but not globally optimal. The known failure mode: if the live bytes in the selected range exceed `maxSegmentBytes`, `compactRange()` returns `noOp`. Under sustained enqueue load, ranges routinely exceed one segment of live data. Once range count hits 4 and no range fits in a single output segment, the queue deadlocks permanently.

The goal of this investigation is to find a replacement strategy that eliminates deadlocks under realistic workloads, minimises write amplification, and has a simple enough trigger model to be reliable on-device.

## Simulator

Implementation: `tools/pqueue_compaction_sim.cpp`. Build with `make -j12 sim`, run as `./build/pqueue-compaction-sim`. The full sweep runs in ~13 seconds.

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
- **Deadlock** -- enqueue failures where at least one range had >= 1% dead bytes at the time of failure. These are true strategy failures: compaction could have made progress but did not.
- **CapExhst** -- enqueue failures where no range had >= 1% dead bytes. These are capacity exhaustion: the queue is full of live data and no compaction strategy can help.

### Candidate strategies

Ten strategies are evaluated. All implement the same interface: given per-range stats (total bytes, live bytes, dead bytes) and the current range count, return a range to compact or nullopt.

1. **OldestFirst** -- always picks the first (oldest) range. v1 baseline.
2. **HighestDeadRatio** -- picks the range with the highest dead bytes / total bytes ratio. Skips if no range has dead bytes.
3. **CostBenefit** -- picks the range with the highest dead bytes / live bytes ratio. Minimises write amplification directly.
4. **RangeConsolidation** -- intended to prefer ranges whose compaction reduces range count by merging with an adjacent range. With single-output compaction this is not achievable (new segments get non-adjacent generations), so it falls back to dead-ratio. Becomes meaningful with multi-segment output.
5. **DeferUntilPressure** -- no-op until range count reaches 75% of the limit, then picks highest dead-ratio range.
6. **PressureWeightedHybrid** -- normally uses cost-benefit; falls back to oldest-first when range count reaches 75% of the limit.
7. **DeadByteThreshold** -- oldest-first, but skips if dead ratio is below a configurable threshold. Overrides threshold under emergency pressure.
8. **LookaheadHeuristic** -- cost-benefit, but skips ranges below a 20% dead-ratio floor unless under pressure.
9. **AgeWeightedDeadRatio** -- dead-ratio weighted by position in the range list (older ranges get higher weight).
10. **MinLiveBytesCopied** -- among ranges with any dead bytes, picks the one with the fewest live bytes to copy.

## Findings

### Capacity exhaustion is not a strategy failure

When the queue fills with live data and enqueue growth permanently exceeds drain rate, the range limit is hit with no dead bytes available to reclaim. No compaction strategy can help in this state -- it is equivalent to any bounded queue hitting its size limit. The right response is backpressure or a larger queue, not a smarter compactor.

The Deadlock and CapExhst metrics distinguish the two cases. Ratio-based strategies on overloaded workloads show CapExhst=1000, Deadlock=0: the queue is genuinely full of live data. OldestFirst on the same workloads shows Deadlock=1000, CapExhst=0: it is hitting the range limit while dead data is still present, because it wastes compaction passes on live ranges.

### Usefulness-gating is a hard requirement

For recoverable workloads (burst with pop >= 90%, burst=12/pop=50%), the strategy split is binary:

**Strategies that fail:** OldestFirst, PressureWeightedHybrid, DeadByteThreshold. All show Deadlock=1000 on workloads the other strategies handle cleanly. The common failure mode: these strategies compact nearly-live ranges -- either always (OldestFirst) or under pressure (PressureWeighted, DeadByteThreshold). Compacting a nearly-live range copies all its data for minimal reclaim, burning a compaction pass without meaningfully reducing range pressure. Under sustained load this exhausts range capacity with dead data still present -- a true deadlock.

**Strategies that pass:** HighestDeadRatio, CostBenefit, RangeConsolidation, DeferUntilPressure, MinLiveBytesCopied, AgeWeightedDeadRatio, LookaheadHeuristic. These gate on dead-byte ratio and refuse to compact a mostly-live range. When they hit the limit on overloaded workloads, the CapExhst column shows it is capacity exhaustion, not a strategy failure.

Usefulness-gating is therefore a first-order correctness requirement. Any strategy without it produces true deadlocks under sustained load.

### Differences within the passing family are small

Among the seven passing strategies, write amplification and flash wear are broadly similar on a given workload. The notable observations:

- DeferUntilPressure behaves identically to HighestDeadRatio on almost all workloads. This is structural: without compaction, segment rotations extend a single range's endGen, so range count stays at 1 and the pressure threshold is never triggered. DeferUntilPressure does not proactively compact -- it only acts under emergency pressure, at which point it falls back to dead-ratio selection.
- MinLiveBytesCopied shows slightly better write amplification on some large-burst/fast-pop workloads (e.g. burst=250/pop=90%/rec=62: 8.15x vs 8.63x for HighestDeadRatio/CostBenefit), but the advantage is not consistent across the sweep.
- AgeWeightedDeadRatio shows a mild advantage on burst=60/pop=90%/rec=8 (0.94x vs 1.69x) that does not persist elsewhere.

### Current default candidate

**HighestDeadRatio or CostBenefit with a state-based trigger.**

Both are simple, robust, and pass all recoverable workloads. MinLiveBytesCopied remains in the simulator as a contender; it is not clearly dominant enough to be the default yet.

## Open questions

**Capacity exhaustion workloads are out of scope for compaction strategy.** The sim now confirms that workloads where ratio-based strategies hit the limit are pure capacity exhaustion (CapExhst=1000, Deadlock=0): the queue is full of live data and no compaction algorithm can help. These workloads model a consumer that is structurally too slow for the device's publish rate, or a queue sized too small for the expected burst. The correct fix is application-level backpressure or a larger queue, not a more powerful compactor. `RangeLimitExceeded` in this state is the correct and expected behaviour.

**Multi-segment output.** `compactRange()` currently returns `noOp` when live bytes in the selected range exceed `maxSegmentBytes`. Multi-segment output would allow it to write multiple output segments per pass, gated on the condition that output segment count is strictly less than input segment count (otherwise range count does not improve). This would unlock the deadlocking workloads for strategy evaluation. It is already partially implemented in the store but the gating condition prevents it from firing in the current sim workloads.

If the slow-drain workloads are in scope, multi-segment output is the next step. If they are out of scope and backpressure is the answer there, the current strategy candidates are sufficient and the investigation can move to on-device validation.

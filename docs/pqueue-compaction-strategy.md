# pqueue Compaction Strategy Investigation

**Editing rules:** ASCII only -- no Unicode symbols (no checkmarks, arrows, emoji). This file is compiled to PDF via LaTeX and non-ASCII characters cause build warnings or missing glyphs.

## Purpose

v1 compaction is greedy and local: it always picks the oldest full range, reads all live records from it, and writes one output segment. This is correct but not globally optimal, and has a known failure mode where a range too large to compact into one segment causes a permanent no-op, eventually deadlocking the queue at the 4-range manifest limit.

Before designing a replacement strategy, we need data. The space of possible compaction algorithms is large, the right tradeoffs depend on workload, and the queue is intended for general use -- not just one known use case. This document describes the investigation plan: a simulator, the metrics it will measure, and the candidate algorithms it will evaluate.

## Simulator

Implementation: `tools/pqueue_compaction_sim.cpp`. Build with `make -j12 sim`, run as `./build/pqueue-compaction-sim`. The simulator uses the real AppendLogStore API via a CountingFileSystem wrapper (in `tools/counting_file_system.h`) that tracks bytes written for flash wear measurement.

A Posix-backed workload simulator that drives the queue through configurable enqueue/pop patterns and observes compaction behavior. Runs on desktop, no device required. Fast enough to sweep a large parameter space.

### Workload parameters

- Enqueue rate and burst size
- Pop rate and burst size (relative to enqueue rate, to control queue fill level)
- Record size distribution (fixed, uniform random, bimodal)
- Workload duration (total number of operations)

The simulator sweeps across combinations of these parameters to cover the range of unknown real-world usage patterns. A strategy that performs well across the sweep is more trustworthy than one tuned to a single workload.

### Pluggable strategy interface

Each candidate algorithm implements the same interface: given the current store state (segment files, dead-byte counts per segment, manifest ranges, free space), return a compaction action. The simulator drives the store and calls the strategy at each decision point. Strategies are swapped without changing anything else.

### On-device validation

The simulator uses the Posix backend, so its cost model is based on operation counts rather than real LittleFS timings. The best-performing simulator candidates are run on-device against representative workloads to confirm that real LittleFS flush costs match the simulator's assumptions. This guards against optimising for a model that does not reflect reality.

## Metrics

Each simulator run records the following metrics across the full workload sweep:

**Write amplification** -- live bytes copied per dead byte reclaimed. Lower is better. A strategy that copies large amounts of live data to reclaim small amounts of dead data causes unnecessary flash wear.

**Flash wear** -- total bytes written to disk over the workload lifetime, including all segment writes and manifest publishes. The primary long-term health metric for flash storage.

**Range pressure** -- distribution of range count over time: mean, peak, and time spent near the 4-range limit. A strategy that routinely operates close to the limit is risky even if it never deadlocks during the simulation.

**Compaction frequency** -- how often compaction must be invoked to keep the queue healthy (i.e. to avoid range limit or free space exhaustion). Lower frequency means less interference with normal queue operations.

**Deadlock frequency** -- how often the queue became unable to make progress (range limit hit with no compactable range). Any non-zero value is a failure.

**Compaction cost per pass** -- number of output segment files written per compaction call. Directly proportional to latency on-device.

**Maximum single-pass stall cost** -- worst-case output segments written in a single compaction call across the full workload. On embedded, a strategy that is globally efficient but occasionally writes 6 segments in one pass produces a ~270 ms stall, which may be unacceptable regardless of average efficiency. This metric catches outliers that average cost hides.

## Candidate algorithms

All candidates implement the same interface and are evaluated against the same workload sweep. The goal is to find which approach gives the best tradeoff across the metrics above.

### 1. Oldest-first (v1 baseline)

Pick the oldest full range. If live bytes fit in one output segment, compact it. Otherwise no-op. Included as the baseline to measure improvement against.

### 2. Highest dead-byte ratio

Score each range by dead bytes / total bytes. Pick the highest scorer. Skips ranges that are mostly live, avoiding wasteful copying.

### 3. Cost-benefit ratio

Score each range by dead bytes reclaimed per live byte copied. Minimises write amplification directly rather than maximising absolute reclaim.

### 4. Range-consolidation-first

Ignore dead-byte ratios entirely. Pick the compaction that reduces range count the most -- i.e. prefer ranges that, after compaction, can be merged with an adjacent range. Trades write efficiency for manifest headroom.

### 5. Defer until pressure

Do nothing until forced: either the range count approaches the limit or free space falls below a threshold. Then compact aggressively. The hypothesis is that waiting allows more records to be popped, reducing live bytes copied when compaction finally runs.

### 6. Pressure-weighted hybrid

Normally picks by cost-benefit ratio. As range pressure increases (range count approaching limit), shifts weight toward range-consolidation. Tries to balance efficiency with safety.

### 7. Oldest-first with dead-byte threshold

Like v1, but skips ranges whose dead-byte ratio falls below a configurable threshold. Simple improvement over baseline that prevents recompacting nearly-live ranges.

### 8. Lookahead heuristic

Score ranges not by current dead-byte ratio but by projected ratio after N more operations (estimated from recent pop rate). A range that is 50% dead now but trending toward 80% dead may be better deferred. Requires tracking recent operation history.

### 9. Age-weighted dead-byte ratio

Weight the dead-byte ratio by segment age. Older segments are more likely to continue accumulating dead records, so their current ratio underestimates their future value. Combines reclaim potential with age signal.

### 10. Minimum live-bytes-copied

Among all ranges that would result in range count reduction after compaction, pick the one with fewest live bytes to copy. Directly minimises the cost of the next compaction step while still making manifest progress.

## Simulation findings (initial run)

### Scheduler cadence dominates strategy quality

The first simulation runs revealed that the compaction trigger cadence matters more than the choice of strategy. With aggressive cadence (compact every 6 enqueues), most compaction calls are no-ops or useless -- one run showed 820 out of 827 compaction attempts producing no useful work. With relaxed cadence (compact every 15 enqueues), all strategies performed similarly and none deadlocked.

This is a strong signal: operation-count-triggered compaction is the wrong primitive. Firing compaction on a fixed schedule regardless of queue state produces pathological behaviour under load -- either compacting fully-live ranges uselessly, or invoking compaction hundreds of times with no effect.

### Strategy quality matters under pressure

Despite the scheduler noise, the gap between strategies under heavy load was large and meaningful. In the enqP=0.65, compact=6 workload, oldest-first produced 206x write amplification while ratio-based strategies (HighestDeadRatio, CostBenefit) produced 2.36x. The difference: oldest-first compacts fully-live ranges, copying all their data for zero reclaim. Ratio-based strategies correctly refuse ranges with no dead bytes.

This confirms that usefulness-gating -- skipping compaction when there is nothing worth reclaiming -- is a first-order correctness requirement, not an optimisation.

### Oldest-first is unsuitable as a long-term strategy

Under any workload with range pressure, oldest-first repeatedly compacts ranges with no dead data. This is pure write amplification with no benefit, and it directly causes the deadlock scenario (range limit hit while compacting live data for no gain). It is retained in the simulator as a baseline only.

### DeferUntilPressure avoids deadlocks because ranges never multiply without compaction

DeferUntilPressure showed MaxRange=1 and zero deadlocks across all workloads. This is explained by a structural property of the store: rotateSegment() merges contiguous generation numbers into the same manifest range. Without compaction, all segment rotations extend a single range's endGen, so range count stays at 1 indefinitely. Compaction is what creates non-contiguous output generations, producing new ranges. DeferUntilPressure therefore never encounters range pressure in practice -- its threshold is never triggered. This is not a strategy win; it is a degenerate case where the strategy never acts and the store never accumulates range pressure to force action.

### v1 single-output limit makes strategy evaluation impossible under pressure

In all workloads with enqueueProb >= 0.55, strategies that compact (all except DeferUntilPressure) produced 4900+ deadlocks out of 5000 operations. The root cause is not strategy choice -- it is the v1 hard limit: if live bytes in the chosen range exceed maxSegmentBytes, compactRange() returns noOp(). Under sustained enqueue load, live bytes in a range routinely exceed one segment. Once range count hits 4 and no range fits in a single output segment, the queue deadlocks permanently regardless of strategy.

All ratio-based strategies (HighestDeadRatio, CostBenefit, AgeWeighted, MinLiveBytesCopied, LookaheadHeuristic) deadlocked identically. They cannot be differentiated by the simulator until multi-segment compaction output is implemented, because they are all blocked by the same v1 constraint.

### Key conclusions

1. The compaction trigger must be state-based, not operation-count-based. A rising-edge trigger -- fire on segment rotation or when range count approaches the limit -- collapses the no-op rate and is implemented in the current simulator.

2. Any viable strategy must gate on usefulness: refuse to compact a range whose dead-byte ratio is below a minimum threshold, except when range count pressure forces action. Oldest-first fails this test catastrophically (2000x+ write amplification vs 2.7x for ratio-based strategies).

3. Strategy evaluation is blocked by the v1 single-output compaction limit. Multi-segment output (allowing compactRange to write multiple output segments when live data exceeds one segment) is a prerequisite for meaningful simulator differentiation under realistic load.

4. Once multi-segment output exists, the simulator workloads and trigger logic are sound. Ratio-based strategies are the correct direction; the simulator is ready to distinguish between them once the v1 bottleneck is removed.

## Next steps

### Step 1: Re-run simulator and record findings (multi-segment output is now implemented)

`compactRange()` now partitions live records across multiple output segments when `liveBytes > maxSegmentBytes`. Partitioning uses a greedy fill: records are packed into segments sequentially until the next record would exceed `maxSegmentBytes`, then a new output segment is started. Greedy fill is simple and correct, but it does not minimise the number of output segments in all cases (e.g., bin-packing could achieve fewer segments with unequal record sizes). This is an intentional design choice accepted for v2; whether a smarter packing strategy is worth the added complexity should be re-evaluated once simulator data shows how often bin-packing would produce a materially different result.

A key constraint on multi-segment output: if the number of output segments would equal or exceed the number of input segments, compaction is skipped (returns noOp). Compacting a range without reducing its segment count provides no manifest headroom benefit, and it fragments the generation space in a way that breaks contiguous-range merging during subsequent rotations, leading to unbounded range count growth. Multi-segment output is therefore only performed when it strictly reduces segment count (i.e., dead bytes in the range allow at least one segment to be eliminated).

### Initial re-run findings (after multi-segment output)

Simulator results after implementing multi-segment output are UNCHANGED from before. Deadlock counts remain at 4932-4940 across all workloads. The root cause is a structural mismatch between the simulator workload and the conditions under which multi-segment output fires:

With `recordSize=64` and `maxSegmentBytes=512`, each segment holds exactly 5 records (floor((512-20)/88) = 5). For a 3-segment range (15 records) to produce fewer than 3 output segments, fewer than 11 records must be live -- requiring more than 33% of records in the range to be popped. The simulator triggers compaction at 25% dead ratio (by bytes), which fires when only 3-4 records in a 15-record range are dead. At that point, 11+ live records still require 3 output segments (outputSegs >= inputSegCount → noOp). Multi-segment output therefore never fires in the current workload.

This is not a bug -- the gating condition is correct. But it means the simulator workload needs adjustment before multi-segment output can be evaluated. Specifically: the dead-ratio trigger must be raised to at least ~35% (the threshold where a full segment can be eliminated from the oldest range), or the record size must be reduced relative to segment size (fewer records per segment means fewer pops are needed to free one segment).

### Step 2: Tune simulator and record findings

Adjust the simulator workload to produce conditions where multi-segment output fires:
- Raise `deadRatioTrigger` to 0.40-0.50 (allow enough dead records to accumulate before triggering), OR
- Reduce `recordSize` relative to `maxSegmentBytes` so fewer pops are needed to eliminate a segment

Re-run and record which strategies minimise write amplification and whether deadlocks collapse. Update `docs/pqueue-append-log-design.md` future work section to reflect what is still outstanding.

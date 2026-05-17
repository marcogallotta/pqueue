# pqueue Compaction Strategy Investigation

**Editing rules:** ASCII only -- no Unicode symbols (no checkmarks, arrows, emoji). This file is compiled to PDF via LaTeX and non-ASCII characters cause build warnings or missing glyphs.

## Purpose

v1 compaction is greedy and local: it always picks the oldest full range, reads all live records from it, and writes one output segment. This is correct but not globally optimal, and has a known failure mode where a range too large to compact into one segment causes a permanent no-op, eventually deadlocking the queue at the 4-range manifest limit.

Before designing a replacement strategy, we need data. The space of possible compaction algorithms is large, the right tradeoffs depend on workload, and the queue is intended for general use -- not just one known use case. This document describes the investigation plan: a simulator, the metrics it will measure, and the candidate algorithms it will evaluate.

## Simulator

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

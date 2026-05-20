# pqueue Compaction Strategy

**Editing rules:** ASCII only -- no Unicode symbols (no checkmarks, arrows, emoji). This file is compiled to PDF via LaTeX and non-ASCII characters cause build warnings or missing glyphs.

## Problem

v1 compaction always picked the oldest full range and wrote one output segment. The failure mode: under sustained enqueue load, ranges accumulate more live data than fits in one segment. Once range count hits 4 and no range fits in a single output segment, the queue deadlocks permanently.

## Strategy

**HighestDeadRatio** is the selected strategy. It picks the range with the highest dead/total byte ratio, skipping if no range has dead bytes. All evaluated alternatives either produced true deadlocks under load or were mathematically identical to HighestDeadRatio (dead/total and dead/live rank ranges identically -- both are monotonically increasing functions of dead bytes given fixed total).

Wired into `chooseCompactionRange()` in the store. `compactOneSegment()` calls `chooseCompactionRange()` then applies `narrowRange()` to bound stall latency.

### Why usefulness-gating is required

Any strategy that compacts live ranges (OldestFirst, PressureWeightedHybrid, DeadByteThreshold) produces true deadlocks under recoverable workloads. Compacting a fully-live range consumes a compaction pass and a range slot without reclaiming anything, while dead data accumulates elsewhere. HighestDeadRatio gates on dead bytes and refuses to touch fully-live ranges.

### Capacity exhaustion is not a strategy failure

When the queue fills with live data and enqueue growth permanently exceeds drain rate, no compaction strategy can help -- it is equivalent to any bounded queue hitting its size limit. The right response is application-level backpressure or a larger queue. The Deadlock/CapExhst metrics distinguish true deadlocks (compaction could have helped but did not) from capacity exhaustion (no dead bytes to reclaim).

## Implementation

### Subrange compaction (store)

`compactRange()` accepts any subrange `[startGen, endGen]` that is contained within a single manifest range. The subrange is classified as exact (matches parent), prefix (shares parent's left endpoint), suffix (shares parent's right endpoint), or middle (strictly interior). On compaction, the parent range is spliced: remainders on each side are preserved as new manifest ranges, and the output range is inserted in between. Two-sided contiguity merge follows the splice. A dead subrange (no live records in `[startGen, endGen]`) is removed without writing output; remainders stay intact.

**Range-count gate.** Splitting a parent produces at most +2 new ranges (middle case). Before any I/O the gate checks `manifestRanges_.size() + gateDelta > kManifestMaxRanges`. `gateDelta` is 2 for a live middle, 1 for a live prefix/suffix, 0 for an exact match. Dead prefix/suffix use gateDelta=0 (no new ranges needed); dead middle uses 1. If the gate fires and `AllowFullRangeFallback::yes`, `compactRange` expands to the full parent and continues. If `AllowFullRangeFallback::no` (default for both `compactRange` and `compactOneSegment` on the write path), it returns noOp immediately. Maintenance paths (`compactIdle`, `compactFull`) pass `yes` explicitly. A second pre-rotate gate evaluates the post-rotate shape (suffix becomes exact after tail merge) before calling `rotateSegment()`; this ensures noOp is returned before any state mutation if the post-rotate split would overflow. The gate fires only when `hypoHasLive` is true (a RAM scan for live records in `[inputRange.startGen, activeGeneration_]`): a pop-only tail means rotate will not fire and dead-suffix removal uses gateDelta=0, so the live split delta must not apply.

### Bounded output window (store)

`narrowRange()` is a private store method called from `compactOneSegment()`. It caps the compaction unit at `AppendLogConfig::maxOutputSegments` (default 8) predicted output segments. Narrowing only applies when the full chosen range exceeds the budget; if it fits, the full range is used unchanged. When narrowing, an O(n^2) sliding window over per-segment stats finds the contiguous subrange with the highest dead ratio that fits within the budget. A single segment is always the minimum unit.

### Rotate-before-compact (store)

When the selected range is the last manifest range and the active tail is contiguous with it, `compactRange()` seals the tail into the compaction input before writing output. Without this, output generations land numerically above the active tail, breaking contiguity and causing rapid range fragmentation that forces compaction of large live ranges.

Design: preflight-before-rotate. Usefulness is evaluated on the hypothetical extended range using only in-memory sizes, without mutating state. The rotate only fires if the compaction would be useful. RangeLimitExceeded is structurally unreachable on the rotate path: merging the contiguous tail into the last range before checking the limit leaves range count unchanged.

Tail dependency guard: rotate-before-compact is suppressed when the active tail contains POP or REWRITE tombstones for records whose source segments fall outside the range being compacted. Rotating and destroying those tombstones would resurrect records on remount. Tracked in `activeTailAffectedGenerations_`; see the impl doc for details.

### O(1) preflight sizing (store)

The preflight loop in `compactRange()` uses `sealedSegmentBytes_.find(gen)` for O(1) size lookup instead of `fs()->fileSize()` per segment (~21ms each on LittleFS).

### Bulk segment reads in collectLiveRecords (store)

`collectLiveRecords()` groups live records by segment generation and reads each segment file once with `readFile`, slicing payloads by offset. This replaced one `readAt` call per live record. With 66 live records across 8 input segments: 66 readAt calls -> 8 readFile calls.

### Targeted input-segment cleanup (store)

After a compaction publishes its manifest, the retired input segment files are known exactly (effectiveRange.startGen..effectiveRange.endGen). `cleanupInputSegments()` removes them directly by name, using `sealedSegmentBytes_` for size accounting. This replaced `cleanupAllDanglingSegments()` which called `listFiles()` once per compaction and `fileSize()` once per file. With N files in the directory at compaction time, the old path cost ~100ms + 12.5ms/file on device; the new path pays only removeFile (~24ms) per retired input segment, with no directory scan.

## Simulator

`tools/pqueue_compaction_sim.cpp`. Build: `make -j12 sim`. Run: `./build/pqueue-compaction-sim`. Full sweep in ~2 seconds (in-memory FS, early abort at 1000 failures).

Drives the real `AppendLogStore` API through two workload families:

**Random interleaved.** enqP in {0.55, 0.65, 0.80}, record size 19 bytes.

**Burst.** Models offline-consumer pattern: enqueue N, drain popRatio fraction, repeat. burstSize in {12, 60, 250}, popRatio in {0.25, 0.5, 0.9}, recordSize in {8, 19, 62} bytes. Parameters scaled ~1/8 from production values to keep runs fast while preserving records-per-segment ratio.

Compaction trigger: rising-edge on segment count (new segment written), fires if any range exceeds `deadRatioTrigger` or range count reaches `rangePressureTrigger`. Segment count is the correct rising edge -- range count stays at 1 with a single contiguous range.

## Posix profiler (preferred iteration method for performance work)

`tools/pqueue_profiling.cpp` has two compaction modes. Both use an in-memory FS and complete in under a second. Use these to iterate before running on-device.

  make -j12 profiling
  ./build/pqueue-profiling compaction <burst> <payloadBytes> <cycles>
  ./build/pqueue-profiling compaction-sim <burst> <payloadBytes> <cycles>
  ./build/pqueue-profiling idle <burst> <payloadBytes> <cycles>
  ./build/pqueue-profiling idle-sim <burst> <payloadBytes> <cycles>

`compaction`: reports I/O op counts (readFile, readAt, writeFile, listFiles, removeFile) and wall-clock latency. Use for correctness and op-count regression.

`compaction-sim`: accumulates simulated latency using per-op LittleFS cost estimates (no sleeping -- pure arithmetic). Use to predict on-device MaxLatency before flashing.

`idle` / `idle-sim`: models the burst->drain->compactIdle pattern. Phase 1 enqueues a burst (detecting hot-path compactions via readFile delta -- rotations produce no readFile, compactions do). Phase 2 drains 90% of records. Phase 3 calls `compactIdle(1)` in a loop until noOp, measuring per-step latency. Reports idleSteps, idleNoOps, maxStepLatency, totalIdleLatency, and hotCompactions. Use to verify the clean-storage invariant: hotCompactions should be 0 if idle compaction fully cleans up between cycles.

Simulated latency model (`littleFsSimLatency()`, scaled 100x from observed device timings):

  readFile/readAt: 345us   (~35ms on device, open+read+close)
  writeFile:       1380us fixed + 518us/KB  (~138ms + ~52ms/KB on device)
  writeAt:         138us   (~14ms on device)
  removeFile:      166us   (~17ms on device)
  listFiles:       690us base + 86us/file  (~69ms + ~8.6ms/file on device)

writeFile and listFiles use variable costs because data size and directory size affect LittleFS GC pressure and scan time. Calibrated from Run 4 (burst=500/pop=90%/rec=492B): simMaxLatency=48.6ms x 100 = 4860ms vs actual 4861ms. Prior constants overestimated by ~45% for write-heavy workloads; rescaled uniformly by 0.69. Individual op multipliers may differ -- next calibration round should measure ops independently.

With subrange compaction enabled (same workload, same model): simMaxLatency=41.6ms, predicting ~4160ms on device (~14% improvement). On-device validation pending.

**Idle compaction sim results (subrange compaction + compactIdle pattern):**

  burst=500 payload=492B cycles=3 pop=90%:  idleSteps=10  maxStepLatency=48.5ms  totalIdle=154.4ms  hotCompactions=0
  burst=100 payload=150B cycles=5 pop=90%:  idleSteps=18  maxStepLatency=10.0ms  totalIdle=65.6ms   hotCompactions=0

hotCompactions=0 confirms the clean-storage invariant holds: idle compaction between cycles fully cleans the queue, so subsequent enqueue bursts never trigger hot-path compaction. maxStepLatency for the heavy workload predicts ~4850ms per idle step on device; this is a worst-case single step, not per-burst latency.

## On-device validation

Test: `tests/arduino/test_pqueue_compaction/test_main.cpp`. Environment: `esp32s3-compaction`. Build and upload: `~/venvs/esp/bin/pio test -e esp32s3-compaction --without-testing`.

**Run 1: burst=100/pop=90%/rec=150B, 5 cycles**

  Compactions: 100  NoOps: 0  MaxOutSegs: 1
  MaxLatency: 1090 ms  Deadlocks: 0  CapExhausted: 0

**Run 2: burst=500/pop=90%/rec=492B, 15 cycles** (pre O(1) cleanup and preflight fix)

  Compactions: 102  NoOps: 30  MaxOutSegs: 9
  MaxLatency: 82580 ms  Deadlocks: 0  CapExhausted: 0

MaxLatency dominated by cleanup (77216ms) and preflight fileSize (1601ms). Both now fixed.

**Run 3: burst=500/pop=90%/rec=492B, 3 cycles** (O(1) cleanup + preflight, pre bulk-read fix)

  Compactions: 17  NoOps: 6  MaxOutSegs: 8
  MaxLatency: 10652 ms  Deadlocks: 0  CapExhausted: 0

Confirmed: cleanup and preflight fixed. MaxLatency dominated by collect (per-record readAt). Now fixed via bulk readFile in collectLiveRecords.

**Run 4: burst=500/pop=90%/rec=492B, 3 cycles** (bulk-read + targeted cleanup)

  Compactions: 17  NoOps: 6  MaxOutSegs: 8
  MaxLatency: 4861 ms  Deadlocks: 0  CapExhausted: 0

Predicted 7.1s (simMaxLatency=70.5ms x 100); actual 4.9s. Sim overestimated by ~45% for this workload -- the x100 calibration factor is conservative. Improvement over Run 3: 10652ms -> 4861ms (~54% reduction).

## Workload characterisation and compaction trigger model

Understanding when compaction is useful requires understanding the actual usage pattern.

Enqueue is driven by backend failures: either brief and intermittent, or a sustained outage producing a burst. Pop is a rapid-fire drain, typically after the backend recovers, throttled by the caller. The two phases rarely overlap significantly. During a pure enqueue burst there is no dead data (nothing has been popped), so compaction has nothing to reclaim and should be a no-op. During a drain burst, pops create dead data but no new enqueues are arriving.

**Key consequence: pops do not currently trigger compaction.** `needsCompaction()` is only checked in `writeRecord`. The drain phase never stalls for compaction, which is correct for latency. But it also means dead data from a completed drain sits unreclaimd until the next enqueue triggers pressure. If a new enqueue burst arrives before compaction has run, it hits the stall.

**The clean-storage invariant.** If the queue is fully compacted before an enqueue burst, the burst writes only new live data, stays within maxSegments, and never triggers compaction at all. The stall during enqueue is a symptom of carrying dead data into the burst because the previous drain left it behind. Fix the drain phase, enqueue phase fixes itself.

**Background compaction fits poorly here.** LittleFS is not concurrent: a background compaction task would still serialize against foreground I/O, adding context-switch overhead without real parallelism. During an active burst (enqueue or drain) there is no gain. The productive window is idle time between phases. Compaction during that window can be driven by the application, which already knows when the backend has recovered and the drain has finished. `compactFull()` called at that boundary is the right mechanism.

## Future work

**Configurable kManifestMaxRanges.** The current manifest format (30B fixed + 4x8B = 62B) fits within the LittleFS 64-byte inline threshold. For devices with larger flash (e.g. 16MB) queueing megabytes of data, compaction latency scales with queue size and can become a data-loss mechanism: if the store cannot compact fast enough, QueueFull drops records. More ranges (e.g. 8 ranges = 94B) enable more subrange splits and tighter per-step latency bounds at scale. Make `kManifestMaxRanges` a config field with default 4. Deferred: touches the manifest binary format, requires a version bump, and expands the test matrix significantly.

**Cost-aware compaction strategy.** Score ranges by `bytes_reclaimed / estimated_compaction_ms`, where estimated cost is derived from the latency model (readFile x readFileUs + writeFile x writeFileUs + ...). Naturally avoids large nearly-live ranges when stall budget is tight. Lower priority while HighestDeadRatio continues to perform well in practice.

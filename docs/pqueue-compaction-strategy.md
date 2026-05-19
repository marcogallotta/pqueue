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

### Bounded output window (store)

`narrowRange()` is a private store method called from `compactOneSegment()`. It caps the compaction unit at `AppendLogConfig::maxOutputSegments` (default 8) predicted output segments. Narrowing only applies when the full chosen range exceeds the budget; if it fits, the full range is used unchanged. When narrowing, an O(n^2) sliding window over per-segment stats finds the contiguous subrange with the highest dead ratio that fits within the budget. A single segment is always the minimum unit.

### Rotate-before-compact (store)

When the selected range is the last manifest range and the active tail is contiguous with it, `compactRange()` seals the tail into the compaction input before writing output. Without this, output generations land numerically above the active tail, breaking contiguity and causing rapid range fragmentation that forces compaction of large live ranges.

Design: preflight-before-rotate. Usefulness is evaluated on the hypothetical extended range using only in-memory sizes, without mutating state. The rotate only fires if the compaction would be useful. RangeLimitExceeded is structurally unreachable on the rotate path: merging the contiguous tail into the last range before checking the limit leaves range count unchanged.

Tail dependency guard: rotate-before-compact is suppressed when the active tail contains POP or REWRITE tombstones for records whose source segments fall outside the range being compacted. Rotating and destroying those tombstones would resurrect records on remount. Tracked in `activeTailAffectedGenerations_`; see the impl doc for details.

### O(1) cleanup and preflight (store)

`cleanupAllDanglingSegments()` calls `listFiles` once and deletes all unreferenced segments in a single pass. The preflight loop in `compactRange()` uses `sealedSegmentBytes_.find(gen)` for O(1) size lookup instead of `fs()->fileSize()` per segment (~21ms each on LittleFS).

### Bulk segment reads in collectLiveRecords (store)

`collectLiveRecords()` groups live records by segment generation and reads each segment file once with `readFile`, slicing payloads by offset. This replaced one `readAt` call per live record. With 66 live records across 8 input segments: 66 readAt calls -> 8 readFile calls.

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

`compaction`: reports I/O op counts (readFile, readAt, writeFile, listFiles, removeFile) and wall-clock latency. Use for correctness and op-count regression.

`compaction-sim`: accumulates simulated latency using per-op LittleFS cost estimates (no sleeping -- pure arithmetic). Use to predict on-device MaxLatency before flashing.

Simulated latency model (`littleFsSimLatency()`, scaled 100x from observed device timings):

  readFile/readAt: 500us   (~50ms on device, open+read+close)
  writeFile:       2000us fixed + 750us/KB  (~200ms + ~75ms/KB on device)
  writeAt:         200us   (~20ms on device)
  removeFile:      240us   (~24ms on device)
  listFiles:       1000us base + 125us/file (~100ms + ~12.5ms/file on device)

writeFile and listFiles use variable costs because data size and directory size affect LittleFS GC pressure and scan time. The model is calibrated: simMaxLatency x 100 matches measured on-device MaxLatency within ~10%.

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

**Run 4 (pending):** Re-run burst=500/pop=90%/rec=492B after bulk-read fix. Predicted MaxLatency: ~8.2s (profiler simMaxLatency=82ms x 100).

## Outstanding

**Run 4 (on-device).** Validate collectLiveRecords bulk-read improvement on real LittleFS.

**Future idea: cost-aware strategy.** Score ranges by `bytes_reclaimed / estimated_compaction_ms`, where estimated cost is derived from the latency model (readFile x readFileUs + writeFile x writeFileUs + listFiles x listFilesBaseUs + ...). Naturally avoids large nearly-live ranges when stall budget is tight. The posix profiler's latency model provides the per-op constants without on-device guesswork.

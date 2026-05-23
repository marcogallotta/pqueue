# pqueue Compaction Strategy

**Editing rules:** ASCII only -- no Unicode symbols (no checkmarks, arrows, emoji). This file is compiled to PDF via LaTeX and non-ASCII characters cause build warnings or missing glyphs.

## Strategy

**HighestDeadRatio** picks the range with the highest dead/total byte ratio,
skipping any range with no dead bytes. All evaluated alternatives either
produced true deadlocks under load or were mathematically identical to
HighestDeadRatio (dead/total and dead/live rank ranges identically -- both are
monotonically increasing functions of dead bytes given fixed total).

### Why usefulness-gating is required

Any strategy that compacts live ranges (OldestFirst, PressureWeightedHybrid,
DeadByteThreshold) produces true deadlocks under recoverable workloads.
Compacting a fully-live range consumes a compaction pass and a range slot
without reclaiming anything, while dead data accumulates elsewhere.
HighestDeadRatio gates on dead bytes and refuses to touch fully-live ranges.

### Capacity exhaustion is not a strategy failure

When the queue fills with live data and enqueue growth permanently exceeds drain
rate, no compaction strategy can help -- it is equivalent to any bounded queue
hitting its size limit. The right response is application-level backpressure or
a larger queue. The Deadlock/CapExhst metrics distinguish true deadlocks
(compaction could have helped but did not) from capacity exhaustion (no dead
bytes to reclaim).

## Implementation

### Subrange compaction

`compactRange()` accepts any subrange `[startGen, endGen]` contained within a
single manifest range. The subrange is classified as exact (matches parent),
prefix (shares parent's left endpoint), suffix (shares parent's right
endpoint), or middle (strictly interior). On compaction, the parent range is
spliced: remainders on each side are preserved as new manifest ranges, and the
output range is inserted in between. Two-sided contiguity merge follows the
splice. A dead subrange (no live records in `[startGen, endGen]`) is removed
without writing output; remainders stay intact.

Range-count gate, fallback expansion, rotate-before-compact, tail dependency
guard, and the no-op gate are described in `docs/pqueue-append-log-impl.md`.

### Bounded output window

`narrowRange()` caps the compaction unit at `AppendLogConfig::maxOutputSegments`
(default 8) predicted output segments. When the full chosen range exceeds the
budget, an O(n^2) sliding window over per-segment stats finds the contiguous
subrange with the highest dead ratio that fits within the budget. A single
segment is always the minimum unit.

### O(1) preflight sizing

The preflight loop in `compactRange()` uses `sealedSegmentBytes_.find(gen)` for
O(1) size lookup instead of `fs()->fileSize()` per segment (~21ms each on
LittleFS).

### Bulk segment reads in collectLiveRecords

`collectLiveRecords()` groups live records by segment generation and reads each
segment file once with `readFile`, slicing payloads by offset. For 66 live
records across 8 input segments this is 8 readFile calls instead of 66 readAt
calls.

### Targeted input-segment cleanup

After a compaction publishes its manifest, `cleanupInputSegments()` removes
retired input segments directly by name using `sealedSegmentBytes_` for size
accounting. No directory scan is required; cost is one `removeFile` (~24ms on
device) per retired segment.

## Simulator

`tools/pqueue_compaction_sim.cpp`. Build: `make -j12 sim`. Run:
`./build/pqueue-compaction-sim`. Full sweep in ~2 seconds (in-memory FS, early
abort at 1000 failures).

Drives the real `AppendLogStore` API through two workload families:

**Random interleaved.** enqP in {0.55, 0.65, 0.80}, record size 19 bytes.

**Burst.** Models offline-consumer pattern: enqueue N, drain popRatio fraction,
repeat. burstSize in {12, 60, 250}, popRatio in {0.25, 0.5, 0.9}, recordSize
in {8, 19, 62} bytes. Parameters scaled ~1/8 from production values to keep
runs fast while preserving records-per-segment ratio.

Compaction trigger: rising-edge on segment count (new segment written), fires
if any range exceeds `deadRatioTrigger` or range count reaches
`rangePressureTrigger`. Segment count is the correct rising edge -- range count
stays at 1 with a single contiguous range.

## Posix profiler

`tools/pqueue_profiling.cpp`. Build: `make -j12 profiling`. All modes use an
in-memory FS and complete in under a second.

```
./build/pqueue-profiling compaction <burst> <payloadBytes> <cycles> [flags]
./build/pqueue-profiling compaction-sim <burst> <payloadBytes> <cycles> [flags]
./build/pqueue-profiling idle <burst> <payloadBytes> <cycles> [flags]
./build/pqueue-profiling idle-sim <burst> <payloadBytes> <cycles> [flags]
```

`compaction`: I/O op counts and wall-clock latency. Use for correctness and
op-count regression.

`compaction-sim`: simulated latency using per-op LittleFS cost estimates (no
sleeping). Use to predict on-device MaxLatency before flashing.

`idle` / `idle-sim`: models the burst->drain->compactIdle pattern. Phase 1
enqueues a burst, detecting hot-path compactions via readFile delta (rotations
produce no readFile; compactions do). Phase 2 drains popRatio fraction. Phase 3
calls `compactIdle(1)` in a loop until noOp, measuring per-step latency.
Reports idleSteps, idleNoOps, maxStepLatency, totalIdleLatency, and
hotCompactions. `hotCompactions = 0` confirms the clean-storage invariant: idle
compaction between cycles prevents hot-path compaction during the next burst.

**Flags** (apply to all modes):

```
--pop <pct>                drain percentage per cycle (default 90)
--multiplier <f>           scale sim latency constants (default 1.0)
--max-segment-bytes <n>    cfg.maxSegmentBytes (default 4096)
--max-total-bytes <n>      cfg.maxTotalBytes (default 1048576)
--max-segments <n>         cfg.maxSegments (default 200)
--max-output-segments <n>  cfg.maxOutputSegments (default 8)
--min-free-bytes <n>       cfg.minFreeBytes: FS floor; enqueue fails if freeBytes drops below n (default 0 = disabled)
--fs-total-bytes <n>       simulated FS total size in bytes (default 0 = effectively unlimited)
```

`--fs-total-bytes` and `--min-free-bytes` model the real safety floor: on device, LittleFS free space is shared
with other files, metadata, and logs, so the queue may be rejected by the FS floor before its own footprint
cap (`maxTotalBytes`) is hit. Without these flags the sim cannot detect this failure mode. Results report
`fsFloorHit` (enqueues rejected by FS floor) separately from `capExhausted` (rejected by queue footprint
cap) so the two failure modes are distinguishable.

**Simulated latency model** (`littleFsSimLatency()`, scaled 100x from observed
device timings):

```
readFile/readAt: 345us   (~35ms on device, open+read+close)
writeFile:       1380us fixed + 518us/KB  (~138ms + ~52ms/KB on device)
writeAt:         138us   (~14ms on device)
removeFile:      166us   (~17ms on device)
listFiles:       690us base + 86us/file  (~69ms + ~8.6ms/file on device)
```

Calibrated for ESP32S3 with QSPI flash: simMaxLatency x 100 = predicted device
ms at `--multiplier 1.0`. Calibrated from on-device Run 4 (burst=500/pop=90%/
rec=492B/cycles=3): simMaxLatency=48.6ms x 100 = 4860ms, actual 4861ms.

**Idle compaction sim results** (burst->drain->compactIdle pattern):

```
burst=500 payload=492B cycles=3 pop=90%:  idleSteps=10  maxStep=48.5ms  total=154.4ms  hotCompactions=0
burst=100 payload=150B cycles=5 pop=90%:  idleSteps=18  maxStep=10.0ms  total=65.6ms   hotCompactions=0
```

Predicted max step on device (x100): 4850ms (heavy), 1000ms (light). Total
idle budget: ~15400ms (heavy, 3 cycles), ~6560ms (light, 5 cycles).

## On-device validation

Test: `tests/arduino/test_pqueue_compaction/test_main.cpp`.
Environment: `esp32s3-compaction`.
Build and upload: `~/venvs/esp/bin/pio test -e esp32s3-compaction --without-testing`.

**Results** (ESP32S3, QSPI flash):

| Config | Compactions | NoOps | MaxOutSegs | MaxLatency | Deadlocks | CapExhausted |
|---|---|---|---|---|---|---|
| burst=100/pop=90%/rec=150B/5cy | 100 | 0 | 1 | 1090ms | 0 | 0 |
| burst=500/pop=90%/rec=492B/3cy | 17 | 6 | 8 | 4861ms | 0 | 0 |

The heavy workload (Run 4) was the calibration baseline for the sim model. Prior
to O(1) preflight sizing, bulk collectLiveRecords reads, and targeted cleanup,
the same workload produced 82580ms MaxLatency; the bottlenecks were directory
scans and per-record fileSize calls on the hot path.

## Workload characterisation

Enqueue is driven by backend failures: brief and intermittent, or a sustained
outage producing a burst. Pop is a rapid-fire drain after the backend recovers,
throttled by the caller. The two phases rarely overlap significantly.

During a pure enqueue burst there is no dead data (nothing has been popped), so
compaction has nothing to reclaim and should be a no-op. During a drain burst,
pops create dead data but no new enqueues arrive.

**Pops do not trigger compaction.** `needsCompaction()` is only checked in
`commitEnqueue`. The drain phase never stalls for compaction, which is correct for
latency. Dead data from a completed drain sits on flash until the next enqueue
triggers pressure or `compactIdle` runs explicitly.

**The clean-storage invariant.** If the queue is fully compacted before an
enqueue burst, the burst writes only new live data, stays within maxSegments,
and triggers no compaction. The write-path stall is a symptom of carrying dead
data into the burst from the previous drain. Fix the drain phase; the enqueue
phase fixes itself.

**Background compaction fits poorly here.** LittleFS is not concurrent: a
background compaction task still serialises against foreground I/O, adding
context-switch overhead without real parallelism. During an active burst or
drain there is no gain. The productive window is idle time between phases, which
the application already knows about. `compactIdle` called at that boundary is
the right mechanism.

## Future work

**Configurable kManifestMaxRanges.** The current manifest format (30B fixed +
4x8B = 62B) fits within the LittleFS 64-byte inline threshold. For devices with
larger flash queueing megabytes of data, compaction latency scales with queue
size and can become a data-loss mechanism: if the store cannot compact fast
enough, QueueFull drops records. More ranges (e.g. 8 ranges = 94B) enable more
subrange splits and tighter per-step latency bounds at scale. Deferred: touches
the manifest binary format, requires a version bump, and expands the test matrix
significantly.

**Cost-aware compaction strategy.** Score ranges by
`bytes_reclaimed / estimated_compaction_ms`, where estimated cost is derived
from the latency model. Naturally avoids large nearly-live ranges when stall
budget is tight. Lower priority while HighestDeadRatio performs well in
practice.

**Pop-triggered compaction.** Pops currently never trigger compaction; dead data
accumulates until the next enqueue or explicit `compactIdle`. An idle-hint
callback -- fired after a drain empties the queue -- would let the application
know compaction is productive without coupling the timing to the write path.

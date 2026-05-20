# pqueue

A durable, power-safe FIFO queue for LittleFS on ESP32. Designed for the
burst-enqueue / batch-drain / idle-compact pattern common in IoT devices that
buffer telemetry or commands when a backend is unreachable.

---

## What it is

pqueue stores records durably on flash. Each enqueue, pop, and compaction step
is crash-safe: a power loss at any point leaves the queue in a consistent state
that remounts cleanly.

It is designed around one specific workload: a device that enqueues records
during a failure window, then drains them in a batch when the backend recovers.
Compaction -- the process of reclaiming flash space from popped records -- is
designed to run during idle time between phases, not interleaved with enqueue or
drain.

**Two backends:**

pqueue has two store layouts selected via `cfg.storeLayout`:

- `AppendLog` (recommended): append-only segments with idle compaction. Low
  per-enqueue cost (~14ms), compaction runs during idle time. This README
  covers the AppendLog backend.
- `FixedSlot`: fixed-size ring buffer. Simpler, no compaction needed, but
  ~400ms per operation due to atomic fixed-size file writes. Use only if your
  workload needs constant-time guarantees and can tolerate the latency.

**What it is not:**

- Not a general-purpose embedded queue. The AppendLog compaction model assumes
  bursty write / batch-drain cycles. Continuous random enqueue/pop will work
  but is not the tuned path.
- Not concurrent. All operations serialize. A background compaction task would
  still block foreground I/O -- LittleFS has no parallelism. Drive compaction
  from your main loop instead.


---

## Operating contract

LittleFS has high per-operation latency (~35ms per file open on ESP32S3) and
significant write amplification under garbage collection. The AppendLog store is
designed so you pay those costs at chosen moments -- not as surprise stalls
during enqueue.

There are three distinct phases. Each has a cost profile:

### Enqueue

`enqueue()` appends to the active segment. Cost: one `writeAt` (~14ms on
device). No reads. Normally no compaction on a clean store.

If the store approaches its segment or byte limits and the previous idle
compaction was skipped, `enqueue` may trigger a compaction step before
returning. This is the stall you want to avoid. The way to avoid it is to
ensure the store is fully compacted before the burst arrives (see Idle
compaction below).

### Drain

`pop()` appends a tombstone event to the active tail segment (one `writeAt`).
It does not rewrite old segment files or reclaim space immediately. Dead records
accumulate in the existing segments until compaction runs.

The drain phase intentionally does not compact. Compaction during drain would
add latency to every pop, which is usually network-bound and already slow
enough. Dead data from a completed drain sits on flash until idle compaction
runs.

### Idle compaction

`compactIdle(n)` performs up to `n` compaction steps. Each step reads a range
of segments, collects live records, and rewrites them into fresh output segments.
Dead records are dropped. The step is bounded by `maxOutputSegments` (default 8)
so each call has a bounded amount of queue work per step.

Call `compactIdle` when the system has budget: after a drain completes, during
WiFi reconnect delay, or in a low-priority task. Run it in a loop until it
returns zero compactions:

```cpp
while (queue.compactIdle(1).compactions > 0) {
    // yield / feed watchdog if needed
}
```

**If you never call `compactIdle`**, the queue is still durable and correct.
But dead data from completed drains will accumulate, and the next enqueue burst
will eventually trigger compaction on the write path -- paying the cleanup cost
at the worst possible moment.


---

## Recommended main loop pattern

```cpp
// When events arrive (e.g. sensor reading, failed HTTP post):
queue.enqueue(payload);

// When backend/network is available -- drain as fast as the backend allows:
std::string rec;
while (queue.peek(rec).ok()) {
    if (backend.send(rec)) {
        auto st = queue.pop();
        if (!st.ok()) { /* handle storage error */ break; }
    } else {
        break;
    }
}

// When the system has idle budget (e.g. after drain, during reconnect delay):
pqueue::CompactIdleResult cr;
do {
    cr = queue.compactIdle(1);
    feedWatchdog();
} while (cr.compactions > 0);
```

The clean-storage invariant: if `compactIdle` runs to completion between
cycles, the next enqueue burst writes only fresh live data and does not trigger
compaction-induced stalls -- assuming the burst and remaining live data fit the
configured capacity.


---

## Quick start

### 1. Configure

```cpp
#include "pqueue/queue.h"

pqueue::Config cfg;
cfg.storeLayout     = pqueue::StoreLayout::AppendLog;
cfg.storageBackend  = pqueue::StorageBackend::LittleFS;
cfg.basePath        = "/pqueue";      // directory on LittleFS
cfg.reservedBytes   = 65536;         // total flash budget
cfg.maxSegmentBytes = 4096;          // bytes per segment file
cfg.maxSegments     = 16;            // max segment files on disk
cfg.minFreeBytes    = 4096;          // headroom to leave for LittleFS metadata

pqueue::Queue queue(cfg);
// No explicit mount() needed -- the store mounts lazily on first use.
```

### 2. Enqueue

```cpp
auto st = queue.enqueue(payload);
if (!st.ok()) { /* handle QueueFull or I/O error */ }
```

### 3. Drain

```cpp
std::string rec;
while (queue.peek(rec).ok()) {
    if (backend.send(rec)) {
        auto st = queue.pop();
        if (!st.ok()) { /* handle storage error */ break; }
    } else {
        break;
    }
}
```

### 4. Compact during idle

```cpp
pqueue::CompactIdleResult cr;
do { cr = queue.compactIdle(1); } while (cr.compactions > 0);
```


---

## Configuration reference

These fields on `pqueue::Config` control the AppendLog backend
(`cfg.storeLayout = StoreLayout::AppendLog`):

| Field | Default | Description |
|---|---|---|
| `maxSegmentBytes` | 4096 | Max bytes per segment file. Larger segments mean fewer files and less directory overhead, but coarser compaction granularity. |
| `reservedBytes` | 131072 | Total flash budget for the queue (`maxTotalBytes` in the underlying store). Set this to the flash you can afford, leaving headroom for LittleFS metadata. |
| `maxSegments` | 16 | Max number of segment files on disk. Each segment is one LittleFS file. Keep within LittleFS directory entry limits for your block device. |
| `minFreeBytes` | 32768 | Reserve this many bytes on the filesystem before writing. Prevents the queue from consuming all available flash. |

`maxOutputSegments` (max output segments per compaction step, default 8) is only
configurable at the `AppendLogStore` level. Through `Queue` it is fixed at 8.
To change it, construct an `AppendLogStore` directly instead of using `Queue`.

### Sizing guidance

A rough capacity estimate:

```
usable_records ~ reservedBytes / (payloadBytes + ~8)
```

For a 64KB budget with 492-byte payloads: ~64000 / 500 = ~128 records.

Set `maxSegmentBytes` so each segment holds 8-16 records. Too small and you
accumulate many files; too large and each compaction step processes more data
than needed.

With the default `maxOutputSegments=8` and `maxSegmentBytes=4096`, a heavy
compaction step reads and writes up to ~32KB, predicting roughly 5s on device
(ESP32S3 with QSPI flash). See the simulator section to predict your specific
config.


---

## Predicting latency with the simulator

The profiling tool runs the real store implementation against an in-memory
filesystem with simulated LittleFS latency. No device needed. Completes in
under a second.

Build and run:

```bash
make -j12 profiling
./build/pqueue-profiling idle-sim <burst> <payloadBytes> <cycles> [flags]
```

Pass your config values as flags so the simulation matches your deployment:

```bash
./build/pqueue-profiling idle-sim 500 492 3 \
    --max-segment-bytes 4096 \
    --max-total-bytes 65536 \
    --max-output-segments 8 \
    --pop 90
```

`Queue` users: leave `--max-output-segments` at 8 (the fixed default). Only
change it if you construct `AppendLogStore` directly.

### Reading the output

```text
idle_compaction  burst=500 payload=492B cycles=3 pop=90% maxOutSegs=8
  idleSteps=10   idleNoOps=3  hotCompactions=0
  maxStepLatency=48.5ms  totalIdleLatency=154.4ms
  (multiply ms by 100 for predicted on-device ms at calibrated flash speed)
  deadlocks=0    capExhausted=0
```

**The primary metric is `hotCompactions`:**

| Value | Meaning |
|---|---|
| `hotCompactions = 0` | Good. Idle cleanup is keeping enqueue clean. The clean-storage invariant holds. |
| `hotCompactions > 0` | Bad. Enqueue is paying cleanup cost. Compaction is not running often enough between cycles, or the burst outpaces idle budget. |

**Secondary: latency**

`maxStepLatency` is the worst single `compactIdle(1)` call, in simulated time.
To convert to predicted device milliseconds:

```
predicted_device_ms = sim_ms * 100 * multiplier
```

At the default `--multiplier 1.0` (calibrated for ESP32S3 with QSPI flash),
multiply sim_ms by 100. A faster device uses a smaller multiplier; a slower
device uses a larger one. The predicted max step must fit your watchdog timeout
or UI loop budget.

`totalIdleLatency` is the total cleanup cost across the run. Divide by the
number of cycles to estimate your idle-time budget requirement per cycle.

**Other fields:**

| Field | Meaning |
|---|---|
| `idleSteps` | Total compaction steps across all cycles. More steps = more fragmented data = more `compactIdle` calls needed per cycle. |
| `capExhausted` | Enqueue failed because live data filled the queue. No compaction strategy can help -- increase `reservedBytes` or drain more aggressively. |
| `deadlocks` | Enqueue failed even though dead data existed. Indicates a compaction strategy failure. Should always be 0. |

### Named examples

**Light** -- infrequent small bursts, nearly full drain:

```bash
./build/pqueue-profiling idle-sim 100 150 5 --pop 90
```

```text
idleSteps=18  maxStepLatency=10.0ms  totalIdle=65.6ms  hotCompactions=0
```

Predicted max step on device: ~1000ms. Predicted total idle for run: ~6560ms
(~1310ms average per cycle across 5 cycles).

**Realistic** -- moderate burst, large payload, high drain:

```bash
./build/pqueue-profiling idle-sim 500 492 3 --pop 90
```

```text
idleSteps=10  maxStepLatency=48.5ms  totalIdle=154.4ms  hotCompactions=0
```

Predicted max step on device: ~4850ms. Predicted total idle for run: ~15400ms
(~5130ms average per cycle across 3 cycles).

**Brutal** -- large burst, low drain (50%):

```bash
./build/pqueue-profiling idle-sim 500 492 3 --pop 50
```

```text
idleSteps=60  idleNoOps=3  hotCompactions=0  capExhausted=486
```

`capExhausted` dominates: 50% drain leaves too much live data across 3 cycles.
Solution: increase `reservedBytes`, reduce burst size, or drain more
aggressively.

### Adjusting for your flash speed

The default latency constants are calibrated for ESP32S3 with QSPI flash
(~35ms per file open). If your device is faster or slower, scale with
`--multiplier`:

```bash
# Device is 2x faster than the calibrated reference:
./build/pqueue-profiling idle-sim 500 492 3 --multiplier 0.5

# Device is 2x slower:
./build/pqueue-profiling idle-sim 500 492 3 --multiplier 2.0
```

To recalibrate from scratch: run an on-device test that exercises each op
type in isolation (readFile, writeFile, removeFile), measure actual ms, then
compare to the sim output at `--multiplier 1.0` and scale accordingly.


---

## Durability and crash safety

The AppendLog store is built on two durability primitives:

**Append-only segments.** Enqueue events and pop tombstones are appended to
the active tail segment. Compaction writes surviving records into new segment
files, then publishes a new manifest. Existing segment content is never
overwritten. A torn write at the tail (crash mid-append) is detected on remount
by checksum and the partial event is truncated. All events before the torn one
are intact.

**Atomic manifest publish.** The manifest (the index of which segment
generations are live) is written to an inactive slot before the active pointer
advances. If power fails between the slot write and the pointer update, remount
picks up the previous valid manifest. Compaction output is only visible after
its manifest is committed; partial output segments from an interrupted
compaction are cleaned up on remount.

**What survives a crash:**

- All records that received a successful `enqueue` return.
- All pops that received a successful `pop` return.
- Compaction steps where the manifest commit completed before the crash.

**What does not survive:**

- An `enqueue` or `pop` that was in flight at crash time.
- A compaction step whose manifest was not yet committed.


---

## Compaction internals (advanced)

The compaction strategy is **HighestDeadRatio**: on each step, pick the range
with the highest fraction of dead bytes. Ranges with no dead bytes are skipped.
This prevents compacting fully-live ranges, which would consume a step and a
range slot without reclaiming anything.

Each compaction step operates on a subrange of a manifest range, bounded by
`maxOutputSegments`. This limits the amount of work per step while still making
progress on large ranges.

The manifest tracks up to 4 contiguous ranges. Range fragmentation (more than
one range) arises from compaction producing output that does not align with the
active write tail. The store merges contiguous ranges automatically after each
compaction step.

For the full strategy rationale, deadlock analysis, and on-device validation
results, see `docs/pqueue-compaction-strategy.md`.


---

## Further reading

- `docs/pqueue-append-log-impl.md` -- implementation internals: manifest format,
  segment layout, record format, compaction mechanics
- `docs/pqueue-compaction-strategy.md` -- compaction strategy selection,
  simulator design, on-device validation runs
- `tools/pqueue_profiling.cpp` -- profiling and simulation tool source
- `tests/posix/pqueue_append_log_compaction.cpp` -- compaction correctness tests

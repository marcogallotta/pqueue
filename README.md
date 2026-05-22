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

`pop()` appends a tombstone to the active tail segment. The common case is one
`writeAt`. If the active tail is full, the pop rotates the tail first (writes a
new segment header and publishes a manifest) before appending the tombstone.
Pops never trigger compaction, but they are not always a single write.

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

`CompactIdleResult` fields:

| Field | Meaning |
|---|---|
| `status` | `ok()` on success, error otherwise. `isNoOp()` means no compaction candidates were found. |
| `stepsRun` | Steps attempted (including noOps). |
| `compactions` | Steps that did real work. |
| `noOps` | Steps that found no compaction candidates. |
| `moreWorkLikely` | True when the loop exhausted `maxSteps` after at least one successful compaction. Use as a scheduling hint to re-run next cycle. |
| `bytesReclaimed` | `totalOnDiskBytes` before minus after. May be 0 after a live rewrite that reduces dead bytes but has not yet freed files. |
| `deadBytesBefore` | Dead bytes across referenced sealed segments before the call. |
| `remainingDeadBytes` | Dead bytes across referenced sealed segments after the call. More reliable than `bytesReclaimed` as a measure of remaining cleanup work. |
| `inputSegments` | Total sealed segments consumed across all steps in this call. |
| `outputSegments` | Total segments written across all steps in this call. |


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

## Store-and-forward layers

For most IoT use cases you do not need to drive `Queue` directly. Two
higher-level wrappers handle the submit/drain lifecycle on top of the queue.

### pqueue::Outbox

`pqueue::Outbox` is a transport-agnostic store-and-forward layer. You provide
a `SendCallback` that attempts delivery; the outbox handles queueing, retry
backoff, and rate limiting.

```cpp
#include "pqueue/outbox.h"

pqueue::SendResult mySend(void* ctx, const std::string& payload, const pqueue::RetryState&) {
    if (myTransport.send(payload)) return {pqueue::SendDecision::Sent};
    return {pqueue::SendDecision::RetryLater};
}

pqueue::Config qcfg;
qcfg.basePath       = "/outbox";
qcfg.reservedBytes  = 65536;

pqueue::OutboxConfig ocfg;
ocfg.retryDelayMs   = 10000;

pqueue::Outbox outbox(qcfg, ocfg, mySend, nullptr, myClock, nullptr);

outbox.submit(payload);          // queue or send immediately if online
outbox.drainUpTo(50);            // attempt up to 50 sends
while (outbox.compactIdle(1).compactions > 0) {}  // reclaim dead space
```

### pqueue::http::Outbox

`pqueue::http::Outbox` wraps `pqueue::Outbox` for HTTP POST use cases. You
provide a `Transport` implementation (or use `CallbackTransport`); the outbox
serialises requests into the queue and replays them when the network is
available.

```cpp
#include "pqueue/http/outbox.h"

pqueue::http::Config cfg;
cfg.queue.basePath     = "/outbox";
cfg.queue.reservedBytes = 65536;
cfg.baseUrl            = "https://api.example.com";

pqueue::http::Outbox outbox(cfg, transport, myClock, nullptr);

outbox.submitPost("/readings", body);   // queue or send immediately
outbox.drainUpTo(50);                   // replay queued requests
while (outbox.compactIdle(1).compactions > 0) {}  // reclaim dead space
```

### Idle compaction with Outbox

Both `pqueue::Outbox` and `pqueue::http::Outbox` expose `compactIdle(maxSteps)`
which forwards to the underlying `Queue`. Drive it the same way as with `Queue`
directly -- after a drain completes, during a reconnect delay, or in a
low-priority task:

```cpp
pqueue::CompactIdleResult cr;
do {
    cr = outbox.compactIdle(1);
    feedWatchdog();
} while (cr.compactions > 0);
```

### Drain-informed compaction

`DrainResult` (returned by `drain()` and `drainUpTo()`) includes
`removedQueuedBytes`: the sum of raw pqueue record bytes for records
successfully removed from the queue (sent or dropped). Records dropped due to
an unreadable corrupt front contribute to the count but 0 bytes (the byte
count is unavailable).

This field is a useful scheduling signal for compaction: when the drain
removes records, it creates dead bytes in the sealed segments. The amount
removed is a rough proxy for how much compaction work has been created.

A budget-aware compaction pattern:

```cpp
const pqueue::DrainResult dr = outbox.drainUpTo(maxDrainAttempts);

// Size the compaction budget from how much the drain freed, capped at
// the configured max. If a previous run flagged more work, use full budget.
bool& moreWork = app.pqueueMoreCompactionLikely; // persistent across ticks
if (dr.removedQueuedBytes > 0 || moreWork) {
    const size_t maxSteps = kConfiguredMaxCompactSteps;
    size_t steps = maxSteps;
    if (!moreWork) {
        steps = std::max<size_t>(1, dr.removedQueuedBytes / kCompactBytesPerStep);
        steps = std::min(maxSteps, steps);
    }
    const auto cr = outbox.compactIdle(steps);
    moreWork = cr.status.ok() && cr.moreWorkLikely;
}
```

`removedQueuedBytes` does not include per-record append-log overhead (~24
bytes per record). It is a proportional guide, not an exact dead-byte count.
Use `CompactIdleResult::remainingDeadBytes` after compaction to see how much
dead data is still on disk.


---

## Configuration reference

These fields on `pqueue::Config` control the AppendLog backend:

| Field | Default | Description |
|---|---|---|
| `maxSegmentBytes` | 4096 | Max bytes per segment file. Larger segments mean fewer files and less directory overhead, but coarser compaction granularity. |
| `reservedBytes` | 131072 | Logical footprint cap for queue segment files (`maxTotalBytes` in the underlying store). Does not reserve or preallocate flash. When the total size of all segment files approaches this limit, writes compact or fail rather than growing further. |
| `maxSegments` | 16 | Compaction pressure threshold. When the live segment count exceeds this value, `enqueue` attempts one compaction step before rotating. Not a hard file-count limit: if compaction is a no-op (all data is live), segment count can exceed this value. |
| `minFreeBytes` | 32768 | Real filesystem safety floor. Writes are rejected when LittleFS free space would drop below this value. This is the actual guard against consuming all available flash, independent of `reservedBytes`. |
| `recordSizeBytes` | 492 | Maximum record payload size. `enqueue` returns `RecordTooLarge` for records exceeding this limit. Also caps how large a single record can be relative to `maxSegmentBytes`. |

`maxOutputSegments` (max output segments per compaction step, default 8) is only
configurable at the `AppendLogStore` level. Through `Queue` it is fixed at 8.
To change it, construct an `AppendLogStore` directly instead of using `Queue`.

### Sizing guidance

A rough clean-burst capacity estimate:

```
records_per_burst ~ reservedBytes / (payloadBytes + 24)
```

The `24` is the per-record AppendLog overhead (16-byte enqueue header + 8-byte
trailer). This is an optimistic lower bound: it does not account for segment
headers (~20 bytes amortized per record at 8-16 records per segment), pop
tombstones, rewrite events, dead data from partial drain cycles, or dangling
files. For a 64KB budget with 492-byte payloads: ~64000 / 516 = ~124 records.

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

**Simulator limitation:** the profiler uses an in-memory filesystem with
unlimited free space and sets `minFreeBytes = 0`. It models compaction behavior
and footprint cap exhaustion (`maxTotalBytes`) but cannot simulate the
`minFreeBytes` filesystem safety floor. If your deployment relies on
`minFreeBytes` to prevent full-flash scenarios, validate that separately on
device.

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
overwritten. A torn write at the active tail (crash mid-append) is detected on
remount by checksum and the partial event is truncated; all events before it are
intact. Corruption in a sealed (non-active) segment is fatal: remount returns
`DataCorrupt`.

**Atomic manifest publish.** The manifest (the index of which segment
generations are live) is written to an inactive slot before the active pointer
advances. If power fails between the slot write and the pointer update, remount
picks up the previous valid manifest. Compaction output is only visible after
its manifest is committed; unpublished output segments are not referenced by the
manifest and are ignored on remount. Unpublished output segments are not referenced by the manifest and are ignored
on remount. Generic dangling segment files are cleaned lazily during mount
scanning. Successful compaction also removes the retired input segments it just
replaced.

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

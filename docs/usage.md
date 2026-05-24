# pqueue Usage Guide

---

## Operating contract

On ESP32-S3/LittleFS, pqueue operations are durable but not microsecond-fast.
Expect enqueue and pop in the tens of milliseconds, larger payloads to cost more,
and occasional tail spikes from segment rollover or flash-state outliers. Large
offline backlogs also increase boot replay time. See `docs/benchmark.md` for
exact numbers.

The design keeps the normal enqueue/pop path simple and append-only. To avoid
expensive cleanup on the enqueue path, run idle compaction after drains or during
reconnect windows.

There are three distinct phases. Each has a cost profile:

### Enqueue

`enqueue()` appends to the active segment. Normally no compaction on a clean store.

If the segment is full, enqueue seals it and opens a new one (segment rollover).
If the store approaches its segment or byte limits and idle compaction was
skipped, `enqueue` may trigger a compaction step before returning. The way to
avoid this is to keep the store compacted between bursts (see Idle compaction
below).

### Drain

`pop()` appends a small durable pop marker to the active tail segment. Pops
never trigger compaction, but if the tail is full, a segment rollover happens
before the marker is written.

Dead records accumulate in the existing segments until compaction runs.
Compaction during drain would add latency to every pop, so it does not happen
automatically on the drain path.

### Idle compaction

`compactIdle(n)` performs up to `n` compaction steps. Each step reads a range
of segments, collects live records, and rewrites them into fresh output segments.
Dead records are dropped. The step is bounded by `maxOutputSegments` (default 8).

One `compactIdle(1)` step can block for up to ~7 s under a heavy backlog (see
`docs/benchmark.md`). Call at most one step per normal idle window rather than
looping to completion on every tick:

```cpp
// Preferred: one step per idle window, re-schedule if more work likely.
auto cr = queue.compactIdle(1);
feedWatchdog();
// cr.moreWorkLikely: schedule another pass next idle window.
```

Loop to completion only in a maintenance window where multi-second stalls are
acceptable:

```cpp
pqueue::CompactIdleResult cr;
do {
    cr = queue.compactIdle(1);
    feedWatchdog();
} while (cr.compactions > 0);
```

**If you never call `compactIdle`**, the queue is still durable and correct.
But dead data from completed drains will accumulate, and the next enqueue burst
will eventually trigger compaction on the write path.

### Boot / mount cost

On mount, the queue rebuilds its in-RAM index by replaying active segment files.
Cost grows with backlog and segment history. A fully drained queue mounts in
milliseconds; a large offline backlog can take seconds. This is a one-time boot
cost, not a per-operation cost. Keep queues drained when possible, and size
`reservedBytes` for the largest offline window you are willing to replay after
a reboot.

`CompactIdleResult` fields:

| Field | Meaning |
|---|---|
| `status` | `ok()` on success, error otherwise. Always `ok()` when steps returned noOp -- check `noOps` or `compactions` for that. |
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
auto cr = queue.compactIdle(1);
feedWatchdog();
// cr.moreWorkLikely: schedule another pass next idle window.
```

The clean-storage invariant: if `compactIdle` runs to completion between
cycles, the next enqueue burst writes only fresh live data and does not trigger
compaction-induced stalls -- assuming the burst and remaining live data fit the
configured capacity.


---

## pqueue::Outbox

`Outbox` is a store-and-forward layer over `Queue`. On `submit`, if the queue
is empty it attempts a live send; if the send fails the record is durably
queued. If a backlog already exists the record is enqueued directly, preserving
FIFO order. `drain` / `drainUpTo` retry queued records according to the retry
policy. The queue, retry state, and attempt counters all survive a power cycle.

### Setup

```cpp
pqueue::Config qcfg;
qcfg.basePath    = "/pqueue";
qcfg.reservedBytes = 65536;

pqueue::OutboxConfig ocfg;
ocfg.initialRetryDelayMs      = 10000;
ocfg.maxRetryDelayMs          = 60000;
ocfg.maxDrainAttemptsPerSecond = 2;

pqueue::Outbox outbox(qcfg, ocfg, mySend, ctx, myClock, clockCtx);
// With jitter (optional): append fakeRandFixed, randCtx as the last two args.
```

### Callbacks

**`ClockCallback`** -- must return monotonic milliseconds. Never use wall/NTP
time; the outbox uses it only for cooldown comparisons, so clock adjustments
would cause spurious notDue results. On ESP32:
`[](void*){ return esp_timer_get_time() / 1000; }`.

**`SendCallback`** -- called synchronously during `submit` and `drain`. Return
one of:

| Decision | Meaning |
|---|---|
| `SendDecision::Sent` | Backend accepted the record; remove it from the queue. |
| `SendDecision::RetryLater` | Transient failure; re-queue and apply backoff. Optionally set `retryAfterMs` to a server-supplied hint (see Retry policy). |
| `SendDecision::Drop` | Permanent failure; remove the record without retrying. |

```cpp
pqueue::SendResult mySend(void* ctx, const std::string& payload, const pqueue::RetryState& retry) {
    if (backend.post(payload))   return {pqueue::SendDecision::Sent};
    if (retry.attempts > 50)     return {pqueue::SendDecision::Drop};
    return {pqueue::SendDecision::RetryLater};
}
```

`RetryState::attempts` is the number of prior failed attempts, persisted in the
envelope and available across reboots.

**`RandCallback`** (optional) -- supplies randomness for jitter. If `nullptr`,
jitter is disabled regardless of `jitterPct`. On ESP32:
```cpp
[](void*, uint32_t range) -> uint32_t { return esp_random() % range; }
```

### submit / drain lifecycle

`submit(payload)` attempts a live send when the queue is empty, or enqueues
directly when a backlog exists. A `RetryLater` from the live send enqueues the
record with `attempts = 1` and arms the first retry cooldown.

`drain()` / `drainUpTo(n)` process at most one (or `n`) records per call,
subject to the rate limit and retry cooldown. Drive them from a timer or
WiFi-event callback:

```cpp
// On every tick:
outbox.drain();          // one attempt per call, rate-limited
// or:
outbox.drainUpTo(5);    // up to 5 attempts if rate and cooldown allow
```

`DrainResult` fields:

| Field | Meaning |
|---|---|
| `attempts` | Send attempts made this call. |
| `sent` | Records successfully sent and removed. |
| `dropped` | Records removed by a `Drop` decision. |
| `corruptDropped` | Proven-corrupt front records discarded (see `maxCorruptDropsPerLifetime`). |
| `removedQueuedBytes` | Raw queue bytes freed (sent + dropped). Useful for sizing compaction budget. |
| `rateLimited` | True when the per-second drain cap prevented an attempt. |
| `notDue` | True when the front record is still in its retry cooldown window. |
| `queueError` / `sendError` | Storage or callback configuration failure. |

### Retry policy

On `RetryLater`, the outbox applies exponential backoff before the next drain
attempt:

```
delay = min(maxRetryDelayMs, initialRetryDelayMs * 2^attempts)
delay = delay +/- rand(0, delay * jitterPct / 100)   // only if RandCallback supplied
```

If the send callback sets `SendResult::retryAfterMs > 0`, that hint is used
instead of the computed backoff, capped at `maxRetryDelayMs`:

```cpp
// Use a server-supplied Retry-After header value (milliseconds):
return {pqueue::SendDecision::RetryLater, retryAfterMs};
```

**FIFO limitation:** a cooling front record blocks all records behind it.
`DrainResult::notDue` signals this state. Keep `maxRetryDelayMs` shorter than
your drain latency budget. Long server-specified hints are clamped for the same
reason.

### OutboxConfig reference

| Field | Default | Description |
|---|---|---|
| `initialRetryDelayMs` | 10000 | Base delay before the first retry (attempt 0 failed). Doubles each subsequent attempt up to `maxRetryDelayMs`. |
| `maxRetryDelayMs` | 60000 | Ceiling for the exponential backoff. Keep modest: a cooling front record blocks all records behind it. |
| `jitterPct` | 20 | +/-% applied to the computed delay after capping. No effect unless a `RandCallback` is supplied. |
| `maxDrainAttemptsPerSecond` | 5 | Rate cap for `drain` calls. 0 is treated as 1. Prevents tight retry loops from hammering the backend. |
| `maxCorruptDropsPerLifetime` | 3 | Corrupt front records the outbox will silently discard per process lifetime before halting. 0 disables automatic dropping. |

---

## pqueue::http::Outbox

`http::Outbox` wraps `pqueue::Outbox` with an HTTP POST transport. It handles
request encoding, response classification, and optional Retry-After compliance.
Use it when your backend speaks HTTP/HTTPS.

### Setup

```cpp
#include "pqueue/http/outbox.h"

pqueue::http::Header headers[] = {
    {"X-API-Key",     "secret"},
    {"Content-Type",  "application/json"},
};

pqueue::http::Config cfg;
cfg.queue.basePath                  = "/pqueue";
cfg.queue.reservedBytes             = 65536;
cfg.outbox.initialRetryDelayMs      = 15000;
cfg.outbox.maxRetryDelayMs          = 60000;
cfg.outbox.maxDrainAttemptsPerSecond = 2;
cfg.baseUrl                         = "https://api.example.com";
cfg.headers                         = headers;
cfg.headerCount                     = 2;

pqueue::http::Esp32ArduinoTransportConfig transportCfg;
transportCfg.caCertPath       = "/certs/server.pem";
transportCfg.caCertFileSystem = &LittleFS;

pqueue::http::Esp32ArduinoTransport transport(transportCfg);
pqueue::http::Outbox outbox(cfg, transport, myClock, clockCtx);
```

### submitPost / drain

```cpp
// Queue (or live-send) a POST to baseUrl + path:
outbox.submitPost("/readings", jsonBody);

// Drain from a timer or WiFi event:
outbox.drain();
```

### Response classification

`defaultClassifyResponse` maps HTTP status codes to send decisions:

| Status range | Decision |
|---|---|
| 2xx | `Sent` |
| 408, 429, 5xx | `RetryLater` (transient) |
| 501, 505, 508 | `Drop` (permanent server capability error) |
| All other 4xx, 3xx | `Drop` |
| Transport error (timeout, TLS, network) | `RetryLater` |

Supply `cfg.classify` to override the default for your endpoint:

```cpp
pqueue::SendDecision myClassify(void* ctx, const pqueue::http::Response& resp) {
    if (resp.statusCode == 409) return pqueue::SendDecision::RetryLater;
    return pqueue::http::defaultClassifyResponse(resp);
}
cfg.classify        = myClassify;
cfg.classifyContext = nullptr;
```

### Retry-After compliance

When a transport sets `Response::retryAfterMs`, the outbox honours it (capped
at `maxRetryDelayMs`). The built-in transports do not yet parse the
`Retry-After` header; populate `retryAfterMs` from your own transport or
classify callback if needed.

### Observability callbacks

```cpp
// Called for every response, success or failure:
cfg.onResponse = [](void*, const pqueue::http::RequestEnvelope& req,
                    const pqueue::http::Response& resp) {
    log("POST %s -> %d", req.path.c_str(), resp.statusCode);
};

// Called when a record is permanently dropped:
cfg.onDrop = [](void*, const pqueue::http::RequestEnvelope* req,
                pqueue::http::DropReason reason,
                const pqueue::http::Response* resp) {
    // reason: DecodeFailed (corrupt envelope) or ServerRejected (Drop decision)
    // req/resp may be nullptr if the envelope could not be decoded
};
```

### http::Config reference

| Field | Default | Description |
|---|---|---|
| `queue` | - | Forwarded to the underlying `Queue`. See Queue config table. |
| `outbox` | - | Forwarded to the inner `pqueue::Outbox`. See OutboxConfig table. |
| `fullQueuePolicy` | `DropOldest` | What to do when the queue is full on `submitPost`. `DropOldest` evicts the front record; `RejectNewest` returns `QueueFull`. |
| `baseUrl` | `""` | Prepended to every `submitPost` path. Leading/trailing slashes are normalised. |
| `headers` / `headerCount` | `nullptr` / `0` | Static headers sent with every request (e.g. auth tokens, content type). |
| `classify` / `classifyContext` | `nullptr` | Override the default response classifier. Return `Sent`, `RetryLater`, or `Drop`. |
| `onResponse` / `responseContext` | `nullptr` | Observer called for every HTTP response before the send decision is applied. |
| `onDrop` / `dropContext` | `nullptr` | Observer called when a record is permanently dropped. |

---

## Idle compaction with Outbox

Both `pqueue::Outbox` and `pqueue::http::Outbox` expose `compactIdle(maxSteps)`
which forwards to the underlying `Queue`. Drive it the same way as with `Queue`
directly -- after a drain completes, during a reconnect delay, or in a
low-priority task:

```cpp
auto cr = outbox.compactIdle(1);
feedWatchdog();
// cr.moreWorkLikely: schedule another pass next idle window.
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

Keep `maxSegmentBytes=4096` (the default) unless you have measured a reason to
change it. Larger segments reduce rollover frequency but increase flush cost and
make compaction steps heavier. For large payloads, accepting fewer records per
segment is usually better than jumping to 8 KB blindly.

With the default `maxOutputSegments=8` and `maxSegmentBytes=4096`, a heavy
compaction step reads and writes up to ~32KB. Actual LittleFS latency depends on
filesystem state and flash wear; see `docs/benchmark.md` for on-device numbers.


---

## Performance

For current ESP32-S3/LittleFS latency numbers and benchmark commands, see
`docs/benchmark.md`.


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
manifest and are ignored on remount. Generic dangling segment files are cleaned
lazily during mount scanning. Successful compaction also removes the retired
input segments it just replaced.

**What survives a crash:**

- All records that received a successful `enqueue` return.
- All pops that received a successful `pop` return.
- Compaction steps where the manifest commit completed before the crash.

**What does not survive:**

- An `enqueue` or `pop` that was in flight at crash time.
- A compaction step whose manifest was not yet committed.


---

## Compaction behaviour

The compaction strategy picks the range with the highest fraction of dead bytes.
Ranges with no dead bytes are skipped entirely -- compacting a fully-live range
would waste a step without reclaiming anything.

Each step is bounded by `maxOutputSegments` (default 8), but a single step can
still block for several seconds on slow flash or a large backlog. Run compaction
after drains or during reconnect windows, not on the enqueue path.

For the full strategy rationale, deadlock analysis, and on-device validation
results, see `docs/internals.md`.


---

## Raw-buffer API (no caller-side heap allocation)

`Queue` and `Outbox` accept and return records via caller-owned buffers using
`pqueue::Span` (read-only) and `pqueue::MutableSpan` (writable):

```cpp
struct Span        { const uint8_t* data; size_t len; };
struct MutableSpan { uint8_t*       data; size_t len; };
```

### `Queue` raw-buffer methods

```cpp
// Enqueue from a caller-owned buffer -- no std::string at the call site.
uint8_t payload[492];
size_t n = buildPayload(payload);
queue.enqueue(pqueue::Span(payload, n));

// Query size without reading the payload (no I/O; size is in RAM).
size_t sz = 0;
auto st = queue.peekSize(sz);  // QueueEmpty if no front record

// Peek into a caller-owned buffer.
uint8_t buf[492];
size_t written = 0;
auto st = queue.peek(pqueue::MutableSpan(buf, sizeof(buf)), written);
// st.code == RecordTooLarge if buf is too small; written is valid on ok().
if (st.ok()) {
    send(buf, written);
    queue.pop();
}
```

The `std::string` overloads remain first-class and are not deprecated.
Internal record storage stays as `std::string`; only caller-side allocations
are eliminated.

The raw-buffer API avoids caller-side allocation; benchmark results show it is
not a latency win over the `std::string` API because LittleFS I/O dominates.

**Error codes:**
- `InvalidArgument` -- `Span` or `MutableSpan` has `len > 0` and `data == nullptr`
- `RecordTooLarge` -- `peek(MutableSpan)` when stored record exceeds `out.len`
- `QueueEmpty` -- `peekSize` or `peek` on an empty queue

### `Outbox` raw callback

`Outbox` can be constructed with a `RawSendCallback` instead of `SendCallback`:

```cpp
using RawSendCallback = SendResult (*)(void* context, pqueue::Span payload, const RetryState& retry);

pqueue::Outbox outbox(queueConfig, outboxConfig, myRawSend, ctx, clock, clockCtx);
```

The callback receives a `Span` pointing into an internal buffer that is valid
only during the callback. Do not retain the pointer after it returns. `submit(Span)` is also
available alongside `submit(const std::string&)`.

Only one callback variant is configured per instance (`SendCallback` or
`RawSendCallback`). `http::Outbox` is unaffected and always uses the string
path internally.

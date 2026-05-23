# pqueue

A durable, power-safe FIFO queue for LittleFS on ESP32. Designed for the
burst-enqueue / batch-drain / idle-compact pattern common in IoT devices that
buffer telemetry or commands when a backend is unreachable.

pqueue stores records durably on flash. Each enqueue, pop, and compaction step
is crash-safe: a power loss at any point leaves the queue in a consistent state
that remounts cleanly.

**What it is not:**

- Not a general-purpose embedded queue. The AppendLog compaction model assumes
  bursty write / batch-drain cycles. Continuous random enqueue/pop will work
  but is not the tuned path.
- Not concurrent. All operations serialize. A background compaction task would
  still block foreground I/O -- LittleFS has no parallelism. Drive compaction
  from your main loop instead.


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


---

## Further reading

- `docs/usage.md` -- operating contract, configuration reference, simulator, crash safety
- `docs/pqueue-append-log-impl.md` -- implementation internals: manifest format, segment layout, compaction mechanics
- `docs/pqueue-compaction-strategy.md` -- compaction strategy selection, simulator design, on-device validation
- `examples/basic_queue.cpp` -- runnable POSIX example: enqueue / drain / compact lifecycle
- `examples/outbox.cpp` -- runnable POSIX example: store-and-forward with retry
- `examples/esp32_http_outbox/main.cpp` -- ESP32 firmware reference sketch
- `tools/pqueue_profiling.cpp` -- profiling and simulation tool source

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
- Not concurrent. All operations serialise. A background compaction task would
  still block foreground I/O -- LittleFS has no parallelism. Drive compaction
  from your main loop instead.


---

## Which API should I use?

- **`pqueue::http::Outbox`** -- HTTP POST delivery with retry and backoff. Use this for most firmware that POSTs sensor data or events to a backend.
- **`pqueue::Outbox`** -- store-and-forward with a custom send callback. Use when you control the transport (MQTT, CoAP, WebSocket) but want durable queueing and retry built in.
- **`pqueue::Queue`** -- raw durable FIFO. Use when you need full control over the read/send/pop loop, or when neither outbox fits your delivery model.

The sections below show each API. Start with `pqueue::http::Outbox` unless you have a specific reason to drop to a lower-level API.

---

## Queue quick start

> If you are using HTTP POST to a backend, skip to [Store-and-forward layers](#store-and-forward-layers) below.

### 1. Configure

```cpp
#include "pqueue/queue.h"

pqueue::Config cfg;
cfg.storageBackend  = pqueue::StorageBackend::LittleFS;
cfg.basePath        = "/pqueue";      // directory on LittleFS
cfg.reservedBytes   = 65536;         // total flash budget
cfg.maxSegmentBytes = 4096;          // bytes per segment file
cfg.maxSegments     = 16;            // compaction pressure threshold (not a hard file-count limit)
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
auto cr = queue.compactIdle(1);
// cr.moreWorkLikely: schedule another pass next idle window.
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
ocfg.initialRetryDelayMs = 10000;

pqueue::Outbox outbox(qcfg, ocfg, mySend, nullptr, myClock, nullptr);

outbox.submit(payload);          // send immediately if no backlog, otherwise queue
outbox.drainUpTo(50);            // attempt up to 50 sends
outbox.compactIdle(1);           // reclaim dead space during idle windows
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

// transport: an Esp32ArduinoTransport or CallbackTransport instance (see docs/usage.md)
pqueue::http::Outbox outbox(cfg, transport, myClock, nullptr);

outbox.submitPost("/readings", body);   // send immediately if no backlog, otherwise queue
outbox.drainUpTo(50);                   // replay queued requests
outbox.compactIdle(1);                  // reclaim dead space during idle windows
```


---

## Maintenance: pqueue_doctor

`src/pqueue/doctor/session.h` provides a serial command loop for inspecting,
validating, compacting, and dumping queue state. The simplest path is
`tools/esp32_pqueue_doctor/main.cpp`, a standalone maintenance image -- no
production firmware changes needed. Trigger mode wires `runSession()` into the
production firmware loop for use without reflashing.

`tools/pqueue_doctor.py` drives the session from the host:

```bash
python3 tools/pqueue_doctor.py \
    --port /dev/ttyACM0 \
    --target api_outbox:/pqueue_api_spool \
    --queue-config reservedBytes=262144 \
    --validate
```

See `docs/doctor.md` for connection modes, all commands, config overrides, and
common workflows.


---

## Further reading

- `docs/usage.md` -- operating contract, configuration reference, crash safety
- `docs/internals.md` -- implementation internals: manifest format, segment layout, compaction mechanics, simulator, on-device validation
- `docs/doctor.md` -- pqueue_doctor reference: commands, targets, config overrides, common workflows
- `examples/basic_queue.cpp` -- runnable POSIX example: enqueue / drain / compact lifecycle
- `examples/outbox.cpp` -- runnable POSIX example: store-and-forward with retry
- `examples/esp32_http_outbox/main.cpp` -- ESP32 firmware reference sketch
- `docs/benchmark.md` -- performance numbers and tuning guidance
- `tools/pqueue_benchmark.cpp` -- POSIX benchmark source
- `tools/pqueue_compaction_sim.cpp` -- compaction correctness sweep
- https://github.com/marcogallotta/esp32-home-display -- real-world integration: HTTP outbox for sensor data upload on ESP32-S3/LittleFS

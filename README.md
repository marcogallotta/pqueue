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

## Maintenance: pqueue_doctor

`tools/esp32_pqueue_doctor/main.cpp` is a maintenance firmware image. Flash it
once for an incident; it runs a serial command loop that lets you inspect,
validate, compact, and dump queue state without modifying the production
firmware.

### Adding the build env to your app

The doctor must be built with the same board, partition table, and filesystem
settings as your app. Add this env to your app's `platformio.ini`:

```ini
[env:pqueue-maintenance]
extends = env:<your-main-env>
build_flags =
    ${env:<your-main-env>.build_flags}
    -Ipath/to/pqueue/tools
build_src_filter =
    -<*>
    +<path/to/pqueue/tools/esp32_pqueue_doctor/main.cpp>
```

Flash with `pio run -e pqueue-maintenance --target upload`.

### Basic session

Connect over serial, then send commands. Queue paths are supplied at runtime:

```
PQUEUE_DOCTOR_START
READY
TARGET api_outbox /pqueue_api_spool
READY
VALIDATE
READY
DONE
```

`DONE` reboots back into the maintenance firmware (the app image is not
restored automatically -- reflash manually when finished).

### Host script

`tools/pqueue_doctor.py` drives the serial session. Commands run in a fixed
order: read-only first (info/list/diag/validate), then mutations, then dump.

```bash
# Validate and dump all files
python3 tools/pqueue_doctor.py \
    --port /dev/ttyACM0 \
    --target api_outbox:/pqueue_api_spool \
    --validate --dump-all --out-dir /tmp/pqdump

# Compact then dump
python3 tools/pqueue_doctor.py \
    --port /dev/ttyACM0 \
    --target api_outbox:/pqueue_api_spool \
    --compact-all 64 --dump-all --out-dir /tmp/pqdump

# Multiple targets (each dumped into out-dir/name/)
python3 tools/pqueue_doctor.py \
    --port /dev/ttyACM0 \
    --targets pqueue_doctor.targets \
    --validate --dump-all --out-dir /tmp/pqdump
```

Targets file format (`pqueue_doctor.targets`):

```
# name  path
api_outbox  /pqueue_api_spool
```

After dumping, run `pqueue_appendlog_diag` (build with `make appendlog-diag`)
on the dumped directory to inspect the manifest and segment layout offline.

Stdin mode (pipe from POSIX dump tool, no device needed):

```bash
./build/pqueue-doctor-dump --base-path /path/to/spool | \
    python3 tools/pqueue_doctor.py --out-dir /tmp/pqdump
```

### Command reference

```
TARGET <name> <path>      -- select queue (required before any other command)
INFO                      -- filesystem stats and file list
LIST                      -- segment files with sizes and status
VALIDATE                  -- run Queue::validate(), print issues and repair hints
DIAG                      -- manifest contents and segment summary
DUMP_FILE <name>          -- transfer one file off-device
DUMP_ALL                  -- transfer all manifest and segment files
COMPACT <steps>           -- run compactIdle(steps)
COMPACT_ALL <max_steps>   -- run compactIdle to completion, up to max_steps
DROP_FRONT_IF_CORRUPT     -- drop front record only if corruption is proven
RECOVER_STALE_LOCK        -- remove a stale lock left by a dead process
FORMAT CONFIRM            -- destructively reinitialize the queue
DONE                      -- exit (reboots into maintenance firmware)
```

`FORMAT` requires the literal argument `CONFIRM` to prevent accidental execution.


---

## Further reading

- `docs/usage.md` -- operating contract, configuration reference, simulator, crash safety
- `docs/internals.md` -- implementation internals: manifest format, segment layout, compaction mechanics, simulator, on-device validation
- `docs/doctor.md` -- pqueue_doctor reference: commands, targets, config overrides, common workflows
- `examples/basic_queue.cpp` -- runnable POSIX example: enqueue / drain / compact lifecycle
- `examples/outbox.cpp` -- runnable POSIX example: store-and-forward with retry
- `examples/esp32_http_outbox/main.cpp` -- ESP32 firmware reference sketch
- `tools/pqueue_profiling.cpp` -- profiling and simulation tool source

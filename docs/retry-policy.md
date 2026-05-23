# Retry Policy Design

Covers `pqueue::Outbox` and `pqueue::http::Outbox` retry behaviour for 1.0,
and deferred work for post-1.0.

---

## Config shape

```cpp
struct OutboxConfig {
    std::uint32_t initialRetryDelayMs = 10000;
    std::uint32_t maxRetryDelayMs     = 60000;
    std::uint8_t  jitterPct           = 20;    // ±% applied after capping
    // ... rest unchanged
};
```

`retryDelayMs` is removed. Defaults keep behaviour close to the previous
single-delay config — existing users get a recompile error that is easy to fix,
not a silent behaviour change.

---

## Jitter random source

Jitter is supplied via a caller-provided callback, injected alongside the clock:

```cpp
// Returns a value in [0, range). Must be thread-safe if drain() is called
// from multiple contexts. On ESP32 use esp_random(); on POSIX use std::rand().
using RandCallback = std::uint32_t (*)(void* context, std::uint32_t range);
```

`Outbox` stores `rand_` / `randContext_` and calls it once per `RetryLater`
to sample the jitter offset. Callers that pass `nullptr` get no jitter
(delay = capped base delay).

---

## Backoff formula

```
base  = min(maxRetryDelayMs, initialRetryDelayMs * 2^attempts)
jitter = rand(0, base * jitterPct / 100)   // symmetric: subtract or add
delay = base ± jitter                       // sign chosen by low bit of rand sample
```

`attempts` is already stored per-envelope in the queue. The clock source
(`clock_` / `clockContext_`) already exists. No new mechanism is needed — this
is the existing `setFrontCooldown` path using what is already there.

Keep `maxRetryDelayMs` modest in production. A record cooling at the front
blocks all records behind it (see FIFO limitation below). The default of 60s is
configurable; tighten it if your workload is latency-sensitive.

---

## SendResult shape

```cpp
struct SendResult {
    SendDecision decision;
    std::uint32_t retryAfterMs = 0; // 0 = no server hint
};
```

`Outbox` interprets `retryAfterMs` only on `RetryLater`:

| `retryAfterMs` | effective delay |
|---|---|
| `> 0` | `min(retryAfterMs, maxRetryDelayMs)` — server hint, hard-capped |
| `0` | computed backoff (see formula above) |

The field gives HTTP callers a place to surface `Retry-After` headers now,
before drain-around-front exists. Long server-specified windows are silently
clamped to `maxRetryDelayMs` so a single cooling record cannot freeze the
outbox indefinitely.

---

## HTTP status code classification

### Permanent errors (move to `ServerRejected`, do not retry)

| Code | Reason |
|---|---|
| `501 Not Implemented` | Server capability gap, not transient |
| `505 HTTP Version Not Supported` | Protocol mismatch, not transient |
| `508 Loop Detected` | Server configuration error, not transient |

### Retryable (unchanged)

`429`, `503`, other `5xx`, transport errors.

---

## FIFO limitation

A cooling front record blocks all records behind it. Consequences:

- Keep `maxRetryDelayMs` short relative to your drain latency budget.
- Long `Retry-After` values from the server are clamped to `maxRetryDelayMs`
  for this reason.
- Callers can observe front-blocking via `DrainResult.notDue`.

This constraint exists because the queue is strictly FIFO. Drain-around-front
requires a semantic change to the Outbox and is deferred (see below).

---

## Deferred: drain-around-front

Allowing a cooling record to be skipped so later records can drain changes
Outbox semantics. It requires an explicit opt-in:

```cpp
enum class OutboxOrdering {
    StrictFifo,         // default; cooling front blocks drain
    AllowRetryReorder,  // cooling records cycle to tail
};
```

`AllowRetryReorder` is appropriate for independent telemetry payloads where
ordering is not required. It can break ordering-sensitive users. Deferred
until the config knob and its interaction with the envelope sequence counter
are fully designed.

Full `Retry-After` compliance (without capping) is also deferred until
drain-around-front exists.

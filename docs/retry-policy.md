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

## Backoff formula

```
delay = min(maxRetryDelayMs, initialRetryDelayMs * 2^attempts)
delay += uniform(-jitterPct%, +jitterPct%) * delay
```

`attempts` is already stored per-envelope in the queue. The clock source
(`clock_` / `clockContext_`) already exists. No new mechanism is needed — this
is the existing `setFrontCooldown` path using what is already there.

Keep `maxRetryDelayMs` modest in production. A record cooling at the front
blocks all records behind it (see FIFO limitation below). The default of 60s is
configurable; tighten it if your workload is latency-sensitive.

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

## Retry-After compliance (capped)

`429` and `503` responses commonly include a `Retry-After` header. Apply it,
but hard-cap at `maxRetryDelayMs`:

```
effectiveDelay = min(parsedRetryAfter, maxRetryDelayMs)
```

Without drain-around-front, a server-specified window longer than
`maxRetryDelayMs` would freeze the entire outbox for that duration. The cap
makes compliance safe while still honouring short server hints. Document that
long `Retry-After` values are silently clamped.

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

# Idle Compaction Integration

**Editing rules:** ASCII only -- no Unicode symbols. This file is a living reference; delete completed items, rewrite state sections, never mark things done inline.

This document covers exposing `compactIdle` through the pqueue outbox layers.
It is a prerequisite for the app migration (`docs/outbox-appendlog-migration.md`).

---

## Expose compactIdle through the outbox layers

Two thin wrappers, no redesign:

- `pqueue::Outbox::compactIdle(size_t maxSteps)` -- calls `queue_.compactIdle(maxSteps)`,
  returns `pqueue::CompactIdleResult`.
- `pqueue::http::Outbox::compactIdle(size_t maxSteps)` -- delegates to `pqueue::Outbox`,
  returns `pqueue::CompactIdleResult`.

The return type flows through unchanged. Callers decide what to log.

---

## Tests

Add a POSIX test (in `pqueue_outbox.cpp` or a new `pqueue_http_outbox.cpp` file) that:

- Constructs a `pqueue::http::Outbox` (or `pqueue::Outbox`) over an AppendLog config with
  enough churn to leave compactable dead ranges.
- Calls `compactIdle(N)` through the outbox layer.
- Asserts the returned `CompactIdleResult` reflects work done (e.g. `moreWorkLikely` or
  a step count), and that the queue drains correctly afterwards.

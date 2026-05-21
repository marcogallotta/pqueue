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

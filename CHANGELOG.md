# Changelog

## [1.0.0] - 2026-05-24

Initial stable release of `pqueue`.

### Added

- Persistent FIFO queue with append-log storage.
- Crash-tolerant segment/manifest design for embedded filesystems without relying on `fsync`.
- POSIX and Arduino LittleFS storage backends.
- `Queue` API for enqueue, peek, pop, stats, validation, and idle compaction.
- Raw-buffer queue API using `Span` / `MutableSpan` to avoid caller-side heap allocation on hot paths.
- Store-and-forward `Outbox` with retry metadata, payload validation, and HTTP transport support.
- Configurable full-queue behaviour, including drop-oldest mode for telemetry/outbox workloads.
- Idle and full compaction support for reclaiming dead log data.
- Validation and repair/doctor tooling for append-log stores.
- Examples for basic queue usage, outbox usage, and ESP32/LittleFS integration.
- POSIX benchmark and regression tooling for deterministic I/O-count and write-amplification tracking.
- Binary format v1 for segment records and manifests.

### Notes

- This release establishes the v1 on-disk format.
- POSIX benchmark latency numbers are host-local only; device runtime numbers should be measured on target hardware.

# AppendLog Validation and Repair

**Editing rules:** ASCII only -- no Unicode symbols. This file is a living reference; delete completed items, rewrite state sections, never mark things done inline.

This is a prerequisite for the fixed-slot elimination. The goal is to bring
AppendLog's validation and repair surface to parity with what the fixed-slot
backend provides today, so nothing regresses when fixed-slot is removed.

---

## AppendLogStore::validateUnlocked

Audit and complete the existing implementation. It must cover:

- Manifest slot election and CRC validation.
- Segment file presence and header validation for all generations in the manifest.
- Sequence continuity across segments.

## AppendLogStore::rebuildMetadata

Currently unimplemented or a stub. For AppendLog, rebuild means: scan all
`seg-*.bin` files on disk, reconstruct the live record set from segment headers
and event replay, and write a fresh manifest. Define the exact recovery
semantics and implement them.

## Diagnostic tool

`src/pqueue_api_outbox_diagnostic_main.cpp` is entirely fixed-slot-specific.
It uses `pqueue::diagnoseFileStore()`, inspects checkpoint slots, journal entries,
and `pqueue.spool` -- all from `diagnostics.h/.cpp` which is deleted with the
fixed-slot backend.

Rewrite it for AppendLog. The AppendLog equivalent should:

- List manifest files and segment files present on LittleFS.
- Call `AppendLogStore::validateUnlocked` and print the result.
- Report manifest state: epoch, ranges, tail generation, segment count.
- Support a repair mode that calls `rebuildMetadata` or `format`.

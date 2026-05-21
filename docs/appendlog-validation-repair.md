# AppendLog Validation and Repair

**Editing rules:** ASCII only -- no Unicode symbols. This file is a living reference; delete completed items, rewrite state sections, never mark things done inline.

This is prerequisite work before the fixed-slot backend can be dropped. Goal: bring
AppendLog's validation, repair hints, and diagnostic surface to the level the
fixed-slot backend provides today.

**Scope constraint.** `validate()` is read-only diagnosis only. No manifest
reconstruction, no guessing from dangling files, no deletions during validation,
no changing mount behavior. The split is:

- `validate()` = read-only structural check
- `rebuildMetadata()` / `format()` = explicit safe mutations, called by the caller
  after inspecting the validation result

---

## Patch 1: manifest-aware validateUnlocked and tail-aware segment scan

This is the core missing piece. The current implementation scans all `seg-*.bin`
files in numeric generation order and never reads the manifest. Replace it with
a manifest-first model matching how mount works.

### Manifest pass

- Attempt to read both manifest slots. One absent slot is not an error (may not
  exist yet after first publish). Both slots present but neither parsing is
  `MetadataCorrupt`.
- Elect winning slot using the same `chooseWinningSlot` logic as `readManifest`.
- No valid manifest + no segment files = fresh/empty store, return ok.
- No valid manifest + segment files present = `MetadataCorrupt` (segments cannot
  be attributed to any manifest; repair = `Format` -- see Patch 2 for why
  `RebuildMetadata` is not used here).
- Validate the winning manifest structure:
  - ranges, if present, are valid and ordered oldest to newest (a store with
    only a tail and no sealed ranges is valid)
  - no duplicate generations across ranges
  - ranges do not overlap
  - `nextGeneration` > max referenced generation
  - `tailGeneration` is 0 (no tail) or > max sealed generation
- For each generation referenced by the manifest: verify the segment file exists
  and has at least `kSegmentHeaderBytes`. Missing = `MetadataCorrupt`.

### Segment scan pass

Only scan manifest-referenced segments (sealed ranges + tail). Dangling files are
not corruption -- `cleanupOneDanglingSegment` handles them on the next mount.
`ValidationResult` has no info/warning field, so dangling file reporting is
diagnostic-only for now: count them and surface the count in the diagnostic tool
(Patch 3), not in `ValidationResult`.

Apply tail-awareness to the scan:

- Sealed segments (not the tail) must not have a torn tail. A record that fails
  CRC or footer in a sealed segment is `JournalCorrupt`, not a torn-tail skip.
- The tail segment (manifest `tailGeneration`) may have a torn tail. Treat it the
  same way `scanSegments` does: stop at the torn boundary, do not report as error.
- Segment header generation must match the filename/manifest generation.
  Mismatch = `MetadataCorrupt`.

### Index consistency check

After manifest and segment passes succeed, call `readIndexFromDisk()` and verify
it returns ok and the resulting `FileStoreIndex` is self-consistent
(`count == tail - head`). This exercises the real mount path without duplicating
its logic in a separate replay model.

### Tests (POSIX, added in this patch)

- Valid store validates cleanly.
- Fresh empty store (no manifest, no segments) validates cleanly.
- Missing referenced segment = `MetadataCorrupt`.
- Both manifest slots corrupt = `MetadataCorrupt`.
- Overlapping manifest ranges = `MetadataCorrupt`.
- `nextGeneration` below max referenced generation = `MetadataCorrupt`.
- Wrong segment header generation = `MetadataCorrupt`.
- Corrupt CRC in referenced sealed segment = `JournalCorrupt`.
- Torn tail in tail segment = ok (not reported as error).
- Torn tail in sealed segment = `JournalCorrupt`.
- Dangling unreferenced segment = ok (not reported as error, reported as info count).

---

## Patch 2: repair hint routing and dead code cleanup

### Repair hint routing

`queue.cpp::addRepairHints` maps codes to `ValidationRepairAction`. Update for
AppendLog semantics:

| Code | Action | Reason |
|---|---|---|
| `MetadataCorrupt` | `RebuildMetadata` | manifest bad but segments may be intact; rescan recovers |
| `JournalCorrupt` | `Format` | segment event data corrupt; rescan will not recover |
| `SlotCrcMismatch` | `DropFrontIfCorrupt` at head / `Format` elsewhere | unchanged, already correct |
| `InvalidConfig` | none | config error, not storage |

### Remove FixedSlot-only ValidationIssueCode values

Remove from the enum: `SpoolMissing`, `SpoolSizeMismatch`, `InvalidRingState`,
`SlotHeaderInvalid`, `SlotReadFailed`, `MetadataMissing`.

Remove the dead cases from `isRebuildMetadataIssue` and `isFormatRepairIssue`
in `queue.cpp`.

Blocked on FixedSlot tests being migrated away from these codes first.

### rebuildMetadata semantics

`rebuildMetadata()` resets RAM state and calls `mount()`. For AppendLog, `mount()`
returns `DataCorrupt` when segment files exist without a valid manifest -- it does
not reconstruct the manifest from segments. So `rebuildMetadata` cannot recover a
`MetadataCorrupt` store; the only safe repair is `format()`. Document in code: do
not call `rebuildMetadata` after `MetadataCorrupt`; it will fail for the same
reason validate reported corruption. Use `format()` instead.

---

## Patch 3: rewrite the diagnostic tool

`src/pqueue_api_outbox_diagnostic_main.cpp` is entirely FixedSlot-specific
(`diagnoseFileStore()`, checkpoint slot dumps, journal state, `pqueue.spool`).
All of this is deleted with the fixed-slot backend.

Rewrite for AppendLog:

- Mount LittleFS, list files in the spool directory.
- Read both manifest slots; show which are present, which wins, winning epoch.
- Show manifest ranges, tail generation, segment count.
- For each segment file: generation, file size.
- Call `Queue::validate()` and print each issue with code and repair action.
- Show dangling segment count if non-zero.
- Support a format mode (compile-time flag, same pattern as existing tool).
- No `RebuildMetadata` mode: rebuild = remount, which happens automatically on
  next boot.

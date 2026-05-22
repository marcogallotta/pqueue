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

## Remaining work: dead code cleanup (blocked)

### Remove FixedSlot-only ValidationIssueCode values

Remove from the enum: `SpoolMissing`, `SpoolSizeMismatch`, `InvalidRingState`,
`SlotHeaderInvalid`, `SlotReadFailed`, `MetadataMissing`.

Remove the dead cases from `isRebuildMetadataIssue` and `isFormatRepairIssue`
in `queue.cpp`.

Blocked on FixedSlot tests being migrated away from these codes first.

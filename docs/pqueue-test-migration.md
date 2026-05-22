# pqueue Test Migration Plan

**Editing rules:** ASCII only -- no Unicode symbols (no checkmarks, arrows, emoji). This file is compiled to PDF via LaTeX and non-ASCII characters cause build warnings or missing glyphs.

Test migration covers two suites: POSIX (doctest, run via `make -j12 test`) and Arduino
(LittleFS on-device, run via `pio test`). Both follow the same principle: axe
fixed-slot implementation tests, port public Queue/Outbox semantics to AppendLog config,
keep AppendLog-native tests unchanged.

---

## POSIX tests

The POSIX test suite has been migrated to AppendLog config. All FixedSlot-specific
test files (`pqueue_file_store.cpp`, `pqueue_rebuild_metadata.cpp`,
`pqueue_diagnostics.cpp`) have been deleted. The following files are current:

- `pqueue_append_log.cpp` -- AppendLog store unit tests (unchanged)
- `pqueue_append_log_manifest.cpp` -- manifest tests (unchanged)
- `pqueue_append_log_rollover.cpp` -- segment rollover tests (unchanged)
- `pqueue_append_log_compaction.cpp` -- compaction tests (unchanged)
- `pqueue_append_log_seq_edges.cpp` -- sequence edge cases (unchanged)
- `pqueue_append_log_validate.cpp` -- AppendLog validate tests (unchanged)
- `pqueue_append_log_support.h` -- AppendLog test helpers (unchanged)
- `pqueue_envelope.cpp` -- envelope codec tests (unchanged)
- `pqueue_http_request_envelope.cpp` -- HTTP envelope tests (unchanged)
- `pqueue_http_outbox.cpp` -- HTTP outbox tests (unchanged)
- `pqueue.cpp` -- Queue API tests on AppendLog config; 9 FixedSlot tests killed,
  13 ported, 1 rewritten (budget-based QueueFull)
- `pqueue_full_queue_policy.cpp` -- all 5 DropOldest/RejectNewest tests on AppendLog
  config; uses `maxSegmentBytes=50` to force per-record rotation
- `pqueue_queue_edges.cpp` -- 5 edge tests on AppendLog config; 1 deferred (see below)
- `pqueue_outbox.cpp` -- 29 Outbox behavioral tests on AppendLog config; 3 spool-
  corruption tests killed; FakeFileSystem tests replaced with FaultInjectingFs
- `pqueue_repair.cpp` -- 5 format/lock tests on AppendLog config; 7 FixedSlot repair
  tests killed; 1 deferred (see below)

### Notes on ported behavior

**Multiple live Queue objects:** AppendLog mounts lazily on first operation. A second
Queue/Outbox instance mounts from disk and sees existing records in its in-RAM state.
Its own stats reflect disk state after mount plus its own subsequent writes.

**Pop failure injection:** AppendLog pop writes a POP event via `writeAt` on the
segment file. `FaultInjectingFs` uses `failNextWriteAtTo = "seg-"` to inject this.

**`recoverStaleLock` with v2 lock format:** POSIX recovery uses PID-only checking
(`removeStalePosixLock`). Tests that use `lockContentsForBoot` must supply a non-zero
PID: dead PID (e.g. 999999) for stale cases, current PID for live cases.

**Outbox corrupt-drop-limit test:** Ported by direct `Queue::enqueue("invalid-data")`
instead of spool-byte corruption. Records fail envelope decode (`DecodeFailed`) rather
than storage CRC (`CrcMismatch`); expected code updated accordingly.

**QueueFull sizing for Outbox test:** Outbox envelope overhead is 14 bytes (10-byte
header + 4-byte CRC) plus payload. First record costs 20 (seg header) + 24 (record
overhead) + envelope bytes. `reservedBytes = 61` holds exactly one envelope for a
3-byte payload ("one").

### Deferred POSIX tests

- `pqueue visitRecords returns read failure from active record` -- needs AppendLog
  segment read-failure injection; not yet supported by FaultInjectingFs.
- `queue format recovers corrupt metadata explicitly` -- requires manifest/segment
  file corruption setup on real POSIX FS. Defer until a targeted helper exists.
- Outbox storage-layer corruption tests (`drops corrupt front records`,
  `drops front record with corrupt storage payload CRC`,
  `drops front record with corrupt storage header`) -- need AppendLog-native
  segment/event corruption injection. Deferred to a later patch.

---

## Arduino / LittleFS tests

All four suites migrated. `test_pqueue_compaction` was AppendLog-only and untouched.

### test_pqueue_littlefs (fast suite) -- DONE

All FixedSlot tests and helpers removed. Suite is AppendLog-only. 15 tests run via
`pio test`.

Tests present:

- `test_quick_reboot_persistence` -- 3-phase reboot smoke (enqueue/pop+rewrite/verify)
  using AppendLog config
- `test_append_log_basic_fifo`
- `test_append_log_remount_persistence`
- `test_append_log_pop_persistence`
- `test_append_log_rewrite_front_persistence`
- `test_append_log_capacity_full_behavior` -- fills to `reservedBytes = 200` with
  distinct payloads "r00".."rNN", asserts `QueueFull` is hit, remounts, drains all
  accepted records in FIFO order, asserts empty
- `test_append_log_validate_clean_queue`
- `test_append_log_record_size_boundary`
- `test_append_log_multiple_queue_objects_share_same_base_path` -- both Queue objects
  enqueue within a scope block, scope closes (both destroyed), fresh remount verifies
  both records in FIFO order; tests that concurrent independent objects can share a path
  without losing writes
- `test_append_log_lock_released_after_each_operation`
- `test_append_log_locks_are_independent_across_base_paths`
- `test_append_log_outbox_backlog_persistence`
- `test_append_log_retryable_failure_does_not_drop`
- `test_append_log_compact_idle_survives_remount`
- `test_append_log_drop_oldest_evicts_and_continues` -- 12-byte payloads, `maxSegmentBytes
  = 80` (1 record per sealed segment), `reservedBytes = 600`; attempts 20 enqueues with
  DropOldest, recording only accepted payloads (not all 20 may succeed -- manifest range
  accumulation can block compaction after enough eviction cycles); remounts, finds front
  in accepted list, asserts `startIdx > 0` (at least 1 eviction), asserts
  `count == accepted.size() - startIdx`, drains accepted suffix in FIFO order, asserts empty

`appendLogQueueConfigForBase` is self-contained: `reservedBytes = 0` (no footprint
cap), `minFreeBytes = 0`, `maxSegmentBytes = 256`, `maxSegments = 8`. Capacity tests
override `reservedBytes` locally. `storage_common.h` no longer included.

### test_pqueue_littlefs_slow (reboot suite) -- DONE

All FixedSlot tests and helpers removed. Suite is AppendLog-only. 6 tests run via
`pio test`.

FixedSlot tests killed (no AppendLog equivalent): `test_reboot_wraparound`,
`test_reboot_index_fallback`, `test_reboot_missing_metadata_fails_safely`,
`test_reboot_corrupt_metadata_fails_safely`, `test_reboot_record_size_mismatch_fails_safely`,
`test_reboot_capacity_mismatch_fails_safely`, `test_reboot_tmp_orphan_ignored`,
`test_reboot_queue_full_safe` (covered by fast suite capacity test).

`appendLogQueueConfigForBase` mirrors the fast suite helper. Tests that need multiple
segments use `maxSegmentBytes = 128` (3-char payloads at 27 bytes/record, 4 records per
segment). The compaction test uses `maxSegmentBytes = 45` (one 1-byte record per segment).

Tests present:

- `test_reboot_fifo_many` -- enqueue 20 records (5 sealed segments), reboot, drain all
  in FIFO order; validates replay across multiple sealed segments
- `test_reboot_pop_remaining` -- enqueue 10, pop 4 across the first segment boundary,
  reboot, drain remaining 6 in FIFO order
- `test_reboot_rewrite_front` -- enqueue "old-front" and "tail", rewriteFront to
  "new-front", reboot, verify "new-front" then "tail"
- `test_reboot_outbox_drain` -- submit to AppendLog Outbox with RetryLater, reboot,
  drain with Sent decision; asserts attempt count and payload
- `test_reboot_compaction` -- 3-phase: phase 0 enqueue A/B/C (one per segment), reboot;
  phase 1 pop A, rewrite B->X, compactIdle (all in one boot so activeTailDependenciesTracked_
  is true, enabling the active segment to be folded into the compaction range), reboot;
  phase 2 verify X then C and drain. Validates compaction durability after a real power cycle.
- `test_churn_without_reboot` -- 5 rounds of enqueue 6/rewriteFront/pop-half/compactIdle
  against a model vector; verifies count after each round, drains the model at end

### test_pqueue_littlefs_soak -- DONE

All FixedSlot tests and helpers removed. Suite is AppendLog-only. 1 test run via
`pio test`.

Removed: `storage_common.h`, `kCapacityRecords`, `kRecordSizeBytes`, `slotSize`,
per-cycle `mountLittleFs`, `verifyAndRestoreModel` (the old drain-and-re-enqueue
pattern that is unnecessary for AppendLog since remount already holds correct state).

Config: `maxSegmentBytes=128`, `reservedBytes=0`, `maxSegments=16`, `kCycles=30`.

Test present:

- `test_model_driven_remount_soak` -- 30 cycles; each cycle: enqueue 1 record, pop
  if backlog > 8, extra pop every 3 cycles. After each cycle: `LittleFS.end()` +
  `LittleFS.begin(true)` (real filesystem remount), then a fresh Queue verifies count
  and front match the model. Final pass: `queue.validate()` (asserts ok and zero
  errors), full drain in FIFO order, assert empty.

Scope notes:

- `compactIdle` excluded: compaction outputs higher-generation segments; when the old
  active tail later rotates it becomes non-contiguous, fragmenting manifest ranges
  under `kManifestMaxRanges=4`. Append-log compaction is covered by focused POSIX
  tests. A dedicated LittleFS compaction soak is deferred.
- `rewriteFront` excluded: rewrite+remount interactions are covered by targeted POSIX
  tests. A dedicated LittleFS rewrite soak can be added separately.


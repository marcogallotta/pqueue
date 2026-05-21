# pqueue Test Migration Plan

**Editing rules:** ASCII only -- no Unicode symbols (no checkmarks, arrows, emoji). This file is compiled to PDF via LaTeX and non-ASCII characters cause build warnings or missing glyphs.

Test migration covers two suites: POSIX (doctest, run via `make -j12 test`) and Arduino
(LittleFS on-device, run via `pio test`). Both follow the same principle: axe
fixed-slot implementation tests, port public Queue/Outbox semantics to AppendLog config,
keep AppendLog-native tests unchanged.

---

## POSIX tests

### Keep unchanged

These are AppendLog-specific or storage-independent and require no changes:

- `pqueue_append_log.cpp`
- `pqueue_append_log_manifest.cpp`
- `pqueue_append_log_rollover.cpp`
- `pqueue_append_log_compaction.cpp`
- `pqueue_append_log_seq_edges.cpp`
- `pqueue_append_log_support.h`
- `pqueue_envelope.cpp`
- `pqueue_http_request_envelope.cpp`
- `pqueue_http_outbox.cpp`

### Delete entirely

Fixed-slot implementation tests with no AppendLog equivalent:

- `pqueue_file_store.cpp` (29 tests) -- spool layout, checkpoint slots, journal replay,
  partial writes, config mismatch. Straight delete.
- `pqueue_rebuild_metadata.cpp` (6 tests) -- scans spool slots, writes fresh checkpoint.
  No AppendLog analog.
- `pqueue_diagnostics.cpp` (3 tests) -- operates only on `FileStoreLayoutDiagnostic`.
  Goes with `diagnostics.h`.
- `tests/support/pqueue_file_store_support.h` -- replaced by a new backend-agnostic
  support header (see below).

### New support header

Create `tests/support/pqueue_queue_support.h` replacing `pqueue_file_store_support.h`:

- Keep: `FakeFileSystem`, `captureEvent` -- backend-agnostic, used by AppendLog tests too.
- Add: `makeAppendLogQueueConfig` -- constructs an AppendLog `Config` with explicit
  budget fields (`maxSegmentBytes`, `maxSegments`, `reservedBytes` (maps to internal
  `maxTotalBytes`), `storeLayout = AppendLog`).
- Delete: `makeStore` (constructs `FileStore`), `corruptSlotHeader`, `corruptSlotPayload`,
  `slotSize` -- all spool-offset arithmetic, gone with FixedSlot.

### pqueue.cpp -- port semantic tests, kill FixedSlot internals

**Kill (9 tests):**

- `pqueue validate detects corrupt active slot payload`
- `pqueue validate ignores inactive corrupt slots`
- `pqueue validate caps reported errors`
- `pqueue validate fails when active lock file exists`
- `pqueue validate reports corrupt checkpoint slots even when fallback checkpoint exists`
- `pqueue validate reports corrupt journal entry before later journal data`
- `pqueue recovers enqueued records from journal before checkpoint`
- `pqueue recovers popped records from journal before checkpoint`
- `pqueue ignores torn final journal entry and keeps valid prefix`

Also delete helpers `testRecordRegionOffset`, `testCheckpointOffset`,
`testJournalOffset`, `testSlotOffset` -- all spool offset arithmetic.

**Port to AppendLog config (13 tests):**

- `pqueue starts empty`
- `pqueue preserves FIFO order`
- `pqueue supports multiple live Queue objects on the same base path`
- `pqueue survives reopening from disk`
- `pqueue rewriteFront updates the front record without popping it`
- `pqueue accepts records exactly at the configured max size`
- `pqueue rejects records over the configured max size`
- `pqueue matches std::deque over deterministic random operations`
- `pqueue active lock file prevents queue operation`
- `pqueue lock timeout emits a clear diagnostic event`
- `pqueue recovers stale POSIX pid lock`
- `pqueue releases lock after each operation`
- `pqueue validate reports clean active records`

**Rewrite (1 test):**

- `pqueue rejects newest record when the fixed ring is full` -- the slot-count capacity
  model disappears entirely. Rewrite as an AppendLog budget/full test: fill by
  `reservedBytes`, expect `QueueFull`, verify existing records survive.

### pqueue_full_queue_policy.cpp -- port all 5

Port to AppendLog config. Replace "160 bytes = 3 slots" capacity assumption with
explicit AppendLog budget config. All 5 test intents survive:

- `RejectNewest returns QueueFull when full`
- `DropOldest evicts front record when full`
- `DropOldest emits warning event on eviction`
- `DropOldest preserves FIFO order after evictions`
- `DropOldest on empty queue enqueues normally`

### pqueue_queue_edges.cpp -- port 5, defer 1

**Port (5 tests):**

- `pqueue reports invalid storage config` -- AppendLog also validates config (e.g.
  zero `maxSegmentBytes`); use an AppendLog-invalid config case.
- `pqueue rewriteFront rejects oversized record and keeps front unchanged`
- `pqueue visitRecords rejects null visitor`
- `pqueue visitRecords stops when visitor returns false`
- `pqueue pop preserves front when index write fails` -- rewrite failure injection
  for AppendLog append failure rather than spool writeAt failure.

**Defer (1 test):**

- `pqueue visitRecords returns read failure from active record` -- needs AppendLog
  segment read-failure setup, not spool offset injection. Defer to a later patch.

### pqueue_outbox.cpp -- port behavioral tests, defer corruption tests

**Port to AppendLog config (28 tests):** all retry/backoff, rate limiting, drop policy,
FIFO order, persistence, attempts counter, send error paths, QueueFull on retry, drain
rate, and unknown decision tests. These are pure Outbox semantics with no storage
coupling.

**Kill or defer (3 tests + helpers):**

- `pqueue outbox drops corrupt front records`
- `pqueue outbox drops front record with corrupt storage payload CRC`
- `pqueue outbox drops front record with corrupt storage header`

Also delete `outboxSlotOffset`, `flipSpoolByte`, `corruptOutboxSlotHeader`,
`corruptOutboxSlotPayload` helpers. AppendLog-native outbox corruption testing
(via segment/event corruption) is deferred to a later patch.

The one remaining test in this area -- `pqueue outbox emits dropped event when stored
envelope cannot be decoded` -- tests envelope-level corruption, not spool-level, and
survives unchanged.

### pqueue_repair.cpp -- keep format and lock recovery, kill the rest

**Keep/port (5 tests):**

- `queue format clears records and allows reuse` -- AppendLog has `format()`.
- `queue recoverStaleLock removes previous-boot token lock`
- `queue recoverStaleLock refuses current-boot token lock`
- `queue recoverStaleLock removes stale POSIX pid lock`
- `queue recoverStaleLock refuses live POSIX pid lock`

**Defer/inspect (1 test):**

- `queue format recovers corrupt metadata explicitly` -- may corrupt checkpoint/spool
  metadata directly; needs inspection before porting. AppendLog equivalent would target
  manifest/segment corruption. Do not port blindly.

**Kill (7 tests):** `dropFrontIfCorrupt` tests (3), `validate suggests format/dropFront`
tests (3), `validate reports MetadataMissing` (1). All FixedSlot repair policy with no
AppendLog equivalent.

### Patch order

1. Create `pqueue_queue_support.h`; update all consumers; keep old support header
   temporarily.
2. Migrate `pqueue.cpp`, `pqueue_full_queue_policy.cpp`, `pqueue_queue_edges.cpp`.
3. Migrate `pqueue_outbox.cpp`; strip spool corruption helpers.
4. Migrate `pqueue_repair.cpp`; strip FixedSlot repair tests.
5. Delete `pqueue_file_store.cpp`, `pqueue_rebuild_metadata.cpp`,
   `pqueue_diagnostics.cpp`, old `pqueue_file_store_support.h`.
6. Grep pass: `FileStore`, `FileStoreConfig`, `storage_common`, `pqueue.spool`,
   checkpoint/journal constants, `RecordHeader`, slot offset helpers.

---

## Arduino / LittleFS tests

Four test suites exist. `test_pqueue_compaction` is AppendLog-only and is untouched.
The other three require migration.

### test_pqueue_littlefs (fast suite)

Rewrite in place. Strip all FixedSlot tests and helpers; keep and expand the
AppendLog block.

**Delete the existing FixedSlot implementations of all tests in this suite, then
re-add or keep their semantic intent as AppendLog tests where listed below.**

Also delete without replacement:

- `corruptSlotPayload`, `slotSize`, `recordRegionOffset`, `recordSlotOffset`, `kSpoolPath`
- `test_corrupt_active_record` -- direct spool offset injection; torn-tail recovery is
  transparent in AppendLog and covered by POSIX tests
- `test_outbox_drops_corrupt_front_record_on_littlefs` -- no AppendLog spool equivalent;
  defer segment-level corruption testing to a later patch

**Rewrite for AppendLog (keep intent):**

- Quick reboot smoke preamble (`runQuickRebootSmokePhaseIfNeeded`) -- rewrite using
  AppendLog config; keep the same 3-phase structure (enqueue, pop+rewrite, verify)
- `appendLogQueueConfigForBase` helper -- currently chains through `queueConfigForBase`
  which calls `slotSize()` from `storage_common.h`; rewrite as self-contained. Set
  `reservedBytes = 0` (disables footprint cap) for the default config; tight
  `reservedBytes` values belong only in the capacity and DropOldest tests. Remove
  `queueConfig`, `queueConfigForBase`, and all FixedSlot slot-size helpers.

**Port as AppendLog variants (currently FixedSlot-only):**

- `test_basic_fifo`
- `test_remount_persistence`
- `test_pop_persistence`
- `test_rewrite_front_persistence`
- `test_validate_clean_queue`
- `test_record_size_boundary`
- `test_multiple_queue_objects_share_same_base_path`
- `test_queue_lock_released_after_each_operation`
- `test_littlefs_locks_are_independent_across_base_paths`
- `test_outbox_backlog_persistence`
- `test_retryable_failure_does_not_drop`

**Keep as-is (already AppendLog):**

All existing `test_append_log_*` tests stay unchanged. This includes
`test_append_log_compact_idle_survives_remount`, which already covers the
rewrite+compactIdle+remount scenario (enqueue A/B/C, rewriteFront, compactIdle,
remount, verify).

**Add new:**

- `test_append_log_capacity_full_behavior` -- fill to `reservedBytes`, verify
  `QueueFull` is returned and existing records survive; uses AppendLog budget fields,
  not slot-count capacity
- `test_append_log_drop_oldest_evicts_and_continues` -- configure `DropOldest` policy,
  enqueue past `reservedBytes`, verify oldest is evicted and queue stays usable

### test_pqueue_littlefs_slow (reboot suite)

Rewrite in place (same folder and env name). All FixedSlot reboot tests are deleted.
New content is AppendLog-only.

**Kill entirely (no AppendLog equivalent):**

- `test_reboot_wraparound` -- fixed-slot ring-buffer wraparound
- `test_reboot_index_fallback` -- checkpoint slot A/B election
- `test_reboot_missing_metadata_fails_safely` -- checkpoint format
- `test_reboot_corrupt_metadata_fails_safely` -- checkpoint format
- `test_reboot_record_size_mismatch_fails_safely` -- spool geometry check
- `test_reboot_capacity_mismatch_fails_safely` -- spool geometry check
- `test_reboot_tmp_orphan_ignored` -- fixed-slot `.tmp` file convention

**Port as AppendLog variants (keep intent, rewrite config and setup):**

- `test_reboot_fifo_many` -- enqueue 20 records spanning multiple segments, reboot,
  drain all; validates segment replay across multiple sealed segments
- `test_reboot_pop_remaining` -- enqueue, pop some across a segment boundary, reboot,
  verify remainder
- `test_reboot_rewrite_front` -- enqueue, rewrite front, reboot, verify
- `test_reboot_outbox_drain` -- submit to AppendLog Outbox, reboot, drain
- `test_churn_without_reboot` -- rewrite for AppendLog with `rewriteFront` and
  periodic `compactIdle`

**Add new:**

- 4-phase compaction reboot: phase 0 enqueue A/B/C, reboot; phase 1 pop A and
  rewrite B to X, reboot; phase 2 `compactIdle`, reboot; phase 3 verify X and C and
  drain. Validates compaction durability after real power cycles, not just in-process
  remount.

### test_pqueue_littlefs_soak

Rewrite in place. Not a config swap: the soak is deliberately re-parameterised for
AppendLog behavior.

Parameters: `maxSegmentBytes` small enough to exercise rotation and compaction across
cycles; `maxSegments` high enough not to deadlock the model; `compactIdle(N)` called
every few cycles.

Each cycle: perform enqueue/pop/rewriteFront operations against the existing queue and
model; optionally call `compactIdle`; destroy and recreate the Queue object (remount);
verify live queue contents match the model. Do not rebuild state by re-enqueuing model
contents -- after remount the queue already holds the correct state. Final pass: full
drain and validate.

Fixed-slot capacity assumptions (`kCapacityRecords * slotSize`) are removed. Capacity
is defined by `reservedBytes`.

### Patch order (Arduino)

Patch 1: fast suite (`test_pqueue_littlefs`). Run on device before proceeding.
Patch 2: slow reboot suite (`test_pqueue_littlefs_slow`).
Patch 3: soak (`test_pqueue_littlefs_soak`).
`test_pqueue_compaction` and `platformio.ini` env names are untouched until Patch 1
is confirmed green.

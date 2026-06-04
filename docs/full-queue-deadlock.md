# Full-Queue Rollover Deadlock

This document describes a deadlock in which a queue that is full of live
records cannot make progress in either direction (enqueue or pop), and which
the doctor cannot currently repair without destroying data. It records the
mechanism, why each escape path fails, and the recovery and debugging plan.

This is a design-level issue, not corruption. A queue in this state passes
`validate()` cleanly.

---

## Symptom

Every mutating operation fails with `RangeLimitExceeded`:

```text
AppendLogStore.commitPop:     range_limit_exceeded; compaction required before rollover
AppendLogStore.commitEnqueue: range_limit_exceeded; compaction required before rollover
```

The queue accepts nothing new and drains nothing out. A `--diag` shows the
manifest at the maximum range count with a full active tail, for example:

```text
winner_epoch=N ranges=4 tail_gen=00000181 next_gen=00000189
  range[0] 00000184..00000185
  range[1] 00000182..00000182
  range[2] 00000186..00000188
  range[3] 00000183..00000183
segments=8 dangling=0
validate: OK  records_checked=126  issues=0
```

Note the ranges are non-contiguous and out of generation order. That ordering
is the FIFO replay order, a consequence of rotate-before-compact (see the
"Orphan tail after rotate-before-compact" item in `internals.md`). The
fragmentation is what consumes all available range slots while the stored data
volume is modest.

---

## Mechanism

The manifest can hold at most `kManifestMaxRanges` sealed ranges
(`append_log_common.h`; currently 4, bounded by the 64-byte LittleFS inline
manifest budget). Rollover promotes the full active tail to a new sealed range.

`rotateSegment()` (`append_log_store/store.cpp`) builds the prospective range
list first and refuses before any I/O if it would overflow the cap:

```cpp
if (newRanges.size() > kManifestMaxRanges) {
    return Status::failure(StatusCode::RangeLimitExceeded,
        "segment range limit exceeded; compaction required before rollover");
}
```

The promoted tail merges into the preceding range only if it is contiguous
(`newRanges.back().endGen + 1 == r.startGen`). When the existing ranges are
fragmented so that the tail generation is not contiguous with the last range, a
new slot is required, and at the cap the rotation fails.

Both write paths require a rotation once the tail is full:

- **Enqueue** (`commitEnqueue`): when `activeSegmentBytes_ + eventBytes >
  maxSegmentBytes`, it attempts compaction (if `needsCompaction()`), then calls
  `rotateSegment()`. If compaction frees no range, rotate fails.
- **Pop** (`commitPop` -> `appendPopEvent`): a pop is recorded by appending a
  20-byte POP tombstone. When `activeSegmentBytes_ + kPopEventBytes >
  maxSegmentBytes`, `appendPopEvent()` calls `rotateSegment()` first. At the cap
  this fails before the tombstone is written, so the pop fails.

With the tail full and the cap reached, neither path can proceed. The queue is
wedged.

---

## Why pop cannot escape (the core design flaw)

Pop is logically a removal but physically an append: it writes a tombstone
before the front record is dropped. So reclaiming space requires popping,
popping requires writing a tombstone, writing requires either room in the tail
or a rotation, and rotation is gated by the range cap.

The consequence is that the one operation that only ever *removes* data inherits
the failure mode of *growth*. A bounded queue should always be drainable; here
it is not. A full queue that cannot be drained has no self-recovery path.

---

## Why compaction cannot help

Compaction reclaims dead bytes only. `chooseCompactionRange()`
(`append_log_store/compaction.cpp`) skips any range with no dead bytes and
returns `nullopt` when none has dead bytes:

```cpp
if (bestRatio <= 0.0f || bestIdx >= manifestRanges_.size()) return std::nullopt;
```

`compactOneSegment()` then returns `noOp`. A queue that filled with live records
and never drained has effectively no dead bytes, so there is nothing to reclaim.
This is capacity exhaustion, not fragmentation that compaction can resolve.

The enqueue path already calls `compactOneSegment()` before `rotateSegment()`,
so the self-heal attempt is made on every blocked enqueue and is a no-op every
time. Pop never attempts compaction at all (`needsCompaction()` is checked only
in `commitEnqueue`).

---

## Why the doctor cannot repair it without data loss

The doctor's mutating commands and their behaviour on a wedged-but-valid queue:

- `--compact` / `--compact-all`: run `compactIdle` -> `compactOneSegment`, which
  returns `noOp` because there are no dead bytes. No effect.
- `--drop-front-if-corrupt`: `dropFrontIfCorrupt()` removes the front record
  only when it is provably corrupt. The wedged queue validates clean, so this
  returns `front_not_corrupt`, `changed=0`. There is no unconditional drop-front
  command.
- `--recover-stale-lock`: not applicable on LittleFS (in-process mutex, no
  persistent lock state).
- `--format`: clears the deadlock, but destroys all queued records.

So the only doctor command that resolves this state is `--format`, which is
destructive. There is currently no non-destructive on-device recovery for a
live-full, range-deadlocked queue.

---

## Recovery and debugging plan

If the queued records can be discarded, `--format` is the correct operational
recovery. `--dump-all` is optional: it is only useful for offline debugging and
regression reproduction, and it is not a backup (raw binary, no restore path).
Compaction, drop-front, and other non-destructive recovery are no-ops or
irrelevant in this state and should not be attempted.

> **Do not combine `--dump-all` and `--format` in one invocation.** The doctor
> host tool runs commands in a fixed order regardless of flag order, and
> `format` runs before `dump-all`. A combined invocation therefore wipes the
> queue and then dumps the freshly formatted (empty) store. If a snapshot is
> wanted, dump in a separate invocation, verify it, then format.

1. **Snapshot the spool off-device** for offline analysis (optional):

   ```text
   pqueue_doctor.py ... --dump-all --out-dir <dir>
   ```

   This transfers `manifest-a.bin`, `manifest-b.bin`, and every `seg-*.bin`. It
   is a forensic snapshot, not a backup: the files are raw binary and there is
   no restore path. Do not write them back to a device.

2. **Format on-device** to restore service:

   ```text
   pqueue_doctor.py ... --format
   ```

   This reinitialises the queue; it resumes accepting records immediately. The
   queued records are lost (acceptable only when the caller can tolerate it).

3. **Debug locally with the dumped spool.** The dumped files are the exact
   on-disk state that wedged, so they are the ground truth for reproducing the
   deadlock in the POSIX build: load them, mount, and confirm `rotateSegment()`
   returns `RangeLimitExceeded` for both an enqueue and a pop. Turn the
   reproduction into a regression test under `tests/posix/`, and/or extend the
   compaction simulator to drive the queue into this fragmented-full state. The
   goal is a deterministic reproduction that a fix can be validated against.

---

## Fixes required

Action items only; each lists the file/doc it touches. Completed items are
removed (see git history). The doctor-stranding fixes are done: device-side idle
timeout in `runSession()` (`session.h`, the un-jammable guarantee), and the host
tool's detect-and-recover, `try/finally` DONE, and format/dump rejection
(`tools/pqueue_doctor.py`).

### The deadlock itself (`append_log_store/`)

- **Guarantee pop can always make progress on a full queue.** The core fix.
  Design not yet settled; see "Design notes: fixing pop (#8)" below.
- **Fix the `needsCompaction` gap (secondary).** `needsCompaction()`
  (`compaction.cpp:657-664`) checks only `maxSegments` and `minFreeBytes`, not
  the manifest range cap. So `commitEnqueue` can go straight to `rotateSegment`
  and fail with `RangeLimitExceeded` without first attempting compaction. Add a
  range-cap-pressure condition so enqueue tries compaction when near the cap.
- **A larger range cap** (`kManifestMaxRanges`, `append_log_common.h`; see also
  "Configurable kManifestMaxRanges" in `internals.md`) reduces how easily
  fragmentation exhausts the slots, but does not eliminate the failure: any
  finite cap can be reached, and the pop-cannot-progress flaw remains. Mitigation
  only.

Doctor entry timing (the trigger was polled only during the sleep window) is
handled in the consuming firmware by polling at work-cycle phase boundaries; a
single long blocking call still cannot be interrupted mid-flight, but the device
idle timeout and host detect-and-recover cover that residual.

---

## Design notes: fixing pop (#8)

Status: not decided. Captured mid-discussion to resume later. No code written.

### The invariant to build to

Given any mounted queue with count > 0, `pop()` must either succeed or fail only
on real I/O / corruption -- never on `RangeLimitExceeded`. A committed pop must
not require active-tail rotation or an additional manifest range.

### Rejected: reserve one tombstone + ordinary compaction

Idea was: reserve `kPopEventBytes` of tail headroom so the first pop always
writes its tombstone, then let compaction reclaim a range. It does NOT guarantee
progress. A pop frees a *range slot* only if it removes the last live record in
a manifest range. If the head's range stays partially live, compaction rewrites
it to a new generation but the range count stays the same (e.g. `{1} -> {10}`),
the tail is still full, and the next pop still needs an impossible rotation. The
scarce resource is manifest range slots, not bytes. One reserved tombstone bounds
nothing; you would need enough reserve to drain an entire range, which is large
and workload-dependent.

### Option A -- durable head/acked sequence in the manifest

Store FIFO pop progress in the manifest (`headSequence`/`ackedThrough`); replay
ignores records below it. Pop = advance head + publish manifest; it never appends
to the tail, so it never rotates. Pros: the pop path is *uniform*, so the
critical path is the common path and is always exercised (no rarely-run recovery
code to rot) -- strong for a data-integrity component. Cons: manifest format
version bump, replay rework, migration of existing stores; a manifest publish per
pop.

### Option B -- `commitPopByCompaction()` fallback

Keep the fast tombstone-append for normal pops; only when `appendPopEvent()`
would need an impossible rotation, commit the pop by rewriting the head's source
range excluding the front record, publishing, deleting the old. Removing the head
is a prefix removal, so range count stays or drops, never grows -> meets the
invariant. Reuses `compactRange` machinery and also recovers already-wedged
stores. Con (raised in review): this critical recovery path is barely exercised
in production, exactly where crash-interleaving bugs and refactor rot hide. If
chosen, it must be driven continuously in the compaction simulator/CI and pinned
by the `~/pqdump` regression.

### Option C -- never enter a too-full state (prevention)

Treat the wedge as capacity exhaustion and apply backpressure before it happens:
keep the manifest from reaching the cap with a full tail (e.g. proactive
compaction to keep a free range slot / keep ranges mergeable), and when the queue
is full of live data, fail enqueue cleanly (`QueueFull`) or `DropOldest` rather
than deadlock. Simpler in spirit and matches bounded-queue semantics. Caveats:
in the all-live case compaction cannot reduce ranges (no dead bytes), so
prevention there reduces to backpressure (data loss or rejection); and prevention
does NOT recover an already-wedged store, so a recovery path (B-style or format)
is still needed for existing spools like `~/pqdump`.

### Leaning and open question

Current lean is A (uniform path = robust, and likely perf-acceptable), but the
robustness-vs-change-surface tradeoff against B, and B-or-C as the
already-wedged recovery path, are unresolved.

### MUST do before choosing: anchor in real perf metrics

The perf reasoning so far (current pop already does a per-pop `writeAt` ~24ms;
manifest publish ~25-35ms; drain rate-capped at 20/s so per-pop cost is
immaterial) is from the `internals.md` timing reference and config, NOT measured
for this decision. Before committing to A/B/C, measure on-device: actual per-pop
cost today, manifest-publish cost, drain throughput under the rate cap, and the
cost of a compaction-style pop. Decide against real numbers, not the reference
table.

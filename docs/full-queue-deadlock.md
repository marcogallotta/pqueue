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

Status: design DIRECTION settled; NOT yet "resolved". Lead is the byte-bounded
elastic range cap (#1). Settled and code-verified: the elastic cap itself, the
hot-path compaction-trigger change, the cap number (1024 ceiling / 4 soft target),
the perf/range-growth model, and the `<=4` audit. See "Working design for #1/#2/#3". NOT resolved until two things
land in the implementation plan: (1) the **drain reserve is not yet proven** --
the one-segment-bridge argument assumes a clean drain and is optimistic under
`rewriteFront`-heavy / flapping drains near full; needs a fuzz test; (2) a set of
**confirmed code gaps** (manifest-byte accounting, growth, DropOldest loop,
targeted front reclaim, no-tiny-segments, mount bounds, rewrite admission) listed
under "Confirmed code gaps". The head file (#2) remains the cleaner-drainability
fallback. Shortlist/detail below.

### Shortlist and ranking (current)

After disproving the head-in-tail assumption, no single option is committed.
Current shortlist (best first):

1. **Byte-bounded elastic range cap (lead).** Stop treating the 4-range cap as
   hard. Keep 4-range inline manifests in the common case, but allow the manifest
   to *spill* past 4 ranges when a rotation would otherwise fail, then let
   compaction shrink it back. Size the ceiling so manifest slots can never be the
   binding constraint before flash bytes are -- then "full" is always a legitimate
   bytes-full, never a slot-jam. Why it leads:
   - **No on-disk format change.** The format already serialises N ranges
     dynamically (`rangeCount` u16, dynamic `headerBytes`/`reserve`, `ranges` is a
     vector); replay already reads N ranges; LittleFS stores a file inline or in
     blocks transparently by size, so there is no explicit small<->large
     transition to manage and no version bump. The change is the policy cap plus
     the three gates (rotate `store.cpp:412`, parse `common.cpp:272`, compaction
     split `compaction.cpp:201/247`).
   - **Removes the entire `RangeLimitExceeded` class**, enqueue *and* pop, not
     just pop. Keeps POP semantics unchanged.
   - **Demotes compaction from correctness-critical to a space optimisation.**
     With slots byte-bounded, mis-timed or absent `idleCompact`/`needsCompaction`
     can only waste space and slow publishes -- never deadlock. Removes a foot-gun
     from the API contract.
   - **Plausible perf win on the fill-up path.** Today, nearing the cap forces
     compaction *during* the burst (and near the cap with all-live data
     `commitEnqueue` calls `compactOneSegment()` and gets a `noOp` every enqueue
     -- wasted hot-path work). With an elastic cap a burst can spill into another
     range (a small manifest-publish cost) and defer the actual compaction to
     idle -- which is exactly where `internals.md` says compaction belongs. A
     manifest publish is far cheaper than a read+rewrite compaction pass, so this
     trades a small per-rotation cost for avoiding heavy mid-burst compaction.
     Not free: at idle there are more ranges to collapse and publishes are bigger
     while fragmented, but the heavy work moves off the latency-sensitive path.
     Conditional: only holds if `maxSegments` is widened (see checks) -- otherwise
     `needsCompaction()` still fires hot-path compaction at 16 segments.
   - Costs: larger, ~7ms+ slower manifest publishes *while fragmented* (bounded;
     common case stays inline); a little more to scan at mount/compaction; pop is
     still a normal per-pop write (not free). To verify: nothing else assumes
     <=4 beyond those gates and the doctor; choose the ceiling derivation against
     worst-case (many tiny) segments.
2. **Separate head checkpoint file.** Cleanest *drain* invariant: pop advances a
   head sequence in its own dual-slot file and writes nothing to the log, so it
   never rotates. Location-independent, fixes the `rewriteFront` hole. But heavier
   than #1 (new metadata file + replay-skip-below-head + three-file crash
   ordering, designed below) and it only directly fixes *pop*, not enqueue-at-cap.
   Pick this over #1 if "pop writes nothing" is worth the extra surface.
3. **Admission control / prevention (Option C).** Reject/`DropOldest` before
   wedging. Largely subsumed by #1 (which makes the wedge impossible); still
   useful as the deliberate bytes-full policy. Does not repair an already-wedged
   store on its own.
4. **Head pointer inside the manifest (Option A, in-manifest).** Hits the 66B
   budget -> +~7ms/publish always or drop to 3 ranges. Inferior to #1/#2.
5. **Out-of-band attempt count / avoid per-retry `rewriteFront` (likely
   essential, pairs with #1).** `rewriteFront` rewrites the whole record to bump a
   1-byte attempt counter on every retry, and retries are the NORMAL workload here
   -- it is the dominant driver of fill/fragmentation and what breaks #1's reserve
   proof. Store attempts out-of-band (sidecar, or RAM-only reset-on-reboot) so a
   retry doesn't rewrite the record. Not a standalone fix (long all-live backlog
   still grows ranges) but it removes the normal-workload churn. Part of the
   target design (elastic cap + admission reserve + rewrite admission +
   out-of-band retry attempts).
6. **Keep B and solve head-in-tail** via a "rewrite tail excluding head" path.
   Hardest; blocked (detailed design below, kept for reference).

Lead is #1 (lowest surface, broadest fix, de-risks compaction). #2 is the
elegance alternative. **Target design = elastic cap + admission reserve + rewrite
admission + out-of-band retry attempts.** (= #1 with its `drainReserveBytes`
admission, that admission also applied to the rewrite path, plus #5 to remove the
normal-workload `rewriteFront` churn.) Together they give: impossible wedge, clean
bytes-full behaviour, and no churn-driven fragmentation in normal operation.

### Lead candidate (#1, elastic range cap): design + open checks

**Load-bearing condition (this is the whole thing).** The cap removes the
deadlock *only if* it is >= the worst-case possible segment count for the store.
If the cap is merely "bigger" (e.g. 8, 16) it still just postpones
`RangeLimitExceeded` to a higher number. It must be derived from the flash byte
budget, not picked arbitrarily -- and the derivation must include manifest
overhead, not just segments, because the manifest itself grows with range count
(two slots, 8 bytes per range each) and consumes flash too. Pick the cap from
segments alone and the manifest's own growth can become the new binding limit.

The cap `N` is the largest `N` whose worst-case footprint fits the budget:

```
N * minSegmentBytes                  (segments: each range needs >= 1 segment,
                                      smallest = header + one event)
+ 2 * (kManifestFixedBytes + 8 * N)  (both manifest slots, sized at N ranges)
+ head-file / other metadata
<= flash budget
```

Solve for the max `N` (a floor of the byte budget -- round down, keep a safety
margin). Then also bound `N` by format and RAM and take the minimum:
- format: `headerBytes` is `u16`, so `N <= (65535 - kManifestFixedBytes) / 8`
  (~8188);
- RAM: `activeGenerations_` and `records_` and the manifest read buffer must fit
  ESP RAM at `N` ranges.

`cap = min(byte-budget N, format N, RAM N)`. Then you provably run out of bytes
before slots, so "full" is always a legitimate bytes-full, surfaced as
`QueueFull`, never a slot-jam.

**Elastic vs fixed-high.** Two ways to spend it:
- *Fixed-high cap*: set `kManifestMaxRanges` to the byte-derived bound. Simplest;
  but the manifest is non-inline (slower publish) whenever range count exceeds 4,
  and the worst-case manifest is large.
- *Elastic (spill-then-shrink)*: keep 4-range inline manifests normally, spill
  past 4 only when a rotation would otherwise fail, and let compaction shrink
  back to inline when it can. Keeps the common-case fast path; the shrink-back is
  just ordinary compaction reducing range count -- confirm no extra machinery is
  needed. Preferred if the shrink-back is genuinely free.

**Implementation checks:**
- Derive the cap from the flash budget (above), `min` of byte/format/RAM bounds.
  STILL OPEN.
- **Mount-time manifest bounds validation (REQUIRED, patch first).** `parseManifest`
  validates `startGen != 0` and `endGen >= startGen` but NOT the span;
  `applyManifestToRam` then expands every range gen-by-gen into
  `activeGenerations_`. A CRC-valid manifest with a huge span (bug, or a higher
  cap) explodes RAM on mount. Validate total generation count (sum of range spans
  + tail) against the derived cap *before* expanding. (CRC guards random
  corruption today, but this must be hardened before raising the cap.)
- **`maxSegments` semantics (REQUIRED for the perf claim).** `maxSegments` is
  `uint8_t`, default 16 (`types.h:49`); `needsCompaction()` fires when
  `activeGenerations_.size() > maxSegments`, so elastic ranges still trigger
  hot-path compaction at 16 segments. The "defer compaction to idle / perf win"
  only holds if `maxSegments` is widened (it's `uint8_t`, max 255) or decoupled
  from the hot-path trigger. Otherwise the perf claim does not stand.
- Keep admission control (#3) for the genuine bytes-full case, surfaced as a
  clean `QueueFull`/`DropOldest`, not a deadlock. STILL OPEN.

**Drainability caveat (don't overclaim).** The elastic cap removes
`RangeLimitExceeded`, but POP still appends a tombstone, so a genuinely
bytes-full filesystem can still fail a pop with a real `WriteFailed`. "Always
drainable" therefore requires reserving byte headroom for tombstones (admission
control's job: stop enqueue before it consumes the bytes pop needs). This is the
one place the head file (#2) is strictly better -- pop writes nothing, so it
drains even when bytes-full. With the elastic cap, drainability = no
`RangeLimitExceeded` + a reserved tombstone byte-headroom.

### Working design for #1/#2/#3 (from code audit + prior-art research + review)

Convergent conclusion of an independent code audit, web prior-art survey (Kafka
offsets, Bitcask dirty-ratio merge, RocksDB stalls, SQLite WAL, the
otel-collector full-disk drain deadlock), and review. Prior art validates both
the elastic cap and the head file: Kafka's out-of-band committed offset is
exactly the head-file model; reserved headroom + dead-ratio background compaction
is exactly the elastic-cap model.

**(1) Drainability reserve.** Reserve for ONE worst-case pop transaction, not for
all future pops:

```
drainReserveBytes = kPopEventBytes + kSegmentHeaderBytes (possible new segment)
                    + manifest-slot growth (one more range, x2 slots)
                    + FS-block / safety margin
```

Enforce on *every* writer, against both budgets:

```
totalOnDiskBytes + writeGrowth + drainReserveBytes <= maxTotalBytes
freeBytes        >= minFreeBytes + writeGrowth + drainReserveBytes
```

Load-bearing code hole (verified): `rewriteRecord()` / `appendRewriteEvent()`
does NO `maxTotalBytes`/`minFreeBytes` admission today -- those checks live only
in `commitEnqueue()`. Since `rewriteFront` runs on every retry, it can eat the
reserve. The reserve must be enforced on the rewrite path too.

**Addition beyond "one-pop reserve" (the subtle part).** A one-pop reserve
guarantees you can always *record* a pop, but not that a full queue can *fully
drain*: recording a pop consumes ~20 bytes and frees nothing until reclaim.
Reclaim must come from deleting whole dead units, which writes no output segment
(verified: the dead path only deletes files), NOT live-range compaction, which
needs peak `old + new` free space (verified) it won't have when full. FIFO saves
us: pops drain oldest-first, so the oldest data dies first and is removed
space-free.

The granularity matters, though. Today's **dead-range removal reclaims a whole
manifest range, and a range can span many segments** -- so "bridge until the
oldest *range* is fully dead" could demand a large reserve (a whole range's worth
of tombstones). Fix: reclaim at **segment** granularity by peeling fully-dead
segments off the *front* of the oldest range. Removing the front segment of
`{s..e}` just advances it to `{s+1..e}` -- a `startGen` bump, NOT a split -- so it
needs no new range slot and no output write. FIFO pops oldest-first, so front
segments of the oldest range die first. That bounds the bridge to **one segment's
tombstones** (~`maxSegmentBytes`/recordSize records x 20B, a few KB), not a whole
range's.

So: add front-segment reclaim within the oldest range, size the reserve (or rely
on `minFreeBytes`, 32KB today) against one segment's worst-case tombstone cost,
and make drainability depend on this space-free segment deletion -- never on live
compaction having output room. (The alternative -- sizing the reserve for the
worst-case oldest-range record count -- works but is large and ugly; prefer
segment-granular reclaim.)

**(2) Hot-path compaction trigger.** Remove `activeGenerations_.size() >
maxSegments` from synchronous `needsCompaction()` -- with elastic ranges, segment
count is no longer correctness pressure. Hot path compacts ONLY when admission
would otherwise fail (real `maxTotalBytes`/`minFreeBytes` pressure); if it still
fails, `QueueFull`/`DropOldest`. Segment count becomes an *idle* compaction hint.
Trigger idle compaction on a dead-byte ratio of sealed segments (Bitcask/Kafka
prior art) -- the metric that decouples "reclaim space" from write latency.
`maxSegments` (currently `uint8_t`=16) becomes a soft idle target (rename to
`idleCompactionTargetSegments`, and/or widen).

**(3) Cap shape: one hard cap + a soft inline target (no two modes).**
- *Hard cap* = `min(byte-budget, format u16, RAM)` derived value (1024 here).
  Rotation only ever checks this; parse/mount validate against it before
  expanding ranges. It is a *ceiling*, not an operating point.
- *Soft target* = 4 ranges, because <=64B manifests are inline and fast. Idle
  compaction prefers shrinking back toward 4 when useful; not guaranteed if data
  is all-live, which is fine.
This is "elastic" without a literal spill mode flag: rotation simply doesn't care
about 4, and idle compaction gravitates back to 4.

**Perf: the high hard cap is free in normal operation.** Manifest size and
publish cost scale with the *actual* range count (`30 + 8*N` bytes), NOT with the
hard cap -- raising the cap from 4 to 1024 adds zero bytes until you actually hold
more ranges. The cap is only approached under pathological fragmentation -- the
case that would otherwise deadlock. Standard soft-target + hard-ceiling pattern
(RocksDB level targets vs stop-writes; Bitcask background-merge target).
Independent nuance: exceeding ~4 ranges crosses the 64 B inline threshold (~7 ms
slower/publish) -- a property of being fragmented at all, not of the ceiling.

**Range-growth dynamics and the pull-back requirement (important).** A high hard
cap does NOT by itself keep the range count low; that is a separate, *required*
mechanism. Verified dynamics:
- Normal sequential enqueue+rotate keeps range count at ~1: `rotateSegment`
  merges a contiguous promoted tail into the last range rather than adding one
  (`store.cpp:407-410`). So ordinary operation does not grow ranges.
- Ranges fragment only when compaction or rewrite-churn punches non-contiguous
  gaps (mid-range dead removal, rotate-before-compact orphan tails, `rewriteFront`
  deadening scattered old locations) -- i.e. the backlog/retry workload.
- Therefore the real risk is that churn-induced fragmentation *persists* if
  nothing defragments it. The high cap prevents the deadlock; it does not keep
  ranges low.

Consequence for the design: idle/background compaction must actively pull ranges
back toward the soft target (defrag to contiguous) after a churn episode.
Restate #2 precisely: removing segment-count from the hot/correctness path is
right, but a *soft* trigger that defragments toward the target must remain.
**Compaction stays required for perf-hygiene; it is only no longer required for
correctness.** Foot-gun removed: "forget to compact -> deadlock". Remaining
obligation: "compact at idle -> stay defragmented", whose failure mode is slower,
not wedged.

**The churn workload is NORMAL for this deployment, not degraded.** This device
is an ESP32 talking to a flaky local server over home WiFi; backend failures and
retries are routine. So fragmentation pressure and the reserve-under-rewrite case
are *main-path*, not edge. And the dominant driver is `rewriteFront`: on every
`RetryLater` the outbox re-encodes the head with an incremented attempt count and
rewrites the WHOLE record (`outbox.cpp:392-398`), retrying the same front (cooldown,
no advance) until it sends. A long outage = hundreds of full-record rewrites of the
stuck head, each leaving a dead copy -- the actual engine of the fill/fragmentation
seen in the incident. This promotes candidate #4 (out-of-band attempt count) from
a mitigation to likely-essential: storing attempts in a sidecar (or RAM-only,
reset-on-reboot) instead of rewriting the record removes the normal-workload churn,
lets the elastic cap's clean-drain reserve proof actually hold, and cuts write
amplification/flash wear (a ~492 B rewrite becomes a counter bump). It does not
replace the elastic cap (a genuine long all-live backlog still grows ranges), but
it removes the dominant normal source of churn. Target design: **elastic cap +
admission reserve + rewrite admission + out-of-band retry attempts.**

**Where this leaves #1 vs #2.** The elastic cap (#1) + reserve + dead-range-based
drainability is the lead: lowest change, no format bump, keeps POP. The head file
(#2) remains strictly cleaner on pure drainability (pop writes nothing, no
reserve reasoning, drains even at the byte floor) -- the fallback if the reserve
math proves fragile.

### Computed numbers (this deployment)

Effective config (only `reservedBytes` is overridden by the firmware): `maxTotalBytes
= 524288` (512 KiB), `maxSegmentBytes = 4096`, `minFreeBytes = 32768` (32 KiB),
`recordSizeBytes = 492`. Constants: `kSegmentHeaderBytes = kPopEventBytes = 20`,
`kEnqueueOverheadBytes = 24`, `kManifestFixedBytes = 30`, manifest = `30 + 8*N`
bytes for N ranges (x2 slots), inline threshold 64 B.

**Hard cap N.**
- Typical worst-case segment count = `maxTotalBytes / maxSegmentBytes = 524288 /
  4096 = 128`.
- Recommended hard cap = **1024 ranges**. Worst-case manifest = `30 + 1024*8 =
  8222 B` (non-inline, but only at full fragmentation, which is essentially never
  -- especially after #5 removes churn; common case stays at 4 ranges / 62 B /
  inline). Well under the format ceiling of `(65535-30)/8 = 8188` ranges. RAM:
  manifest read whole into RAM (~8 KB worst case) and `activeGenerations_` ~=
  segment count -- both fine.
- **Guarantee scope (state explicitly):** slot-jam is impossible for normal and
  tolerated sealed-segment sizes (>= 512 B), NOT for pathological sub-512 B (down
  to ~44 B) sealed segments. Light coalescing keeps us in the tolerated regime;
  outside it the guarantee degrades to "very unlikely", not "provably impossible".
- Why 1024 over 256: no finite cap fully guarantees against the absolute worst
  case (min sealed segment ~44 B -> 524288/44 ~= 11916 segments, above even the
  format max 8188), so *some* min-sealed-size bound is always needed. 1024 only
  needs sealed segments >= `524288/1024 = 512 B`, which light best-effort
  coalescing of tiny remainders satisfies -- versus cap 256 needing a strict
  >= 2048 B invariant (active coalescing + a real proof burden). 1024 makes the
  min-segment bound a perf-hygiene nicety, not a correctness proof obligation, at
  the price of an 8 KB worst-case manifest that is essentially never reached.

**Drain reserve.**
- Bridge to the first front-segment reclaim = one segment's worth of tombstones.
  Max records per 4096 B segment = `floor((4096-20)/24) = 169`; tombstones = `169
  * 20 = 3380 B`. Plus one-pop overhead (`pop 20 + new-seg header 20 + manifest
  growth ~16` ~= 56 B).
- `drainReserveBytes ~= 4 KB` (3.4 KB bridge + margin); use 4096-8192.
- Effective enqueue/rewrite ceiling = `maxTotalBytes - drainReserveBytes ~=
  516 KiB`. `minFreeBytes` (32 KiB) already covers the physical bridge ~9x, so no
  change there -- the fix is *enforcing* the logical `maxTotalBytes` admission
  (minus reserve) on BOTH enqueue and rewrite, the latter of which does none
  today.

Enforce on enqueue and rewrite:
```
totalOnDiskBytes + writeGrowth + drainReserveBytes <= maxTotalBytes
freeBytes        >= minFreeBytes + writeGrowth + drainReserveBytes
```

### Final verification (doc + code, this pass)

- `rewriteRecord()` (`store.cpp`) does NO `maxTotalBytes`/`minFreeBytes`
  admission -- only `RecordTooLarge` checks -- then calls `appendRewriteEvent`.
  CONFIRMED load-bearing: the reserve must be enforced here too.
- Dead-prefix reclaim machinery already exists in `compactRange` (prefix/suffix
  classification, "dead prefix/suffix costs 0 extra ranges" `compaction.cpp:186`,
  remainder re-insertion). Front-segment reclaim for drainability is a
  *targeting/trigger* addition, not new mechanism.
- `readManifest` reads the whole slot into RAM (`manifest.cpp`), and
  `applyManifestToRam` expands per-generation into `activeGenerations_` -- both
  bound the cap by RAM; at cap 1024 the manifest is ~8 KB worst case and
  `activeGenerations_` ~= segment count -- both fine.
- Serialise/parse remain dynamic (no fixed-4 buffers); the only hard `4` is the
  policy constant + the three gates + parse check (audit above).

Design assessment: coherent and code-grounded. None of the items below hit a wall
in the current code, but they ARE required -- the design is not "resolved" until
they're built and the reserve is fuzz-proven.

### Confirmed code gaps to fix (all verified this pass)

Accounting / admission:
1. **`totalOnDiskBytes_` does not count manifest bytes.** `publishManifest` writes
   slots via raw `f->writeFile` (`manifest.cpp:115`), not the tracked path, so
   manifest bytes are excluded. Cap/reserve math that assumes manifests count is
   wrong -- either track manifest slot bytes or subtract worst-case manifest size
   (`2 * (30 + 8*cap)`) from `maxTotalBytes`.
2. **`appendGrowthBytes()` ignores manifest growth.** It returns `eventBytes +
   maybe kSegmentHeaderBytes` only (`store.cpp`). Admission must add possible
   manifest-slot growth on rotation.
3. **`rewriteRecord()` does no `maxTotalBytes`/`minFreeBytes` admission**
   (`store.cpp`); only `RecordTooLarge`. Load-bearing -- `rewriteFront` is hot on
   retries and can eat the reserve. Add full admission to the rewrite path.
4. **`DropOldest` evicts once, no loop** (`queue.cpp:255-274`). With a reserve,
   one eviction may not free enough; loop evict/retry until admission passes or
   the queue is empty.

Reclaim / compaction:
5. **No targeted front reclaim.** `chooseCompactionRange()` picks highest
   dead-ratio, not the oldest fully-dead front segment. Drainability needs an
   explicit "reclaim dead front segment of the oldest range" path triggered under
   reserve pressure (machinery exists via the dead-prefix path; needs targeting).
6. **Tiny sealed segments are possible.** `compactRange()` emits small remainder
   segments. With cap 1024 (chosen) only sub-512 B segments matter, so light
   best-effort coalescing of tiny remainders suffices -- not the strict
   "no tiny sealed segments" proof that cap 256 would have required.
7. **Keep the soft-target defrag trigger.** Move segment-count off the hot path
   (idle, dead-ratio) but retain a soft trigger that pulls ranges back toward the
   target after churn (see "Range-growth dynamics"). `maxSegments` -> soft
   `idleCompactionTargetSegments`.

Mount safety:
8. **Mount bounds-check before expansion.** `parseManifest` validates
   `startGen`/`endGen` but not span; `applyManifestToRam` then expands every range
   gen-by-gen into `activeGenerations_`. With a higher cap, validate total
   generation count against the cap BEFORE expanding.

Proof obligation:
9. **Fuzz the reserve under rewrite-heavy drain near full** (sealed live records +
   head repeatedly `rewriteFront`'d into the tail + bytes near full). If it drains
   without spurious `WriteFailed`, the one-segment reserve holds; if not, the
   reserve bound is larger than one segment and must be recomputed.

**`<=4` audit -- DONE. Raising the cap is structurally safe.** Findings:

Load-bearing (must use the new derived cap):
- `kManifestMaxRanges` constant (`append_log_common.h:25`).
- Parse gate `rangeCount > kManifestMaxRanges` (`append_log_common.cpp:272`).
- Rotate gate (`store.cpp:412`).
- Compaction split gates (`compaction.cpp:201` and `:247`).

Already dynamic -- no change needed (this is why it's safe):
- All range storage is `std::vector<ManifestRange>` (`manifestRanges_`,
  `ManifestData::ranges`, every compaction `newRanges`). No fixed `[4]` array
  anywhere.
- Manifest serialise (dynamic `headerBytes`/`reserve`/loop) and parse (reads
  `rangeCount`, dynamic) already handle N ranges.
- Doctor `--diag`/`--list` iterate `ranges.size()` (`session.h:156`); no fixed
  loop to 4.
- The only `std::array<,256>` are CRC tables; the stray `==4`/`<4` hits are
  byte-level (u32 loops, magic-size torn-tail checks) and HTTP status codes --
  none are range assumptions.

Tooling/tests to update (not load-bearing, but will break or mislead):
- `tests/posix/pqueue_append_log_rollover.cpp` "range limit exceeded returns
  failure" encodes the *current* cap=4 deadlock (plants 4 ranges, asserts the 5th
  rotation fails). Under an elastic/byte-bounded cap it should no longer fail at
  4 -- rewrite it to assert the new behaviour (spill past 4; fail only at the
  byte-derived ceiling).
- `tools/pqueue_compaction_sim.cpp`: `rangePressureTrigger = 3` (line 30) and the
  `maxRanges` usage (line 288) -- retune for the new cap and extend to exercise
  >4 ranges and the fragmented-full state it never reaches today (`Deadlocks:
  0`).
- `append_log_common.h:90` -- stale "max kManifestMaxRanges entries" comment.

### Staged implementation plan

Implement in staged, independently-reviewable PRs -- not one giant change. Each
stage is safe to land on its own and leaves the queue working. Order chosen so
the deadlock class dies first, then drainability, then churn, then proof.

1. ~~**Elastic cap + mount bounds.**~~ **DONE.** `kManifestMaxRanges` raised to 1024; rotate/parse/compaction gates updated; mount-time range-span bounds validation added before `applyManifestToRam` expands; rollover test rewritten to assert spill-past-4 behaviour. (gaps 6 partial, 8.) -> kills the `RangeLimitExceeded` deadlock class.
2. ~~**Admission + drain reserve on enqueue AND rewrite.**~~ **DONE.** `drainReserveBytes`
   added to both `maxTotalBytes` and `minFreeBytes` admission in `commitEnqueue` and
   `rewriteRecord`; `appendGrowthBytes` now includes manifest range-entry cost on rotation;
   `totalOnDiskBytes_` now counts both manifest slot files; `AppendLogConfig::drainReserveBytes`
   field added (default 0, firmware sets 4096). (gaps 1, 2, 3.)
~~**Pre-conditions before proceeding past stage 3** (found in code review, not yet fixed):~~
- ~~**`drainReserveBytes` not wired in `Queue::makeStore`**~~ **DONE.** `pqueue::Config::drainReserveBytes` field added (default 0; firmware sets `kDrainReserveBytes`); wired through `makeStore` into `AppendLogConfig`.
- **`rewriteFront` ignores `QueueFull` under `DropOldest`** — deferred. Evicting then retrying `rewriteRecord(index_.head, record)` is a data-corruption bug: after eviction `index_.head` points to the next record, so the retry overwrites the wrong record. Correct resolution requires the caller (Outbox) to handle `QueueFull` explicitly — either drop the front and move on, or surface backpressure. Not fixed here.

3. ~~**DropOldest loop.**~~ **DONE.** `Queue::enqueue` now loops eviction until
   admission passes or the queue is empty; the single-eviction + one-retry logic
   is replaced by a `while` on `QueueFull`. (gap 4.)
4. ~~**Move segment-count compaction off the hot path.**~~ **DONE.** Removed `activeGenerations_.size() > maxSegments` from `needsCompaction()`; the pre-rotate compaction block in `commitEnqueue` is removed entirely (dead after segment-count arm gone); `maxSegments` renamed to `idleCompactionTargetSegments` in `AppendLogConfig`, `pqueue::Config`, doctor, tests, and tools; `CompactIdleResult::segmentCountExceedsTarget` added so callers can schedule idle compaction when segment count drifts above the soft target. (gap 7.)
5. **Targeted front-dead-segment reclaim** wired to drain/admission pressure
   (reuse the dead-prefix path; advance `startGen`, count-neutral). Light
   coalescing of sub-512 B remainders. (gaps 5, 6.)
6. **Out-of-band retry attempts.** Stop `rewriteFront` rewriting the whole record
   to bump the attempt counter; store attempts in a sidecar (or RAM-only,
   reset-on-reboot). Removes the normal-workload churn. (candidate #5.)
7. **Fuzz the ugly case (gates "resolved").** Near-full + `rewriteFront`-heavy
   retries + interleaved pop/drain + fragmented ranges > 4. Invariant: pop never
   returns `RangeLimitExceeded`; the queue drains without spurious `WriteFailed`.
   If it fails, the one-segment reserve is too small -- recompute. (gap 9.)

Sequencing note: 1 makes the wedge impossible; 2-3 make a full queue drainable;
4-5 keep normal operation defragmented and fast; 6 removes the dominant churn
source; 7 proves the reserve. 1 alone already prevents recurrence of the original
incident; the rest harden perf and drainability.

### Candidate #2 (separate head file): crash-ordering design

(Design notes for the head-file option, #2 in the shortlist -- kept because if we
ever prefer the "pop writes nothing" invariant this is the hard part.) The hard
part of the separate head checkpoint is crash ordering across three durable
things: segments, the manifest, and the new head file. Worked design:

**Structural simplifier: disjoint writers.** No single operation writes both the
head and the manifest. Pop writes only the head file. Enqueue / rotation /
rewrite / compaction write segments + manifest but never the head. So there is
*no cross-file atomic-commit problem* -- each of the manifest and head is its own
dual-slot, epoch-versioned, atomic publish, and we only have to govern segment
deletion relative to whichever metadata justifies it.

**Safety principle (decides every tie): fail-low, never fail-high.** A head that
lags reality re-delivers already-popped records (at-least-once duplicates, which
the ingest pipeline already dedups). A head ahead of reality skips unpopped
records (data loss). So every crash path must bias the head low. Dual-slot
publish (higher epoch wins; write inactive slot) gives this: a torn head publish
leaves the prior, lower head.

**Pop protocol.** Write the advanced head to the inactive head slot and fsync;
that publish is the commit point; only then advance the in-RAM head (mirror the
manifest's "RAM after durable publish" discipline). No segment or manifest write.
Crash before publish -> record still live, re-delivered. Crash after -> popped.

**Deletion ordering rules.**
- *Rule A (head before delete):* a segment whose records are all below the head
  may be deleted only after the head that makes them below-head is durably
  published. Crash between = segments linger harmlessly; reclaimed next pass.
- *Rule B (manifest before delete, existing):* a segment may be deleted only
  after a manifest that no longer references it (or its replacement) is published.
- Compaction *reads* the durable (== RAM) head to decide dead = popped (seq <
  head) and never copies below-head records into its output, but it never
  *writes* the head. So compaction can only ever act on records the head already
  certifies as dead.

**Replay.** Read winning manifest (live generations + order) and winning head;
scan segments; rebuild `records_` from ENQUEUE/REWRITE events with seq >= head
(skip seq < head as popped; latest REWRITE wins as today). A below-head record
whose segment was already cleaned is simply absent -- consistent either way.

**Head file loss/corruption (both slots).** Fall back to head = lowest sequence
present in the live segments (assume nothing popped). This is fail-low: redeliver
the live set, never skip. A missing head file therefore degrades to at-least-once,
not data loss.

**Load-bearing assumption to confirm:** the ingest side tolerates duplicate
delivery (the server already classifies duplicate/conflict and the firmware
handles it). If that ever stops being true, fail-low is no longer free.

**Crash-injection test matrix (required):** crash before/after head publish;
crash between head publish and below-head segment deletion; crash during
compaction (replacement written, manifest not yet published) with head at various
points; torn/corrupt single head slot; both head slots corrupt (fallback); and
interleaved `rewriteFront` + pop + compaction. The property invariant: no crash
sequence yields a head higher than the set of durably-popped records.

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

### Option A -- durable head/acked sequence in the manifest (BACK ON THE TABLE)

Idea: store FIFO pop progress in the manifest (`headSequence`); pop = advance
head + publish manifest, never touching the tail. Its decisive advantage given
the disproof below: it is **location-independent** -- pop advances a sequence
pointer regardless of where the record physically lives, so the
`rewriteFront`-into-tail behaviour that breaks Option B's head-in-tail case
simply does not arise. Correct by construction, uniform pop path.

Previously set aside on the manifest size budget (30B fixed + 4x8B ranges = 62B
vs a hard 64B LittleFS inline limit; a 4-byte head field makes it 66B, costing
~7ms per publish or forcing the cap to 3 ranges). That is a tradeoff, not a
disqualifier -- so A is **not ruled out**; keep it as a live candidate until a
clearly better solution exists. Note the manifest budget is not immovable: the
range cap can be raised (carry more ranges) by accepting a larger,
non-inline manifest (per-publish cost) or restructuring the format. Whether to
spend that to fit both `headSequence` and >=4 ranges is part of the A evaluation,
not a reason to drop A.

### Option B -- pop-via-compaction at tail-fill (candidate #5, BLOCKED)

No longer the chosen approach: blocked by the head-in-tail hole (see disproof
below) and ranked last in the shortlist. Kept as a worked design for reference.
Full design below ("Design: pop-via-compaction"). Summary: keep the fast
tombstone-append while the tombstone
fits the tail; whenever a pop would otherwise **rotate** the tail (tombstone
doesn't fit), commit it by rewriting the head's source range *excluding the front
record* instead of rotating. That removes the head without appending to the tail
or adding a range, so range-count pressure is irrelevant to committing the pop --
pop never rotates and so never returns `RangeLimitExceeded`.

Crucially this is **not** a rare wedged-corner fallback: it runs on every drain
that fills the tail (roughly once per tail-capacity worth of pops), so the
critical path is exercised continuously in normal operation, not just at the
cap. It reuses `compactRange` machinery and also recovers already-wedged stores.

OPEN: this does not yet cover the head-in-tail-at-cap case, which `rewriteFront`
makes common under the retry workload. See the design section. Not ready to
implement until resolved.

### Option C -- never enter a too-full state (prevention) (DEFERRED)

Treat the wedge as capacity exhaustion and apply backpressure before it happens:
keep the manifest from reaching the cap with a full tail (e.g. proactive
compaction to keep a free range slot / keep ranges mergeable), and when the queue
is full of live data, fail enqueue cleanly (`QueueFull`) or `DropOldest` rather
than deadlock. Simpler in spirit and matches bounded-queue semantics. Caveats:
in the all-live case compaction cannot reduce ranges (no dead bytes), so
prevention there reduces to backpressure (data loss or rejection); and prevention
does NOT recover an already-wedged store, so a recovery path (B-style or format)
is still needed for existing spools like `~/pqdump`.

---

## Design: pop-via-compaction (Option B, candidate #5 -- blocked, kept for reference)

Status: design IN PROGRESS, NOT ready to implement. The head-in-sealed-range and
head-in-tail-below-cap cases are worked out, but the head-in-tail-at-cap case is
an OPEN HOLE (see "Mechanism, by case" -> head in the active tail). Resolve that
before writing code.

### Goal and invariant

Make `pop()` satisfy: given any mounted queue with count > 0, it either succeeds
or fails only on real I/O / corruption -- never on `RangeLimitExceeded`. Pop
never rotates the tail: the fast tombstone-append handles pops while the tombstone
fits, and a pop that would otherwise rotate is committed by compaction instead.

### Not a rare path (the design point)

The compaction commit runs on **every** pop that would rotate the tail -- i.e.
once per tail-capacity worth of pops during any drain -- not only in the
fragmented-full-at-cap corner. So it is exercised continuously in normal
operation and cannot rot as an unused recovery branch. This is the deliberate
choice over a corner-only fallback. The cost is paid at tail-fill boundaries
(measured in the benchmark plan), not on every pop, so it is not the per-pop
overhead of a fully uniform path.

### Key insight (why this works where reserve+compaction failed)

The commit removes the head record by rewriting the head's source range with that
record excluded -- **without touching the tail or adding a range**. Committing the
pop therefore does not depend on freeing a range slot, so the partial-dead-range
counterexample (a live neighbour keeping the range count at the cap) does not
apply: the pop is committed by the rewrite itself.

### Trigger predicate (in `commitPop`)

Take the compaction path whenever the tombstone would not fit the active tail
(`activeSegmentBytes_ + kPopEventBytes > maxSegmentBytes`), i.e. exactly the
cases where the old code would call `rotateSegment()` for a pop. RAM-only, no
`fileSize` I/O. Otherwise take the existing `appendPopEvent` tombstone fast path
unchanged. Pop no longer calls `rotateSegment()` at all; `appendPopEvent` drops
its rotation branch.

### Mechanism, by case

Let `head = records_.front()`, in generation `head.segmentGeneration`, inside
manifest range `P = findParentRangeIdx(...)`.

- **Head alone-live in its range:** rewriting `P` excluding the head yields no
  live records for `P` -> dead-range removal drops `P`. Range count decreases.
- **Head in a partially-live range:** rewrite `P` excluding the head into a new
  output generation and splice it back in place of `P`. This is a prefix removal
  of the FIFO head, so the range count stays the same; it never grows.
- **Head in the active tail** (`head.segmentGeneration == activeGeneration_`):
  reachable, and now known to be **common**, not edge. OPEN HOLE -- see below.

> **DISPROVEN (verified in code): "head-in-tail implies all sealed ranges are
> dead" is false.** Physical generation is decoupled from sequence by REWRITE.
> `appendRewriteEvent` (`store.cpp:536`) relocates a record's `segmentGeneration`
> to the active tail while keeping its sequence and FIFO position; `records_` is a
> `std::deque` ordered by sequence so `front()` stays the lowest sequence.
> `Queue::rewriteFront()` (`queue.cpp:377-392`) rewrites the **head**, and the
> outbox calls it on **every `RetryLater`** (`outbox.cpp:398`, re-encoding the
> head with an incremented attempt count). So under the backend-failure/retry
> workload -- the very workload that wedges the queue -- the head is routinely in
> the tail while higher-sequence records remain live in sealed ranges.

Consequence: in the head-in-tail case there is no guaranteed dead range to
reclaim, and the rewrite-excluding-head path does not apply (head is not in a
sealed range). The reachable bad state is: head rewritten into the tail + tail
full + at the range cap + all sealed ranges live -> a pop still needs a rotation
that returns `RangeLimitExceeded`. **The pop-via-compaction design does not yet
cover this; it must be reworked before implementation.**

Candidate directions to evaluate (not chosen):
- Treat the tail like a range for the exclude-head rewrite: rewrite the tail's
  live records excluding the head into a replacement segment. Needs to avoid
  growing the range count (the open hard part).
- Reconsider a location-independent pop (e.g. the Option A head pointer), which
  sidesteps "where is the head physically" entirely -- previously ruled out on
  the 64B manifest budget, but the budget tradeoff may be worth revisiting given
  this hole.

### Proof obligations (must be tested, or the design has a hole)

These claims are load-bearing; each needs an explicit test:

1. ~~Head-in-tail => all sealed ranges fully dead~~ -- DISPROVEN above; the design
   must not rely on it.
2. **Dead-range removal always reduces the manifest range count** when invoked on
   a fully-dead range (no live records), independent of fragmentation.
3. **Rewrite-excluding-head never increases range count** (prefix removal of the
   FIFO head: range count stays or drops, never grows).
4. **No path inside a `pop()` calls `rotateSegment()` in a way that can return
   `RangeLimitExceeded`** -- either the tombstone fits, or a slot is freed first.
   (Currently FAILS for the head-in-tail case above.)

A fuzz/property test should assert the top-level invariant directly, and must
include rewrite-front activity: for any reachable mounted state with count > 0
(including states produced by interleaved `rewriteFront` retries), `pop()` never
returns `RangeLimitExceeded`.

### Reuse vs new code

Ride on the existing, well-exercised compaction path; keep the new surface thin:

- `collectLiveRecords(range, out, excludeSequence)` -- add an optional excluded
  sequence so the head's payload is omitted from the rewrite.
- `compactRange(...)` -- when invoked for a pop, target the head's range
  specifically (bypass `chooseCompactionRange`) and bypass the no-op gates
  (`noOp(noDead)` / `hypoOutputSegs == hypoInputSegs && no dead`), since the
  reclaim here is the head removal itself, not dead bytes. The manifest splice,
  dead-range removal, `cleanupInputSegments`, and `records_` updates are reused
  unchanged.
- `commitPopByCompaction()` -- orchestrator: locate head range, run the excluded
  rewrite, then drop `records_.front()` on success.

### Crash consistency

Same ordering as compaction: write replacement segment(s) -> publish manifest ->
`cleanupInputSegments` + drop `records_.front()`. The commit point is the
manifest publish:

- crash before publish: old segment still referenced, head still live, pop NOT
  committed; safe to retry on remount.
- crash after publish: head's bytes no longer referenced, pop committed;
  `records_` is rebuilt from the manifest on mount.

Honour the existing tail-dependency guard (`activeTailAffectedGenerations_` /
`tailDepsContained`) so POP/REWRITE tombstones in the tail referencing the
rewritten range cannot resurrect records.

### Free space

The rewrite writes a new output (<= input size) before deleting the old, so peak
usage is old + new. If `minFreeBytes` blocks the write it fails as real I/O,
which the invariant permits. Net effect after cleanup frees the head's bytes.

### Robustness: the path is exercised by design

The rarely-hit-critical-path risk is removed structurally: because the compaction
commit runs on every tail-fill during any drain (not only at the cap), it is on
the common drain path and gets continuous coverage. Still pin it down with tests:

- The new surface is thin and rides on `compactRange`, which routine compaction
  also exercises constantly.
- Drive it in the compaction simulator (`tools/pqueue_compaction_sim.cpp`) --
  extend it to reach the fragmented-full-at-cap state it currently never hits
  (`Deadlocks: 0` today) and to exercise ordinary tail-fill pops.
- Pin a deterministic POSIX regression from the `~/pqdump` specimen: mount,
  assert the old behaviour wedged, assert pop drains it.
- POSIX transition tests: head-alone-in-range, head-in-partial-range,
  head-in-tail below the cap (plain rotate), head-in-tail at the cap with dead
  sealed ranges (reclaim-then-rotate), and crash-before/after-publish
  interleavings.

### Secondary fix to land with this

`needsCompaction()` (`compaction.cpp:657-664`) ignores range-cap pressure, so
`commitEnqueue` can hit `RangeLimitExceeded` without trying compaction. Add a
range-cap-pressure condition so enqueue compacts when near the cap.

### Benchmark plan (anchor in real metrics, not the reference table)

The prior perf reasoning (pop already does ~24ms `writeAt`; manifest publish
~25-35ms; drain rate-capped at 20/s) is from `internals.md` and config, NOT
measured for this decision. Measure on-device before drawing conclusions:

- normal tombstone-pop latency today (baseline),
- pop-via-compaction latency at a tail-fill boundary vs baseline,
- drain throughput under the 20/s rate cap (the compaction commit lands roughly
  once per tail-capacity worth of pops; confirm the amortised cost stays within
  the rate-cap budget),
- how that frequency shifts under a realistic fragmented workload.

If the amortised cost proves too high, the trigger can be revisited, but the
default is: compaction commit on every would-rotate pop.

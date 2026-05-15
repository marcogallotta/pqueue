# pqueue append-log working notes

**Status of this document:** living working notes for the pqueue append-log backend. It is a code-reading accelerator, not a substitute for inspecting the current code.

**Current project phase:** Stage 2 is done and Stage 3 is starting. Stage 2 means the compaction journal record format, logical active segment ordering, and journal-aware remount/replay are wired into `AppendLogStore::scanSegments()`. Stage 3 is planned work around `compactRange(oldStart, oldEnd)`: copy live records out of an old segment range into new compacted segments, durably commit the replacement by appending a compaction journal record, update RAM state, and leave old segment deletion for a later cleanup stage.

**Important rule for future assistants/developers:** before giving implementation advice, inspect the relevant current files and tests. Do not infer behavior from filenames, this document, or generic append-log/storage-engine knowledge alone. The code is the source of truth.

**This review inspected code and tests but did not execute the POSIX tests.** The extracted tar did not include `doctest/doctest.h`, and `make test` fails at compile time because the Makefile expects doctest under `../third_party/doctest` by default. If tests need to be run from a similar tar, fetch doctest's single header from `https://github.com/doctest/doctest/blob/master/doctest/doctest.h` and place it so `#include "doctest/doctest.h"` is found, or set `DOCTEST_INCLUDE` appropriately.

---

## 0. How to use this document

Read this document first to get the mental model, then inspect the current implementation before making changes.

Start with these files:

- `src/pqueue/append_log_common.h`
- `src/pqueue/append_log_common.cpp`
- `src/pqueue/append_log_store.h`
- `src/pqueue/append_log_store.cpp`

Then inspect only the supporting integration points needed for the question:

- `src/pqueue/queue.h`
- `src/pqueue/queue.cpp`
- `src/pqueue/file_store.h`
- `src/pqueue/file_store.cpp`
- `src/pqueue/types.h`
- `src/pqueue/file_system.h`
- `src/pqueue/storage_posix.cpp`
- `src/pqueue/storage_littlefs.cpp`
- `src/pqueue/storage_common.*` for fixed-slot context

Primary tests to inspect:

- `tests/posix/pqueue_append_log.cpp`
- `tests/posix/pqueue_compact_journal.cpp`
- `tests/posix/pqueue_file_store.cpp`
- `tests/posix/pqueue_rebuild_metadata.cpp`
- `tests/posix/pqueue_repair.cpp`
- `tests/support/pqueue_file_store_support.h`

The append-log backend is the current design focus. The fixed-slot backend is useful mostly as context because the current `Store` interface is still fixed-slot-shaped.

---

## 1. What pqueue is

`pqueue` is a durable FIFO queue for small opaque records, represented as `std::string` at the current public API layer.

The main public queue operations are in `Queue`:

- `enqueue(record)`
- `peek(out)`
- `pop()`
- `rewriteFront(record)`
- `format()`
- `dropFrontIfCorrupt()`
- `recoverStaleLock()`
- `rebuildMetadata()`
- `validate()`
- `statsResult()` / `stats()`

`Queue` owns the public API, locking, and the logical queue index. It delegates persistent storage behavior to a backend implementing the abstract `Store` interface in `file_store.h`.

The two storage layouts are:

- `StoreLayout::FixedSlot` — old/default fixed-slot backend implemented by `FileStore`.
- `StoreLayout::AppendLog` — newer append-only segment backend implemented by `AppendLogStore`.

The runtime selection happens in `queue.cpp` inside `makeStore(const Config&)`:

- For `AppendLog`, `Config` is copied into an `AppendLogConfig`.
- `recordSizeBytes` becomes append-log `maxRecordBytes`.
- `reservedBytes` becomes append-log `maxTotalBytes`.
- `maxSegmentBytes`, `minFreeBytes`, and `maxSegments` are append-log-specific settings.

The queue-level `FileStoreIndex` has:

- `head`
- `tail`
- `count`

The queue assumes sequences are monotonically increasing. `head` is the front sequence, `tail` is the next enqueue sequence, and `count == tail - head` for a healthy loaded queue.

---

## 2. Existing fixed-slot backend

The fixed-slot backend is implemented by `FileStore` in `file_store.*` and the shared binary format helpers in `storage_common.*`.

It stores everything in one file named:

```text
pqueue.spool
```

The spool layout is:

```text
checkpoint slots | journal region | fixed-size record slots
```

Important fixed-slot constants in `storage_common.h`:

- checkpoint magic: `PQCK`
- record magic: `PQRC`
- journal magic: `PQJN`
- format version: `0`
- checkpoint slots: `4`
- checkpoint record bytes: `64`
- journal entry bytes: `32`
- record header bytes: `20`

A fixed-slot record is stored in a slot chosen by:

```text
sequence % capacityRecords
```

A record slot contains:

```text
RecordHeader(20 bytes) + payload + zero padding to slot size
```

The fixed-slot backend has separate data and metadata commit behavior:

1. `FileStore::writeRecord(sequence, record)` writes the record bytes into the fixed slot.
2. `FileStore::writeIndex(nextIndex)` commits the logical queue index transition, either by appending a journal entry or writing a checkpoint.

This is the model the current `Store` interface reflects.

Fixed-slot metadata loading uses `loadStoredState()`:

1. Read checkpoint + journal metadata region.
2. Pick the highest valid checkpoint generation.
3. Replay valid consecutive journal entries after that checkpoint.
4. Stop at the first invalid/non-contiguous journal entry.

The fixed-slot journal supports:

- enqueue
- pop
- rewrite front

This backend matters for append-log because the shared `Store` interface still has fixed-slot-shaped methods:

- `writeRecord()`
- `writeIndex()`
- `removeRecord()`

For append-log, that split is awkward because an ENQUEUE event is itself the durable commit.

---

## 3. Why append-log exists

The append-log backend exists because the fixed-slot backend's write pattern is expensive on ESP32 + LittleFS.

The fixed-slot backend uses a large spool file and writes at positions inside that file. Prior measurements from the design work showed that LittleFS flush cost grows sharply with file size. Approximate observed flush behavior:

```text
512 B file flush:   ~13 ms
4 KB file flush:    ~45 ms
8 KB+ file flush:   ~70 ms+
```

Fixed-slot enqueue of about 512 B records was observed around 300–400 ms on ESP32-S3 + LittleFS. The major cost was LittleFS open/write/flush behavior, especially flushing larger files.

Append-log changes the write pattern:

- use multiple small segment files rather than one large spool file;
- append framed events rather than rewriting fixed slots;
- rotate segments when they reach `maxSegmentBytes`;
- keep the active segment small, with the current default around 4 KB.

This makes `maxSegmentBytes` performance-critical, not just a storage tuning knob.

---

## 4. Append-log file format

Append-log binary format lives in:

- `append_log_common.h`
- `append_log_common.cpp`

All multi-byte fields are serialized little-endian by local helper functions.

### Files

Append-log stores segment files named:

```text
pqueue-seg-XXXXXXXX.bin
```

where `XXXXXXXX` is an 8-digit lowercase hexadecimal generation, for example:

```text
pqueue-seg-00000001.bin
pqueue-seg-00000002.bin
pqueue-seg-0000000a.bin
```

The persistent compaction journal file is:

```text
pqueue-compact.bin
```

### Magic values

The append-log constants are:

```text
PQSG  segment header
PQEQ  enqueue event
PQPE  pop event
PQRE  rewrite event
POK!  event/footer magic
PQCJ  compaction journal record
```

Current append-log format version is `0`.

### Segment header

Each segment starts with a fixed 20-byte `SegmentHeader`:

```text
magic          u32  PQSG
version        u16  0
headerBytes    u16  20
generation     u32
baseSequence   u32
headerCrc      u32
```

The segment header CRC covers the first 16 bytes of the header. `parseSegmentHeader()` validates magic, version, header size, and CRC.

`scanSegments()` separately checks that the parsed header generation equals the generation implied by the filename.

`baseSequence` is currently informational in the code path reviewed. It is written by `serializeSegmentHeader(generation, baseSequence)` and parsed/CRC-checked, but replay does not currently enforce sequence ordering from it.

### ENQUEUE and REWRITE events

ENQUEUE and REWRITE share the same fixed 16-byte prefix type, `EnqueueHeader`:

```text
magic          u32  PQEQ or PQRE
version        u16  0
headerBytes    u16  16
sequence       u32
payloadBytes   u32
payload         payloadBytes bytes
crc            u32
footer         u32  POK!
```

The fixed overhead beyond payload is:

```text
kEnqueueOverheadBytes = 16 + 8 = 24 bytes
```

The CRC covers:

```text
magic/version/headerBytes/sequence/payloadBytes + payload
```

A REWRITE event uses magic `PQRE`, but otherwise serializes the same shape as ENQUEUE.

### POP events

POP is a fixed 20-byte event:

```text
magic          u32  PQPE
version        u16  0
headerBytes    u16  20
sequence       u32
eventCrc       u32
footer         u32  POK!
```

The POP CRC covers the fixed prefix before the CRC field.

### Compaction journal records

A compaction journal record is 36 bytes:

```text
magic          u32  PQCJ
version        u16  0
headerBytes    u16  36
commitSeq      u32
oldStart       u32
oldEnd         u32
newStart       u32
newEnd         u32
crc            u32
footer         u32  POK!
```

Meaning:

```text
oldStart..oldEnd was replaced by newStart..newEnd
```

Examples:

```text
1..4  -> 10..11
10..11 -> 20..21
```

`serializeCompactionJournalRecord()` forces magic/version/headerBytes/CRC/footer from constants. It does not trust those fields from the input struct.

`parseCompactionJournalRecord()` rejects:

- buffer shorter than 36 bytes;
- wrong magic;
- wrong version;
- wrong header size;
- wrong footer;
- bad CRC;
- `oldStart == 0`;
- `newStart == 0`;
- `oldStart > oldEnd`;
- `newStart > newEnd`.

`commitSeq == 0` is currently accepted by tests. `commitSeq` is parsed and serialized, but the current logical-order helper and `scanSegments()` use journal file order, not `commitSeq`, as the replacement order.

---

## 5. Append-log mount/replay

Append-log mount is implemented by:

```text
AppendLogStore::mount()
  -> fs()->mount(config_.basePath)
  -> scanSegments()
```

The important function is:

```text
AppendLogStore::scanSegments()
```

This function is the normal reboot/remount recovery path. It reconstructs RAM state from segment files and the compaction journal.

### Step 1: list segment files

`scanSegments()` calls `FileSystem::listFiles()` and collects filenames matching:

```text
pqueue-seg-XXXXXXXX.bin
```

The filename parser requires exactly eight hex digits. Generations are sorted numerically into `sortedGenerations`.

### Step 2: read compaction journal if present

`pqueue-compact.bin` presence is determined from the same `listFiles()` result. This is deliberate: if the journal is listed but cannot be read, mount fails instead of silently treating it as absent.

If present:

1. Read the whole journal file with `readFile()`.
2. Compute complete record count using truncating division by 36.
3. Parse each complete record in order.
4. Ignore any partial trailing bytes after the final complete record.
5. Treat any complete malformed record as `DataCorrupt`.

Important policy:

```text
partial final journal record -> ignored as torn tail
bad complete journal record  -> DataCorrupt
```

The current parser does not have a special “bad middle” branch; it simply parses all complete records. Therefore a bad complete record anywhere in the complete-record prefix fails the mount.

### Step 3: build logical segment order

`scanSegments()` calls:

```text
AppendLogStore::buildActiveSegmentOrder(sortedGenerations, replacements, logicalOrder)
```

This is the Stage 2A pure helper wired into Stage 2B mount/replay.

No journal case:

- segment generations must be exactly consecutive from `1..N`;
- `found = {1,2,3}` succeeds;
- `found = {1,3}` fails `DataCorrupt`;
- `found = {5}` fails `DataCorrupt`.

Journal case:

1. Collect every generation that appears in any replacement new range.
2. Build a base set from found generations not in any new range.
3. Add old ranges from journal records, except old generations that were themselves produced by a prior replacement new range. This supports chained replacements.
4. Sort the base set and require it to form a consecutive base chain from generation 1.
5. Apply each replacement in journal order by finding the old range as a contiguous subsequence and replacing it with the new range.
6. Reject duplicate generations in the final active chain.
7. Require every generation in the final active chain to exist on disk.

This means physical generation order and logical replay order are not necessarily the same.

Examples from tests:

```text
No journal:
found: 1,2,3
out:   1,2,3

No journal gap:
found: 1,3
DataCorrupt

Simple journal, old files still present:
found:   1,2,3,10,11
journal: 1..2 -> 10..11
out:     10,11,3

Simple journal, old files cleaned:
found:   3,10,11
journal: 1..2 -> 10..11
out:     10,11,3

Chained compaction:
found:   4,20,21
journal: 1..3 -> 10..11
         10..11 -> 20..21
out:     20,21,4

Non-monotonic logical order:
found:   3,4,5,6
journal: 1..2 -> 5..6
out:     5,6,3,4
```

The helper rejects overly large individual ranges using a hard-coded range bound of 4096 generations. The comment says this may later be tied to `config.maxSegments`.

### Step 4: reset RAM state before replay

Before scanning segment contents, `scanSegments()` resets:

- `records_.clear()`
- `activeGeneration_ = 0`
- `activeSegmentBytes_ = kSegmentHeaderBytes`
- `nextGeneration_ = 1`
- `nextSequence_ = 0`

Then it sets:

```text
nextGeneration_ = max(all disk generations) + 1
```

This uses all segment files on disk, including old compacted-away segments that are not in the logical active chain. This prevents reusing a generation number that still exists on disk.

A regression test covers this: with disk `{1,2,3,10,11}` and journal `1..2 -> 10..11`, the next generation after remount and rotation must be `12`, not `4`.

### Step 5: replay segments in logical order

For each generation in `logicalOrder`:

1. Determine whether it is the logical last segment by position in `logicalOrder`, not by numeric generation.
2. Read file size and require it to be at least the 20-byte segment header.
3. Read and parse the segment header.
4. Require header generation to match filename generation.
5. Scan events from offset 20.

This “logical last” distinction is crucial after compaction. In a chain like:

```text
5,6,3,4
```

generation 4 is the active/logically last segment even though it is not numerically largest, and generation 6 is not logically last even though it is numerically larger.

### Step 6: event replay rules

For ENQUEUE:

- parse header;
- reject payload sizes above `config_.maxRecordBytes`;
- require full payload + CRC + footer;
- verify CRC and footer;
- push a `SegmentRecord` into `records_` with sequence, generation, payload offset, and payload size;
- update `nextSequence_` to at least `sequence + 1`.

For REWRITE:

- parse and validate the same frame shape as ENQUEUE;
- find an existing live `SegmentRecord` with the same sequence;
- update that live record's generation, payload offset, and payload size;
- update `nextSequence_` to at least `sequence + 1`.

Current implementation detail: if a REWRITE sequence is not found in `records_`, the replay loop does not currently report an error; it just does not update any record.

For POP:

- parse the fixed POP event;
- if `records_` is non-empty and the front sequence matches the POP sequence, pop the front;
- update `nextSequence_` to at least `sequence + 1`.

Current implementation detail: if a POP sequence does not match the current front, replay does not currently report an error; it simply does not pop anything.

For unknown magic:

- mark corruption.

### Step 7: torn-tail and corruption policy

The scan loop maintains:

- `offset`
- `lastGoodOffset`
- `corrupt`

After a valid event, `lastGoodOffset` moves to the end of that event.

If a partial event, invalid payload size, bad CRC/footer, invalid POP, or unknown magic appears:

- in a non-last logical segment: mount fails with `DataCorrupt`;
- in the logical last segment: the tail from `lastGoodOffset` onward is discarded by truncating the file, and mount continues.

Important nuance: if a corrupt event appears in the last segment and a later valid-looking event follows it, the later event is also discarded. The code truncates from the last known-good offset because bytes after a corrupt event are not trusted.

Tests cover:

- torn tail in active segment is recoverable;
- corrupt CRC at tail of last segment is recoverable;
- corrupt payloadBytes at tail of last segment is recoverable without allocating huge memory;
- corrupt middle event in the last segment discards that event and all following events;
- corruption in non-last segments is `DataCorrupt`;
- torn tail classification uses logical order, not numeric generation order.

### Step 8: empty store

If there are no segment files and no journal-backed required segments, the append-log store mounts empty:

- `activeGeneration_ = 0`
- `activeSegmentBytes_ = 0`
- `nextGeneration_ = 1`
- `nextSequence_ = 0`

---

## 6. Runtime RAM model

`AppendLogStore` holds the current live queue state in RAM.

Key fields:

```text
activeGeneration_      currently active write segment generation
activeSegmentBytes_    current append offset / size of active segment
nextGeneration_        next generation number to use when creating a segment
records_               deque of live SegmentRecord entries in FIFO order
nextSequence_          one past highest sequence seen/replayed; preserves empty-queue sequence after remount
hasPendingEnqueue_     bridge state for current Store API split
pendingRecord_         RAM pointer for just-written ENQUEUE before writeIndex()
```

Each live `SegmentRecord` contains:

```text
sequence
segmentGeneration
payloadOffset
payloadBytes
```

`indexFromRecords()` turns RAM state into the queue-level `FileStoreIndex`:

- if `records_` is empty: `head = tail = nextSequence_`, `count = 0`;
- otherwise: `head = records_.front().sequence`, `tail = records_.back().sequence + 1`, `count = records_.size()`.

This means append-log's durable state is the event log, while the queue index is reconstructed from `records_`.

`readRecord(sequence, out)` currently assumes the live sequence range is contiguous. It checks:

```text
sequence >= records_.front().sequence
sequence < records_.front().sequence + records_.size()
```

Then it indexes:

```text
records_[sequence - head]
```

That makes contiguous live sequence ordering an important current invariant/assumption for append-log replay and operations.

---

## 7. Segment sizing and LittleFS performance

Current append-log config defaults in `types.h` / `AppendLogConfig`:

```text
maxSegmentBytes = 4096
minFreeBytes    = 32 * 1024
maxSegments     = 16
maxRecordBytes  = Config.recordSizeBytes
maxTotalBytes   = Config.reservedBytes
```

The current public `Config.recordSizeBytes` default is `492`, but append-log tests often set it to `256`. `AppendLogConfig` itself defaults `maxRecordBytes` to `4096`, but Queue construction passes `Config.recordSizeBytes` into it.

Segment rotation happens when appending the next event would exceed `maxSegmentBytes` and the current segment already has at least one event:

```text
activeSegmentBytes_ > kSegmentHeaderBytes &&
activeSegmentBytes_ + nextEventBytes > maxSegmentBytes
```

Because of the `activeSegmentBytes_ > kSegmentHeaderBytes` guard, the first event in a segment may exceed `maxSegmentBytes` if the record is allowed by `maxRecordBytes`. In other words, `maxSegmentBytes` is a rotation target, not an absolute hard cap on a segment containing one large allowed record.

`createSegment()` writes only the segment header with `writeFile()`. Event appends use `writeAt()` at `activeSegmentBytes_`.

On Arduino/LittleFS:

- `writeFile()` opens with `"w"`, writes, flushes, and closes.
- `writeAt()` opens with `"r+"`, seeks, writes, flushes, and closes.
- `resizeFile()` writes zero chunks or recreates the file when shrinking.
- `freeBytes()` uses `LittleFS.totalBytes() - LittleFS.usedBytes()`.

On POSIX:

- `writeFile()` uses `std::ofstream` with truncate.
- `writeAt()` uses `std::fstream` positioned write.
- there is no explicit `fsync()` in the POSIX implementation reviewed.

The LittleFS performance motivation is tied to keeping active segment files small, because every event append currently flushes the file.

`AppendLogStore::canEnqueue()` currently checks:

```text
freeBytes() >= minFreeBytes + recordSize + kEnqueueOverheadBytes
```

It does not currently enforce `maxTotalBytes` directly.

`needsCompaction()` currently returns true if:

- the numeric span from the front live record's segment generation to `activeGeneration_` exceeds `maxSegments`; or
- free bytes are below `minFreeBytes`.

At the current Stage 2 / start of Stage 3 state, non-empty compaction is not implemented yet, so `compact()` returns success without changing anything when `records_` is non-empty. This is current active development state, not a finished backend behavior.

---

## 8. Operation semantics

This section follows real Queue -> Store call paths.

### Locking

Every public mutating/reading queue operation uses `Queue::ScopedLock`, which calls `Queue::acquireLock()` and releases in the destructor.

The lock filename is:

```text
.pqueue.lock
```

Lock contents include:

```text
pqueue-lock-v2
owner=queue
pid=...
boot_id=...
token=...
```

On POSIX, `PosixFileLock` uses `open(..., O_CREAT | O_EXCL)` and removes stale locks only if the PID is no longer alive.

In tests using `FakeFileSystem`, stale lock recovery is based on different boot ID.

On ESP32/LittleFS, the current implementation uses a named FreeRTOS mutex keyed by base path + lock name. `recoverStale()` is a success/no-op there because it is an in-process mutex, not a persistent lock file.

### enqueue(record)

`Queue::enqueue()`:

1. Acquire lock.
2. `loadLatestIndex()` using `store_->readIndexFromDisk(index_)`.
3. Reject if `record.size() > config_.recordSizeBytes`.
4. Call `store_->canEnqueue(record.size(), index_.count)`.
5. If full and `DropOldest`, call `evictFront()` first; otherwise return `QueueFull`.
6. Use `sequence = index_.tail`.
7. Call `store_->writeRecord(sequence, record)`.
8. Build next index with `tail + 1`, `count + 1`.
9. Call `store_->writeIndex(next)`.
10. Update cached `index_`.

Append-log-specific path:

`AppendLogStore::writeRecord()`:

1. Ensure mounted.
2. Reject records over `maxRecordBytes`.
3. Compute ENQUEUE event size.
4. If event would overflow the active segment and the active segment has data:
   - if `needsCompaction()`, call `compact()`;
   - if still too large, rotate segment.
5. Ensure there is an active segment.
6. Compute payload offset.
7. Serialize and append ENQUEUE event.
8. Store `pendingRecord_` and set `hasPendingEnqueue_ = true`.

`AppendLogStore::writeIndex(next)` then sees `hasPendingEnqueue_` and pushes `pendingRecord_` into `records_` without writing another durable commit record.

Important commit-semantic mismatch:

- fixed-slot: `writeRecord()` writes data, `writeIndex()` commits the logical enqueue;
- append-log: writing the ENQUEUE event is already the durable logical enqueue.

This mismatch exists because the shared `Store` interface is still shaped around the fixed-slot backend.

### peek(out)

`Queue::peek()`:

1. Acquire lock.
2. Load latest index from store.
3. If count is zero, return `QueueEmpty`.
4. Read `index_.head` via `store_->readRecord()`.

Append-log `readRecord()`:

1. Ensure mounted.
2. Require non-empty `records_`.
3. Check sequence is inside the contiguous live range.
4. Compute ordinal as `sequence - head`.
5. Read payload bytes from the stored segment generation and payload offset.

### pop()

`Queue::pop()`:

1. Acquire lock.
2. Load latest index.
3. If count is zero, return `QueueEmpty`.
4. Build next index with `head + 1`, `count - 1`.
5. Call `store_->writeIndex(next)`.
6. Update cached `index_`.
7. Call `store_->removeRecord(oldHead)`.

Append-log `writeIndex(next)` detects a pop when `index.head > current.head` and `records_` is non-empty:

1. It takes `poppedSeq = records_.front().sequence`.
2. Appends a POP event for that sequence.
3. Pops the front RAM record.

Append-log `removeRecord()` is a no-op because the POP event is written by `writeIndex()`.

### rewriteFront(record)

`Queue::rewriteFront()`:

1. Acquire lock.
2. Load latest index.
3. Require non-empty queue.
4. Reject if record is larger than `config_.recordSizeBytes`.
5. Call `store_->rewriteRecord(index_.head, record)`.

Append-log `rewriteRecord()` calls `appendRewriteEvent(sequence, record)`.

`appendRewriteEvent()`:

1. Rotates if the REWRITE event would overflow a non-empty active segment.
2. Ensures active segment.
3. Computes payload offset.
4. Serializes and writes a REWRITE event.
5. Updates the matching live `SegmentRecord` in RAM.

There is no queue index change for rewrite. The rewrite is durable because the REWRITE event is in the log.

Current implementation detail: `appendRewriteEvent()` does not call `needsCompaction()` before rotating; it only rotates if needed.

### format()

`Queue::format()` acquires the lock, calls `store_->format()`, then resets cached `index_`.

Append-log `format()`:

1. Mounts the filesystem.
2. Lists files.
3. Removes all segment files matching append-log segment names.
4. Removes `pqueue-compact.bin`.
5. Clears RAM state.
6. Sets `mounted_ = true`.

Tests specifically cover that `format()` removes the compaction journal and that a remount after format ignores stale journal artifacts.

### rebuildMetadata()

For append-log, `rebuildMetadata()` is simply a full re-scan:

```text
records_.clear();
hasPendingEnqueue_ = false;
mounted_ = false;
return mount();
```

This differs from fixed-slot `rebuildMetadata()`, which scans fixed slots and reconstructs checkpoint/journal metadata.

### validate()

`Queue::validate()`:

1. Acquires lock.
2. Attempts to load latest index.
3. Calls `store_->validateUnlocked()`.
4. Adds repair hints based on issue type and loaded head.
5. Compares cached and disk indexes.

Append-log `validateUnlocked()` currently scans physical segment files in sorted numeric order and validates segment headers plus event framing/CRC/footer. It does not currently use the compaction journal or the logical active segment ordering path used by `scanSegments()`.

That distinction matters for future validation work: mount/replay and validate are not currently equivalent for journal-backed layouts.

---

## 9. Current implementation state / active development boundaries

This section describes the current code state at the start of Stage 3. These are not “surprising defects”; they are boundaries of the active append-log development pipeline.

### Implemented now

Implemented in current code:

- append-log segment file naming;
- segment header serialization/parsing/CRC;
- ENQUEUE/REWRITE/POP event serialization/parsing/CRC/footer;
- append-log mount/replay from segment files;
- tail truncation recovery on the logical last segment;
- `Queue` integration through the existing `Store` interface;
- segment rotation;
- in-RAM live record map via `records_`;
- compaction journal record serialization/parsing;
- pure logical active segment ordering helper;
- journal-aware mount/replay using logical order;
- format removes segments and compaction journal;
- new generation allocation skips every segment generation on disk, including old compacted-away generations.

### Not implemented yet in this tar

Not implemented yet in current code:

- `compactRange(oldStart, oldEnd)` function;
- writing new compacted segment files for non-empty queues;
- appending compaction journal records from production compaction code;
- updating RAM pointers after a committed compaction;
- deletion of old compacted-away segments;
- cleanup of uncommitted compacted leftovers;
- crash/fault injection tests for actual compaction writer stages;
- append-log validation that fully mirrors journal-aware mount/replay;
- high-level Store API refactor.

`AppendLogStore::compact()` currently does real cleanup only for an empty queue. For a non-empty queue it returns success without changing files. The comment says non-empty compaction is crash-unsafe until there is a compaction commit marker or dedup recovery. Stage 3 is the work that replaces that placeholder with crash-safe compacted segment writing and journal commit.

### Current assumptions to keep visible

Current code assumes or currently behaves as follows:

- Live queue sequences are contiguous for `readRecord()` ordinal lookup.
- `baseSequence` in segment headers is informational during replay.
- `commitSeq` in compaction journal records is parsed but not currently used for ordering.
- REWRITE of a missing live sequence is ignored during replay.
- POP of a non-front sequence is ignored during replay.
- `maxTotalBytes` exists in `AppendLogConfig` but is not directly enforced by append-log admission.
- `maxSegmentBytes` is not a hard cap for a single large first event in a segment.
- POSIX write paths do not explicitly `fsync()` in the reviewed implementation.
- LittleFS lock recovery is mutex-based/no-op rather than persistent stale lock-file recovery.

Future design work may intentionally change any of these, but do not assume they have already been handled.

---

## 10. Compaction problem

Append-log grows because queue operations are append-only:

- ENQUEUE writes a payload.
- POP removes logical visibility but does not remove old bytes.
- REWRITE changes the current payload pointer but leaves old payload bytes behind.

Without compaction, old segments accumulate obsolete data and stale events.

Compaction must reclaim space by copying only live records from an old range of segments into new compacted segment files.

The key safety problem:

```text
Never delete old segments until the replacement is durably committed.
```

If new segments are written but no commit record exists, remount must use the old segments.

If the commit record exists, remount must use the new segments in place of the old range.

This is why the design uses a persistent compaction journal instead of simply writing replacement files and deleting old ones.

The target Stage 3 unit of work is described as:

```text
compactRange(oldStart, oldEnd)
```

In current code this exact function does not yet exist. It is the roadmap term for the next implementation step.

Stage 3 intent:

1. Choose an old active logical segment range to compact.
2. Collect live records whose current payload location is in that old range.
3. Write those live records into one or more new segment files, preserving FIFO/sequence semantics.
4. Flush/verify the new segment files.
5. Append a compaction journal record mapping `oldStart..oldEnd -> newStart..newEnd`.
6. Only after the journal commit succeeds, update RAM state to point those live records at the new segment payload offsets.
7. Do not delete old segments in Stage 3.

Deletion and broader cleanup are Stage 4.

---

## 11. Compaction journal design

The chosen design is a persistent append-only compaction journal, not a full active-segment manifest.

Reason from the design discussion:

- A full active manifest would need updates during normal segment rotation, adding metadata writes to the hot path.
- The compaction journal is only touched during compaction.
- Normal enqueue/pop/rewrite operations should not pay extra manifest-update cost.

The journal file is:

```text
pqueue-compact.bin
```

Each record says:

```text
oldStart..oldEnd -> newStart..newEnd
```

Meaning:

- before the journal record is committed, old segments are authoritative;
- after the journal record is committed, new segments are authoritative in the logical position where the old range used to be.

Example:

```text
base chain: 1,2,3,4,5
journal:    1..4 -> 10..11
scan order: 10,11,5
```

Old segments may still exist after commit. They must not be replayed if they are replaced by the journal.

Old segments may also have been cleaned up later. Mount must still work if the committed new segments exist and the reconstructed logical chain is complete.

Chained compaction is supported by applying journal records in order:

```text
journal:
  1..3   -> 10..11
  10..11 -> 20..21

found: 4,20,21
out:   20,21,4
```

The logical active segment order can be non-monotonic:

```text
journal: 1..2 -> 5..6
found:   3,4,5,6
out:     5,6,3,4
```

This is why recovery and torn-tail handling must use logical order, not numeric generation order.

---

## 12. Crash-safety invariants

These are the invariants the current design is moving toward. Some are already implemented by Stage 2; some are Stage 3+ requirements.

### Already implemented or directly tested in Stage 2

1. **No journal means strict consecutive generations from 1.**
   A gap such as `{1,3}` is `DataCorrupt`.

2. **Journal-backed layouts reconstruct a logical chain.**
   The active scan order is built from found segment files plus committed replacement records.

3. **Old compacted segments left on disk are ignored.**
   If journal says `1..2 -> 10..11`, then gens 1 and 2 must not be replayed even if they still exist.

4. **Committed new segments must exist.**
   If the journal's final active chain includes a generation missing from disk, mount fails `DataCorrupt`.

5. **Partial trailing journal bytes are ignored.**
   A torn final journal record that is shorter than 36 bytes is ignored.

6. **Bad complete journal records fail closed.**
   A complete record with bad CRC/magic/footer/etc. is `DataCorrupt`.

7. **Torn tail recovery applies only to the logical last segment.**
   This is based on `logicalOrder`, not numeric generation.

8. **New generation allocation skips every segment generation found on disk.**
   Even old compacted-away segments keep their generation numbers reserved while they remain on disk.

### Stage 3+ required invariants

1. **New compacted segments must be durable before the journal commit.**
   A crash before journal append should leave mount using old segments.

2. **The journal append is the compaction commit point.**
   After the journal record is durable, remount must use the new segment range in place of the old range.

3. **Old segments must not be deleted during Stage 3.**
   Deletion belongs to Stage 4 cleanup, after the commit model is proven.

4. **RAM state must not claim compacted locations unless commit succeeds.**
   If writing/verifying new segments fails, live records should still point to old segment payload locations.

5. **Compaction must preserve FIFO order and sequence identity.**
   It must not reorder live records, resurrect popped records, or lose rewriteFront results.

6. **Cleanup must be idempotent.**
   Deleting old segments after commit should be safe if interrupted and retried.

7. **Default to `DataCorrupt` unless recovery is clearly safe.**
   Normal mount should avoid aggressive salvage. More aggressive salvage can belong to explicit repair tooling later.

---

## 13. Stage roadmap

### Stage 1 — compaction journal record format

Done according to the design context and tests.

Implemented/tested:

- `CompactionJournalRecord` struct;
- serializer;
- parser;
- fixed 36-byte size;
- CRC/footer validation;
- rejection of invalid ranges;
- acceptance of single-segment ranges;
- acceptance of `commitSeq == 0` and `UINT32_MAX` round-trip.

Primary test file:

```text
tests/posix/pqueue_compact_journal.cpp
```

### Stage 2A — pure logical ordering helper

Done in current code.

Function:

```text
AppendLogStore::buildActiveSegmentOrder(sortedGenerations, replacements, out)
```

Purpose:

Given segment generations found on disk and parsed journal records in journal order, compute the logical active segment generations to scan.

No filesystem. No journal reading. No segment scanning. No compaction writer.

Primary tests are in `pqueue_append_log.cpp` under `buildActiveSegmentOrder` test cases.

### Stage 2B — wire ordering into remount/recovery

Done in current code.

`scanSegments()` now:

- reads `pqueue-compact.bin` if present;
- parses complete journal records;
- ignores partial trailing journal bytes;
- fails on bad complete journal records;
- calls `buildActiveSegmentOrder()`;
- scans segment generations in logical order;
- uses logical last segment for torn-tail policy.

Primary tests are in `pqueue_append_log.cpp` under journal-aware mount/replay integration tests.

### Stage 3 — compactRange writer and commit

Current next focus.

Target behavior:

- collect live records from `oldStart..oldEnd`;
- write new compacted segment files;
- flush and verify new segments;
- append compaction journal record;
- update RAM state after successful commit;
- leave old segments on disk.

Current code does not yet have `compactRange(oldStart, oldEnd)`.

### Stage 4 — cleanup/idempotent deletion

Planned after Stage 3.

Target behavior:

- delete old segments superseded by committed journal records;
- ignore/delete uncommitted compacted leftovers if distinguishable;
- make cleanup safe to interrupt and retry;
- preserve mount behavior whether old segments have been cleaned or not.

### Stage 5 — crash/fault tests

Planned after writer and cleanup logic.

Important scenarios from the design discussion:

- crash before journal commit keeps old data;
- crash after journal commit uses new data;
- truncated final journal record ignored;
- bad middle/complete journal record gives `DataCorrupt`;
- missing committed new segment gives `DataCorrupt`;
- old segments left after commit are ignored;
- old segments deleted after commit are OK;
- compaction preserves FIFO order;
- compaction preserves `rewriteFront()` result;
- compaction after POP does not resurrect popped records;
- randomized model test against `std::deque` later.

---

## 14. Testing strategy

Current append-log tests are mostly in:

```text
tests/posix/pqueue_append_log.cpp
```

They cover several categories.

### Basic append-log queue behavior

Covered behaviors:

- empty queue starts empty;
- enqueue + peek;
- FIFO order;
- persistence across remount;
- pop persists across remount;
- segment rotation;
- segment rotation persists;
- rewriteFront persists;
- format clears records;
- format then enqueue works;
- validate returns OK for clean/empty append-log stores;
- mixed enqueue/pop persistence.

### Current compaction trigger placeholder

There is a test named:

```text
append-log: compaction triggered by segment count
```

At the current code state, this verifies that enqueue/read behavior still works when enough records are written to cross the current `maxSegments` trigger path. It does not prove real non-empty compaction because non-empty `compact()` currently returns success without rewriting segments.

Future Stage 3 tests should become stricter and assert actual compacted segment/journal behavior.

### Recovery policy tests

Tests cover:

- corrupt payloadBytes at tail of last segment is recoverable;
- corrupt payloadBytes in non-last segment fails mount;
- torn tail on active segment is recoverable;
- corrupt CRC at tail of last segment is recoverable;
- corrupt CRC in non-last segment fails;
- corrupt magic in non-last segment fails;
- corrupt middle event in last segment discards tail from last good offset;
- missing segment causes mount failure.

These tests define the current fail-closed policy for normal remount.

### Logical segment ordering unit tests

`buildActiveSegmentOrder()` tests cover:

- no journal consecutive success;
- no journal gap failure;
- old segments still present after replacement;
- old segments cleaned after replacement;
- old range compacted into fewer new segments;
- chained compactions;
- missing new range segment failure;
- old range not found failure;
- oversized range failure;
- duplicate final chain failure.

### Journal-aware mount/replay tests

Tests cover:

- journal-backed layout replays in logical order;
- old compacted segments on disk are not replayed;
- corrupt journal record causes mount failure;
- truncated final journal record is ignored;
- journal with missing active generation fails;
- torn tail on logically last segment recovers;
- torn tail on logically non-last segment fails;
- no journal with first generation not 1 fails;
- non-monotonic order `[5,6,3,4]` works;
- non-monotonic torn-tail classification is logical, not numeric;
- multi-record journal is applied in order;
- no-journal physical gap fails;
- empty store mounts;
- journal present but no segments fails;
- non-consecutive base chain fails;
- format removes compaction journal;
- bad middle complete journal record fails;
- next generation skips all disk generations, including compacted-away old gens.

### Fixed-slot tests that matter as context

`pqueue_file_store.cpp`, `pqueue_rebuild_metadata.cpp`, and `pqueue_repair.cpp` mostly test the older fixed-slot backend and shared Queue behavior.

Useful context from those tests:

- fixed-slot validation catches config mismatch, missing spool, wrong spool size, bad journal entries, active slot read failure;
- `Queue` should not advance index after torn record writes;
- `dropFrontIfCorrupt()` only drops front record if front is provably corrupt;
- `rebuildMetadata()` for fixed-slot scans active slots and refuses gaps;
- stale lock recovery behavior is tested for fake boot IDs and POSIX PIDs.

These are not append-log compaction specs, but they show expected Queue-level repair discipline.

### Running tests

The Makefile builds all POSIX tests into:

```text
build/pqueue-tests
```

and `make test` runs them.

In the inspected tar, this fails because `doctest/doctest.h` is missing. The Makefile defaults:

```text
DOCTEST_INCLUDE ?= ../third_party/doctest
```

So either put doctest where that include path resolves, or override `DOCTEST_INCLUDE`.

---

## 15. Future API/config cleanup

The current `Store` interface is inherited from the fixed-slot backend and is not ideal for append-log.

Current interface shape:

```text
writeRecord(sequence, record)
writeIndex(index)
removeRecord(sequence)
rewriteRecord(sequence, record)
readRecord(sequence, out)
```

This works naturally for fixed-slot because data write and logical index commit are separate.

For append-log, the durable operation is the event itself:

- ENQUEUE event commits enqueue;
- POP event commits pop;
- REWRITE event commits rewrite.

A future backend-shaped API could be closer to:

```text
enqueue(record, policy)
peek(out)
pop()
rewriteFront(record)
compact()
stats()
validate()
```

Queue should probably continue to own the public API and locking, but Store should own backend-specific durable operation semantics.

Potential config naming cleanup from prior design context:

```text
recordSizeBytes -> maxRecordBytes   for append-log meaning
reservedBytes   -> maxLogBytes      or similar, if enforced
maxSegmentBytes -> keep explicit
maxSegments     -> keep explicit
minFreeBytes    -> keep explicit
```

Public positioning from the design context:

- fixed-slot can remain the stable/default backend;
- append-log should remain experimental until compaction, ESP32/LittleFS tests, and crash/fault tests are strong.

---

## Quick code-reading checklist for future append-log work

Before proposing changes, answer these from the current code:

1. Which operation path is involved: mount/replay, enqueue, pop, rewrite, validation, compaction, cleanup, or repair?
2. Which exact functions are in the path?
3. Is the behavior defined by current code, current tests, or only by roadmap intent?
4. Does the answer depend on physical generation order or logical segment order?
5. Is this about normal remount recovery or explicit repair/salvage?
6. Is the relevant backend append-log or fixed-slot?
7. Have the current append-log tests been inspected for this behavior?
8. If suggesting Stage 3 compaction changes, where is the journal commit point and what happens if power dies before or after it?

Do not give a deep design review from this document alone. Use it to know where to look, then inspect the code.

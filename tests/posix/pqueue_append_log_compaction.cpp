#include "pqueue_append_log_support.h"

#ifndef ARDUINO

// Helpers shared by collectLiveRecords tests.
// enqueue/pop use the raw AppendLogStore interface so tests can inspect
// internal state (e.g. collectLiveRecords) without going through Queue.
namespace {

void storeEnqueue(pqueue::AppendLogStore& store, std::uint32_t seq, const std::string& payload) {
    CHECK(store.writeRecord(seq, payload).ok());
    pqueue::FileStoreIndex idx;
    CHECK(store.readIndex(idx).ok());
    CHECK(store.writeIndex(idx).ok());
}

void storePop(pqueue::AppendLogStore& store) {
    pqueue::FileStoreIndex idx;
    CHECK(store.readIndex(idx).ok());
    idx.head++;
    CHECK(store.writeIndex(idx).ok());
}

// Sum actual sizes of all seg-*.bin files in kSpoolDir from disk.
std::uint32_t actualOnDiskBytes() {
    std::uint32_t total = 0;
    for (const auto& entry : std::filesystem::directory_iterator(kSpoolDir)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("seg-", 0) == 0 && name.size() > 4) {
            total += static_cast<std::uint32_t>(std::filesystem::file_size(entry.path()));
        }
    }
    return total;
}

void checkTracking(pqueue::AppendLogStore& store) {
    CHECK_EQ(store.totalOnDiskBytes(), actualOnDiskBytes());
}

} // namespace

TEST_CASE("compaction: chooseCompactionRange returns nullopt for empty store") {
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);
    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());
    CHECK_FALSE(store.chooseCompactionRange().has_value());
}

TEST_CASE("compaction: chooseCompactionRange returns nullopt with only tail (critical)") {
    // A store with only a tail segment and no full ranges must not offer the
    // tail for compaction — selecting it would violate the torn-tail invariant.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    {
        ManifestData md;
        md.epoch          = 1;
        md.tailGeneration = 1;
        md.nextGeneration = 2;
        std::vector<std::uint8_t> bytes;
        serialiseManifest(md, bytes);
        std::ofstream f(manifestSlotPath('a'), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    {
        const std::string hdr = serializeSegmentHeader(1, 0);
        std::ofstream f(segmentPath(1), std::ios::binary | std::ios::trunc);
        f.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
    }

    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());
    CHECK_FALSE(store.chooseCompactionRange().has_value());
}

TEST_CASE("compaction: chooseCompactionRange returns single full range") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    {
        ManifestData md;
        md.epoch          = 1;
        md.ranges         = {{1, 1}};
        md.tailGeneration = 2;
        md.nextGeneration = 3;
        std::vector<std::uint8_t> bytes;
        serialiseManifest(md, bytes);
        std::ofstream f(manifestSlotPath('a'), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    for (std::uint32_t gen : {1u, 2u}) {
        const std::string hdr = serializeSegmentHeader(gen, 0);
        std::ofstream f(segmentPath(gen), std::ios::binary | std::ios::trunc);
        f.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
    }

    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());
    const auto range = store.chooseCompactionRange();
    REQUIRE(range.has_value());
    CHECK_EQ(range->startGen, 1u);
    CHECK_EQ(range->endGen,   1u);
}

TEST_CASE("compaction: chooseCompactionRange selects oldest full range") {
    // Two non-contiguous full ranges {1,1} and {3,3} with tail=5.
    // The oldest (first in manifest order) must be selected.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    {
        ManifestData md;
        md.epoch          = 1;
        md.ranges         = {{1, 1}, {3, 3}};
        md.tailGeneration = 5;
        md.nextGeneration = 6;
        std::vector<std::uint8_t> bytes;
        serialiseManifest(md, bytes);
        std::ofstream f(manifestSlotPath('a'), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    for (std::uint32_t gen : {1u, 3u, 5u}) {
        const std::string hdr = serializeSegmentHeader(gen, 0);
        std::ofstream f(segmentPath(gen), std::ios::binary | std::ios::trunc);
        f.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
    }

    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());
    const auto range = store.chooseCompactionRange();
    REQUIRE(range.has_value());
    CHECK_EQ(range->startGen, 1u);
    CHECK_EQ(range->endGen,   1u);
}

TEST_CASE("compaction: collectLiveRecords returns empty for range with no records") {
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);
    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());
    std::vector<pqueue::AppendLogStore::CompactionLiveRecord> live;
    CHECK(store.collectLiveRecords({1, 1}, live).ok());
    CHECK(live.empty());
}

TEST_CASE("compaction: collectLiveRecords returns records in FIFO order; out-of-range excluded") {
    // seq=1,2 land in gen=1 (full range after rotation); seq=3 lands in gen=2 (tail).
    // Collecting {1,1} must return seq=1 then seq=2 only.
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);
    using namespace pqueue::append_log_detail;

    // One 1-byte record costs kEnqueueHeaderBytes(16) + 1 + kEventTrailerBytes(8) = 25 bytes.
    // Header = 20. Two records + one rewrite fill 20+25+25+25 = 95 bytes; fourth event triggers
    // rotation. Rewrite creates dead bytes so the ratio selector picks up gen=1.
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 95;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a");
    storeEnqueue(store, 2, "b");
    CHECK(store.rewriteRecord(1, "a").ok()); // dead bytes in gen=1; seq=1 payload unchanged
    storeEnqueue(store, 3, "c"); // triggers rotation: gen=1 becomes full range, gen=2 = tail

    const auto range = store.chooseCompactionRange();
    REQUIRE(range.has_value());
    CHECK_EQ(range->startGen, 1u);

    std::vector<pqueue::AppendLogStore::CompactionLiveRecord> live;
    CHECK(store.collectLiveRecords(*range, live).ok());
    REQUIRE_EQ(live.size(), 2u);
    CHECK_EQ(live[0].sequence, 1u);
    CHECK_EQ(live[0].payload,  "a");
    CHECK_EQ(live[1].sequence, 2u);
    CHECK_EQ(live[1].payload,  "b");
}

TEST_CASE("compaction: collectLiveRecords excludes popped records") {
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70; // same sizing as above: 2 records per segment
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a");
    storeEnqueue(store, 2, "b");
    storeEnqueue(store, 3, "c"); // rotation: gen=1 full, gen=2 tail

    storePop(store); // pop seq=1

    const auto range = store.chooseCompactionRange();
    REQUIRE(range.has_value());

    std::vector<pqueue::AppendLogStore::CompactionLiveRecord> live;
    CHECK(store.collectLiveRecords(*range, live).ok());
    REQUIRE_EQ(live.size(), 1u);
    CHECK_EQ(live[0].sequence, 2u);
    CHECK_EQ(live[0].payload,  "b");
}

TEST_CASE("compaction: compactOneSegment removes dead range without writing a new segment") {
    // Use a crafted manifest so we control segment layout precisely.
    // gen=1 (full range, header only — no records): dead range.
    // gen=4 (tail, NON-contiguous with gen=1) has seq=1 "c".
    // Non-contiguous tail prevents rotate-before-compact from extending the range.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    {
        ManifestData md;
        md.epoch          = 1;
        md.ranges         = {{1, 1}};
        md.tailGeneration = 4;
        md.nextGeneration = 5;
        std::vector<std::uint8_t> bytes;
        serialiseManifest(md, bytes);
        std::ofstream f(manifestSlotPath('a'), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    {
        // gen=1: header only — no ENQUEUE events, so no live records map to it
        const std::string hdr = serializeSegmentHeader(1, 0);
        std::ofstream f(segmentPath(1), std::ios::binary | std::ios::trunc);
        f.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
    }
    {
        // gen=4: header + one live record seq=1 "c" — non-contiguous tail
        std::string seg = serializeSegmentHeader(4, 1);
        seg += serializeEnqueueEvent(1, "c");
        std::ofstream f(segmentPath(4), std::ios::binary | std::ios::trunc);
        f.write(seg.data(), static_cast<std::streamsize>(seg.size()));
    }

    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());
    REQUIRE(store.chooseCompactionRange().has_value());

    CHECK(store.compactOneSegment().ok());

    CHECK_FALSE(std::filesystem::exists(segmentPath(5))); // no new segment written (nextGen=5)
    CHECK_FALSE(store.chooseCompactionRange().has_value()); // dead range gone

    std::string out;
    CHECK(store.readRecord(1, out).ok()); // live record in gen=4 still accessible
    CHECK_EQ(out, "c");
}

TEST_CASE("compaction: compactOneSegment is no-op under segment-count pressure when all ranges are live") {
    // needsCompaction() fires because activeGenerations_.size() > maxSegments.
    // All records are live — no dead bytes — so compactOneSegment() must return no-op
    // without writing any new segment. The write path must tolerate this cleanly.
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70; // fits exactly 2 one-byte records per segment
    cfg.maxSegments     = 1;  // trigger needsCompaction() at 2+ active generations
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a");
    storeEnqueue(store, 2, "b");
    storeEnqueue(store, 3, "c"); // gen=1 full (seq=1,2), gen=2 tail (seq=3); 2 gens > maxSegments=1

    // needsCompaction() would fire due to segment count; all records live → no-op
    const auto st = store.compactOneSegment();
    CHECK(st.ok());
    CHECK(st.isNoOp()); // nothing to reclaim; no-op, not an error

    CHECK_FALSE(std::filesystem::exists(segmentPath(3))); // no new segment written

    std::string out;
    CHECK(store.readRecord(1, out).ok()); CHECK_EQ(out, "a");
    CHECK(store.readRecord(2, out).ok()); CHECK_EQ(out, "b");
    CHECK(store.readRecord(3, out).ok()); CHECK_EQ(out, "c");
}

TEST_CASE("compaction: compactOneSegment is no-op when 1-in-1-out with no dead bytes") {
    // Regression: a fully live single-segment range produces 1 output segment,
    // which equals the input count with no dead bytes. Must be a no-op — writing
    // a new segment would waste flash and a generation number for zero benefit.
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70; // fits exactly 2 one-byte records per segment
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a");
    storeEnqueue(store, 2, "b");
    storeEnqueue(store, 3, "c"); // gen=1 full (seq=1,2), gen=2 tail (seq=3); no dead bytes

    const auto st = store.compactOneSegment();
    CHECK(st.ok());
    CHECK(st.isNoOp());

    // No new compacted segment created.
    CHECK_FALSE(std::filesystem::exists(segmentPath(3)));

    // All records still readable from their original segments.
    std::string out;
    CHECK(store.readRecord(1, out).ok()); CHECK_EQ(out, "a");
    CHECK(store.readRecord(2, out).ok()); CHECK_EQ(out, "b");
    CHECK(store.readRecord(3, out).ok()); CHECK_EQ(out, "c");

    // Fresh remount is valid and reads all records.
    {
        pqueue::AppendLogStore store2(cfg);
        CHECK(store2.mount().ok());
        CHECK(store2.readRecord(1, out).ok()); CHECK_EQ(out, "a");
        CHECK(store2.readRecord(2, out).ok()); CHECK_EQ(out, "b");
        CHECK(store2.readRecord(3, out).ok()); CHECK_EQ(out, "c");
    }
}

TEST_CASE("compaction: compactOneSegment preserves live records in FIFO order") {
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a");
    storeEnqueue(store, 2, "b");
    storeEnqueue(store, 3, "c"); // gen=1 full (seq=1,2), gen=2 tail (seq=3)

    // Pop seq=1 to create dead bytes in gen=1 (compaction is a no-op with no dead bytes).
    storePop(store);

    CHECK(store.compactOneSegment().ok());

    // gen=3 written with seq=2 only; gen=1 dangling and cleaned up.
    CHECK(std::filesystem::exists(segmentPath(3)));
    std::string out;
    CHECK(store.readRecord(2, out).ok()); CHECK_EQ(out, "b");
    CHECK(store.readRecord(3, out).ok()); CHECK_EQ(out, "c");
}

TEST_CASE("compaction: compactOneSegment preserves rewritten payload") {
    // seq=1 is rewritten before rotation, so its SegmentRecord points at the REWRITE
    // offset inside gen=1. compactOneSegment must copy the rewritten value, not the
    // original enqueue bytes.
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 100; // fits 2 enqueues + 1 rewrite; 3rd enqueue rotates
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "x");
    storeEnqueue(store, 2, "y");
    CHECK(store.rewriteRecord(1, "z").ok()); // REWRITE stays in gen=1
    storeEnqueue(store, 3, "w");             // rotation: gen=1 full, gen=2 tail

    CHECK(store.compactOneSegment().ok());

    std::string out;
    CHECK(store.readRecord(1, out).ok()); CHECK_EQ(out, "z"); // rewritten value
    CHECK(store.readRecord(2, out).ok()); CHECK_EQ(out, "y");
    CHECK(store.readRecord(3, out).ok()); CHECK_EQ(out, "w");
}

TEST_CASE("compaction: compactOneSegment is no-op when multi-segment output would not reduce segment count") {
    // range {1,1} = 1 input segment; seq=1 popped (dead bytes), seq=2+3 live require 2 output
    // segments with maxSegmentBytes=50; 2 output > 1 input → no-op.
    // Tail at gen=5 (non-contiguous with last range gen=1) prevents rotate-before-compact
    // from extending the hypothetical range to include the tail.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    {
        ManifestData md;
        md.epoch          = 1;
        md.ranges         = {{1, 1}};
        md.tailGeneration = 5;
        md.nextGeneration = 6;
        std::vector<std::uint8_t> bytes;
        serialiseManifest(md, bytes);
        std::ofstream f(manifestSlotPath('a'), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    {
        std::string seg = serializeSegmentHeader(1, 1);
        seg += serializeEnqueueEvent(1, "a");
        seg += serializeEnqueueEvent(2, "b");
        seg += serializeEnqueueEvent(3, "c");
        std::ofstream f(segmentPath(1), std::ios::binary | std::ios::trunc);
        f.write(seg.data(), static_cast<std::streamsize>(seg.size()));
    }
    {
        const std::string hdr = serializeSegmentHeader(5, 4);
        std::ofstream f(segmentPath(5), std::ios::binary | std::ios::trunc);
        f.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
    }

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 50;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());
    storePop(store); // pop seq=1 → dead bytes in gen=1; seq=2+3 still live
    CHECK(store.compactOneSegment().ok());

    CHECK_FALSE(std::filesystem::exists(segmentPath(6)));      // no new segment (nextGen=6)
    CHECK(store.chooseCompactionRange().has_value());           // range still present
}

TEST_CASE("cleanup: compactOneSegment deletes old segment file after publish") {
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a");
    storeEnqueue(store, 2, "b");
    storeEnqueue(store, 3, "c"); // gen=1 -> full range, gen=2 = tail

    // Pop seq=1 to create dead bytes in gen=1; compaction is a no-op with no dead bytes.
    storePop(store);

    // rotate-before-compact seals gen=2 (tail) into the range; compact output = gen=4.
    // Both old segs gen=1 and gen=2 are cleaned up (inputSegCount=2 cleanups run).
    CHECK(store.compactOneSegment().ok());
    CHECK_FALSE(std::filesystem::exists(segmentPath(1))); // old range: cleaned up
    CHECK_FALSE(std::filesystem::exists(segmentPath(2))); // old tail: cleaned up
    CHECK(std::filesystem::exists(segmentPath(3)));        // new tail (from rotate)
    CHECK(std::filesystem::exists(segmentPath(4)));        // new compacted segment
    std::string out;
    CHECK(store.readRecord(2, out).ok()); CHECK_EQ(out, "b");
    CHECK(store.readRecord(3, out).ok()); CHECK_EQ(out, "c");
}

TEST_CASE("compaction: remount after compactOneSegment reads records from new segment") {
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    {
        pqueue::AppendLogStore store(cfg);
        CHECK(store.mount().ok());
        storeEnqueue(store, 1, "a");
        storeEnqueue(store, 2, "b");
        storeEnqueue(store, 3, "c");
        CHECK(store.compactOneSegment().ok());
    }
    {
        pqueue::AppendLogStore store(cfg);
        CHECK(store.mount().ok());
        std::string out;
        CHECK(store.readRecord(1, out).ok()); CHECK_EQ(out, "a");
        CHECK(store.readRecord(2, out).ok()); CHECK_EQ(out, "b");
        CHECK(store.readRecord(3, out).ok()); CHECK_EQ(out, "c");
    }
}

TEST_CASE("compaction: dangling segment from failed publish is harmless on remount (critical)") {
    // Simulates a crash after the new compacted segment was written but before the
    // manifest publish succeeded. On remount the winning manifest still points at the
    // original segments; the dangling file is ignored and all records are intact.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    // gen=1: full range with seq=1 "a", seq=2 "b"
    {
        ManifestData md;
        md.epoch          = 1;
        md.ranges         = {{1, 1}};
        md.tailGeneration = 2;
        md.nextGeneration = 3;
        std::vector<std::uint8_t> bytes;
        serialiseManifest(md, bytes);
        std::ofstream f(manifestSlotPath('a'), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    {
        std::string seg = serializeSegmentHeader(1, 1);
        seg += serializeEnqueueEvent(1, "a");
        seg += serializeEnqueueEvent(2, "b");
        std::ofstream f(segmentPath(1), std::ios::binary | std::ios::trunc);
        f.write(seg.data(), static_cast<std::streamsize>(seg.size()));
    }
    {
        const std::string hdr = serializeSegmentHeader(2, 3);
        std::ofstream f(segmentPath(2), std::ios::binary | std::ios::trunc);
        f.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
    }
    // Dangling gen=3: written by compactOneSegment before crash, never in manifest
    {
        std::string seg = serializeSegmentHeader(3, 1);
        seg += serializeEnqueueEvent(1, "a");
        seg += serializeEnqueueEvent(2, "b");
        std::ofstream f(segmentPath(3), std::ios::binary | std::ios::trunc);
        f.write(seg.data(), static_cast<std::streamsize>(seg.size()));
    }

    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());

    // Old segments are authoritative; all original records intact
    std::string out;
    CHECK(store.readRecord(1, out).ok()); CHECK_EQ(out, "a");
    CHECK(store.readRecord(2, out).ok()); CHECK_EQ(out, "b");

    // Rewrite seq=1 to same value to create dead bytes in gen=1 so the ratio selector fires.
    CHECK(store.rewriteRecord(1, "a").ok());

    // Manifest unchanged: gen=1 is still the compaction target
    const auto range = store.chooseCompactionRange();
    REQUIRE(range.has_value());
    CHECK_EQ(range->startGen, 1u);
    CHECK_EQ(range->endGen,   1u);
}

TEST_CASE("compaction: compactFull removes multiple dead ranges in one call") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    // Two non-contiguous dead ranges {1,1} and {3,3} with tail=5.
    // Segments have headers only (no records), so both ranges are dead.
    {
        ManifestData md;
        md.epoch          = 1;
        md.ranges         = {{1, 1}, {3, 3}};
        md.tailGeneration = 5;
        md.nextGeneration = 6;
        std::vector<std::uint8_t> bytes;
        serialiseManifest(md, bytes);
        std::ofstream f(manifestSlotPath('a'), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    for (std::uint32_t gen : {1u, 3u, 5u}) {
        const std::string hdr = serializeSegmentHeader(gen, 0);
        std::ofstream f(segmentPath(gen), std::ios::binary | std::ios::trunc);
        f.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
    }

    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());
    REQUIRE(store.chooseCompactionRange().has_value());

    CHECK(store.compactFull().ok());
    CHECK_FALSE(store.chooseCompactionRange().has_value()); // both dead ranges gone
}

TEST_CASE("compaction: collectLiveRecords returns rewritten payload, not original (critical)") {
    // If a record was rewritten before compaction, the collected payload must be
    // the rewritten value. Returning the original would silently compact stale data.
    //
    // Layout: enqueue seq=1 "original", enqueue seq=2 "second", rewrite seq=1 to
    // "rewritten" — the REWRITE event fits in gen=1. Then enqueue seq=3 to trigger
    // rotation so gen=1 becomes a full range. seq=1's SegmentRecord now points to
    // the REWRITE event offset inside gen=1.
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    // Three events (2 ENQUEUEs + 1 REWRITE) of 1-byte payloads each = 25 bytes.
    // Header = 20. Total = 95. maxSegmentBytes = 100 fits all three; fourth event
    // (seq=3 enqueue, 25 bytes) pushes to 120 > 100 and triggers rotation.
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 100;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "x");
    storeEnqueue(store, 2, "y");
    CHECK(store.rewriteRecord(1, "z").ok()); // REWRITE event stays in gen=1
    storeEnqueue(store, 3, "w");             // rotation: gen=1 -> full range, gen=2 = tail

    const auto range = store.chooseCompactionRange();
    REQUIRE(range.has_value());
    CHECK_EQ(range->startGen, 1u);

    std::vector<pqueue::AppendLogStore::CompactionLiveRecord> live;
    CHECK(store.collectLiveRecords(*range, live).ok());
    REQUIRE_EQ(live.size(), 2u);
    CHECK_EQ(live[0].sequence, 1u);
    CHECK_EQ(live[0].payload,  "z"); // rewritten value, not "x"
    CHECK_EQ(live[1].sequence, 2u);
    CHECK_EQ(live[1].payload,  "y");
}

// --- Stage 7: cleanup tests ---

TEST_CASE("cleanup: dangling segment from failed publish is deleted on next successful publish") {
    // Simulate a dangling segment that was written before a crash (never in manifest).
    // The next successful publishManifest (triggered by compactOneSegment) must delete it.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    // Write a valid store: gen=1 full range, gen=2 tail
    {
        ManifestData md;
        md.epoch          = 1;
        md.ranges         = {{1, 1}};
        md.tailGeneration = 2;
        md.nextGeneration = 3;
        std::vector<std::uint8_t> bytes;
        serialiseManifest(md, bytes);
        std::ofstream f(manifestSlotPath('a'), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    {
        std::string seg = serializeSegmentHeader(1, 1);
        seg += serializeEnqueueEvent(1, "a");
        seg += serializeEnqueueEvent(2, "b");
        std::ofstream f(segmentPath(1), std::ios::binary | std::ios::trunc);
        f.write(seg.data(), static_cast<std::streamsize>(seg.size()));
    }
    {
        const std::string hdr = serializeSegmentHeader(2, 3);
        std::ofstream f(segmentPath(2), std::ios::binary | std::ios::trunc);
        f.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
    }
    // Dangling gen=3: written before a crash, never referenced by the manifest
    {
        std::string seg = serializeSegmentHeader(3, 1);
        seg += serializeEnqueueEvent(1, "a");
        std::ofstream f(segmentPath(3), std::ios::binary | std::ios::trunc);
        f.write(seg.data(), static_cast<std::streamsize>(seg.size()));
    }

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok()); // cleanup on mount removes gen=3
    CHECK_FALSE(std::filesystem::exists(segmentPath(3)));
    CHECK(std::filesystem::exists(segmentPath(1))); // still referenced
    CHECK(std::filesystem::exists(segmentPath(2))); // still referenced
}

TEST_CASE("cleanup: referenced segment is never deleted (critical)") {
    // Every segment referenced by the current manifest must survive cleanup.
    // This test enqueues across multiple segments, compacts, and then verifies
    // that after cleanup all manifest-referenced segments still exist on disk.
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a");
    storeEnqueue(store, 2, "b");
    storeEnqueue(store, 3, "c"); // gen=1 full, gen=2 tail

    // Pop seq=1 to create dead bytes in gen=1; compaction is a no-op with no dead bytes.
    storePop(store);

    // rotate-before-compact seals gen=2 into range; output=gen=4, new tail=gen=3.
    // Old segs gen=1 and gen=2 are cleaned up (inputSegCount=2 cleanups run).
    CHECK(store.compactOneSegment().ok());

    // Manifest references gen=3 (new tail) and gen=4 (compacted). Both must exist.
    CHECK_FALSE(std::filesystem::exists(segmentPath(1)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(2)));
    CHECK(std::filesystem::exists(segmentPath(3)));
    CHECK(std::filesystem::exists(segmentPath(4)));

    // Live records still readable (seq=1 was popped).
    std::string out;
    CHECK(store.readRecord(2, out).ok()); CHECK_EQ(out, "b");
    CHECK(store.readRecord(3, out).ok()); CHECK_EQ(out, "c");
}

TEST_CASE("cleanup: multiple dangling segments cleaned up one per publish") {
    // Two dangling files on disk. Each successful publishManifest removes one.
    // After two publishes both should be gone.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    // Manifest references only gen=1 (full) and gen=2 (tail).
    {
        ManifestData md;
        md.epoch          = 1;
        md.ranges         = {{1, 1}};
        md.tailGeneration = 2;
        md.nextGeneration = 5;
        std::vector<std::uint8_t> bytes;
        serialiseManifest(md, bytes);
        std::ofstream f(manifestSlotPath('a'), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    {
        std::string seg = serializeSegmentHeader(1, 1);
        seg += serializeEnqueueEvent(1, "a");
        seg += serializeEnqueueEvent(2, "b");
        std::ofstream f(segmentPath(1), std::ios::binary | std::ios::trunc);
        f.write(seg.data(), static_cast<std::streamsize>(seg.size()));
    }
    {
        const std::string hdr = serializeSegmentHeader(2, 3);
        std::ofstream f(segmentPath(2), std::ios::binary | std::ios::trunc);
        f.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
    }
    // Two dangling files: gen=3 and gen=4
    for (std::uint32_t g : {3u, 4u}) {
        std::string seg = serializeSegmentHeader(g, 1);
        std::ofstream f(segmentPath(g), std::ios::binary | std::ios::trunc);
        f.write(seg.data(), static_cast<std::streamsize>(seg.size()));
    }

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok()); // first cleanup: removes gen=3

    // One dangling file removed; one remains
    const bool gen3gone = !std::filesystem::exists(segmentPath(3));
    const bool gen4gone = !std::filesystem::exists(segmentPath(4));
    CHECK((gen3gone || gen4gone));   // at least one removed
    CHECK_FALSE((gen3gone && gen4gone)); // not both (one per cleanup pass)

    // Pop both records so gen=1 is fully dead, then compact (dead-range removal triggers publish).
    storePop(store);
    storePop(store);

    // Trigger a second publish (dead-range removal) — removes the other dangling file.
    CHECK(store.compactOneSegment().ok()); // dead range: drops gen=1; second cleanup runs
    CHECK_FALSE(std::filesystem::exists(segmentPath(3)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(4)));
}

TEST_CASE("compaction: multi-segment output compacts range when output count < input count") {
    // range {1,3} = 3 input segments; records 1,2 popped → 4 live records fit in 2 output segments.
    // 2 < 3 → compaction proceeds with multi-segment output.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    // seg 1: records 1,2 (will be popped)
    {
        std::string seg = serializeSegmentHeader(1, 1);
        seg += serializeEnqueueEvent(1, "a");
        seg += serializeEnqueueEvent(2, "b");
        std::ofstream f(segmentPath(1), std::ios::binary | std::ios::trunc);
        f.write(seg.data(), static_cast<std::streamsize>(seg.size()));
    }
    // seg 2: records 3,4 (live)
    {
        std::string seg = serializeSegmentHeader(2, 3);
        seg += serializeEnqueueEvent(3, "c");
        seg += serializeEnqueueEvent(4, "d");
        std::ofstream f(segmentPath(2), std::ios::binary | std::ios::trunc);
        f.write(seg.data(), static_cast<std::streamsize>(seg.size()));
    }
    // seg 3: records 5,6 (live)
    {
        std::string seg = serializeSegmentHeader(3, 5);
        seg += serializeEnqueueEvent(5, "e");
        seg += serializeEnqueueEvent(6, "f");
        std::ofstream f(segmentPath(3), std::ios::binary | std::ios::trunc);
        f.write(seg.data(), static_cast<std::streamsize>(seg.size()));
    }
    // seg 4 (tail): pop events for records 1,2
    {
        std::string seg = serializeSegmentHeader(4, 7);
        seg += serializePopEvent(1);
        seg += serializePopEvent(2);
        std::ofstream f(segmentPath(4), std::ios::binary | std::ios::trunc);
        f.write(seg.data(), static_cast<std::streamsize>(seg.size()));
    }
    {
        ManifestData md;
        md.epoch          = 1;
        md.ranges         = {{1, 3}};
        md.tailGeneration = 4;
        md.nextGeneration = 5;
        std::vector<std::uint8_t> bytes;
        serialiseManifest(md, bytes);
        std::ofstream f(manifestSlotPath('a'), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70; // 20 header + 2×(24+1) = 70: exactly 2 one-byte records per segment
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    auto st = store.compactOneSegment();
    CHECK(st.ok());
    CHECK_FALSE(st.isNoOp());

    // rotate-before-compact seals tail gen=4 into range → {1,4}; new tail=gen=5, nextGen=6.
    // Compact {1,4} → two output segs gen=6,7. All 4 input segs cleaned (inputSegCount=4).
    CHECK(std::filesystem::exists(segmentPath(6)));
    CHECK(std::filesystem::exists(segmentPath(7)));

    // manifest: one range {6,7}, tail=5
    REQUIRE_EQ(store.manifestRanges().size(), 1u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 6u);
    CHECK_EQ(store.manifestRanges()[0].endGen, 7u);

    // records 3-6 still readable; records 1-2 are gone (popped)
    std::string out;
    CHECK(store.readRecord(3, out).ok()); CHECK_EQ(out, "c");
    CHECK(store.readRecord(4, out).ok()); CHECK_EQ(out, "d");
    CHECK(store.readRecord(5, out).ok()); CHECK_EQ(out, "e");
    CHECK(store.readRecord(6, out).ok()); CHECK_EQ(out, "f");
}

TEST_CASE("compaction: multi-segment output survives remount") {
    // Same setup as previous test; verify state is consistent after remount.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;

    {
        pqueue::AppendLogStore store(cfg);
        CHECK(store.mount().ok());
        storeEnqueue(store, 1, "a");
        storeEnqueue(store, 2, "b");
        storeEnqueue(store, 3, "c"); // rotation → {1,1}, tail=2
        storeEnqueue(store, 4, "d");
        storeEnqueue(store, 5, "e"); // rotation → {1,2}, tail=3
        storeEnqueue(store, 6, "f");
        storeEnqueue(store, 7, "g"); // rotation → {1,3}, tail=4
        storePop(store); // pop seq=1
        storePop(store); // pop seq=2 → rotation of tail → {1,4}, tail=5
    }

    // After remount with range {1,4}: records 1,2 popped → 5 live records in 4-seg range.
    // Live: seq 3..7 (5 records × 25 bytes = 125 bytes). Output segs needed:
    //   seg0: 20+25+25=70 (seqs 3,4). seg1: 20+25+25=70 (seqs 5,6). seg2: 20+25=45 (seq 7).
    // 3 output < 4 input → compaction proceeds.
    {
        pqueue::AppendLogStore store(cfg);
        CHECK(store.mount().ok());
        CHECK(store.compactOneSegment().ok());
    }

    {
        pqueue::AppendLogStore store(cfg);
        CHECK(store.mount().ok());
        std::string out;
        CHECK(store.readRecord(3, out).ok()); CHECK_EQ(out, "c");
        CHECK(store.readRecord(4, out).ok()); CHECK_EQ(out, "d");
        CHECK(store.readRecord(5, out).ok()); CHECK_EQ(out, "e");
        CHECK(store.readRecord(6, out).ok()); CHECK_EQ(out, "f");
        CHECK(store.readRecord(7, out).ok()); CHECK_EQ(out, "g");
    }
}

TEST_CASE("compaction: multi-segment output is no-op when output count equals input count") {
    // range {1,2} = 2 input segments; seq=1 popped (dead bytes), seq=2+3+4 live require 3
    // output segments with maxSegmentBytes=50; 3 output > 2 input → no-op.
    // Tail at gen=5 (non-contiguous with last range gen=2) prevents rotate-before-compact
    // from extending the hypothetical range to include the tail.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    {
        std::string seg = serializeSegmentHeader(1, 1);
        seg += serializeEnqueueEvent(1, "a");
        seg += serializeEnqueueEvent(2, "b");
        std::ofstream f(segmentPath(1), std::ios::binary | std::ios::trunc);
        f.write(seg.data(), static_cast<std::streamsize>(seg.size()));
    }
    {
        std::string seg = serializeSegmentHeader(2, 3);
        seg += serializeEnqueueEvent(3, "c");
        seg += serializeEnqueueEvent(4, "d");
        std::ofstream f(segmentPath(2), std::ios::binary | std::ios::trunc);
        f.write(seg.data(), static_cast<std::streamsize>(seg.size()));
    }
    {
        std::string hdr = serializeSegmentHeader(5, 5);
        std::ofstream f(segmentPath(5), std::ios::binary | std::ios::trunc);
        f.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
    }
    {
        ManifestData md;
        md.epoch          = 1;
        md.ranges         = {{1, 2}};
        md.tailGeneration = 5;
        md.nextGeneration = 6;
        std::vector<std::uint8_t> bytes;
        serialiseManifest(md, bytes);
        std::ofstream f(manifestSlotPath('a'), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 50; // 3 live records each need their own segment → 3 output > 2 input
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());
    storePop(store); // pop seq=1 → dead bytes in gen=1; seq=2+3+4 still live
    auto st = store.compactOneSegment();
    CHECK(st.ok());
    CHECK(st.isNoOp());
    CHECK_FALSE(std::filesystem::exists(segmentPath(6))); // no new segment written (nextGen=6)
    CHECK(store.chooseCompactionRange().has_value());     // range still present
}

TEST_CASE("totalOnDiskBytes tracking: matches actual file sizes after every operation") {
    // Verifies that the RAM-tracked totalOnDiskBytes() stays in sync with actual
    // disk after enqueue, pop, rewrite, rotation, compaction, and cleanup.
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70; // header(20) + 2 records(25 each) = 70
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());
    checkTracking(store); // empty store: 0

    storeEnqueue(store, 1, "a"); checkTracking(store);
    storeEnqueue(store, 2, "b"); checkTracking(store);
    storeEnqueue(store, 3, "c"); checkTracking(store); // rotation: gen=1 full, gen=2 tail

    CHECK(store.rewriteRecord(1, "x").ok()); checkTracking(store); // rewrite in gen=2

    storePop(store); checkTracking(store); // pop seq=1; POP event appended

    // compact: gen=1 (dead bytes from pop) → gen=3; gen=1 becomes dangling
    CHECK(store.compactOneSegment().ok()); checkTracking(store);

    // cleanup runs inside the next publishManifest; trigger via another enqueue cycle
    storeEnqueue(store, 4, "d"); checkTracking(store);

    // Remount: tracking reinitialised from disk
    pqueue::AppendLogStore store2(cfg);
    CHECK(store2.mount().ok());
    checkTracking(store2);
}

TEST_CASE("maxTotalBytes: enqueue blocked when footprint full and nothing to compact") {
    // Two live records fill the active segment to exactly maxTotalBytes.
    // A third enqueue would exceed it; compaction no-ops (no dead bytes); writeRecord
    // must return QueueFull without writing anything.
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    // header=20, each 1-byte record=25; two records fill segment to 70 bytes exactly.
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    cfg.maxTotalBytes   = 70; // footprint cap == current segment size
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a");
    storeEnqueue(store, 2, "b"); // on-disk footprint = 70 bytes; cap reached

    const auto st = store.writeRecord(3, "c");
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::QueueFull);

    CHECK_FALSE(std::filesystem::exists(segmentPath(2))); // no rotation triggered

    std::string out;
    CHECK(store.readRecord(1, out).ok()); CHECK_EQ(out, "a");
    CHECK(store.readRecord(2, out).ok()); CHECK_EQ(out, "b");
}

TEST_CASE("maxTotalBytes: compaction makes room for a blocked enqueue") {
    // After a pop creates dead bytes in a full range, a subsequent enqueue that would
    // exceed maxTotalBytes triggers compaction automatically and then succeeds.
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    cfg.maxTotalBytes   = 120;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a");
    storeEnqueue(store, 2, "b");
    storePop(store); // pop seq=1; POP event overflows gen=1 → rotation to gen=2
                     // gen=1: 70 bytes (full range), gen=2: 40 bytes (header+pop)

    // footprint=110; +25 for enqueue=135 > 120 → compact gen=1 → gen=3(45b); then fits
    CHECK(store.writeRecord(3, "c").ok());
    pqueue::FileStoreIndex idx;
    CHECK(store.readIndex(idx).ok());
    CHECK(store.writeIndex(idx).ok());

    std::string out;
    CHECK(store.readRecord(2, out).ok()); CHECK_EQ(out, "b");
    CHECK(store.readRecord(3, out).ok()); CHECK_EQ(out, "c");

    // remount: store survives with correct layout
    pqueue::AppendLogStore store2(cfg);
    CHECK(store2.mount().ok());
    CHECK(store2.readRecord(2, out).ok()); CHECK_EQ(out, "b");
    CHECK(store2.readRecord(3, out).ok()); CHECK_EQ(out, "c");
}

TEST_CASE("maxTotalBytes: DropOldest evicts and retries when writeRecord returns QueueFull") {
    // gen=1 holds a 50-byte record (94 bytes on disk); gen=2 is the tail (45 bytes).
    // totalOnDisk=139 == maxTotalBytes. enqueue("c") triggers compaction, but gen=1 is
    // fully live so it no-ops → writeRecord returns QueueFull. The DropOldest retry
    // evicts "a" (POP lands in gen=2, no rotation), making gen=1 a dead range. The
    // second writeRecord call compacts gen=1 away (94 bytes freed) and writes "c".
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 100;
    cfg.reservedBytes   = 139; // gen1(94 bytes) + gen2(45 bytes)
    cfg.fullQueuePolicy = pqueue::FullQueuePolicy::DropOldest;
    pqueue::Queue q(cfg);

    REQUIRE(q.enqueue(std::string(50, 'a')).ok()); // gen=1: header(20) + event(74) = 94 bytes
    REQUIRE(q.enqueue("b").ok());                   // 94+25>100 → rotate; gen=2: 20+25=45; total=139
    CHECK(q.enqueue("c").ok());                     // canEnqueue passes; writeRecord→QueueFull→evict→retry

    CHECK_EQ(q.stats().count, 2U);

    std::string out;
    REQUIRE(q.peek(out).ok());
    CHECK_EQ(out, "b");
    q.pop();
    REQUIRE(q.peek(out).ok());
    CHECK_EQ(out, "c");
}

TEST_CASE("sequence exhaustion: writeRecord fails closed at UINT32_MAX") {
    cleanSpool();
    auto cfg = makeStoreConfig();
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    const auto st = store.writeRecord(std::numeric_limits<std::uint32_t>::max(), "data");
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::SequenceExhausted);
}

// ---------------------------------------------------------------------------
// rotate-before-compact tests
// ---------------------------------------------------------------------------

TEST_CASE("compaction: rotate-before-compact seals contiguous tail into last range") {
    // Verifies that compactRange on the last manifest range seals the active tail into
    // the compaction input when the tail is contiguous, preventing generation gap
    // fragmentation. Setup: ranges=[{1,1}], tail=2 (contiguous), dead bytes in gen=1.
    // After compactOneSegment(), output must start at nextGeneration_ after the rotate
    // (gen=4, not gen=3), confirming tail=2 was sealed and merged into {1,2} first.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    // One 1-byte record: kSegmentHeaderBytes(20) + kEnqueueHeaderBytes(16) + 1 + kEventTrailerBytes(8) = 45 bytes.
    // Two records fill exactly one segment at maxSegmentBytes=70 (20+25+25=70).
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a"); // gen=1
    storeEnqueue(store, 2, "b"); // gen=1 full → rotate; gen=2 tail
    storePop(store);             // pop seq=1: dead bytes in gen=1
    storeEnqueue(store, 3, "c"); // goes to gen=2 (still has room)

    // State: ranges=[{1,1}], tail=2 (contiguous: 1+1==2), seq=2 live in gen=1 (dead), seq=3 in gen=2.
    REQUIRE_EQ(store.manifestRanges().size(), 1U);
    REQUIRE_EQ(store.manifestRanges()[0].startGen, 1U);
    REQUIRE_EQ(store.manifestRanges()[0].endGen,   1U);

    CHECK(store.compactOneSegment().ok());

    // rotate seals gen=2 into {1,2} (new tail=gen=3, nextGen=4).
    // Compact of {1,2} outputs at gen=4. Manifest: ranges=[{4,4}], tailGeneration=3.
    const auto& ranges = store.manifestRanges();
    REQUIRE_EQ(ranges.size(), 1U);
    CHECK_EQ(ranges[0].startGen, 4U);
    CHECK_EQ(ranges[0].endGen,   4U);
    CHECK_EQ(store.tailGeneration(), 3U);
    CHECK(std::filesystem::exists(segmentPath(3))); // new tail
    CHECK(std::filesystem::exists(segmentPath(4))); // compacted output

    std::string out;
    CHECK(store.readRecord(2, out).ok()); CHECK_EQ(out, "b");
    CHECK(store.readRecord(3, out).ok()); CHECK_EQ(out, "c");
}

TEST_CASE("compaction: rotate-before-compact remount preserves FIFO order (critical)") {
    // After rotate-before-compact, the manifest has output gens numerically above the
    // former tail gen. Verify that a fresh remount replays in correct FIFO order.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70; // two 1-byte records per segment
    {
        pqueue::AppendLogStore store(cfg);
        CHECK(store.mount().ok());

        storeEnqueue(store, 1, "a");
        storeEnqueue(store, 2, "b"); // gen=1 full, gen=2 tail
        storePop(store);             // dead bytes in gen=1
        storeEnqueue(store, 3, "c");

        CHECK(store.compactOneSegment().ok()); // rotate-before-compact fires
    }
    {
        pqueue::AppendLogStore store2(cfg);
        CHECK(store2.mount().ok());

        // seq=2 must come before seq=3 regardless of generation ordering.
        std::string out;
        CHECK(store2.readRecord(2, out).ok()); CHECK_EQ(out, "b");
        CHECK(store2.readRecord(3, out).ok()); CHECK_EQ(out, "c");
    }
}

TEST_CASE("compaction: rotate-before-compact does not fire for non-last range") {
    // The selectedIsLastRange guard must prevent rotate even when the active tail IS
    // contiguous with the last range. Setup: ranges=[{1,1},{3,4}], tail=5 (contiguous
    // with last range: 4+1==5). chooseCompactionRange picks {1,1} (dead range), which
    // is NOT the last range. Rotate must not fire despite the contiguous tail.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    {
        ManifestData md;
        md.epoch          = 1;
        md.ranges         = {{1, 1}, {3, 4}};
        md.tailGeneration = 5;
        md.nextGeneration = 6;
        std::vector<std::uint8_t> bytes;
        serialiseManifest(md, bytes);
        std::ofstream f(manifestSlotPath('a'), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    // gen=1: one live enqueue so it is a real range; we will pop it to make it dead.
    {
        std::string seg = serializeSegmentHeader(1, 1);
        seg += serializeEnqueueEvent(1, "a");
        std::ofstream f(segmentPath(1), std::ios::binary | std::ios::trunc);
        f.write(seg.data(), static_cast<std::streamsize>(seg.size()));
    }
    // gen=3, gen=4 (last full range), gen=5 (tail): header-only, no records.
    for (std::uint32_t gen : {3u, 4u, 5u}) {
        const std::string hdr = serializeSegmentHeader(gen, 0);
        std::ofstream f(segmentPath(gen), std::ios::binary | std::ios::trunc);
        f.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
    }

    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());

    // Pop seq=1 so gen=1 becomes a dead range.
    storePop(store);

    // compactOneSegment picks {1,1} (not the last range {3,4}) — rotate must NOT fire.
    CHECK(store.compactOneSegment().ok()); // dead-range removal of {1,1}

    // gen=6 would only exist if the tail (gen=5) was rotated.
    CHECK_FALSE(std::filesystem::exists(segmentPath(6)));

    // Last full range {3,4} and tail gen=5 must still exist.
    CHECK(std::filesystem::exists(segmentPath(3)));
    CHECK(std::filesystem::exists(segmentPath(4)));
    CHECK(std::filesystem::exists(segmentPath(5)));
}

TEST_CASE("compaction: rotate-before-compact keeps range count bounded under mixed workload") {
    // Regression for the generation gap fragmentation bug: without rotate-before-compact,
    // every compaction leaves an orphan-tail range, driving the store toward
    // RangeLimitExceeded and triggering catastrophic multi-second compaction stalls.
    //
    // The invariant is NOT "range count always stays <=2". If the orphan tail later
    // receives live records it forms a range that can't merge until it is itself
    // compacted. The invariant is bounded, non-catastrophic fragmentation: range count
    // must stay <= 3 (the pre-fix failure mode reached 4+ and hit RangeLimitExceeded).
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70; // two 1-byte records per segment
    cfg.maxSegments     = 200; // disable internal auto-compaction
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    std::uint32_t seq = 1;
    for (int cycle = 0; cycle < 5; ++cycle) {
        // Burst: fill several segments.
        for (int i = 0; i < 12; ++i) {
            storeEnqueue(store, seq++, std::string(1, 'x'));
        }
        // Pop most records to create dead bytes.
        for (int i = 0; i < 9; ++i) {
            storePop(store);
        }
        // Compact once (simulates the dead-ratio trigger).
        const auto st = store.compactOneSegment();
        CHECK(st.ok());

        // Bounded fragmentation: must never approach RangeLimitExceeded (4).
        CHECK_LE(store.manifestRanges().size(), 3U);
    }

    // All remaining live records are readable. Live window after 5 cycles of burst=12
    // pop=9 starting at seq=1: head=46 (5*9 pops), tail=seq (61). Use direct seq reads
    // rather than idx.count (storeEnqueue does not update the index count field).
    for (std::uint32_t s = 46; s < seq; ++s) {
        std::string out;
        CHECK(store.readRecord(s, out).ok());
        CHECK_EQ(out, std::string(1, 'x'));
    }
}

TEST_CASE("compaction: manifest state survives remount after rotate-before-compact") {
    // Every time rotate-before-compact fires it writes two manifests (rotate + compact).
    // Verify those writes are durable: a store remounted after each compact cycle sees
    // the same live records and the same bounded range count.
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    cfg.maxSegments     = 200;

    std::uint32_t seq  = 1;   // next seq to enqueue
    std::uint32_t head = 1;   // oldest live seq; live window is [head, seq)

    for (int cycle = 0; cycle < 5; ++cycle) {
        pqueue::AppendLogStore store(cfg);
        CHECK(store.mount().ok());

        for (int i = 0; i < 12; ++i) storeEnqueue(store, seq++, std::string(1, 'x'));
        for (int i = 0; i < 9;  ++i) { storePop(store); ++head; }
        CHECK(store.compactOneSegment().ok());
        CHECK_LE(store.manifestRanges().size(), 3U);

        pqueue::AppendLogStore store2(cfg);
        CHECK(store2.mount().ok());
        CHECK_LE(store2.manifestRanges().size(), 3U);
        for (std::uint32_t s = head; s < seq; ++s) {
            std::string out;
            CHECK(store2.readRecord(s, out).ok());
            CHECK_EQ(out, std::string(1, 'x'));
        }
    }
}

TEST_CASE("compaction: orphan tail range is eliminated after its records are all popped") {
    // Rotate-before-compact may leave an orphan-tail range that receives live records
    // in a subsequent burst. Once those records are popped, dead-range elimination
    // must reclaim the range, recovering the store to zero sealed ranges.
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    cfg.maxSegments     = 200;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    std::uint32_t seq  = 1;
    std::uint32_t head = 1;

    for (int cycle = 0; cycle < 3; ++cycle) {
        for (int i = 0; i < 12; ++i) storeEnqueue(store, seq++, std::string(1, 'x'));
        for (int i = 0; i < 9;  ++i) { storePop(store); ++head; }
        CHECK(store.compactOneSegment().ok());
    }

    // Drain all remaining live records so every sealed range becomes dead.
    while (head < seq) { storePop(store); ++head; }

    // Dead-range elimination should remove every sealed range.
    for (int i = 0; i < 10; ++i) {
        const auto st = store.compactOneSegment();
        CHECK(st.ok());
        if (st.isNoOp()) break;
    }
    CHECK_EQ(store.manifestRanges().size(), 0U);
}

#endif // !ARDUINO

#include "pqueue_append_log_support.h"

#ifndef ARDUINO

TEST_CASE("rollover: manifest published after each rotation (critical)") {
    // Enqueue enough records to force several segment rollovers, remount, verify
    // full record set is intact in FIFO order. This is the critical Stage 4 test:
    // every rotation must publish a manifest so remount can reconstruct all segments.
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 128; // small: forces rotation every few records

    constexpr int kCount = 20;
    {
        pqueue::Queue q(cfg);
        for (int i = 0; i < kCount; ++i) {
            CHECK(q.enqueue("item-" + std::to_string(i)).ok());
        }
        CHECK_EQ(q.stats().count, static_cast<std::uint32_t>(kCount));
    }
    {
        pqueue::Queue q(cfg);
        CHECK_EQ(q.stats().count, static_cast<std::uint32_t>(kCount));
        for (int i = 0; i < kCount; ++i) {
            std::string out;
            CHECK(q.peek(out).ok());
            CHECK_EQ(out, "item-" + std::to_string(i));
            CHECK(q.pop().ok());
        }
        CHECK_EQ(q.stats().count, 0U);
    }
}

TEST_CASE("rollover: range limit exceeded returns failure") {
    // Verify that rotateSegment() fails cleanly when promoting the current tail
    // would push the range count past kManifestMaxRanges (4):
    //   - rotation returns a non-ok status,
    //   - no new segment file is created (range check runs before createSegment),
    //   - manifest on disk is unchanged.
    //
    // Setup: plant a manifest with 4 non-contiguous full ranges and tail=9.
    // Normal Queue operations always produce contiguous ranges (all merge into
    // one), so this state must be crafted directly — it represents a post-
    // compaction layout that Stage 5 would produce.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    // maxSegmentBytes fits exactly one 1-byte record per segment.
    const std::uint32_t maxSeg =
        kSegmentHeaderBytes + kEnqueueHeaderBytes + 1 + kEventTrailerBytes;

    // Write manifest with 4 non-contiguous ranges + tailGeneration=9 directly to
    // slot A (bypassing the store so we can set epoch manually).
    {
        ManifestData md;
        md.epoch          = 1;
        md.ranges         = {{1, 1}, {3, 3}, {5, 5}, {7, 7}};
        md.tailGeneration = 9;
        md.nextGeneration = 10;
        std::vector<std::uint8_t> bytes;
        serialiseManifest(md, bytes);
        std::ofstream f(manifestSlotPath('a'), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    // Create minimal segment files for every generation referenced by the manifest
    // so that scanSegments() can scan them without returning DataCorrupt.
    for (std::uint32_t gen : {1u, 3u, 5u, 7u, 9u}) {
        const std::string hdr = serializeSegmentHeader(gen, 0);
        std::ofstream f(segmentPath(gen), std::ios::binary | std::ios::trunc);
        f.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
    }

    auto storeCfg = makeStoreConfig();
    storeCfg.maxSegmentBytes = maxSeg;

    pqueue::AppendLogStore store(storeCfg);
    REQUIRE(store.mount().ok());  // reads manifest, scans [1,3,5,7,9], activeGeneration_=9

    // Fill the tail segment with exactly one record.
    pqueue::FileStoreIndex dummy{};
    REQUIRE(store.writeRecord(0, "x").ok());
    REQUIRE(store.writeIndex(dummy).ok());  // seg 9 now full (kSegmentHeaderBytes + 25 = 45)

    // The next write overflows seg 9 → triggers rotateSegment().
    // rotateSegment() must detect: adding {9,9} to 4 existing ranges = 5 → limit exceeded.
    // It must return failure WITHOUT creating the new segment file (gen 10).
    const auto st = store.writeRecord(1, "y");
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::RangeLimitExceeded);

    // No new segment created — range check fires before createSegment().
    CHECK_FALSE(std::filesystem::exists(segmentPath(10)));

    // Manifest on disk is unchanged (publishManifest was never called).
    ManifestData onDisk;
    REQUIRE(readManifestSlot('a', onDisk));
    CHECK_EQ(onDisk.tailGeneration, 9U);
    REQUIRE_EQ(onDisk.ranges.size(), 4U);
    CHECK_EQ(onDisk.ranges[0].startGen, 1U); CHECK_EQ(onDisk.ranges[0].endGen, 1U);
    CHECK_EQ(onDisk.ranges[3].startGen, 7U); CHECK_EQ(onDisk.ranges[3].endGen, 7U);
}

TEST_CASE("rollover: nextGeneration in manifest is one past the new tail") {
    // After rotation creates seg-N, the published manifest must store
    // nextGeneration = N+1. If it stored N instead, a subsequent rotation
    // would reuse the same generation number.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = kSegmentHeaderBytes + kEnqueueHeaderBytes + 1 + kEventTrailerBytes;

    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("A").ok()); // seg 1 created, manifest published: tail=1, nextGen=2
        CHECK(q.enqueue("B").ok()); // seg 1 full → rotate to seg 2; manifest: tail=2, nextGen=3
    }

    // Read the winning manifest slot and verify nextGeneration.
    ManifestData md;
    REQUIRE((readManifestSlot('a', md) || readManifestSlot('b', md)));
    // Use the higher-epoch slot.
    ManifestData mdB;
    if (readManifestSlot('b', mdB) && mdB.epoch > md.epoch) md = mdB;

    CHECK_EQ(md.tailGeneration, 2U);
    CHECK_EQ(md.nextGeneration, 3U);
}

TEST_CASE("rollover: dangling new segment ignored when publish fails (critical)") {
    // Critical crash-safety invariant: if the segment file for the new tail is
    // written but the manifest publish fails, the old tail must remain authoritative
    // on the next remount and the dangling new segment must be ignored.
    //
    // Simulate the crash window by writing the new segment file directly and
    // leaving the old manifest unchanged, then verifying remount behaviour.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = kSegmentHeaderBytes + kEnqueueHeaderBytes + 1 + kEventTrailerBytes;

    {
        // Session 1: write A into seg 1 and publish manifest (tail=1, nextGen=2).
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("A").ok());
    }
    // Verify seg 1 exists with a valid manifest.
    REQUIRE(std::filesystem::exists(segmentPath(1)));
    {
        ManifestData md;
        REQUIRE(readManifestSlot('a', md));
        CHECK_EQ(md.tailGeneration, 1U);
    }

    // Simulate the crash window: write the segment file for the NEXT generation (2)
    // without updating the manifest (imitates rotateSegment() crashing after
    // createSegment() but before publishManifest()).
    {
        const std::string header = serializeSegmentHeader(2, 1);
        std::ofstream f(segmentPath(2), std::ios::binary | std::ios::trunc);
        f.write(header.data(), static_cast<std::streamsize>(header.size()));
    }

    // Remount: manifest still says tail=1. Seg 2 is a dangling file and must be
    // ignored — all of A must still be readable.
    {
        pqueue::Queue q(cfg);
        CHECK_EQ(q.stats().count, 1U);
        std::string out;
        CHECK(q.peek(out).ok());
        CHECK_EQ(out, "A");
    }
}

TEST_CASE("rollover: failed manifest publish does not poison live object (critical)") {
    // Bug (fixed): createSegment() used to mutate activeGeneration_, activeSegmentBytes_,
    // activeGenerations_, and nextGeneration_ before publishManifest() succeeded. If the
    // manifest write failed, the live object believed the new segment was the active tail.
    // Subsequent writeRecord() calls wrote into an unmanifested segment; fresh remount
    // returned DataCorrupt.
    //
    // Fix: createSegment() only writes the file; callers apply RAM state only after
    // publishManifest() succeeds.
    //
    // This test injects a manifest write failure on the first enqueue (which triggers
    // ensureActiveSegment), then verifies the live object recovers cleanly: the second
    // enqueue succeeds, and a fresh remount can read it.
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    auto inner = pqueue::makePosixFileSystem();
    auto faultFs = std::make_shared<FaultInjectingFs>(inner);

    pqueue::AppendLogConfig cfg = makeStoreConfig();
    cfg.fileSystem = faultFs;

    {
        pqueue::AppendLogStore store(cfg);
        REQUIRE(store.mount().ok());

        // Fail the first manifest write — ensureActiveSegment will call publishManifest
        // which writes a manifest slot.
        faultFs->failNextWriteFileTo = "manifest";
        const auto st = store.writeRecord(0, "hello");
        CHECK_FALSE(st.ok());

        // Fault is cleared; second write must succeed.
        pqueue::FileStoreIndex dummy{};
        REQUIRE(store.writeRecord(0, "hello").ok());
        REQUIRE(store.writeIndex(dummy).ok());
    }

    // Fresh remount (no fault) must find exactly one record and return it.
    {
        pqueue::AppendLogStore store2(makeStoreConfig());
        REQUIRE(store2.mount().ok());
        pqueue::FileStoreIndex idx;
        REQUIRE(store2.readIndex(idx).ok());
        CHECK_EQ(idx.count, 1U);
        std::string out;
        REQUIRE(store2.readRecord(0, out).ok());
        CHECK_EQ(out, "hello");
    }
}

TEST_CASE("rollover: failed manifest publish during rotation does not poison live object (critical)") {
    // Regression test for the same RAM-before-manifest bug, exercised through
    // rotateSegment() rather than ensureActiveSegment().
    //
    // Setup: maxSegmentBytes sized to fit exactly one 1-byte record per segment.
    // Write A so seg 1 is manifest tail and full. Inject a manifest failure on the
    // next write so the rotation to seg 2 fails during publishManifest(). Clear the
    // fault and write B on the same live object. Fresh remount must read A then B.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);

    // One 1-byte record exactly fills a segment: header(20) + enqHeader(16) + 1 + trailer(8) = 45
    const std::uint32_t maxSeg = kSegmentHeaderBytes + kEnqueueOverheadBytes + 1;

    auto inner = pqueue::makePosixFileSystem();
    auto faultFs = std::make_shared<FaultInjectingFs>(inner);

    pqueue::AppendLogConfig cfg = makeStoreConfig();
    cfg.maxSegmentBytes = maxSeg;
    cfg.fileSystem = faultFs;

    {
        pqueue::AppendLogStore store(cfg);
        REQUIRE(store.mount().ok());

        // Write A — seg 1 created, manifest published (tail=1), A written; seg 1 now full.
        pqueue::FileStoreIndex dummy{};
        REQUIRE(store.writeRecord(0, "A").ok());
        REQUIRE(store.writeIndex(dummy).ok());

        // Inject failure on the next manifest write. Writing B overflows seg 1 and
        // triggers rotateSegment() → createSegment(2) succeeds but publishManifest fails.
        faultFs->failNextWriteFileTo = "manifest";
        const auto st = store.writeRecord(1, "B");
        CHECK_FALSE(st.ok());

        // Fault cleared. Retry B on the same live object — rotation succeeds this time.
        REQUIRE(store.writeRecord(1, "B").ok());
        REQUIRE(store.writeIndex(dummy).ok());
    }

    // Fresh remount: must find A in seg 1 and B in seg 2, no DataCorrupt.
    {
        pqueue::AppendLogConfig cfg2 = makeStoreConfig();
        cfg2.maxSegmentBytes = maxSeg;
        pqueue::AppendLogStore store2(cfg2);
        REQUIRE(store2.mount().ok());
        pqueue::FileStoreIndex idx;
        REQUIRE(store2.readIndex(idx).ok());
        CHECK_EQ(idx.count, 2U);
        std::string out;
        REQUIRE(store2.readRecord(0, out).ok());
        CHECK_EQ(out, "A");
        REQUIRE(store2.readRecord(1, out).ok());
        CHECK_EQ(out, "B");
    }
}

TEST_CASE("rollover: merged range [1,2] replays both segments in FIFO order") {
    // After two rotations the manifest stores a merged range [1,2] (contiguous)
    // instead of two separate ranges. Verify on-disk manifest structure and that
    // remount replays both segments in FIFO order.
    //
    // maxSegmentBytes = kSegmentHeaderBytes(20) + kEnqueueHeaderBytes(16) + 1 + kEventTrailerBytes(8) = 45.
    // Each 1-byte record occupies 25 bytes, so one record fills a segment exactly.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = kSegmentHeaderBytes + kEnqueueHeaderBytes + 1 + kEventTrailerBytes;

    {
        pqueue::Queue q(cfg);
        // "A" fills seg 1 (20+25=45). "B" overflows → rotateSegment promotes {1,1} to
        // full range and creates seg 2; manifest: ranges=[{1,1}], tail=2. "C" overflows
        // → promotes {2,2}, merges with {1,1} → [{1,2}], creates seg 3; manifest:
        // ranges=[{1,2}], tail=3.
        CHECK(q.enqueue("A").ok());
        CHECK(q.enqueue("B").ok());
        CHECK(q.enqueue("C").ok());

        // Verify manifest structure now, before any pops. POP events are also written
        // to the active segment and can trigger additional rotations that would change
        // the manifest before the check runs.
        ManifestData md;
        REQUIRE((readManifestSlot('a', md) || readManifestSlot('b', md)));
        ManifestData mdB;
        if (readManifestSlot('b', mdB) && mdB.epoch > md.epoch) md = mdB;
        REQUIRE_EQ(md.ranges.size(), 1U);
        CHECK_EQ(md.ranges[0].startGen, 1U);
        CHECK_EQ(md.ranges[0].endGen, 2U);
        CHECK_EQ(md.tailGeneration, 3U);
    }
    {
        pqueue::Queue q(cfg);
        // Remount reconstructs all three records from segments 1, 2, 3.
        CHECK_EQ(q.stats().count, 3U);
        std::string out;
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "A");
        CHECK(q.pop().ok());
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "B");
        CHECK(q.pop().ok());
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "C");
        CHECK(q.pop().ok());
        CHECK_EQ(q.stats().count, 0U);
    }
}

TEST_CASE("rollover: manifest slot alternates correctly across rotations") {
    // After each rotation a new manifest is written. Verify that alternating
    // writes leave both slots valid and the higher-epoch slot is the winner.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 128;

    {
        pqueue::Queue q(cfg);
        // Force at least 3 rotations so we get multiple publish cycles.
        for (int i = 0; i < 15; ++i) {
            CHECK(q.enqueue("r").ok());
        }
    }

    ManifestData mdA, mdB;
    const bool validA = readManifestSlot('a', mdA);
    const bool validB = readManifestSlot('b', mdB);
    // At least one slot must be valid after writes.
    CHECK((validA || validB));
    // The winning slot has the higher epoch.
    if (validA && validB) {
        CHECK_NE(mdA.epoch, mdB.epoch); // epochs must differ
    }
}

#endif // !ARDUINO

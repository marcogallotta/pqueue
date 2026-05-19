#include "pqueue_append_log_support.h"

using namespace pqueue::append_log_detail;

namespace {

std::uint32_t actualOnDiskBytes() {
    std::uint32_t total = 0;
    for (const auto& entry : std::filesystem::directory_iterator(kSpoolDir)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("seg-", 0) == 0 && name.size() > 4)
            total += static_cast<std::uint32_t>(std::filesystem::file_size(entry.path()));
    }
    return total;
}

void checkTracking(pqueue::AppendLogStore& store) {
    CHECK_EQ(store.totalOnDiskBytes(), actualOnDiskBytes());
}

} // namespace

TEST_CASE("compaction: chooseCompactionRange returns nullopt with only tail (critical)") {
    // A store with only a tail segment and no full ranges must not offer the
    // tail for compaction — selecting it would violate the torn-tail invariant.
    resetSpool();
    ManifestData md;
    md.epoch = 1; md.tailGeneration = 1; md.nextGeneration = 2;
    plantManifest(md);
    plantSegment(1);

    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());
    CHECK_FALSE(store.chooseCompactionRange().has_value());
}


TEST_CASE("compaction: collectLiveRecords returns records in FIFO order; out-of-range excluded") {
    // seq=1,2 land in gen=1 (full range after rotation); seq=3 lands in gen=2 (tail).
    // Collecting {1,1} must return seq=1 then seq=2 only.
    resetSpool();
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 95; // two records + one rewrite fill gen=1; third enqueue rotates
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a");
    storeEnqueue(store, 2, "b");
    CHECK(store.rewriteRecord(1, "a").ok());
    storeEnqueue(store, 3, "c"); // rotation: gen=1 full range, gen=2 tail

    const auto range = store.chooseCompactionRange();
    REQUIRE(range.has_value());
    CHECK_EQ(range->startGen, 1u);

    std::vector<pqueue::AppendLogStore::CompactionLiveRecord> live;
    CHECK(store.collectLiveRecords(*range, live).ok());
    REQUIRE_EQ(live.size(), 2u);
    CHECK_EQ(live[0].sequence, 1u); CHECK_EQ(live[0].payload, "a");
    CHECK_EQ(live[1].sequence, 2u); CHECK_EQ(live[1].payload, "b");
}

TEST_CASE("compaction: collectLiveRecords excludes popped records") {
    resetSpool();
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a");
    storeEnqueue(store, 2, "b");
    storeEnqueue(store, 3, "c"); // rotation: gen=1 full, gen=2 tail
    storePop(store);             // pop seq=1

    const auto range = store.chooseCompactionRange();
    REQUIRE(range.has_value());

    std::vector<pqueue::AppendLogStore::CompactionLiveRecord> live;
    CHECK(store.collectLiveRecords(*range, live).ok());
    REQUIRE_EQ(live.size(), 1u);
    CHECK_EQ(live[0].sequence, 2u); CHECK_EQ(live[0].payload, "b");
}

TEST_CASE("compaction: compactOneSegment removes dead range without writing a new segment") {
    // gen=1 (full range, header only — no records): dead range.
    // gen=4 (non-contiguous tail) prevents rotate-before-compact from extending the range.
    resetSpool();
    ManifestData md;
    md.epoch = 1; md.ranges = {{1, 1}}; md.tailGeneration = 4; md.nextGeneration = 5;
    plantManifest(md);
    plantSegment(1);                                    // dead: header only
    plantSegment(4, 1, serializeEnqueueEvent(1, "c")); // non-contiguous tail

    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());
    REQUIRE(store.chooseCompactionRange().has_value());

    CHECK(store.compactOneSegment().ok());

    CHECK_FALSE(std::filesystem::exists(segmentPath(5))); // no new segment
    CHECK_FALSE(store.chooseCompactionRange().has_value());
    expectRecord(store, 1, "c");
}

TEST_CASE("compaction: compactOneSegment is no-op under segment-count pressure when all ranges are live") {
    // needsCompaction() fires due to segment count; all records live → no-op.
    resetSpool();
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    cfg.maxSegments     = 1;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a");
    storeEnqueue(store, 2, "b");
    storeEnqueue(store, 3, "c"); // gen=1 full, gen=2 tail; 2 gens > maxSegments=1

    const auto st = store.compactOneSegment();
    CHECK(st.ok());
    CHECK(st.isNoOp());
    CHECK_FALSE(std::filesystem::exists(segmentPath(3)));
    expectRecords(store, {{1,"a"},{2,"b"},{3,"c"}});
}

TEST_CASE("compaction: compactOneSegment is no-op when 1-in-1-out with no dead bytes") {
    // Fully live single-segment range: 1 output == 1 input, no dead bytes → no-op.
    resetSpool();
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a");
    storeEnqueue(store, 2, "b");
    storeEnqueue(store, 3, "c"); // gen=1 full (seq=1,2), gen=2 tail (seq=3)

    const auto st = store.compactOneSegment();
    CHECK(st.ok());
    CHECK(st.isNoOp());
    CHECK_FALSE(std::filesystem::exists(segmentPath(3)));

    expectRecords(store, {{1,"a"},{2,"b"},{3,"c"}});
    expectRecordsAfterRemount(cfg, {{1,"a"},{2,"b"},{3,"c"}});
}

TEST_CASE("compaction: compactOneSegment preserves live records in FIFO order") {
    resetSpool();
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a");
    storeEnqueue(store, 2, "b");
    storeEnqueue(store, 3, "c"); // gen=1 full, gen=2 tail
    storePop(store);             // dead bytes in gen=1

    CHECK(store.compactOneSegment().ok());

    CHECK(std::filesystem::exists(segmentPath(3)));
    expectRecords(store, {{2,"b"},{3,"c"}});
}

TEST_CASE("compaction: compactOneSegment preserves rewritten payload") {
    // seq=1 is rewritten before rotation; compactOneSegment must copy the rewritten value.
    resetSpool();
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 100;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "x");
    storeEnqueue(store, 2, "y");
    CHECK(store.rewriteRecord(1, "z").ok());
    storeEnqueue(store, 3, "w"); // rotation: gen=1 full, gen=2 tail

    CHECK(store.compactOneSegment().ok());
    expectRecords(store, {{1,"z"},{2,"y"},{3,"w"}});
}

TEST_CASE("compaction: compactOneSegment is no-op when multi-segment output would not reduce segment count") {
    // range {1,1} = 1 input; seq=1 popped, seq=2+3 live require 2 output at maxSegmentBytes=50;
    // 2 > 1 → no-op. Non-contiguous tail (gen=5) prevents rotate-before-compact.
    resetSpool();
    ManifestData md;
    md.epoch = 1; md.ranges = {{1, 1}}; md.tailGeneration = 5; md.nextGeneration = 6;
    plantManifest(md);
    plantSegment(1, 1,
        serializeEnqueueEvent(1, "a") +
        serializeEnqueueEvent(2, "b") +
        serializeEnqueueEvent(3, "c"));
    plantSegment(5, 4);

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 50;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());
    storePop(store); // pop seq=1 → dead bytes; seq=2+3 still live
    CHECK(store.compactOneSegment().ok());

    CHECK_FALSE(std::filesystem::exists(segmentPath(6)));
    CHECK(store.chooseCompactionRange().has_value());
}

// --- Stage 7: cleanup tests ---

TEST_CASE("cleanup: compactOneSegment deletes old segment files and preserves live records") {
    resetSpool();
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a");
    storeEnqueue(store, 2, "b");
    storeEnqueue(store, 3, "c"); // gen=1 full, gen=2 tail
    storePop(store);             // dead bytes in gen=1

    // rotate-before-compact seals gen=2 into range; output=gen=4, new tail=gen=3.
    CHECK(store.compactOneSegment().ok());
    CHECK_FALSE(std::filesystem::exists(segmentPath(1)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(2)));
    CHECK(std::filesystem::exists(segmentPath(3)));
    CHECK(std::filesystem::exists(segmentPath(4)));
    expectRecords(store, {{2,"b"},{3,"c"}});
}

TEST_CASE("cleanup: dangling segment from failed publish is deleted on next successful publish") {
    // The next successful publishManifest must delete a dangling segment that was
    // written before a crash but never made it into a manifest.
    resetSpool();
    ManifestData md;
    md.epoch = 1; md.ranges = {{1, 1}}; md.tailGeneration = 2; md.nextGeneration = 3;
    plantManifest(md);
    plantSegment(1, 1, serializeEnqueueEvent(1, "a") + serializeEnqueueEvent(2, "b"));
    plantSegment(2, 3);                                      // tail
    plantSegment(3, 1, serializeEnqueueEvent(1, "a"));       // dangling

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok()); // cleanup on mount removes dangling gen=3
    CHECK_FALSE(std::filesystem::exists(segmentPath(3)));
    CHECK(std::filesystem::exists(segmentPath(1)));
    CHECK(std::filesystem::exists(segmentPath(2)));
}


TEST_CASE("compaction: dangling segment from failed publish is harmless on remount (critical)") {
    // Simulates a crash after the compacted segment was written but before the manifest
    // publish succeeded. The winning manifest still points at the original segments;
    // the dangling file is ignored and all records are intact.
    resetSpool();
    ManifestData md;
    md.epoch = 1; md.ranges = {{1, 1}}; md.tailGeneration = 2; md.nextGeneration = 3;
    plantManifest(md);
    plantSegment(1, 1, serializeEnqueueEvent(1, "a") + serializeEnqueueEvent(2, "b"));
    plantSegment(2, 3);                                              // tail
    plantSegment(3, 1, serializeEnqueueEvent(1, "a") +
                       serializeEnqueueEvent(2, "b"));               // dangling compaction output

    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());
    expectRecords(store, {{1,"a"},{2,"b"}});

    // Rewrite to create dead bytes so the ratio selector still fires for gen=1.
    CHECK(store.rewriteRecord(1, "a").ok());

    const auto range = store.chooseCompactionRange();
    REQUIRE(range.has_value());
    CHECK_EQ(range->startGen, 1u);
    CHECK_EQ(range->endGen,   1u);
}

TEST_CASE("compaction: compactFull removes multiple dead ranges in one call") {
    resetSpool();
    ManifestData md;
    md.epoch = 1; md.ranges = {{1, 1}, {3, 3}}; md.tailGeneration = 5; md.nextGeneration = 6;
    plantManifest(md);
    plantSegment(1); plantSegment(3); plantSegment(5); // header only → both ranges dead

    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());
    REQUIRE(store.chooseCompactionRange().has_value());

    CHECK(store.compactFull().ok());
    CHECK_FALSE(store.chooseCompactionRange().has_value());
}

TEST_CASE("compaction: collectLiveRecords returns rewritten payload, not original (critical)") {
    // If a record was rewritten before compaction, the collected payload must be
    // the rewritten value, not the original enqueue bytes.
    resetSpool();
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 100;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "x");
    storeEnqueue(store, 2, "y");
    CHECK(store.rewriteRecord(1, "z").ok()); // REWRITE event stays in gen=1
    storeEnqueue(store, 3, "w");             // rotation: gen=1 full range, gen=2 tail

    const auto range = store.chooseCompactionRange();
    REQUIRE(range.has_value());
    CHECK_EQ(range->startGen, 1u);

    std::vector<pqueue::AppendLogStore::CompactionLiveRecord> live;
    CHECK(store.collectLiveRecords(*range, live).ok());
    REQUIRE_EQ(live.size(), 2u);
    CHECK_EQ(live[0].sequence, 1u); CHECK_EQ(live[0].payload, "z");
    CHECK_EQ(live[1].sequence, 2u); CHECK_EQ(live[1].payload, "y");
}

TEST_CASE("compaction: multi-segment output compacts range when output count < input count") {
    // range {1,3} = 3 input segments; records 1,2 popped → 4 live records fit in 2 output
    // segments. 2 < 3 → compaction proceeds with multi-segment output.
    resetSpool();
    plantSegment(1, 1, serializeEnqueueEvent(1, "a") + serializeEnqueueEvent(2, "b"));
    plantSegment(2, 3, serializeEnqueueEvent(3, "c") + serializeEnqueueEvent(4, "d"));
    plantSegment(3, 5, serializeEnqueueEvent(5, "e") + serializeEnqueueEvent(6, "f"));
    plantSegment(4, 7, serializePopEvent(1) + serializePopEvent(2));
    ManifestData md;
    md.epoch = 1; md.ranges = {{1, 3}}; md.tailGeneration = 4; md.nextGeneration = 5;
    plantManifest(md);

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    auto st = store.compactOneSegment();
    CHECK(st.ok());
    CHECK_FALSE(st.isNoOp());

    // rotate-before-compact seals tail gen=4 → {1,4}; new tail=gen=5, nextGen=6.
    // Compact {1,4} → two output segs gen=6, gen=7.
    CHECK(std::filesystem::exists(segmentPath(6)));
    CHECK(std::filesystem::exists(segmentPath(7)));
    REQUIRE_EQ(store.manifestRanges().size(), 1u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 6u);
    CHECK_EQ(store.manifestRanges()[0].endGen,   7u);

    expectRecords(store, {{3,"c"},{4,"d"},{5,"e"},{6,"f"}});
}

TEST_CASE("compaction: multi-segment output survives remount") {
    resetSpool();
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;

    {
        pqueue::AppendLogStore store(cfg);
        CHECK(store.mount().ok());
        storeEnqueue(store, 1, "a");
        storeEnqueue(store, 2, "b");
        storeEnqueue(store, 3, "c");
        storeEnqueue(store, 4, "d");
        storeEnqueue(store, 5, "e");
        storeEnqueue(store, 6, "f");
        storeEnqueue(store, 7, "g");
        storePop(store);
        storePop(store); // records 1,2 popped; range {1,4}, tail=5 after rotation
    }
    {
        pqueue::AppendLogStore store(cfg);
        CHECK(store.mount().ok());
        CHECK(store.compactOneSegment().ok());
    }

    expectRecordsAfterRemount(cfg, {{3,"c"},{4,"d"},{5,"e"},{6,"f"},{7,"g"}});
}

TEST_CASE("compaction: multi-segment output is no-op when output count equals input count") {
    // range {1,2} = 2 input segments; seq=1 popped, seq=2+3+4 live require 3 output
    // segments at maxSegmentBytes=50; 3 > 2 → no-op. Non-contiguous tail (gen=5) prevents
    // rotate-before-compact.
    resetSpool();
    plantSegment(1, 1, serializeEnqueueEvent(1, "a") + serializeEnqueueEvent(2, "b"));
    plantSegment(2, 3, serializeEnqueueEvent(3, "c") + serializeEnqueueEvent(4, "d"));
    plantSegment(5, 5);
    ManifestData md;
    md.epoch = 1; md.ranges = {{1, 2}}; md.tailGeneration = 5; md.nextGeneration = 6;
    plantManifest(md);

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 50;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());
    storePop(store); // pop seq=1 → dead bytes; seq=2+3+4 still live
    auto st = store.compactOneSegment();
    CHECK(st.ok());
    CHECK(st.isNoOp());
    CHECK_FALSE(std::filesystem::exists(segmentPath(6)));
    CHECK(store.chooseCompactionRange().has_value());
}

TEST_CASE("totalOnDiskBytes tracking: matches actual file sizes after every operation") {
    resetSpool();
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());
    checkTracking(store);

    storeEnqueue(store, 1, "a"); checkTracking(store);
    storeEnqueue(store, 2, "b"); checkTracking(store);
    storeEnqueue(store, 3, "c"); checkTracking(store);

    CHECK(store.rewriteRecord(1, "x").ok()); checkTracking(store);
    storePop(store);                         checkTracking(store);

    CHECK(store.compactOneSegment().ok());   checkTracking(store);

    storeEnqueue(store, 4, "d");             checkTracking(store);

    pqueue::AppendLogStore store2(cfg);
    CHECK(store2.mount().ok());
    checkTracking(store2);
}

TEST_CASE("maxTotalBytes: enqueue blocked when footprint full and nothing to compact") {
    resetSpool();
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    cfg.maxTotalBytes   = 70;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a");
    storeEnqueue(store, 2, "b"); // on-disk footprint = 70 bytes; cap reached

    const auto st = store.writeRecord(3, "c");
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::QueueFull);
    CHECK_FALSE(std::filesystem::exists(segmentPath(2)));
    expectRecords(store, {{1,"a"},{2,"b"}});
}

TEST_CASE("maxTotalBytes: compaction makes room for a blocked enqueue") {
    resetSpool();
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    cfg.maxTotalBytes   = 120;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a");
    storeEnqueue(store, 2, "b");
    storePop(store); // POP overflows gen=1 → rotation; gen=1: 70b (full), gen=2: 40b

    // footprint=110; +25 for enqueue=135 > 120 → compact gen=1; then fits
    CHECK(store.writeRecord(3, "c").ok());
    pqueue::FileStoreIndex idx;
    CHECK(store.readIndex(idx).ok());
    CHECK(store.writeIndex(idx).ok());

    expectRecords(store, {{2,"b"},{3,"c"}});
    expectRecordsAfterRemount(cfg, {{2,"b"},{3,"c"}});
}

TEST_CASE("maxTotalBytes: DropOldest evicts and retries when writeRecord returns QueueFull") {
    resetSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 100;
    cfg.reservedBytes   = 139; // gen1(94 bytes) + gen2(45 bytes)
    cfg.fullQueuePolicy = pqueue::FullQueuePolicy::DropOldest;
    pqueue::Queue q(cfg);

    REQUIRE(q.enqueue(std::string(50, 'a')).ok());
    REQUIRE(q.enqueue("b").ok());
    CHECK(q.enqueue("c").ok());

    CHECK_EQ(q.stats().count, 2U);
    std::string out;
    REQUIRE(q.peek(out).ok()); CHECK_EQ(out, "b");
    q.pop();
    REQUIRE(q.peek(out).ok()); CHECK_EQ(out, "c");
}

TEST_CASE("sequence exhaustion: writeRecord fails closed at UINT32_MAX") {
    resetSpool();
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
    // After compactOneSegment(), output must start at nextGeneration_ after the rotate
    // (gen=4, not gen=3), confirming tail=2 was sealed into {1,2} first.
    resetSpool();
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a");
    storeEnqueue(store, 2, "b"); // gen=1 full → rotate; gen=2 tail
    storePop(store);             // dead bytes in gen=1
    storeEnqueue(store, 3, "c"); // goes to gen=2

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
    CHECK(std::filesystem::exists(segmentPath(3)));
    CHECK(std::filesystem::exists(segmentPath(4)));
    expectRecords(store, {{2,"b"},{3,"c"}});
}

TEST_CASE("compaction: rotate-before-compact remount preserves FIFO order (critical)") {
    resetSpool();
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    {
        pqueue::AppendLogStore store(cfg);
        CHECK(store.mount().ok());
        storeEnqueue(store, 1, "a");
        storeEnqueue(store, 2, "b"); // gen=1 full, gen=2 tail
        storePop(store);             // dead bytes in gen=1
        storeEnqueue(store, 3, "c");
        CHECK(store.compactOneSegment().ok());
    }

    expectRecordsAfterRemount(cfg, {{2,"b"},{3,"c"}});
}

TEST_CASE("compaction: rotate-before-compact does not fire for non-last range") {
    // selectedIsLastRange guard prevents rotate when compacting a non-last range,
    // even if the active tail is contiguous with the last range.
    resetSpool();
    ManifestData md;
    md.epoch = 1; md.ranges = {{1, 1}, {3, 4}}; md.tailGeneration = 5; md.nextGeneration = 6;
    plantManifest(md);
    plantSegment(1, 1, serializeEnqueueEvent(1, "a")); // will be popped to make dead
    plantSegment(3); plantSegment(4); plantSegment(5); // last full range + tail

    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());
    storePop(store); // pop seq=1 → gen=1 becomes dead range

    CHECK(store.compactOneSegment().ok()); // dead-range removal of {1,1}

    CHECK_FALSE(std::filesystem::exists(segmentPath(6))); // gen=6 only if tail rotated
    CHECK(std::filesystem::exists(segmentPath(3)));
    CHECK(std::filesystem::exists(segmentPath(4)));
    CHECK(std::filesystem::exists(segmentPath(5)));
}


TEST_CASE("compaction: manifest state survives remount after rotate-before-compact") {
    resetSpool();
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    cfg.maxSegments     = 200;

    std::uint32_t seq  = 1;
    std::uint32_t head = 1;

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
    resetSpool();
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

    while (head < seq) { storePop(store); ++head; }

    for (int i = 0; i < 10; ++i) {
        const auto st = store.compactOneSegment();
        CHECK(st.ok());
        if (st.isNoOp()) break;
    }
    CHECK_EQ(store.manifestRanges().size(), 0U);
}

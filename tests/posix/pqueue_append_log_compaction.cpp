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
    plantLayout({.ranges={}, .tail = 1, .next = 2, .segments = {{.gen=1,.firstSeq=0,.body={}}}});

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
    plantLayout({
        .ranges = {{1, 1}}, .tail = 4, .next = 5,
        .segments = {{.gen=1,.firstSeq=0,.body={}}, {.gen=4, .firstSeq=1, .body=serializeEnqueueEvent(1, "c")}},
    });

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
    plantLayout({
        .ranges = {{1, 1}}, .tail = 5, .next = 6,
        .segments = {
            {.gen=1, .firstSeq=1, .body=serializeEnqueueEvent(1,"a")+serializeEnqueueEvent(2,"b")+serializeEnqueueEvent(3,"c")},
            {.gen=5, .firstSeq=4, .body={}},
        },
    });

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

TEST_CASE("cleanup: dangling segment is removed on mount") {
    // A segment whose generation is not referenced by the manifest is cleaned up
    // during mount (scanSegments → cleanupOneDanglingSegment).
    plantLayout({
        .ranges = {{1, 1}}, .tail = 2, .next = 3,
        .segments = {
            {.gen=1, .firstSeq=1, .body=serializeEnqueueEvent(1,"a")+serializeEnqueueEvent(2,"b")},
            {.gen=2, .firstSeq=3, .body={}},                          // tail
            {.gen=3, .firstSeq=1, .body=serializeEnqueueEvent(1,"a")},// dangling
        },
    });

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());
    CHECK_FALSE(std::filesystem::exists(segmentPath(3)));
    CHECK(std::filesystem::exists(segmentPath(1)));
    CHECK(std::filesystem::exists(segmentPath(2)));
}


TEST_CASE("compaction: dangling segment from failed publish is harmless on remount (critical)") {
    // Simulates a crash after the compacted segment was written but before the manifest
    // publish succeeded. The winning manifest still points at the original segments;
    // the dangling file is ignored and all records are intact.
    plantLayout({
        .ranges = {{1, 1}}, .tail = 2, .next = 3,
        .segments = {
            {.gen=1, .firstSeq=1, .body=serializeEnqueueEvent(1,"a")+serializeEnqueueEvent(2,"b")},
            {.gen=2, .firstSeq=3, .body={}},                                         // tail
            {.gen=3, .firstSeq=1, .body=serializeEnqueueEvent(1,"a")+serializeEnqueueEvent(2,"b")}, // dangling
        },
    });

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
    plantLayout({
        .ranges = {{1,1},{3,3}}, .tail = 5, .next = 6,
        .segments = {{.gen=1,.firstSeq=0,.body={}},{.gen=3,.firstSeq=0,.body={}},{.gen=5,.firstSeq=0,.body={}}},  // header only → both ranges dead
    });

    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());
    REQUIRE(store.chooseCompactionRange().has_value());

    CHECK(store.compactFull().ok());
    CHECK_FALSE(store.chooseCompactionRange().has_value());
}

TEST_CASE("compaction: compactIdle maxSteps=0 does nothing") {
    plantLayout({
        .ranges = {{1,1}}, .tail = 2, .next = 3,
        .segments = {{.gen=1,.firstSeq=0,.body={}},{.gen=2,.firstSeq=0,.body={}}},
    });

    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());
    REQUIRE(store.chooseCompactionRange().has_value());

    const auto r = store.compactIdle(0);
    CHECK(r.status.ok());
    CHECK_EQ(r.stepsRun, 0u);
    CHECK_EQ(r.compactions, 0u);
    CHECK_EQ(r.noOps, 0u);
    CHECK_FALSE(r.moreWorkLikely);
    CHECK(store.chooseCompactionRange().has_value()); // nothing changed
}

TEST_CASE("compaction: compactIdle maxSteps=1 runs at most one step") {
    // Two dead ranges: compactIdle(1) should compact only one of them.
    plantLayout({
        .ranges = {{1,1},{3,3}}, .tail = 5, .next = 6,
        .segments = {{.gen=1,.firstSeq=0,.body={}},{.gen=3,.firstSeq=0,.body={}},{.gen=5,.firstSeq=0,.body={}}},
    });

    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());

    const auto r = store.compactIdle(1);
    CHECK(r.status.ok());
    CHECK_EQ(r.stepsRun, 1u);
    CHECK_EQ(r.compactions, 1u);
    CHECK_EQ(r.noOps, 0u);
    CHECK(r.moreWorkLikely); // stopped by budget, second range still pending
}

TEST_CASE("compaction: compactIdle moreWorkLikely false when exhausted naturally") {
    // One dead range: compactIdle with a large budget should finish in one step.
    plantLayout({
        .ranges = {{1,1}}, .tail = 3, .next = 4,
        .segments = {{.gen=1,.firstSeq=0,.body={}},{.gen=3,.firstSeq=0,.body={}}},
    });

    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());

    const auto r = store.compactIdle(10);
    CHECK(r.status.ok());
    CHECK_EQ(r.compactions, 1u);
    CHECK_GE(r.noOps, 1u);    // hit noOp after the one useful step
    CHECK_FALSE(r.moreWorkLikely);
    CHECK_FALSE(store.chooseCompactionRange().has_value());
}

TEST_CASE("compaction: compactFull still bounded by initial range count") {
    plantLayout({
        .ranges = {{1,1},{3,3}}, .tail = 5, .next = 6,
        .segments = {{.gen=1,.firstSeq=0,.body={}},{.gen=3,.firstSeq=0,.body={}},{.gen=5,.firstSeq=0,.body={}}},
    });

    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());

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
    // Build state via live ops so activeTailDependenciesTracked_=true when compaction runs.
    // 6 one-byte records fill segs 1-3 (2 per seg at maxSegmentBytes=70). Popping records
    // 1+2 triggers rotation (seg 3 is full) → pop events land in seg 4. Affected gens={1}.
    // Compact range {1,3}: tail is contiguous and gen 1 ∈ [1,4] → rotate-before-compact
    // fires → effective range {1,4}, 4 live records → 2 output segs. 2 < 4 → proceeds.
    resetSpool();

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70; // 20-byte header + 2×25-byte enqueue events = exactly 2 records
    pqueue::AppendLogStore store(cfg);
    REQUIRE(store.mount().ok());

    storeEnqueue(store, 1, "a");
    storeEnqueue(store, 2, "b");
    storeEnqueue(store, 3, "c");
    storeEnqueue(store, 4, "d");
    storeEnqueue(store, 5, "e");
    storeEnqueue(store, 6, "f");
    storePop(store); // seg 3 full → rotation → pop for seq 1 lands in seg 4; affected={1}
    storePop(store); // pop for seq 2 lands in seg 4; affected={1} (unchanged)

    REQUIRE_EQ(store.manifestRanges().size(), 1u);
    REQUIRE_EQ(store.manifestRanges()[0].startGen, 1u);
    REQUIRE_EQ(store.manifestRanges()[0].endGen, 3u);

    auto st = store.compactOneSegment();
    CHECK(st.ok());
    CHECK_FALSE(st.isNoOp());

    // rotate-before-compact fired: seals gen 4 → range {1,4}, new tail=gen 5, nextGen=6.
    // 4 live records → 2 output segs at gen 6 and gen 7. Result: range {6,7}.
    REQUIRE_EQ(store.manifestRanges().size(), 1u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 6u);
    CHECK_EQ(store.manifestRanges()[0].endGen, 7u);

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
    plantLayout({
        .ranges = {{1, 2}}, .tail = 5, .next = 6,
        .segments = {
            {1, 1, serializeEnqueueEvent(1,"a")+serializeEnqueueEvent(2,"b")},
            {2, 3, serializeEnqueueEvent(3,"c")+serializeEnqueueEvent(4,"d")},
            {.gen=5, .firstSeq=5, .body={}},
        },
    });

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

    const auto st = store.commitEnqueue(3, "c");
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
    CHECK(store.commitEnqueue(3, "c").ok());

    expectRecords(store, {{2,"b"},{3,"c"}});
    expectRecordsAfterRemount(cfg, {{2,"b"},{3,"c"}});
}

TEST_CASE("maxTotalBytes: DropOldest evicts and retries when commitEnqueue returns QueueFull") {
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

TEST_CASE("sequence exhaustion: commitEnqueue fails closed at UINT32_MAX") {
    resetSpool();
    auto cfg = makeStoreConfig();
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    const auto st = store.commitEnqueue(std::numeric_limits<std::uint32_t>::max(), "data");
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::SequenceExhausted);
}

TEST_CASE("commitPop: sequence mismatch returns InvalidIndex") {
    resetSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());
    CHECK(store.commitEnqueue(0, "a").ok());

    const auto st = store.commitPop(99);
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::InvalidIndex);
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

// ---------------------------------------------------------------------------
// Subrange compaction
//
// maxSegmentBytes=185, payload=60 bytes ("aaa...60" etc).
//   kSegmentHeaderBytes=20, kEnqueueOverheadBytes=24 -> 104 bytes/segment.
//   Second enqueue: 104+84=188 > 185 -> rotation. One record per gen.
//   Four pops: 104+4*20=184 <= 185 -> no rotation from pops.
//   After enqueueing seqs 1-5: ranges=[{1,4}], tail=5, nextGen=6.
//   seq i lives in gen i.
// ---------------------------------------------------------------------------

namespace {

pqueue::AppendLogConfig makeSubrangeConfig() {
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 185;
    return cfg;
}

const std::string kP1(60, 'a');
const std::string kP2(60, 'b');
const std::string kP3(60, 'c');
const std::string kP4(60, 'd');
const std::string kP5(60, 'e');

// Enqueue seqs 1-5, each in its own gen. Result: ranges=[{1,4}], tail=5, nextGen=6.
void setupSubrangeStore(pqueue::AppendLogStore& store) {
    storeEnqueue(store, 1, kP1);
    storeEnqueue(store, 2, kP2);
    storeEnqueue(store, 3, kP3);
    storeEnqueue(store, 4, kP4);
    storeEnqueue(store, 5, kP5);
}

} // namespace

TEST_CASE("subrange: prefix compaction produces correct manifest split and FIFO order") {
    // ranges=[{1,4}], tail=5, nextGen=6. Pop seq=1 (dead gen=1).
    // Compact prefix {1,2}: live=[seq=2], output->gen=6.
    // Result: ranges=[{6,6},{3,4}], tail=5.
    resetSpool();
    pqueue::AppendLogStore store(makeSubrangeConfig());
    CHECK(store.mount().ok());
    setupSubrangeStore(store);
    storePop(store); // seq=1 dead

    const auto st = store.compactRange({1, 2});
    CHECK(st.ok());
    CHECK_FALSE(st.isNoOp());

    REQUIRE_EQ(store.manifestRanges().size(), 2u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 6u);
    CHECK_EQ(store.manifestRanges()[0].endGen,   6u);
    CHECK_EQ(store.manifestRanges()[1].startGen, 3u);
    CHECK_EQ(store.manifestRanges()[1].endGen,   4u);
    CHECK_EQ(store.tailGeneration(), 5u);

    CHECK_FALSE(std::filesystem::exists(segmentPath(1)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(2)));
    CHECK(std::filesystem::exists(segmentPath(3)));
    CHECK(std::filesystem::exists(segmentPath(4)));
    CHECK(std::filesystem::exists(segmentPath(5)));
    CHECK(std::filesystem::exists(segmentPath(6)));

    expectRecord(store, 2, kP2);
    expectRecord(store, 3, kP3);
    expectRecord(store, 4, kP4);
    expectRecord(store, 5, kP5);
    checkTracking(store);
}

TEST_CASE("subrange: suffix compaction produces correct manifest split and FIFO order") {
    // Pop seq=1,2,3 (3 pops, no rotation since 104+60=164<=185).
    // Compact suffix {3,4}: gen=3 dead, gen=4 live(seq=4), output->gen=6.
    // tailDepsContained=false (gen=1,2 outside {3,5}), so no rotate.
    // Result: ranges=[{1,2},{6,6}], tail=5.
    resetSpool();
    pqueue::AppendLogStore store(makeSubrangeConfig());
    CHECK(store.mount().ok());
    setupSubrangeStore(store);
    storePop(store); storePop(store); storePop(store); // seq=1,2,3

    const auto st = store.compactRange({3, 4});
    CHECK(st.ok());
    CHECK_FALSE(st.isNoOp());

    REQUIRE_EQ(store.manifestRanges().size(), 2u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 1u);
    CHECK_EQ(store.manifestRanges()[0].endGen,   2u);
    CHECK_EQ(store.manifestRanges()[1].startGen, 6u);
    CHECK_EQ(store.manifestRanges()[1].endGen,   6u);
    CHECK_EQ(store.tailGeneration(), 5u);

    CHECK(std::filesystem::exists(segmentPath(1)));
    CHECK(std::filesystem::exists(segmentPath(2)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(3)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(4)));
    CHECK(std::filesystem::exists(segmentPath(5)));
    CHECK(std::filesystem::exists(segmentPath(6)));

    expectRecord(store, 4, kP4);
    expectRecord(store, 5, kP5);
    checkTracking(store);
}

TEST_CASE("subrange: middle compaction produces three-range manifest and FIFO order") {
    // Pop seq=1,2. Compact middle {2,3}: gen=2 dead, gen=3 live(seq=3), output->gen=6.
    // Result: ranges=[{1,1},{6,6},{4,4}], tail=5.
    resetSpool();
    pqueue::AppendLogStore store(makeSubrangeConfig());
    CHECK(store.mount().ok());
    setupSubrangeStore(store);
    storePop(store); storePop(store); // seq=1,2

    const auto st = store.compactRange({2, 3});
    CHECK(st.ok());
    CHECK_FALSE(st.isNoOp());

    REQUIRE_EQ(store.manifestRanges().size(), 3u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 1u);
    CHECK_EQ(store.manifestRanges()[0].endGen,   1u);
    CHECK_EQ(store.manifestRanges()[1].startGen, 6u);
    CHECK_EQ(store.manifestRanges()[1].endGen,   6u);
    CHECK_EQ(store.manifestRanges()[2].startGen, 4u);
    CHECK_EQ(store.manifestRanges()[2].endGen,   4u);
    CHECK_EQ(store.tailGeneration(), 5u);

    CHECK(std::filesystem::exists(segmentPath(1)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(2)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(3)));
    CHECK(std::filesystem::exists(segmentPath(4)));
    CHECK(std::filesystem::exists(segmentPath(5)));
    CHECK(std::filesystem::exists(segmentPath(6)));

    expectRecord(store, 3, kP3);
    expectRecord(store, 4, kP4);
    expectRecord(store, 5, kP5);
    checkTracking(store);
}

TEST_CASE("subrange: dead prefix drops subrange and leaves remainder") {
    // Pop seq=1,2 -> both dead. Dead prefix {1,2}: gateDelta=0, no output written.
    // Result: ranges=[{3,4}].
    resetSpool();
    pqueue::AppendLogStore store(makeSubrangeConfig());
    CHECK(store.mount().ok());
    setupSubrangeStore(store);
    storePop(store); storePop(store);

    const auto st = store.compactRange({1, 2});
    CHECK(st.ok());
    CHECK_FALSE(st.isNoOp());

    REQUIRE_EQ(store.manifestRanges().size(), 1u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 3u);
    CHECK_EQ(store.manifestRanges()[0].endGen,   4u);

    CHECK_FALSE(std::filesystem::exists(segmentPath(1)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(2)));
    CHECK(std::filesystem::exists(segmentPath(3)));
    CHECK(std::filesystem::exists(segmentPath(4)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(6)));
    checkTracking(store);
}

TEST_CASE("subrange: dead suffix drops subrange and leaves remainder") {
    // Pop seq=1..4 (4 pops, 104+80=184<=185, no rotation).
    // Dead suffix {3,4}: gateDelta=0. Result: ranges=[{1,2}].
    resetSpool();
    pqueue::AppendLogStore store(makeSubrangeConfig());
    CHECK(store.mount().ok());
    setupSubrangeStore(store);
    storePop(store); storePop(store); storePop(store); storePop(store);

    const auto st = store.compactRange({3, 4});
    CHECK(st.ok());
    CHECK_FALSE(st.isNoOp());

    REQUIRE_EQ(store.manifestRanges().size(), 1u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 1u);
    CHECK_EQ(store.manifestRanges()[0].endGen,   2u);

    CHECK(std::filesystem::exists(segmentPath(1)));
    CHECK(std::filesystem::exists(segmentPath(2)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(3)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(4)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(6)));
    checkTracking(store);
}

TEST_CASE("subrange: dead middle splits parent into two remainder ranges") {
    // Pop seq=1..3. Dead middle {2,3}: gateDelta=1 (dead middle), 1+1=2<=4.
    // Result: ranges=[{1,1},{4,4}], no output seg.
    resetSpool();
    pqueue::AppendLogStore store(makeSubrangeConfig());
    CHECK(store.mount().ok());
    setupSubrangeStore(store);
    storePop(store); storePop(store); storePop(store);

    const auto st = store.compactRange({2, 3});
    CHECK(st.ok());
    CHECK_FALSE(st.isNoOp());

    REQUIRE_EQ(store.manifestRanges().size(), 2u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 1u);
    CHECK_EQ(store.manifestRanges()[0].endGen,   1u);
    CHECK_EQ(store.manifestRanges()[1].startGen, 4u);
    CHECK_EQ(store.manifestRanges()[1].endGen,   4u);

    CHECK(std::filesystem::exists(segmentPath(1)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(2)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(3)));
    CHECK(std::filesystem::exists(segmentPath(4)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(6)));
    checkTracking(store);
}

TEST_CASE("subrange: remount after prefix split preserves manifest and record accessibility") {
    resetSpool();
    {
        pqueue::AppendLogStore store(makeSubrangeConfig());
        CHECK(store.mount().ok());
        setupSubrangeStore(store);
        storePop(store);
        CHECK(store.compactRange({1, 2}).ok());
    }
    pqueue::AppendLogStore store2(makeSubrangeConfig());
    CHECK(store2.mount().ok());
    REQUIRE_EQ(store2.manifestRanges().size(), 2u);
    CHECK_EQ(store2.manifestRanges()[0].startGen, 6u);
    CHECK_EQ(store2.manifestRanges()[0].endGen,   6u);
    CHECK_EQ(store2.manifestRanges()[1].startGen, 3u);
    CHECK_EQ(store2.manifestRanges()[1].endGen,   4u);
    expectRecord(store2, 2, kP2);
    expectRecord(store2, 3, kP3);
    expectRecord(store2, 4, kP4);
    expectRecord(store2, 5, kP5);
}

TEST_CASE("subrange: remount after middle split preserves three-range manifest") {
    resetSpool();
    {
        pqueue::AppendLogStore store(makeSubrangeConfig());
        CHECK(store.mount().ok());
        setupSubrangeStore(store);
        storePop(store); storePop(store);
        CHECK(store.compactRange({2, 3}).ok());
    }
    pqueue::AppendLogStore store2(makeSubrangeConfig());
    CHECK(store2.mount().ok());
    REQUIRE_EQ(store2.manifestRanges().size(), 3u);
    CHECK_EQ(store2.manifestRanges()[0].startGen, 1u);
    CHECK_EQ(store2.manifestRanges()[0].endGen,   1u);
    CHECK_EQ(store2.manifestRanges()[1].startGen, 6u);
    CHECK_EQ(store2.manifestRanges()[1].endGen,   6u);
    CHECK_EQ(store2.manifestRanges()[2].startGen, 4u);
    CHECK_EQ(store2.manifestRanges()[2].endGen,   4u);
    expectRecord(store2, 3, kP3);
    expectRecord(store2, 4, kP4);
    expectRecord(store2, 5, kP5);
}

TEST_CASE("subrange: gate 1 fallback=yes on live middle expands to full parent and compacts it") {
    // 3 ranges. Live record in gen=2 (middle of {1,3}).
    // Middle {2,2}: liveDelta=2, 3+2=5>4 -> gate 1 fires.
    // fallback=yes -> inputRange widened to {1,3}, hasLeft=false, hasRight=false.
    // Full parent compacted: only live record is seq=2 -> output gen=31.
    // Result: ranges=[{31,31},{10,11},{20,20}].
    plantLayout({
        .ranges = {{1,3},{10,11},{20,20}},
        .tail   = 30,
        .next   = 31,
        .segments = {
            {.gen=1,  .firstSeq=1},
            {.gen=2,  .firstSeq=2, .body=serializeEnqueueEvent(2, "bbbb")},
            {.gen=3,  .firstSeq=3},
            {.gen=10, .firstSeq=10},
            {.gen=11, .firstSeq=11},
            {.gen=20, .firstSeq=20},
            {.gen=30, .firstSeq=30},
        }
    });
    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());
    REQUIRE_EQ(store.manifestRanges().size(), 3u);

    const auto st = store.compactRange({2, 2},
                                       nullptr,
                                       pqueue::AppendLogStore::AllowFullRangeFallback::yes);
    CHECK(st.ok());
    CHECK_FALSE(st.isNoOp());

    // Full parent {1,3} was compacted, not just {2,2}. Gen=1 and gen=3 had no records.
    REQUIRE_EQ(store.manifestRanges().size(), 3u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 31u);
    CHECK_EQ(store.manifestRanges()[0].endGen,   31u);
    CHECK_EQ(store.manifestRanges()[1].startGen, 10u);
    CHECK_EQ(store.manifestRanges()[1].endGen,   11u);
    CHECK_EQ(store.manifestRanges()[2].startGen, 20u);
    CHECK_EQ(store.manifestRanges()[2].endGen,   20u);

    CHECK_FALSE(std::filesystem::exists(segmentPath(1)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(2)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(3)));
    CHECK(std::filesystem::exists(segmentPath(31)));
    checkTracking(store);
}

TEST_CASE("subrange: gate 1 fallback=yes on live suffix at range count 4 widens and rotate fires") {
    // 4 ranges; last range has live suffix with hasLeft=true and contiguous tail.
    // State built via live ops so activeTailDependenciesTracked_=true.
    //
    // Plant: ranges=[{1,2},{10,11},{20,21},{30,33}], tail=34 (enqueue seq=2, 104B), next=35.
    //   gen=30: enqueue(seq=1,kP1)=104B (live). gen=31,32,33: header-only (dead).
    //
    // Mount + storeEnqueue(seq=3,kP3): 104+84=188>185 -> rotate gen=34 sealed -> {30,34}.
    //   ranges=[{1,2},{10,11},{20,21},{30,34}], tail=35, tracking=true.
    //
    // compactRange({32,34}, fallback=yes):
    //   suffix of {30,34}: hasLeft=true, hasRight=false.
    //   rangeHasLive=true (seq=2 in gen=34). liveDelta=1. 4+1=5>4 -> gate 1 fires.
    //   fallback=yes -> inputRange={30,34}, hasLeft=false.
    //   wouldRotate=true (34==lastRange.endGen, tail=35 contiguous, deps={}).
    //   hypoHasLive=true (seq=1,2,3 in [30,35]). Gate 2: hasLeft=false -> does not fire.
    //   Rotate: gen=35 sealed -> {30,35}, tail=36. inputRange extended to {30,35}.
    //   Live records: seq=1(gen=30), seq=2(gen=34), seq=3(gen=35). Output: gen=37,38,39.
    //   Splice: erase {30,35}, insert {37,39}. Result: [{1,2},{10,11},{20,21},{37,39}], tail=36.

    plantLayout({
        .ranges = {{1,2},{10,11},{20,21},{30,33}},
        .tail   = 34,
        .next   = 35,
        .segments = {
            {.gen=1,  .firstSeq=1},
            {.gen=2,  .firstSeq=2},
            {.gen=10, .firstSeq=10},
            {.gen=11, .firstSeq=11},
            {.gen=20, .firstSeq=20},
            {.gen=21, .firstSeq=21},
            {.gen=30, .firstSeq=1, .body=serializeEnqueueEvent(1, kP1)},
            {.gen=31, .firstSeq=0, .body={}},
            {.gen=32, .firstSeq=0, .body={}},
            {.gen=33, .firstSeq=0, .body={}},
            {.gen=34, .firstSeq=2, .body=serializeEnqueueEvent(2, kP2)},
        }
    });

    pqueue::AppendLogStore store(makeSubrangeConfig());
    CHECK(store.mount().ok());
    REQUIRE_EQ(store.manifestRanges().size(), 4u);
    CHECK_EQ(store.tailGeneration(), 34u);

    storeEnqueue(store, 3, kP3); // 104+84>185 -> rotate gen=34 sealed -> {30,34}, tail=35
    REQUIRE_EQ(store.manifestRanges().size(), 4u);
    CHECK_EQ(store.manifestRanges()[3].startGen, 30u);
    CHECK_EQ(store.manifestRanges()[3].endGen,   34u);
    CHECK_EQ(store.tailGeneration(), 35u);

    const auto st = store.compactRange({32, 34},
                                       nullptr,
                                       pqueue::AppendLogStore::AllowFullRangeFallback::yes);
    CHECK(st.ok());
    CHECK_FALSE(st.isNoOp());

    CHECK_EQ(store.tailGeneration(), 36u); // rotate fired

    REQUIRE_EQ(store.manifestRanges().size(), 4u);
    CHECK_EQ(store.manifestRanges()[3].startGen, 37u);
    CHECK_EQ(store.manifestRanges()[3].endGen,   39u);

    CHECK_FALSE(std::filesystem::exists(segmentPath(30)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(34)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(35)));
    CHECK(std::filesystem::exists(segmentPath(37)));
    CHECK(std::filesystem::exists(segmentPath(38)));
    CHECK(std::filesystem::exists(segmentPath(39)));

    expectRecord(store, 1, kP1);
    expectRecord(store, 2, kP2);
    expectRecord(store, 3, kP3);
    checkTracking(store);
}

TEST_CASE("subrange: range-count gate returns noOp on hot path for live middle subrange") {
    // 3 existing ranges. Live record in gen=2 (middle of {1,3}).
    // Middle subrange {2,2}: liveDelta=2, 3+2=5>4 -> noOp in hot path.
    // Full parent must not be compacted.
    plantLayout({
        .ranges = {{1,3},{10,11},{20,20}},
        .tail   = 30,
        .next   = 31,
        .segments = {
            {.gen=1,  .firstSeq=1},
            {.gen=2,  .firstSeq=2, .body=serializeEnqueueEvent(2, "bbbb")},
            {.gen=3,  .firstSeq=3},
            {.gen=10, .firstSeq=10},
            {.gen=11, .firstSeq=11},
            {.gen=20, .firstSeq=20},
            {.gen=30, .firstSeq=30},
        }
    });
    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());
    REQUIRE_EQ(store.manifestRanges().size(), 3u);

    const auto st = store.compactRange({2, 2},
                                       nullptr,
                                       pqueue::AppendLogStore::AllowFullRangeFallback::no);
    CHECK(st.ok());
    CHECK(st.isNoOp());

    // Manifest unchanged; full parent {1,3} not touched.
    REQUIRE_EQ(store.manifestRanges().size(), 3u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 1u);
    CHECK_EQ(store.manifestRanges()[0].endGen,   3u);
}

TEST_CASE("subrange: dead prefix/suffix does not block at range count 4") {
    // At count=4, dead prefix has gateDelta=0 -> must proceed (not noOp).
    // Compact dead prefix {1,1} of {1,2}: drops gen=1, remainder {2,2}. Count stays 4.
    plantLayout({
        .ranges = {{1,2},{10,11},{20,21},{30,31}},
        .tail   = 40,
        .next   = 41,
        .segments = {
            {.gen=1,  .firstSeq=1},
            {.gen=2,  .firstSeq=2},
            {.gen=10, .firstSeq=10},
            {.gen=11, .firstSeq=11},
            {.gen=20, .firstSeq=20},
            {.gen=21, .firstSeq=21},
            {.gen=30, .firstSeq=30},
            {.gen=31, .firstSeq=31},
            {.gen=40, .firstSeq=40},
        }
    });
    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());
    REQUIRE_EQ(store.manifestRanges().size(), 4u);

    const auto st = store.compactRange({1, 1},
                                       nullptr,
                                       pqueue::AppendLogStore::AllowFullRangeFallback::no);
    CHECK(st.ok());
    CHECK_FALSE(st.isNoOp());

    REQUIRE_EQ(store.manifestRanges().size(), 4u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 2u);
    CHECK_EQ(store.manifestRanges()[0].endGen,   2u);
    CHECK_FALSE(std::filesystem::exists(segmentPath(1)));
    CHECK(std::filesystem::exists(segmentPath(2)));
    checkTracking(store);
}

TEST_CASE("subrange: dead suffix with pop-only tail proceeds without rotate at range count 4") {
    // Regression for hypoHasLive guard in the pre-rotate gate.
    // Without it, wouldRotate && hasLeft fires the gate even when the extended range
    // has no live records (tail is pop-only). Dead-suffix cleanup would be wrongly
    // blocked at range count 4 with fallback=no.
    //
    // State is built via live operations so activeTailDependenciesTracked_=true.
    //
    // Plant gen=25 with enqueue(1)+enqueue(2)+pop(1)+pop(2) so that after mount
    // records_=[] and activeSegmentBytes_=178 (20hdr+59+59+20+20).
    //
    // Drive two rotations with maxSegmentBytes=150, 35-byte payloads (59B/record):
    //   enqueue(3): 178+59=237>150 → rotate gen=25 → {25,25}, 4 ranges, tail=26, tracking=true
    //   enqueue(4): gen=26=138B
    //   enqueue(5): 138+59=197>150 → rotate gen=26 → merge {25,26}, tail=27, tracking=true
    //
    // Pop seq=3,4 (gen=26) and seq=5 (gen=27):
    //   tail=27 has enqueue(5)+pop(3)+pop(4)+pop(5) = 139B, all pop-only for live records
    //   affected={26,27}; tailDepsContained for suffix {26,26}: all gens in [26,27]. TRUE.
    //   hypoHasLive=false (no records in [26,27]) → pre-rotate gate must NOT fire.
    //
    // compactRange({26,26}) must proceed as dead-range removal; tail must not rotate.

    const std::string p35(35, 'x');
    plantLayout({
        .ranges = {{1,2},{10,11},{20,21}},
        .tail   = 25,
        .next   = 26,
        .segments = {
            {.gen=1,  .firstSeq=1},
            {.gen=2,  .firstSeq=2},
            {.gen=10, .firstSeq=10},
            {.gen=11, .firstSeq=11},
            {.gen=20, .firstSeq=20},
            {.gen=21, .firstSeq=21},
            {.gen=25, .firstSeq=1, .body = serializeEnqueueEvent(1, p35) +
                                           serializeEnqueueEvent(2, p35) +
                                           serializePopEvent(1) +
                                           serializePopEvent(2)},
        }
    });

    pqueue::AppendLogConfig cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 150;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());
    REQUIRE_EQ(store.manifestRanges().size(), 3u);
    CHECK_EQ(store.tailGeneration(), 25u);

    storeEnqueue(store, 3, p35); // 178+59>150 → rotate gen=25 → {25,25}, 4 ranges, tail=26
    storeEnqueue(store, 4, p35); // gen=26: 138B
    storeEnqueue(store, 5, p35); // 138+59>150 → rotate gen=26 → merge {25,26}, tail=27

    REQUIRE_EQ(store.manifestRanges().size(), 4u);
    CHECK_EQ(store.manifestRanges()[3].startGen, 25u);
    CHECK_EQ(store.manifestRanges()[3].endGen,   26u);
    CHECK_EQ(store.tailGeneration(), 27u);

    storePop(store); // seq=3 (gen=26) → affected={26}
    storePop(store); // seq=4 (gen=26)
    storePop(store); // seq=5 (gen=27) → affected={26,27}
    // tail=27: 20+59+20+20+20=139B (enqueue+3 pops); no live records in [26,27]

    const auto st = store.compactRange({26, 26},
                                       nullptr,
                                       pqueue::AppendLogStore::AllowFullRangeFallback::no);
    CHECK(st.ok());
    CHECK_FALSE(st.isNoOp());

    CHECK_EQ(store.tailGeneration(), 27u); // no rotation
    REQUIRE_EQ(store.manifestRanges().size(), 4u);
    CHECK_EQ(store.manifestRanges()[3].startGen, 25u);
    CHECK_EQ(store.manifestRanges()[3].endGen,   25u);

    CHECK_FALSE(std::filesystem::exists(segmentPath(26)));
    CHECK(std::filesystem::exists(segmentPath(25)));
    checkTracking(store);
}

TEST_CASE("subrange: live suffix triggers rotate-before-compact extending inputRange to old tail") {
    // Regression for rotate-before-compact on suffix subranges.
    // activeTailDependenciesTracked_=true is required for wouldRotate; build state via live ops.
    //
    // Plant: ranges=[{1,4}], tail=5, next=6.
    //   gen=1..3: header-only (no records).
    //   gen=4: enqueue(seq=1,kP1)+enqueue(seq=2,kP2) = 188B, two live records.
    //   gen=5 (tail): enqueue(seq=3,kP3) = 104B.
    //
    // Mount + storeEnqueue(seq=4, kP4): 104+84=188>185 -> rotate gen=5 sealed.
    //   ranges=[{1,5}], tail=6, nextGen=7, tracking=true.
    //   seq=4 written to gen=6: 104B.
    //
    // storePop x2: dead gen=4 (seq=1,seq=2). tail=6 gains two pop tombstones: 144B.
    //   affected={4}; tailDepsContained for suffix {4,5}: gen=4 in [4,6]. TRUE.
    //
    // compactRange({4,5}):
    //   suffix of {1,5}; wouldRotate=true (5==lastRange.endGen, tail=6 contiguous).
    //   hypoHasLive=true (seq=3 in gen=5, seq=4 in gen=6, both in [4,6]).
    //   Pre-rotate gate: +1, size=1 -> 2<=4. Rotate fires: gen=6 sealed -> {1,6}, tail=7.
    //   inputRange extended to {4,6}; suffix of {1,6}: hasLeft=true, hasRight=false.
    //   Live records in [4,6]: seq=3(gen=5), seq=4(gen=6). Output: gen=8(seq=3), gen=9(seq=4).
    //   Splice: erase {1,6}, insert [{1,3},{8,9}].
    //   Result: ranges=[{1,3},{8,9}], tail=7.

    plantLayout({
        .ranges = {{1, 4}},
        .tail   = 5,
        .next   = 6,
        .segments = {
            {.gen=1, .firstSeq=0, .body={}},
            {.gen=2, .firstSeq=0, .body={}},
            {.gen=3, .firstSeq=0, .body={}},
            {.gen=4, .firstSeq=1, .body=serializeEnqueueEvent(1, kP1) + serializeEnqueueEvent(2, kP2)},
            {.gen=5, .firstSeq=3, .body=serializeEnqueueEvent(3, kP3)},
        }
    });

    pqueue::AppendLogStore store(makeSubrangeConfig());
    CHECK(store.mount().ok());
    REQUIRE_EQ(store.manifestRanges().size(), 1u);
    CHECK_EQ(store.tailGeneration(), 5u);

    storeEnqueue(store, 4, kP4); // 104+84>185 -> rotate gen=5 sealed; seq=4 to gen=6
    REQUIRE_EQ(store.manifestRanges().size(), 1u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 1u);
    CHECK_EQ(store.manifestRanges()[0].endGen,   5u);
    CHECK_EQ(store.tailGeneration(), 6u);

    storePop(store); // seq=1 (gen=4) dead; pop tombstone in gen=6
    storePop(store); // seq=2 (gen=4) dead; pop tombstone in gen=6

    const auto st = store.compactRange({4, 5});
    CHECK(st.ok());
    CHECK_FALSE(st.isNoOp());

    CHECK_EQ(store.tailGeneration(), 7u); // rotate fired

    REQUIRE_EQ(store.manifestRanges().size(), 2u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 1u);
    CHECK_EQ(store.manifestRanges()[0].endGen,   3u);
    CHECK_EQ(store.manifestRanges()[1].startGen, 8u);
    CHECK_EQ(store.manifestRanges()[1].endGen,   9u);

    CHECK_FALSE(std::filesystem::exists(segmentPath(4)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(5)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(6)));
    CHECK(std::filesystem::exists(segmentPath(8)));
    CHECK(std::filesystem::exists(segmentPath(9)));

    expectRecord(store, 3, kP3);
    expectRecord(store, 4, kP4);
    checkTracking(store);
}

// ---------------------------------------------------------------------------
// rewriteFront / rewrite semantics tests
//
// Dead bytes for compaction come from the REWRITE event superseding the
// original ENQUEUE bytes. No pop of the rewritten record is needed.
// maxSegmentBytes=300 keeps the REWRITE in the same sealed segment as the
// original ENQUEUE so collectLiveRecords reads the rewritten payload and
// the compaction output carries it forward without FIFO disruption.
// ---------------------------------------------------------------------------

namespace {

pqueue::AppendLogConfig makeRewriteCompactionConfig() {
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 300;
    return cfg;
}

const std::string kP6(60, 'f');
const std::string kP7(60, 'g');
const std::string kP8(60, 'h');

} // namespace

TEST_CASE("rewrite: rewriteFront payload survives compaction and remount") {
    // maxSegmentBytes=300: seq=1..3 fit in gen=1 alongside their REWRITE event.
    // Dead bytes = original ENQUEUE bytes for seq=1 (superseded by REWRITE).
    // No pop of seq=1 needed; dead bytes from the REWRITE itself trigger compaction.
    //
    // storeEnqueue(1,kP1): gen=1=104B.
    // storeEnqueue(2,kP2): gen=1=188B.
    // rewriteRecord(1,"X"): REWRITE(25B) in gen=1=213B. seq=1.segGen=gen=1.
    // storeEnqueue(3,kP3): gen=1=297B.
    // storeEnqueue(4,kP4): 297+84=381>300 -> rotate gen=1 sealed. ranges=[{1,1}], tail=2.
    //
    // compactRange({1,1}): live=[seq=1("X"),seq=2(kP2),seq=3(kP3)]. Dead=84B (original ENQUEUE seq=1).
    // rotate-before-compact fires (gen=2 tail contiguous, deps={}): gen=3 becomes new tail,
    // inputRange extends to {1,2}, output goes to gen=4. ranges=[{4,4}], tail=3.
    // Remount: front=seq=1 payload="X". FIFO preserved.
    resetSpool();
    auto rwCfg = makeRewriteCompactionConfig();
    pqueue::AppendLogStore store(rwCfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, kP1);
    storeEnqueue(store, 2, kP2);
    CHECK(store.rewriteRecord(1, std::string(1, 'X')).ok()); // REWRITE in gen=1, 213B; no rotation
    storeEnqueue(store, 3, kP3);
    storeEnqueue(store, 4, kP4); // 297+84>300 -> rotate gen=1 sealed

    REQUIRE_EQ(store.manifestRanges().size(), 1u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 1u);
    CHECK_EQ(store.manifestRanges()[0].endGen,   1u);
    CHECK_EQ(store.tailGeneration(), 2u);

    const auto st = store.compactRange({1, 1});
    CHECK(st.ok());
    CHECK_FALSE(st.isNoOp());

    // rotate-before-compact fires (gen=2 tail contiguous, deps={}):
    // gen=3 created as new tail, inputRange extended to {1,2}, output gen=4.
    REQUIRE_EQ(store.manifestRanges().size(), 1u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 4u);
    CHECK_EQ(store.manifestRanges()[0].endGen,   4u);
    CHECK_EQ(store.tailGeneration(), 3u);

    expectRecord(store, 1, std::string(1, 'X'));
    expectRecord(store, 2, kP2);
    expectRecord(store, 3, kP3);
    expectRecord(store, 4, kP4);
    checkTracking(store);

    expectRecordsAfterRemount(rwCfg, {{1, std::string(1,'X')}, {2, kP2}, {3, kP3}, {4, kP4}});
}

TEST_CASE("rewrite: rewriteFront payload survives subrange compaction and remount") {
    // Same maxSegmentBytes=300 setup; build two sealed ranges so we can compact a prefix subrange.
    //
    // storeEnqueue(1..8): gen=1 sealed (seq=1..3 + REWRITE seq=1). gen=2 sealed (seq=4..6).
    //   gen=3 (tail) has seq=7..8. ranges=[{1,2}], tail=3.
    // compactRange({1,1}): prefix of {1,2}. live=[seq=1("X"),seq=2,seq=3]. Dead=84B.
    // Output gen=4. ranges=[{4,4},{2,2}], tail=3. seq=1 payload="X" after remount.
    resetSpool();
    auto rwCfg = makeRewriteCompactionConfig();
    pqueue::AppendLogStore store(rwCfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, kP1);
    storeEnqueue(store, 2, kP2);
    CHECK(store.rewriteRecord(1, std::string(1, 'X')).ok()); // REWRITE in gen=1
    storeEnqueue(store, 3, kP3);
    storeEnqueue(store, 4, kP4); // 297+84>300 -> rotate gen=1. ranges=[{1,1}], tail=2.
    storeEnqueue(store, 5, kP5);
    storeEnqueue(store, 6, kP6); // fills gen=2 (104+84=188B)
    storeEnqueue(store, 7, kP7); // 272+84=356>300 -> rotate gen=2; seq=7 in gen=3. ranges=[{1,2}], tail=3.
    storeEnqueue(store, 8, kP8); // seq=8 in gen=3: 104+84=188B.

    REQUIRE_EQ(store.manifestRanges().size(), 1u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 1u);
    CHECK_EQ(store.manifestRanges()[0].endGen,   2u);
    CHECK_EQ(store.tailGeneration(), 3u);

    // compact prefix {1,1}: dead bytes from original ENQUEUE seq=1; live=seq=1("X"),seq=2,seq=3
    const auto st = store.compactRange({1, 1});
    CHECK(st.ok());
    CHECK_FALSE(st.isNoOp());

    REQUIRE_EQ(store.manifestRanges().size(), 2u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 4u);
    CHECK_EQ(store.manifestRanges()[0].endGen,   4u);
    CHECK_EQ(store.manifestRanges()[1].startGen, 2u);
    CHECK_EQ(store.manifestRanges()[1].endGen,   2u);
    CHECK_EQ(store.tailGeneration(), 3u);

    expectRecord(store, 1, std::string(1, 'X'));
    expectRecord(store, 4, kP4);
    checkTracking(store);

    expectRecordsAfterRemount(rwCfg, {
        {1, std::string(1,'X')}, {2,kP2}, {3,kP3},
        {4,kP4}, {5,kP5}, {6,kP6}, {7,kP7}, {8,kP8}
    });
}

TEST_CASE("tail dep: rewrite event in tail suppresses rotate-before-compact (cross-range)") {
    // Only a cross-range REWRITE dep suppresses rotate. Dead bytes in gen=4 come from a
    // pre-planted REWRITE superseding the original ENQUEUE (not a current-session event),
    // so activeTailAffectedGenerations_ contains only the dep on gen=1 -- the sole out-of-range dep.
    //
    // Plant: ranges=[{1,4}], tail=5, nextGen=6.
    //   gen=1..3: ENQUEUE(seq=1..3). gen=5: ENQUEUE(seq=5) [tail].
    //   gen=4: ENQUEUE(seq=4,kP4=60B) + REWRITE(seq=4,"D"=1B). 129B total.
    //     Mount applies REWRITE by sequence: seq=4.segGen=4, payload="D". Dead=84B (original ENQUEUE).
    //
    // Mount: records={seq=1(gen=1),seq=2(gen=2),seq=3(gen=3),seq=4(gen=4,"D"),seq=5(gen=5)}.
    //   tracking=false, deps={}.
    //
    // storeEnqueue(6, kP6): 104+84=188>185 -> rotate gen=5 sealed; ranges=[{1,5}], tail=6.
    //   tracking=true, deps={}.
    //
    // rewriteRecord(1, "R"): REWRITE(25B) in gen=6: 104+25=129B. seq=1.segGen=1->6. deps={1}.
    //
    // compactRange({4,5}):
    //   suffix of {1,5}. gen=4: 84B dead (original ENQUEUE superseded). gen=5: live (seq=5).
    //   Live records in [4,5]: seq=4("D",1B) + seq=5(kP5,60B). Both fit in one output segment.
    //   tailDepsContained: gen=1 not in [4,6] -> FALSE. Rotate suppressed.
    //   Output gen=7. ranges=[{1,3},{7,7}], tail=6.
    plantLayout({
        .ranges = {{1, 4}},
        .tail   = 5,
        .next   = 6,
        .segments = {
            {.gen=1, .firstSeq=1, .body=serializeEnqueueEvent(1, kP1)},
            {.gen=2, .firstSeq=2, .body=serializeEnqueueEvent(2, kP2)},
            {.gen=3, .firstSeq=3, .body=serializeEnqueueEvent(3, kP3)},
            {.gen=4, .firstSeq=4, .body=serializeEnqueueEvent(4, kP4) + serializeRewriteEvent(4, std::string(1,'D'))},
            {.gen=5, .firstSeq=5, .body=serializeEnqueueEvent(5, kP5)},
        }
    });

    pqueue::AppendLogStore store(makeSubrangeConfig());
    CHECK(store.mount().ok());
    CHECK_EQ(store.tailGeneration(), 5u);

    storeEnqueue(store, 6, kP6); // 104+84=188>185 -> rotate gen=5 sealed; tail=6, tracking=true
    REQUIRE_EQ(store.manifestRanges().size(), 1u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 1u);
    CHECK_EQ(store.manifestRanges()[0].endGen,   5u);
    CHECK_EQ(store.tailGeneration(), 6u);

    // REWRITE seq=1 (segGen=1, cross-range) into gen=6: sole active-tail dep
    CHECK(store.rewriteRecord(1, std::string(1, 'R')).ok());

    const auto st = store.compactRange({4, 5});
    CHECK(st.ok());
    CHECK_FALSE(st.isNoOp());

    // rotate must NOT have fired -- cross-range REWRITE dep on gen=1 suppresses it
    CHECK_EQ(store.tailGeneration(), 6u);

    REQUIRE_EQ(store.manifestRanges().size(), 2u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 1u);
    CHECK_EQ(store.manifestRanges()[0].endGen,   3u);
    CHECK_EQ(store.manifestRanges()[1].startGen, 7u);
    CHECK_EQ(store.manifestRanges()[1].endGen,   7u);

    CHECK(std::filesystem::exists(segmentPath(1)));
    CHECK(std::filesystem::exists(segmentPath(2)));
    CHECK(std::filesystem::exists(segmentPath(3)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(4)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(5)));
    CHECK(std::filesystem::exists(segmentPath(6)));
    CHECK(std::filesystem::exists(segmentPath(7)));

    expectRecord(store, 4, std::string(1,'D'));
    expectRecord(store, 5, kP5);
    checkTracking(store);

    expectRecordsAfterRemount(makeSubrangeConfig(), {
        {1, std::string(1,'R')}, {2, kP2}, {3, kP3},
        {4, std::string(1,'D')}, {5, kP5}, {6, kP6}
    });
}

TEST_CASE("tail dep: contained rewrite dependency allows rotate-before-compact") {
    // When all active-tail deps fall within the compaction input range, rotate fires.
    // The REWRITE dependency (old segGen=4, current location gen=6) is contained within [4,6];
    // subsequent POP deps (poppedGen=6, poppedGen=4) are also contained. tailDepsContained=TRUE.
    //
    // Plant: ranges=[{1,4}], tail=5, nextGen=6.
    //   gen=1..3: header-only. gen=4: enqueue(1,kP1)+enqueue(2,kP2)=188B.
    //   gen=5: enqueue(3,kP3)=104B (tail).
    //
    // Mount + storeEnqueue(4,kP4): 104+84=188>185 -> rotate gen=5 sealed; ranges=[{1,5}], tail=6.
    //   tracking=true, deps={}.
    //
    // rewriteRecord(1, "R"): seq=1 in gen=4. REWRITE event=25B in gen=6. 104+25=129<=185.
    //   deps={4}. seq=1->gen=6.
    // storePop seq=1 (poppedGen=6): gen=6=149B. deps={4,6}.
    // storePop seq=2 (poppedGen=4): gen=6=169B. deps={4,6}.
    //
    // compactRange({4,5}):
    //   suffix of {1,5}. gen=4 dead(seq=1 rewritten+seq=2 popped), gen=5 live(seq=3).
    //   tailDepsContained: {4,6} in [4,6] -> TRUE. wouldRotate=TRUE. Rotate fires!
    //   gen=6 sealed -> merge {1,6}. tail=7. inputRange extended to {4,6}.
    //   Live in {4,6}: seq=3(gen=5), seq=4(gen=6,kP4). Output: gen=8(seq=3), gen=9(seq=4).
    //   Result: ranges=[{1,3},{8,9}], tail=7.
    plantLayout({
        .ranges = {{1, 4}},
        .tail   = 5,
        .next   = 6,
        .segments = {
            {.gen=1, .firstSeq=0, .body={}},
            {.gen=2, .firstSeq=0, .body={}},
            {.gen=3, .firstSeq=0, .body={}},
            {.gen=4, .firstSeq=1, .body=serializeEnqueueEvent(1, kP1) + serializeEnqueueEvent(2, kP2)},
            {.gen=5, .firstSeq=3, .body=serializeEnqueueEvent(3, kP3)},
        }
    });

    pqueue::AppendLogStore store(makeSubrangeConfig());
    CHECK(store.mount().ok());
    CHECK_EQ(store.tailGeneration(), 5u);

    storeEnqueue(store, 4, kP4); // 104+84>185 -> rotate gen=5 sealed; seq=4 in gen=6
    REQUIRE_EQ(store.manifestRanges().size(), 1u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 1u);
    CHECK_EQ(store.manifestRanges()[0].endGen,   5u);
    CHECK_EQ(store.tailGeneration(), 6u);

    // REWRITE seq=1 (in gen=4) into gen=6: creates dep={4}, not a POP
    CHECK(store.rewriteRecord(1, std::string(1, 'R')).ok());
    storePop(store); // seq=1 (poppedGen=6): deps={4,6}
    storePop(store); // seq=2 (poppedGen=4): deps={4,6}

    const auto st = store.compactRange({4, 5});
    CHECK(st.ok());
    CHECK_FALSE(st.isNoOp());

    // rotate MUST have fired
    CHECK_EQ(store.tailGeneration(), 7u);

    REQUIRE_EQ(store.manifestRanges().size(), 2u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 1u);
    CHECK_EQ(store.manifestRanges()[0].endGen,   3u);
    CHECK_EQ(store.manifestRanges()[1].startGen, 8u);
    CHECK_EQ(store.manifestRanges()[1].endGen,   9u);

    CHECK_FALSE(std::filesystem::exists(segmentPath(4)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(5)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(6)));
    CHECK(std::filesystem::exists(segmentPath(8)));
    CHECK(std::filesystem::exists(segmentPath(9)));

    expectRecord(store, 3, kP3);
    expectRecord(store, 4, kP4);
    checkTracking(store);
}

TEST_CASE("rewrite: rewriteFront into newer segment; compact+remount preserves FIFO order (regression)") {
    // Regression: collectLiveRecords iterated by segment generation (std::map order), so a
    // rewrite that moved seq=1's segmentGeneration past seq=2 and seq=3 caused the compacted
    // output to be written in generation order [B,X,C] instead of FIFO order [X,B,C].
    // After remount, records_.front().sequence was 2 ("B") instead of 1 ("X").
    //
    // maxSegmentBytes=45: exactly one 1-byte record per segment.
    //   enqueue(1,"A"): gen=1 full -> rotate. enqueue(2,"B"): gen=2 full -> rotate.
    //   enqueue(3,"C"): gen=3 (tail). rewriteRecord(1,"X"): REWRITE in gen=3 -> seq=1.segGen=3.
    //   rotate-before-compact seals gen=3; range {1,3}: byGen={2:[B], 3:[X,C]}.
    //   Without FIFO sort output=[B,X,C]. Fix: sort by sequence -> [X,B,C].
    resetSpool();
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 45;
    {
        pqueue::AppendLogStore store(cfg);
        CHECK(store.mount().ok());
        storeEnqueue(store, 1, "A");
        storeEnqueue(store, 2, "B");
        storeEnqueue(store, 3, "C");
        CHECK(store.rewriteRecord(1, "X").ok());
        const auto result = store.compactIdle(16);
        CHECK(result.status.ok());
        CHECK_GT(result.compactions, 0u);
    }

    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());
    pqueue::QueueIndex idx;
    CHECK(store.readIndex(idx).ok());
    CHECK_EQ(idx.head, 1u); // must be seq=1 ("X"), not seq=2 ("B")

    std::string out;
    CHECK(store.readRecord(1, out).ok()); CHECK_EQ(out, "X");
    CHECK(store.readRecord(2, out).ok()); CHECK_EQ(out, "B");
    CHECK(store.readRecord(3, out).ok()); CHECK_EQ(out, "C");
}

TEST_CASE("rewrite: pop + rewriteRecord + compact + remount preserves FIFO order (regression)") {
    // Regression companion to the collectLiveRecords FIFO-sort fix.
    // After pop() removes the front, rewriteRecord(2,"X") moves seq=2
    // into the tail segment (gen=4), which has a higher generation than seq=3
    // (gen=3). Without the sort, collectLiveRecords returns live records in
    // segment-generation order: [C(gen3), X(gen4), D(gen4)], producing wrong
    // FIFO output [C, X, D] after compaction. With the fix, records are sorted
    // by sequence: [X(seq=2), C(seq=3), D(seq=4)].
    //
    // maxSegmentBytes=45: each 1-byte record fills exactly one segment.
    //   enqueue(1..4): ranges=[{1,3}], tail=4.
    //   pop seq=1: dead in gen=1; pop event lands in gen=4.
    //   rewriteRecord(2,"X"): REWRITE in gen=4; seq=2.segGen becomes 4.
    //   Without sort: byGen={gen=3:[C], gen=4:[X,D]} -> compact output [C,X,D].
    //   With fix: sort by seq -> [X,C,D].
    resetSpool();
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 45;
    {
        pqueue::AppendLogStore store(cfg);
        CHECK(store.mount().ok());
        storeEnqueue(store, 1, "A");
        storeEnqueue(store, 2, "B");
        storeEnqueue(store, 3, "C");
        storeEnqueue(store, 4, "D"); // ranges=[{1,3}], tail=4
        storePop(store);             // seq=1 dead in gen=1; pop tombstone in gen=4
        CHECK(store.rewriteRecord(2, "X").ok()); // seq=2.segGen: gen=2 -> gen=4
        const auto result = store.compactIdle(16);
        CHECK(result.status.ok());
        CHECK_GT(result.compactions, 0u);
    }
    expectRecordsAfterRemount(cfg, {{2,"X"},{3,"C"},{4,"D"}});
}

TEST_CASE("rewrite: index head and ordinals are correct after rewrite+compact+remount") {
    // After a rewrite moves a record into a newer segment, compaction, and remount:
    // - the index head must still point at the lowest live sequence
    // - reading by ordinal (head, head+1, head+2, ...) must yield the correct payloads
    // This validates that the index is internally consistent with the compacted segment layout.
    //
    // Setup is the same as the pop+rewrite regression above: after compaction the live
    // records are seq=2("X"), seq=3("C"), seq=4("D"), so head=2, count=3.
    resetSpool();
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 45;
    {
        pqueue::AppendLogStore store(cfg);
        CHECK(store.mount().ok());
        storeEnqueue(store, 1, "A");
        storeEnqueue(store, 2, "B");
        storeEnqueue(store, 3, "C");
        storeEnqueue(store, 4, "D");
        storePop(store);
        CHECK(store.rewriteRecord(2, "X").ok());
        const auto result = store.compactIdle(16);
        CHECK(result.status.ok());
        CHECK_GT(result.compactions, 0u);
    }

    struct Expected { std::uint32_t seq; const char* payload; };
    const Expected expected[] = {{2,"X"},{3,"C"},{4,"D"}};

    auto checkOrdinals = [&](pqueue::AppendLogStore& s) {
        pqueue::QueueIndex idx;
        CHECK(s.readIndex(idx).ok());
        CHECK_EQ(idx.head, 2u);
        CHECK_EQ(idx.count, 3u);
        for (std::uint32_t ordinal = 0; ordinal < 3u; ++ordinal) {
            const std::uint32_t seq = idx.head + ordinal;
            CHECK_EQ(seq, expected[ordinal].seq);
            std::string out;
            CHECK(s.readRecord(seq, out).ok());
            CHECK_EQ(out, std::string(expected[ordinal].payload));
        }
    };

    pqueue::AppendLogStore s1(cfg);
    CHECK(s1.mount().ok());
    checkOrdinals(s1);

    pqueue::AppendLogStore s2(cfg);
    CHECK(s2.mount().ok());
    checkOrdinals(s2);
}

// Randomized model test (rewrite+compact+remount FIFO) deferred -- scope too broad for this patch.
// Future task: start with no compaction, add remount, then add compactIdle.
// Every operation must REQUIRE before mutating model; diagnostic output required from the start.

// ---------------------------------------------------------------------------
// Transition-matrix tests
//
// One focused test per operation-sequence transition. Every test asserts
// payload FIFO order after remount. maxSegmentBytes is kept tiny where useful
// so records cross segment boundaries, exercising sequence-sort in
// collectLiveRecords.
// ---------------------------------------------------------------------------

TEST_CASE("transition: pop → compact → remount preserves FIFO order") {
    // maxSegmentBytes=70: header(20) + 2×enqueue(25) = 70B → sealed after 2 records.
    // enqueue(1,2): gen=1 sealed (70B); enqueue(3): gen=2 tail (45B).
    // storePop: pop event(20B) in gen=2 → 65B, no rotation; seq=1 dead.
    // compactOneSegment: range {1,1} has dead bytes; rotate-before-compact fires
    // (gen=2 contiguous, dep={1} ⊆ [1,2]); gen=3 tail created.
    // Remount: {2,"b"},{3,"c"}.
    resetSpool();
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a");
    storeEnqueue(store, 2, "b");
    storeEnqueue(store, 3, "c"); // gen=1 sealed (seq=1,2); gen=2 tail (seq=3)
    storePop(store);              // seq=1 dead; pop tombstone in gen=2 (65B, no rotation)

    const auto st = store.compactOneSegment();
    CHECK(st.ok());
    CHECK_FALSE(st.isNoOp());

    expectRecordsAfterRemount(cfg, {{2,"b"},{3,"c"}});
}

TEST_CASE("transition: rewrite → compact → remount preserves FIFO order") {
    // The rewritten payload must survive compaction and appear first in FIFO order
    // after remount. rotate-before-compact fires because tail (gen=2) is contiguous.
    resetSpool();
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70; // 20B header + 2×25B enqueue → gen fills after 2 records
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a");
    storeEnqueue(store, 2, "b");
    storeEnqueue(store, 3, "c"); // gen=1 full (seq=1,2), gen=2 tail (seq=3)
    CHECK(store.rewriteRecord(1, "R").ok()); // REWRITE in gen=2; dead bytes in gen=1

    CHECK(store.compactOneSegment().ok());
    expectRecordsAfterRemount(cfg, {{1,"R"},{2,"b"},{3,"c"}});
}

TEST_CASE("transition: pop → rewrite → compact → remount preserves FIFO order") {
    // Pop the front record, then rewrite the new front. Compact must sort live
    // records by sequence, not by source-segment generation.
    //
    // maxSegmentBytes=45: header(20)+enqueue(25)=45B → each enqueue fills its own segment.
    //   enqueue(1..4): gen=1(seq=1), gen=2(seq=2), gen=3(seq=3) sealed; gen=4 tail(seq=4).
    //   ranges=[{1,3}].
    //   storePop: pop event(20B) overflows gen=4 (45+20=65>45) → rotate! gen=4 sealed
    //   into {1,4}; gen=5 created (fresh tail). Pop event in gen=5 → gen=5=40B. dep={1}.
    //   rewrite(2,"X"): REWRITE(25B) overflows gen=5 (40+25=65>45) → rotate! gen=5 sealed
    //   into {1,5}; gen=6 created. REWRITE in gen=6 → gen=6=45B. seq=2.segGen=6; dep={1,2}.
    //   compactOneSegment: range {1,5}; wouldRotate (gen=6 contiguous, dep={1,2}⊆[1,6]).
    //   Rotate → gen=6 sealed into {1,6}; gen=7 tail. Live: seq=2("X"),3("C"),4("D").
    //   3 output segs. Remount: {2,"X"},{3,"C"},{4,"D"}.
    resetSpool();
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 45;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "A"); // gen=1 sealed (45B)
    storeEnqueue(store, 2, "B"); // gen=2 sealed (45B)
    storeEnqueue(store, 3, "C"); // gen=3 sealed (45B)
    storeEnqueue(store, 4, "D"); // gen=4 tail (45B); ranges=[{1,3}]
    storePop(store);              // overflows gen=4 → gen=4 sealed into {1,4}; pop in gen=5
    CHECK(store.rewriteRecord(2, "X").ok()); // overflows gen=5 → gen=5 sealed; REWRITE in gen=6

    CHECK(store.compactOneSegment().ok());
    expectRecordsAfterRemount(cfg, {{2,"X"},{3,"C"},{4,"D"}});
}

TEST_CASE("transition: rewrite → pop → compact → remount preserves FIFO order") {
    // Rewrite the front record to move its live bytes into the tail, then pop it.
    // The popped record must not appear after compaction; the remaining records
    // must be in correct FIFO order after remount.
    //
    // maxSegmentBytes=100: header(20)+3×enqueue(25)=95B < 100 → all three fit in gen=1.
    // rewrite(1,"R"): 95+25=120>100 → rotate gen=1 (sealed, 95B); gen=2 created (20B).
    //   REWRITE event in gen=2 → gen=2=45B. seq=1.segGen=2; dep={1} (old segGen).
    // storePop: pop event(20B) in gen=2 → gen=2=65B < 100, no rotation.
    //   poppedGen=gen=2 (seq=1's current segGen) → dep={1,2}.
    // State: ranges=[{1,1}] (gen=1 sealed by the rewrite rotation), tail=2.
    // compactOneSegment: {1,1} has dead bytes (seq=1's original ENQUEUE superseded+popped).
    //   wouldRotate (dep={1,2}⊆[1,2]); rotate → gen=2 sealed into {1,2}; gen=3 tail.
    //   Live: seq=2(gen=1,"b"), seq=3(gen=1,"c") → 1 output seg (gen=4).
    //   Remount: {2,"b"},{3,"c"}.
    resetSpool();
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 100;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a");
    storeEnqueue(store, 2, "b");
    storeEnqueue(store, 3, "c"); // all in gen=1 (95B); no rotation during enqueues
    CHECK(store.rewriteRecord(1, "R").ok()); // 95+25>100 → rotate gen=1; REWRITE in gen=2
    storePop(store);                          // pop seq=1; gen=2=65B; dep={1,2}

    REQUIRE_EQ(store.manifestRanges().size(), 1u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 1u);
    CHECK_EQ(store.manifestRanges()[0].endGen,   1u);
    CHECK_EQ(store.tailGeneration(), 2u);

    CHECK(store.compactOneSegment().ok());
    expectRecordsAfterRemount(cfg, {{2,"b"},{3,"c"}});
}

TEST_CASE("transition: rewrite old/front → rotate tail → compact → remount preserves FIFO order") {
    // Rewrite a sealed record (seq=1 in gen=1) into the current tail (gen=2),
    // then enqueue enough to fill and rotate gen=2, sealing the rewrite into the
    // range. Compact must produce the rewritten value at the correct FIFO position
    // after remount, not at the position of the source segment.
    //
    // maxSegmentBytes=70: enqueue(1): gen=1=45B; enqueue(2): gen=1=70B (not >70, no rotation);
    //   enqueue(3): 70+25=95>70 → rotate gen=1→{1,1}; gen=2=45B.
    //   rewrite(1,"R"): 45+25=70B (not >70, no rotation); gen=2=70B; dep={1}.
    //   enqueue(4): 70+25=95>70 → rotate gen=2 → merged into {1,2}; gen=3 created (fresh
    //   tail, dep reset to {}). enqueue(4) in gen=3: gen=3=45B.
    //   compactOneSegment: range {1,2} has dead bytes (seq=1's original ENQUEUE superseded).
    //   wouldRotate: gen=3 contiguous, dep={} (fresh tail) → vacuously true → rotate fires.
    //   gen=3 sealed → {1,3}; gen=4 tail. live={1("R"),2("b"),3("c"),4("d")} → 2 output segs.
    resetSpool();
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a");             // gen=1: 45B
    storeEnqueue(store, 2, "b");             // gen=1: 70B (not >70, no rotation yet)
    storeEnqueue(store, 3, "c");             // 95>70 → rotate gen=1→{1,1}; gen=2 tail: 45B
    CHECK(store.rewriteRecord(1, "R").ok()); // 70B (not >70); gen=2=70B; dep={1} in gen=2
    storeEnqueue(store, 4, "d");             // 95>70 → rotate gen=2→{1,2}; gen=3 fresh tail

    REQUIRE_EQ(store.manifestRanges().size(), 1u);
    CHECK_EQ(store.manifestRanges()[0].startGen, 1u);
    CHECK_EQ(store.manifestRanges()[0].endGen,   2u);
    CHECK_EQ(store.tailGeneration(), 3u);

    CHECK(store.compactOneSegment().ok());
    expectRecordsAfterRemount(cfg, {{1,"R"},{2,"b"},{3,"c"},{4,"d"}});
}

TEST_CASE("transition: subrange compact → remount preserves FIFO order (live suffix)") {
    // Suffix subrange compaction on a partially-dead range.
    // Pop seq=1,2,3 making records in gens 1-3 dead; compact suffix {3,4} (gen=3 dead,
    // gen=4 live). After remount the manifest must have two ranges and the
    // live records {seq=4,kP4} and {seq=5,kP5} must be accessible in FIFO order.
    resetSpool();
    pqueue::AppendLogStore store(makeSubrangeConfig());
    CHECK(store.mount().ok());
    setupSubrangeStore(store); // seqs 1-5; ranges=[{1,4}], tail=5, next=6
    storePop(store); storePop(store); storePop(store); // seq=1,2,3 dead

    const auto st = store.compactRange({3, 4});
    CHECK(st.ok());
    CHECK_FALSE(st.isNoOp());
    REQUIRE_EQ(store.manifestRanges().size(), 2u); // {1,2} + {6,6}

    expectRecordsAfterRemount(makeSubrangeConfig(), {{4,kP4},{5,kP5}});
}

TEST_CASE("transition: full compact (compactFull) → remount preserves FIFO order") {
    // Three dead records (seq=1,2,3 popped) and two live records (seq=4,5).
    // compactFull must collapse the dead portions of the range and the rotation
    // triggered by rotate-before-compact; remount must see only the two live
    // records in correct FIFO order.
    resetSpool();
    pqueue::AppendLogStore store(makeSubrangeConfig());
    CHECK(store.mount().ok());
    setupSubrangeStore(store); // seqs 1-5; ranges=[{1,4}], tail=5, next=6
    storePop(store); storePop(store); storePop(store); // seq=1,2,3 dead

    CHECK(store.compactFull().ok());
    expectRecordsAfterRemount(makeSubrangeConfig(), {{4,kP4},{5,kP5}});
}

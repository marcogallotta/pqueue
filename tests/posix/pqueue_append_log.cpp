#include "pqueue_append_log_support.h"

TEST_CASE("append-log: starts empty") {
    cleanSpool();
    pqueue::Queue q(makeConfig());
    std::string out;
    CHECK_FALSE(q.peek(out).ok());
    CHECK_EQ(q.stats().count, 0U);
}

TEST_CASE("append-log: basic enqueue and peek") {
    cleanSpool();
    pqueue::Queue q(makeConfig());
    CHECK(q.enqueue("hello").ok());
    std::string out;
    CHECK(q.peek(out).ok());
    CHECK_EQ(out, "hello");
    CHECK_EQ(q.stats().count, 1U);
}

TEST_CASE("append-log: FIFO order preserved") {
    cleanSpool();
    pqueue::Queue q(makeConfig());
    CHECK(q.enqueue("one").ok());
    CHECK(q.enqueue("two").ok());
    CHECK(q.enqueue("three").ok());
    CHECK_EQ(q.stats().count, 3U);

    std::string out;
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "one");
    CHECK(q.pop().ok());
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "two");
    CHECK(q.pop().ok());
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "three");
    CHECK(q.pop().ok());
    CHECK_EQ(q.stats().count, 0U);
}

TEST_CASE("append-log: persistence across remount") {
    cleanSpool();
    auto cfg = makeConfig();
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("alpha").ok());
        CHECK(q.enqueue("beta").ok());
    }
    {
        pqueue::Queue q(cfg);
        std::string out;
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "alpha");
        CHECK(q.pop().ok());
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "beta");
        CHECK(q.pop().ok());
        CHECK_EQ(q.stats().count, 0U);
    }
}

TEST_CASE("append-log: pop persists across remount") {
    cleanSpool();
    auto cfg = makeConfig();
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("keep").ok());
        CHECK(q.enqueue("discard").ok());
        CHECK(q.pop().ok()); // FIFO: pops "keep", "discard" remains
    }
    {
        pqueue::Queue q(cfg);
        std::string out;
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "discard");
        CHECK_EQ(q.stats().count, 1U);
    }
}

TEST_CASE("append-log: segment rotation") {
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 256;
    pqueue::Queue q(cfg);

    for (int i = 0; i < 20; ++i)
        CHECK(q.enqueue("record-" + std::to_string(i)).ok());
    CHECK_EQ(q.stats().count, 20U);

    for (int i = 0; i < 20; ++i) {
        std::string out;
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "record-" + std::to_string(i));
        CHECK(q.pop().ok());
    }
    CHECK_EQ(q.stats().count, 0U);
}
TEST_CASE("append-log: format clears all records") {
    cleanSpool();
    pqueue::Queue q(makeConfig());
    CHECK(q.enqueue("a").ok());
    CHECK(q.enqueue("b").ok());
    CHECK(q.format().ok());
    CHECK_EQ(q.stats().count, 0U);
    std::string out;
    CHECK_FALSE(q.peek(out).ok());
}

TEST_CASE("append-log: validate detects corrupt CRC in non-last segment") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 128;
    {
        pqueue::Queue q(cfg);
        for (int i = 0; i < 6; ++i)
            CHECK(q.enqueue("X").ok());
    }
    REQUIRE(std::filesystem::exists(segmentPath(2)));
    constexpr std::uintmax_t kCrcOffset = kSegmentHeaderBytes + kEnqueueHeaderBytes + 1;
    patchFile(segmentPath(1), kCrcOffset, {0xDE, 0xAD, 0xBE, 0xEF});
    pqueue::Queue q(cfg);
    const auto result = q.validate();
    CHECK_FALSE(result.ok);
    CHECK_FALSE(result.errors.empty());
}

TEST_CASE("append-log: compaction triggered by segment count") {
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 128;
    cfg.maxSegments = 3;
    pqueue::Queue q(cfg);

    std::vector<std::string> expected;
    for (int i = 0; i < 30; ++i) {
        const std::string rec = "record-" + std::to_string(i);
        CHECK(q.enqueue(rec).ok());
        expected.push_back(rec);
    }
    CHECK_EQ(q.stats().count, 30U);

    for (int i = 0; i < 30; ++i) {
        std::string out;
        CHECK(q.peek(out).ok()); CHECK_EQ(out, expected[i]);
        CHECK(q.pop().ok());
    }
}

TEST_CASE("compaction trigger: remount after auto-compaction preserves FIFO order (critical)") {
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 128;
    cfg.maxSegments = 3;

    std::vector<std::string> expected;
    {
        pqueue::Queue q(cfg);
        for (int i = 0; i < 30; ++i) {
            const std::string rec = "record-" + std::to_string(i);
            REQUIRE(q.enqueue(rec).ok());
            expected.push_back(rec);
        }
        REQUIRE_EQ(q.stats().count, 30u);
    }
    {
        pqueue::Queue q(cfg);
        CHECK_EQ(q.stats().count, 30u);
        for (int i = 0; i < 30; ++i) {
            std::string out;
            CHECK(q.peek(out).ok()); CHECK_EQ(out, expected[i]);
            CHECK(q.pop().ok());
        }
        std::string out;
        CHECK_FALSE(q.peek(out).ok());
    }
}

TEST_CASE("compaction trigger: auto-triggers on size-based count after non-monotonic compaction") {
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 128;
    cfg.maxSegments = 2;

    std::vector<std::string> expected;
    {
        pqueue::Queue q(cfg);
        for (int i = 0; i < 30; ++i) {
            const std::string rec = "msg-" + std::to_string(i);
            REQUIRE(q.enqueue(rec).ok());
            expected.push_back(rec);
        }
        CHECK_EQ(q.stats().count, 30u);
    }
    {
        pqueue::Queue q(cfg);
        CHECK_EQ(q.stats().count, 30u);
        for (int i = 0; i < 30; ++i) {
            std::string out;
            CHECK(q.peek(out).ok()); CHECK_EQ(out, expected[i]);
            CHECK(q.pop().ok());
        }
    }
}


// --- Recovery policy tests ---

TEST_CASE("append-log: corrupt payloadBytes at tail of last segment is recoverable") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("keep").ok());
        CHECK(q.enqueue("torn").ok());
    }
    constexpr std::uintmax_t kEventSize = kEnqueueHeaderBytes + 4 + kEventTrailerBytes;
    constexpr std::uintmax_t kPayloadBytesOffset = kSegmentHeaderBytes + kEventSize + kEnqueueHeaderBytes - 4;
    patchFile(segmentPath(1), kPayloadBytesOffset, {0xFF, 0xFF, 0xFF, 0xFF});
    {
        pqueue::Queue q(cfg);
        CHECK_EQ(q.stats().count, 1U);
        std::string out;
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "keep");
    }
}

TEST_CASE("append-log: corrupt payloadBytes in non-last segment causes mount failure") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 128;
    {
        pqueue::Queue q(cfg);
        for (int i = 0; i < 6; ++i)
            CHECK(q.enqueue("X").ok());
    }
    REQUIRE(std::filesystem::exists(segmentPath(2)));
    constexpr std::uintmax_t kPayloadBytesOffset = kSegmentHeaderBytes + kEnqueueHeaderBytes - 4;
    patchFile(segmentPath(1), kPayloadBytesOffset, {0xFF, 0xFF, 0xFF, 0xFF});
    {
        pqueue::Queue q(cfg);
        std::string out;
        const auto st = q.peek(out);
        CHECK_FALSE(st.ok());
        CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
    }
}

TEST_CASE("append-log: torn tail on active segment is recoverable") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("A").ok());
        CHECK(q.enqueue("B").ok());
        CHECK(q.enqueue("C").ok());
    }
    constexpr std::uintmax_t kEventSize = kEnqueueHeaderBytes + 1 + kEventTrailerBytes;
    const std::uintmax_t tornOffset = kSegmentHeaderBytes + 2 * kEventSize + 5;
    std::filesystem::resize_file(segmentPath(1), tornOffset);
    {
        pqueue::Queue q(cfg);
        CHECK_EQ(q.stats().count, 2U);
        std::string out;
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "A");
        CHECK(q.pop().ok());
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "B");
    }
}

TEST_CASE("append-log: corrupt CRC at tail of last segment is recoverable") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("keep").ok());
        CHECK(q.enqueue("torn").ok());
    }
    constexpr std::uintmax_t kEventSize = kEnqueueHeaderBytes + 4 + kEventTrailerBytes;
    constexpr std::uintmax_t kCrcOffset = kSegmentHeaderBytes + kEventSize + kEnqueueHeaderBytes + 4;
    patchFile(segmentPath(1), kCrcOffset, {0xDE, 0xAD, 0xBE, 0xEF});
    {
        pqueue::Queue q(cfg);
        CHECK_EQ(q.stats().count, 1U);
        std::string out;
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "keep");
    }
}

TEST_CASE("append-log: corrupt CRC in non-last segment causes mount failure") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 128;
    {
        pqueue::Queue q(cfg);
        for (int i = 0; i < 6; ++i)
            CHECK(q.enqueue("X").ok());
    }
    REQUIRE(std::filesystem::exists(segmentPath(2)));
    constexpr std::uintmax_t kCrcOffset = kSegmentHeaderBytes + kEnqueueHeaderBytes + 1;
    patchFile(segmentPath(1), kCrcOffset, {0xDE, 0xAD, 0xBE, 0xEF});
    {
        pqueue::Queue q(cfg);
        std::string out;
        const auto st = q.peek(out);
        CHECK_FALSE(st.ok());
        CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
    }
}

TEST_CASE("append-log: corrupt magic in non-last segment causes mount failure") {
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 128;
    {
        pqueue::Queue q(cfg);
        for (int i = 0; i < 6; ++i)
            CHECK(q.enqueue("X").ok());
    }
    REQUIRE(std::filesystem::exists(segmentPath(2)));
    patchFile(segmentPath(1), pqueue::append_log_detail::kSegmentHeaderBytes,
              {0xDE, 0xAD, 0xBE, 0xEF});
    {
        pqueue::Queue q(cfg);
        std::string out;
        const auto st = q.peek(out);
        CHECK_FALSE(st.ok());
        CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
    }
}

TEST_CASE("append-log: corrupt mid-last-segment discards tail from lastGoodOffset") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("A").ok());
        CHECK(q.enqueue("B").ok());
        CHECK(q.enqueue("C").ok());
    }
    constexpr std::uintmax_t kEventSize = kEnqueueHeaderBytes + 1 + kEventTrailerBytes;
    constexpr std::uintmax_t kBCrcOffset = kSegmentHeaderBytes + kEventSize + kEnqueueHeaderBytes + 1;
    patchFile(segmentPath(1), kBCrcOffset, {0xDE, 0xAD, 0xBE, 0xEF});
    {
        pqueue::Queue q(cfg);
        CHECK_EQ(q.stats().count, 1U);
        std::string out;
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "A");
    }
}

// --- Segment naming and generation tests ---

TEST_CASE("append-log: segment files use seg- prefix") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = kSegmentHeaderBytes + kEnqueueHeaderBytes + 1 + kEventTrailerBytes;
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("A").ok());
        CHECK(q.enqueue("B").ok());
    }
    CHECK(std::filesystem::exists(kSpoolDir / "seg-00000001.bin"));
    CHECK(std::filesystem::exists(kSpoolDir / "seg-00000002.bin"));
    CHECK_FALSE(std::filesystem::exists(kSpoolDir / "pqueue-seg-00000001.bin"));
}

TEST_CASE("append-log: torn tail in non-last segment causes DataCorrupt") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 128;
    {
        pqueue::Queue q(cfg);
        for (int i = 0; i < 6; ++i)
            CHECK(q.enqueue("X").ok());
    }
    REQUIRE(std::filesystem::exists(segmentPath(1)));
    REQUIRE(std::filesystem::exists(segmentPath(2)));
    std::filesystem::resize_file(segmentPath(1), kSegmentHeaderBytes + 5);
    {
        pqueue::Queue q(cfg);
        std::string out;
        const auto st = q.peek(out);
        CHECK_FALSE(st.ok());
        CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
    }
}

TEST_CASE("append-log: nextGeneration advances past all disk generations on remount") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = kSegmentHeaderBytes + kEnqueueHeaderBytes + 1 + kEventTrailerBytes;
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("A").ok());
        CHECK(q.enqueue("B").ok());
        CHECK(q.enqueue("C").ok());
    }
    REQUIRE(std::filesystem::exists(segmentPath(3)));
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("D").ok());
        CHECK(std::filesystem::exists(segmentPath(4)));
        CHECK_FALSE(std::filesystem::exists(segmentPath(0)));
    }
}

TEST_CASE("append-log: nextGeneration skips stray high disk generation") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = kSegmentHeaderBytes + kEnqueueHeaderBytes + 1 + kEventTrailerBytes;
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("A").ok());
        CHECK(q.enqueue("B").ok());
        CHECK(q.enqueue("C").ok());
    }
    plantSegment(10); // stray high-gen file
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("D").ok()); // fills stray seg 10
        CHECK(q.enqueue("E").ok()); // overflows → rotation to gen 11
        CHECK(std::filesystem::exists(kSpoolDir / "seg-0000000b.bin"));
        CHECK_FALSE(std::filesystem::exists(kSpoolDir / "seg-00000004.bin"));
    }
}

TEST_CASE("append-log: stale pqueue-compact.bin is ignored") {
    cleanSpool();
    auto cfg = makeConfig();
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("A").ok());
        CHECK(q.enqueue("B").ok());
    }
    {
        std::ofstream f(kSpoolDir / "pqueue-compact.bin", std::ios::binary | std::ios::trunc);
        f.write("\xDE\xAD\xBE\xEF", 4);
    }
    {
        pqueue::Queue q(cfg);
        CHECK_EQ(q.stats().count, 2U);
        std::string out;
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "A");
    }
}

// ---------------------------------------------------------------------------
// rotate-before-compact dependency guard tests
// ---------------------------------------------------------------------------

TEST_CASE("compaction: rotate-before-compact fires after remount when tail dependencies are tracked") {
    // After remount activeTailDependenciesTracked_ is true (tail rescanned at mount).
    // The tail is contiguous with the last range and the range has dead bytes, so
    // rotate-before-compact fires: the tail is sealed and included in the compaction.
    using namespace pqueue::append_log_detail;
    resetSpool();

    // Two 1-byte records fill one segment at maxSegmentBytes=70.
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;

    {
        pqueue::AppendLogStore store(cfg);
        REQUIRE(store.mount().ok());
        storeEnqueue(store, 1, "a"); // gen=1
        storeEnqueue(store, 2, "b"); // gen=1 full → rotate; gen=2 tail
        storePop(store);             // pop seq=1 (source gen=1): POP in tail gen=2
        storeEnqueue(store, 3, "c"); // goes to gen=2
    }
    // Remount: tail scan populates tracking from POP(seq=1) → affected={1}.
    pqueue::AppendLogStore store(cfg);
    REQUIRE(store.mount().ok());

    // State: ranges=[{1,1}], tail=2 (contiguous: 1+1=2). Dead bytes in gen=1.
    // tailDepsContained: gen=1 ∈ [1,2] → true → wouldRotate=true.
    REQUIRE_EQ(store.manifestRanges().size(), 1U);
    CHECK_EQ(store.manifestRanges()[0].startGen, 1U);
    CHECK_EQ(store.manifestRanges()[0].endGen,   1U);
    CHECK_EQ(store.tailGeneration(), 2U);

    // rotate gen=2 → new tail=gen=3, compact {1,2} → output gen=4.
    auto st = store.compactOneSegment();
    CHECK(st.ok());

    // Tail was rotated and a fresh tail was created.
    CHECK_EQ(store.tailGeneration(), 3U);
    // Compaction output is gen=4 (covers the rotated range {1,2}).
    REQUIRE_EQ(store.manifestRanges().size(), 1U);
    CHECK_EQ(store.manifestRanges()[0].startGen, 4U);
    CHECK_EQ(store.manifestRanges()[0].endGen,   4U);
    CHECK_FALSE(std::filesystem::exists(segmentPath(2))); // old tail rotated and compacted away

    // Live records remain readable.
    std::string out;
    CHECK(store.readRecord(2, out).ok()); CHECK_EQ(out, "b");
    CHECK(store.readRecord(3, out).ok()); CHECK_EQ(out, "c");

    // Verify durability after another remount.
    pqueue::AppendLogStore store2(cfg);
    REQUIRE(store2.mount().ok());
    CHECK(store2.readRecord(2, out).ok()); CHECK_EQ(out, "b");
    CHECK(store2.readRecord(3, out).ok()); CHECK_EQ(out, "c");
}

TEST_CASE("compaction: rotate-before-compact allows same-range tail dependencies") {
    // Tail has a POP for a generation inside the selected compaction range.
    // tailDepsContained must be true → rotate-before-compact is permitted.
    using namespace pqueue::append_log_detail;
    resetSpool();

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70; // two 1-byte records per segment

    pqueue::AppendLogStore store(cfg);
    REQUIRE(store.mount().ok());

    storeEnqueue(store, 1, "a"); // gen=1
    storeEnqueue(store, 2, "b"); // gen=1 full → rotate; gen=2 tail
    storePop(store);             // pop seq=1 (gen=1): POP event in tail gen=2; affected={1}
    storeEnqueue(store, 3, "c"); // goes to gen=2

    // State: ranges=[{1,1}], tail=2, tracking=true, affected={1}.
    // gen=1 is in [range.startGen=1, activeGen=2] → tailDepsContained=true → wouldRotate=true.
    REQUIRE_EQ(store.manifestRanges().size(), 1U);
    CHECK_EQ(store.tailGeneration(), 2U);

    auto st = store.compactOneSegment();
    CHECK(st.ok());
    CHECK_FALSE(st.isNoOp());

    // rotate fired: seals gen=2 → new tail=gen=3, compact {1,2} → output gen=4.
    REQUIRE_EQ(store.manifestRanges().size(), 1U);
    CHECK_EQ(store.manifestRanges()[0].startGen, 4U);
    CHECK_EQ(store.manifestRanges()[0].endGen,   4U);
    CHECK_EQ(store.tailGeneration(), 3U);
    CHECK(std::filesystem::exists(segmentPath(3))); // new tail
    CHECK(std::filesystem::exists(segmentPath(4))); // compacted output

    // seq=1 was popped and must remain absent.
    std::string out;
    CHECK_FALSE(store.readRecord(1, out).ok());
    CHECK(store.readRecord(2, out).ok()); CHECK_EQ(out, "b");
    CHECK(store.readRecord(3, out).ok()); CHECK_EQ(out, "c");

    // Verify FIFO order survives remount.
    pqueue::AppendLogStore store2(cfg);
    REQUIRE(store2.mount().ok());
    CHECK_FALSE(store2.readRecord(1, out).ok());
    CHECK(store2.readRecord(2, out).ok()); CHECK_EQ(out, "b");
    CHECK(store2.readRecord(3, out).ok()); CHECK_EQ(out, "c");
}

TEST_CASE("compaction: rotate-before-compact is blocked by cross-range tail pop dependency") {
    // Two manifest ranges. Tail is contiguous with the LAST range. The tail contains
    // POP events for records whose source segments are in the EARLIER range (gen=1,
    // outside the last range {3,4}). tailDepsContained must be false → rotate must
    // NOT fire; otherwise the POP tombstones could be lost and popped records could
    // resurrect after remount.
    //
    // maxSegmentBytes=150, 25-byte payloads (49-byte events): 2 records per segment.
    // New tail after rotation starts with 1 enqueue (69 bytes); 4 pop events add
    // 80 bytes (total 149 ≤ 150) — all fit without triggering a second rotation that
    // would clear activeTailAffectedGenerations_.
    using namespace pqueue::append_log_detail;

    // Plant: ranges=[{1,1},{3,3}], tail=4.
    // gen=1: seq=1,2 (25-byte payloads, fills 118-byte segment, well under 150).
    // gen=3: seq=3,4.  gen=4: empty tail.
    const std::string p1(25, 'a'), p2(25, 'b'), p3(25, 'c'), p4(25, 'd');
    const std::string p5(25, 'e'), p6(25, 'f'), p7(25, 'g');
    plantLayout({
        .ranges   = {{1u, 1u}, {3u, 3u}},
        .tail     = 4u,
        .next     = 5u,
        .segments = {
            {1u, 1u, serializeEnqueueEvent(1, p1) + serializeEnqueueEvent(2, p2)},
            {3u, 3u, serializeEnqueueEvent(3, p3) + serializeEnqueueEvent(4, p4)},
            {4u, 5u, ""},
        },
    });

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 150; // two 25-byte-payload records per segment

    {
        pqueue::AppendLogStore store(cfg);
        REQUIRE(store.mount().ok());

        // Verify planted state.
        REQUIRE_EQ(store.manifestRanges().size(), 2U);
        CHECK_EQ(store.manifestRanges()[0].startGen, 1U);
        CHECK_EQ(store.manifestRanges()[0].endGen,   1U);
        CHECK_EQ(store.manifestRanges()[1].startGen, 3U);
        CHECK_EQ(store.manifestRanges()[1].endGen,   3U);
        CHECK_EQ(store.tailGeneration(), 4U);

        // Fill gen=4 to force rotation → new tail=gen=5, tracking=true, affected={}.
        // Two enqueues fill gen=4 (20+49+49=118 ≤ 150), third overflows.
        storeEnqueue(store, 5, p5); // gen=4: 118 bytes after this
        storeEnqueue(store, 6, p6); // gen=4: still fits (118 bytes)
        storeEnqueue(store, 7, p7); // overflow: gen=4 seals → merges with {3,3} → {3,4}; new tail=gen=5

        REQUIRE_EQ(store.manifestRanges().size(), 2U);
        CHECK_EQ(store.manifestRanges()[1].startGen, 3U);
        CHECK_EQ(store.manifestRanges()[1].endGen,   4U);
        CHECK_EQ(store.tailGeneration(), 5U);

        // Pop seq=1,2 (gen=1, EARLIER range): cross-range deps → affected gains gen=1.
        // Gen=1 becomes fully dead (dead-range removal candidate).
        // Pop seq=3,4 (gen=3, last range): same-range deps; create dead bytes in {3,4}.
        // Gen=5 after pops: 20(hdr)+49(seq7)+4×20(pops)=149 ≤ 150 — no rotation.
        storePop(store); // seq=1 → gen=1
        storePop(store); // seq=2 → gen=1
        storePop(store); // seq=3 → gen=3
        storePop(store); // seq=4 → gen=3
        // affected={1,3}; gen=1 is NOT in [3, activeGen=5] → tailDepsContained=false.

        // First compactOneSegment: {1,1} fully dead → dead-range removal (not last range,
        // no rotate). After cleanup, gen=1 is pruned from activeTailAffectedGenerations_;
        // affected becomes {3}.
        auto st1 = store.compactOneSegment();
        CHECK(st1.ok());
        CHECK_FALSE(st1.isNoOp());
        REQUIRE_EQ(store.manifestRanges().size(), 1U);
        CHECK_EQ(store.manifestRanges()[0].startGen, 3U);
        CHECK_EQ(store.manifestRanges()[0].endGen,   4U);
        CHECK_EQ(store.tailGeneration(), 5U);

        // Second compactOneSegment: {3,4} is now the last range; tail=5 is contiguous
        // (4+1=5). affected={3}; gen=3 is in [3,5] → tailDepsContained=true → rotate fires.
        // Gen=1's tombstones no longer risk resurrection: gen=1 is gone from the manifest.
        // Rotate seals gen=5 into {3,5}; new tail=gen=6. Live records seq=5,6,7 → 2 output
        // segs (3×49-byte events span 118B and 167B; 167>150 → split): gen=7 (seq=5,6),
        // gen=8 (seq=7).
        auto st2 = store.compactOneSegment();
        CHECK(st2.ok());
        CHECK_FALSE(st2.isNoOp());

        CHECK_EQ(store.tailGeneration(), 6U);
        REQUIRE_EQ(store.manifestRanges().size(), 1U);
        CHECK_EQ(store.manifestRanges()[0].startGen, 7U);
        CHECK_EQ(store.manifestRanges()[0].endGen,   8U);

        // Popped records absent; live records readable.
        std::string out;
        CHECK_FALSE(store.readRecord(1, out).ok());
        CHECK_FALSE(store.readRecord(2, out).ok());
        CHECK_FALSE(store.readRecord(3, out).ok());
        CHECK_FALSE(store.readRecord(4, out).ok());
        CHECK(store.readRecord(5, out).ok()); CHECK_EQ(out, p5);
        CHECK(store.readRecord(6, out).ok()); CHECK_EQ(out, p6);
        CHECK(store.readRecord(7, out).ok()); CHECK_EQ(out, p7);
    }

    // After remount: popped records remain absent (no resurrection).
    {
        pqueue::AppendLogStore store(cfg);
        REQUIRE(store.mount().ok());

        std::string out;
        CHECK_FALSE(store.readRecord(1, out).ok());
        CHECK_FALSE(store.readRecord(2, out).ok());
        CHECK_FALSE(store.readRecord(3, out).ok());
        CHECK_FALSE(store.readRecord(4, out).ok());
        CHECK(store.readRecord(5, out).ok()); CHECK_EQ(out, p5);
        CHECK(store.readRecord(6, out).ok()); CHECK_EQ(out, p6);
        CHECK(store.readRecord(7, out).ok()); CHECK_EQ(out, p7);
    }
}

TEST_CASE("compaction: cross-range tail deps reconstructed after remount; no resurrection") {
    // Verifies the activeTailDependenciesTracked_ suppression-window fix:
    // after remount, the tail scan must reconstruct activeTailAffectedGenerations_
    // so that cross-range deps are detected and popped records do not resurrect.
    //
    // Layout: ranges=[{1,1},{3,3}], tail=4 (contiguous: 3+1=4).
    // gen=1: seq=1,2 (both dead -- popped via POP events in tail gen=4).
    // gen=3: seq=3 (live).
    // gen=4: POP(1)+POP(2) [cross-range deps on gen=1] + ENQUEUE(4,p4).
    //
    // 25-byte payloads; maxSegmentBytes=150.
    using namespace pqueue::append_log_detail;

    const std::string p1(25, 'a'), p2(25, 'b'), p3(25, 'c'), p4(25, 'd');

    plantLayout({
        .ranges   = {{1u, 1u}, {3u, 3u}},
        .tail     = 4u,
        .next     = 5u,
        .segments = {
            {1u, 1u, serializeEnqueueEvent(1, p1) + serializeEnqueueEvent(2, p2)},
            {3u, 3u, serializeEnqueueEvent(3, p3)},
            {4u, 3u, serializePopEvent(1) + serializePopEvent(2) + serializeEnqueueEvent(4, p4)},
        },
    });

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 150;

    // Remount: tail scan must reconstruct affected={1}, tracking=true.
    // Cross-range dep gen=1 is NOT in [3,4] → tailDepsContained=false → rotate suppressed if
    // {3,3} were chosen. {1,1} is fully dead → dead-range fast path chosen first (not last
    // range, no rotate risk). After dead-range removal, gen=1 is cleared from affected.
    {
        pqueue::AppendLogStore store(cfg);
        REQUIRE(store.mount().ok());

        REQUIRE_EQ(store.manifestRanges().size(), 2U);
        CHECK_EQ(store.manifestRanges()[0].startGen, 1U);
        CHECK_EQ(store.manifestRanges()[0].endGen,   1U);
        CHECK_EQ(store.manifestRanges()[1].startGen, 3U);
        CHECK_EQ(store.manifestRanges()[1].endGen,   3U);
        CHECK_EQ(store.tailGeneration(), 4U);

        auto st = store.compactOneSegment(); // removes dead range {1,1}
        CHECK(st.ok());
        CHECK_FALSE(st.isNoOp());

        // {1,1} removed; {3,3} and tail=4 remain.
        REQUIRE_EQ(store.manifestRanges().size(), 1U);
        CHECK_EQ(store.manifestRanges()[0].startGen, 3U);
        CHECK_EQ(store.manifestRanges()[0].endGen,   3U);
        CHECK_EQ(store.tailGeneration(), 4U);

        // seq=1,2 popped; seq=3,4 live.
        std::string out;
        CHECK_FALSE(store.readRecord(1, out).ok());
        CHECK_FALSE(store.readRecord(2, out).ok());
        CHECK(store.readRecord(3, out).ok()); CHECK_EQ(out, p3);
        CHECK(store.readRecord(4, out).ok()); CHECK_EQ(out, p4);
    }

    // After remount: verify seq=1,2 did not resurrect.
    {
        pqueue::AppendLogStore store(cfg);
        REQUIRE(store.mount().ok());

        std::string out;
        CHECK_FALSE(store.readRecord(1, out).ok());
        CHECK_FALSE(store.readRecord(2, out).ok());
        CHECK(store.readRecord(3, out).ok()); CHECK_EQ(out, p3);
        CHECK(store.readRecord(4, out).ok()); CHECK_EQ(out, p4);
    }
}

TEST_CASE("compaction: rotate-before-compact allows same-range tail rewrite dependency") {
    // Tail has a REWRITE for a sequence whose source segment is inside the selected
    // compaction range. tailDepsContained must be true → rotate-before-compact fires.
    using namespace pqueue::append_log_detail;
    resetSpool();

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70; // two 1-byte records per segment

    pqueue::AppendLogStore store(cfg);
    REQUIRE(store.mount().ok());

    storeEnqueue(store, 1, "a"); // gen=1
    storeEnqueue(store, 2, "b"); // gen=1 full → rotate; gen=2 tail
    // REWRITE seq=1 (in gen=1): REWRITE event appended to tail gen=2; affected={1}.
    // Use 1-byte payload so REWRITE(25 bytes) + ENQUEUE("c", 25 bytes) fills gen=2 exactly.
    REQUIRE(store.rewriteRecord(1, "A").ok());
    storeEnqueue(store, 3, "c"); // fills gen=2 exactly (20+25+25=70 bytes)

    // State: ranges=[{1,1}], tail=2, tracking=true, affected={1}.
    // gen=1 is in [range.startGen=1, activeGen=2] → tailDepsContained=true → wouldRotate=true.
    REQUIRE_EQ(store.manifestRanges().size(), 1U);
    CHECK_EQ(store.tailGeneration(), 2U);

    auto st = store.compactOneSegment();
    CHECK(st.ok());
    CHECK_FALSE(st.isNoOp());

    // rotate fired: seals gen=2 → new tail=gen=3.
    // compact {1,2}: seq=1("A") + seq=2("b") fit in gen=4; seq=3("c") in gen=5.
    REQUIRE_EQ(store.manifestRanges().size(), 1U);
    CHECK_EQ(store.manifestRanges()[0].startGen, 4U);
    CHECK_EQ(store.manifestRanges()[0].endGen,   5U);
    CHECK_EQ(store.tailGeneration(), 3U);
    CHECK(std::filesystem::exists(segmentPath(3)));
    CHECK(std::filesystem::exists(segmentPath(4)));
    CHECK(std::filesystem::exists(segmentPath(5)));

    // seq=1 must return the rewritten payload.
    std::string out;
    CHECK(store.readRecord(1, out).ok()); CHECK_EQ(out, "A");
    CHECK(store.readRecord(2, out).ok()); CHECK_EQ(out, "b");
    CHECK(store.readRecord(3, out).ok()); CHECK_EQ(out, "c");

    // Verify rewritten payload survives remount.
    pqueue::AppendLogStore store2(cfg);
    REQUIRE(store2.mount().ok());
    CHECK(store2.readRecord(1, out).ok()); CHECK_EQ(out, "A");
    CHECK(store2.readRecord(2, out).ok()); CHECK_EQ(out, "b");
    CHECK(store2.readRecord(3, out).ok()); CHECK_EQ(out, "c");
}

TEST_CASE("compaction: cross-range tail rewrite deps reconstructed after remount; no reversion") {
    // Verifies the activeTailDependenciesTracked_ fix for REWRITE tombstones:
    // after remount the tail scan must reconstruct activeTailAffectedGenerations_
    // so that cross-range REWRITE deps are detected and the rewritten payload does
    // not revert to the original after compaction removes the source segment.
    //
    // Layout: ranges=[{1,1},{3,3}], tail=4.
    // gen=1: seq=1 (original payload p1_orig) — will be rewritten in tail.
    // gen=3: seq=2 (live, untouched).
    // gen=4: REWRITE(1, p1_new) [cross-range dep on gen=1] + ENQUEUE(3, p3).
    using namespace pqueue::append_log_detail;

    const std::string p1_orig(25, 'a'), p1_new(25, 'z'), p2(25, 'b'), p3(25, 'c');

    plantLayout({
        .ranges   = {{1u, 1u}, {3u, 3u}},
        .tail     = 4u,
        .next     = 5u,
        .segments = {
            {1u, 1u, serializeEnqueueEvent(1, p1_orig)},
            {3u, 3u, serializeEnqueueEvent(2, p2)},
            {4u, 3u, serializeRewriteEvent(1, p1_new) + serializeEnqueueEvent(3, p3)},
        },
    });

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 150;

    // First mount: tail scan reconstructs affected={1}, tracking=true.
    // {1,1} is the dead-data candidate (seq=1 moved to tail via REWRITE).
    {
        pqueue::AppendLogStore store(cfg);
        REQUIRE(store.mount().ok());

        REQUIRE_EQ(store.manifestRanges().size(), 2U);
        CHECK_EQ(store.tailGeneration(), 4U);

        // seq=1 must already read as the rewritten value before any compaction.
        std::string out;
        CHECK(store.readRecord(1, out).ok()); CHECK_EQ(out, p1_new);

        auto st = store.compactOneSegment(); // removes dead range {1,1}
        CHECK(st.ok());
        CHECK_FALSE(st.isNoOp());

        REQUIRE_EQ(store.manifestRanges().size(), 1U);
        CHECK_EQ(store.manifestRanges()[0].startGen, 3U);
        CHECK_EQ(store.manifestRanges()[0].endGen,   3U);
        CHECK_EQ(store.tailGeneration(), 4U);

        CHECK(store.readRecord(1, out).ok()); CHECK_EQ(out, p1_new);
        CHECK(store.readRecord(2, out).ok()); CHECK_EQ(out, p2);
        CHECK(store.readRecord(3, out).ok()); CHECK_EQ(out, p3);
    }

    // After remount: seq=1 must still return the rewritten payload (no reversion).
    {
        pqueue::AppendLogStore store(cfg);
        REQUIRE(store.mount().ok());

        std::string out;
        CHECK(store.readRecord(1, out).ok()); CHECK_EQ(out, p1_new);
        CHECK(store.readRecord(2, out).ok()); CHECK_EQ(out, p2);
        CHECK(store.readRecord(3, out).ok()); CHECK_EQ(out, p3);
    }
}

TEST_CASE("compaction: orphaned REWRITE then POP in same tail does not resurrect after remount") {
    // Validates the orphaned-REWRITE scan path against false resurrection.
    //
    // After appendRewriteEvent validates that the sequence is live before writing,
    // the only orphaned REWRITEs that can reach the scan are those whose source
    // ENQUEUE was in a dead range later compacted away.  If the same tail also
    // holds a subsequent POP for the same sequence, the insert+pop pair must
    // cancel out correctly -- the record must remain absent after remount.
    //
    // Two-range layout so {1,1} is NOT the last range; subrangeReachesLastGen=false
    // ensures the dead-range fast path runs without triggering rotate-before-compact.
    //
    // Layout: ranges=[{1,1},{3,3}], tail=4, next=5.
    // gen=1: ENQUEUE(1, p1) -- dead range after REWRITE+POP in tail.
    // gen=3: ENQUEUE(2, p2) -- live range.
    // gen=4 (tail): REWRITE(1, p1_new) + POP(1) + ENQUEUE(3, p3).
    using namespace pqueue::append_log_detail;

    const std::string p1(10, 'a'), p1_new(10, 'z'), p2(10, 'b'), p3(10, 'c');

    plantLayout({
        .ranges   = {{1u, 1u}, {3u, 3u}},
        .tail     = 4u,
        .next     = 5u,
        .segments = {
            {1u, 1u, serializeEnqueueEvent(1, p1)},
            {3u, 3u, serializeEnqueueEvent(2, p2)},
            {4u, 3u, serializeRewriteEvent(1, p1_new) + serializePopEvent(1) + serializeEnqueueEvent(3, p3)},
        },
    });

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 150;

    {
        pqueue::AppendLogStore store(cfg);
        REQUIRE(store.mount().ok());

        REQUIRE_EQ(store.manifestRanges().size(), 2U);
        CHECK_EQ(store.tailGeneration(), 4U);

        // gen=1: hasLive=false (seq=1 rewritten to gen=4, then popped).
        // subrangeReachesLastGen=false (last range is {3,3}, not {1,1}).
        // Dead-range fast path: delete gen=1 without rotate.
        auto st = store.compactOneSegment();
        CHECK(st.ok());
        CHECK_FALSE(st.isNoOp());

        REQUIRE_EQ(store.manifestRanges().size(), 1U);
        CHECK_EQ(store.manifestRanges()[0].startGen, 3U);
        CHECK_EQ(store.manifestRanges()[0].endGen,   3U);
        CHECK_EQ(store.tailGeneration(), 4U);
        CHECK_FALSE(std::filesystem::exists(segmentPath(1)));

        std::string out;
        CHECK_FALSE(store.readRecord(1, out).ok()); // popped
        CHECK(store.readRecord(2, out).ok()); CHECK_EQ(out, p2);
        CHECK(store.readRecord(3, out).ok()); CHECK_EQ(out, p3);
    }

    // After remount: orphaned REWRITE(1)+POP(1) in tail must cancel out.
    // seq=1 must remain absent (no resurrection).
    {
        pqueue::AppendLogStore store(cfg);
        REQUIRE(store.mount().ok());

        std::string out;
        CHECK_FALSE(store.readRecord(1, out).ok());
        CHECK(store.readRecord(2, out).ok()); CHECK_EQ(out, p2);
        CHECK(store.readRecord(3, out).ok()); CHECK_EQ(out, p3);
    }
}

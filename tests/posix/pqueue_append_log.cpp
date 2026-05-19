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


TEST_CASE("append-log: rewriteFront persists") {
    cleanSpool();
    auto cfg = makeConfig();
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("original").ok());
        CHECK(q.rewriteFront("updated").ok());
    }
    {
        pqueue::Queue q(cfg);
        std::string out;
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "updated");
    }
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

TEST_CASE("compaction: rotate-before-compact is skipped after remount when tail dependencies are unknown") {
    // After remount activeTailDependenciesTracked_ is false. Even if the tail is
    // contiguous with the last range and the range has dead bytes, rotate must not fire.
    // The compaction should still make progress (compact the range without rotating).
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
        storePop(store);             // pop seq=1: dead bytes in gen=1, seq=2 still live
        storeEnqueue(store, 3, "c"); // goes to gen=2
    }
    // Remount: tracking becomes false.
    pqueue::AppendLogStore store(cfg);
    REQUIRE(store.mount().ok());

    // State: ranges=[{1,1}], tail=2 (contiguous: 1+1=2). Dead bytes in gen=1.
    REQUIRE_EQ(store.manifestRanges().size(), 1U);
    CHECK_EQ(store.manifestRanges()[0].startGen, 1U);
    CHECK_EQ(store.manifestRanges()[0].endGen,   1U);
    CHECK_EQ(store.tailGeneration(), 2U);

    // Without rotate: compact only {1,1} → output at gen=3, tail stays gen=2.
    // With rotate (if it had fired): rotate gen=2 → new tail=gen=3, compact {1,2} → output gen=4.
    auto st = store.compactOneSegment();
    CHECK(st.ok());

    // Tail must NOT have been rotated: still gen=2.
    CHECK_EQ(store.tailGeneration(), 2U);
    // Output range must be gen=3 (not gen=4, which would only appear after a rotate).
    REQUIRE_EQ(store.manifestRanges().size(), 1U);
    CHECK_EQ(store.manifestRanges()[0].startGen, 3U);
    CHECK_EQ(store.manifestRanges()[0].endGen,   3U);
    CHECK(std::filesystem::exists(segmentPath(2))); // old tail still present, not rotated

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
        // no rotate). affected stays {1,3} — no new tail created.
        auto st1 = store.compactOneSegment();
        CHECK(st1.ok());
        CHECK_FALSE(st1.isNoOp());
        REQUIRE_EQ(store.manifestRanges().size(), 1U);
        CHECK_EQ(store.manifestRanges()[0].startGen, 3U);
        CHECK_EQ(store.manifestRanges()[0].endGen,   4U);
        CHECK_EQ(store.tailGeneration(), 5U);

        // Second compactOneSegment: {3,4} is now the last range; tail=5 is contiguous
        // (4+1=5). tailDepsContained=false (gen=1 still in affected, not in [3,5]).
        // Rotate must NOT fire; tail must remain gen=5.
        auto st2 = store.compactOneSegment();
        CHECK(st2.ok());
        CHECK_FALSE(st2.isNoOp());

        // Prove no rotate: tail still gen=5. If rotate had fired, tail would be gen=6.
        CHECK_EQ(store.tailGeneration(), 5U);
        // Output: 1 seg for live records seq=5,6 (in gen=4).
        REQUIRE_EQ(store.manifestRanges().size(), 1U);
        CHECK_EQ(store.manifestRanges()[0].startGen, 6U);
        CHECK_EQ(store.manifestRanges()[0].endGen,   6U);

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

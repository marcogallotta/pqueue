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

TEST_CASE("append-log: segment rotation persists") {
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 256;
    {
        pqueue::Queue q(cfg);
        for (int i = 0; i < 10; ++i)
            CHECK(q.enqueue("msg-" + std::to_string(i)).ok());
    }
    {
        pqueue::Queue q(cfg);
        CHECK_EQ(q.stats().count, 10U);
        for (int i = 0; i < 10; ++i) {
            std::string out;
            CHECK(q.peek(out).ok()); CHECK_EQ(out, "msg-" + std::to_string(i));
            CHECK(q.pop().ok());
        }
    }
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

TEST_CASE("append-log: format then enqueue works") {
    cleanSpool();
    pqueue::Queue q(makeConfig());
    CHECK(q.enqueue("before").ok());
    CHECK(q.format().ok());
    CHECK(q.enqueue("after").ok());
    std::string out;
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "after");
}

TEST_CASE("append-log: validate returns ok for clean store") {
    cleanSpool();
    pqueue::Queue q(makeConfig());
    CHECK(q.enqueue("x").ok());
    CHECK(q.validate().ok);
}

TEST_CASE("append-log: validate on empty store") {
    cleanSpool();
    pqueue::Queue q(makeConfig());
    CHECK(q.validate().ok);
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

TEST_CASE("append-log: mixed enqueue and pop with persistence") {
    cleanSpool();
    auto cfg = makeConfig();
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("a").ok());
        CHECK(q.enqueue("b").ok());
        CHECK(q.enqueue("c").ok());
        CHECK(q.pop().ok());
    }
    {
        pqueue::Queue q(cfg);
        CHECK_EQ(q.stats().count, 2U);
        std::string out;
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "b");
        CHECK(q.pop().ok());
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "c");
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

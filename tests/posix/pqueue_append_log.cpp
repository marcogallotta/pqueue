#include "pqueue_append_log_support.h"

TEST_CASE("append-log: starts empty") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Queue q(makeConfig());
    std::string out;
    CHECK_FALSE(q.peek(out).ok());
    CHECK_EQ(q.stats().count, 0U);
#endif
}

TEST_CASE("append-log: basic enqueue and peek") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Queue q(makeConfig());
    CHECK(q.enqueue("hello").ok());
    std::string out;
    CHECK(q.peek(out).ok());
    CHECK_EQ(out, "hello");
    CHECK_EQ(q.stats().count, 1U);
#endif
}

TEST_CASE("append-log: FIFO order preserved") {
#ifndef ARDUINO
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
#endif
}

TEST_CASE("append-log: persistence across remount") {
#ifndef ARDUINO
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
        CHECK(q.peek(out).ok());
        CHECK_EQ(out, "alpha");
        CHECK(q.pop().ok());
        CHECK(q.peek(out).ok());
        CHECK_EQ(out, "beta");
        CHECK(q.pop().ok());
        CHECK_EQ(q.stats().count, 0U);
    }
#endif
}

TEST_CASE("append-log: pop persists across remount") {
#ifndef ARDUINO
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
        CHECK(q.peek(out).ok());
        CHECK_EQ(out, "discard");
        CHECK_EQ(q.stats().count, 1U);
    }
#endif
}

TEST_CASE("append-log: segment rotation") {
#ifndef ARDUINO
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 256; // very small to force rotation
    pqueue::Queue q(cfg);

    // Enqueue enough records to force multiple segment rotations
    for (int i = 0; i < 20; ++i) {
        CHECK(q.enqueue("record-" + std::to_string(i)).ok());
    }
    CHECK_EQ(q.stats().count, 20U);

    // Verify all records are readable
    for (int i = 0; i < 20; ++i) {
        std::string out;
        CHECK(q.peek(out).ok());
        CHECK_EQ(out, "record-" + std::to_string(i));
        CHECK(q.pop().ok());
    }
    CHECK_EQ(q.stats().count, 0U);
#endif
}

TEST_CASE("append-log: segment rotation persists") {
#ifndef ARDUINO
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 256;
    {
        pqueue::Queue q(cfg);
        for (int i = 0; i < 10; ++i) {
            CHECK(q.enqueue("msg-" + std::to_string(i)).ok());
        }
    }
    {
        pqueue::Queue q(cfg);
        CHECK_EQ(q.stats().count, 10U);
        for (int i = 0; i < 10; ++i) {
            std::string out;
            CHECK(q.peek(out).ok());
            CHECK_EQ(out, "msg-" + std::to_string(i));
            CHECK(q.pop().ok());
        }
    }
#endif
}

TEST_CASE("append-log: rewriteFront persists") {
#ifndef ARDUINO
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
        CHECK(q.peek(out).ok());
        CHECK_EQ(out, "updated");
    }
#endif
}

TEST_CASE("append-log: format clears all records") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Queue q(makeConfig());
    CHECK(q.enqueue("a").ok());
    CHECK(q.enqueue("b").ok());
    CHECK(q.format().ok());
    CHECK_EQ(q.stats().count, 0U);
    std::string out;
    CHECK_FALSE(q.peek(out).ok());
#endif
}

TEST_CASE("append-log: format then enqueue works") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Queue q(makeConfig());
    CHECK(q.enqueue("before").ok());
    CHECK(q.format().ok());
    CHECK(q.enqueue("after").ok());
    std::string out;
    CHECK(q.peek(out).ok());
    CHECK_EQ(out, "after");
#endif
}

TEST_CASE("append-log: validate returns ok for clean store") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Queue q(makeConfig());
    CHECK(q.enqueue("x").ok());
    const auto result = q.validate();
    CHECK(result.ok);
#endif
}

TEST_CASE("append-log: validate on empty store") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Queue q(makeConfig());
    const auto result = q.validate();
    CHECK(result.ok);
#endif
}

TEST_CASE("append-log: compaction triggered by segment count") {
#ifndef ARDUINO
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 128;
    cfg.maxSegments = 3;
    pqueue::Queue q(cfg);

    // Enqueue enough to trigger many rotations and compaction
    std::vector<std::string> expected;
    for (int i = 0; i < 30; ++i) {
        const std::string rec = "record-" + std::to_string(i);
        CHECK(q.enqueue(rec).ok());
        expected.push_back(rec);
    }
    CHECK_EQ(q.stats().count, 30U);

    // All records should still be readable
    for (int i = 0; i < 30; ++i) {
        std::string out;
        CHECK(q.peek(out).ok());
        CHECK_EQ(out, expected[i]);
        CHECK(q.pop().ok());
    }
#endif
}

TEST_CASE("compaction trigger: remount after auto-compaction preserves FIFO order (critical)") {
#ifndef ARDUINO
    // Trigger compaction automatically via writeRecord, then remount and verify
    // every record is still peekable and poppable in the original FIFO order.
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
            CHECK(q.peek(out).ok());
            CHECK_EQ(out, expected[i]);
            CHECK(q.pop().ok());
        }
        std::string out;
        CHECK_FALSE(q.peek(out).ok());
    }
#endif
}

TEST_CASE("compaction trigger: auto-triggers on size-based count after non-monotonic compaction") {
#ifndef ARDUINO
    // Verify that compaction is triggered by activeGenerations_.size(), not by
    // the numeric generation span. After a compaction, the new segment gets a
    // generation number higher than the tail's, creating a non-monotonic ordering.
    // The span-based heuristic becomes incorrect; the size-based one must not.
    //
    // Setup: maxSegments=2, maxSegmentBytes small. Enqueue enough to produce 3
    // live segments, then compact to get 2. Enqueue more to fill the tail and
    // force a rotation — this must succeed (compaction triggers if needed, not
    // RangeLimitExceeded). Verify all records remain correct after a remount.
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
            CHECK(q.peek(out).ok());
            CHECK_EQ(out, expected[i]);
            CHECK(q.pop().ok());
        }
    }
#endif
}

TEST_CASE("append-log: mixed enqueue and pop with persistence") {
#ifndef ARDUINO
    cleanSpool();
    auto cfg = makeConfig();
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("a").ok());
        CHECK(q.enqueue("b").ok());
        CHECK(q.enqueue("c").ok());
        CHECK(q.pop().ok()); // remove "a"
    }
    {
        pqueue::Queue q(cfg);
        CHECK_EQ(q.stats().count, 2U);
        std::string out;
        CHECK(q.peek(out).ok());
        CHECK_EQ(out, "b");
        CHECK(q.pop().ok());
        CHECK(q.peek(out).ok());
        CHECK_EQ(out, "c");
    }
#endif
}

// --- Recovery policy tests ---

TEST_CASE("append-log: corrupt payloadBytes at tail of last segment is recoverable") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("keep").ok());
        CHECK(q.enqueue("torn").ok());
    }
    // Corrupt payloadBytes of the second (last) event to 0xFFFFFFFF.
    // The guard fires before any allocation; the event is discarded as a torn tail.
    // payloadBytes field offset within an event: magic(4)+version(2)+headerBytes(2)+sequence(4) = 12
    constexpr std::uintmax_t kEventSize = kEnqueueHeaderBytes + 4 + kEventTrailerBytes; // "keep" = 4 bytes
    constexpr std::uintmax_t kPayloadBytesOffset = kSegmentHeaderBytes + kEventSize + kEnqueueHeaderBytes - 4;
    patchFile(segmentPath(1), kPayloadBytesOffset, {0xFF, 0xFF, 0xFF, 0xFF});
    {
        pqueue::Queue q(cfg);
        CHECK_EQ(q.stats().count, 1U);
        std::string out;
        CHECK(q.peek(out).ok());
        CHECK_EQ(out, "keep");
    }
#endif
}

TEST_CASE("append-log: corrupt payloadBytes in non-last segment causes mount failure") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 128;
    {
        pqueue::Queue q(cfg);
        for (int i = 0; i < 6; ++i) {
            CHECK(q.enqueue("X").ok());
        }
    }
    REQUIRE(std::filesystem::exists(segmentPath(2)));
    // Corrupt payloadBytes of first event in segment 1 (non-last segment)
    constexpr std::uintmax_t kPayloadBytesOffset = kSegmentHeaderBytes + kEnqueueHeaderBytes - 4;
    patchFile(segmentPath(1), kPayloadBytesOffset, {0xFF, 0xFF, 0xFF, 0xFF});
    {
        pqueue::Queue q(cfg);
        std::string out;
        const auto st = q.peek(out);
        CHECK_FALSE(st.ok());
        CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
    }
#endif
}

TEST_CASE("append-log: torn tail on active segment is recoverable") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("A").ok());
        CHECK(q.enqueue("B").ok());
        CHECK(q.enqueue("C").ok());
    }
    // Each 1-byte event: kEnqueueHeaderBytes(16) + 1 + kEventTrailerBytes(8) = 25 bytes
    // Event A: offsets 20-44, B: 45-69, C: 70-94
    // Truncate 5 bytes into event C's header → torn tail
    constexpr std::uintmax_t kEventSize = kEnqueueHeaderBytes + 1 + kEventTrailerBytes;
    const std::uintmax_t tornOffset = kSegmentHeaderBytes + 2 * kEventSize + 5;
    std::filesystem::resize_file(segmentPath(1), tornOffset);
    {
        pqueue::Queue q(cfg);
        CHECK_EQ(q.stats().count, 2U);
        std::string out;
        CHECK(q.peek(out).ok());
        CHECK_EQ(out, "A");
        CHECK(q.pop().ok());
        CHECK(q.peek(out).ok());
        CHECK_EQ(out, "B");
    }
#endif
}

TEST_CASE("append-log: corrupt CRC at tail of last segment is recoverable") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("keep").ok());
        CHECK(q.enqueue("torn").ok());
    }
    // Corrupt CRC of the second (last) event: seg_header + event_"keep"(25) + header(16) + payload(4) = 65
    constexpr std::uintmax_t kEventSize = kEnqueueHeaderBytes + 4 + kEventTrailerBytes; // "keep" = 4 bytes
    constexpr std::uintmax_t kCrcOffset = kSegmentHeaderBytes + kEventSize + kEnqueueHeaderBytes + 4;
    patchFile(segmentPath(1), kCrcOffset, {0xDE, 0xAD, 0xBE, 0xEF});
    {
        pqueue::Queue q(cfg);
        // Mount succeeds; the corrupt final event is discarded as a torn tail
        CHECK_EQ(q.stats().count, 1U);
        std::string out;
        CHECK(q.peek(out).ok());
        CHECK_EQ(out, "keep");
    }
#endif
}

TEST_CASE("append-log: corrupt CRC in non-last segment causes mount failure") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 128;
    {
        pqueue::Queue q(cfg);
        for (int i = 0; i < 6; ++i) {
            CHECK(q.enqueue("X").ok());
        }
    }
    REQUIRE(std::filesystem::exists(segmentPath(2)));
    // Corrupt CRC of the first event in segment 1 (non-last segment)
    constexpr std::uintmax_t kCrcOffset = kSegmentHeaderBytes + kEnqueueHeaderBytes + 1;
    patchFile(segmentPath(1), kCrcOffset, {0xDE, 0xAD, 0xBE, 0xEF});
    {
        pqueue::Queue q(cfg);
        std::string out;
        const auto st = q.peek(out);
        CHECK_FALSE(st.ok());
        CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
    }
#endif
}

TEST_CASE("append-log: corrupt magic in non-last segment causes mount failure") {
#ifndef ARDUINO
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 128; // fits ~4 one-byte events; 5th triggers rotation
    {
        pqueue::Queue q(cfg);
        for (int i = 0; i < 6; ++i) {
            CHECK(q.enqueue("X").ok()); // forces segment 1 full, segment 2 created
        }
    }
    REQUIRE(std::filesystem::exists(segmentPath(2))); // confirm rotation happened
    // Corrupt the magic of the first event in segment 1
    patchFile(segmentPath(1), pqueue::append_log_detail::kSegmentHeaderBytes,
              {0xDE, 0xAD, 0xBE, 0xEF});
    {
        pqueue::Queue q(cfg);
        std::string out;
        const auto st = q.peek(out);
        CHECK_FALSE(st.ok());
        CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
    }
#endif
}

TEST_CASE("append-log: corrupt mid-last-segment discards tail from lastGoodOffset") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("A").ok());
        CHECK(q.enqueue("B").ok()); // will be corrupted
        CHECK(q.enqueue("C").ok()); // valid but follows the bad event
    }
    // Corrupt CRC of event B (middle event). All three fit in segment 1.
    // Event A: offsets 20-44 (1-byte payload, 25 bytes total).
    // Event B starts at 45; CRC is at 45 + kEnqueueHeaderBytes(16) + 1 = 62.
    constexpr std::uintmax_t kEventSize = kEnqueueHeaderBytes + 1 + kEventTrailerBytes;
    constexpr std::uintmax_t kBCrcOffset = kSegmentHeaderBytes + kEventSize + kEnqueueHeaderBytes + 1;
    patchFile(segmentPath(1), kBCrcOffset, {0xDE, 0xAD, 0xBE, 0xEF});
    {
        pqueue::Queue q(cfg);
        // B and C are both discarded; only A (before lastGoodOffset) survives.
        CHECK_EQ(q.stats().count, 1U);
        std::string out;
        CHECK(q.peek(out).ok());
        CHECK_EQ(out, "A");
    }
#endif
}


// --- Segment naming and generation tests ---

#ifndef ARDUINO

TEST_CASE("append-log: segment files use seg- prefix") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = kSegmentHeaderBytes + kEnqueueHeaderBytes + 1 + kEventTrailerBytes;
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("A").ok()); // fills seg 1
        CHECK(q.enqueue("B").ok()); // rotates to seg 2
    }
    CHECK(std::filesystem::exists(kSpoolDir / "seg-00000001.bin"));
    CHECK(std::filesystem::exists(kSpoolDir / "seg-00000002.bin"));
    CHECK_FALSE(std::filesystem::exists(kSpoolDir / "pqueue-seg-00000001.bin"));
}

TEST_CASE("append-log: torn tail in non-last segment causes DataCorrupt") {
    // Truncate a non-last segment midway through an event.
    // The scan loop treats this as DataCorrupt (not a recoverable torn tail),
    // because only the last segment may have an incomplete trailing event.
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
    // Truncate segment 1 (non-last) five bytes into its first event
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
    // After remount, the next segment rotation must use a generation number higher
    // than every generation already on disk — including any that were not the active
    // (last) segment. Regression guard: if nextGeneration_ were set from the active
    // gen alone, a higher-numbered inactive segment could cause gen reuse.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    // Size limit fits exactly one 1-byte event; each subsequent enqueue rotates.
    cfg.maxSegmentBytes = kSegmentHeaderBytes + kEnqueueHeaderBytes + 1 + kEventTrailerBytes;
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("A").ok()); // seg 1
        CHECK(q.enqueue("B").ok()); // seg 2
        CHECK(q.enqueue("C").ok()); // seg 3
    }
    REQUIRE(std::filesystem::exists(segmentPath(3)));
    {
        pqueue::Queue q(cfg);
        // Active seg is 3; next enqueue must rotate to seg 4, not any lower number.
        CHECK(q.enqueue("D").ok());
        CHECK(std::filesystem::exists(segmentPath(4)));
        CHECK_FALSE(std::filesystem::exists(segmentPath(0)));
    }
}

TEST_CASE("append-log: nextGeneration skips stray high disk generation") {
    // A segment file with a generation number higher than the logical tail may
    // exist on disk (e.g. left by a partially applied compaction). nextGeneration_
    // must be set above it so the stray file's generation is never reused.
    // Regression guard: if nextGeneration_ were derived only from activeGenerations_,
    // the stray high-gen file would be silently overwritten on the next rotation.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = kSegmentHeaderBytes + kEnqueueHeaderBytes + 1 + kEventTrailerBytes;
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("A").ok()); // seg 1
        CHECK(q.enqueue("B").ok()); // seg 2
        CHECK(q.enqueue("C").ok()); // seg 3
    }
    // Plant a stray segment at gen 10 (higher than the active gen 3).
    // Write a valid header so it passes isSegmentName() and gets picked up by listFiles().
    {
        const std::string header = serializeSegmentHeader(10, 0);
        std::ofstream f(kSpoolDir / "seg-0000000a.bin", std::ios::binary | std::ios::trunc);
        f.write(header.data(), static_cast<std::streamsize>(header.size()));
    }
    {
        pqueue::Queue q(cfg);
        // The stray seg-0000000a.bin becomes the active tail (it has a valid header
        // but no events, so activeSegmentBytes_ = kSegmentHeaderBytes).
        // "D" fills it; "E" overflows it and forces rotation. nextGeneration_ must
        // be 11 (past the stray gen 10), so the new segment is seg-0000000b.bin.
        CHECK(q.enqueue("D").ok()); // fills stray seg 10
        CHECK(q.enqueue("E").ok()); // overflows → rotation to gen 11
        CHECK(std::filesystem::exists(kSpoolDir / "seg-0000000b.bin")); // gen 11
        CHECK_FALSE(std::filesystem::exists(kSpoolDir / "seg-00000004.bin"));
    }
}

TEST_CASE("append-log: stale pqueue-compact.bin is ignored") {
    // After the journal was removed, a leftover pqueue-compact.bin from an old
    // store must not cause a mount failure or corrupt the queue.
    cleanSpool();
    auto cfg = makeConfig();
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("A").ok());
        CHECK(q.enqueue("B").ok());
    }
    // Drop a bogus journal file into the spool directory.
    {
        std::ofstream f(kSpoolDir / "pqueue-compact.bin", std::ios::binary | std::ios::trunc);
        f.write("\xDE\xAD\xBE\xEF", 4);
    }
    {
        pqueue::Queue q(cfg);
        CHECK_EQ(q.stats().count, 2U);
        std::string out;
        CHECK(q.peek(out).ok());
        CHECK_EQ(out, "A");
    }
}

#endif // !ARDUINO

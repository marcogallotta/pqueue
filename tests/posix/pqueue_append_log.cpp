#include "pqueue/queue.h"
#include "pqueue/append_log_store.h"
#include "pqueue/append_log_common.h"
#include "pqueue/status.h"

#include "doctest/doctest.h"

#ifndef ARDUINO
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#endif

namespace {

#ifndef ARDUINO
const std::filesystem::path kSpoolDir = "build/pqueue-spools/pqueue_append_log_spool";

std::filesystem::path segmentPath(std::uint32_t gen) {
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", gen);
    return kSpoolDir / ("pqueue-seg-" + std::string(buf, 8) + ".bin");
}

void patchFile(const std::filesystem::path& path, std::uintmax_t offset,
               std::initializer_list<std::uint8_t> bytes) {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(static_cast<std::streamoff>(offset));
    for (std::uint8_t b : bytes) {
        const char c = static_cast<char>(b);
        f.write(&c, 1);
    }
}

void cleanSpool() {
    std::error_code ec;
    std::filesystem::remove_all(kSpoolDir, ec);
}

pqueue::Config makeConfig() {
    pqueue::Config cfg;
    cfg.basePath = kSpoolDir.string();
    cfg.storeLayout = pqueue::StoreLayout::AppendLog;
    cfg.recordSizeBytes = 256;
    cfg.reservedBytes = 64 * 1024;
    cfg.maxSegmentBytes = 1024; // small to force rotation in tests
    return cfg;
}
#endif

} // namespace

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

TEST_CASE("append-log: missing segment causes mount failure") {
#ifndef ARDUINO
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
    std::filesystem::remove(segmentPath(1)); // delete first segment, leaving a gap
    {
        pqueue::Queue q(cfg);
        std::string out;
        const auto st = q.peek(out);
        CHECK_FALSE(st.ok());
        CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
    }
#endif
}

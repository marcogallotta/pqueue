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

std::filesystem::path manifestSlotPath(char slot) {
    return kSpoolDir / (std::string("manifest-") + slot + ".bin");
}

bool readManifestSlot(char slot, pqueue::append_log_detail::ManifestData& out) {
    using namespace pqueue::append_log_detail;
    std::ifstream f(manifestSlotPath(slot), std::ios::binary);
    if (!f) return false;
    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(f)),
        std::istreambuf_iterator<char>()
    );
    return parseManifest(bytes.data(), bytes.size(), out);
}

void writeManifestSlotDirect(char slot, uint32_t epoch) {
    using namespace pqueue::append_log_detail;
    ManifestData md;
    md.epoch = epoch;
    md.nextGeneration = 1;
    md.tailGeneration = 0;
    std::vector<std::uint8_t> bytes;
    serialiseManifest(md, bytes);
    std::ofstream f(manifestSlotPath(slot), std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

pqueue::AppendLogConfig makeStoreConfig() {
    pqueue::AppendLogConfig cfg;
    cfg.basePath = kSpoolDir.string();
    cfg.maxSegmentBytes = 1024;
    return cfg;
}

std::filesystem::path segmentPath(std::uint32_t gen) {
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", gen);
    return kSpoolDir / ("seg-" + std::string(buf, 8) + ".bin");
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

// --- Manifest binary format tests ---

TEST_CASE("manifest: round-trip empty store (tailGen=0, no ranges)") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch          = 1;
    m.nextGeneration = 1;
    m.tailGeneration = 0;

    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);
    CHECK_EQ(buf.size(), kManifestFixedBytes);

    ManifestData out;
    CHECK(parseManifest(buf.data(), buf.size(), out));
    CHECK_EQ(out.epoch,          m.epoch);
    CHECK_EQ(out.nextGeneration, m.nextGeneration);
    CHECK_EQ(out.tailGeneration, m.tailGeneration);
    CHECK(out.ranges.empty());
#endif
}

TEST_CASE("manifest: round-trip with two ranges") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch          = 7;
    m.nextGeneration = 10;
    m.tailGeneration = 9;
    m.ranges         = {{1, 3}, {5, 7}};

    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);
    CHECK_EQ(buf.size(), std::size_t(kManifestFixedBytes + 2 * 8));

    ManifestData out;
    CHECK(parseManifest(buf.data(), buf.size(), out));
    CHECK_EQ(out.epoch,          m.epoch);
    CHECK_EQ(out.nextGeneration, m.nextGeneration);
    CHECK_EQ(out.tailGeneration, m.tailGeneration);
    REQUIRE_EQ(out.ranges.size(), 2u);
    CHECK_EQ(out.ranges[0].startGen, 1u);
    CHECK_EQ(out.ranges[0].endGen,   3u);
    CHECK_EQ(out.ranges[1].startGen, 5u);
    CHECK_EQ(out.ranges[1].endGen,   7u);
#endif
}

TEST_CASE("manifest: binary layout field offsets") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch          = 0x11223344;
    m.nextGeneration = 0xAABBCCDD;
    m.tailGeneration = 0x55667788;
    // No ranges: headerBytes = 30

    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);
    REQUIRE_EQ(buf.size(), std::size_t(30));

    // magic at 0 (LE: 0x50, 0x51, 0x4D, 0x46 = "PQMF")
    CHECK_EQ(buf[0], 0x50u);
    CHECK_EQ(buf[1], 0x51u);
    CHECK_EQ(buf[2], 0x4Du);
    CHECK_EQ(buf[3], 0x46u);
    // version at 4 = 1 (LE)
    CHECK_EQ(buf[4], 0x01u);
    CHECK_EQ(buf[5], 0x00u);
    // headerBytes at 6 = 30 (LE)
    CHECK_EQ(buf[6], 0x1Eu);
    CHECK_EQ(buf[7], 0x00u);
    // epoch at 8
    CHECK_EQ(buf[8],  0x44u);
    CHECK_EQ(buf[9],  0x33u);
    CHECK_EQ(buf[10], 0x22u);
    CHECK_EQ(buf[11], 0x11u);
    // nextGeneration at 12
    CHECK_EQ(buf[12], 0xDDu);
    CHECK_EQ(buf[13], 0xCCu);
    CHECK_EQ(buf[14], 0xBBu);
    CHECK_EQ(buf[15], 0xAAu);
    // rangeCount at 16 = 0
    CHECK_EQ(buf[16], 0x00u);
    CHECK_EQ(buf[17], 0x00u);
    // tailGeneration at 18
    CHECK_EQ(buf[18], 0x88u);
    CHECK_EQ(buf[19], 0x77u);
    CHECK_EQ(buf[20], 0x66u);
    CHECK_EQ(buf[21], 0x55u);
    // footer at 26 (LE: 0x50, 0x4F, 0x4B, 0x21 = "POK!")
    CHECK_EQ(buf[26], 0x50u);
    CHECK_EQ(buf[27], 0x4Fu);
    CHECK_EQ(buf[28], 0x4Bu);
    CHECK_EQ(buf[29], 0x21u);
#endif
}

TEST_CASE("manifest: corrupted CRC is rejected") {
    // Critical: a valid manifest with one CRC byte corrupted must not parse.
    // This is the entire basis of crash-safety on mount.
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch          = 3;
    m.nextGeneration = 5;
    m.tailGeneration = 4;
    m.ranges         = {{1, 3}};

    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);

    // Corrupt one byte of the CRC field (4th from end: footer(4) then crc(4)).
    // With 1 range: total = 38 bytes. CRC is at buf[30..33], footer at buf[34..37].
    CHECK(buf.size() == std::size_t(kManifestFixedBytes + 8));
    buf[buf.size() - 8] ^= 0xFF; // flip a byte in the CRC field

    ManifestData out;
    CHECK_FALSE(parseManifest(buf.data(), buf.size(), out));
#endif
}

TEST_CASE("manifest: wrong magic is rejected") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch = 1; m.nextGeneration = 1; m.tailGeneration = 0;
    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);
    buf[0] ^= 0xFF; // corrupt magic byte 0
    ManifestData out;
    CHECK_FALSE(parseManifest(buf.data(), buf.size(), out));
#endif
}

TEST_CASE("manifest: wrong version is rejected") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch = 1; m.nextGeneration = 1; m.tailGeneration = 0;
    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);
    buf[4] = 0x02; // version = 2 instead of 1
    ManifestData out;
    CHECK_FALSE(parseManifest(buf.data(), buf.size(), out));
#endif
}

TEST_CASE("manifest: wrong headerBytes is rejected") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch = 1; m.nextGeneration = 1; m.tailGeneration = 0;
    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);
    buf[6] = 0xFF; // corrupt headerBytes low byte
    ManifestData out;
    CHECK_FALSE(parseManifest(buf.data(), buf.size(), out));
#endif
}

TEST_CASE("manifest: wrong footer is rejected") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch = 1; m.nextGeneration = 1; m.tailGeneration = 0;
    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);
    buf[buf.size() - 1] ^= 0xFF; // corrupt last footer byte
    ManifestData out;
    CHECK_FALSE(parseManifest(buf.data(), buf.size(), out));
#endif
}

TEST_CASE("manifest: rangeCount > 4 is rejected") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch = 1; m.nextGeneration = 1; m.tailGeneration = 0;
    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);
    // Force rangeCount to 5 (at offset 16, LE)
    buf[16] = 0x05;
    ManifestData out;
    CHECK_FALSE(parseManifest(buf.data(), buf.size(), out));
#endif
}

TEST_CASE("manifest: tailGen==0 with non-zero rangeCount is rejected") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    // Build a manifest with rangeCount=1 but tailGeneration=0 — this is malformed.
    // We can't construct this via serialiseManifest (which is well-behaved), so
    // we serialise a valid one and then patch tailGeneration to 0 in the raw bytes.
    ManifestData m;
    m.epoch          = 2;
    m.nextGeneration = 5;
    m.tailGeneration = 4;
    m.ranges         = {{1, 3}};
    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);

    // tailGeneration is at offset 18 + 1*8 = 26 (after 1 range)
    // layout: magic(4)+ver(2)+hdr(2)+epoch(4)+nextGen(4)+rangeCount(2)+range(8)+tailGen(4)+crc(4)+footer(4)
    // tailGen starts at offset: 4+2+2+4+4+2+8 = 26
    buf[26] = 0x00;
    buf[27] = 0x00;
    buf[28] = 0x00;
    buf[29] = 0x00; // tailGeneration = 0

    // Recompute CRC since we changed the data (otherwise we'd fail CRC first, not the tailGen check)
    // CRC covers bytes 0 through 29 (offset of crc field = 30)
    const uint32_t newCrc = crc32(0, buf.data(), 30);
    buf[30] = static_cast<uint8_t>((newCrc      ) & 0xFF);
    buf[31] = static_cast<uint8_t>((newCrc >>  8) & 0xFF);
    buf[32] = static_cast<uint8_t>((newCrc >> 16) & 0xFF);
    buf[33] = static_cast<uint8_t>((newCrc >> 24) & 0xFF);

    ManifestData out;
    CHECK_FALSE(parseManifest(buf.data(), buf.size(), out));
#endif
}

TEST_CASE("manifest: buffer too small returns false") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch = 1; m.nextGeneration = 1; m.tailGeneration = 0;
    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);
    ManifestData out;
    CHECK_FALSE(parseManifest(buf.data(), buf.size() - 1, out));
    CHECK_FALSE(parseManifest(buf.data(), 0, out));
#endif
}

TEST_CASE("manifest: trailing bytes rejected") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch = 1; m.nextGeneration = 1; m.tailGeneration = 0;
    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);
    buf.push_back(0x00); // one extra byte
    ManifestData out;
    CHECK_FALSE(parseManifest(buf.data(), buf.size(), out));
#endif
}

namespace {
// Patch bytes in buf then recompute and overwrite the CRC field.
// CRC covers bytes [0, crcOffset), which equals headerBytes - 8 (crc(4)+footer(4)).
void recomputeManifestCrc(std::vector<uint8_t>& buf) {
    using namespace pqueue::append_log_detail;
    // headerBytes is at offset 6 (LE u16)
    const std::size_t headerBytes = std::size_t(buf[6]) | std::size_t(buf[7]) << 8;
    const std::size_t crcOffset   = headerBytes - 8; // crc(4) + footer(4)
    const uint32_t newCrc = crc32(0, buf.data(), crcOffset);
    buf[crcOffset + 0] = static_cast<uint8_t>((newCrc      ) & 0xFF);
    buf[crcOffset + 1] = static_cast<uint8_t>((newCrc >>  8) & 0xFF);
    buf[crcOffset + 2] = static_cast<uint8_t>((newCrc >> 16) & 0xFF);
    buf[crcOffset + 3] = static_cast<uint8_t>((newCrc >> 24) & 0xFF);
}
} // namespace

TEST_CASE("manifest: startGen == 0 in range is rejected") {
    // CRC is recomputed so the failure proves range validation, not CRC.
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch = 2; m.nextGeneration = 5; m.tailGeneration = 4;
    m.ranges = {{1, 3}};
    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);

    // startGen of first range is at offset 18 (4+2+2+4+4+2 = 18)
    buf[18] = 0x00; buf[19] = 0x00; buf[20] = 0x00; buf[21] = 0x00;
    recomputeManifestCrc(buf);

    ManifestData out;
    CHECK_FALSE(parseManifest(buf.data(), buf.size(), out));
#endif
}

TEST_CASE("manifest: endGen < startGen in range is rejected") {
    // CRC is recomputed so the failure proves range validation, not CRC.
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch = 2; m.nextGeneration = 5; m.tailGeneration = 4;
    m.ranges = {{3, 5}};
    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);

    // endGen of first range is at offset 22 (startGen at 18, endGen at 22)
    buf[22] = 0x01; buf[23] = 0x00; buf[24] = 0x00; buf[25] = 0x00; // endGen = 1 < startGen = 3
    recomputeManifestCrc(buf);

    ManifestData out;
    CHECK_FALSE(parseManifest(buf.data(), buf.size(), out));
#endif
}

// --- Manifest publish tests ---

TEST_CASE("manifest-publish: first publish writes slot A with epoch 1") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    ManifestData md;
    md.nextGeneration = 1;
    md.tailGeneration = 0;
    CHECK(store.publishManifest(md).ok());

    ManifestData slotA, slotB;
    CHECK(readManifestSlot('a', slotA));
    CHECK_EQ(slotA.epoch, 1U);
    CHECK_FALSE(readManifestSlot('b', slotB)); // slot B not written yet
#endif
}

TEST_CASE("manifest-publish: second publish writes slot B; A valid B missing") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    ManifestData md;
    md.nextGeneration = 1;
    md.tailGeneration = 0;
    REQUIRE(store.publishManifest(md).ok()); // writes A, epoch=1
    REQUIRE(store.publishManifest(md).ok()); // B missing → writes B, epoch=2

    ManifestData slotA, slotB;
    CHECK(readManifestSlot('a', slotA));
    CHECK_EQ(slotA.epoch, 1U); // A untouched
    CHECK(readManifestSlot('b', slotB));
    CHECK_EQ(slotB.epoch, 2U);
#endif
}

TEST_CASE("manifest-publish: A missing B valid writes slot A") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    // Plant slot B (epoch 5) directly; slot A absent.
    writeManifestSlotDirect('b', 5);

    ManifestData md;
    md.nextGeneration = 1;
    md.tailGeneration = 0;
    CHECK(store.publishManifest(md).ok()); // A missing → writes A, epoch=6

    ManifestData slotA, slotB;
    CHECK(readManifestSlot('a', slotA));
    CHECK_EQ(slotA.epoch, 6U);
    CHECK(readManifestSlot('b', slotB));
    CHECK_EQ(slotB.epoch, 5U); // B untouched
#endif
}

TEST_CASE("manifest-publish: both valid writes lower-epoch slot") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    // A has epoch 7, B has epoch 3 — B is lower → overwrite B.
    writeManifestSlotDirect('a', 7);
    writeManifestSlotDirect('b', 3);

    ManifestData md;
    md.nextGeneration = 1;
    md.tailGeneration = 0;
    CHECK(store.publishManifest(md).ok());

    ManifestData slotA, slotB;
    CHECK(readManifestSlot('a', slotA));
    CHECK_EQ(slotA.epoch, 7U); // A untouched
    CHECK(readManifestSlot('b', slotB));
    CHECK_EQ(slotB.epoch, 8U); // B overwritten with epoch = max(7,3)+1
#endif
}

TEST_CASE("manifest-publish: equal epochs writes slot B (tiebreaker)") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    writeManifestSlotDirect('a', 4);
    writeManifestSlotDirect('b', 4);

    ManifestData md;
    md.nextGeneration = 1;
    md.tailGeneration = 0;
    CHECK(store.publishManifest(md).ok());

    ManifestData slotA, slotB;
    CHECK(readManifestSlot('a', slotA));
    CHECK_EQ(slotA.epoch, 4U); // A untouched
    CHECK(readManifestSlot('b', slotB));
    CHECK_EQ(slotB.epoch, 5U); // B overwritten
#endif
}

TEST_CASE("manifest-publish: corrupt A, absent B returns DataCorrupt") {
    // Slot A exists but is corrupt; slot B absent. The existing file is evidence
    // of a committed layout — this is not a fresh store. Must fail loudly.
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    {
        std::ofstream fa(manifestSlotPath('a'), std::ios::binary | std::ios::trunc);
        fa.write("GARBAGE", 7);
    }

    ManifestData md;
    md.nextGeneration = 1;
    md.tailGeneration = 0;
    const auto st = store.publishManifest(md);
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
#endif
}

TEST_CASE("manifest-publish: absent A, corrupt B returns DataCorrupt") {
    // Slot B exists but is corrupt; slot A absent. Same argument as above.
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    {
        std::ofstream fb(manifestSlotPath('b'), std::ios::binary | std::ios::trunc);
        fb.write("GARBAGE", 7);
    }

    ManifestData md;
    md.nextGeneration = 1;
    md.tailGeneration = 0;
    const auto st = store.publishManifest(md);
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
#endif
}

TEST_CASE("manifest-publish: both slots corrupt returns DataCorrupt") {
    // Both slot files exist but fail CRC. Fail loudly — do not overwrite.
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    {
        std::ofstream fa(manifestSlotPath('a'), std::ios::binary | std::ios::trunc);
        fa.write("GARBAGE_A", 9);
        std::ofstream fb(manifestSlotPath('b'), std::ios::binary | std::ios::trunc);
        fb.write("GARBAGE_B", 9);
    }

    ManifestData md;
    md.nextGeneration = 1;
    md.tailGeneration = 0;
    const auto st = store.publishManifest(md);
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
#endif
}

TEST_CASE("manifest-publish: corrupt slot A is treated as missing; slot B wins election") {
    // Slot A corrupt (fails CRC), slot B valid with epoch 7.
    // Publish must treat A as invalid/missing and overwrite it with epoch 8.
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    writeManifestSlotDirect('b', 7);
    {
        std::ofstream f(manifestSlotPath('a'), std::ios::binary | std::ios::trunc);
        f.write("GARBAGE", 7);
    }

    ManifestData md;
    md.nextGeneration = 1;
    md.tailGeneration = 0;
    CHECK(store.publishManifest(md).ok()); // A invalid → writes A, epoch = 7+1 = 8

    ManifestData slotA, slotB;
    CHECK(readManifestSlot('a', slotA));
    CHECK_EQ(slotA.epoch, 8U);
    CHECK(readManifestSlot('b', slotB));
    CHECK_EQ(slotB.epoch, 7U); // B untouched
#endif
}

TEST_CASE("manifest-publish: corrupt slot B leaves slot A as the valid winner") {
    // Critical: first publish writes A (inactive = A when both missing).
    // Second publish writes B. After corrupting B, A (lower epoch) must be
    // the only valid slot — confirming inactive-slot-first write order.
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    ManifestData md;
    md.nextGeneration = 1;
    md.tailGeneration = 0;
    REQUIRE(store.publishManifest(md).ok()); // writes A, epoch=1
    REQUIRE(store.publishManifest(md).ok()); // writes B, epoch=2

    // Corrupt slot B
    {
        std::ofstream f(manifestSlotPath('b'), std::ios::binary | std::ios::trunc);
        f.write("GARBAGE", 7);
    }

    ManifestData slotA, slotB;
    CHECK(readManifestSlot('a', slotA));
    CHECK_EQ(slotA.epoch, 1U);       // A intact with epoch 1
    CHECK_FALSE(readManifestSlot('b', slotB)); // B corrupt — rejected by parseManifest
#endif
}

// --- Manifest read tests (Stage 3a) ---

TEST_CASE("manifest-read: both slots missing returns false") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    ManifestData out;
    CHECK_FALSE(store.readManifest(out));
#endif
}

TEST_CASE("manifest-read: slot A valid, slot B missing returns A") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    writeManifestSlotDirect('a', 3);

    ManifestData out;
    CHECK(store.readManifest(out));
    CHECK_EQ(out.epoch, 3U);
#endif
}

TEST_CASE("manifest-read: slot A missing, slot B valid returns B") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    writeManifestSlotDirect('b', 5);

    ManifestData out;
    CHECK(store.readManifest(out));
    CHECK_EQ(out.epoch, 5U);
#endif
}

TEST_CASE("manifest-read: both valid, A higher epoch wins") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    writeManifestSlotDirect('a', 9);
    writeManifestSlotDirect('b', 4);

    ManifestData out;
    CHECK(store.readManifest(out));
    CHECK_EQ(out.epoch, 9U);
#endif
}

TEST_CASE("manifest-read: both valid, B higher epoch wins") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    writeManifestSlotDirect('a', 2);
    writeManifestSlotDirect('b', 8);

    ManifestData out;
    CHECK(store.readManifest(out));
    CHECK_EQ(out.epoch, 8U);
#endif
}

TEST_CASE("manifest-read: equal epochs, slot A wins (tiebreaker)") {
    // Slots have the same epoch but different nextGeneration values so we can
    // tell from the returned manifest which slot actually won the election.
    // Spec says: equal epoch → slot A.
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    auto writeSlot = [](char slot, uint32_t epoch, uint32_t nextGen) {
        ManifestData md;
        md.epoch = epoch;
        md.nextGeneration = nextGen;
        md.tailGeneration = 0;
        std::vector<std::uint8_t> bytes;
        serialiseManifest(md, bytes);
        std::ofstream f(manifestSlotPath(slot), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    };

    writeSlot('a', 6, 10); // A: epoch 6, nextGeneration 10
    writeSlot('b', 6, 20); // B: epoch 6, nextGeneration 20 — same epoch, different payload

    ManifestData out;
    CHECK(store.readManifest(out));
    CHECK_EQ(out.epoch, 6U);
    CHECK_EQ(out.nextGeneration, 10U); // slot A's payload — proves A won, not just epoch match
#endif
}

TEST_CASE("manifest-read: corrupt slot A, valid slot B returns B") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    {
        std::ofstream fa(manifestSlotPath('a'), std::ios::binary | std::ios::trunc);
        fa.write("GARBAGE", 7);
    }
    writeManifestSlotDirect('b', 4);

    ManifestData out;
    CHECK(store.readManifest(out));
    CHECK_EQ(out.epoch, 4U);
#endif
}

TEST_CASE("manifest-read: valid slot A, corrupt slot B returns A") {
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    writeManifestSlotDirect('a', 2);
    {
        std::ofstream fb(manifestSlotPath('b'), std::ios::binary | std::ios::trunc);
        fb.write("GARBAGE", 7);
    }

    ManifestData out;
    CHECK(store.readManifest(out));
    CHECK_EQ(out.epoch, 2U);
#endif
}

TEST_CASE("manifest-read: higher-epoch slot with corrupt CRC loses to valid lower-epoch slot") {
    // Critical: a partially-written higher-epoch slot must never beat a valid lower-epoch slot.
    // This is the key invariant that makes inactive-slot writes crash-safe.
#ifndef ARDUINO
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    // Slot A: valid, epoch 3.
    writeManifestSlotDirect('a', 3);

    // Slot B: written with epoch 10 but then CRC-corrupted (simulates a crash mid-write).
    {
        ManifestData md;
        md.epoch = 10;
        md.nextGeneration = 1;
        md.tailGeneration = 0;
        std::vector<std::uint8_t> bytes;
        serialiseManifest(md, bytes);
        bytes[bytes.size() - 8] ^= 0xFF; // flip a CRC byte
        std::ofstream fb(manifestSlotPath('b'), std::ios::binary | std::ios::trunc);
        fb.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    }

    ManifestData out;
    CHECK(store.readManifest(out));
    CHECK_EQ(out.epoch, 3U); // slot A wins; corrupt B is discarded
#endif
}

// --- Stage 3b: manifest wired into mount ---

TEST_CASE("manifest-mount: referenced segment missing returns DataCorrupt") {
    // If the manifest names a segment that does not exist on disk, mount must fail.
    // The manifest is now authoritative metadata; a missing referenced file is true corruption.
    cleanSpool();
    auto storeCfg = makeStoreConfig();
    {
        pqueue::AppendLogStore store(storeCfg);
        REQUIRE(store.mount().ok());

        pqueue::FileStoreIndex dummy{};
        REQUIRE(store.writeRecord(0, "A").ok());
        REQUIRE(store.writeIndex(dummy).ok());

        pqueue::append_log_detail::ManifestData md;
        md.tailGeneration = 1;
        md.nextGeneration = 2;
        REQUIRE(store.publishManifest(md).ok()); // manifest references seg-00000001.bin
    }
    // Delete the referenced segment — manifest is now pointing at a ghost
    std::filesystem::remove(segmentPath(1));
    {
        pqueue::AppendLogStore store(storeCfg);
        const auto st = store.mount();
        CHECK_FALSE(st.ok());
        CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
    }
}

TEST_CASE("manifest-mount: segment files without manifest return DataCorrupt") {
    // A store cannot have segments without a manifest. Simulate a segment written
    // by an older implementation that never published a manifest by planting a
    // valid segment file directly — bypassing Queue, which always publishes.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);
    {
        const std::string header = serializeSegmentHeader(1, 0);
        std::ofstream f(segmentPath(1), std::ios::binary | std::ios::trunc);
        f.write(header.data(), static_cast<std::streamsize>(header.size()));
    }
    {
        pqueue::Queue q(makeConfig());
        std::string out;
        const auto st = q.peek(out);
        CHECK_FALSE(st.ok());
        CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
    }
}

TEST_CASE("manifest-mount: normal mount with manifest recovers records") {
    // Write records to a segment, publish a manifest, remount — all records readable.
    cleanSpool();
    auto storeCfg = makeStoreConfig();
    {
        pqueue::AppendLogStore store(storeCfg);
        REQUIRE(store.mount().ok());

        pqueue::FileStoreIndex dummy{};
        REQUIRE(store.writeRecord(0, "alpha").ok());
        REQUIRE(store.writeIndex(dummy).ok());
        REQUIRE(store.writeRecord(1, "beta").ok());
        REQUIRE(store.writeIndex(dummy).ok());
        REQUIRE(store.writeRecord(2, "gamma").ok());
        REQUIRE(store.writeIndex(dummy).ok());

        pqueue::append_log_detail::ManifestData md;
        md.tailGeneration = 1;
        md.nextGeneration = 2;
        REQUIRE(store.publishManifest(md).ok());
    }
    {
        pqueue::AppendLogStore store(storeCfg);
        REQUIRE(store.mount().ok());
        pqueue::FileStoreIndex idx;
        REQUIRE(store.readIndex(idx).ok());
        CHECK_EQ(idx.count, 3U);
        std::string out;
        REQUIRE(store.readRecord(0, out).ok()); CHECK_EQ(out, "alpha");
        REQUIRE(store.readRecord(1, out).ok()); CHECK_EQ(out, "beta");
        REQUIRE(store.readRecord(2, out).ok()); CHECK_EQ(out, "gamma");
    }
}

TEST_CASE("manifest-mount: corrupt inactive slot does not affect mount (critical)") {
    // Critical crash-safety test. After two publishes (A then B), truncate B to
    // simulate a crash mid-write. Remount must recover all records via slot A.
    cleanSpool();
    auto storeCfg = makeStoreConfig();
    {
        pqueue::AppendLogStore store(storeCfg);
        REQUIRE(store.mount().ok());

        pqueue::FileStoreIndex dummy{};
        REQUIRE(store.writeRecord(0, "one").ok());
        REQUIRE(store.writeIndex(dummy).ok());
        REQUIRE(store.writeRecord(1, "two").ok());
        REQUIRE(store.writeIndex(dummy).ok());
        REQUIRE(store.writeRecord(2, "three").ok());
        REQUIRE(store.writeIndex(dummy).ok());

        pqueue::append_log_detail::ManifestData md;
        md.tailGeneration = 1;
        md.nextGeneration = 2;
        REQUIRE(store.publishManifest(md).ok()); // slot A, epoch 1
        REQUIRE(store.publishManifest(md).ok()); // slot B, epoch 2

        // Simulate crash mid-write to slot B
        std::filesystem::resize_file(manifestSlotPath('b'), 5);
    }
    {
        pqueue::AppendLogStore store(storeCfg);
        REQUIRE(store.mount().ok()); // slot A (epoch 1) is the surviving slot
        pqueue::FileStoreIndex idx;
        REQUIRE(store.readIndex(idx).ok());
        CHECK_EQ(idx.count, 3U);
        std::string out;
        REQUIRE(store.readRecord(0, out).ok()); CHECK_EQ(out, "one");
        REQUIRE(store.readRecord(1, out).ok()); CHECK_EQ(out, "two");
        REQUIRE(store.readRecord(2, out).ok()); CHECK_EQ(out, "three");
    }
}

// --- Stage 4: rollover wired to manifest ---

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

} // namespace

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
    // Header = 20. Two records fill 20+25+25 = 70 bytes exactly; third triggers rotation.
    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 70;
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());

    storeEnqueue(store, 1, "a");
    storeEnqueue(store, 2, "b");
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
    // gen=2 (tail) has seq=1 "c".
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
        // gen=2: header + one live record seq=1 "c"
        std::string seg = serializeSegmentHeader(2, 1);
        seg += serializeEnqueueEvent(1, "c");
        std::ofstream f(segmentPath(2), std::ios::binary | std::ios::trunc);
        f.write(seg.data(), static_cast<std::streamsize>(seg.size()));
    }

    pqueue::AppendLogStore store(makeStoreConfig());
    CHECK(store.mount().ok());
    REQUIRE(store.chooseCompactionRange().has_value());

    CHECK(store.compactOneSegment().ok());

    CHECK_FALSE(std::filesystem::exists(segmentPath(3))); // no new segment written
    CHECK_FALSE(store.chooseCompactionRange().has_value()); // dead range gone

    std::string out;
    CHECK(store.readRecord(1, out).ok()); // live record in gen=2 still accessible
    CHECK_EQ(out, "c");
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
    storeEnqueue(store, 3, "c"); // gen=1 full, gen=2 tail

    CHECK(store.compactOneSegment().ok());

    // gen=3 was written; records are accessible in order
    CHECK(std::filesystem::exists(segmentPath(3)));
    std::string out;
    CHECK(store.readRecord(1, out).ok()); CHECK_EQ(out, "a");
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

TEST_CASE("compaction: compactOneSegment is no-op when live bytes exceed maxSegmentBytes") {
    // gen=1 holds 2 records (liveBytes=70); maxSegmentBytes=50 < 70 → no-op.
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

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 50; // liveBytes(70) > 50 → no-op
    pqueue::AppendLogStore store(cfg);
    CHECK(store.mount().ok());
    CHECK(store.compactOneSegment().ok());

    CHECK_FALSE(std::filesystem::exists(segmentPath(3)));      // no new segment
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

    CHECK(store.compactOneSegment().ok()); // compacts gen=1 -> gen=3; cleanup removes gen=1
    CHECK_FALSE(std::filesystem::exists(segmentPath(1))); // cleaned up
    CHECK(std::filesystem::exists(segmentPath(2)));        // tail: live
    CHECK(std::filesystem::exists(segmentPath(3)));        // new compacted segment: live
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

    CHECK(store.compactOneSegment().ok()); // gen=1 -> gen=3; cleanup removes gen=1

    // Manifest references gen=2 (tail) and gen=3 (compacted). Both must exist.
    CHECK(std::filesystem::exists(segmentPath(2)));
    CHECK(std::filesystem::exists(segmentPath(3)));

    // All records still readable
    std::string out;
    CHECK(store.readRecord(1, out).ok()); CHECK_EQ(out, "a");
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

    // Trigger a second publish (compact the dead range) — removes the other dangling file
    CHECK(store.compactOneSegment().ok()); // dead range: drops gen=1; second cleanup runs
    CHECK_FALSE(std::filesystem::exists(segmentPath(3)));
    CHECK_FALSE(std::filesystem::exists(segmentPath(4)));
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

#endif // !ARDUINO

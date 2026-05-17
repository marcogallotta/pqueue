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

#endif // !ARDUINO

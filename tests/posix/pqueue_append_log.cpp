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

// --- buildActiveSegmentOrder unit tests ---

using pqueue::AppendLogStore;
using pqueue::append_log_detail::CompactionJournalRecord;

namespace {

CompactionJournalRecord makeReplacement(std::uint32_t oldStart, std::uint32_t oldEnd,
                                        std::uint32_t newStart, std::uint32_t newEnd) {
    CompactionJournalRecord r;
    r.oldStart = oldStart;
    r.oldEnd   = oldEnd;
    r.newStart = newStart;
    r.newEnd   = newEnd;
    return r;
}

} // namespace

TEST_CASE("buildActiveSegmentOrder: no journal, consecutive -> returns as-is") {
    std::vector<std::uint32_t> out;
    const auto st = AppendLogStore::buildActiveSegmentOrder({1, 2, 3}, {}, out);
    CHECK(st.ok());
    CHECK_EQ(out, std::vector<std::uint32_t>({1, 2, 3}));
}

TEST_CASE("buildActiveSegmentOrder: no journal, gap -> DataCorrupt") {
    std::vector<std::uint32_t> out;
    const auto st = AppendLogStore::buildActiveSegmentOrder({1, 3}, {}, out);
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
}

TEST_CASE("buildActiveSegmentOrder: journal 1..2->10..11, old segs still present") {
    std::vector<std::uint32_t> out;
    const auto st = AppendLogStore::buildActiveSegmentOrder(
        {1, 2, 3, 10, 11}, {makeReplacement(1, 2, 10, 11)}, out);
    CHECK(st.ok());
    CHECK_EQ(out, std::vector<std::uint32_t>({10, 11, 3}));
}

TEST_CASE("buildActiveSegmentOrder: journal 1..2->10..11, old segs cleaned up") {
    std::vector<std::uint32_t> out;
    const auto st = AppendLogStore::buildActiveSegmentOrder(
        {3, 10, 11}, {makeReplacement(1, 2, 10, 11)}, out);
    CHECK(st.ok());
    CHECK_EQ(out, std::vector<std::uint32_t>({10, 11, 3}));
}

TEST_CASE("buildActiveSegmentOrder: 4 old segs compacted into 2") {
    // Real compaction shape: many old segments -> fewer new segments
    std::vector<std::uint32_t> out;
    const auto st = AppendLogStore::buildActiveSegmentOrder(
        {5, 10, 11}, {makeReplacement(1, 4, 10, 11)}, out);
    CHECK(st.ok());
    CHECK_EQ(out, std::vector<std::uint32_t>({10, 11, 5}));
}

TEST_CASE("buildActiveSegmentOrder: 2 old segs compacted into 1") {
    std::vector<std::uint32_t> out;
    const auto st = AppendLogStore::buildActiveSegmentOrder(
        {3, 10}, {makeReplacement(1, 2, 10, 10)}, out);
    CHECK(st.ok());
    CHECK_EQ(out, std::vector<std::uint32_t>({10, 3}));
}

TEST_CASE("buildActiveSegmentOrder: chained compaction, each step reduces segment count") {
    // R1: 3 segs -> 2 segs, R2: those 2 -> 2 new segs; seg 4 untouched
    std::vector<std::uint32_t> out;
    const auto st = AppendLogStore::buildActiveSegmentOrder(
        {4, 20, 21},
        {makeReplacement(1, 3, 10, 11), makeReplacement(10, 11, 20, 21)},
        out);
    CHECK(st.ok());
    CHECK_EQ(out, std::vector<std::uint32_t>({20, 21, 4}));
}

TEST_CASE("buildActiveSegmentOrder: chained replacements") {
    std::vector<std::uint32_t> out;
    const auto st = AppendLogStore::buildActiveSegmentOrder(
        {3, 20, 21},
        {makeReplacement(1, 2, 10, 11), makeReplacement(10, 11, 20, 21)},
        out);
    CHECK(st.ok());
    CHECK_EQ(out, std::vector<std::uint32_t>({20, 21, 3}));
}

TEST_CASE("buildActiveSegmentOrder: new range segment missing -> DataCorrupt") {
    std::vector<std::uint32_t> out;
    const auto st = AppendLogStore::buildActiveSegmentOrder(
        {3, 10}, {makeReplacement(1, 2, 10, 11)}, out);
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
}

TEST_CASE("buildActiveSegmentOrder: old range not in logical chain -> DataCorrupt") {
    std::vector<std::uint32_t> out;
    const auto st = AppendLogStore::buildActiveSegmentOrder(
        {1, 2, 3}, {makeReplacement(5, 6, 10, 11)}, out);
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
}

TEST_CASE("buildActiveSegmentOrder: oversized new range -> DataCorrupt") {
    std::vector<std::uint32_t> out;
    const auto st = AppendLogStore::buildActiveSegmentOrder(
        {}, {makeReplacement(1, 5000, 6000, 11000)}, out);
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
}

TEST_CASE("buildActiveSegmentOrder: overlapping new ranges produce duplicates -> DataCorrupt") {
    // R1: 1->10, R2: 2->10 — both map different originals to generation 10,
    // producing a duplicate in the final chain
    std::vector<std::uint32_t> out;
    const auto st = AppendLogStore::buildActiveSegmentOrder(
        {1, 2, 3, 10},
        {makeReplacement(1, 1, 10, 10), makeReplacement(2, 2, 10, 10)},
        out);
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
}

// --- Journal-aware mount/replay integration tests ---

#ifndef ARDUINO

namespace {

const std::filesystem::path kJSpoolDir =
    "build/pqueue-spools/pqueue_append_log_journal_spool";

std::filesystem::path jSegPath(std::uint32_t gen) {
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", gen);
    return kJSpoolDir / ("pqueue-seg-" + std::string(buf, 8) + ".bin");
}

std::filesystem::path jJournalPath() {
    return kJSpoolDir / "pqueue-compact.bin";
}

void jClean() {
    std::error_code ec;
    std::filesystem::remove_all(kJSpoolDir, ec);
}

pqueue::Config makeJConfig() {
    pqueue::Config cfg;
    cfg.basePath        = kJSpoolDir.string();
    cfg.storeLayout     = pqueue::StoreLayout::AppendLog;
    cfg.recordSizeBytes = 256;
    cfg.reservedBytes   = 64 * 1024;
    cfg.maxSegmentBytes = 4096;
    return cfg;
}

void jWriteJournal(
    const std::vector<pqueue::append_log_detail::CompactionJournalRecord>& records)
{
    using namespace pqueue::append_log_detail;
    std::filesystem::create_directories(kJSpoolDir);
    std::ofstream f(jJournalPath(), std::ios::binary | std::ios::trunc);
    for (const auto& r : records) {
        const auto bytes = serializeCompactionJournalRecord(r);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
}

// Write a segment file with fixed-size (1-byte) payloads starting at baseSeq.
void jWriteSegment(std::uint32_t gen, std::uint32_t baseSeq,
                   const std::vector<std::string>& payloads)
{
    using namespace pqueue::append_log_detail;
    std::filesystem::create_directories(kJSpoolDir);
    std::string data = serializeSegmentHeader(gen, baseSeq);
    std::uint32_t seq = baseSeq;
    for (const auto& p : payloads)
        data += serializeEnqueueEvent(seq++, p);
    std::ofstream f(jSegPath(gen), std::ios::binary | std::ios::trunc);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

pqueue::append_log_detail::CompactionJournalRecord jReplacement(
    std::uint32_t oldStart, std::uint32_t oldEnd,
    std::uint32_t newStart, std::uint32_t newEnd)
{
    pqueue::append_log_detail::CompactionJournalRecord r;
    r.oldStart = oldStart; r.oldEnd = oldEnd;
    r.newStart = newStart; r.newEnd = newEnd;
    return r;
}

} // namespace

TEST_CASE("append-log mount: journal-backed layout replays in logical order") {
    // Disk: {3,10,11}; journal replaces [1..2] with [10..11]; logical order: [10,11,3]
    jClean();
    jWriteSegment(10, 0, {"A", "B"});  // seqs 0,1
    jWriteSegment(11, 2, {"C"});       // seq  2
    jWriteSegment(3,  3, {"D"});       // seq  3
    jWriteJournal({jReplacement(1, 2, 10, 11)});

    pqueue::Queue q(makeJConfig());
    CHECK_EQ(q.stats().count, 4U);
    std::string out;
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "A"); CHECK(q.pop().ok());
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "B"); CHECK(q.pop().ok());
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "C"); CHECK(q.pop().ok());
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "D");
}

TEST_CASE("append-log mount: old compacted segments on disk are not replayed") {
    // Old gens 1 and 2 still present (pre-cleanup); must not appear in the queue.
    jClean();
    jWriteSegment(1,  0, {"old1"});
    jWriteSegment(2,  1, {"old2"});
    jWriteSegment(10, 0, {"A", "B"});
    jWriteSegment(11, 2, {"C"});
    jWriteSegment(3,  3, {"D"});
    jWriteJournal({jReplacement(1, 2, 10, 11)});

    pqueue::Queue q(makeJConfig());
    CHECK_EQ(q.stats().count, 4U);
    std::string out;
    CHECK(q.peek(out).ok());
    CHECK_EQ(out, "A");
}

TEST_CASE("append-log mount: corrupt journal record causes mount failure") {
    // Both segments present so the only reason mount can fail is the bad CRC.
    jClean();
    jWriteSegment(1, 0, {"A"});
    jWriteSegment(2, 1, {"B"});
    {
        auto bytes = pqueue::append_log_detail::serializeCompactionJournalRecord(
            jReplacement(1, 1, 2, 2));
        bytes[28] ^= 0xFF; // corrupt CRC field at offset 28
        std::filesystem::create_directories(kJSpoolDir);
        std::ofstream f(jJournalPath(), std::ios::binary | std::ios::trunc);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    pqueue::Queue q(makeJConfig());
    std::string out;
    const auto st = q.peek(out);
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
}

TEST_CASE("append-log mount: truncated journal (partial record) causes mount failure") {
    // Both segments present so the only reason mount can fail is the partial size.
    // If partial tails were incorrectly ignored, the complete first record would succeed
    // and mount would succeed by replaying gen 2 only (one item, not two events) — this
    // test catches that regression.
    jClean();
    jWriteSegment(1, 0, {"A"});
    jWriteSegment(2, 1, {"B"});
    {
        using namespace pqueue::append_log_detail;
        auto bytes = serializeCompactionJournalRecord(jReplacement(1, 1, 2, 2));
        bytes += "\xDE\xAD\xBE\xEF"; // partial second record (4 bytes, not 36)
        std::filesystem::create_directories(kJSpoolDir);
        std::ofstream f(jJournalPath(), std::ios::binary | std::ios::trunc);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    pqueue::Queue q(makeJConfig());
    std::string out;
    const auto st = q.peek(out);
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
}

TEST_CASE("append-log mount: journal with missing active generation fails") {
    // Journal says gens 1..2 were replaced by gen 10, but gen 10 is absent from disk.
    jClean();
    jWriteSegment(3, 0, {"X"});
    jWriteJournal({jReplacement(1, 2, 10, 10)});

    pqueue::Queue q(makeJConfig());
    std::string out;
    const auto st = q.peek(out);
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
}

TEST_CASE("append-log mount: torn tail on logically-last segment is recoverable") {
    // Logical order: [10, 3]; seg 3 is the active (last) segment.
    jClean();
    jWriteSegment(10, 0, {"A"});
    jWriteSegment(3,  1, {"B", "C"}); // C will be torn
    // Truncate seg 3 midway through event C (each 1-byte event is 25 bytes total)
    const std::uintmax_t oneEventBytes =
        pqueue::append_log_detail::kEnqueueHeaderBytes + 1 +
        pqueue::append_log_detail::kEventTrailerBytes;
    std::filesystem::resize_file(jSegPath(3),
        pqueue::append_log_detail::kSegmentHeaderBytes + oneEventBytes + 5);
    jWriteJournal({jReplacement(1, 2, 10, 10)});

    pqueue::Queue q(makeJConfig());
    CHECK_EQ(q.stats().count, 2U);
    std::string out;
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "A"); CHECK(q.pop().ok());
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "B");
}

TEST_CASE("append-log mount: torn tail on logically-non-last segment causes failure") {
    // Logical order: [10, 3]; seg 10 is logically first (isLastSegment = false).
    // A torn tail in seg 10 must be DataCorrupt, not silently recovered.
    // Regression guard: without logical isLastSegment, numeric order would place
    // gen 10 last (10 > 3), silently recovering the torn tail instead of failing.
    jClean();
    jWriteSegment(10, 0, {"A", "B"}); // B will be torn
    const std::uintmax_t oneEventBytes =
        pqueue::append_log_detail::kEnqueueHeaderBytes + 1 +
        pqueue::append_log_detail::kEventTrailerBytes;
    std::filesystem::resize_file(jSegPath(10),
        pqueue::append_log_detail::kSegmentHeaderBytes + oneEventBytes + 5);
    jWriteSegment(3, 2, {"C"});
    jWriteJournal({jReplacement(1, 2, 10, 10)});

    pqueue::Queue q(makeJConfig());
    std::string out;
    const auto st = q.peek(out);
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
}

TEST_CASE("append-log mount: no journal, first segment generation != 1 causes failure") {
    // Without a journal the layout must be consecutive from 1.
    // A store that starts at gen 5 with no journal is corrupt.
    jClean();
    jWriteSegment(5, 0, {"A"});
    pqueue::Queue q(makeJConfig());
    std::string out;
    const auto st = q.peek(out);
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
}

TEST_CASE("append-log mount: single-record journal, non-monotonic order [5,6,3,4]") {
    // One journal record replaces [1..2] with [5..6].  Segs 3 and 4 written after.
    // Disk: {3,4,5,6}; logical order: [5,6,3,4].
    jClean();
    jWriteSegment(5, 0, {"A"});  // seq 0
    jWriteSegment(6, 1, {"B"});  // seq 1
    jWriteSegment(3, 2, {"C"});  // seq 2
    jWriteSegment(4, 3, {"D"});  // seq 3
    jWriteJournal({jReplacement(1, 2, 5, 6)});

    pqueue::Queue q(makeJConfig());
    CHECK_EQ(q.stats().count, 4U);
    std::string out;
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "A"); CHECK(q.pop().ok());
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "B"); CHECK(q.pop().ok());
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "C"); CHECK(q.pop().ok());
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "D");
}

TEST_CASE("append-log mount: non-monotonic layout, torn tail on logically-non-last (gen 6) fails") {
    // Logical order [5,6,3,4]: gen 6 is at index 1 (not last), but numerically largest.
    // Regression guard: without logical isLastSegment the numeric-last gen 6 would be
    // silently recovered instead of failing.
    jClean();
    jWriteSegment(5, 0, {"A"});
    jWriteSegment(6, 1, {"B", "C"}); // C will be torn
    const std::uintmax_t oneEventBytes =
        pqueue::append_log_detail::kEnqueueHeaderBytes + 1 +
        pqueue::append_log_detail::kEventTrailerBytes;
    std::filesystem::resize_file(jSegPath(6),
        pqueue::append_log_detail::kSegmentHeaderBytes + oneEventBytes + 5);
    jWriteSegment(3, 3, {"D"});
    jWriteSegment(4, 4, {"E"});
    jWriteJournal({jReplacement(1, 2, 5, 6)});

    pqueue::Queue q(makeJConfig());
    std::string out;
    const auto st = q.peek(out);
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
}

TEST_CASE("append-log mount: non-monotonic layout, torn tail on logically-last (gen 4) is recoverable") {
    // Logical order [5,6,3,4]: gen 4 is the active (last) segment, but numerically not last.
    // Without logical isLastSegment gen 4 would be treated as non-last and fail; with it,
    // the torn tail is recovered correctly.
    jClean();
    jWriteSegment(5, 0, {"A"});
    jWriteSegment(6, 1, {"B"});
    jWriteSegment(3, 2, {"C"});
    jWriteSegment(4, 3, {"D", "E"}); // E will be torn
    const std::uintmax_t oneEventBytes =
        pqueue::append_log_detail::kEnqueueHeaderBytes + 1 +
        pqueue::append_log_detail::kEventTrailerBytes;
    std::filesystem::resize_file(jSegPath(4),
        pqueue::append_log_detail::kSegmentHeaderBytes + oneEventBytes + 5);
    jWriteJournal({jReplacement(1, 2, 5, 6)});

    pqueue::Queue q(makeJConfig());
    CHECK_EQ(q.stats().count, 4U);
    std::string out;
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "A"); CHECK(q.pop().ok());
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "B"); CHECK(q.pop().ok());
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "C"); CHECK(q.pop().ok());
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "D");
}

TEST_CASE("append-log mount: multi-record journal applies all records in order") {
    // Two journal records: R1 compacts [1..3] into [10..11], then R2 compacts
    // [10..11] into [20..21].  Seg 4 was written after all compactions.
    // Disk: {4,20,21}; logical order: [20,21,4].
    jClean();
    jWriteSegment(20, 0, {"A", "B", "C"}); // seqs 0,1,2 (compacted content of 1..3)
    jWriteSegment(21, 3, {"D"});            // seq 3
    jWriteSegment(4,  4, {"E"});            // seq 4  (written after compactions)
    jWriteJournal({jReplacement(1, 3, 10, 11), jReplacement(10, 11, 20, 21)});

    pqueue::Queue q(makeJConfig());
    CHECK_EQ(q.stats().count, 5U);
    std::string out;
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "A"); CHECK(q.pop().ok());
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "B"); CHECK(q.pop().ok());
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "C"); CHECK(q.pop().ok());
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "D"); CHECK(q.pop().ok());
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "E");
}

TEST_CASE("append-log mount: no journal, physical gap [1,3] causes DataCorrupt") {
    // Without a journal, segments must be consecutive from gen 1.
    // {1, 3} with gen 2 absent is a gap — DataCorrupt.
    jClean();
    jWriteSegment(1, 0, {"A"});
    jWriteSegment(3, 1, {"B"});
    pqueue::Queue q(makeJConfig());
    std::string out;
    const auto st = q.peek(out);
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
}

TEST_CASE("append-log mount: empty store no journal mounts successfully") {
    jClean();
    pqueue::Queue q(makeJConfig());
    CHECK_EQ(q.stats().count, 0U);
    std::string out;
    CHECK_FALSE(q.peek(out).ok());
}

TEST_CASE("append-log mount: journal present but no segments on disk causes DataCorrupt") {
    // Journal refers to active gens 10..11 that are absent from disk.
    jClean();
    jWriteJournal({jReplacement(1, 2, 10, 11)});
    pqueue::Queue q(makeJConfig());
    std::string out;
    const auto st = q.peek(out);
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
}

TEST_CASE("append-log mount: non-consecutive base chain causes DataCorrupt") {
    // Journal replaces [1..2] with gen 10; disk has {4,10}.
    // Base chain reconstructs as [1,2,4] — gen 3 is missing — DataCorrupt.
    jClean();
    jWriteSegment(4,  0, {"X"});
    jWriteSegment(10, 0, {"Y"});
    jWriteJournal({jReplacement(1, 2, 10, 10)});
    pqueue::Queue q(makeJConfig());
    std::string out;
    const auto st = q.peek(out);
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
}

TEST_CASE("append-log mount: bad middle complete journal record causes DataCorrupt") {
    // Three complete journal records; index 1 (middle) has a corrupted CRC.
    // Disk has exactly the active gens needed by r1 + r3: if the parser wrongly skipped
    // r2, r1+r3 would yield logical order [10,11] and mount would succeed.  Correct
    // fail-closed code must reject r2 and return DataCorrupt before reaching that point.
    //
    //   r1 valid  : [1..2] -> [10]   (new seg 10 present on disk)
    //   r2 corrupt: complete-sized, bad CRC
    //   r3 valid  : [3..4] -> [11]   (new seg 11 present on disk)
    jClean();
    jWriteSegment(10, 0, {"A", "B"});
    jWriteSegment(11, 2, {"C", "D"});
    {
        using namespace pqueue::append_log_detail;
        std::filesystem::create_directories(kJSpoolDir);
        auto r1 = serializeCompactionJournalRecord(jReplacement(1, 2, 10, 10));
        auto r2 = serializeCompactionJournalRecord(jReplacement(3, 4, 11, 11));
        r2[28] ^= 0xFF; // corrupt CRC field at offset 28
        auto r3 = serializeCompactionJournalRecord(jReplacement(3, 4, 11, 11));
        const std::string bytes = r1 + r2 + r3;
        std::ofstream f(jJournalPath(), std::ios::binary | std::ios::trunc);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    pqueue::Queue q(makeJConfig());
    std::string out;
    const auto st = q.peek(out);
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
}

TEST_CASE("append-log mount: new generation skips all disk gens including old compacted-away") {
    // Disk: {1,2,3,10,11}; journal replaces [1..2] with [10..11]; logical order [10,11,3].
    // Max disk gen = 11 even though logical-last gen = 3;
    // nextGeneration_ must be 12, not 4.
    // maxSegmentBytes is set to seg 3's exact on-disk size so that the next enqueue
    // overflows it and forces rotation to a new generation.
    jClean();
    jWriteSegment(1,  0, {"old-A"});
    jWriteSegment(2,  1, {"old-B"});
    jWriteSegment(10, 0, {"A", "B"});
    jWriteSegment(11, 2, {"C"});
    jWriteSegment(3,  3, {"D"});
    jWriteJournal({jReplacement(1, 2, 10, 11)});

    auto cfg = makeJConfig();
    {
        using namespace pqueue::append_log_detail;
        // Seg 3 holds exactly one 1-byte event; set limit to that size so the next
        // enqueue triggers rotation rather than appending in-place.
        cfg.maxSegmentBytes = kSegmentHeaderBytes + kEnqueueHeaderBytes + 1 + kEventTrailerBytes;
    }

    pqueue::Queue q(cfg);
    CHECK_EQ(q.stats().count, 4U);

    CHECK(q.enqueue("E").ok());
    CHECK_EQ(q.stats().count, 5U);

    // FIFO order must be A,B,C,D,E
    std::string out;
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "A"); CHECK(q.pop().ok());
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "B"); CHECK(q.pop().ok());
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "C"); CHECK(q.pop().ok());
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "D"); CHECK(q.pop().ok());
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "E"); CHECK(q.pop().ok());
    CHECK_EQ(q.stats().count, 0U);

    // Rotation must have created gen 12 (max disk gen 11 + 1), not gen 4.
    CHECK(std::filesystem::exists(jSegPath(12)));
    CHECK_FALSE(std::filesystem::exists(jSegPath(4)));
}

#endif // !ARDUINO

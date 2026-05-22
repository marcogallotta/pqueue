#include "pqueue_append_log_support.h"

#ifndef ARDUINO

#include "pqueue/queue.h"
#include "pqueue/append_log_store.h"

#include <filesystem>
#include <fstream>

// All tests call validateUnlocked() directly on AppendLogStore rather than
// going through Queue::validate(). This bypasses the queue lock, which
// requires a successful mount -- corrupt stores would fail the lock and
// never reach validateUnlocked.

namespace {

pqueue::ValidationResult validateStore(const pqueue::AppendLogConfig& cfg) {
    pqueue::AppendLogStore store(cfg);
    return store.validateUnlocked();
}

} // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("validate: valid store validates cleanly") {
    cleanSpool();
    {
        pqueue::Queue q(makeConfig());
        CHECK(q.enqueue("a").ok());
        CHECK(q.enqueue("b").ok());
    }
    const auto r = validateStore(makeStoreConfig());
    CHECK(r.ok);
    CHECK(r.errors.empty());
    CHECK_GE(r.checkedRecords, 2U);
}

TEST_CASE("validate: fresh empty store validates cleanly") {
    resetSpool();
    const auto r = validateStore(makeStoreConfig());
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

TEST_CASE("validate: missing referenced segment is MetadataCorrupt") {
    resetSpool();
    using namespace pqueue::append_log_detail;
    ManifestData md;
    md.epoch = 1;
    md.ranges = {{ 1, 1 }};
    md.tailGeneration = 2;
    md.nextGeneration = 3;
    plantManifest(md);
    plantSegment(2); // tail only; sealed seg-1 is missing
    const auto r = validateStore(makeStoreConfig());
    CHECK_FALSE(r.ok);
    REQUIRE_FALSE(r.errors.empty());
    CHECK(r.errors[0].code == pqueue::ValidationIssueCode::MetadataCorrupt);
}

TEST_CASE("validate: both manifest slots corrupt is MetadataCorrupt") {
    resetSpool();
    corruptSlot('a');
    corruptSlot('b');
    const auto r = validateStore(makeStoreConfig());
    CHECK_FALSE(r.ok);
    REQUIRE_FALSE(r.errors.empty());
    CHECK(r.errors[0].code == pqueue::ValidationIssueCode::MetadataCorrupt);
}

TEST_CASE("validate: overlapping manifest ranges is MetadataCorrupt") {
    resetSpool();
    using namespace pqueue::append_log_detail;
    ManifestData md;
    md.epoch = 1;
    md.ranges = {{ 1, 3 }, { 3, 4 }}; // overlap at gen 3
    md.tailGeneration = 5;
    md.nextGeneration = 6;
    plantManifest(md);
    plantSegment(1); plantSegment(2); plantSegment(3); plantSegment(4); plantSegment(5);
    const auto r = validateStore(makeStoreConfig());
    CHECK_FALSE(r.ok);
    REQUIRE_FALSE(r.errors.empty());
    CHECK(r.errors[0].code == pqueue::ValidationIssueCode::MetadataCorrupt);
}

TEST_CASE("validate: nextGeneration below max referenced generation is MetadataCorrupt") {
    resetSpool();
    using namespace pqueue::append_log_detail;
    ManifestData md;
    md.epoch = 1;
    md.ranges = {};
    md.tailGeneration = 5;
    md.nextGeneration = 3; // must be > 5
    plantManifest(md);
    plantSegment(5);
    const auto r = validateStore(makeStoreConfig());
    CHECK_FALSE(r.ok);
    REQUIRE_FALSE(r.errors.empty());
    CHECK(r.errors[0].code == pqueue::ValidationIssueCode::MetadataCorrupt);
}

TEST_CASE("validate: wrong segment header generation is MetadataCorrupt") {
    resetSpool();
    using namespace pqueue::append_log_detail;
    ManifestData md;
    md.epoch = 1;
    md.ranges = {};
    md.tailGeneration = 1;
    md.nextGeneration = 2;
    plantManifest(md);
    // Plant a segment with generation=2 in the header, but name it seg-00000001.bin
    plantSegment(2);
    std::error_code ec;
    std::filesystem::rename(segmentPath(2), segmentPath(1), ec);
    REQUIRE_FALSE(ec);
    const auto r = validateStore(makeStoreConfig());
    CHECK_FALSE(r.ok);
    REQUIRE_FALSE(r.errors.empty());
    CHECK(r.errors[0].code == pqueue::ValidationIssueCode::MetadataCorrupt);
}

TEST_CASE("validate: corrupt CRC in sealed segment is JournalCorrupt") {
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
    constexpr std::uintmax_t kCrcOffset =
        pqueue::append_log_detail::kSegmentHeaderBytes +
        pqueue::append_log_detail::kEnqueueHeaderBytes + 1;
    patchFile(segmentPath(1), kCrcOffset, {0xDE, 0xAD, 0xBE, 0xEF});
    auto scfg = makeStoreConfig();
    scfg.maxSegmentBytes = 128;
    const auto r = validateStore(scfg);
    CHECK_FALSE(r.ok);
    REQUIRE_FALSE(r.errors.empty());
    CHECK(r.errors[0].code == pqueue::ValidationIssueCode::JournalCorrupt);
}

TEST_CASE("validate: torn tail in tail segment is ok") {
    resetSpool();
    using namespace pqueue::append_log_detail;
    ManifestData md;
    md.epoch = 1;
    md.ranges = {};
    md.tailGeneration = 1;
    md.nextGeneration = 2;
    plantManifest(md);
    // Header-only tail segment: no committed events (simulates torn tail)
    plantSegment(1);
    const auto r = validateStore(makeStoreConfig());
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

TEST_CASE("validate: torn tail in sealed segment is JournalCorrupt") {
    resetSpool();
    using namespace pqueue::append_log_detail;
    ManifestData md;
    md.epoch = 1;
    md.ranges = {{ 1, 1 }};
    md.tailGeneration = 2;
    md.nextGeneration = 3;
    plantManifest(md);
    // Sealed segment with partial trailing bytes: 3 bytes after the header
    // triggers `remaining < 4` in the scan loop, so offset < fileSize → JournalCorrupt.
    plantSegment(1, 0, "\x51\x45\x50"); // 3 garbage bytes
    plantSegment(2);                     // valid tail
    const auto r = validateStore(makeStoreConfig());
    CHECK_FALSE(r.ok);
    REQUIRE_FALSE(r.errors.empty());
    CHECK(r.errors[0].code == pqueue::ValidationIssueCode::JournalCorrupt);
}

TEST_CASE("validate: dangling unreferenced segment does not cause error") {
    cleanSpool();
    {
        pqueue::Queue q(makeConfig());
        CHECK(q.enqueue("hello").ok());
    }
    plantSegment(99); // not referenced by manifest
    const auto r = validateStore(makeStoreConfig());
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

#endif // !ARDUINO

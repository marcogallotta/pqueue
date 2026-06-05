#ifndef ARDUINO

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "pqueue/queue.h"
#include "pqueue/file_system.h"
#include "pqueue_append_log_support.h"

#include "doctest/doctest.h"

namespace {

const std::filesystem::path kEdgesSpoolDir = "build/pqueue-spools/pqueue_queue_edges_spool";

void cleanEdgesSpool() {
    std::error_code ec;
    std::filesystem::remove_all(kEdgesSpoolDir, ec);
}

pqueue::Config makeEdgesConfig() {
    pqueue::Config cfg;
    cfg.basePath = kEdgesSpoolDir.string();
    cfg.maxSegmentBytes = 1024;
    cfg.minFreeBytes = 0;
    return cfg;
}

struct VisitContext {
    std::vector<std::string> records;
    std::vector<std::uint32_t> sequences;
    std::vector<std::uint32_t> ordinals;
    std::size_t stopAfter = std::numeric_limits<std::size_t>::max();
};

bool capturingVisitor(void* rawContext, const std::string& record, std::uint32_t sequence, std::uint32_t ordinal) {
    auto* context = static_cast<VisitContext*>(rawContext);
    context->records.push_back(record);
    context->sequences.push_back(sequence);
    context->ordinals.push_back(ordinal);
    return context->records.size() < context->stopAfter;
}

} // namespace

TEST_CASE("pqueue rejects enqueue when append-log segment is too small") {
    cleanEdgesSpool();
    pqueue::Config config = makeEdgesConfig();
    config.maxSegmentBytes = 1; // too small to fit any record
    pqueue::Queue queue(config);

    const auto status = queue.enqueue("x");

    CHECK_FALSE(status.ok());
    CHECK(status.code == pqueue::StatusCode::RecordTooLarge);
}

TEST_CASE("pqueue rewriteFront rejects oversized record and keeps front unchanged") {
    cleanEdgesSpool();
    pqueue::Config config = makeEdgesConfig();
    config.recordSizeBytes = 4;
    pqueue::Queue queue(config);

    REQUIRE(queue.enqueue("1234").ok());
    const auto status = pqueue::QueueTestAccess::rewriteFront(queue, "12345");

    CHECK_FALSE(status.ok());
    CHECK(status.code == pqueue::StatusCode::RecordTooLarge);

    std::string out;
    REQUIRE(queue.peek(out).ok());
    CHECK_EQ(out, "1234");
}

TEST_CASE("pqueue visitRecords rejects null visitor") {
    cleanEdgesSpool();
    pqueue::Queue queue(makeEdgesConfig());
    REQUIRE(queue.enqueue("one").ok());

    const auto status = pqueue::QueueTestAccess::visitRecords(queue, nullptr, nullptr);

    CHECK_FALSE(status.ok());
    CHECK(status.code == pqueue::StatusCode::InvalidArgument);
}

TEST_CASE("pqueue visitRecords stops when visitor returns false") {
    cleanEdgesSpool();
    pqueue::Queue queue(makeEdgesConfig());
    REQUIRE(queue.enqueue("one").ok());
    REQUIRE(queue.enqueue("two").ok());
    REQUIRE(queue.enqueue("three").ok());

    VisitContext context;
    context.stopAfter = 1;
    const auto status = pqueue::QueueTestAccess::visitRecords(queue, capturingVisitor, &context);

    CHECK(status.ok());
    REQUIRE_EQ(context.records.size(), 1U);
    CHECK_EQ(context.records[0], "one");
    CHECK_EQ(context.sequences[0], 0U);
    CHECK_EQ(context.ordinals[0], 0U);
}

// pqueue visitRecords returns read failure from active record -- deferred:
// needs AppendLog segment read-failure injection, not yet supported.

TEST_CASE("pqueue rewriteFront returns QueueFull when byte budget exhausted; front unchanged") {
    // Verifies that QueueFull from rewriteFront leaves the front record intact.
    // The correct caller response is to pop the front and move on — NOT to evict
    // and retry rewriteFront, which would overwrite the wrong record after eviction.
    cleanEdgesSpool();
    using namespace pqueue::append_log_detail;

    // One 1-byte record per segment: kSegmentHeaderBytes(20) + kEnqueueOverheadBytes(24) + 1 = 45 B.
    // After two records: seg1(45) + seg2(45) + manifest-a(30) + manifest-b(38) = 158 B.
    // A third write cannot fit: 158 + appendGrowthBytes(61) + drainReserve(45) = 264 > 235.
    const std::uint32_t kPerRecord    = kSegmentHeaderBytes + kEnqueueOverheadBytes + 1;
    const std::uint32_t kDrainReserve = kPerRecord;
    const std::uint32_t kBudget       = 2 * kPerRecord + 2 * kManifestFixedBytes + kDrainReserve + 40;

    pqueue::Config cfg;
    cfg.basePath          = kEdgesSpoolDir.string();
    cfg.maxSegmentBytes   = kPerRecord + 4; // forces rotation after each 1-byte record
    cfg.reservedBytes     = kBudget;
    cfg.drainReserveBytes = kDrainReserve;
    cfg.minFreeBytes      = 0;
    pqueue::Queue queue(cfg);

    REQUIRE(queue.enqueue("a").ok());
    REQUIRE(queue.enqueue("b").ok());

    const auto st = pqueue::QueueTestAccess::rewriteFront(queue, "x");
    REQUIRE_FALSE(st.ok());
    CHECK(st.code == pqueue::StatusCode::QueueFull);

    std::string front;
    REQUIRE(queue.peek(front).ok());
    CHECK_EQ(front, "a");
    CHECK_EQ(queue.stats().count, 2U);
}

TEST_CASE("admission: fresh store budget includes manifest-a creation") {
    using namespace pqueue::append_log_detail;
    // Growth for the first write = segment header + event + manifest-a creation.
    const std::uint32_t firstWriteGrowth =
        kSegmentHeaderBytes + kEnqueueOverheadBytes + 1 + kManifestFixedBytes;

    cleanEdgesSpool();
    {
        pqueue::Config cfg;
        cfg.basePath          = kEdgesSpoolDir.string();
        cfg.maxSegmentBytes   = 1024;
        cfg.reservedBytes     = firstWriteGrowth - 1;
        cfg.drainReserveBytes = 0;
        cfg.minFreeBytes      = 0;
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("a").code == pqueue::StatusCode::QueueFull);
    }
    cleanEdgesSpool();
    {
        pqueue::Config cfg;
        cfg.basePath          = kEdgesSpoolDir.string();
        cfg.maxSegmentBytes   = 1024;
        cfg.reservedBytes     = firstWriteGrowth;
        cfg.drainReserveBytes = 0;
        cfg.minFreeBytes      = 0;
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("a").ok());
    }
}

TEST_CASE("admission: first rotation budget includes creating manifest-b") {
    using namespace pqueue::append_log_detail;
    // One 1-byte record per segment.
    const std::uint32_t kPerRecord = kSegmentHeaderBytes + kEnqueueOverheadBytes + 1;
    // After the first enqueue: totalOnDiskBytes = kPerRecord + kManifestFixedBytes.
    // Second enqueue triggers rotation and writes manifest-b for the first time:
    // growth = segment header + event + (kManifestFixedBytes + 1 range entry).
    const std::uint32_t afterFirst   = kPerRecord + kManifestFixedBytes;
    const std::uint32_t secondGrowth = kSegmentHeaderBytes + kEnqueueOverheadBytes + 1
                                     + kManifestFixedBytes + kManifestRangeEntryBytes;

    cleanEdgesSpool();
    {
        pqueue::Config cfg;
        cfg.basePath          = kEdgesSpoolDir.string();
        cfg.maxSegmentBytes   = kPerRecord + 4;
        cfg.reservedBytes     = afterFirst + secondGrowth - 1;
        cfg.drainReserveBytes = 0;
        cfg.minFreeBytes      = 0;
        pqueue::Queue q(cfg);
        REQUIRE(q.enqueue("a").ok());
        CHECK(q.enqueue("b").code == pqueue::StatusCode::QueueFull);
    }
    cleanEdgesSpool();
    {
        pqueue::Config cfg;
        cfg.basePath          = kEdgesSpoolDir.string();
        cfg.maxSegmentBytes   = kPerRecord + 4;
        cfg.reservedBytes     = afterFirst + secondGrowth;
        cfg.drainReserveBytes = 0;
        cfg.minFreeBytes      = 0;
        pqueue::Queue q(cfg);
        REQUIRE(q.enqueue("a").ok());
        CHECK(q.enqueue("b").ok());
    }
}

TEST_CASE("pqueue pop preserves front when index write fails") {
    cleanEdgesSpool();
    auto inner = pqueue::makePosixFileSystem();
    auto fs = std::make_shared<FaultInjectingFs>(inner);

    pqueue::Config config = makeEdgesConfig();
    config.fileSystem = fs;
    pqueue::Queue queue(config);
    REQUIRE(queue.enqueue("one").ok());
    REQUIRE(queue.enqueue("two").ok());

    fs->failNextWriteAtTo = "seg-";
    const auto status = queue.pop();

    CHECK_FALSE(status.ok());
    CHECK(status.code == pqueue::StatusCode::WriteFailed);

    std::string out;
    REQUIRE(queue.peek(out).ok());
    CHECK_EQ(out, "one");
    CHECK_EQ(queue.stats().count, 2U);
}

#endif // !ARDUINO

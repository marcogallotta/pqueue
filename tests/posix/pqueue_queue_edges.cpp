#ifndef ARDUINO

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#define private public
#include "pqueue/queue.h"
#undef private

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
    const auto status = queue.rewriteFront("12345");

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

    const auto status = queue.visitRecords(nullptr, nullptr);

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
    const auto status = queue.visitRecords(capturingVisitor, &context);

    CHECK(status.ok());
    REQUIRE_EQ(context.records.size(), 1U);
    CHECK_EQ(context.records[0], "one");
    CHECK_EQ(context.sequences[0], 0U);
    CHECK_EQ(context.ordinals[0], 0U);
}

// pqueue visitRecords returns read failure from active record -- deferred:
// needs AppendLog segment read-failure injection, not yet supported.

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

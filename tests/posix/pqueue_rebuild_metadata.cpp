#include "pqueue/queue.h"
#include "support/pqueue_file_store_support.h"

#include "doctest/doctest.h"

#ifndef ARDUINO

#include <algorithm>
#include <cstddef>
#include <string>

namespace {

constexpr const char* kSpoolName = "pqueue.spool";

void corruptMetadataArea(const std::shared_ptr<pqueue_test::FakeFileSystem>& fileSystem) {
    auto it = fileSystem->files.find(kSpoolName);
    REQUIRE(it != fileSystem->files.end());
    constexpr std::size_t kMetadataBytes =
        pqueue::storage_detail::kCheckpointSlots * pqueue::storage_detail::kCheckpointRecordBytes
        + 4096; // default journalBytes
    REQUIRE(it->second.size() >= kMetadataBytes);
    std::fill(it->second.begin(), it->second.begin() + kMetadataBytes, '\xff');
}

} // namespace

TEST_CASE("rebuildMetadata recovers queue state from corrupt metadata") {
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    pqueue::Config config = pqueue_test::makeQueueConfig(fileSystem, 4096, 32);

    {
        pqueue::Queue queue(config);
        REQUIRE(queue.enqueue("alpha").ok());
        REQUIRE(queue.enqueue("bravo").ok());
        REQUIRE(queue.enqueue("charlie").ok());
    }

    corruptMetadataArea(fileSystem);

    pqueue::Queue queue(config);
    REQUIRE(queue.rebuildMetadata().ok());

    CHECK_EQ(queue.stats().count, 3U);

    std::string out;
    REQUIRE(queue.peek(out).ok());
    CHECK_EQ(out, "alpha");
    CHECK(queue.pop().ok());
    REQUIRE(queue.peek(out).ok());
    CHECK_EQ(out, "bravo");
    CHECK(queue.pop().ok());
    REQUIRE(queue.peek(out).ok());
    CHECK_EQ(out, "charlie");
}

TEST_CASE("rebuildMetadata produces empty queue when no active slots") {
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    pqueue::Config config = pqueue_test::makeQueueConfig(fileSystem, 4096, 32);

    {
        pqueue::Queue queue(config);
        REQUIRE(queue.enqueue("gone").ok());
        REQUIRE(queue.pop().ok());
    }

    corruptMetadataArea(fileSystem);

    pqueue::Queue queue(config);
    REQUIRE(queue.rebuildMetadata().ok());

    CHECK_EQ(queue.stats().count, 0U);
    std::string out;
    CHECK_EQ(queue.peek(out).code, pqueue::StatusCode::QueueEmpty);
}

TEST_CASE("rebuildMetadata recovers remaining records after partial dequeue") {
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    pqueue::Config config = pqueue_test::makeQueueConfig(fileSystem, 4096, 32);

    {
        pqueue::Queue queue(config);
        REQUIRE(queue.enqueue("one").ok());
        REQUIRE(queue.enqueue("two").ok());
        REQUIRE(queue.enqueue("three").ok());
        REQUIRE(queue.enqueue("four").ok());
        REQUIRE(queue.enqueue("five").ok());
        REQUIRE(queue.pop().ok());
        REQUIRE(queue.pop().ok());
    }

    corruptMetadataArea(fileSystem);

    pqueue::Queue queue(config);
    REQUIRE(queue.rebuildMetadata().ok());

    CHECK_EQ(queue.stats().count, 3U);

    std::string out;
    REQUIRE(queue.peek(out).ok());
    CHECK_EQ(out, "three");
    CHECK(queue.pop().ok());
    REQUIRE(queue.peek(out).ok());
    CHECK_EQ(out, "four");
    CHECK(queue.pop().ok());
    REQUIRE(queue.peek(out).ok());
    CHECK_EQ(out, "five");
}

TEST_CASE("rebuildMetadata is idempotent on healthy queue") {
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    pqueue::Config config = pqueue_test::makeQueueConfig(fileSystem, 4096, 32);
    pqueue::Queue queue(config);

    REQUIRE(queue.enqueue("x").ok());
    REQUIRE(queue.enqueue("y").ok());
    REQUIRE(queue.rebuildMetadata().ok());

    CHECK_EQ(queue.stats().count, 2U);
    std::string out;
    REQUIRE(queue.peek(out).ok());
    CHECK_EQ(out, "x");
}

TEST_CASE("rebuildMetadata refuses when active slot sequence numbers have a gap") {
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    pqueue::Config config = pqueue_test::makeQueueConfig(fileSystem, 4096, 32);

    {
        pqueue::Queue queue(config);
        REQUIRE(queue.enqueue("first").ok());
        REQUIRE(queue.enqueue("second").ok());
        REQUIRE(queue.enqueue("third").ok());
    }

    fileSystem->corruptSlotHeader(1, pqueue_test::slotSize(32));
    corruptMetadataArea(fileSystem);

    pqueue::Queue queue(config);
    CHECK_FALSE(queue.rebuildMetadata().ok());
}

TEST_CASE("rebuildMetadata leaves queue fully functional and validate-clean") {
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    pqueue::Config config = pqueue_test::makeQueueConfig(fileSystem, 4096, 32);

    {
        pqueue::Queue queue(config);
        REQUIRE(queue.enqueue("test-record").ok());
    }

    corruptMetadataArea(fileSystem);

    pqueue::Queue queue(config);
    REQUIRE(queue.rebuildMetadata().ok());

    const auto validation = queue.validate();
    CHECK(validation.ok);
    CHECK(validation.errors.empty());

    REQUIRE(queue.enqueue("new-after-rebuild").ok());
    CHECK_EQ(queue.stats().count, 2U);
}

#endif // !ARDUINO

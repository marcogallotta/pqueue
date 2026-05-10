#include "pqueue/queue.h"
#include "support/pqueue_file_store_support.h"

#include "doctest/doctest.h"

#ifndef ARDUINO

#include <string>
#include <vector>

TEST_CASE("queue RejectNewest returns QueueFull when full (default)") {
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    pqueue::Queue queue(pqueue_test::makeQueueConfig(fileSystem));

    // Fill to capacity (160 bytes / 52 bytes per slot = 3 records)
    REQUIRE(queue.enqueue("one").ok());
    REQUIRE(queue.enqueue("two").ok());
    REQUIRE(queue.enqueue("three").ok());

    const pqueue::Status result = queue.enqueue("four");
    CHECK(result.code == pqueue::StatusCode::QueueFull);
    CHECK_EQ(queue.stats().count, 3U);
}

TEST_CASE("queue DropOldest evicts front record when full") {
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    pqueue::Config config = pqueue_test::makeQueueConfig(fileSystem);
    config.fullQueuePolicy = pqueue::FullQueuePolicy::DropOldest;
    pqueue::Queue queue(config);

    REQUIRE(queue.enqueue("one").ok());
    REQUIRE(queue.enqueue("two").ok());
    REQUIRE(queue.enqueue("three").ok());

    CHECK(queue.enqueue("four").ok());
    CHECK_EQ(queue.stats().count, 3U);

    std::string front;
    REQUIRE(queue.peek(front).ok());
    CHECK_EQ(front, "two");
}

TEST_CASE("queue DropOldest emits warning event on eviction") {
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    pqueue::Config config = pqueue_test::makeQueueConfig(fileSystem);
    config.fullQueuePolicy = pqueue::FullQueuePolicy::DropOldest;

    std::vector<pqueue_test::CapturedEvent> events;
    config.events.sink = pqueue_test::captureEvent;
    config.events.user = &events;

    pqueue::Queue queue(config);

    REQUIRE(queue.enqueue("one").ok());
    REQUIRE(queue.enqueue("two").ok());
    REQUIRE(queue.enqueue("three").ok());
    events.clear();

    REQUIRE(queue.enqueue("four").ok());

    const bool hasEvictionWarning = std::any_of(events.begin(), events.end(), [](const pqueue_test::CapturedEvent& e) {
        return e.severity == pqueue::Severity::Warning && e.code == pqueue::StatusCode::QueueFull;
    });
    CHECK(hasEvictionWarning);
}

TEST_CASE("queue DropOldest preserves FIFO order after evictions") {
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    pqueue::Config config = pqueue_test::makeQueueConfig(fileSystem);
    config.fullQueuePolicy = pqueue::FullQueuePolicy::DropOldest;
    pqueue::Queue queue(config);

    REQUIRE(queue.enqueue("a").ok());
    REQUIRE(queue.enqueue("b").ok());
    REQUIRE(queue.enqueue("c").ok());
    REQUIRE(queue.enqueue("d").ok());
    REQUIRE(queue.enqueue("e").ok());

    // After 5 enqueues into capacity-3 queue: a and b evicted, holds c/d/e
    CHECK_EQ(queue.stats().count, 3U);

    std::string out;
    REQUIRE(queue.peek(out).ok()); CHECK_EQ(out, "c"); queue.pop();
    REQUIRE(queue.peek(out).ok()); CHECK_EQ(out, "d"); queue.pop();
    REQUIRE(queue.peek(out).ok()); CHECK_EQ(out, "e"); queue.pop();
}

TEST_CASE("queue DropOldest on empty queue enqueues normally") {
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    pqueue::Config config = pqueue_test::makeQueueConfig(fileSystem);
    config.fullQueuePolicy = pqueue::FullQueuePolicy::DropOldest;
    pqueue::Queue queue(config);

    CHECK(queue.enqueue("only").ok());
    CHECK_EQ(queue.stats().count, 1U);
}

#endif // !ARDUINO

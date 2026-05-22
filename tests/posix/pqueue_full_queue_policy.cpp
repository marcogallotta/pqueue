#include "pqueue/queue.h"

#include "doctest/doctest.h"

#ifndef ARDUINO

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace {

const std::filesystem::path kSpoolDir = "build/pqueue-spools/pqueue_full_queue_policy_spool";

void cleanSpool() {
    std::error_code ec;
    std::filesystem::remove_all(kSpoolDir, ec);
}

// maxSegmentBytes=50: each record rotates into a sealed segment before the next one.
// reservedBytes=179: capacity for exactly 3 records of the payloads used below.
// DropOldest relies on compactOneSegment reclaiming the evicted record's dead bytes
// from sealed segments; single-segment queues cannot compact and would stall.
pqueue::Config makeConfig(pqueue::FullQueuePolicy policy = pqueue::FullQueuePolicy::RejectNewest) {
    pqueue::Config cfg;
    cfg.basePath = kSpoolDir.string();
    cfg.maxSegmentBytes = 50;
    cfg.reservedBytes = 179;
    cfg.minFreeBytes = 0;
    cfg.fullQueuePolicy = policy;
    return cfg;
}

struct CapturedEvent {
    pqueue::EventKind kind;
    pqueue::Severity severity;
    pqueue::StatusCode code;
};

void captureEvent(const pqueue::Event& event, void* user) {
    auto* events = static_cast<std::vector<CapturedEvent>*>(user);
    events->push_back({event.kind, event.severity, event.status.code});
}

} // namespace

TEST_CASE("queue RejectNewest returns QueueFull when full (default)") {
    cleanSpool();
    pqueue::Queue queue(makeConfig());

    REQUIRE(queue.enqueue("one").ok());
    REQUIRE(queue.enqueue("two").ok());
    REQUIRE(queue.enqueue("three").ok());

    const pqueue::Status result = queue.enqueue("four");
    CHECK(result.code == pqueue::StatusCode::QueueFull);
    CHECK_EQ(queue.stats().count, 3U);
}

TEST_CASE("queue DropOldest evicts front record when full") {
    cleanSpool();
    pqueue::Queue queue(makeConfig(pqueue::FullQueuePolicy::DropOldest));

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
    cleanSpool();
    pqueue::Config config = makeConfig(pqueue::FullQueuePolicy::DropOldest);

    std::vector<CapturedEvent> events;
    config.events.sink = captureEvent;
    config.events.user = &events;

    pqueue::Queue queue(config);

    REQUIRE(queue.enqueue("one").ok());
    REQUIRE(queue.enqueue("two").ok());
    REQUIRE(queue.enqueue("three").ok());
    events.clear();

    REQUIRE(queue.enqueue("four").ok());

    const bool hasEvictionWarning = std::any_of(events.begin(), events.end(), [](const CapturedEvent& e) {
        return e.severity == pqueue::Severity::Warning && e.code == pqueue::StatusCode::QueueFull;
    });
    CHECK(hasEvictionWarning);
}

TEST_CASE("queue DropOldest preserves FIFO order after evictions") {
    cleanSpool();
    pqueue::Queue queue(makeConfig(pqueue::FullQueuePolicy::DropOldest));

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
    cleanSpool();
    pqueue::Queue queue(makeConfig(pqueue::FullQueuePolicy::DropOldest));

    CHECK(queue.enqueue("only").ok());
    CHECK_EQ(queue.stats().count, 1U);
}

#endif // !ARDUINO

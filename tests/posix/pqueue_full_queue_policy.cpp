#include "pqueue/queue.h"
#include "pqueue/append_log_common.h"

#include "doctest/doctest.h"

#ifndef ARDUINO

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using namespace pqueue::append_log_detail;

const std::filesystem::path kSpoolDir = "build/pqueue-spools/pqueue_full_queue_policy_spool";

void cleanSpool() {
    std::error_code ec;
    std::filesystem::remove_all(kSpoolDir, ec);
}

// Budget for behaviour tests: fits 3 records of up to 5-byte payloads, each in its own
// segment (maxSegmentBytes=50 forces rotation after every record), plus both manifest slots
// with slack. Not a tight boundary — intent is behavioural, not byte-level.
constexpr std::uint32_t kThreeRecordBudget =
    3 * (kSegmentHeaderBytes + kEnqueueOverheadBytes + 5)
    + 2 * kManifestFixedBytes
    + 32; // slack

std::filesystem::path segmentPath(std::uint32_t gen) {
    char name[32];
    std::snprintf(name, sizeof(name), "seg-%08x.bin", gen);
    return kSpoolDir / name;
}

// maxSegmentBytes=50: each record rotates into a sealed segment before the next one.
// DropOldest relies on compactOneSegment reclaiming the evicted record's dead bytes
// from sealed segments; single-segment queues cannot compact and would stall.
pqueue::Config makeConfig(pqueue::FullQueuePolicy policy = pqueue::FullQueuePolicy::RejectNewest) {
    pqueue::Config cfg;
    cfg.basePath = kSpoolDir.string();
    cfg.maxSegmentBytes = 50;
    cfg.reservedBytes = kThreeRecordBudget;
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

TEST_CASE("queue DropOldest preserves FIFO order after eviction") {
    cleanSpool();
    pqueue::Config cfg = makeConfig(pqueue::FullQueuePolicy::DropOldest);
    cfg.reservedBytes = kThreeRecordBudget;
    pqueue::Queue queue(cfg);

    REQUIRE(queue.enqueue("one").ok());
    REQUIRE(queue.enqueue("two").ok());
    REQUIRE(queue.enqueue("three").ok());
    REQUIRE(queue.enqueue("four").ok());

    // After one eviction, the remaining records must still be FIFO ordered.
    CHECK_EQ(queue.stats().count, 3U);

    std::string out;
    REQUIRE(queue.peek(out).ok()); CHECK_EQ(out, "two"); queue.pop();
    REQUIRE(queue.peek(out).ok()); CHECK_EQ(out, "three"); queue.pop();
    REQUIRE(queue.peek(out).ok()); CHECK_EQ(out, "four"); queue.pop();
}

TEST_CASE("queue DropOldest evicts repeatedly until new record fits") {
    cleanSpool();
    pqueue::Config cfg = makeConfig(pqueue::FullQueuePolicy::DropOldest);
    cfg.maxSegmentBytes = 130;
    cfg.reservedBytes   = 250; // tight enough that one eviction is insufficient for the 80-byte record
    pqueue::Queue queue(cfg);

    REQUIRE(queue.enqueue("one").ok());
    REQUIRE(queue.enqueue("two").ok());
    REQUIRE(queue.enqueue("three").ok());
    REQUIRE(queue.enqueue(std::string(80, 'x')).ok());

    CHECK_EQ(queue.stats().count, 2U);

    std::string out;
    REQUIRE(queue.peek(out).ok()); CHECK_EQ(out, "three"); queue.pop();
    REQUIRE(queue.peek(out).ok()); CHECK_EQ(out, std::string(80, 'x')); queue.pop();
}


TEST_CASE("queue DropOldest reclaims already-dead front segment before evicting") {
    cleanSpool();
    pqueue::Config cfg = makeConfig(pqueue::FullQueuePolicy::DropOldest);
    cfg.maxSegmentBytes = 70;
    cfg.reservedBytes   = 260;
    pqueue::Queue queue(cfg);

    REQUIRE(queue.enqueue("a").ok());
    REQUIRE(queue.enqueue("bb").ok());
    REQUIRE(queue.enqueue("ccc").ok());
    REQUIRE(queue.pop().ok()); // makes gen=1 dead while gen=2 remains live

    REQUIRE(std::filesystem::exists(segmentPath(1)));
    REQUIRE(std::filesystem::exists(segmentPath(2)));

    REQUIRE(queue.enqueue("d").ok());

    // The enqueue should reclaim the already-dead front segment before entering
    // DropOldest eviction. If it evicted instead, count would stay at 2 and the
    // front record would be "ccc".
    CHECK_EQ(queue.stats().count, 3U);
    CHECK_FALSE(std::filesystem::exists(segmentPath(1)));
    CHECK(std::filesystem::exists(segmentPath(2))); // live gen=2 was not rewritten away

    std::string out;
    REQUIRE(queue.peek(out).ok()); CHECK_EQ(out, "bb"); queue.pop();
    REQUIRE(queue.peek(out).ok()); CHECK_EQ(out, "ccc"); queue.pop();
    REQUIRE(queue.peek(out).ok()); CHECK_EQ(out, "d"); queue.pop();
}

TEST_CASE("queue DropOldest on empty queue enqueues normally") {
    cleanSpool();
    pqueue::Queue queue(makeConfig(pqueue::FullQueuePolicy::DropOldest));

    CHECK(queue.enqueue("only").ok());
    CHECK_EQ(queue.stats().count, 1U);
}

#endif // !ARDUINO

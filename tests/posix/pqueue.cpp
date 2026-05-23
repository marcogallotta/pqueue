#include "pqueue/queue.h"

#include "doctest/doctest.h"

#ifndef ARDUINO
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <vector>
#include <unistd.h>
#endif

namespace {

#ifndef ARDUINO
const std::filesystem::path kSpoolDir = "build/pqueue-spools/pqueue_test_spool";

void cleanSpool() {
    std::error_code ec;
    std::filesystem::remove_all(kSpoolDir, ec);
}

void capturePqueueEvent(const pqueue::Event& event, void* user) {
    auto* events = static_cast<std::vector<pqueue::Event>*>(user);
    events->push_back(event);
}

void writeActiveLockFile() {
    std::filesystem::create_directories(kSpoolDir);
    std::ofstream lock(kSpoolDir / ".pqueue.lock", std::ios::binary | std::ios::trunc);
    lock << "pqueue-lock-v1\n";
    lock << "pid=" << static_cast<long>(::getpid()) << "\n";
    lock << "token=active-test-lock\n";
}

pqueue::Config makeConfig() {
    pqueue::Config cfg;
    cfg.basePath = kSpoolDir.string();
    cfg.maxSegmentBytes = 1024;
    cfg.minFreeBytes = 0;
    return cfg;
}
#endif

} // namespace

TEST_CASE("pqueue starts empty") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Queue queue(makeConfig());

    std::string out;
    CHECK_FALSE(queue.peek(out).ok());
    CHECK_EQ(queue.stats().count, 0U);
#endif
}

TEST_CASE("pqueue preserves FIFO order") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Queue queue(makeConfig());

    REQUIRE(queue.enqueue("first").ok());
    REQUIRE(queue.enqueue("second").ok());

    std::string out;
    REQUIRE(queue.peek(out).ok());
    CHECK_EQ(out, "first");
    REQUIRE(queue.pop().ok());
    REQUIRE(queue.peek(out).ok());
    CHECK_EQ(out, "second");
    REQUIRE(queue.pop().ok());
    CHECK_FALSE(queue.peek(out).ok());
#endif
}


TEST_CASE("pqueue supports multiple live Queue objects on the same base path") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Config config = makeConfig();

    // AppendLog instances don't share in-RAM state; each has its own records_.
    // Both can enqueue to the same base path (lock serialises them), and a fresh
    // Queue sees all committed records in FIFO order.
    pqueue::Queue first(config);
    pqueue::Queue second(config);

    REQUIRE(first.enqueue("first").ok());
    REQUIRE(second.enqueue("second").ok());

    pqueue::Queue third(config);
    CHECK_EQ(third.stats().count, 2U);

    std::string out;
    REQUIRE(third.peek(out).ok());
    CHECK_EQ(out, "first");
    REQUIRE(third.pop().ok());
    REQUIRE(third.peek(out).ok());
    CHECK_EQ(out, "second");
    REQUIRE(third.pop().ok());

    CHECK_EQ(third.stats().count, 0U);
#endif
}

TEST_CASE("pqueue survives reopening from disk") {
#ifndef ARDUINO
    cleanSpool();
    {
        pqueue::Queue queue(makeConfig());
        REQUIRE(queue.enqueue("one").ok());
        REQUIRE(queue.enqueue("two").ok());
    }

    pqueue::Queue reopened(makeConfig());

    std::string out;
    REQUIRE(reopened.peek(out).ok());
    CHECK_EQ(out, "one");
    CHECK_EQ(reopened.stats().count, 2U);
#endif
}

TEST_CASE("pqueue accepts records exactly at the configured max size") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Config config = makeConfig();
    config.recordSizeBytes = 4;
    pqueue::Queue queue(config);

    REQUIRE(queue.enqueue("1234").ok());
    CHECK_EQ(queue.stats().count, 1U);

    std::string out;
    REQUIRE(queue.peek(out).ok());
    CHECK_EQ(out, "1234");
#endif
}

TEST_CASE("pqueue rejects records over the configured max size") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Config config = makeConfig();
    config.recordSizeBytes = 4;
    pqueue::Queue queue(config);

    CHECK_FALSE(queue.enqueue("12345").ok());
    CHECK_EQ(queue.stats().count, 0U);
#endif
}

TEST_CASE("pqueue rejects newest record when budget is full") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Config config = makeConfig();
    config.maxSegmentBytes = 4096;
    config.reservedBytes = 256;

    std::vector<std::string> accepted;
    {
        pqueue::Queue queue(config);

        bool sawFull = false;
        for (int i = 0; i < 100; ++i) {
            const std::string payload = "r" + std::to_string(i);
            const auto status = queue.enqueue(payload);
            if (!status.ok()) {
                REQUIRE(status.code == pqueue::StatusCode::QueueFull);
                sawFull = true;
                break;
            }
            accepted.push_back(payload);
        }

        REQUIRE(sawFull);
        CHECK_GE(accepted.size(), 1U);
        CHECK_EQ(queue.stats().count, accepted.size());
    }

    pqueue::Queue reopened(config);
    CHECK_EQ(reopened.stats().count, accepted.size());

    for (const auto& expected : accepted) {
        std::string out;
        REQUIRE(reopened.peek(out).ok());
        CHECK_EQ(out, expected);
        REQUIRE(reopened.pop().ok());
    }
    std::string out;
    CHECK(reopened.peek(out).code == pqueue::StatusCode::QueueEmpty);
#endif
}

TEST_CASE("pqueue matches std::deque over deterministic random operations") {
#ifndef ARDUINO
    cleanSpool();

    constexpr std::size_t kRecordSizeBytes = 16;
    constexpr int kOperationCount = 1000;
    constexpr std::uint32_t kSeed = 0x70515545U; // "pQUE"

    pqueue::Config config = makeConfig();
    config.recordSizeBytes = kRecordSizeBytes;
    config.maxSegmentBytes = 256;
    config.reservedBytes = 0; // unlimited: QueueFull coverage is in full_queue_policy tests

    std::deque<std::string> model;
    auto queue = std::make_unique<pqueue::Queue>(config);
    std::mt19937 rng(kSeed);

    auto recordFor = [](int value) {
        return std::string("r") + std::to_string(value);
    };

    auto assertMatchesModel = [&]() {
        CHECK_EQ(queue->stats().count, model.size());

        std::string out;
        const pqueue::Status peek = queue->peek(out);
        if (model.empty()) {
            CHECK_FALSE(peek.ok());
            CHECK(peek.code == pqueue::StatusCode::QueueEmpty);
        } else {
            REQUIRE(peek.ok());
            CHECK_EQ(out, model.front());
        }
    };

    assertMatchesModel();

    for (int step = 0; step < kOperationCount; ++step) {
        const int op = static_cast<int>(rng() % 4);
        switch (op) {
        case 0: { // enqueue
            const std::string record = recordFor(step);
            REQUIRE(queue->enqueue(record).ok());
            model.push_back(record);
            break;
        }
        case 1: { // pop
            const pqueue::Status status = queue->pop();
            if (model.empty()) {
                CHECK_FALSE(status.ok());
                CHECK(status.code == pqueue::StatusCode::QueueEmpty);
            } else {
                REQUIRE(status.ok());
                model.pop_front();
            }
            break;
        }
        case 2: { // explicit peek
            std::string out;
            const pqueue::Status status = queue->peek(out);
            if (model.empty()) {
                CHECK_FALSE(status.ok());
                CHECK(status.code == pqueue::StatusCode::QueueEmpty);
            } else {
                REQUIRE(status.ok());
                CHECK_EQ(out, model.front());
            }
            break;
        }
        case 3: // remount/recreate queue
            queue.reset();
            queue = std::make_unique<pqueue::Queue>(config);
            break;
        }

        assertMatchesModel();
    }

    queue.reset();
    queue = std::make_unique<pqueue::Queue>(config);
    assertMatchesModel();
#endif
}

TEST_CASE("pqueue active lock file prevents queue operation") {
#ifndef ARDUINO
    cleanSpool();

    pqueue::Config config = makeConfig();

    {
        pqueue::Queue queue(config);
        REQUIRE(queue.enqueue("before").ok());
    }

    writeActiveLockFile();

    pqueue::Queue blocked(config);
    const auto status = blocked.enqueue("blocked");
    CHECK_FALSE(status.ok());
    CHECK(status.code == pqueue::StatusCode::LockTimeout);
#endif
}

TEST_CASE("pqueue lock timeout emits a clear diagnostic event") {
#ifndef ARDUINO
    cleanSpool();

    std::vector<pqueue::Event> events;
    pqueue::Config config = makeConfig();
    config.events = {capturePqueueEvent, &events};

    {
        pqueue::Queue queue(config);
        REQUIRE(queue.enqueue("before").ok());
    }

    writeActiveLockFile();

    pqueue::Queue blocked(config);
    const auto status = blocked.enqueue("blocked");
    REQUIRE_FALSE(status.ok());
    CHECK(status.code == pqueue::StatusCode::LockTimeout);

    bool sawLockTimeout = false;
    for (const auto& event : events) {
        if (event.status.code == pqueue::StatusCode::LockTimeout &&
            std::string(event.component) == "Queue" &&
            std::string(event.operation) == "acquireLock") {
            sawLockTimeout = true;
            CHECK(event.kind == pqueue::EventKind::Diagnostic);
            CHECK(event.severity == pqueue::Severity::Error);
        }
    }
    CHECK(sawLockTimeout);
#endif
}


TEST_CASE("pqueue recovers stale POSIX pid lock") {
#ifndef ARDUINO
    cleanSpool();

    pqueue::Config config = makeConfig();

    {
        pqueue::Queue queue(config);
        REQUIRE(queue.enqueue("before").ok());
    }

    std::filesystem::create_directories(kSpoolDir);
    std::ofstream lock(kSpoolDir / ".pqueue.lock", std::ios::binary | std::ios::trunc);
    lock << "pqueue-lock-v1\n";
    lock << "pid=99999999\n";
    lock << "token=stale-test\n";
    lock.close();

    pqueue::Queue reopened(config);
    REQUIRE(reopened.enqueue("after").ok());
    CHECK_EQ(reopened.stats().count, 2U);
#endif
}

TEST_CASE("pqueue releases lock after each operation") {
#ifndef ARDUINO
    cleanSpool();

    pqueue::Config config = makeConfig();

    {
        pqueue::Queue first(config);
        REQUIRE(first.enqueue("first").ok());
    }

    pqueue::Queue second(config);
    REQUIRE(second.enqueue("second").ok());
    CHECK_EQ(second.stats().count, 2U);
#endif
}

TEST_CASE("pqueue validate reports clean active records") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Config config = makeConfig();
    pqueue::Queue queue(config);

    REQUIRE(queue.enqueue("first").ok());
    REQUIRE(queue.enqueue("second").ok());

    const auto result = queue.validate();
    CHECK(result.ok);
    CHECK_EQ(result.checkedRecords, 2U);
    CHECK(result.errors.empty());
#endif
}

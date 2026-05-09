#include "pqueue/queue.h"
#include "pqueue/storage_common.h"

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

std::uint64_t testRecordRegionOffset(const pqueue::Config& config) {
    return static_cast<std::uint64_t>(pqueue::storage_detail::kCheckpointSlots) *
           pqueue::storage_detail::kCheckpointRecordBytes +
           config.journalBytes;
}

std::uint64_t testCheckpointOffset(std::uint32_t slot) {
    return static_cast<std::uint64_t>(slot) * pqueue::storage_detail::kCheckpointRecordBytes;
}

std::uint64_t testJournalOffset(std::uint32_t entryIndex) {
    return static_cast<std::uint64_t>(pqueue::storage_detail::kCheckpointSlots) *
           pqueue::storage_detail::kCheckpointRecordBytes +
           static_cast<std::uint64_t>(entryIndex) * pqueue::storage_detail::kJournalEntryBytes;
}

std::uint64_t testSlotOffset(const pqueue::Config& config, std::uint32_t sequence) {
    const auto slotSize = pqueue::storage_detail::kRecordHeaderBytes + config.recordSizeBytes;
    const auto capacity = config.reservedBytes / slotSize;
    return testRecordRegionOffset(config) + static_cast<std::uint64_t>(sequence % capacity) * slotSize;
}
#endif

} // namespace

TEST_CASE("pqueue starts empty") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Config config;
    config.basePath = kSpoolDir.string();
    pqueue::Queue queue(config);

    std::string out;
    CHECK_FALSE(queue.peek(out).ok());
    CHECK_EQ(queue.stats().count, 0U);
#endif
}

TEST_CASE("pqueue preserves FIFO order") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Config config;
    config.basePath = kSpoolDir.string();
    pqueue::Queue queue(config);

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
    pqueue::Config config;
    config.basePath = kSpoolDir.string();

    pqueue::Queue first(config);
    pqueue::Queue second(config);

    REQUIRE(first.enqueue("first").ok());
    REQUIRE(second.enqueue("second").ok());

    CHECK_EQ(first.stats().count, 2U);
    CHECK_EQ(second.stats().count, 2U);

    std::string out;
    REQUIRE(second.peek(out).ok());
    CHECK_EQ(out, "first");
    REQUIRE(second.pop().ok());

    REQUIRE(first.peek(out).ok());
    CHECK_EQ(out, "second");
    REQUIRE(first.pop().ok());

    CHECK_EQ(first.stats().count, 0U);
    CHECK_EQ(second.stats().count, 0U);
#endif
}

TEST_CASE("pqueue survives reopening from disk") {
#ifndef ARDUINO
    cleanSpool();
    {
        pqueue::Config config;
        config.basePath = kSpoolDir.string();
        pqueue::Queue queue(config);
        REQUIRE(queue.enqueue("one").ok());
        REQUIRE(queue.enqueue("two").ok());
    }

    pqueue::Config reopenedConfig;
    reopenedConfig.basePath = kSpoolDir.string();
    pqueue::Queue reopened(reopenedConfig);

    std::string out;
    REQUIRE(reopened.peek(out).ok());
    CHECK_EQ(out, "one");
    CHECK_EQ(reopened.stats().count, 2U);
#endif
}

TEST_CASE("pqueue rewriteFront updates the front record without popping it") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Config config;
    config.basePath = kSpoolDir.string();
    pqueue::Queue queue(config);

    REQUIRE(queue.enqueue("retry=0").ok());
    REQUIRE(queue.rewriteFront("retry=1").ok());

    std::string out;
    REQUIRE(queue.peek(out).ok());
    CHECK_EQ(out, "retry=1");
#endif
}

TEST_CASE("pqueue accepts records exactly at the configured max size") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Config config;
    config.basePath = kSpoolDir.string();
    config.recordSizeBytes = 4;
    config.reservedBytes = 128;
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
    pqueue::Config config;
    config.basePath = kSpoolDir.string();
    config.recordSizeBytes = 4;
    config.reservedBytes = 128;
    pqueue::Queue queue(config);

    CHECK_FALSE(queue.enqueue("12345").ok());
    CHECK_EQ(queue.stats().count, 0U);
#endif
}


TEST_CASE("pqueue rejects newest record when the fixed ring is full") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Config config;
    config.basePath = kSpoolDir.string();
    config.recordSizeBytes = 8;
    config.reservedBytes = static_cast<std::uint32_t>((pqueue::storage_detail::kRecordHeaderBytes + config.recordSizeBytes) * 2);
    pqueue::Queue queue(config);

    REQUIRE(queue.enqueue("one").ok());
    REQUIRE(queue.enqueue("two").ok());
    const auto full = queue.enqueue("three");

    CHECK_FALSE(full.ok());
    CHECK(full.code == pqueue::StatusCode::QueueFull);
    CHECK_EQ(queue.stats().count, 2U);

    std::string out;
    REQUIRE(queue.peek(out).ok());
    CHECK_EQ(out, "one");
#endif
}



TEST_CASE("pqueue matches std::deque over deterministic random operations") {
#ifndef ARDUINO
    cleanSpool();

    constexpr std::size_t kRecordSizeBytes = 16;
    constexpr std::uint32_t kCapacityRecords = 7;
    constexpr int kOperationCount = 1000;
    constexpr std::uint32_t kSeed = 0x70515545U; // "pQUE"

    pqueue::Config config;
    config.basePath = kSpoolDir.string();
    config.recordSizeBytes = kRecordSizeBytes;
    config.reservedBytes = static_cast<std::uint32_t>((sizeof(pqueue::storage_detail::RecordHeader) + config.recordSizeBytes) * kCapacityRecords);

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
        const int op = static_cast<int>(rng() % 5);
        switch (op) {
        case 0: { // enqueue
            const std::string record = recordFor(step);
            const pqueue::Status status = queue->enqueue(record);
            if (model.size() >= kCapacityRecords) {
                CHECK_FALSE(status.ok());
                CHECK(status.code == pqueue::StatusCode::QueueFull);
            } else {
                REQUIRE(status.ok());
                model.push_back(record);
            }
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
        case 2: { // rewriteFront
            const std::string record = std::string("w") + std::to_string(step);
            const pqueue::Status status = queue->rewriteFront(record);
            if (model.empty()) {
                CHECK_FALSE(status.ok());
                CHECK(status.code == pqueue::StatusCode::QueueEmpty);
            } else {
                REQUIRE(status.ok());
                model.front() = record;
            }
            break;
        }
        case 3: { // explicit peek
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
        case 4: // remount/recreate queue
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

    pqueue::Config config;
    config.basePath = kSpoolDir.string();
    config.recordSizeBytes = 32;
    config.reservedBytes = 160;

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
    pqueue::Config config;
    config.basePath = kSpoolDir.string();
    config.recordSizeBytes = 32;
    config.reservedBytes = 160;
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

    pqueue::Config config;
    config.basePath = kSpoolDir.string();
    config.recordSizeBytes = 32;
    config.reservedBytes = 160;

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

    pqueue::Config config;
    config.basePath = kSpoolDir.string();
    config.recordSizeBytes = 32;
    config.reservedBytes = 160;

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
    pqueue::Config config;
    config.basePath = kSpoolDir.string();
    config.recordSizeBytes = 32;
    config.reservedBytes = 512;
    pqueue::Queue queue(config);

    REQUIRE(queue.enqueue("first").ok());
    REQUIRE(queue.enqueue("second").ok());

    const auto result = queue.validate();
    CHECK(result.ok);
    CHECK_EQ(result.checkedRecords, 2U);
    CHECK(result.errors.empty());
#endif
}

TEST_CASE("pqueue validate detects corrupt active slot payload") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Config config;
    config.basePath = kSpoolDir.string();
    config.recordSizeBytes = 32;
    config.reservedBytes = 512;
    {
        pqueue::Queue queue(config);
        REQUIRE(queue.enqueue("payload").ok());
    }

    std::fstream spool(kSpoolDir / "pqueue.spool", std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(spool.good());
    spool.seekp(static_cast<std::streamoff>(testSlotOffset(config, 0) + pqueue::storage_detail::kRecordHeaderBytes));
    char c = 'X';
    spool.write(&c, 1);
    spool.close();

    pqueue::Queue queue(config);
    const auto result = queue.validate();
    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(result.errors.empty());
    CHECK(result.errors[0].code == pqueue::ValidationIssueCode::SlotCrcMismatch);
    CHECK_EQ(result.checkedRecords, 0U);
#endif
}

TEST_CASE("pqueue validate ignores inactive corrupt slots") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Config config;
    config.basePath = kSpoolDir.string();
    config.recordSizeBytes = 32;
    config.reservedBytes = 512;
    {
        pqueue::Queue queue(config);
        REQUIRE(queue.enqueue("old").ok());
        REQUIRE(queue.enqueue("active").ok());
        REQUIRE(queue.pop().ok());
    }

    std::fstream spool(kSpoolDir / "pqueue.spool", std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(spool.good());
    char c = 'X';
    spool.seekp(static_cast<std::streamoff>(testSlotOffset(config, 0)));
    spool.write(&c, 1);
    spool.close();

    pqueue::Queue queue(config);
    const auto result = queue.validate();
    CHECK(result.ok);
    CHECK_EQ(result.checkedRecords, 1U);
    CHECK(result.errors.empty());
#endif
}

TEST_CASE("pqueue validate caps reported errors") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Config config;
    config.basePath = kSpoolDir.string();
    config.recordSizeBytes = 32;
    config.reservedBytes = 512;
    {
        pqueue::Queue queue(config);
        REQUIRE(queue.enqueue("first").ok());
        REQUIRE(queue.enqueue("second").ok());
    }

    std::fstream spool(kSpoolDir / "pqueue.spool", std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(spool.good());
    char c = 'X';
    spool.seekp(static_cast<std::streamoff>(testSlotOffset(config, 0)));
    spool.write(&c, 1);
    spool.seekp(static_cast<std::streamoff>(testSlotOffset(config, 1)));
    spool.write(&c, 1);
    spool.close();

    pqueue::ValidationOptions options;
    options.maxErrors = 1;
    pqueue::Queue queue(config);
    const auto result = queue.validate(options);
    REQUIRE_FALSE(result.ok);
    CHECK(result.stoppedEarly);
    CHECK_EQ(result.errors.size(), 1U);
#endif
}

TEST_CASE("pqueue validate fails when active lock file exists") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Config config;
    config.basePath = kSpoolDir.string();
    config.recordSizeBytes = 32;
    config.reservedBytes = 512;

    {
        pqueue::Queue queue(config);
        REQUIRE(queue.enqueue("held").ok());
    }

    writeActiveLockFile();

    pqueue::Queue second(config);
    const auto result = second.validate();
    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(result.errors.empty());
    CHECK(result.errors[0].code == pqueue::ValidationIssueCode::QueueLoadFailed);
    CHECK(result.errors[0].message.find("queue lock timeout") != std::string::npos);
#endif
}


TEST_CASE("pqueue validate reports corrupt checkpoint slots even when fallback checkpoint exists") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Config config;
    config.basePath = kSpoolDir.string();
    config.recordSizeBytes = 32;
    config.reservedBytes = 512;
    config.checkpointEveryOps = 1;
    {
        pqueue::Queue queue(config);
        REQUIRE(queue.enqueue("one").ok());
        REQUIRE(queue.enqueue("two").ok());
    }

    std::fstream spool(kSpoolDir / "pqueue.spool", std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(spool.good());
    char c = 'X';
    spool.seekp(static_cast<std::streamoff>(testCheckpointOffset(3)));
    spool.write(&c, 1);
    spool.close();

    pqueue::Queue queue(config);
    const auto result = queue.validate();
    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(result.errors.empty());
    CHECK(result.errors[0].code == pqueue::ValidationIssueCode::MetadataCorrupt);
#endif
}

TEST_CASE("pqueue validate reports corrupt journal entry before later journal data") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Config config;
    config.basePath = kSpoolDir.string();
    config.recordSizeBytes = 32;
    config.reservedBytes = 512;
    config.checkpointEveryOps = 64;
    {
        pqueue::Queue queue(config);
        REQUIRE(queue.enqueue("one").ok());
        REQUIRE(queue.enqueue("two").ok());
        REQUIRE(queue.enqueue("three").ok());
    }

    std::fstream spool(kSpoolDir / "pqueue.spool", std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(spool.good());
    char c = 'X';
    spool.seekp(static_cast<std::streamoff>(testJournalOffset(1)));
    spool.write(&c, 1);
    spool.close();

    pqueue::Queue queue(config);
    const auto result = queue.validate();
    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(result.errors.empty());
    CHECK(result.errors[0].code == pqueue::ValidationIssueCode::JournalCorrupt);
#endif
}

TEST_CASE("pqueue recovers enqueued records from journal before checkpoint") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Config config;
    config.basePath = kSpoolDir.string();
    config.recordSizeBytes = 32;
    config.reservedBytes = 512;
    config.checkpointEveryOps = 64;
    {
        pqueue::Queue queue(config);
        REQUIRE(queue.enqueue("one").ok());
        REQUIRE(queue.enqueue("two").ok());
        REQUIRE(queue.enqueue("three").ok());
    }

    pqueue::Queue reopened(config);
    CHECK_EQ(reopened.stats().count, 3U);

    std::string out;
    REQUIRE(reopened.peek(out).ok());
    CHECK_EQ(out, "one");
    REQUIRE(reopened.pop().ok());
    REQUIRE(reopened.peek(out).ok());
    CHECK_EQ(out, "two");
#endif
}

TEST_CASE("pqueue recovers popped records from journal before checkpoint") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Config config;
    config.basePath = kSpoolDir.string();
    config.recordSizeBytes = 32;
    config.reservedBytes = 512;
    config.checkpointEveryOps = 64;
    {
        pqueue::Queue queue(config);
        REQUIRE(queue.enqueue("one").ok());
        REQUIRE(queue.enqueue("two").ok());
        REQUIRE(queue.enqueue("three").ok());
        REQUIRE(queue.pop().ok());
        REQUIRE(queue.pop().ok());
    }

    pqueue::Queue reopened(config);
    CHECK_EQ(reopened.stats().count, 1U);

    std::string out;
    REQUIRE(reopened.peek(out).ok());
    CHECK_EQ(out, "three");
#endif
}

TEST_CASE("pqueue ignores torn final journal entry and keeps valid prefix") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Config config;
    config.basePath = kSpoolDir.string();
    config.recordSizeBytes = 32;
    config.reservedBytes = 512;
    config.checkpointEveryOps = 64;
    {
        pqueue::Queue queue(config);
        REQUIRE(queue.enqueue("one").ok());
        REQUIRE(queue.enqueue("two").ok());
    }

    std::fstream spool(kSpoolDir / "pqueue.spool", std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(spool.good());
    char c = 'X';
    spool.seekp(static_cast<std::streamoff>(testJournalOffset(1)));
    spool.write(&c, 1);
    spool.close();

    pqueue::Queue reopened(config);
    CHECK_EQ(reopened.stats().count, 1U);

    std::string out;
    REQUIRE(reopened.peek(out).ok());
    CHECK_EQ(out, "one");

    const auto validation = reopened.validate();
    CHECK(validation.ok);
    CHECK(validation.errors.empty());
#endif
}

#include <Arduino.h>
#include <LittleFS.h>
#include <unity.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "pqueue/outbox.h"
#include "pqueue/queue.h"
#include "pqueue/status.h"
#include "pqueue/storage_common.h"
#include "pqueue/types.h"

namespace {

constexpr const char* kBasePath = "/pqueue_slow";
constexpr const char* kStatePath = "/pqueue_slow_state";
constexpr const char* kSpoolPath = "/pqueue_slow/pqueue.spool";
constexpr const char* kTmpOrphanPath = "/pqueue_slow/pqueue.spool.tmp";

enum class SlowTest : std::uint8_t {
    FifoMany = 1,
    PopRemaining = 2,
    RewriteFront = 3,
    Wraparound = 4,
    IndexFallback = 5,
    MissingMetadataFailsSafely = 6,
    CorruptMetadataFailsSafely = 7,
    TmpOrphanIgnored = 8,
    QueueFullRebootSafe = 9,
    RecordSizeMismatchFailsSafely = 10,
    CapacityMismatchFailsSafely = 11,
    OutboxRebootDrain = 12,
    Churn = 13,
    Done = 255,
};

struct SlowState {
    SlowTest test = SlowTest::FifoMany;
    std::uint8_t phase = 0;
};

std::uint32_t slotSize(std::size_t recordSizeBytes) {
    return static_cast<std::uint32_t>(sizeof(pqueue::storage_detail::RecordHeader) + recordSizeBytes);
}

pqueue::Config queueConfig(
    std::size_t recordSizeBytes = 32,
    std::uint32_t capacityRecords = 40,
    std::uint32_t checkpointEveryOps = 64
) {
    pqueue::Config config;
    config.basePath = kBasePath;
    config.storageBackend = pqueue::StorageBackend::LittleFS;
    config.recordSizeBytes = recordSizeBytes;
    config.reservedBytes = slotSize(recordSizeBytes) * capacityRecords;
    config.checkpointEveryOps = checkpointEveryOps;
    return config;
}

struct FakeSender {
    pqueue::SendDecision decision = pqueue::SendDecision::Sent;
    std::uint16_t calls = 0;
    std::string lastPayload;
    std::uint8_t lastAttempts = 0;
};

std::uint64_t fakeClock(void*) {
    return 0;
}

pqueue::SendResult fakeSend(void* context, const std::string& payload, const pqueue::RetryState& retry) {
    auto* sender = static_cast<FakeSender*>(context);
    sender->calls += 1;
    sender->lastPayload = payload;
    sender->lastAttempts = retry.attempts;
    return {sender->decision};
}

pqueue::OutboxConfig outboxConfig() {
    pqueue::OutboxConfig config;
    config.retryDelayMs = 0;
    config.maxDrainAttemptsPerSecond = 1000;
    return config;
}

// Outbox records include envelope metadata, so use a larger queue record size than
// the raw payload-only queue tests.
pqueue::Config outboxQueueConfig() {
    return queueConfig(128, 8);
}

bool mountLittleFs() {
    LittleFS.end();
    return LittleFS.begin(true);
}

void cleanLittleFs() {
    LittleFS.end();
    TEST_ASSERT_TRUE_MESSAGE(LittleFS.begin(true), "LittleFS mount failed");
    TEST_ASSERT_TRUE_MESSAGE(LittleFS.format(), "LittleFS format failed");
    LittleFS.end();
}

bool readSlowState(SlowState& out) {
    if (!mountLittleFs()) {
        TEST_FAIL_MESSAGE("failed to mount LittleFS while reading slow state");
        return false;
    }

    File file = LittleFS.open(kStatePath, "r");
    if (!file) {
        LittleFS.end();
        return false;
    }

    String testLine = file.readStringUntil('\n');
    String phaseLine = file.readStringUntil('\n');
    file.close();
    LittleFS.end();

    out.test = static_cast<SlowTest>(std::atoi(testLine.c_str()));
    out.phase = static_cast<std::uint8_t>(std::atoi(phaseLine.c_str()));
    return true;
}

void writeSlowState(SlowTest test, std::uint8_t phase) {
    TEST_ASSERT_TRUE_MESSAGE(mountLittleFs(), "failed to mount LittleFS while writing slow state");
    File file = LittleFS.open(kStatePath, "w");
    TEST_ASSERT_TRUE_MESSAGE(file, "failed to open slow state file for write");
    file.printf("%u\n%u\n", static_cast<unsigned>(test), static_cast<unsigned>(phase));
    file.flush();
    file.close();
    LittleFS.end();
}

void clearSlowState() {
    if (!mountLittleFs()) {
        return;
    }
    LittleFS.remove(kStatePath);
    LittleFS.end();
}

void rebootNow() {
    delay(200);
    ESP.restart();
    while (true) {
        delay(1000);
    }
}

void rebootTo(SlowTest test, std::uint8_t phase) {
    writeSlowState(test, phase);
    rebootNow();
}

std::string numbered(const char* prefix, int value) {
    char buffer[24];
    snprintf(buffer, sizeof(buffer), "%s%02d", prefix, value);
    return std::string(buffer);
}

void expectQueueEmpty(pqueue::Queue& queue) {
    std::string out;
    const pqueue::Status status = queue.peek(out);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(pqueue::StatusCode::QueueEmpty), static_cast<int>(status.code));
}

void expectFrontAndPop(pqueue::Queue& queue, const std::string& expected) {
    std::string out;
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());
}

void expectDrain(pqueue::Queue& queue, const std::vector<std::string>& expected) {
    TEST_ASSERT_EQUAL_UINT32(expected.size(), queue.stats().count);
    for (const std::string& record : expected) {
        expectFrontAndPop(queue, record);
    }
    expectQueueEmpty(queue);
}

std::uint32_t checkpointOffset(std::uint32_t slot) {
    return slot * pqueue::storage_detail::kCheckpointRecordBytes;
}

void writeSpoolBytes(std::uint32_t offset, const std::string& bytes) {
    TEST_ASSERT_TRUE_MESSAGE(mountLittleFs(), "failed to mount LittleFS for spool write");
    File file = LittleFS.open(kSpoolPath, "r+");
    TEST_ASSERT_TRUE_MESSAGE(file, "failed to open pqueue spool for write");
    TEST_ASSERT_TRUE(file.seek(offset, SeekSet));
    TEST_ASSERT_EQUAL_UINT(bytes.size(), file.write(
        reinterpret_cast<const std::uint8_t*>(bytes.data()),
        bytes.size()
    ));
    file.flush();
    file.close();
    LittleFS.end();
}

void corruptCheckpointSlot(std::uint32_t slot) {
    const char bad = static_cast<char>(0xff);
    writeSpoolBytes(checkpointOffset(slot), std::string(1, bad));
}

void corruptAllCheckpointSlots() {
    for (std::uint32_t slot = 0; slot < pqueue::storage_detail::kCheckpointSlots; ++slot) {
        corruptCheckpointSlot(slot);
    }
}

void zeroAllCheckpointSlots() {
    const std::string zeroes(
        pqueue::storage_detail::kCheckpointSlots * pqueue::storage_detail::kCheckpointRecordBytes,
        '\0'
    );
    writeSpoolBytes(0, zeroes);
}

void writeOrphanTmpFile() {
    TEST_ASSERT_TRUE_MESSAGE(mountLittleFs(), "failed to mount LittleFS for tmp orphan");
    File file = LittleFS.open(kTmpOrphanPath, "w");
    TEST_ASSERT_TRUE_MESSAGE(file, "failed to create tmp orphan");
    file.print("orphan tmp file that must be ignored");
    file.flush();
    file.close();
    LittleFS.end();
}

std::uint8_t phaseFor(SlowTest expected) {
    SlowState state;
    if (!readSlowState(state)) {
        return 0;
    }
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(expected), static_cast<std::uint8_t>(state.test));
    return state.phase;
}

void completeSlowTest() {
    clearSlowState();
}

void test_reboot_fifo_many() {
    const std::uint8_t phase = phaseFor(SlowTest::FifoMany);
    if (phase == 0) {
        cleanLittleFs();
        {
            pqueue::Queue queue(queueConfig());
            for (int i = 0; i < 20; ++i) {
                TEST_ASSERT_TRUE(queue.enqueue(numbered("a", i)).ok());
            }
            TEST_ASSERT_EQUAL_UINT32(20, queue.stats().count);
        }
        rebootTo(SlowTest::FifoMany, 1);
    }

    TEST_ASSERT_EQUAL_UINT8(1, phase);
    {
        pqueue::Queue queue(queueConfig());
        std::vector<std::string> expected;
        for (int i = 0; i < 20; ++i) {
            expected.push_back(numbered("a", i));
        }
        expectDrain(queue, expected);
    }
    completeSlowTest();
}

void test_reboot_pop_remaining() {
    const std::uint8_t phase = phaseFor(SlowTest::PopRemaining);
    if (phase == 0) {
        cleanLittleFs();
        {
            pqueue::Queue queue(queueConfig());
            for (int i = 0; i < 10; ++i) {
                TEST_ASSERT_TRUE(queue.enqueue(numbered("p", i)).ok());
            }
            for (int i = 0; i < 4; ++i) {
                expectFrontAndPop(queue, numbered("p", i));
            }
            TEST_ASSERT_EQUAL_UINT32(6, queue.stats().count);
        }
        rebootTo(SlowTest::PopRemaining, 1);
    }

    TEST_ASSERT_EQUAL_UINT8(1, phase);
    {
        pqueue::Queue queue(queueConfig());
        std::vector<std::string> expected;
        for (int i = 4; i < 10; ++i) {
            expected.push_back(numbered("p", i));
        }
        expectDrain(queue, expected);
    }
    completeSlowTest();
}

void test_reboot_rewrite_front() {
    const std::uint8_t phase = phaseFor(SlowTest::RewriteFront);
    if (phase == 0) {
        cleanLittleFs();
        {
            pqueue::Queue queue(queueConfig());
            TEST_ASSERT_TRUE(queue.enqueue("old-front").ok());
            TEST_ASSERT_TRUE(queue.enqueue("tail").ok());
            TEST_ASSERT_TRUE(queue.rewriteFront("new-front").ok());
        }
        rebootTo(SlowTest::RewriteFront, 1);
    }

    TEST_ASSERT_EQUAL_UINT8(1, phase);
    {
        pqueue::Queue queue(queueConfig());
        expectFrontAndPop(queue, "new-front");
        expectFrontAndPop(queue, "tail");
        expectQueueEmpty(queue);
    }
    completeSlowTest();
}

void test_reboot_wraparound() {
    const std::uint8_t phase = phaseFor(SlowTest::Wraparound);
    if (phase == 0) {
        cleanLittleFs();
        {
            pqueue::Queue queue(queueConfig(32, 8));
            for (int i = 0; i < 8; ++i) {
                TEST_ASSERT_TRUE(queue.enqueue(numbered("w", i)).ok());
            }
            for (int i = 0; i < 5; ++i) {
                expectFrontAndPop(queue, numbered("w", i));
            }
            for (int i = 8; i < 13; ++i) {
                TEST_ASSERT_TRUE(queue.enqueue(numbered("w", i)).ok());
            }
            TEST_ASSERT_EQUAL_UINT32(8, queue.stats().count);
        }
        rebootTo(SlowTest::Wraparound, 1);
    }

    TEST_ASSERT_EQUAL_UINT8(1, phase);
    {
        pqueue::Queue queue(queueConfig(32, 8));
        std::vector<std::string> expected;
        for (int i = 5; i < 13; ++i) {
            expected.push_back(numbered("w", i));
        }
        expectDrain(queue, expected);
    }
    completeSlowTest();
}

void test_reboot_index_fallback() {
    const std::uint8_t phase = phaseFor(SlowTest::IndexFallback);
    if (phase == 0) {
        cleanLittleFs();
        {
            pqueue::Queue queue(queueConfig(32, 40, 1));
            TEST_ASSERT_TRUE(queue.enqueue("kept-by-older-checkpoint").ok());
            TEST_ASSERT_TRUE(queue.enqueue("lost-with-newer-checkpoint").ok());
        }

        // With checkpointEveryOps=1: initial generation is slot 1, first enqueue is slot 2,
        // second enqueue is slot 3. Corrupting slot 3 should make mount fall back to slot 2.
        corruptCheckpointSlot(3);
        rebootTo(SlowTest::IndexFallback, 1);
    }

    TEST_ASSERT_EQUAL_UINT8(1, phase);
    {
        pqueue::Queue queue(queueConfig(32, 40, 1));
        TEST_ASSERT_EQUAL_UINT32(1, queue.stats().count);

        const pqueue::ValidationResult validation = queue.validate();
        TEST_ASSERT_FALSE(validation.ok);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, validation.errors.size());

        expectFrontAndPop(queue, "kept-by-older-checkpoint");
        expectQueueEmpty(queue);
    }
    completeSlowTest();
}

void test_reboot_missing_metadata_fails_safely() {
    const std::uint8_t phase = phaseFor(SlowTest::MissingMetadataFailsSafely);
    if (phase == 0) {
        cleanLittleFs();
        {
            pqueue::Queue queue(queueConfig());
            TEST_ASSERT_TRUE(queue.enqueue("spool-without-checkpoints").ok());
        }
        zeroAllCheckpointSlots();
        rebootTo(SlowTest::MissingMetadataFailsSafely, 1);
    }

    TEST_ASSERT_EQUAL_UINT8(1, phase);
    {
        pqueue::Queue queue(queueConfig());
        std::string out;
        const pqueue::Status status = queue.peek(out);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(pqueue::StatusCode::InvalidIndex), static_cast<int>(status.code));
    }
    completeSlowTest();
}

void test_reboot_corrupt_metadata_fails_safely() {
    const std::uint8_t phase = phaseFor(SlowTest::CorruptMetadataFailsSafely);
    if (phase == 0) {
        cleanLittleFs();
        {
            pqueue::Queue queue(queueConfig());
            TEST_ASSERT_TRUE(queue.enqueue("spool-with-corrupt-checkpoints").ok());
        }
        corruptAllCheckpointSlots();
        rebootTo(SlowTest::CorruptMetadataFailsSafely, 1);
    }

    TEST_ASSERT_EQUAL_UINT8(1, phase);
    {
        pqueue::Queue queue(queueConfig());
        std::string out;
        const pqueue::Status status = queue.peek(out);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(pqueue::StatusCode::InvalidIndex), static_cast<int>(status.code));

        const pqueue::ValidationResult validation = queue.validate();
        TEST_ASSERT_FALSE(validation.ok);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, validation.errors.size());
    }
    completeSlowTest();
}

void test_reboot_tmp_orphan_ignored() {
    const std::uint8_t phase = phaseFor(SlowTest::TmpOrphanIgnored);
    if (phase == 0) {
        cleanLittleFs();
        {
            pqueue::Queue queue(queueConfig());
            TEST_ASSERT_TRUE(queue.enqueue("survivor").ok());
        }
        writeOrphanTmpFile();
        rebootTo(SlowTest::TmpOrphanIgnored, 1);
    }

    TEST_ASSERT_EQUAL_UINT8(1, phase);
    {
        pqueue::Queue queue(queueConfig());
        expectFrontAndPop(queue, "survivor");
        expectQueueEmpty(queue);
    }
    completeSlowTest();
}

void test_reboot_queue_full_safe() {
    const std::uint8_t phase = phaseFor(SlowTest::QueueFullRebootSafe);
    if (phase == 0) {
        cleanLittleFs();
        {
            pqueue::Queue queue(queueConfig(16, 2));
            TEST_ASSERT_TRUE(queue.enqueue("one").ok());
            TEST_ASSERT_TRUE(queue.enqueue("two").ok());

            const pqueue::Status full = queue.enqueue("three");
            TEST_ASSERT_EQUAL_INT(static_cast<int>(pqueue::StatusCode::QueueFull), static_cast<int>(full.code));
            TEST_ASSERT_EQUAL_UINT32(2, queue.stats().count);
        }
        rebootTo(SlowTest::QueueFullRebootSafe, 1);
    }

    TEST_ASSERT_EQUAL_UINT8(1, phase);
    {
        pqueue::Queue queue(queueConfig(16, 2));
        TEST_ASSERT_EQUAL_UINT32(2, queue.stats().count);
        expectFrontAndPop(queue, "one");
        expectFrontAndPop(queue, "two");
        expectQueueEmpty(queue);
    }
    completeSlowTest();
}

void test_reboot_record_size_mismatch_fails_safely() {
    const std::uint8_t phase = phaseFor(SlowTest::RecordSizeMismatchFailsSafely);
    if (phase == 0) {
        cleanLittleFs();
        {
            pqueue::Queue queue(queueConfig(32, 4));
            TEST_ASSERT_TRUE(queue.enqueue("record-size-baseline").ok());
        }
        rebootTo(SlowTest::RecordSizeMismatchFailsSafely, 1);
    }

    TEST_ASSERT_EQUAL_UINT8(1, phase);
    {
        pqueue::Queue queue(queueConfig(64, 4));
        std::string out;
        const pqueue::Status status = queue.peek(out);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(pqueue::StatusCode::InvalidIndex), static_cast<int>(status.code));

        const pqueue::ValidationResult validation = queue.validate();
        TEST_ASSERT_FALSE(validation.ok);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, validation.errors.size());
    }
    completeSlowTest();
}

void test_reboot_capacity_mismatch_fails_safely() {
    const std::uint8_t phase = phaseFor(SlowTest::CapacityMismatchFailsSafely);
    if (phase == 0) {
        cleanLittleFs();
        {
            pqueue::Queue queue(queueConfig(32, 4));
            TEST_ASSERT_TRUE(queue.enqueue("capacity-baseline").ok());
        }
        rebootTo(SlowTest::CapacityMismatchFailsSafely, 1);
    }

    TEST_ASSERT_EQUAL_UINT8(1, phase);
    {
        pqueue::Queue queue(queueConfig(32, 5));
        std::string out;
        const pqueue::Status status = queue.peek(out);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(pqueue::StatusCode::InvalidIndex), static_cast<int>(status.code));

        const pqueue::ValidationResult validation = queue.validate();
        TEST_ASSERT_FALSE(validation.ok);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, validation.errors.size());
    }
    completeSlowTest();
}

void test_reboot_outbox_drain() {
    const std::uint8_t phase = phaseFor(SlowTest::OutboxRebootDrain);
    if (phase == 0) {
        cleanLittleFs();
        {
            FakeSender retrying;
            retrying.decision = pqueue::SendDecision::RetryLater;
            pqueue::Outbox outbox(outboxQueueConfig(), outboxConfig(), fakeSend, &retrying, fakeClock, nullptr);

            const pqueue::SubmitResult submitted = outbox.submit("outbox-reboot-payload");
            TEST_ASSERT_EQUAL_INT(static_cast<int>(pqueue::SubmitStatus::Queued), static_cast<int>(submitted.status));
            TEST_ASSERT_EQUAL_UINT16(1, retrying.calls);
            TEST_ASSERT_EQUAL_STRING("outbox-reboot-payload", retrying.lastPayload.c_str());
            TEST_ASSERT_EQUAL_UINT8(0, retrying.lastAttempts);
            TEST_ASSERT_EQUAL_UINT32(1, outbox.stats().count);
        }
        rebootTo(SlowTest::OutboxRebootDrain, 1);
    }

    TEST_ASSERT_EQUAL_UINT8(1, phase);
    {
        FakeSender succeeding;
        succeeding.decision = pqueue::SendDecision::Sent;
        pqueue::Outbox outbox(outboxQueueConfig(), outboxConfig(), fakeSend, &succeeding, fakeClock, nullptr);

        TEST_ASSERT_EQUAL_UINT32(1, outbox.stats().count);
        const pqueue::DrainResult drained = outbox.drain();
        TEST_ASSERT_EQUAL_UINT16(1, drained.attempts);
        TEST_ASSERT_EQUAL_UINT16(1, drained.sent);
        TEST_ASSERT_EQUAL_UINT16(0, drained.dropped);
        TEST_ASSERT_FALSE(drained.queueError);
        TEST_ASSERT_FALSE(drained.sendError);
        TEST_ASSERT_EQUAL_UINT16(1, succeeding.calls);
        TEST_ASSERT_EQUAL_STRING("outbox-reboot-payload", succeeding.lastPayload.c_str());
        TEST_ASSERT_EQUAL_UINT8(1, succeeding.lastAttempts);
        TEST_ASSERT_EQUAL_UINT32(0, outbox.stats().count);
    }
    completeSlowTest();
}

void test_churn_without_reboot() {
    cleanLittleFs();
    pqueue::Queue queue(queueConfig(32, 24));

    std::vector<std::string> expected;
    expected.reserve(24);

    for (int round = 0; round < 5; ++round) {
        while (expected.size() < 24) {
            const std::string record = numbered("q", round * 24 + static_cast<int>(expected.size()));
            TEST_ASSERT_TRUE(queue.enqueue(record).ok());
            expected.push_back(record);
        }

        const pqueue::Status full = queue.enqueue("overflow");
        TEST_ASSERT_EQUAL_INT(static_cast<int>(pqueue::StatusCode::QueueFull), static_cast<int>(full.code));

        for (int i = 0; i < 12; ++i) {
            expectFrontAndPop(queue, expected.front());
            expected.erase(expected.begin());
        }
    }

    expectDrain(queue, expected);
    completeSlowTest();
}

SlowTest nextSlowTest(SlowTest test) {
    switch (test) {
    case SlowTest::FifoMany:
        return SlowTest::PopRemaining;
    case SlowTest::PopRemaining:
        return SlowTest::RewriteFront;
    case SlowTest::RewriteFront:
        return SlowTest::Wraparound;
    case SlowTest::Wraparound:
        return SlowTest::IndexFallback;
    case SlowTest::IndexFallback:
        return SlowTest::MissingMetadataFailsSafely;
    case SlowTest::MissingMetadataFailsSafely:
        return SlowTest::CorruptMetadataFailsSafely;
    case SlowTest::CorruptMetadataFailsSafely:
        return SlowTest::TmpOrphanIgnored;
    case SlowTest::TmpOrphanIgnored:
        return SlowTest::QueueFullRebootSafe;
    case SlowTest::QueueFullRebootSafe:
        return SlowTest::RecordSizeMismatchFailsSafely;
    case SlowTest::RecordSizeMismatchFailsSafely:
        return SlowTest::CapacityMismatchFailsSafely;
    case SlowTest::CapacityMismatchFailsSafely:
        return SlowTest::OutboxRebootDrain;
    case SlowTest::OutboxRebootDrain:
        return SlowTest::Churn;
    case SlowTest::Churn:
    case SlowTest::Done:
        return SlowTest::Done;
    }
    return SlowTest::Done;
}

SlowTest startingSlowTest() {
    SlowState state;
    if (readSlowState(state)) {
        return state.test;
    }
    return SlowTest::FifoMany;
}

bool slowTestStillInProgress(SlowTest test) {
    SlowState state;
    return readSlowState(state) && state.test == test;
}

void runSlowTest(SlowTest test) {
    switch (test) {
    case SlowTest::FifoMany:
        RUN_TEST(test_reboot_fifo_many);
        break;
    case SlowTest::PopRemaining:
        RUN_TEST(test_reboot_pop_remaining);
        break;
    case SlowTest::RewriteFront:
        RUN_TEST(test_reboot_rewrite_front);
        break;
    case SlowTest::Wraparound:
        RUN_TEST(test_reboot_wraparound);
        break;
    case SlowTest::IndexFallback:
        RUN_TEST(test_reboot_index_fallback);
        break;
    case SlowTest::MissingMetadataFailsSafely:
        RUN_TEST(test_reboot_missing_metadata_fails_safely);
        break;
    case SlowTest::CorruptMetadataFailsSafely:
        RUN_TEST(test_reboot_corrupt_metadata_fails_safely);
        break;
    case SlowTest::TmpOrphanIgnored:
        RUN_TEST(test_reboot_tmp_orphan_ignored);
        break;
    case SlowTest::QueueFullRebootSafe:
        RUN_TEST(test_reboot_queue_full_safe);
        break;
    case SlowTest::RecordSizeMismatchFailsSafely:
        RUN_TEST(test_reboot_record_size_mismatch_fails_safely);
        break;
    case SlowTest::CapacityMismatchFailsSafely:
        RUN_TEST(test_reboot_capacity_mismatch_fails_safely);
        break;
    case SlowTest::OutboxRebootDrain:
        RUN_TEST(test_reboot_outbox_drain);
        break;
    case SlowTest::Churn:
        RUN_TEST(test_churn_without_reboot);
        break;
    case SlowTest::Done:
        break;
    }
}

} // namespace

void setup() {
    delay(2000);
    UNITY_BEGIN();

    SlowTest current = startingSlowTest();
    while (current != SlowTest::Done) {
        runSlowTest(current);
        if (slowTestStillInProgress(current)) {
            break;
        }
        current = nextSlowTest(current);
    }

    clearSlowState();
    cleanLittleFs();
    UNITY_END();
}

void loop() {}

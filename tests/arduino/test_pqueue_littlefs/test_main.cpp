#include <Arduino.h>
#include <LittleFS.h>
#include <unity.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

#include "pqueue/outbox.h"
#include "pqueue/queue.h"
#include "pqueue/status.h"
#include "pqueue/storage_common.h"
#include "pqueue/types.h"

namespace {

constexpr const char* kBasePath = "/pqueue_test";
constexpr const char* kOtherBasePath = "/pqueue_test_other";
constexpr const char* kAppendLogBasePath = "/pqueue_test_append_log";
constexpr const char* kAppendLogOtherBasePath = "/pqueue_test_append_log_other";
constexpr const char* kSpoolPath = "/pqueue_test/pqueue.spool";
constexpr const char* kRebootStatePath = "/pqueue_reboot_state";
constexpr std::uint8_t kRebootPhaseVerifyInitial = 1;
constexpr std::uint8_t kRebootPhaseVerifyMutated = 2;
constexpr std::uint8_t kRebootPhaseFailed = 99;

std::uint64_t g_nowMs = 0;

struct FakeSender {
    pqueue::SendDecision decision = pqueue::SendDecision::Sent;
    std::uint16_t calls = 0;
    std::string lastPayload;
    std::uint8_t lastAttempts = 0;
};

std::uint64_t fakeClock(void*) {
    return g_nowMs;
}

pqueue::SendResult fakeSend(void* context, const std::string& payload, const pqueue::RetryState& retry) {
    auto* sender = static_cast<FakeSender*>(context);
    sender->calls += 1;
    sender->lastPayload = payload;
    sender->lastAttempts = retry.attempts;
    return {sender->decision};
}

std::uint32_t slotSize(std::size_t recordSizeBytes) {
    return static_cast<std::uint32_t>(sizeof(pqueue::storage_detail::RecordHeader) + recordSizeBytes);
}

std::uint32_t recordRegionOffset(const pqueue::Config& config) {
    return static_cast<std::uint32_t>(pqueue::storage_detail::kCheckpointSlots) *
               pqueue::storage_detail::kCheckpointRecordBytes +
           config.journalBytes;
}

std::uint32_t recordSlotOffset(const pqueue::Config& config, std::uint32_t slot) {
    return recordRegionOffset(config) + slot * slotSize(config.recordSizeBytes);
}

void corruptSlotPayload(const pqueue::Config& config, std::uint32_t slot) {
    LittleFS.end();
    TEST_ASSERT_TRUE_MESSAGE(LittleFS.begin(true), "LittleFS mount failed for slot corruption");
    File spool = LittleFS.open(kSpoolPath, "r+");
    TEST_ASSERT_TRUE_MESSAGE(spool, "failed to open pqueue spool for slot corruption");
    const std::uint32_t payloadOffset =
        recordSlotOffset(config, slot) + pqueue::storage_detail::kRecordHeaderBytes;
    TEST_ASSERT_TRUE(spool.seek(payloadOffset, SeekSet));
    TEST_ASSERT_EQUAL_UINT(1, spool.write(static_cast<uint8_t>(0xff)));
    spool.flush();
    spool.close();
    LittleFS.end();
}

pqueue::Config queueConfigForBase(
    const char* basePath,
    std::size_t recordSizeBytes = 32,
    std::uint32_t capacityRecords = 8
) {
    pqueue::Config config;
    config.basePath = basePath;
    config.storageBackend = pqueue::StorageBackend::LittleFS;
    config.recordSizeBytes = recordSizeBytes;
    config.reservedBytes = slotSize(recordSizeBytes) * capacityRecords;
    return config;
}

pqueue::Config queueConfig(std::size_t recordSizeBytes = 32, std::uint32_t capacityRecords = 8) {
    return queueConfigForBase(kBasePath, recordSizeBytes, capacityRecords);
}

pqueue::Config appendLogQueueConfigForBase(
    const char* basePath,
    std::size_t recordSizeBytes = 32
) {
    auto config = queueConfigForBase(basePath, recordSizeBytes, 8);
    config.storeLayout = pqueue::StoreLayout::AppendLog;
    config.maxSegmentBytes = 256;
    config.maxSegments = 8;
    config.minFreeBytes = 0;
    return config;
}

pqueue::Config appendLogQueueConfig(std::size_t recordSizeBytes = 32) {
    return appendLogQueueConfigForBase(kAppendLogBasePath, recordSizeBytes);
}

pqueue::OutboxConfig outboxConfig() {
    pqueue::OutboxConfig config;
    config.retryDelayMs = 0;
    config.maxDrainAttemptsPerSecond = 1000;
    return config;
}

void cleanLittleFs() {
    LittleFS.end();
    TEST_ASSERT_TRUE_MESSAGE(LittleFS.begin(true), "LittleFS mount failed");
    TEST_ASSERT_TRUE_MESSAGE(LittleFS.format(), "LittleFS format failed");
    LittleFS.end();
    g_nowMs = 0;
}


bool mountLittleFsForRebootSmoke() {
    LittleFS.end();
    return LittleFS.begin(true);
}

bool formatLittleFsForRebootSmoke() {
    LittleFS.end();
    if (!LittleFS.begin(true)) {
        return false;
    }
    if (!LittleFS.format()) {
        LittleFS.end();
        return false;
    }
    LittleFS.end();
    g_nowMs = 0;
    return true;
}

bool readRebootState(std::uint8_t& phase, std::string& message) {
    phase = 0;
    message.clear();

    if (!mountLittleFsForRebootSmoke()) {
        phase = kRebootPhaseFailed;
        message = "LittleFS mount failed while reading reboot state";
        return true;
    }

    File file = LittleFS.open(kRebootStatePath, "r");
    if (!file) {
        LittleFS.end();
        return false;
    }

    String phaseLine = file.readStringUntil('\n');
    String messageLine = file.readStringUntil('\n');
    file.close();
    LittleFS.end();

    phase = static_cast<std::uint8_t>(std::atoi(phaseLine.c_str()));
    message = messageLine.c_str();
    return true;
}

void writeRebootState(std::uint8_t phase, const char* message = "") {
    if (!mountLittleFsForRebootSmoke()) {
        Serial.println("[pqueue reboot smoke] failed to mount LittleFS while writing state");
        delay(100);
        ESP.restart();
    }

    File file = LittleFS.open(kRebootStatePath, "w");
    if (!file) {
        Serial.println("[pqueue reboot smoke] failed to open state file for write");
        LittleFS.end();
        delay(100);
        ESP.restart();
    }

    file.printf("%u\n%s\n", static_cast<unsigned>(phase), message);
    file.flush();
    file.close();
    LittleFS.end();
}

void clearRebootState() {
    if (!mountLittleFsForRebootSmoke()) {
        return;
    }
    LittleFS.remove(kRebootStatePath);
    LittleFS.end();
}

void rebootSmokeFail(const char* message) {
    Serial.print("[pqueue reboot smoke] FAIL: ");
    Serial.println(message);
    writeRebootState(kRebootPhaseFailed, message);
    delay(100);
    ESP.restart();
}

void verifyOkOrRebootFail(const pqueue::Status& status, const char* message) {
    if (!status.ok()) {
        rebootSmokeFail(message);
    }
}

void verifyStringOrRebootFail(const std::string& actual, const char* expected, const char* message) {
    if (actual != expected) {
        rebootSmokeFail(message);
    }
}

void runQuickRebootSmokePhaseIfNeeded() {
    std::uint8_t phase = 0;
    std::string message;
    const bool hasState = readRebootState(phase, message);

    if (!hasState) {
        Serial.println("[pqueue reboot smoke] phase 0: write initial queue, then reboot");
        if (!formatLittleFsForRebootSmoke()) {
            rebootSmokeFail("phase 0 LittleFS format failed");
        }
        {
            pqueue::Queue queue(queueConfig());
            verifyOkOrRebootFail(queue.enqueue("one"), "phase 0 enqueue one failed");
            verifyOkOrRebootFail(queue.enqueue("two"), "phase 0 enqueue two failed");
            verifyOkOrRebootFail(queue.enqueue("three"), "phase 0 enqueue three failed");
        }
        writeRebootState(kRebootPhaseVerifyInitial);
        delay(100);
        ESP.restart();
    }

    if (phase == kRebootPhaseVerifyInitial) {
        Serial.println("[pqueue reboot smoke] phase 1: verify initial queue, mutate, then reboot");
        {
            pqueue::Queue queue(queueConfig());
            std::string out;
            verifyOkOrRebootFail(queue.peek(out), "phase 1 peek one failed");
            verifyStringOrRebootFail(out, "one", "phase 1 expected one");
            verifyOkOrRebootFail(queue.pop(), "phase 1 pop one failed");
            verifyOkOrRebootFail(queue.peek(out), "phase 1 peek two failed");
            verifyStringOrRebootFail(out, "two", "phase 1 expected two");
            verifyOkOrRebootFail(queue.rewriteFront("two-rewritten"), "phase 1 rewrite front failed");
        }
        writeRebootState(kRebootPhaseVerifyMutated);
        delay(100);
        ESP.restart();
    }
}

void assertQueueEmpty(pqueue::Queue& queue) {
    std::string out;
    const pqueue::Status status = queue.peek(out);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(pqueue::StatusCode::QueueEmpty), static_cast<int>(status.code));
}

void test_basic_fifo() {
    cleanLittleFs();
    pqueue::Queue queue(queueConfig());

    TEST_ASSERT_TRUE(queue.enqueue("one").ok());
    TEST_ASSERT_TRUE(queue.enqueue("two").ok());
    TEST_ASSERT_TRUE(queue.enqueue("three").ok());

    std::string out;
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("one", out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());

    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("two", out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());

    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("three", out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());

    assertQueueEmpty(queue);
}

void test_remount_persistence() {
    cleanLittleFs();
    {
        pqueue::Queue queue(queueConfig());
        TEST_ASSERT_TRUE(queue.enqueue("one").ok());
        TEST_ASSERT_TRUE(queue.enqueue("two").ok());
        TEST_ASSERT_TRUE(queue.enqueue("three").ok());
    }

    pqueue::Queue queue(queueConfig());
    std::string out;
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("one", out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("two", out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("three", out.c_str());
}

void test_pop_persistence() {
    cleanLittleFs();
    {
        pqueue::Queue queue(queueConfig());
        TEST_ASSERT_TRUE(queue.enqueue("one").ok());
        TEST_ASSERT_TRUE(queue.enqueue("two").ok());
        TEST_ASSERT_TRUE(queue.enqueue("three").ok());
        TEST_ASSERT_TRUE(queue.pop().ok());
        TEST_ASSERT_TRUE(queue.pop().ok());
    }

    pqueue::Queue queue(queueConfig());
    std::string out;
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("three", out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());
    assertQueueEmpty(queue);
}

void test_rewrite_front_persistence() {
    cleanLittleFs();
    {
        pqueue::Queue queue(queueConfig());
        TEST_ASSERT_TRUE(queue.enqueue("old").ok());
        TEST_ASSERT_TRUE(queue.enqueue("tail").ok());
        TEST_ASSERT_TRUE(queue.rewriteFront("new").ok());
    }

    pqueue::Queue queue(queueConfig());
    std::string out;
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("new", out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("tail", out.c_str());
}

void test_capacity_full_behavior() {
    cleanLittleFs();
    {
        pqueue::Queue queue(queueConfig(16, 2));

        const auto one = queue.enqueue("one");
        TEST_ASSERT_TRUE(one.ok());

        const auto two = queue.enqueue("two");
        TEST_ASSERT_TRUE(two.ok());

        const pqueue::Status full = queue.enqueue("three");
        TEST_ASSERT_EQUAL_INT(static_cast<int>(pqueue::StatusCode::QueueFull), static_cast<int>(full.code));

        TEST_ASSERT_EQUAL_UINT32(2, queue.stats().count);
    }

    pqueue::Queue queue(queueConfig(16, 2));
    TEST_ASSERT_EQUAL_UINT32(2, queue.stats().count);

    std::string out;
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("one", out.c_str());

    TEST_ASSERT_TRUE(queue.pop().ok());

    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("two", out.c_str());

    TEST_ASSERT_TRUE(queue.pop().ok());

    assertQueueEmpty(queue);
}

void test_validate_clean_queue() {
    cleanLittleFs();
    pqueue::Queue queue(queueConfig());

    TEST_ASSERT_TRUE(queue.enqueue("one").ok());
    TEST_ASSERT_TRUE(queue.enqueue("two").ok());

    const pqueue::ValidationResult validation = queue.validate();
    TEST_ASSERT_TRUE(validation.ok);
    TEST_ASSERT_EQUAL_UINT32(0, validation.errors.size());
}

void test_record_size_boundary() {
    cleanLittleFs();
    pqueue::Queue queue(queueConfig(4, 2));

    TEST_ASSERT_TRUE(queue.enqueue("1234").ok());
    const pqueue::Status tooLarge = queue.enqueue("12345");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(pqueue::StatusCode::RecordTooLarge), static_cast<int>(tooLarge.code));
    TEST_ASSERT_EQUAL_UINT32(1, queue.stats().count);
}

void test_multiple_queue_objects_share_same_base_path() {
    cleanLittleFs();
    pqueue::Queue first(queueConfig());
    pqueue::Queue second(queueConfig());

    const auto firstStatus = first.enqueue("first");
    TEST_ASSERT_TRUE(firstStatus.ok());

    const auto secondStatus = second.enqueue("second");
    TEST_ASSERT_TRUE(secondStatus.ok());

    std::string out;
    TEST_ASSERT_TRUE(first.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("first", out.c_str());

    TEST_ASSERT_TRUE(first.pop().ok());

    TEST_ASSERT_TRUE(second.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("second", out.c_str());
    TEST_ASSERT_EQUAL_UINT32(1, second.stats().count);
}

void test_queue_lock_released_after_each_operation() {
    cleanLittleFs();

    pqueue::Queue first(queueConfig());
    TEST_ASSERT_TRUE(first.enqueue("first").ok());

    pqueue::Queue second(queueConfig());
    TEST_ASSERT_TRUE(second.enqueue("second").ok());

    TEST_ASSERT_EQUAL_UINT32(2, second.stats().count);
}

void test_littlefs_locks_are_independent_across_base_paths() {
    cleanLittleFs();

    pqueue::Queue first(queueConfigForBase(kBasePath));
    TEST_ASSERT_TRUE(first.enqueue("first").ok());

    pqueue::Queue second(queueConfigForBase(kOtherBasePath));
    TEST_ASSERT_TRUE(second.enqueue("other-base").ok());
    TEST_ASSERT_EQUAL_UINT32(1, first.stats().count);
    TEST_ASSERT_EQUAL_UINT32(1, second.stats().count);
}

void test_corrupt_active_record() {
    cleanLittleFs();
    {
        pqueue::Queue queue(queueConfig());
        TEST_ASSERT_TRUE(queue.enqueue("one").ok());
    }

    File spool = LittleFS.open(kSpoolPath, "r+");
    TEST_ASSERT_TRUE_MESSAGE(spool, "failed to open pqueue spool for corruption");

    const std::uint32_t payloadOffset =
        static_cast<std::uint32_t>(pqueue::storage_detail::kCheckpointSlots) *
            pqueue::storage_detail::kCheckpointRecordBytes +
        4096U +
        pqueue::storage_detail::kRecordHeaderBytes;

    TEST_ASSERT_TRUE(spool.seek(payloadOffset, SeekSet));
    TEST_ASSERT_EQUAL_UINT(1, spool.write(static_cast<uint8_t>(0xff)));
    spool.flush();
    spool.close();

    pqueue::Queue queue(queueConfig());
    std::string out;
    const pqueue::Status readStatus = queue.peek(out);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(pqueue::StatusCode::CrcMismatch), static_cast<int>(readStatus.code));

    const pqueue::ValidationResult validation = queue.validate();
    TEST_ASSERT_FALSE(validation.ok);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, validation.errors.size());
}

void test_outbox_drops_corrupt_front_record_on_littlefs() {
    cleanLittleFs();
    g_nowMs = 1000;
    const pqueue::Config config = queueConfig();

    {
        FakeSender retrying;
        retrying.decision = pqueue::SendDecision::RetryLater;
        pqueue::Outbox outbox(config, outboxConfig(), fakeSend, &retrying, fakeClock, nullptr);

        const pqueue::SubmitResult first = outbox.submit("bad");
        TEST_ASSERT_EQUAL_INT(static_cast<int>(pqueue::SubmitStatus::Queued), static_cast<int>(first.status));
        TEST_ASSERT_EQUAL_UINT16(1, retrying.calls);

        const pqueue::SubmitResult second = outbox.submit("ok");
        TEST_ASSERT_EQUAL_INT(static_cast<int>(pqueue::SubmitStatus::Queued), static_cast<int>(second.status));
        TEST_ASSERT_EQUAL_UINT32(2, outbox.stats().count);
    }

    corruptSlotPayload(config, 0);

    FakeSender succeeding;
    succeeding.decision = pqueue::SendDecision::Sent;
    pqueue::Outbox outbox(config, outboxConfig(), fakeSend, &succeeding, fakeClock, nullptr);
    const pqueue::DrainResult drained = outbox.drainUpTo(2);

    TEST_ASSERT_EQUAL_UINT16(1, drained.attempts);
    TEST_ASSERT_EQUAL_UINT16(1, drained.corruptDropped);
    TEST_ASSERT_EQUAL_UINT16(1, drained.sent);
    TEST_ASSERT_EQUAL_UINT16(0, drained.dropped);
    TEST_ASSERT_FALSE(drained.queueError);
    TEST_ASSERT_FALSE(drained.sendError);
    TEST_ASSERT_EQUAL_UINT16(1, succeeding.calls);
    TEST_ASSERT_EQUAL_STRING("ok", succeeding.lastPayload.c_str());
    TEST_ASSERT_EQUAL_UINT32(0, outbox.stats().count);
}

void test_outbox_backlog_persistence() {
    cleanLittleFs();
    FakeSender retrying;
    retrying.decision = pqueue::SendDecision::RetryLater;
    {
        pqueue::Outbox outbox(queueConfig(), outboxConfig(), fakeSend, &retrying, fakeClock, nullptr);
        const pqueue::SubmitResult submitted = outbox.submit("payload");
        TEST_ASSERT_EQUAL_INT(static_cast<int>(pqueue::SubmitStatus::Queued), static_cast<int>(submitted.status));
        TEST_ASSERT_EQUAL_UINT16(1, retrying.calls);
        TEST_ASSERT_EQUAL_UINT32(1, outbox.stats().count);
    }

    FakeSender succeeding;
    succeeding.decision = pqueue::SendDecision::Sent;
    pqueue::Outbox outbox(queueConfig(), outboxConfig(), fakeSend, &succeeding, fakeClock, nullptr);
    TEST_ASSERT_EQUAL_UINT32(1, outbox.stats().count);
    const pqueue::DrainResult drained = outbox.drain();
    TEST_ASSERT_EQUAL_UINT16(1, drained.attempts);
    TEST_ASSERT_EQUAL_UINT16(1, drained.sent);
    TEST_ASSERT_EQUAL_UINT32(0, outbox.stats().count);
    TEST_ASSERT_EQUAL_STRING("payload", succeeding.lastPayload.c_str());
}

void test_retryable_failure_does_not_drop() {
    cleanLittleFs();
    FakeSender retrying;
    retrying.decision = pqueue::SendDecision::RetryLater;
    {
        pqueue::Outbox outbox(queueConfig(), outboxConfig(), fakeSend, &retrying, fakeClock, nullptr);
        const pqueue::SubmitResult submitted = outbox.submit("payload");
        TEST_ASSERT_EQUAL_INT(static_cast<int>(pqueue::SubmitStatus::Queued), static_cast<int>(submitted.status));
        TEST_ASSERT_EQUAL_UINT32(1, outbox.stats().count);

        for (int i = 0; i < 3; ++i) {
            const pqueue::DrainResult drained = outbox.drainUpTo(1);
            TEST_ASSERT_EQUAL_UINT16(1, drained.attempts);
            TEST_ASSERT_EQUAL_UINT16(0, drained.sent);
            TEST_ASSERT_EQUAL_UINT16(0, drained.dropped);
            TEST_ASSERT_FALSE(drained.queueError);
            TEST_ASSERT_EQUAL_UINT32(1, outbox.stats().count);
        }
    }

    pqueue::Queue queue(queueConfig());
    TEST_ASSERT_EQUAL_UINT32(1, queue.stats().count);
}


void test_quick_reboot_persistence() {
    std::uint8_t phase = 0;
    std::string message;
    TEST_ASSERT_TRUE_MESSAGE(readRebootState(phase, message), "quick reboot state missing");
    if (phase == kRebootPhaseFailed) {
        TEST_FAIL_MESSAGE(message.empty() ? "quick reboot smoke failed before Unity phase" : message.c_str());
    }
    TEST_ASSERT_EQUAL_UINT8(kRebootPhaseVerifyMutated, phase);

    pqueue::Queue queue(queueConfig());
    std::string out;
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("two-rewritten", out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("three", out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());
    assertQueueEmpty(queue);

    clearRebootState();
}

// --- AppendLog Queue variants ---

void test_append_log_basic_fifo() {
    cleanLittleFs();
    pqueue::Queue queue(appendLogQueueConfig());

    TEST_ASSERT_TRUE(queue.enqueue("one").ok());
    TEST_ASSERT_TRUE(queue.enqueue("two").ok());
    TEST_ASSERT_TRUE(queue.enqueue("three").ok());

    std::string out;
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("one", out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());

    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("two", out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());

    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("three", out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());

    assertQueueEmpty(queue);
}

void test_append_log_remount_persistence() {
    cleanLittleFs();
    {
        pqueue::Queue queue(appendLogQueueConfig());
        TEST_ASSERT_TRUE(queue.enqueue("one").ok());
        TEST_ASSERT_TRUE(queue.enqueue("two").ok());
        TEST_ASSERT_TRUE(queue.enqueue("three").ok());
    }

    pqueue::Queue queue(appendLogQueueConfig());
    std::string out;
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("one", out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("two", out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("three", out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());
    assertQueueEmpty(queue);
}

void test_append_log_pop_persistence() {
    cleanLittleFs();
    {
        pqueue::Queue queue(appendLogQueueConfig());
        TEST_ASSERT_TRUE(queue.enqueue("one").ok());
        TEST_ASSERT_TRUE(queue.enqueue("two").ok());
        TEST_ASSERT_TRUE(queue.enqueue("three").ok());
        TEST_ASSERT_TRUE(queue.pop().ok());
        TEST_ASSERT_TRUE(queue.pop().ok());
    }

    pqueue::Queue queue(appendLogQueueConfig());
    std::string out;
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("three", out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());
    assertQueueEmpty(queue);
}

void test_append_log_rewrite_front_persistence() {
    cleanLittleFs();
    {
        pqueue::Queue queue(appendLogQueueConfig());
        TEST_ASSERT_TRUE(queue.enqueue("old").ok());
        TEST_ASSERT_TRUE(queue.enqueue("tail").ok());
        TEST_ASSERT_TRUE(queue.rewriteFront("new").ok());
    }

    pqueue::Queue queue(appendLogQueueConfig());
    std::string out;
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("new", out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("tail", out.c_str());
}

void test_append_log_validate_clean_queue() {
    cleanLittleFs();
    pqueue::Queue queue(appendLogQueueConfig());

    TEST_ASSERT_TRUE(queue.enqueue("one").ok());
    TEST_ASSERT_TRUE(queue.enqueue("two").ok());

    const pqueue::ValidationResult validation = queue.validate();
    TEST_ASSERT_TRUE(validation.ok);
    TEST_ASSERT_EQUAL_UINT32(0, validation.errors.size());
}

void test_append_log_record_size_boundary() {
    cleanLittleFs();
    pqueue::Queue queue(appendLogQueueConfig(4));

    TEST_ASSERT_TRUE(queue.enqueue("1234").ok());
    const pqueue::Status tooLarge = queue.enqueue("12345");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(pqueue::StatusCode::RecordTooLarge), static_cast<int>(tooLarge.code));
    TEST_ASSERT_EQUAL_UINT32(1, queue.stats().count);
}

void test_append_log_lock_released_after_each_operation() {
    cleanLittleFs();

    pqueue::Queue first(appendLogQueueConfig());
    TEST_ASSERT_TRUE(first.enqueue("first").ok());

    pqueue::Queue second(appendLogQueueConfig());
    TEST_ASSERT_TRUE(second.enqueue("second").ok());

    TEST_ASSERT_EQUAL_UINT32(2, second.stats().count);
}

void test_append_log_locks_are_independent_across_base_paths() {
    cleanLittleFs();

    pqueue::Queue first(appendLogQueueConfigForBase(kAppendLogBasePath));
    TEST_ASSERT_TRUE(first.enqueue("first").ok());

    pqueue::Queue second(appendLogQueueConfigForBase(kAppendLogOtherBasePath));
    TEST_ASSERT_TRUE(second.enqueue("other-base").ok());
    TEST_ASSERT_EQUAL_UINT32(1, first.stats().count);
    TEST_ASSERT_EQUAL_UINT32(1, second.stats().count);
}

void test_append_log_compact_idle_survives_remount() {
    cleanLittleFs();
    // maxSegmentBytes=45: kSegmentHeaderBytes(20)+kEnqueueHeaderBytes(16)+1byte+kEventTrailerBytes(8)
    // fits exactly one 1-byte record per segment, so A/B/C each seal a segment and
    // the rewrite of A leaves dead bytes in seg1, giving compactIdle real work.
    auto cfg = appendLogQueueConfigForBase(kAppendLogBasePath);
    cfg.maxSegmentBytes = 45;
    {
        pqueue::Queue queue(cfg);
        TEST_ASSERT_TRUE(queue.enqueue("A").ok());
        TEST_ASSERT_TRUE(queue.enqueue("B").ok());
        TEST_ASSERT_TRUE(queue.enqueue("C").ok());
        TEST_ASSERT_TRUE(queue.rewriteFront("X").ok());
        const pqueue::CompactIdleResult result = queue.compactIdle(16);
        TEST_ASSERT_TRUE(result.status.ok());
        TEST_ASSERT_TRUE(result.compactions > 0);
    }

    pqueue::Queue queue(cfg);
    std::string out;
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("X", out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("B", out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("C", out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());
    assertQueueEmpty(queue);
}

} // namespace

void setup() {
    delay(2000);
    runQuickRebootSmokePhaseIfNeeded();
    UNITY_BEGIN();
    RUN_TEST(test_quick_reboot_persistence);
    RUN_TEST(test_basic_fifo);
    RUN_TEST(test_remount_persistence);
    RUN_TEST(test_pop_persistence);
    RUN_TEST(test_rewrite_front_persistence);
    RUN_TEST(test_capacity_full_behavior);
    RUN_TEST(test_validate_clean_queue);
    RUN_TEST(test_record_size_boundary);
    RUN_TEST(test_multiple_queue_objects_share_same_base_path);
    RUN_TEST(test_queue_lock_released_after_each_operation);
    RUN_TEST(test_littlefs_locks_are_independent_across_base_paths);
    RUN_TEST(test_corrupt_active_record);
    RUN_TEST(test_outbox_drops_corrupt_front_record_on_littlefs);
    RUN_TEST(test_outbox_backlog_persistence);
    RUN_TEST(test_retryable_failure_does_not_drop);
    RUN_TEST(test_append_log_basic_fifo);
    RUN_TEST(test_append_log_remount_persistence);
    RUN_TEST(test_append_log_pop_persistence);
    RUN_TEST(test_append_log_rewrite_front_persistence);
    RUN_TEST(test_append_log_validate_clean_queue);
    RUN_TEST(test_append_log_record_size_boundary);
    RUN_TEST(test_append_log_lock_released_after_each_operation);
    RUN_TEST(test_append_log_locks_are_independent_across_base_paths);
    RUN_TEST(test_append_log_compact_idle_survives_remount);
    UNITY_END();
}

void loop() {}

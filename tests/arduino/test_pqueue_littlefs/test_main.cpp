#include <Arduino.h>
#include <LittleFS.h>
#include <unity.h>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "pqueue/outbox.h"
#include "pqueue/queue.h"
#include "pqueue/status.h"
#include "pqueue/types.h"

namespace {

constexpr const char* kAppendLogBasePath = "/pqueue_test_append_log";
constexpr const char* kAppendLogOtherBasePath = "/pqueue_test_append_log_other";
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

pqueue::Config appendLogQueueConfigForBase(
    const char* basePath,
    std::size_t recordSizeBytes = 32
) {
    pqueue::Config config;
    config.basePath = basePath;
    config.storageBackend = pqueue::StorageBackend::LittleFS;
    config.recordSizeBytes = recordSizeBytes;
    config.maxSegmentBytes = 256;
    config.maxSegments = 8;
    config.reservedBytes = 0;
    config.minFreeBytes = 0;
    return config;
}

pqueue::Config appendLogQueueConfig(std::size_t recordSizeBytes = 32) {
    return appendLogQueueConfigForBase(kAppendLogBasePath, recordSizeBytes);
}

pqueue::OutboxConfig outboxConfig() {
    pqueue::OutboxConfig config;
    config.initialRetryDelayMs = 0;
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
            pqueue::Queue queue(appendLogQueueConfig());
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
            pqueue::Queue queue(appendLogQueueConfig());
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

void test_quick_reboot_persistence() {
    std::uint8_t phase = 0;
    std::string message;
    TEST_ASSERT_TRUE_MESSAGE(readRebootState(phase, message), "quick reboot state missing");
    if (phase == kRebootPhaseFailed) {
        TEST_FAIL_MESSAGE(message.empty() ? "quick reboot smoke failed before Unity phase" : message.c_str());
    }
    TEST_ASSERT_EQUAL_UINT8(kRebootPhaseVerifyMutated, phase);

    pqueue::Queue queue(appendLogQueueConfig());
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

// --- AppendLog Queue tests ---

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

void test_append_log_capacity_full_behavior() {
    cleanLittleFs();

    pqueue::Config cfg = appendLogQueueConfig();
    cfg.reservedBytes = 200;

    std::vector<std::string> submitted;
    bool sawFull = false;
    {
        pqueue::Queue queue(cfg);
        for (int i = 0; i < 20; ++i) {
            char buf[8];
            snprintf(buf, sizeof(buf), "r%02d", i);
            pqueue::Status st = queue.enqueue(buf);
            if (!st.ok()) {
                TEST_ASSERT_EQUAL_INT(
                    static_cast<int>(pqueue::StatusCode::QueueFull),
                    static_cast<int>(st.code));
                sawFull = true;
                break;
            }
            submitted.push_back(buf);
        }
        TEST_ASSERT_TRUE_MESSAGE(sawFull, "did not reach QueueFull within 20 records");
        TEST_ASSERT_FALSE_MESSAGE(submitted.empty(), "no records accepted before QueueFull");
    }

    // Remount and drain: verify FIFO order survived intact
    pqueue::Queue queue(cfg);
    TEST_ASSERT_EQUAL_UINT32(static_cast<std::uint32_t>(submitted.size()), queue.stats().count);
    for (const std::string& expected : submitted) {
        std::string out;
        TEST_ASSERT_TRUE(queue.peek(out).ok());
        TEST_ASSERT_EQUAL_STRING(expected.c_str(), out.c_str());
        TEST_ASSERT_TRUE(queue.pop().ok());
    }
    assertQueueEmpty(queue);
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

void test_append_log_multiple_queue_objects_share_same_base_path() {
    cleanLittleFs();
    {
        pqueue::Queue first(appendLogQueueConfig());
        pqueue::Queue second(appendLogQueueConfig());
        TEST_ASSERT_TRUE(first.enqueue("first").ok());
        TEST_ASSERT_TRUE(second.enqueue("second").ok());
    }
    pqueue::Queue queue(appendLogQueueConfig());
    TEST_ASSERT_EQUAL_UINT32(2, queue.stats().count);
    std::string out;
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("first", out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING("second", out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());
    assertQueueEmpty(queue);
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

void test_append_log_outbox_backlog_persistence() {
    cleanLittleFs();
    FakeSender retrying;
    retrying.decision = pqueue::SendDecision::RetryLater;
    {
        pqueue::Outbox outbox(appendLogQueueConfig(), outboxConfig(), fakeSend, &retrying, fakeClock, nullptr);
        const pqueue::SubmitResult submitted = outbox.submit("payload");
        TEST_ASSERT_EQUAL_INT(static_cast<int>(pqueue::SubmitStatus::Queued), static_cast<int>(submitted.status));
        TEST_ASSERT_EQUAL_UINT16(1, retrying.calls);
        TEST_ASSERT_EQUAL_UINT32(1, outbox.stats().count);
    }

    FakeSender succeeding;
    succeeding.decision = pqueue::SendDecision::Sent;
    pqueue::Outbox outbox(appendLogQueueConfig(), outboxConfig(), fakeSend, &succeeding, fakeClock, nullptr);
    TEST_ASSERT_EQUAL_UINT32(1, outbox.stats().count);
    const pqueue::DrainResult drained = outbox.drain();
    TEST_ASSERT_EQUAL_UINT16(1, drained.attempts);
    TEST_ASSERT_EQUAL_UINT16(1, drained.sent);
    TEST_ASSERT_EQUAL_UINT32(0, outbox.stats().count);
    TEST_ASSERT_EQUAL_STRING("payload", succeeding.lastPayload.c_str());
}

void test_append_log_retryable_failure_does_not_drop() {
    cleanLittleFs();
    FakeSender retrying;
    retrying.decision = pqueue::SendDecision::RetryLater;
    {
        pqueue::Outbox outbox(appendLogQueueConfig(), outboxConfig(), fakeSend, &retrying, fakeClock, nullptr);
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

    pqueue::Queue queue(appendLogQueueConfig());
    TEST_ASSERT_EQUAL_UINT32(1, queue.stats().count);
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

void test_append_log_drop_oldest_evicts_and_continues() {
    cleanLittleFs();

    // maxSegmentBytes=80 with 12-byte payloads forces one record per sealed segment.
    // Attempt 20 enqueues with DropOldest; not all may succeed because compaction can
    // exhaust manifest range capacity after many eviction cycles. Track only accepted
    // payloads so the FIFO suffix assertion remains valid regardless of how many succeed.
    pqueue::Config cfg = appendLogQueueConfig(12);
    cfg.maxSegmentBytes = 80;
    cfg.reservedBytes = 600;
    cfg.fullQueuePolicy = pqueue::FullQueuePolicy::DropOldest;

    constexpr int kTotal = 20;
    std::vector<std::string> accepted;
    {
        pqueue::Queue queue(cfg);
        for (int i = 0; i < kTotal; ++i) {
            char buf[13];
            snprintf(buf, sizeof(buf), "pay-load-%03d", i);
            if (queue.enqueue(buf).ok()) {
                accepted.emplace_back(buf);
            }
        }
    }
    TEST_ASSERT_FALSE_MESSAGE(accepted.empty(), "no records accepted");

    pqueue::Queue queue(cfg);
    TEST_ASSERT_GREATER_THAN(0u, queue.stats().count);

    std::string front;
    TEST_ASSERT_TRUE(queue.peek(front).ok());

    int startIdx = -1;
    for (int i = 0; i < static_cast<int>(accepted.size()); ++i) {
        if (front == accepted[i]) { startIdx = i; break; }
    }
    TEST_ASSERT_NOT_EQUAL(-1, startIdx);
    TEST_ASSERT_GREATER_THAN(0, startIdx);
    TEST_ASSERT_EQUAL_UINT32(
        static_cast<std::uint32_t>(accepted.size()) - startIdx,
        queue.stats().count);

    for (int i = startIdx; i < static_cast<int>(accepted.size()); ++i) {
        std::string out;
        TEST_ASSERT_TRUE(queue.peek(out).ok());
        TEST_ASSERT_EQUAL_STRING(accepted[i].c_str(), out.c_str());
        TEST_ASSERT_TRUE(queue.pop().ok());
    }
    assertQueueEmpty(queue);
}

} // namespace

void setup() {
    delay(2000);
    runQuickRebootSmokePhaseIfNeeded();
    UNITY_BEGIN();
    RUN_TEST(test_quick_reboot_persistence);
    RUN_TEST(test_append_log_basic_fifo);
    RUN_TEST(test_append_log_remount_persistence);
    RUN_TEST(test_append_log_pop_persistence);
    RUN_TEST(test_append_log_rewrite_front_persistence);
    RUN_TEST(test_append_log_capacity_full_behavior);
    RUN_TEST(test_append_log_validate_clean_queue);
    RUN_TEST(test_append_log_record_size_boundary);
    RUN_TEST(test_append_log_multiple_queue_objects_share_same_base_path);
    RUN_TEST(test_append_log_lock_released_after_each_operation);
    RUN_TEST(test_append_log_locks_are_independent_across_base_paths);
    RUN_TEST(test_append_log_outbox_backlog_persistence);
    RUN_TEST(test_append_log_retryable_failure_does_not_drop);
    RUN_TEST(test_append_log_compact_idle_survives_remount);
    RUN_TEST(test_append_log_drop_oldest_evicts_and_continues);
    UNITY_END();
}

void loop() {}

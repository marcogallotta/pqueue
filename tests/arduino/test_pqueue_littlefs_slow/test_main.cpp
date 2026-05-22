#include <Arduino.h>
#include <LittleFS.h>
#include <unity.h>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "pqueue/outbox.h"
#include "pqueue/queue.h"
#include "pqueue/status.h"
#include "pqueue/types.h"

namespace {

constexpr const char* kBasePath  = "/pqueue_slow";
constexpr const char* kStatePath = "/pqueue_slow_state";

enum class SlowTest : std::uint8_t {
    FifoMany         = 1,
    PopRemaining     = 2,
    RewriteFront     = 3,
    OutboxDrain      = 4,
    CompactionReboot = 5,
    Churn            = 6,
    Done             = 255,
};

struct SlowState {
    SlowTest    test  = SlowTest::FifoMany;
    std::uint8_t phase = 0;
};

pqueue::Config appendLogQueueConfigForBase(const char* basePath) {
    pqueue::Config cfg;
    cfg.basePath       = basePath;
    cfg.storeLayout    = pqueue::StoreLayout::AppendLog;
    cfg.reservedBytes  = 0;
    cfg.minFreeBytes   = 0;
    cfg.maxSegmentBytes = 256;
    cfg.maxSegments    = 8;
    return cfg;
}

struct FakeSender {
    pqueue::SendDecision decision = pqueue::SendDecision::Sent;
    std::uint16_t calls = 0;
    std::string lastPayload;
    std::uint8_t lastAttempts = 0;
};

std::uint64_t fakeClock(void*) { return 0; }

pqueue::SendResult fakeSend(void* context, const std::string& payload, const pqueue::RetryState& retry) {
    auto* sender = static_cast<FakeSender*>(context);
    sender->calls += 1;
    sender->lastPayload  = payload;
    sender->lastAttempts = retry.attempts;
    return {sender->decision};
}

pqueue::OutboxConfig outboxConfig() {
    pqueue::OutboxConfig config;
    config.retryDelayMs             = 0;
    config.maxDrainAttemptsPerSecond = 1000;
    return config;
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
    String testLine  = file.readStringUntil('\n');
    String phaseLine = file.readStringUntil('\n');
    file.close();
    LittleFS.end();
    out.test  = static_cast<SlowTest>(std::atoi(testLine.c_str()));
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
    if (!mountLittleFs()) return;
    LittleFS.remove(kStatePath);
    LittleFS.end();
}

void rebootNow() {
    delay(200);
    ESP.restart();
    while (true) { delay(1000); }
}

void rebootTo(SlowTest test, std::uint8_t phase) {
    writeSlowState(test, phase);
    rebootNow();
}

std::string numbered(const char* prefix, int value) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%s%02d", prefix, value);
    return std::string(buf);
}

void expectQueueEmpty(pqueue::Queue& queue) {
    std::string out;
    const pqueue::Status st = queue.peek(out);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(pqueue::StatusCode::QueueEmpty), static_cast<int>(st.code));
}

void expectFrontAndPop(pqueue::Queue& queue, const std::string& expected) {
    std::string out;
    TEST_ASSERT_TRUE(queue.peek(out).ok());
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), out.c_str());
    TEST_ASSERT_TRUE(queue.pop().ok());
}

void expectDrain(pqueue::Queue& queue, const std::vector<std::string>& expected) {
    TEST_ASSERT_EQUAL_UINT32(static_cast<std::uint32_t>(expected.size()), queue.stats().count);
    for (const std::string& record : expected) {
        expectFrontAndPop(queue, record);
    }
    expectQueueEmpty(queue);
}

std::uint8_t phaseFor(SlowTest expected) {
    SlowState state;
    if (!readSlowState(state)) return 0;
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(expected), static_cast<std::uint8_t>(state.test));
    return state.phase;
}

void completeSlowTest() { clearSlowState(); }

// ---------------------------------------------------------------------------

void test_reboot_fifo_many() {
    const std::uint8_t phase = phaseFor(SlowTest::FifoMany);

    // maxSegmentBytes=128: 3-char payloads "a00" use 27 bytes/record;
    // 128-byte segments hold exactly 4 records, so 20 records span 5 sealed segments.
    pqueue::Config cfg = appendLogQueueConfigForBase(kBasePath);
    cfg.maxSegmentBytes = 128;

    if (phase == 0) {
        cleanLittleFs();
        {
            pqueue::Queue queue(cfg);
            for (int i = 0; i < 20; ++i) {
                TEST_ASSERT_TRUE(queue.enqueue(numbered("a", i)).ok());
            }
            TEST_ASSERT_EQUAL_UINT32(20, queue.stats().count);
        }
        rebootTo(SlowTest::FifoMany, 1);
    }

    TEST_ASSERT_EQUAL_UINT8(1, phase);
    {
        pqueue::Queue queue(cfg);
        std::vector<std::string> expected;
        for (int i = 0; i < 20; ++i) expected.push_back(numbered("a", i));
        expectDrain(queue, expected);
    }
    completeSlowTest();
}

void test_reboot_pop_remaining() {
    const std::uint8_t phase = phaseFor(SlowTest::PopRemaining);

    pqueue::Config cfg = appendLogQueueConfigForBase(kBasePath);
    cfg.maxSegmentBytes = 128;

    if (phase == 0) {
        cleanLittleFs();
        {
            pqueue::Queue queue(cfg);
            for (int i = 0; i < 10; ++i) {
                TEST_ASSERT_TRUE(queue.enqueue(numbered("p", i)).ok());
            }
            // Pop 4 records across the segment boundary (first segment holds 4 records).
            for (int i = 0; i < 4; ++i) {
                expectFrontAndPop(queue, numbered("p", i));
            }
            TEST_ASSERT_EQUAL_UINT32(6, queue.stats().count);
        }
        rebootTo(SlowTest::PopRemaining, 1);
    }

    TEST_ASSERT_EQUAL_UINT8(1, phase);
    {
        pqueue::Queue queue(cfg);
        std::vector<std::string> expected;
        for (int i = 4; i < 10; ++i) expected.push_back(numbered("p", i));
        expectDrain(queue, expected);
    }
    completeSlowTest();
}

void test_reboot_rewrite_front() {
    const std::uint8_t phase = phaseFor(SlowTest::RewriteFront);

    if (phase == 0) {
        cleanLittleFs();
        {
            pqueue::Queue queue(appendLogQueueConfigForBase(kBasePath));
            TEST_ASSERT_TRUE(queue.enqueue("old-front").ok());
            TEST_ASSERT_TRUE(queue.enqueue("tail").ok());
            TEST_ASSERT_TRUE(queue.rewriteFront("new-front").ok());
        }
        rebootTo(SlowTest::RewriteFront, 1);
    }

    TEST_ASSERT_EQUAL_UINT8(1, phase);
    {
        pqueue::Queue queue(appendLogQueueConfigForBase(kBasePath));
        expectFrontAndPop(queue, "new-front");
        expectFrontAndPop(queue, "tail");
        expectQueueEmpty(queue);
    }
    completeSlowTest();
}

void test_reboot_outbox_drain() {
    const std::uint8_t phase = phaseFor(SlowTest::OutboxDrain);

    if (phase == 0) {
        cleanLittleFs();
        {
            FakeSender retrying;
            retrying.decision = pqueue::SendDecision::RetryLater;
            pqueue::Outbox outbox(appendLogQueueConfigForBase(kBasePath), outboxConfig(),
                                  fakeSend, &retrying, fakeClock, nullptr);
            const pqueue::SubmitResult submitted = outbox.submit("outbox-reboot-payload");
            TEST_ASSERT_EQUAL_INT(static_cast<int>(pqueue::SubmitStatus::Queued),
                                  static_cast<int>(submitted.status));
            TEST_ASSERT_EQUAL_UINT16(1, retrying.calls);
            TEST_ASSERT_EQUAL_STRING("outbox-reboot-payload", retrying.lastPayload.c_str());
            TEST_ASSERT_EQUAL_UINT8(0, retrying.lastAttempts);
            TEST_ASSERT_EQUAL_UINT32(1, outbox.stats().count);
        }
        rebootTo(SlowTest::OutboxDrain, 1);
    }

    TEST_ASSERT_EQUAL_UINT8(1, phase);
    {
        FakeSender succeeding;
        succeeding.decision = pqueue::SendDecision::Sent;
        pqueue::Outbox outbox(appendLogQueueConfigForBase(kBasePath), outboxConfig(),
                              fakeSend, &succeeding, fakeClock, nullptr);
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

void test_reboot_compaction() {
    const std::uint8_t phase = phaseFor(SlowTest::CompactionReboot);

    // maxSegmentBytes=45: exactly one 1-byte record per segment
    // (20 seg header + 16 enqueue header + 1 payload + 8 trailer = 45).
    pqueue::Config cfg = appendLogQueueConfigForBase(kBasePath);
    cfg.maxSegmentBytes = 45;

    if (phase == 0) {
        cleanLittleFs();
        {
            pqueue::Queue queue(cfg);
            TEST_ASSERT_TRUE(queue.enqueue("A").ok());
            TEST_ASSERT_TRUE(queue.enqueue("B").ok());
            TEST_ASSERT_TRUE(queue.enqueue("C").ok());
            TEST_ASSERT_EQUAL_UINT32(3, queue.stats().count);
        }
        rebootTo(SlowTest::CompactionReboot, 1);
    }

    // Phase 1: pop A, rewrite B -> X, compactIdle -- all in one boot so that
    // activeTailDependenciesTracked_ is true when compaction runs and the active
    // segment (containing the rewrite event) is folded into the compaction range.
    if (phase == 1) {
        {
            pqueue::Queue queue(cfg);
            TEST_ASSERT_EQUAL_UINT32(3, queue.stats().count);
            TEST_ASSERT_TRUE(queue.pop().ok());              // drop A
            TEST_ASSERT_TRUE(queue.rewriteFront("X").ok());  // B -> X
            TEST_ASSERT_EQUAL_UINT32(2, queue.stats().count);
            const pqueue::CompactIdleResult result = queue.compactIdle(16);
            TEST_ASSERT_TRUE(result.status.ok());
            TEST_ASSERT_GREATER_THAN(0u, static_cast<std::uint32_t>(result.compactions));
        }
        rebootTo(SlowTest::CompactionReboot, 2);
    }

    TEST_ASSERT_EQUAL_UINT8(2, phase);
    {
        pqueue::Queue queue(cfg);
        expectFrontAndPop(queue, "X");
        expectFrontAndPop(queue, "C");
        expectQueueEmpty(queue);
    }
    completeSlowTest();
}

void test_churn_without_reboot() {
    cleanLittleFs();

    // maxSegmentBytes=128: 3-char payloads use 27 bytes/record, 4 records per segment.
    // Five rounds of enqueue/rewriteFront/pop/compactIdle exercise rotation and
    // compaction without a FixedSlot capacity cap.
    pqueue::Config cfg = appendLogQueueConfigForBase(kBasePath);
    cfg.maxSegmentBytes = 128;

    pqueue::Queue queue(cfg);
    std::vector<std::string> model;

    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 6; ++i) {
            const std::string rec = numbered("r", round * 6 + i);
            TEST_ASSERT_TRUE(queue.enqueue(rec).ok());
            model.push_back(rec);
        }
        if (!model.empty()) {
            const std::string rewritten = numbered("x", round);
            TEST_ASSERT_TRUE(queue.rewriteFront(rewritten).ok());
            model.front() = rewritten;
        }
        const int toPop = static_cast<int>(model.size()) / 2;
        for (int i = 0; i < toPop; ++i) {
            expectFrontAndPop(queue, model.front());
            model.erase(model.begin());
        }
        const pqueue::CompactIdleResult result = queue.compactIdle(4);
        TEST_ASSERT_TRUE(result.status.ok());
        TEST_ASSERT_EQUAL_UINT32(static_cast<std::uint32_t>(model.size()), queue.stats().count);
    }
    expectDrain(queue, model);
    completeSlowTest();
}

// ---------------------------------------------------------------------------

SlowTest nextSlowTest(SlowTest test) {
    switch (test) {
    case SlowTest::FifoMany:         return SlowTest::PopRemaining;
    case SlowTest::PopRemaining:     return SlowTest::RewriteFront;
    case SlowTest::RewriteFront:     return SlowTest::OutboxDrain;
    case SlowTest::OutboxDrain:      return SlowTest::CompactionReboot;
    case SlowTest::CompactionReboot: return SlowTest::Churn;
    case SlowTest::Churn:
    case SlowTest::Done:             return SlowTest::Done;
    }
    return SlowTest::Done;
}

SlowTest startingSlowTest() {
    SlowState state;
    if (readSlowState(state)) return state.test;
    return SlowTest::FifoMany;
}

bool slowTestStillInProgress(SlowTest test) {
    SlowState state;
    return readSlowState(state) && state.test == test;
}

void runSlowTest(SlowTest test) {
    switch (test) {
    case SlowTest::FifoMany:         RUN_TEST(test_reboot_fifo_many);      break;
    case SlowTest::PopRemaining:     RUN_TEST(test_reboot_pop_remaining);  break;
    case SlowTest::RewriteFront:     RUN_TEST(test_reboot_rewrite_front);  break;
    case SlowTest::OutboxDrain:      RUN_TEST(test_reboot_outbox_drain);   break;
    case SlowTest::CompactionReboot: RUN_TEST(test_reboot_compaction);     break;
    case SlowTest::Churn:            RUN_TEST(test_churn_without_reboot);  break;
    case SlowTest::Done:                                                    break;
    }
}

} // namespace

void setup() {
    delay(2000);
    UNITY_BEGIN();

    SlowTest current = startingSlowTest();
    while (current != SlowTest::Done) {
        runSlowTest(current);
        if (slowTestStillInProgress(current)) break;
        current = nextSlowTest(current);
    }

    clearSlowState();
    cleanLittleFs();
    UNITY_END();
}

void loop() {}

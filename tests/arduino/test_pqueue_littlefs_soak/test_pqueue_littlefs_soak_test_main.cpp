#include <Arduino.h>
#include <LittleFS.h>
#include <unity.h>

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "pqueue/queue.h"
#include "pqueue/status.h"
#include "pqueue/types.h"

// This soak covers LittleFS persistence and remount correctness under
// append-log enqueue/pop churn with segment rollover. It does not call
// compactIdle: compaction creates higher-generation output segments, and
// when the old active tail later rotates it becomes non-contiguous, which
// fragments manifest ranges under the current implementation. Append-log
// compaction is covered by focused POSIX tests. A dedicated LittleFS
// compaction/rewrite soak is deferred.

namespace {

constexpr const char* kBasePath   = "/pqueue_soak";
constexpr int         kCycles     = 30;
constexpr std::size_t kMaxBacklog = 8;

pqueue::Config queueConfig() {
    pqueue::Config cfg;
    cfg.basePath        = kBasePath;
    cfg.storageBackend  = pqueue::StorageBackend::LittleFS;
    cfg.reservedBytes   = 0;
    cfg.minFreeBytes    = 0;
    cfg.maxSegmentBytes = 128;
    cfg.maxSegments     = 16;
    return cfg;
}

void cleanLittleFs() {
    LittleFS.end();
    TEST_ASSERT_TRUE_MESSAGE(LittleFS.begin(true), "LittleFS mount failed");
    TEST_ASSERT_TRUE_MESSAGE(LittleFS.format(), "LittleFS format failed");
    LittleFS.end();
}

std::string numbered(const char* prefix, int value) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%s%03d", prefix, value);
    return std::string(buf);
}

void assertQueueEmpty(pqueue::Queue& queue) {
    std::string out;
    const pqueue::Status st = queue.peek(out);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(pqueue::StatusCode::QueueEmpty),
                          static_cast<int>(st.code));
}

void popFront(pqueue::Queue& queue, std::vector<std::string>& model) {
    std::string out;
    const pqueue::Status peek = queue.peek(out);
    TEST_ASSERT_TRUE_MESSAGE(peek.ok(), peek.message);
    TEST_ASSERT_EQUAL_STRING(model.front().c_str(), out.c_str());
    const pqueue::Status pop = queue.pop();
    TEST_ASSERT_TRUE_MESSAGE(pop.ok(), pop.message);
    model.erase(model.begin());
}

void runCycle(int cycle, int& nextRecord, std::vector<std::string>& model) {
    {
        pqueue::Queue queue(queueConfig());

        // Enqueue one record per cycle.
        {
            const std::string rec = numbered("s", nextRecord++);
            const pqueue::Status st = queue.enqueue(rec);
            TEST_ASSERT_TRUE_MESSAGE(st.ok(), st.message);
            model.push_back(rec);
        }

        // Pop to keep backlog bounded, ensuring segment rollover happens
        // without unbounded range accumulation.
        if (model.size() > kMaxBacklog) {
            popFront(queue, model);
        }

        // Occasional extra pop to vary the dead-byte pattern.
        if ((cycle % 3) == 0 && !model.empty()) {
            popFront(queue, model);
        }

        TEST_ASSERT_EQUAL_UINT32(static_cast<std::uint32_t>(model.size()),
                                 queue.stats().count);
    }

    // LittleFS remount: unmount and remount the filesystem, then verify state.
    LittleFS.end();
    TEST_ASSERT_TRUE_MESSAGE(LittleFS.begin(true), "LittleFS remount failed");
    {
        pqueue::Queue queue(queueConfig());
        TEST_ASSERT_EQUAL_UINT32(static_cast<std::uint32_t>(model.size()),
                                 queue.stats().count);
        if (!model.empty()) {
            std::string front;
            const pqueue::Status st = queue.peek(front);
            TEST_ASSERT_TRUE_MESSAGE(st.ok(), st.message);
            TEST_ASSERT_EQUAL_STRING(model.front().c_str(), front.c_str());
        }
    }
}

void test_model_driven_remount_soak() {
    cleanLittleFs();

    int nextRecord = 0;
    std::vector<std::string> model;

    for (int cycle = 0; cycle < kCycles; ++cycle) {
        runCycle(cycle, nextRecord, model);
    }

    // Final pass: validate, then full drain in FIFO order.
    {
        pqueue::Queue queue(queueConfig());
        const pqueue::ValidationResult validation = queue.validate();
        TEST_ASSERT_TRUE(validation.ok);
        TEST_ASSERT_EQUAL_UINT32(0, static_cast<std::uint32_t>(validation.errors.size()));
        TEST_ASSERT_EQUAL_UINT32(static_cast<std::uint32_t>(model.size()),
                                 queue.stats().count);
        for (const std::string& expected : model) {
            std::string out;
            {
                const pqueue::Status st = queue.peek(out);
                TEST_ASSERT_TRUE_MESSAGE(st.ok(), st.message);
            }
            TEST_ASSERT_EQUAL_STRING(expected.c_str(), out.c_str());
            {
                const pqueue::Status st = queue.pop();
                TEST_ASSERT_TRUE_MESSAGE(st.ok(), st.message);
            }
        }
        assertQueueEmpty(queue);
    }
}

} // namespace

void setup() {
    delay(2000);
    UNITY_BEGIN();
    RUN_TEST(test_model_driven_remount_soak);
    UNITY_END();
}

void loop() {}

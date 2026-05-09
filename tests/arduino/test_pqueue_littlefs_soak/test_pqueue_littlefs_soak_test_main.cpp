#include <Arduino.h>
#include <LittleFS.h>
#include <unity.h>

#include <cstdint>
#include <string>
#include <vector>

#include "pqueue/queue.h"
#include "pqueue/status.h"
#include "pqueue/storage_common.h"
#include "pqueue/types.h"

namespace {

constexpr const char* kBasePath = "/pqueue_soak";
constexpr std::uint32_t kCapacityRecords = 32;
constexpr std::size_t kRecordSizeBytes = 32;
constexpr int kCycles = 30;

std::uint32_t slotSize(std::size_t recordSizeBytes) {
    return static_cast<std::uint32_t>(sizeof(pqueue::storage_detail::RecordHeader) + recordSizeBytes);
}

pqueue::Config queueConfig() {
    pqueue::Config config;
    config.basePath = kBasePath;
    config.storageBackend = pqueue::StorageBackend::LittleFS;
    config.recordSizeBytes = kRecordSizeBytes;
    config.reservedBytes = slotSize(kRecordSizeBytes) * kCapacityRecords;
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

std::string numbered(const char* prefix, int value) {
    char buffer[24];
    snprintf(buffer, sizeof(buffer), "%s%03d", prefix, value);
    return std::string(buffer);
}

void modelPopFront(std::vector<std::string>& model) {
    TEST_ASSERT_FALSE_MESSAGE(model.empty(), "model unexpectedly empty");
    model.erase(model.begin());
}

void assertQueueEmpty(pqueue::Queue& queue) {
    std::string out;
    const pqueue::Status status = queue.peek(out);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(pqueue::StatusCode::QueueEmpty), static_cast<int>(status.code));
}

void verifyAndRestoreModel(const std::vector<std::string>& model) {
    TEST_ASSERT_TRUE_MESSAGE(mountLittleFs(), "LittleFS remount failed before verify");

    {
        pqueue::Queue queue(queueConfig());
        TEST_ASSERT_EQUAL_UINT32(model.size(), queue.stats().count);

        for (const std::string& expected : model) {
            std::string out;
            TEST_ASSERT_TRUE(queue.peek(out).ok());
            TEST_ASSERT_EQUAL_STRING(expected.c_str(), out.c_str());
            TEST_ASSERT_TRUE(queue.pop().ok());
        }
        assertQueueEmpty(queue);
    }

    {
        pqueue::Queue queue(queueConfig());
        for (const std::string& record : model) {
            TEST_ASSERT_TRUE(queue.enqueue(record).ok());
        }
        TEST_ASSERT_EQUAL_UINT32(model.size(), queue.stats().count);
    }

    LittleFS.end();
}

void runCycle(int cycle, int& nextRecord, std::vector<std::string>& model) {
    TEST_ASSERT_TRUE_MESSAGE(mountLittleFs(), "LittleFS remount failed before cycle");

    {
        pqueue::Queue queue(queueConfig());

        const int enqueueCount = 1 + (cycle % 4);
        for (int i = 0; i < enqueueCount && model.size() < kCapacityRecords; ++i) {
            const std::string record = numbered("s", nextRecord++);
            TEST_ASSERT_TRUE(queue.enqueue(record).ok());
            model.push_back(record);
        }

        const int popCount = cycle % 3;
        for (int i = 0; i < popCount && !model.empty(); ++i) {
            std::string out;
            TEST_ASSERT_TRUE(queue.peek(out).ok());
            TEST_ASSERT_EQUAL_STRING(model.front().c_str(), out.c_str());
            TEST_ASSERT_TRUE(queue.pop().ok());
            modelPopFront(model);
        }

        if ((cycle % 5) == 0 && !model.empty()) {
            const std::string rewritten = numbered("r", cycle);
            TEST_ASSERT_TRUE(queue.rewriteFront(rewritten).ok());
            model.front() = rewritten;
        }

        if (model.size() == kCapacityRecords) {
            const pqueue::Status full = queue.enqueue("overflow");
            TEST_ASSERT_EQUAL_INT(static_cast<int>(pqueue::StatusCode::QueueFull), static_cast<int>(full.code));
        }

        TEST_ASSERT_EQUAL_UINT32(model.size(), queue.stats().count);
    }

    LittleFS.end();
    verifyAndRestoreModel(model);
}

void test_model_driven_remount_soak() {
    cleanLittleFs();

    int nextRecord = 0;
    std::vector<std::string> model;
    model.reserve(kCapacityRecords);

    for (int cycle = 0; cycle < kCycles; ++cycle) {
        runCycle(cycle, nextRecord, model);
    }

    TEST_ASSERT_TRUE_MESSAGE(mountLittleFs(), "LittleFS final remount failed");
    {
        pqueue::Queue queue(queueConfig());
        const pqueue::ValidationResult validation = queue.validate();
        TEST_ASSERT_TRUE(validation.ok);
        TEST_ASSERT_EQUAL_UINT32(0, validation.errors.size());
        TEST_ASSERT_EQUAL_UINT32(model.size(), queue.stats().count);
    }
    LittleFS.end();
}

} // namespace

void setup() {
    delay(2000);
    UNITY_BEGIN();
    RUN_TEST(test_model_driven_remount_soak);
    UNITY_END();
}

void loop() {}

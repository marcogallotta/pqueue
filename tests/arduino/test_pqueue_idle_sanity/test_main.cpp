#include <Arduino.h>
#include <LittleFS.h>
#include <esp_timer.h>
#include <unity.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "pqueue/queue.h"
#include "pqueue/types.h"

// On-device sanity check: run the heavy idle-compaction workload on real
// LittleFS and compare worst compactIdle(1) step time against the calibrated
// POSIX sim prediction (burst=500/pop=90%/rec=492B/cycles=3).
//
// This is a rough measurement, not a precise benchmark. Serial prints and
// std::string payload construction run in the hot path and add overhead, but
// the gap between device and sim is large enough that neither affects the
// conclusion. No latency assertion -- this run produces the ratio to judge
// whether the sim's absolute latency claims are trustworthy.

namespace {

constexpr const char*  kBasePath        = "/pqueue_idle";
constexpr std::uint32_t kBurstSize      = 500;
constexpr float         kPopRatio       = 0.90f;
constexpr std::uint32_t kCycles         = 3;
constexpr double        kSimMaxStepMs   = 1175.915;

void formatLittleFs() {
    LittleFS.end();
    TEST_ASSERT_TRUE_MESSAGE(LittleFS.begin(true), "LittleFS begin failed");
    TEST_ASSERT_TRUE_MESSAGE(LittleFS.format(),    "LittleFS format failed");
    LittleFS.mkdir(kBasePath);
    LittleFS.end();
}

std::string makePayload(std::uint32_t seq) {
    char buf[493];
    snprintf(buf, sizeof(buf), "r%09u", seq);
    memset(buf + 10, 'x', 492 - 10);
    buf[492] = '\0';
    return std::string(buf, 492);
}

void test_compactIdle_latency() {
    formatLittleFs();

    pqueue::Config cfg;
    cfg.basePath        = kBasePath;
    cfg.storageBackend  = pqueue::StorageBackend::LittleFS;
    cfg.reservedBytes   = 2108736;
    cfg.recordSizeBytes = 492;
    cfg.maxSegments     = 200;
    cfg.minFreeBytes    = 0;

    pqueue::Queue q(cfg);
    TEST_ASSERT_TRUE_MESSAGE(q.statsResult().status.ok(), "Queue mount failed");

    std::uint32_t nextSeq        = 1;
    std::uint32_t queueSize      = 0;
    std::uint32_t productiveSteps = 0;
    std::uint32_t noopSteps      = 0;
    std::uint64_t maxStepUs      = 0;
    std::uint64_t totalIdleUs    = 0;

    for (std::uint32_t cycle = 0; cycle < kCycles; ++cycle) {
        Serial.printf("[cycle %u] enqueue %u...\n", cycle, kBurstSize);
        Serial.flush();

        // Phase 1: enqueue burst.
        for (std::uint32_t i = 0; i < kBurstSize; ++i) {
            const auto st = q.enqueue(makePayload(nextSeq));
            TEST_ASSERT_TRUE_MESSAGE(st.ok(), "enqueue failed");
            ++nextSeq;
            ++queueSize;
            if ((i + 1) % 100 == 0) {
                Serial.printf("  enqueued %u/%u\n", i + 1, kBurstSize);
                Serial.flush();
            }
            yield();
        }

        const std::uint32_t toPop =
            static_cast<std::uint32_t>(static_cast<float>(queueSize) * kPopRatio);
        Serial.printf("[cycle %u] pop %u...\n", cycle, toPop);
        Serial.flush();

        // Phase 2: drain.
        std::string out;
        for (std::uint32_t i = 0; i < toPop; ++i) {
            TEST_ASSERT_TRUE_MESSAGE(q.peek(out).ok(), "peek failed");
            TEST_ASSERT_TRUE_MESSAGE(q.pop().ok(),     "pop failed");
            --queueSize;
            if ((i + 1) % 100 == 0) {
                Serial.printf("  popped %u/%u\n", i + 1, toPop);
                Serial.flush();
            }
            yield();
        }

        Serial.printf("[cycle %u] compact...\n", cycle);
        Serial.flush();

        // Phase 3: compact to completion.
        for (;;) {
            const std::uint64_t t0 = static_cast<std::uint64_t>(esp_timer_get_time());
            const auto cr = q.compactIdle(1);
            const std::uint64_t dt = static_cast<std::uint64_t>(esp_timer_get_time()) - t0;
            if (!cr.status.ok()) {
                char msg[64];
                snprintf(msg, sizeof(msg), "compactIdle failed: %s",
                    pqueue::statusCodeName(cr.status.code));
                TEST_FAIL_MESSAGE(msg);
                break;
            }
            if (cr.compactions > 0) {
                ++productiveSteps;
                totalIdleUs += dt;
                if (dt > maxStepUs) maxStepUs = dt;
                Serial.printf("  compact step %u: %.1fms\n",
                    productiveSteps, static_cast<double>(dt) / 1000.0);
                Serial.flush();
            } else {
                ++noopSteps;
                Serial.printf("  compact noop\n");
                Serial.flush();
                break;
            }
            yield();
        }

        Serial.printf("[cycle %u] q=%u productive=%u noops=%u maxStep=%.1fms\n",
            cycle, queueSize, productiveSteps, noopSteps,
            static_cast<double>(maxStepUs) / 1000.0);
        Serial.flush();
    }

    const double maxStepMs  = static_cast<double>(maxStepUs)  / 1000.0;
    const double totalIdleMs = static_cast<double>(totalIdleUs) / 1000.0;
    const double ratio = maxStepMs / kSimMaxStepMs;

    Serial.printf("\n=== idle compaction latency ===\n");
    Serial.printf("maxStepMs=%.1f\n",     maxStepMs);
    Serial.printf("totalIdleMs=%.1f\n",   totalIdleMs);
    Serial.printf("productiveSteps=%u\n", productiveSteps);
    Serial.printf("noopSteps=%u\n",       noopSteps);
    Serial.printf("simMaxStepMs=%.3f\n",  kSimMaxStepMs);
    Serial.printf("ratio=%.2fx\n",        ratio);
    Serial.flush();

    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(
        500, nextSeq - 1,
        "fewer than one burst of enqueues succeeded -- workload did not run");
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(
        0, productiveSteps,
        "no productive compaction steps -- workload did not exercise compaction");
}

} // namespace

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("boot");
    Serial.flush();
    UNITY_BEGIN();
    RUN_TEST(test_compactIdle_latency);
    UNITY_END();
}

void loop() {}

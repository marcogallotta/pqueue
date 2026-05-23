#include <Arduino.h>
#include <LittleFS.h>
#include <esp_timer.h>
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "pqueue/queue.h"
#include "pqueue/types.h"

// On-device benchmark: application-visible Queue latency on real LittleFS.
// Uses the std::string Queue API — results reflect string-path overhead, not
// the raw-buffer hot path. All timing via esp_timer_get_time(). No Serial
// calls inside timed regions. Output: "bench " key=value lines.

namespace {

constexpr const char*   kBasePath  = "/pqueue_bench";
constexpr std::uint32_t kEnqN      = 100;
constexpr std::uint32_t kPpN       = 100;
constexpr std::uint32_t kBurst     = 100;
constexpr float         kPopRatio  = 0.90f;
constexpr std::uint32_t kCycles    = 2;

static void formatFs() {
    LittleFS.end();
    TEST_ASSERT_TRUE_MESSAGE(LittleFS.begin(true), "LittleFS begin failed");
    TEST_ASSERT_TRUE_MESSAGE(LittleFS.format(),    "LittleFS format failed");
    LittleFS.mkdir(kBasePath);
    LittleFS.end();
}

static std::string makePayload(std::uint32_t b, std::uint32_t seq) {
    std::string s(b, 'x');
    char h[11];
    snprintf(h, sizeof(h), "r%09u", seq);
    s.replace(0, 10, h, 10);
    return s;
}

static pqueue::Config makeConfig(std::uint32_t recordSizeBytes) {
    pqueue::Config c;
    c.basePath        = kBasePath;
    c.storageBackend  = pqueue::StorageBackend::LittleFS;
    c.recordSizeBytes = recordSizeBytes;
    c.maxSegments     = 200;
    c.reservedBytes   = 2108736;
    c.minFreeBytes    = 0;
    return c;
}

static std::uint64_t pctile(const std::vector<std::uint64_t>& v, double p) {
    std::size_t i = static_cast<std::size_t>(p / 100.0 * (v.size() - 1) + 0.5);
    return v[std::min(i, v.size() - 1)];
}

static void printOpStats(const char* scenario, std::uint32_t payloadB,
                         std::uint32_t n, std::vector<std::uint64_t> s) {
    std::sort(s.begin(), s.end());
    Serial.printf(
        "bench scenario=%s payload_b=%u n=%u"
        " p50_us=%llu p90_us=%llu p99_us=%llu max_us=%llu\n",
        scenario, payloadB, n,
        (unsigned long long)pctile(s, 50),
        (unsigned long long)pctile(s, 90),
        (unsigned long long)pctile(s, 99),
        (unsigned long long)s.back());
    Serial.flush();
}

static void runEnqueue(std::uint32_t payloadB) {
    formatFs();
    pqueue::Queue q(makeConfig(payloadB));
    TEST_ASSERT_TRUE_MESSAGE(q.statsResult().status.ok(), "mount failed");

    std::vector<std::string> pls;
    pls.reserve(kEnqN);
    for (std::uint32_t i = 0; i < kEnqN; ++i)
        pls.push_back(makePayload(payloadB, i + 1));

    std::vector<std::uint64_t> s;
    s.reserve(kEnqN);
    for (std::uint32_t i = 0; i < kEnqN; ++i) {
        const std::int64_t t0 = esp_timer_get_time();
        const auto st = q.enqueue(pls[i]);
        s.push_back(static_cast<std::uint64_t>(esp_timer_get_time() - t0));
        TEST_ASSERT_TRUE_MESSAGE(st.ok(), "enqueue failed");
        yield();
    }
    printOpStats("enqueue", payloadB, kEnqN, std::move(s));
}

static void runPeekPop(std::uint32_t payloadB) {
    formatFs();
    pqueue::Queue q(makeConfig(payloadB));
    TEST_ASSERT_TRUE_MESSAGE(q.statsResult().status.ok(), "mount failed");

    std::vector<std::string> pls;
    pls.reserve(kPpN);
    for (std::uint32_t i = 0; i < kPpN; ++i)
        pls.push_back(makePayload(payloadB, i + 1));

    for (std::uint32_t i = 0; i < kPpN; ++i) {
        TEST_ASSERT_TRUE_MESSAGE(q.enqueue(pls[i]).ok(), "setup enqueue failed");
        yield();
    }

    std::vector<std::uint64_t> s;
    s.reserve(kPpN);
    std::string out;
    for (std::uint32_t i = 0; i < kPpN; ++i) {
        const std::int64_t t0 = esp_timer_get_time();
        const auto ps = q.peek(out);
        const auto pp = q.pop();
        s.push_back(static_cast<std::uint64_t>(esp_timer_get_time() - t0));
        TEST_ASSERT_TRUE_MESSAGE(ps.ok(), "peek failed");
        TEST_ASSERT_TRUE_MESSAGE(pp.ok(), "pop failed");
        yield();
    }
    printOpStats("peek_pop", payloadB, kPpN, std::move(s));
}

// Preloads n records at 256 B, unmounts, then times a fresh mount.
// n=1000 takes ~30 s to preload; progress is printed every 100 records.
static void runMountCase(std::uint32_t n) {
    formatFs();
    {
        pqueue::Queue preload(makeConfig(256));
        TEST_ASSERT_TRUE_MESSAGE(preload.statsResult().status.ok(), "preload mount failed");
        for (std::uint32_t i = 0; i < n; ++i) {
            TEST_ASSERT_TRUE_MESSAGE(
                preload.enqueue(makePayload(256, i + 1)).ok(), "preload enqueue failed");
            if ((i + 1) % 100 == 0) {
                Serial.printf("  preloaded %u/%u\n", i + 1, n);
                Serial.flush();
            }
            yield();
        }
    }

    pqueue::Queue q(makeConfig(256));
    const std::int64_t t0 = esp_timer_get_time();
    const auto sr = q.statsResult();
    const std::uint64_t dt = static_cast<std::uint64_t>(esp_timer_get_time() - t0);
    TEST_ASSERT_TRUE_MESSAGE(sr.status.ok(), "remount failed");

    Serial.printf("bench scenario=mount payload_b=256 preload=%u mount_us=%llu\n",
        n, (unsigned long long)dt);
    Serial.flush();
}

static void runCompactIdle(std::uint32_t payloadB) {
    formatFs();
    pqueue::Queue q(makeConfig(payloadB));
    TEST_ASSERT_TRUE_MESSAGE(q.statsResult().status.ok(), "mount failed");

    std::vector<std::string> burstPls;
    burstPls.reserve(kBurst);
    for (std::uint32_t i = 0; i < kBurst; ++i)
        burstPls.push_back(makePayload(payloadB, i + 1));

    std::uint32_t qSz       = 0;
    std::uint32_t prodTotal = 0;
    std::uint32_t noopTotal = 0;
    std::uint64_t totalUs   = 0;
    std::vector<std::uint64_t> stepSamples;

    for (std::uint32_t cy = 0; cy < kCycles; ++cy) {
        for (std::uint32_t i = 0; i < kBurst; ++i) {
            TEST_ASSERT_TRUE_MESSAGE(
                q.enqueue(burstPls[i]).ok(), "enqueue failed");
            ++qSz;
            yield();
        }

        const std::uint32_t toPop =
            static_cast<std::uint32_t>(static_cast<float>(qSz) * kPopRatio);
        std::string out;
        for (std::uint32_t i = 0; i < toPop; ++i) {
            TEST_ASSERT_TRUE_MESSAGE(q.peek(out).ok(), "peek failed");
            TEST_ASSERT_TRUE_MESSAGE(q.pop().ok(),     "pop failed");
            --qSz;
            yield();
        }

        for (;;) {
            const std::int64_t t0 = esp_timer_get_time();
            const auto cr = q.compactIdle(1);
            const std::uint64_t dt = static_cast<std::uint64_t>(esp_timer_get_time() - t0);
            TEST_ASSERT_TRUE_MESSAGE(cr.status.ok(), "compactIdle failed");

            if (cr.compactions > 0) {
                ++prodTotal;
                totalUs += dt;
                stepSamples.push_back(dt);
                Serial.printf(
                    "bench scenario=compact_idle payload_b=%u cycle=%u step=%u dt_us=%llu\n",
                    payloadB, cy, prodTotal, (unsigned long long)dt);
                Serial.flush();
            } else {
                ++noopTotal;
                break;
            }
            yield();
        }
    }

    std::sort(stepSamples.begin(), stepSamples.end());
    const bool hasSamples = !stepSamples.empty();
    Serial.printf(
        "bench scenario=compact_idle payload_b=%u summary"
        " cycles=%u productive=%u noops=%u"
        " p50_us=%llu p90_us=%llu p99_us=%llu max_us=%llu total_us=%llu\n",
        payloadB, kCycles, prodTotal, noopTotal,
        (unsigned long long)(hasSamples ? pctile(stepSamples, 50) : 0),
        (unsigned long long)(hasSamples ? pctile(stepSamples, 90) : 0),
        (unsigned long long)(hasSamples ? pctile(stepSamples, 99) : 0),
        (unsigned long long)(hasSamples ? stepSamples.back()      : 0),
        (unsigned long long)totalUs);
    Serial.flush();

    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(
        0, prodTotal, "no productive compaction steps -- workload did not exercise compaction");
}

} // namespace

void test_enqueue_256b()      { runEnqueue(256); }
void test_enqueue_1kb()       { runEnqueue(1024); }
void test_peek_pop_256b()     { runPeekPop(256); }
void test_peek_pop_1kb()      { runPeekPop(1024); }
void test_compact_idle_256b() { runCompactIdle(256); }

void test_mount() {
    runMountCase(0);
    runMountCase(50);
    runMountCase(200);
    runMountCase(1000);
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("=== pqueue on-device benchmark ===");
    Serial.printf(
        "bench config reserved_bytes=%u max_segments=%u"
        " enq_n=%u pp_n=%u burst=%u pop_ratio=%.2f cycles=%u\n",
        2108736u, 200u, kEnqN, kPpN, kBurst, (double)kPopRatio, kCycles);
    Serial.flush();
    UNITY_BEGIN();
    RUN_TEST(test_enqueue_256b);
    RUN_TEST(test_enqueue_1kb);
    RUN_TEST(test_peek_pop_256b);
    RUN_TEST(test_peek_pop_1kb);
    RUN_TEST(test_mount);
    RUN_TEST(test_compact_idle_256b);
    UNITY_END();
}

void loop() {}

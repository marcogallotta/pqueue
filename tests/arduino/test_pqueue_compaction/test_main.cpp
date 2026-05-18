#include <Arduino.h>
#include <LittleFS.h>
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "pqueue/append_log_common.h"
#include "pqueue/append_log_store.h"
#include "pqueue/file_store.h"
#include "pqueue/status.h"

// On-device compaction validation.
//
// Drives AppendLogStore directly with an external HighestDeadRatio strategy
// and rising-edge trigger, matching the sim setup. Measures compactRange()
// latency and output segment count on real LittleFS hardware, and verifies
// all live records are readable after the workload.
//
// Workload: burst=100/pop=90%/rec=150B -- the lightest passing burst scenario
// from the real-world sim run.

namespace {

constexpr const char* kBasePath   = "/pqueue_compact";
constexpr std::uint32_t kMaxSegmentBytes   = 4096;
constexpr std::uint32_t kMaxSegments       = 200;  // disables internal auto-compaction
constexpr std::uint32_t kMaxTotalBytes     = 512 * 1024;
constexpr std::uint32_t kBurstSize         = 100;
constexpr float         kPopRatio          = 0.90f;
constexpr std::uint32_t kCycles            = 5;
constexpr float         kDeadRatioTrigger  = 0.25f;
constexpr std::uint32_t kRangePressureTrigger = 3;
constexpr std::size_t   kRecordSize        = 150;

// Acceptable stall per compactRange() call. At 45ms/segment and MaxOutSeg=9
// (sim result for this workload), 450ms is the expected worst case.
// Allow 3x headroom for LittleFS variance.
constexpr std::uint32_t kMaxAcceptableLatencyMs = 1500;

void formatAndUnmountLittleFs() {
    LittleFS.end();
    TEST_ASSERT_TRUE_MESSAGE(LittleFS.begin(true),   "LittleFS mount failed");
    TEST_ASSERT_TRUE_MESSAGE(LittleFS.format(),      "LittleFS format failed");
    LittleFS.end();
}

std::string makePayload(std::uint32_t seq) {
    char buf[kRecordSize + 1];
    snprintf(buf, sizeof(buf), "r%09u", seq);
    memset(buf + 10, 'x', kRecordSize - 10);
    buf[kRecordSize] = '\0';
    return std::string(buf, kRecordSize);
}

struct RangeStat {
    pqueue::AppendLogStore::CompactionRange range;
    std::uint32_t totalBytes = 0;
    std::uint32_t liveBytes  = 0;
    std::uint32_t deadBytes() const {
        return totalBytes > liveBytes ? totalBytes - liveBytes : 0;
    }
    float deadRatio() const {
        return totalBytes > 0
            ? static_cast<float>(deadBytes()) / static_cast<float>(totalBytes)
            : 0.0f;
    }
};

std::vector<RangeStat> buildRangeStats(const pqueue::AppendLogStore& store) {
    const auto& ranges   = store.manifestRanges();
    const auto  segStats = store.segmentStats();
    std::vector<RangeStat> result;
    for (const auto& r : ranges) {
        RangeStat rs;
        rs.range = {r.startGen, r.endGen};
        for (const auto& ss : segStats) {
            if (ss.generation >= r.startGen && ss.generation <= r.endGen) {
                rs.totalBytes += ss.totalBytes;
                rs.liveBytes  += ss.liveBytes;
            }
        }
        result.push_back(rs);
    }
    return result;
}

std::optional<pqueue::AppendLogStore::CompactionRange> chooseHighestDeadRatio(
    const std::vector<RangeStat>& stats
) {
    if (stats.empty()) return std::nullopt;
    auto best = std::max_element(stats.begin(), stats.end(),
        [](const RangeStat& a, const RangeStat& b) {
            return a.deadRatio() < b.deadRatio();
        });
    if (best->deadBytes() == 0) return std::nullopt;
    return best->range;
}

void test_compaction_burst_workload() {
    Serial.flush();
    formatAndUnmountLittleFs();

    pqueue::AppendLogConfig cfg;
    cfg.basePath        = kBasePath;
    cfg.backend         = pqueue::StorageBackend::LittleFS;
    cfg.maxSegmentBytes = kMaxSegmentBytes;
    cfg.maxSegments     = kMaxSegments;
    cfg.maxTotalBytes   = kMaxTotalBytes;
    cfg.minFreeBytes    = 0;

    pqueue::AppendLogStore store(cfg);
    TEST_ASSERT_TRUE_MESSAGE(store.mount().ok(), "AppendLogStore mount failed");

    std::uint32_t nextSeq       = 1;
    std::uint32_t queueSize     = 0;
    std::uint32_t deadlocks     = 0;
    std::uint32_t capExhausted  = 0;
    std::uint32_t compactions   = 0;
    std::uint32_t noOps         = 0;
    std::uint32_t maxOutSegs    = 0;
    std::uint32_t maxLatencyMs  = 0;
    std::uint32_t prevSegCount = 0;

    auto tryCompact = [&]() {
        const auto rangeStats = buildRangeStats(store);
        const auto chosen = chooseHighestDeadRatio(rangeStats);
        if (!chosen) return;

        ++compactions;
        std::uint32_t outSegs = 0;
        const std::uint32_t t0 = millis();
        const auto st = store.compactRange(*chosen, &outSegs);
        const std::uint32_t elapsed = millis() - t0;

        if (st.ok() && !st.isNoOp()) {
            maxOutSegs   = std::max(maxOutSegs, outSegs);
            maxLatencyMs = std::max(maxLatencyMs, elapsed);
            Serial.printf("[compact] outSegs=%u latency=%ums\n", outSegs, elapsed);
        } else {
            ++noOps;
        }
    };

    auto checkAndCompact = [&]() {
        const std::uint32_t segCount =
            static_cast<std::uint32_t>(store.segmentStats().size());
        const std::uint32_t rangeCount =
            static_cast<std::uint32_t>(store.manifestRanges().size());
        const bool newSeg  = segCount > prevSegCount;
        const bool pressure = rangeCount >= kRangePressureTrigger;
        if (newSeg || pressure) {
            bool useful = pressure;
            if (!useful) {
                for (const auto& rs : buildRangeStats(store)) {
                    if (rs.deadRatio() >= kDeadRatioTrigger) { useful = true; break; }
                }
            }
            if (useful) tryCompact();
        }
        prevSegCount = segCount;
    };

    for (std::uint32_t cycle = 0; cycle < kCycles; ++cycle) {
        for (std::uint32_t i = 0; i < kBurstSize; ++i) {
            const auto st = store.writeRecord(nextSeq, makePayload(nextSeq));
            if (st.ok()) {
                pqueue::FileStoreIndex dummy;
                store.writeIndex(dummy);
                ++nextSeq;
                ++queueSize;
            } else {
                bool reclaimable = false;
                for (const auto& rs : buildRangeStats(store)) {
                    if (rs.deadRatio() >= 0.01f) { reclaimable = true; break; }
                }
                if (reclaimable) ++deadlocks;
                else ++capExhausted;
            }
            checkAndCompact();
        }

        const std::uint32_t toPop =
            static_cast<std::uint32_t>(static_cast<float>(queueSize) * kPopRatio);
        for (std::uint32_t i = 0; i < toPop; ++i) {
            pqueue::FileStoreIndex idx;
            if (store.readIndex(idx).ok() && idx.count > 0) {
                idx.head++;
                idx.count--;
                store.writeIndex(idx);
                --queueSize;
            }
            checkAndCompact();
        }

        Serial.printf("[cycle %2u] queueSize=%u ranges=%u\n",
            cycle, queueSize,
            static_cast<unsigned>(store.manifestRanges().size()));
        Serial.flush();
    }

    // Verify all remaining live records are readable and have correct payloads.
    {
        pqueue::FileStoreIndex idx;
        TEST_ASSERT_TRUE_MESSAGE(store.readIndex(idx).ok(), "readIndex failed");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(queueSize, idx.count,
            "index count mismatch after workload");
        for (std::uint32_t i = 0; i < idx.count; ++i) {
            std::string out;
            const auto st = store.readRecord(idx.head + i, out);
            TEST_ASSERT_TRUE_MESSAGE(st.ok(), "readRecord failed after compaction");
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(
                kRecordSize, static_cast<std::uint32_t>(out.size()),
                "record size mismatch after compaction");
            const std::string expected = makePayload(idx.head + i);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(
                expected.c_str(), out.c_str(),
                "record payload mismatch after compaction");
        }
    }

    Serial.printf("\n=== Compaction validation results ===\n");
    Serial.flush();
    Serial.printf("Workload: burst=%u pop=%.0f%% rec=%uB cycles=%u\n",
        kBurstSize, kPopRatio * 100.0f, static_cast<unsigned>(kRecordSize), kCycles);
    Serial.printf("Compactions: %u  NoOps: %u  MaxOutSegs: %u\n",
        compactions, noOps, maxOutSegs);
    Serial.printf("MaxLatency: %u ms  (45ms/seg estimate: %u ms at MaxOutSegs=%u)\n",
        maxLatencyMs, maxOutSegs * 45, maxOutSegs);
    Serial.printf("Deadlocks: %u  CapExhausted: %u  FinalQueueSize: %u\n",
        deadlocks, capExhausted, queueSize);

    // Guard: catch vacuous passes where all enqueues silently failed.
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(
        kBurstSize, nextSeq - 1,
        "fewer enqueues than one burst succeeded -- workload did not run");
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(
        0, compactions,
        "no compactions fired -- workload did not exercise compaction path");

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, deadlocks, "true deadlocks occurred");
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(
        kMaxAcceptableLatencyMs, maxLatencyMs,
        "compaction stall exceeded acceptable threshold");

    LittleFS.end();
}

} // namespace

void setup() {
    Serial.begin(115200);
    delay(2000);
    UNITY_BEGIN();
    RUN_TEST(test_compaction_burst_workload);
    UNITY_END();
}

void loop() {}

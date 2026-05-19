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
// Workload: burst=500/pop=90%/rec=492B -- the worst-case passing burst scenario
// from the real-world sim run. Sim predicts MaxOutSegs up to 60. Goal is to
// measure actual stall duration on real LittleFS hardware before committing to
// trigger thresholds. kMaxAcceptableLatencyMs is set generously; tune after
// this run produces real data.

namespace {

constexpr const char* kBasePath   = "/pqueue_compact";
constexpr std::uint32_t kMaxSegmentBytes   = 4096;
constexpr std::uint32_t kMaxSegments       = 200;  // disables internal auto-compaction
constexpr std::uint32_t kMaxTotalBytes     = 1024 * 1024;
constexpr std::uint32_t kBurstSize         = 500;
constexpr float         kPopRatio          = 0.90f;
constexpr std::uint32_t kCycles            = 15;
constexpr float         kDeadRatioTrigger  = 0.10f;
constexpr std::uint32_t kRangePressureTrigger = 3;
constexpr std::size_t   kRecordSize        = 492;
constexpr std::uint32_t kMaxOutputSegs     = 8;

// Generous upper bound -- this run is for measurement, not enforcement.
// The previous run (burst=100/rec=150) measured up to 1090ms at MaxOutSegs=1.
// Sim predicts up to MaxOutSegs=60 for this workload; at ~500-1100ms per pass
// the true worst case could be tens of seconds.
constexpr std::uint32_t kMaxAcceptableLatencyMs = 120000;

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
    std::uint32_t totalBytes    = 0;
    std::uint32_t liveBytes     = 0;
    std::uint32_t inputSegCount = 0;
    std::uint32_t deadBytes() const {
        return totalBytes > liveBytes ? totalBytes - liveBytes : 0;
    }
    float deadRatio() const {
        return totalBytes > 0
            ? static_cast<float>(deadBytes()) / static_cast<float>(totalBytes)
            : 0.0f;
    }
    std::uint32_t predictedOutputSegs() const {
        if (liveBytes == 0) return 0;
        return (liveBytes + kMaxSegmentBytes - 1) / kMaxSegmentBytes;
    }
    bool wouldConsolidate() const {
        return predictedOutputSegs() < inputSegCount;
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
                rs.totalBytes    += ss.totalBytes;
                rs.liveBytes     += ss.liveBytes;
                rs.inputSegCount += 1;
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

// If the chosen range would produce more than kMaxOutputSegs output segments,
// finds the contiguous window of input segments with the highest dead ratio
// that fits within the budget. Single-segment windows always fit, so this
// always returns a valid (possibly single-segment) subrange.
pqueue::AppendLogStore::CompactionRange narrowRange(
    const pqueue::AppendLogStore::CompactionRange& range,
    const RangeStat& rs,
    const pqueue::AppendLogStore& store
) {
    if (rs.predictedOutputSegs() <= kMaxOutputSegs) return range;

    const auto allSegs = store.segmentStats();
    std::vector<pqueue::AppendLogStore::SegmentStat> segs;
    for (const auto& ss : allSegs) {
        if (ss.generation >= range.startGen && ss.generation <= range.endGen)
            segs.push_back(ss);
    }
    std::sort(segs.begin(), segs.end(),
        [](const auto& a, const auto& b) { return a.generation < b.generation; });

    pqueue::AppendLogStore::CompactionRange best = {segs.front().generation, segs.front().generation};
    float bestDeadRatio = -1.0f;

    for (std::size_t i = 0; i < segs.size(); ++i) {
        std::uint32_t liveBytes = 0, totalBytes = 0;
        for (std::size_t j = i; j < segs.size(); ++j) {
            liveBytes  += segs[j].liveBytes;
            totalBytes += segs[j].totalBytes;
            const std::uint32_t outSegs = liveBytes == 0 ? 0u
                : (liveBytes + kMaxSegmentBytes - 1) / kMaxSegmentBytes;
            if (outSegs > kMaxOutputSegs) break;
            const float dr = totalBytes > 0
                ? static_cast<float>(totalBytes - liveBytes) / static_cast<float>(totalBytes)
                : 0.0f;
            if (dr > bestDeadRatio) {
                bestDeadRatio = dr;
                best = {segs[i].generation, segs[j].generation};
            }
        }
    }

    return best;
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
    std::uint32_t prevSegCount  = 0;
    std::uint32_t prevRangeCount = 0;

    auto tryCompact = [&]() {
        const auto rangeStats = buildRangeStats(store);
        const auto chosen = chooseHighestDeadRatio(rangeStats);
        if (!chosen) return;

        const RangeStat* chosenStat = nullptr;
        for (const auto& rs : rangeStats) {
            if (rs.range.startGen == chosen->startGen && rs.range.endGen == chosen->endGen) {
                chosenStat = &rs;
                break;
            }
        }
        const auto target = chosenStat ? narrowRange(*chosen, *chosenStat, store) : *chosen;
        if (target.startGen != chosen->startGen || target.endGen != chosen->endGen) {
            Serial.printf("[narrow] [%u,%u]->>[%u,%u]\n",
                chosen->startGen, chosen->endGen, target.startGen, target.endGen);
        }

        ++compactions;
        std::uint32_t outSegs = 0;
        const std::uint32_t t0 = millis();
        const auto st = store.compactRange(target, &outSegs);
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
        // Logical count: sealed gens from manifest + 1 for active tail.
        // Avoids fileSize() per generation that segmentStats() would trigger.
        const auto& ranges = store.manifestRanges();
        std::uint32_t logicalSegCount = 1; // active tail always exists
        for (const auto& r : ranges) logicalSegCount += r.endGen - r.startGen + 1;
        const std::uint32_t rangeCount = static_cast<std::uint32_t>(ranges.size());
        const bool changed  = logicalSegCount != prevSegCount || rangeCount != prevRangeCount;
        const bool pressure = rangeCount >= kRangePressureTrigger;
        if (changed) {
            bool useful = false;
            for (const auto& rs : buildRangeStats(store)) {
                if (rs.deadRatio() >= kDeadRatioTrigger) { useful = true; break; }
                if (pressure && rs.wouldConsolidate())   { useful = true; break; }
            }
            if (useful) tryCompact();
        }
        prevSegCount  = logicalSegCount;
        prevRangeCount = rangeCount;
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
    Serial.printf("MaxLatency: %u ms  MaxOutSegs=%u\n",
        maxLatencyMs, maxOutSegs);
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

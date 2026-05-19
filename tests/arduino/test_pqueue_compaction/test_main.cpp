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
#include "counting_file_system.h"

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

    auto countingFs = std::make_shared<CountingFileSystem>(pqueue::makeLittleFsFileSystem());

    pqueue::AppendLogConfig cfg;
    cfg.basePath        = kBasePath;
    cfg.backend         = pqueue::StorageBackend::LittleFS;
    cfg.maxSegmentBytes = kMaxSegmentBytes;
    cfg.maxSegments     = kMaxSegments;
    cfg.maxTotalBytes   = kMaxTotalBytes;
    cfg.minFreeBytes    = 0;
    cfg.fileSystem      = countingFs;

    pqueue::AppendLogStore store(cfg);
    TEST_ASSERT_TRUE_MESSAGE(store.mount().ok(), "AppendLogStore mount failed");

    std::uint32_t nextSeq        = 1;
    std::uint32_t queueSize      = 0;
    std::uint32_t deadlocks      = 0;
    std::uint32_t capExhausted   = 0;
    std::uint32_t compactions    = 0;
    std::uint32_t noOps          = 0;
    std::uint32_t maxOutSegs     = 0;
    std::uint32_t maxLatencyMs   = 0;
    std::uint32_t prevSegCount   = 0;
    std::uint32_t prevRangeCount = 0;

    // Per-cycle accumulators (ms).
    std::uint32_t t_wr = 0, t_widx = 0, t_ridx = 0;
    std::uint32_t t_check = 0, t_stats = 0, t_compact = 0;
    std::uint32_t t_enq = 0, t_pop = 0;

    auto checkAndCompact = [&]() {
        // Step 1: logical count (in-memory, no I/O).
        const std::uint32_t t0_count = millis();
        const auto& ranges = store.manifestRanges();
        std::uint32_t logicalSegCount = 1;
        for (const auto& r : ranges) logicalSegCount += r.endGen - r.startGen + 1;
        const std::uint32_t rangeCount = static_cast<std::uint32_t>(ranges.size());
        const bool changed  = logicalSegCount != prevSegCount || rangeCount != prevRangeCount;
        const bool pressure = rangeCount >= kRangePressureTrigger;
        const std::uint32_t ms_count = millis() - t0_count;
        prevSegCount   = logicalSegCount;
        prevRangeCount = rangeCount;
        if (!changed) return;

        // Step 2: buildRangeStats.
        const std::uint32_t t0_stats = millis();
        const auto rangeStats = buildRangeStats(store);
        const std::uint32_t ms_stats = millis() - t0_stats;

        bool useful = false;
        for (const auto& rs : rangeStats) {
            if (rs.deadRatio() >= kDeadRatioTrigger) { useful = true; break; }
            if (pressure && rs.wouldConsolidate())   { useful = true; break; }
        }

        // Step 3: choose + narrow.
        std::uint32_t ms_choose = 0, ms_compact = 0;
        std::uint32_t chosen_s = 0, chosen_e = 0, target_s = 0, target_e = 0;
        std::uint32_t chosen_inSegs = 0, chosen_predOut = 0;
        std::uint32_t chosen_live = 0, chosen_total = 0;
        float chosen_dead = 0.0f;
        bool did_narrow = false, did_compact = false;
        std::uint32_t outSegs = 0;
        bool compact_noop = false;

        if (useful) {
            const std::uint32_t t0_choose = millis();
            const auto chosen = chooseHighestDeadRatio(rangeStats);
            std::optional<pqueue::AppendLogStore::CompactionRange> target_opt;
            if (chosen) {
                chosen_s = chosen->startGen; chosen_e = chosen->endGen;
                const RangeStat* cstat = nullptr;
                for (const auto& rs : rangeStats) {
                    if (rs.range.startGen == chosen_s && rs.range.endGen == chosen_e) {
                        cstat = &rs; break;
                    }
                }
                if (cstat) {
                    chosen_inSegs  = cstat->inputSegCount;
                    chosen_predOut = cstat->predictedOutputSegs();
                    chosen_live    = cstat->liveBytes;
                    chosen_total   = cstat->totalBytes;
                    chosen_dead    = cstat->deadRatio();
                }
                const auto tgt = cstat ? narrowRange(*chosen, *cstat, store) : *chosen;
                target_s = tgt.startGen; target_e = tgt.endGen;
                did_narrow  = (target_s != chosen_s || target_e != chosen_e);
                target_opt  = tgt;
            }
            ms_choose = millis() - t0_choose;

            // Step 4: compactRange.
            if (target_opt) {
                ++compactions;
                const std::uint32_t t0_compact = millis();
                const auto st = store.compactRange(*target_opt, &outSegs);
                ms_compact = millis() - t0_compact;
                t_compact += ms_compact;
                if (st.ok() && !st.isNoOp()) {
                    maxOutSegs   = std::max(maxOutSegs, outSegs);
                    maxLatencyMs = std::max(maxLatencyMs, ms_compact);
                    did_compact  = true;
                } else {
                    ++noOps;
                    compact_noop = true;
                }

                Serial.printf(
                    "[tryCompact] chosen=[%u,%u] target=[%u,%u] "
                    "in=%u predOut=%u live=%u total=%u dead=%u%% "
                    "choose_ms=%u compact_ms=%u outSegs=%u %s\n",
                    chosen_s, chosen_e, target_s, target_e,
                    chosen_inSegs, chosen_predOut, chosen_live, chosen_total,
                    static_cast<unsigned>(chosen_dead * 100.0f + 0.5f),
                    ms_choose, ms_compact, outSegs,
                    compact_noop ? "noOp" : "ok");
                Serial.flush();
            }
        }

        t_check += ms_count + ms_stats + ms_choose + ms_compact;
        t_stats += ms_stats;

        const std::uint32_t ms_total = ms_count + ms_stats + ms_choose + ms_compact;
        if (ms_total > 100 || did_compact) {
            Serial.printf("[check] changed=1 pressure=%u ranges=%u segs=%u "
                "count_ms=%u stats_ms=%u choose_ms=%u compact_ms=%u useful=%u",
                pressure, rangeCount, logicalSegCount,
                ms_count, ms_stats, ms_choose, ms_compact, useful);
            if (did_narrow)
                Serial.printf(" narrow=[%u,%u]->[%u,%u]", chosen_s, chosen_e, target_s, target_e);
            else if (chosen_s)
                Serial.printf(" compact=[%u,%u]", target_s, target_e);
            if (did_compact)
                Serial.printf(" outSegs=%u", outSegs);
            Serial.printf("\n");
            Serial.flush();
        }
    };

    for (std::uint32_t cycle = 0; cycle < kCycles; ++cycle) {
        t_wr = t_widx = t_ridx = 0;
        t_check = t_stats = t_compact = 0;
        t_enq = t_pop = 0;
        countingFs->resetCounters();
        const std::uint32_t t_cycle = millis();

        const std::uint32_t t_enq0 = millis();
        for (std::uint32_t i = 0; i < kBurstSize; ++i) {
            std::uint32_t t0;

            t0 = millis();
            const auto st = store.writeRecord(nextSeq, makePayload(nextSeq));
            t_wr += millis() - t0;

            if (st.ok()) {
                pqueue::FileStoreIndex dummy;
                t0 = millis();
                const auto idxSt = store.writeIndex(dummy);
                t_widx += millis() - t0;
                if (idxSt.ok()) {
                    ++nextSeq;
                    ++queueSize;
                } else {
                    Serial.printf("[enq] writeIndex failed: code=%d ranges=%u q=%u\n",
                        static_cast<int>(idxSt.code),
                        static_cast<unsigned>(static_cast<unsigned>(store.manifestRanges().size())),
                        queueSize);
                    Serial.flush();
                }
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
        t_enq = millis() - t_enq0;

        const std::uint32_t toPop =
            static_cast<std::uint32_t>(static_cast<float>(queueSize) * kPopRatio);
        const std::uint32_t t_pop0 = millis();
        for (std::uint32_t i = 0; i < toPop; ++i) {
            std::uint32_t t0;
            pqueue::FileStoreIndex idx;
            t0 = millis();
            const bool ok = store.readIndex(idx).ok() && idx.count > 0;
            t_ridx += millis() - t0;
            if (ok) {
                idx.head++;
                idx.count--;
                t0 = millis();
                const auto idxSt = store.writeIndex(idx);
                t_widx += millis() - t0;
                if (idxSt.ok()) {
                    --queueSize;
                } else {
                    Serial.printf("[pop] writeIndex failed: code=%d ranges=%u q=%u\n",
                        static_cast<int>(idxSt.code),
                        static_cast<unsigned>(static_cast<unsigned>(store.manifestRanges().size())),
                        queueSize);
                    Serial.flush();
                }
            }
            checkAndCompact();
        }
        t_pop = millis() - t_pop0;

        const std::uint32_t t_total = millis() - t_cycle;
        const std::uint32_t segs = [&]() {
            std::uint32_t s = 1;
            for (const auto& r : store.manifestRanges()) s += r.endGen - r.startGen + 1;
            return s;
        }();

        Serial.printf(
            "[cycle %2u] q=%u ranges=%u segs=%u total=%u enq=%u pop=%u "
            "check=%u stats=%u compact=%u wr=%u widx=%u ridx=%u\n",
            cycle, queueSize,
            static_cast<unsigned>(store.manifestRanges().size()),
            segs, t_total, t_enq, t_pop,
            t_check, t_stats, t_compact, t_wr, t_widx, t_ridx);
        {
            const auto& c = countingFs->counters();
            Serial.printf(
                "[fs] fileSize n=%u ms=%u readFile n=%u ms=%u writeFile n=%u bytes=%u ms=%u "
                "readAt n=%u bytes=%u ms=%u writeAt n=%u bytes=%u ms=%u "
                "removeFile n=%u ms=%u listFiles n=%u ms=%u\n",
                static_cast<unsigned>(c.fileSize),   c.msFileSize,
                static_cast<unsigned>(c.readFile),   c.msReadFile,
                static_cast<unsigned>(c.writeFile),  static_cast<unsigned>(c.bytesWritten), c.msWriteFile,
                static_cast<unsigned>(c.readAt),     static_cast<unsigned>(c.bytesRead),    c.msReadAt,
                static_cast<unsigned>(c.writeAt),    static_cast<unsigned>(c.bytesWritten), c.msWriteAt,
                static_cast<unsigned>(c.removeFile), c.msRemoveFile,
                static_cast<unsigned>(c.listFiles),  c.msListFiles);
        }
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

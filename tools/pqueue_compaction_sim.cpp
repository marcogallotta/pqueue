#include "counting_file_system.h"
#include "memory_file_system.h"
#include "pqueue/append_log_store.h"
#include "pqueue/append_log_common.h"
#include "pqueue/file_system.h"
#include "pqueue/queue.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Workload
// ---------------------------------------------------------------------------

struct WorkloadParams {
    std::uint32_t numOps               = 5000;
    double        enqueueProb          = 0.6;   // random mode: probability of enqueue vs pop
    std::uint32_t recordSize           = 150;   // bytes per record
    std::uint32_t maxSegmentBytes      = 4096;
    std::uint8_t  maxSegments          = 200;   // high: disables internal auto-compact
    float         deadRatioTrigger     = 0.25f; // compact if any range >= this fraction dead
    std::uint32_t rangePressureTrigger = 3;     // compact if range count >= this (out of kManifestMaxRanges=4)
    // Burst mode: models offline consumer pattern (enqueue N, drain ratio R, repeat).
    // When burst=true, enqueueProb is ignored.
    bool          burst                = false;
    std::uint32_t burstSize            = 500;   // records to enqueue per offline cycle
    float         popRatio             = 0.5f;  // fraction of queue to drain per online cycle
};

// ---------------------------------------------------------------------------
// Per-range stats (aggregated from per-segment stats)
// ---------------------------------------------------------------------------

struct RangeStat {
    pqueue::AppendLogStore::CompactionRange range;
    std::uint32_t totalBytes = 0;
    std::uint32_t liveBytes  = 0;
    std::uint32_t deadBytes() const { return totalBytes > liveBytes ? totalBytes - liveBytes : 0; }
    float deadRatio() const {
        return totalBytes > 0 ? static_cast<float>(deadBytes()) / static_cast<float>(totalBytes) : 0.0f;
    }
};

static std::vector<RangeStat> buildRangeStats(
    const pqueue::AppendLogStore& store)
{
    const auto& ranges = store.manifestRanges();
    const auto segStats = store.segmentStats();

    std::vector<RangeStat> result;
    result.reserve(ranges.size());

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

// ---------------------------------------------------------------------------
// Strategy interface
// ---------------------------------------------------------------------------

class Strategy {
public:
    virtual ~Strategy() = default;
    virtual const char* name() const = 0;
    // Return the range to compact, or nullopt to skip this opportunity.
    virtual std::optional<pqueue::AppendLogStore::CompactionRange> choose(
        const std::vector<RangeStat>& stats,
        std::uint32_t maxRanges) = 0;
    virtual void reset() {}
};

// ---------------------------------------------------------------------------
// Strategy implementations
// ---------------------------------------------------------------------------

// 1. Oldest-first (v1 baseline): always pick the first (oldest) range.
class OldestFirst : public Strategy {
public:
    const char* name() const override { return "OldestFirst"; }
    std::optional<pqueue::AppendLogStore::CompactionRange> choose(
        const std::vector<RangeStat>& stats, std::uint32_t) override
    {
        if (stats.empty()) return std::nullopt;
        return stats[0].range;
    }
};

// 2. Highest dead-byte ratio: pick the range with the most dead data proportionally.
class HighestDeadRatio : public Strategy {
public:
    const char* name() const override { return "HighestDeadRatio"; }
    std::optional<pqueue::AppendLogStore::CompactionRange> choose(
        const std::vector<RangeStat>& stats, std::uint32_t) override
    {
        if (stats.empty()) return std::nullopt;
        auto best = std::max_element(stats.begin(), stats.end(),
            [](const RangeStat& a, const RangeStat& b) { return a.deadRatio() < b.deadRatio(); });
        if (best->deadBytes() == 0) return std::nullopt;
        return best->range;
    }
};

// 3. Cost-benefit ratio: maximise dead bytes reclaimed per live byte copied.
class CostBenefit : public Strategy {
public:
    const char* name() const override { return "CostBenefit"; }
    std::optional<pqueue::AppendLogStore::CompactionRange> choose(
        const std::vector<RangeStat>& stats, std::uint32_t) override
    {
        if (stats.empty()) return std::nullopt;
        const RangeStat* best = nullptr;
        float bestScore = 0.0f;
        for (const auto& rs : stats) {
            if (rs.deadBytes() == 0) continue;
            float score = rs.liveBytes > 0
                ? static_cast<float>(rs.deadBytes()) / static_cast<float>(rs.liveBytes)
                : static_cast<float>(rs.deadBytes());
            if (score > bestScore) { bestScore = score; best = &rs; }
        }
        if (!best) return std::nullopt;
        return best->range;
    }
};

// 4. Range-consolidation-first: prefer ranges whose compaction would reduce range count
//    by merging with an adjacent range (contiguous generation numbers).
class RangeConsolidation : public Strategy {
public:
    const char* name() const override { return "RangeConsolidation"; }
    std::optional<pqueue::AppendLogStore::CompactionRange> choose(
        const std::vector<RangeStat>& stats, std::uint32_t) override
    {
        if (stats.empty()) return std::nullopt;
        // After compaction, new segment gets the next generation — it won't be
        // numerically adjacent to any existing range. So consolidation-by-merge
        // is not achievable via simple compaction in v1. Fall back to dead-ratio.
        // This strategy becomes meaningful when multi-output compaction exists.
        auto best = std::max_element(stats.begin(), stats.end(),
            [](const RangeStat& a, const RangeStat& b) { return a.deadRatio() < b.deadRatio(); });
        if (best->deadBytes() == 0) return std::nullopt;
        return best->range;
    }
};

// 5. Defer until pressure: no-op until range count reaches threshold, then
//    pick the highest dead-ratio range.
class DeferUntilPressure : public Strategy {
    float pressureThreshold_; // fraction of maxRanges
public:
    explicit DeferUntilPressure(float threshold = 0.75f) : pressureThreshold_(threshold) {}
    const char* name() const override { return "DeferUntilPressure"; }
    std::optional<pqueue::AppendLogStore::CompactionRange> choose(
        const std::vector<RangeStat>& stats, std::uint32_t maxRanges) override
    {
        const float pressure = maxRanges > 0
            ? static_cast<float>(stats.size()) / static_cast<float>(maxRanges)
            : 1.0f;
        if (pressure < pressureThreshold_) return std::nullopt;
        if (stats.empty()) return std::nullopt;
        auto best = std::max_element(stats.begin(), stats.end(),
            [](const RangeStat& a, const RangeStat& b) { return a.deadRatio() < b.deadRatio(); });
        if (best->deadBytes() == 0) return std::nullopt;
        return best->range;
    }
};

// 6. Pressure-weighted hybrid: cost-benefit normally; shift to oldest-first under pressure.
class PressureWeightedHybrid : public Strategy {
public:
    const char* name() const override { return "PressureWeightedHybrid"; }
    std::optional<pqueue::AppendLogStore::CompactionRange> choose(
        const std::vector<RangeStat>& stats, std::uint32_t maxRanges) override
    {
        if (stats.empty()) return std::nullopt;
        const float pressure = maxRanges > 0
            ? static_cast<float>(stats.size()) / static_cast<float>(maxRanges)
            : 1.0f;
        if (pressure >= 0.75f) {
            // Under pressure: oldest-first to guarantee progress.
            return stats[0].range;
        }
        // Normal: cost-benefit.
        const RangeStat* best = nullptr;
        float bestScore = 0.0f;
        for (const auto& rs : stats) {
            if (rs.deadBytes() == 0) continue;
            float score = rs.liveBytes > 0
                ? static_cast<float>(rs.deadBytes()) / static_cast<float>(rs.liveBytes)
                : static_cast<float>(rs.deadBytes());
            if (score > bestScore) { bestScore = score; best = &rs; }
        }
        if (!best) return std::nullopt;
        return best->range;
    }
};

// 7. Dead-byte threshold: oldest-first but skip if dead ratio below threshold.
class DeadByteThreshold : public Strategy {
    float threshold_;
public:
    explicit DeadByteThreshold(float threshold = 0.25f) : threshold_(threshold) {}
    const char* name() const override { return "DeadByteThreshold"; }
    std::optional<pqueue::AppendLogStore::CompactionRange> choose(
        const std::vector<RangeStat>& stats, std::uint32_t maxRanges) override
    {
        if (stats.empty()) return std::nullopt;
        const float pressure = maxRanges > 0
            ? static_cast<float>(stats.size()) / static_cast<float>(maxRanges)
            : 1.0f;
        // Override threshold under pressure.
        const float effective = pressure >= 0.75f ? 0.0f : threshold_;
        if (stats[0].deadRatio() >= effective) return stats[0].range;
        return std::nullopt;
    }
};

// 8. Minimum live bytes copied: among ranges with any dead bytes, pick the one
//    with fewest live bytes to copy (minimises write cost directly).
class MinLiveBytesCopied : public Strategy {
public:
    const char* name() const override { return "MinLiveBytesCopied"; }
    std::optional<pqueue::AppendLogStore::CompactionRange> choose(
        const std::vector<RangeStat>& stats, std::uint32_t) override
    {
        if (stats.empty()) return std::nullopt;
        const RangeStat* best = nullptr;
        for (const auto& rs : stats) {
            if (rs.deadBytes() == 0) continue;
            if (!best || rs.liveBytes < best->liveBytes) best = &rs;
        }
        if (!best) return std::nullopt;
        return best->range;
    }
};

// 9. Age-weighted dead-byte ratio: weight dead ratio by position in range list
//    (older ranges get higher weight).
class AgeWeightedDeadRatio : public Strategy {
public:
    const char* name() const override { return "AgeWeightedDeadRatio"; }
    std::optional<pqueue::AppendLogStore::CompactionRange> choose(
        const std::vector<RangeStat>& stats, std::uint32_t) override
    {
        if (stats.empty()) return std::nullopt;
        const float n = static_cast<float>(stats.size());
        const RangeStat* best = nullptr;
        float bestScore = -1.0f;
        for (std::size_t i = 0; i < stats.size(); ++i) {
            if (stats[i].deadBytes() == 0) continue;
            float ageWeight = (n - static_cast<float>(i)) / n; // 1.0 for oldest, ~0 for newest
            float score = stats[i].deadRatio() * (1.0f + ageWeight);
            if (score > bestScore) { bestScore = score; best = &stats[i]; }
        }
        if (!best) return std::nullopt;
        return best->range;
    }
};

// 10. Lookahead heuristic: skip ranges that are still mostly live (ratio < 0.2)
//     on the assumption they will accumulate more dead data soon; otherwise
//     use cost-benefit.
class LookaheadHeuristic : public Strategy {
    static constexpr float kSkipThreshold = 0.2f;
public:
    const char* name() const override { return "LookaheadHeuristic"; }
    std::optional<pqueue::AppendLogStore::CompactionRange> choose(
        const std::vector<RangeStat>& stats, std::uint32_t maxRanges) override
    {
        if (stats.empty()) return std::nullopt;
        const float pressure = maxRanges > 0
            ? static_cast<float>(stats.size()) / static_cast<float>(maxRanges)
            : 1.0f;
        const RangeStat* best = nullptr;
        float bestScore = 0.0f;
        for (const auto& rs : stats) {
            if (rs.deadBytes() == 0) continue;
            if (rs.deadRatio() < kSkipThreshold && pressure < 0.75f) continue;
            float score = rs.liveBytes > 0
                ? static_cast<float>(rs.deadBytes()) / static_cast<float>(rs.liveBytes)
                : static_cast<float>(rs.deadBytes());
            if (score > bestScore) { bestScore = score; best = &rs; }
        }
        if (!best) return std::nullopt;
        return best->range;
    }
};

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

struct SimMetrics {
    std::uint64_t flashWearBytes          = 0;  // total bytes written to disk
    std::uint64_t liveBytesCompacted      = 0;  // live bytes written in compaction segments
    std::uint64_t deadBytesReclaimed      = 0;  // dead bytes in ranges that were compacted
    std::uint32_t compactionAttempts      = 0;  // times strategy returned a range
    std::uint32_t compactionNoOps         = 0;  // strategy returned range but store returned no-op
    std::uint32_t compactionSkips         = 0;  // strategy returned nullopt
    std::uint32_t trueDeadlocks           = 0;  // range limit hit with reclaimable dead bytes
    std::uint32_t capacityExhausted       = 0;  // range limit hit with no reclaimable dead bytes
    std::uint32_t maxRangeCount           = 0;
    std::uint32_t maxSegmentsPerCompact   = 0;  // always 0 or 1 in v1; tracked for future
    std::uint64_t totalCompactSegments    = 0;

    double writeAmplification() const {
        return deadBytesReclaimed > 0
            ? static_cast<double>(liveBytesCompacted) / static_cast<double>(deadBytesReclaimed)
            : 0.0;
    }
    double avgCompactCost() const {
        return compactionAttempts > 0
            ? static_cast<double>(totalCompactSegments) / static_cast<double>(compactionAttempts)
            : 0.0;
    }
};

// ---------------------------------------------------------------------------
// Simulation runner
// ---------------------------------------------------------------------------

static SimMetrics runSimulation(const WorkloadParams& wp, Strategy& strategy) {
    strategy.reset();

    auto counting = std::make_shared<CountingFileSystem>(std::make_shared<MemoryFileSystem>());

    pqueue::AppendLogConfig cfg;
    cfg.basePath        = "sim";
    cfg.fileSystem      = counting;
    cfg.maxSegmentBytes = wp.maxSegmentBytes;
    cfg.maxSegments     = wp.maxSegments;
    cfg.maxTotalBytes   = wp.maxSegmentBytes * wp.maxSegments * 4;
    cfg.minFreeBytes    = 0;

    pqueue::AppendLogStore store(cfg);
    store.mount();

    SimMetrics metrics;
    std::uint32_t nextSeq = 1;
    std::uint32_t queueSize = 0;
    std::uint32_t prevRangeCount = 0;

    // Simple LCG for reproducible pseudo-random sequence.
    std::uint32_t rng = 0xdeadbeef;
    auto nextRand = [&]() -> double {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<double>(rng) / static_cast<double>(0xFFFFFFFFu);
    };

    auto tryCompact = [&]() {
        if (store.manifestRanges().empty()) return;
        const auto rangeStats = buildRangeStats(store);
        const std::uint32_t maxRanges = pqueue::append_log_detail::kManifestMaxRanges;

        auto chosen = strategy.choose(rangeStats, maxRanges);
        if (!chosen) {
            ++metrics.compactionSkips;
            return;
        }
        ++metrics.compactionAttempts;

        std::uint32_t deadBefore = 0;
        std::uint32_t liveBefore = 0;
        for (const auto& rs : rangeStats) {
            if (rs.range.startGen == chosen->startGen && rs.range.endGen == chosen->endGen) {
                deadBefore = rs.deadBytes();
                liveBefore = rs.liveBytes;
                break;
            }
        }

        counting->resetCounters();
        auto cst = store.compactRange(*chosen);
        const std::uint64_t compactWritten = counting->counters().bytesWritten;

        if (cst.ok() && !cst.isNoOp()) {
            metrics.flashWearBytes     += compactWritten;
            metrics.liveBytesCompacted += liveBefore;
            metrics.deadBytesReclaimed += deadBefore;
            ++metrics.totalCompactSegments;
            metrics.maxSegmentsPerCompact = std::max(metrics.maxSegmentsPerCompact, 1u);
        } else {
            ++metrics.compactionNoOps;
        }
    };

    // Rising-edge trigger: fire when a new full range is added (rotation), or
    // when range count hits the emergency pressure threshold.
    auto checkAndCompact = [&]() {
        const std::uint32_t currentRangeCount =
            static_cast<std::uint32_t>(store.manifestRanges().size());

        const bool newRangeAdded = currentRangeCount > prevRangeCount;
        const bool emergencyPressure = currentRangeCount >= wp.rangePressureTrigger;

        if (newRangeAdded || emergencyPressure) {
            bool anyUseful = emergencyPressure;
            if (!anyUseful) {
                for (const auto& rs : buildRangeStats(store)) {
                    if (rs.deadRatio() >= wp.deadRatioTrigger) { anyUseful = true; break; }
                }
            }
            if (anyUseful) tryCompact();
        }
        prevRangeCount = currentRangeCount;
    };

    const std::string payload(wp.recordSize, 'x');

    auto doEnqueue = [&]() {
        counting->resetCounters();
        auto st = store.writeRecord(nextSeq, payload);
        if (st.ok()) {
            pqueue::FileStoreIndex dummy;
            store.writeIndex(dummy);
            ++nextSeq;
            ++queueSize;
            metrics.flashWearBytes += counting->counters().bytesWritten;
        } else {
            static constexpr float kMinDeadRatioReclaimable = 0.01f;
            bool reclaimable = false;
            for (const auto& rs : buildRangeStats(store)) {
                if (rs.deadRatio() >= kMinDeadRatioReclaimable) { reclaimable = true; break; }
            }
            if (reclaimable)
                ++metrics.trueDeadlocks;
            else
                ++metrics.capacityExhausted;
        }
        metrics.maxRangeCount = std::max(metrics.maxRangeCount,
            static_cast<std::uint32_t>(store.manifestRanges().size()));
        checkAndCompact();
    };

    auto doPop = [&]() {
        counting->resetCounters();
        pqueue::FileStoreIndex idx;
        store.readIndex(idx);
        if (idx.count > 0) {
            idx.head++;
            idx.count--;
            store.writeIndex(idx);
            --queueSize;
            metrics.flashWearBytes += counting->counters().bytesWritten;
        }
        metrics.maxRangeCount = std::max(metrics.maxRangeCount,
            static_cast<std::uint32_t>(store.manifestRanges().size()));
        checkAndCompact();
    };

    static constexpr std::uint32_t kDeadlockAbortThreshold = 1000;

    if (wp.burst) {
        std::uint32_t opsLeft = wp.numOps;
        while (opsLeft > 0) {
            for (std::uint32_t i = 0; i < wp.burstSize && opsLeft > 0; ++i, --opsLeft) {
                doEnqueue();
                if (metrics.trueDeadlocks + metrics.capacityExhausted >= kDeadlockAbortThreshold) return metrics;
            }
            const std::uint32_t toPop =
                static_cast<std::uint32_t>(static_cast<float>(queueSize) * wp.popRatio);
            for (std::uint32_t i = 0; i < toPop && opsLeft > 0; ++i, --opsLeft)
                doPop();
        }
    } else {
        for (std::uint32_t op = 0; op < wp.numOps; ++op) {
            if ((queueSize == 0) || (nextRand() < wp.enqueueProb))
                doEnqueue();
            else
                doPop();
            if (metrics.trueDeadlocks + metrics.capacityExhausted >= kDeadlockAbortThreshold) return metrics;
        }
    }

    return metrics;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

static void printHeader() {
    std::printf("%-22s | %8s | %9s | %8s | %8s | %8s | %8s | %8s | %8s\n",
        "Strategy", "WriteAmp", "Wear(KB)", "MaxRange", "Compacts", "NoOps", "Skips", "Deadlock", "CapExhst");
    std::printf("%-22s-+-%8s-+-%9s-+-%8s-+-%8s-+-%8s-+-%8s-+-%8s-+-%8s\n",
        "----------------------", "--------", "---------", "--------",
        "--------", "--------", "--------", "--------", "--------");
}

static void printRow(const char* name, const SimMetrics& m) {
    std::printf("%-22s | %8.2f | %9.1f | %8u | %8u | %8u | %8u | %8u | %8u\n",
        name,
        m.writeAmplification(),
        static_cast<double>(m.flashWearBytes) / 1024.0,
        m.maxRangeCount,
        m.compactionAttempts,
        m.compactionNoOps,
        m.compactionSkips,
        m.trueDeadlocks,
        m.capacityExhausted);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    std::vector<std::unique_ptr<Strategy>> strategies;
    strategies.push_back(std::make_unique<OldestFirst>());
    strategies.push_back(std::make_unique<HighestDeadRatio>());
    strategies.push_back(std::make_unique<CostBenefit>());
    strategies.push_back(std::make_unique<RangeConsolidation>());
    strategies.push_back(std::make_unique<DeferUntilPressure>());
    strategies.push_back(std::make_unique<PressureWeightedHybrid>());
    strategies.push_back(std::make_unique<DeadByteThreshold>());
    strategies.push_back(std::make_unique<MinLiveBytesCopied>());
    strategies.push_back(std::make_unique<AgeWeightedDeadRatio>());
    strategies.push_back(std::make_unique<LookaheadHeuristic>());

    struct WorkloadDef {
        const char* label;
        WorkloadParams params;
    };

    // maxSegments=200 disables internal auto-compaction so the external strategy
    // is solely responsible for keeping range count in check.
    //
    // SPEED NOTE: all sizes below are proportionally scaled down from the real-world
    // values for simulation runtime. Real values: maxSegmentBytes=4096, recordSizes=
    // 64/150/492, burstSizes=100/500/2000. Scaled by 1/8: maxSegmentBytes=512,
    // recordSizes=8/19/62, burstSizes=12/60/250. The records-per-segment ratio is
    // preserved within rounding (overhead is fixed at 24 bytes per event).
    // BEFORE MAKING A FINAL STRATEGY DECISION: revert to real-world sizes and rerun
    // to confirm findings hold at actual LittleFS block sizes and payload sizes.

    // --- Random interleaved workload (baseline) ---
    const std::vector<WorkloadDef> randomWorkloads = {
        { "enqP=0.55", { 5000, 0.55, 19, 512, 200, 0.25f, 3 } },
        { "enqP=0.65", { 5000, 0.65, 19, 512, 200, 0.25f, 3 } },
        { "enqP=0.80", { 5000, 0.80, 19, 512, 200, 0.25f, 3 } },
    };

    // --- Burst workload (offline consumer pattern) ---
    // Models the primary real-world failure mode: device enqueues continuously
    // while the server/network is down (burst phase), then drains rapidly when
    // connectivity resumes (pop phase). This creates full ranges with concentrated
    // dead bytes -- exactly the case compaction must handle.
    //
    // Parameters swept:
    //   burstSize  -- records enqueued per offline cycle (12=short, 250=long outage)
    //   popRatio   -- fraction of queue drained per online cycle (25%=slow, 90%=fast)
    //   recordSize -- bytes per record (8=small, 19=target app, 62=large)
    //
    // Record size matters because it determines records-per-segment, which controls
    // how many pops are needed before a segment can be eliminated by compaction.
    // Small records pack more per segment: more pops required, compaction fires later.
    // Large records pack fewer per segment: compaction fires earlier and more often.
    const std::uint32_t numBurstOps = 15000;
    const std::vector<WorkloadDef> burstWorkloads = {
        // small records (8 bytes, ~14 records/segment)
        { "burst=12  pop=25% rec=8",  { numBurstOps, 0.0, 8,  512, 200, 0.25f, 3, true, 12,  0.25f } },
        { "burst=12  pop=50% rec=8",  { numBurstOps, 0.0, 8,  512, 200, 0.25f, 3, true, 12,  0.50f } },
        { "burst=12  pop=90% rec=8",  { numBurstOps, 0.0, 8,  512, 200, 0.25f, 3, true, 12,  0.90f } },
        { "burst=60  pop=25% rec=8",  { numBurstOps, 0.0, 8,  512, 200, 0.25f, 3, true, 60,  0.25f } },
        { "burst=60  pop=50% rec=8",  { numBurstOps, 0.0, 8,  512, 200, 0.25f, 3, true, 60,  0.50f } },
        { "burst=60  pop=90% rec=8",  { numBurstOps, 0.0, 8,  512, 200, 0.25f, 3, true, 60,  0.90f } },
        { "burst=250 pop=25% rec=8",  { numBurstOps, 0.0, 8,  512, 200, 0.25f, 3, true, 250, 0.25f } },
        { "burst=250 pop=50% rec=8",  { numBurstOps, 0.0, 8,  512, 200, 0.25f, 3, true, 250, 0.50f } },
        { "burst=250 pop=90% rec=8",  { numBurstOps, 0.0, 8,  512, 200, 0.25f, 3, true, 250, 0.90f } },
        // target records (19 bytes, ~11 records/segment)
        { "burst=12  pop=25% rec=19", { numBurstOps, 0.0, 19, 512, 200, 0.25f, 3, true, 12,  0.25f } },
        { "burst=12  pop=50% rec=19", { numBurstOps, 0.0, 19, 512, 200, 0.25f, 3, true, 12,  0.50f } },
        { "burst=12  pop=90% rec=19", { numBurstOps, 0.0, 19, 512, 200, 0.25f, 3, true, 12,  0.90f } },
        { "burst=60  pop=25% rec=19", { numBurstOps, 0.0, 19, 512, 200, 0.25f, 3, true, 60,  0.25f } },
        { "burst=60  pop=50% rec=19", { numBurstOps, 0.0, 19, 512, 200, 0.25f, 3, true, 60,  0.50f } },
        { "burst=60  pop=90% rec=19", { numBurstOps, 0.0, 19, 512, 200, 0.25f, 3, true, 60,  0.90f } },
        { "burst=250 pop=25% rec=19", { numBurstOps, 0.0, 19, 512, 200, 0.25f, 3, true, 250, 0.25f } },
        { "burst=250 pop=50% rec=19", { numBurstOps, 0.0, 19, 512, 200, 0.25f, 3, true, 250, 0.50f } },
        { "burst=250 pop=90% rec=19", { numBurstOps, 0.0, 19, 512, 200, 0.25f, 3, true, 250, 0.90f } },
        // large records (62 bytes, ~5 records/segment)
        { "burst=12  pop=25% rec=62", { numBurstOps, 0.0, 62, 512, 200, 0.25f, 3, true, 12,  0.25f } },
        { "burst=12  pop=50% rec=62", { numBurstOps, 0.0, 62, 512, 200, 0.25f, 3, true, 12,  0.50f } },
        { "burst=12  pop=90% rec=62", { numBurstOps, 0.0, 62, 512, 200, 0.25f, 3, true, 12,  0.90f } },
        { "burst=60  pop=25% rec=62", { numBurstOps, 0.0, 62, 512, 200, 0.25f, 3, true, 60,  0.25f } },
        { "burst=60  pop=50% rec=62", { numBurstOps, 0.0, 62, 512, 200, 0.25f, 3, true, 60,  0.50f } },
        { "burst=60  pop=90% rec=62", { numBurstOps, 0.0, 62, 512, 200, 0.25f, 3, true, 60,  0.90f } },
        { "burst=250 pop=25% rec=62", { numBurstOps, 0.0, 62, 512, 200, 0.25f, 3, true, 250, 0.25f } },
        { "burst=250 pop=50% rec=62", { numBurstOps, 0.0, 62, 512, 200, 0.25f, 3, true, 250, 0.50f } },
        { "burst=250 pop=90% rec=62", { numBurstOps, 0.0, 62, 512, 200, 0.25f, 3, true, 250, 0.90f } },
    };

    std::printf("=== Random interleaved workload (baseline) ===\n");
    for (const auto& wl : randomWorkloads) {
        std::printf("\nWorkload: %s  ops=%u  rec=%uB  maxSeg=%uB  deadTrig=%.0f%%\n",
            wl.label, wl.params.numOps, wl.params.recordSize,
            wl.params.maxSegmentBytes, wl.params.deadRatioTrigger * 100.0f);
        printHeader();
        for (auto& strat : strategies) {
            SimMetrics m = runSimulation(wl.params, *strat);
            printRow(strat->name(), m);
        }
    }

    std::printf("\n=== Burst workload (offline consumer pattern) ===\n");
    for (const auto& wl : burstWorkloads) {
        std::printf("\nWorkload: %s  ops=%u  maxSeg=%uB  deadTrig=%.0f%%\n",
            wl.label, wl.params.numOps,
            wl.params.maxSegmentBytes, wl.params.deadRatioTrigger * 100.0f);
        printHeader();
        for (auto& strat : strategies) {
            SimMetrics m = runSimulation(wl.params, *strat);
            printRow(strat->name(), m);
        }
    }

    std::printf("\n");
    return 0;
}

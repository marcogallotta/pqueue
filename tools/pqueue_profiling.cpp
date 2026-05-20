#include "counting_file_system.h"
#include "memory_file_system.h"
#include "pqueue/append_log_store.h"
#include "pqueue/append_log_common.h"
#include "pqueue/queue.h"
#include "pqueue/outbox.h"
#include "pqueue/http/outbox.h"
#include "pqueue/storage_common.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Timings {
    std::vector<double> us;

    void add(Clock::time_point start, Clock::time_point end) {
        us.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    }

    double avg() const {
        if (us.empty()) return 0.0;
        return std::accumulate(us.begin(), us.end(), 0.0) / static_cast<double>(us.size());
    }

    double p95() const {
        if (us.empty()) return 0.0;
        auto copy = us;
        std::sort(copy.begin(), copy.end());
        const std::size_t index = static_cast<std::size_t>((copy.size() - 1) * 0.95);
        return copy[index];
    }

    double max() const {
        if (us.empty()) return 0.0;
        return *std::max_element(us.begin(), us.end());
    }
};

struct ScenarioResult {
    std::string scenario;
    std::uint32_t records = 0;
    std::uint32_t payloadBytes = 0;
    Timings timings;
    FsCounters fs;
    pqueue::Status status = pqueue::Status::success();
};

std::string tempBase(const std::string& name) {
    auto root = std::filesystem::temp_directory_path() / "pqueue_profiling" / name;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root.string();
}

std::string payload(std::size_t size, char fill = 'x') {
    std::string out(size, fill);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>('a' + (i % 26));
    }
    return out;
}

pqueue::Config queueConfig(const std::string& basePath, std::shared_ptr<CountingFileSystem> fs, std::uint32_t recordSizeBytes, std::uint32_t requestedRecords) {
    pqueue::Config cfg;
    cfg.basePath = basePath;
    cfg.recordSizeBytes = recordSizeBytes;
    cfg.reservedBytes = requestedRecords * static_cast<std::uint32_t>(pqueue::storage_detail::kRecordHeaderBytes + recordSizeBytes);
    cfg.storageBackend = pqueue::StorageBackend::Posix;
    cfg.fileSystem = fs;
    return cfg;
}

void recordTiming(Timings& timings, const std::function<pqueue::Status()>& fn, pqueue::Status& last) {
    const auto start = Clock::now();
    last = fn();
    const auto end = Clock::now();
    timings.add(start, end);
}

ScenarioResult scenarioQueueEnqueue(std::uint32_t records, std::uint32_t payloadBytes) {
    auto fs = std::make_shared<CountingFileSystem>(pqueue::makePosixFileSystem());
    auto cfg = queueConfig(tempBase("queue_enqueue"), fs, 1024, records + 8);
    pqueue::Queue q(cfg);
    (void)q.stats();
    const auto data = payload(payloadBytes);
    fs->resetCounters();

    ScenarioResult result{};
    result.scenario = "queue_enqueue";
    result.records = records;
    result.payloadBytes = payloadBytes;
    for (std::uint32_t i = 0; i < records; ++i) {
        recordTiming(result.timings, [&] { return q.enqueue(data); }, result.status);
        if (!result.status.ok()) break;
    }
    result.fs = fs->counters();
    return result;
}

ScenarioResult scenarioQueuePeekPop(std::uint32_t records, std::uint32_t payloadBytes) {
    auto fs = std::make_shared<CountingFileSystem>(pqueue::makePosixFileSystem());
    auto cfg = queueConfig(tempBase("queue_peek_pop"), fs, 1024, records + 8);
    pqueue::Queue q(cfg);
    const auto data = payload(payloadBytes);
    for (std::uint32_t i = 0; i < records; ++i) {
        auto st = q.enqueue(data);
        if (!st.ok()) return {"queue_peek_pop_setup", records, payloadBytes, {}, fs->counters(), st};
    }
    fs->resetCounters();

    ScenarioResult result{};
    result.scenario = "queue_peek_pop";
    result.records = records;
    result.payloadBytes = payloadBytes;
    std::string out;
    for (std::uint32_t i = 0; i < records; ++i) {
        recordTiming(result.timings, [&] {
            auto st = q.peek(out);
            if (!st.ok()) return st;
            return q.pop();
        }, result.status);
        if (!result.status.ok()) break;
    }
    result.fs = fs->counters();
    return result;
}

ScenarioResult scenarioValidateFull(std::uint32_t records, std::uint32_t payloadBytes) {
    auto fs = std::make_shared<CountingFileSystem>(pqueue::makePosixFileSystem());
    auto cfg = queueConfig(tempBase("validate_full"), fs, 1024, records + 8);
    pqueue::Queue q(cfg);
    const auto data = payload(payloadBytes);
    for (std::uint32_t i = 0; i < records; ++i) {
        auto st = q.enqueue(data);
        if (!st.ok()) return {"validate_full_setup", records, payloadBytes, {}, fs->counters(), st};
    }
    fs->resetCounters();

    ScenarioResult result{};
    result.scenario = "validate_full";
    result.records = records;
    result.payloadBytes = payloadBytes;
    recordTiming(result.timings, [&] {
        const auto validation = q.validate();
        return validation.ok ? pqueue::Status::success() : pqueue::Status::failure(pqueue::StatusCode::InvalidIndex, "validation failed");
    }, result.status);
    result.fs = fs->counters();
    return result;
}

struct FakeClock {
    std::uint64_t now = 0;
};

std::uint64_t fakeClock(void* context) {
    return static_cast<FakeClock*>(context)->now;
}

struct FakeTransport final : public pqueue::http::Transport {
    bool online = true;

    pqueue::http::Response post(
        const char*,
        const pqueue::http::Header*,
        std::size_t,
        const std::uint8_t*,
        std::size_t
    ) override {
        if (!online) {
            return {pqueue::http::kNoStatusCode, pqueue::http::TransportError::Network};
        }
        return {200, pqueue::http::TransportError::None};
    }
};

struct PreparedHttpOutbox {
    std::shared_ptr<CountingFileSystem> fs;
    std::unique_ptr<pqueue::http::Outbox> outbox;
    std::unique_ptr<FakeTransport> transport;
    std::unique_ptr<FakeClock> clock;
    pqueue::Status status = pqueue::Status::success();
};

PreparedHttpOutbox prepareHttpOutboxBacklog(std::uint32_t records, std::uint32_t payloadBytes, const std::string& baseName) {
    PreparedHttpOutbox prepared;
    prepared.fs = std::make_shared<CountingFileSystem>(pqueue::makePosixFileSystem());
    prepared.transport = std::make_unique<FakeTransport>();
    prepared.clock = std::make_unique<FakeClock>();

    pqueue::http::Config cfg;
    cfg.queue = queueConfig(tempBase(baseName), prepared.fs, 2048, records + 8);
    cfg.outbox.maxDrainAttemptsPerSecond = 1000;
    cfg.outbox.retryDelayMs = 1;
    cfg.baseUrl = "https://example.test";

    prepared.outbox = std::make_unique<pqueue::http::Outbox>(cfg, *prepared.transport, fakeClock, prepared.clock.get());
    const auto body = payload(payloadBytes);
    prepared.transport->online = false;
    for (std::uint32_t i = 0; i < records; ++i) {
        const auto submit = prepared.outbox->submitPost("/readings", body);
        if (submit.status != pqueue::SubmitStatus::Queued) {
            prepared.status = submit.detail;
            break;
        }
        prepared.clock->now += 2;
    }
    return prepared;
}

ScenarioResult scenarioHttpOutboxOfflineSubmit(std::uint32_t records, std::uint32_t payloadBytes) {
    auto fs = std::make_shared<CountingFileSystem>(pqueue::makePosixFileSystem());
    auto transport = std::make_unique<FakeTransport>();
    auto clock = std::make_unique<FakeClock>();

    pqueue::http::Config cfg;
    cfg.queue = queueConfig(tempBase("http_outbox_offline_submit"), fs, 2048, records + 8);
    cfg.outbox.maxDrainAttemptsPerSecond = 1000;
    cfg.outbox.retryDelayMs = 1;
    cfg.baseUrl = "https://example.test";

    pqueue::http::Outbox outbox(cfg, *transport, fakeClock, clock.get());
    (void)outbox.stats();
    const auto body = payload(payloadBytes);
    transport->online = false;
    fs->resetCounters();

    ScenarioResult result{};
    result.scenario = "http_offline_submit";
    result.records = records;
    result.payloadBytes = payloadBytes;
    for (std::uint32_t i = 0; i < records; ++i) {
        const auto start = Clock::now();
        const auto submit = outbox.submitPost("/readings", body);
        const auto end = Clock::now();
        result.timings.add(start, end);
        if (submit.status != pqueue::SubmitStatus::Queued) {
            result.status = submit.detail;
            break;
        }
        clock->now += 2;
    }
    result.fs = fs->counters();
    return result;
}

ScenarioResult scenarioHttpOutboxDrainBacklog(std::uint32_t records, std::uint32_t payloadBytes) {
    auto prepared = prepareHttpOutboxBacklog(records, payloadBytes, "http_outbox_drain_backlog");
    ScenarioResult result{};
    result.scenario = "http_drain_backlog";
    result.records = records;
    result.payloadBytes = payloadBytes;
    if (!prepared.status.ok()) {
        result.status = prepared.status;
        result.fs = prepared.fs->counters();
        return result;
    }

    prepared.transport->online = true;
    prepared.fs->resetCounters();
    for (std::uint32_t i = 0; i < records; ++i) {
        const auto start = Clock::now();
        const auto drain = prepared.outbox->drainUpTo(1);
        const auto end = Clock::now();
        result.timings.add(start, end);
        if (drain.queueError || drain.sendError || drain.sent != 1) {
            result.status = drain.detail;
            break;
        }
        prepared.clock->now += 2;
    }
    result.fs = prepared.fs->counters();
    return result;
}

// ---------------------------------------------------------------------------
// Compaction burst scenario
// ---------------------------------------------------------------------------

struct CompactionBurstResult {
    std::uint32_t burstSize      = 0;
    std::uint32_t payloadBytes   = 0;
    std::uint32_t cycles         = 0;
    std::uint32_t maxOutSegsLimit = 0;
    std::uint32_t maxInSegsLimit  = 0;
    std::uint32_t compactions    = 0;
    std::uint32_t noOps          = 0;
    std::uint32_t maxOutSegs     = 0;
    std::uint64_t maxLatencyUs   = 0;
    std::uint32_t deadlocks      = 0;
    std::uint32_t capExhausted   = 0;
    std::uint32_t finalQSize     = 0;
    FsCounters    fs;
    bool          ok             = true;
};

namespace {

struct CompactRangeStat {
    pqueue::AppendLogStore::CompactionRange range;
    std::uint32_t totalBytes    = 0;
    std::uint32_t liveBytes     = 0;
    std::uint32_t inputSegCount = 0;
    std::uint32_t deadBytes() const { return totalBytes > liveBytes ? totalBytes - liveBytes : 0; }
    float deadRatio() const {
        return totalBytes > 0 ? static_cast<float>(deadBytes()) / static_cast<float>(totalBytes) : 0.0f;
    }
    std::uint32_t predictedOutputSegs(std::uint32_t maxSegBytes) const {
        if (liveBytes == 0) return 0;
        return (liveBytes + maxSegBytes - 1) / maxSegBytes;
    }
};

std::vector<CompactRangeStat> buildCompactRangeStats(const pqueue::AppendLogStore& store) {
    const auto& ranges   = store.manifestRanges();
    const auto  segStats = store.segmentStats();
    std::vector<CompactRangeStat> result;
    for (const auto& r : ranges) {
        CompactRangeStat rs;
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

std::optional<pqueue::AppendLogStore::CompactionRange> chooseHighestDeadRatioCompact(
    const std::vector<CompactRangeStat>& stats)
{
    if (stats.empty()) return std::nullopt;
    auto best = std::max_element(stats.begin(), stats.end(),
        [](const CompactRangeStat& a, const CompactRangeStat& b) {
            return a.deadRatio() < b.deadRatio();
        });
    if (best->deadBytes() == 0) return std::nullopt;
    return best->range;
}

pqueue::AppendLogStore::CompactionRange narrowCompactRange(
    const pqueue::AppendLogStore::CompactionRange& range,
    const CompactRangeStat& rs,
    const pqueue::AppendLogStore& store,
    std::uint32_t maxSegBytes,
    std::uint32_t maxOutputSegs)
{
    if (rs.predictedOutputSegs(maxSegBytes) <= maxOutputSegs) return range;

    const auto allSegs = store.segmentStats();
    std::vector<pqueue::AppendLogStore::SegmentStat> segs;
    for (const auto& ss : allSegs) {
        if (ss.generation >= range.startGen && ss.generation <= range.endGen)
            segs.push_back(ss);
    }
    std::sort(segs.begin(), segs.end(),
        [](const auto& a, const auto& b) { return a.generation < b.generation; });

    pqueue::AppendLogStore::CompactionRange best = {segs.front().generation, segs.front().generation};
    float bestDead = -1.0f;
    for (std::size_t i = 0; i < segs.size(); ++i) {
        std::uint32_t live = 0, total = 0;
        for (std::size_t j = i; j < segs.size(); ++j) {
            live  += segs[j].liveBytes;
            total += segs[j].totalBytes;
            const std::uint32_t out = live == 0 ? 0u : (live + maxSegBytes - 1) / maxSegBytes;
            if (out > maxOutputSegs) break;
            const float dr = total > 0 ? static_cast<float>(total - live) / static_cast<float>(total) : 0.0f;
            if (dr > bestDead) { bestDead = dr; best = {segs[i].generation, segs[j].generation}; }
        }
    }
    return best;
}

} // namespace

// LittleFS observed timings scaled down 100x for fast simulation.
// Calibrated from Run 4 (burst=500/pop=90%/rec=492B/cycles=3):
//   simMaxLatency=48.6ms x 100 = 4860ms, actual MaxLatency=4861ms.
// Previous constants (pre-Run4) used a uniform ~50ms/op estimate and
// overestimated by ~45% for write-heavy workloads. Rescaled by 0.69.
// readFile and readAt model open+read+close on LittleFS (~35ms each).
// writeFile: ~140ms fixed metadata/GC cost + ~52ms per KB of data written.
// listFiles: ~70ms base + ~9ms per file in the directory.
inline FsLatency littleFsSimLatency() {
    FsLatency lat;
    lat.readFileUs        = 345;   // ~35ms on device
    lat.readAtUs          = 345;   // ~35ms on device
    lat.writeFileFixedUs  = 1380;  // ~138ms fixed create/metadata cost
    lat.writeFilePerKbUs  = 518;   // ~52ms per KB written
    lat.writeAtUs         = 138;   // ~14ms on device
    lat.removeFileUs      = 166;   // ~17ms on device
    lat.listFilesBaseUs   = 690;   // ~69ms base cost
    lat.listFilesPerFileUs = 86;   // ~8.6ms per file
    return lat;
}

CompactionBurstResult scenarioCompactionBurst(
    std::uint32_t burstSize,
    std::uint32_t payloadBytes,
    std::uint32_t cycles,
    std::uint32_t maxOutputSegs = 8,
    float deadRatioTrigger = 0.10f,
    std::uint32_t rangePressureTrigger = 3,
    FsLatency latency = {})
{
    constexpr std::uint32_t kMaxSegmentBytes = 4096;
    constexpr std::uint32_t kMaxTotalBytes   = 1024 * 1024;

    auto memFs    = std::make_shared<MemoryFileSystem>();
    auto counting = std::make_shared<CountingFileSystem>(memFs);
    counting->setLatency(latency);

    pqueue::AppendLogConfig cfg;
    cfg.basePath        = "/compact_prof";
    cfg.backend         = pqueue::StorageBackend::Posix;
    cfg.maxSegmentBytes = kMaxSegmentBytes;
    cfg.maxSegments     = 200;
    cfg.maxTotalBytes   = kMaxTotalBytes;
    cfg.minFreeBytes    = 0;
    cfg.fileSystem      = counting;

    pqueue::AppendLogStore store(cfg);
    CompactionBurstResult result;
    result.burstSize       = burstSize;
    result.payloadBytes    = payloadBytes;
    result.cycles          = cycles;
    result.maxOutSegsLimit = maxOutputSegs;
    result.maxInSegsLimit  = 0;

    if (!store.mount().ok()) { result.ok = false; return result; }

    const std::string payload(payloadBytes, 'x');
    std::uint32_t nextSeq      = 1;
    std::uint32_t queueSize    = 0;
    std::uint32_t prevSegCount = 0;
    std::uint32_t prevRangeCount = 0;

    auto checkAndCompact = [&]() {
        const auto& ranges = store.manifestRanges();
        std::uint32_t logicalSegs = 1;
        for (const auto& r : ranges) logicalSegs += r.endGen - r.startGen + 1;
        const std::uint32_t rangeCount = static_cast<std::uint32_t>(ranges.size());
        const bool changed  = logicalSegs != prevSegCount || rangeCount != prevRangeCount;
        const bool pressure = rangeCount >= rangePressureTrigger;
        prevSegCount   = logicalSegs;
        prevRangeCount = rangeCount;
        if (!changed) return;

        const auto rangeStats = buildCompactRangeStats(store);
        bool useful = false;
        for (const auto& rs : rangeStats) {
            if (rs.deadRatio() >= deadRatioTrigger) { useful = true; break; }
            if (pressure && rs.predictedOutputSegs(kMaxSegmentBytes) < rs.inputSegCount) { useful = true; break; }
        }
        if (!useful) return;

        const auto chosen = chooseHighestDeadRatioCompact(rangeStats);
        if (!chosen) return;

        const CompactRangeStat* cstat = nullptr;
        for (const auto& rs : rangeStats) {
            if (rs.range.startGen == chosen->startGen && rs.range.endGen == chosen->endGen) {
                cstat = &rs; break;
            }
        }
        const auto target = cstat ? narrowCompactRange(*chosen, *cstat, store, kMaxSegmentBytes, maxOutputSegs) : *chosen;

        std::uint32_t outSegs = 0;
        const std::uint64_t simBefore = counting->counters().simLatencyUs;
        const auto st = store.compactRange(target, &outSegs);
        const std::uint64_t simDelta = counting->counters().simLatencyUs - simBefore;

        if (st.ok() && !st.isNoOp()) {
            ++result.compactions;
            result.maxOutSegs   = std::max(result.maxOutSegs, outSegs);
            result.maxLatencyUs = std::max(result.maxLatencyUs, simDelta);
        } else {
            ++result.noOps;
        }
    };

    for (std::uint32_t cycle = 0; cycle < cycles; ++cycle) {
        for (std::uint32_t i = 0; i < burstSize; ++i) {
            const auto st = store.writeRecord(nextSeq, payload);
            if (st.ok()) {
                pqueue::FileStoreIndex dummy;
                store.writeIndex(dummy);
                ++nextSeq;
                ++queueSize;
            } else {
                bool reclaimable = false;
                for (const auto& rs : buildCompactRangeStats(store))
                    if (rs.deadRatio() >= 0.01f) { reclaimable = true; break; }
                if (reclaimable) ++result.deadlocks;
                else             ++result.capExhausted;
            }
            checkAndCompact();
        }
        const std::uint32_t toPop = static_cast<std::uint32_t>(static_cast<float>(queueSize) * 0.90f);
        for (std::uint32_t i = 0; i < toPop; ++i) {
            pqueue::FileStoreIndex idx;
            if (store.readIndex(idx).ok() && idx.count > 0) {
                idx.head++; idx.count--;
                store.writeIndex(idx);
                --queueSize;
            }
            checkAndCompact();
        }
    }

    result.finalQSize = queueSize;
    result.fs = counting->counters();
    return result;
}

// ---------------------------------------------------------------------------
// Idle compaction scenario
// Models the intended usage pattern:
//   1. Enqueue a burst (no compaction during burst)
//   2. Drain (pop popRatio fraction)
//   3. compactIdle until clean (measure steps and per-step latency)
//   4. Next burst -- assert zero hot-path compactions
// ---------------------------------------------------------------------------

struct IdleCompactionResult {
    std::uint32_t burstSize     = 0;
    std::uint32_t payloadBytes  = 0;
    std::uint32_t cycles        = 0;
    std::uint32_t idleSteps     = 0;     // total compactIdle steps across all cycles
    std::uint32_t idleNoOps     = 0;
    std::uint64_t maxStepUs     = 0;     // worst single idle step
    std::uint64_t totalIdleUs   = 0;     // sum of all idle steps
    std::uint32_t hotCompactions = 0;    // compactions triggered on write path (should be 0)
    std::uint32_t deadlocks     = 0;
    std::uint32_t capExhausted  = 0;
    bool          ok            = true;
};

IdleCompactionResult scenarioIdleCompaction(
    std::uint32_t burstSize,
    std::uint32_t payloadBytes,
    std::uint32_t cycles,
    float popRatio = 0.90f,
    FsLatency latency = {})
{
    constexpr std::uint32_t kMaxSegmentBytes = 4096;
    constexpr std::uint32_t kMaxTotalBytes   = 1024 * 1024;

    auto memFs    = std::make_shared<MemoryFileSystem>();
    auto counting = std::make_shared<CountingFileSystem>(memFs);
    counting->setLatency(latency);

    pqueue::AppendLogConfig cfg;
    cfg.basePath        = "/idle_prof";
    cfg.backend         = pqueue::StorageBackend::Posix;
    cfg.maxSegmentBytes = kMaxSegmentBytes;
    cfg.maxSegments     = 200;
    cfg.maxTotalBytes   = kMaxTotalBytes;
    cfg.minFreeBytes    = 0;
    cfg.fileSystem      = counting;

    pqueue::AppendLogStore store(cfg);
    IdleCompactionResult result;
    result.burstSize   = burstSize;
    result.payloadBytes = payloadBytes;
    result.cycles      = cycles;

    if (!store.mount().ok()) { result.ok = false; return result; }

    const std::string payload(payloadBytes, 'x');
    std::uint32_t nextSeq   = 1;
    std::uint32_t queueSize = 0;

    for (std::uint32_t cycle = 0; cycle < cycles; ++cycle) {
        // Phase 1: enqueue burst -- detect hot-path compactions via readFile delta
        // Rotations do no readFile; compactions read input segments.
        for (std::uint32_t i = 0; i < burstSize; ++i) {
            const std::uint64_t rfBefore = counting->counters().readFile;
            const auto st = store.writeRecord(nextSeq, payload);
            if (counting->counters().readFile > rfBefore) ++result.hotCompactions;
            if (st.ok()) {
                pqueue::FileStoreIndex dummy;
                store.writeIndex(dummy);
                ++nextSeq;
                ++queueSize;
            } else {
                bool reclaimable = false;
                for (const auto& rs : buildCompactRangeStats(store))
                    if (rs.deadRatio() >= 0.01f) { reclaimable = true; break; }
                if (reclaimable) ++result.deadlocks;
                else             ++result.capExhausted;
            }
        }

        // Phase 2: drain
        const std::uint32_t toPop = static_cast<std::uint32_t>(static_cast<float>(queueSize) * popRatio);
        for (std::uint32_t i = 0; i < toPop; ++i) {
            pqueue::FileStoreIndex idx;
            if (store.readIndex(idx).ok() && idx.count > 0) {
                idx.head++; idx.count--;
                store.writeIndex(idx);
                --queueSize;
            }
        }

        // Phase 3: idle compaction -- run to completion
        for (;;) {
            const std::uint64_t simBefore = counting->counters().simLatencyUs;
            const auto cr = store.compactIdle(1);
            const std::uint64_t simDelta = counting->counters().simLatencyUs - simBefore;
            ++result.idleSteps;
            if (cr.compactions > 0) {
                result.maxStepUs   = std::max(result.maxStepUs, simDelta);
                result.totalIdleUs += simDelta;
            } else {
                ++result.idleNoOps;
                break;
            }
        }
    }

    return result;
}

void printIdleCompactionResult(const IdleCompactionResult& r, bool simMode) {
    std::printf("idle_compaction  burst=%u payload=%uB cycles=%u pop=90%%\n",
        r.burstSize, r.payloadBytes, r.cycles);
    std::printf("  idleSteps=%-4u idleNoOps=%-2u hotCompactions=%u\n",
        r.idleSteps, r.idleNoOps, r.hotCompactions);
    if (simMode) {
        std::printf("  maxStepLatency=%.1fms  totalIdleLatency=%.1fms\n",
            static_cast<double>(r.maxStepUs) / 1000.0,
            static_cast<double>(r.totalIdleUs) / 1000.0);
    }
    std::printf("  deadlocks=%-4u capExhausted=%-4u\n", r.deadlocks, r.capExhausted);
    if (!r.ok) std::printf("  FAILED: mount error\n");
    if (r.hotCompactions > 0)
        std::printf("  WARNING: hot-path compactions fired during burst -- clean-storage invariant violated\n");
}

void printCompactionBurstResult(const CompactionBurstResult& r) {
    std::printf("compaction_burst  burst=%u payload=%uB cycles=%u maxOutSegs=%u maxInSegs=%u\n",
        r.burstSize, r.payloadBytes, r.cycles, r.maxOutSegsLimit, r.maxInSegsLimit);
    std::printf("  compactions=%-4u noOps=%-4u maxOutSegs=%-3u simMaxLatency=%.1fms\n",
        r.compactions, r.noOps, r.maxOutSegs,
        static_cast<double>(r.maxLatencyUs) / 1000.0);
    std::printf("  deadlocks=%-4u capExhausted=%-4u finalQ=%-4u\n",
        r.deadlocks, r.capExhausted, r.finalQSize);
    std::printf("  readFile=%-6lu readAt=%-6lu writeFile=%-6lu listFiles=%-6lu removeFile=%-6lu\n",
        (unsigned long)r.fs.readFile, (unsigned long)r.fs.readAt, (unsigned long)r.fs.writeFile,
        (unsigned long)r.fs.listFiles, (unsigned long)r.fs.removeFile);
    if (!r.ok) std::printf("  FAILED: mount error\n");
}

void printHeader() {
    std::cout << std::left
              << std::setw(24) << "scenario"
              << std::right
              << std::setw(8) << "records"
              << std::setw(9) << "payload"
              << std::setw(10) << "avg_us"
              << std::setw(10) << "p95_us"
              << std::setw(10) << "max_us"
              << std::setw(8) << "readAt"
              << std::setw(8) << "writeAt"
              << std::setw(10) << "writeFile"
              << std::setw(8) << "rename"
              << std::setw(8) << "remove"
              << std::setw(12) << "bytesW"
              << std::setw(12) << "bytesR"
              << std::setw(12) << "status"
              << "\n";
}

void printResult(const ScenarioResult& r) {
    std::cout << std::left
              << std::setw(24) << r.scenario
              << std::right
              << std::setw(8) << r.records
              << std::setw(9) << r.payloadBytes
              << std::setw(10) << static_cast<std::uint64_t>(r.timings.avg())
              << std::setw(10) << static_cast<std::uint64_t>(r.timings.p95())
              << std::setw(10) << static_cast<std::uint64_t>(r.timings.max())
              << std::setw(8) << r.fs.readAt
              << std::setw(8) << r.fs.writeAt
              << std::setw(10) << r.fs.writeFile
              << std::setw(8) << r.fs.renameFile
              << std::setw(8) << r.fs.removeFile
              << std::setw(12) << r.fs.bytesWritten
              << std::setw(12) << r.fs.bytesRead
              << std::setw(12) << pqueue::statusCodeName(r.status.code)
              << "\n";
}

void printRedFlags(const std::vector<ScenarioResult>& results) {
    std::cout << "\nred-flag notes:\n";
    for (const auto& r : results) {
        if (!r.status.ok()) {
            std::cout << "- " << r.scenario << " failed: " << pqueue::statusCodeName(r.status.code)
                      << " (" << r.status.message << ")\n";
        }
        if ((r.scenario == "queue_enqueue" || r.scenario == "http_offline_submit") && r.fs.writeAt > r.records * 3ULL + 16ULL) {
            std::cout << "- " << r.scenario << " has unexpectedly high writeAt count; expected slot + journal/checkpoint writes only\n";
        }
        if (r.scenario == "queue_enqueue" && r.fs.readAt > r.records * 2ULL + 8ULL) {
            std::cout << "- " << r.scenario << " has unexpected readAt activity on enqueue hot path\n";
        }
        if (r.scenario == "http_offline_submit" && r.fs.readAt > r.records * 3ULL + 8ULL) {
            std::cout << "- " << r.scenario << " has unexpected readAt activity on http submit hot path\n";
        }
        if (r.scenario == "validate_full" && r.fs.writeAt != 0) {
            std::cout << "- validate_full wrote to storage; validation should be read-only\n";
        }
        if (r.records > 0 && r.fs.metadataOps() > r.records * 10ULL) {
            std::cout << "- " << r.scenario << " has high metadata op count; inspect atomic metadata path\n";
        }
    }
}

void usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [queue|validate|outbox|all] [records] [payloadBytes]\n";
}

} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc >= 2 ? argv[1] : "all";
    const std::uint32_t records = argc >= 3 ? static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10)) : 500;
    const std::uint32_t payloadBytes = argc >= 4 ? static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10)) : 256;

    if (mode != "queue" && mode != "validate" && mode != "outbox" && mode != "all"
        && mode != "compaction" && mode != "compaction-sim"
        && mode != "idle" && mode != "idle-sim") {
        usage(argv[0]);
        return 2;
    }

    std::vector<ScenarioResult> results;
    if (mode == "queue" || mode == "all") {
        results.push_back(scenarioQueueEnqueue(records, payloadBytes));
        results.push_back(scenarioQueuePeekPop(records, payloadBytes));
    }
    if (mode == "validate" || mode == "all") {
        results.push_back(scenarioValidateFull(records, payloadBytes));
    }
    if (mode == "outbox" || mode == "all") {
        results.push_back(scenarioHttpOutboxOfflineSubmit(records, payloadBytes));
        results.push_back(scenarioHttpOutboxDrainBacklog(records, payloadBytes));
    }

    if (mode == "compaction" || mode == "compaction-sim") {
        const std::uint32_t cycles = argc >= 5 ? static_cast<std::uint32_t>(std::strtoul(argv[4], nullptr, 10)) : 3;
        const FsLatency latency = (mode == "compaction-sim") ? littleFsSimLatency() : FsLatency{};
        printCompactionBurstResult(scenarioCompactionBurst(records, payloadBytes, cycles, 8, 0.10f, 3, latency));
        return 0;
    }

    if (mode == "idle" || mode == "idle-sim") {
        const std::uint32_t cycles = argc >= 5 ? static_cast<std::uint32_t>(std::strtoul(argv[4], nullptr, 10)) : 3;
        const FsLatency latency = (mode == "idle-sim") ? littleFsSimLatency() : FsLatency{};
        printIdleCompactionResult(scenarioIdleCompaction(records, payloadBytes, cycles, 0.90f, latency),
                                  mode == "idle-sim");
        return 0;
    }

    printHeader();
    for (const auto& r : results) {
        printResult(r);
    }
    printRedFlags(results);

    const bool failed = std::any_of(results.begin(), results.end(), [](const ScenarioResult& r) { return !r.status.ok(); });
    return failed ? 1 : 0;
}

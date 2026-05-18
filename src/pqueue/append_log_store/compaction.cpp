#include "pqueue/append_log_store.h"
#include "pqueue/append_log_common.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace pqueue {

using namespace append_log_detail;

std::optional<AppendLogStore::CompactionRange> AppendLogStore::chooseCompactionRange() const {
    if (manifestRanges_.empty()) return std::nullopt;

    const auto stats = segmentStats();

    std::size_t bestIdx   = manifestRanges_.size();
    float       bestRatio = -1.0f;
    for (std::size_t i = 0; i < manifestRanges_.size(); ++i) {
        const auto& r = manifestRanges_[i];

        bool hasLive = false;
        for (const auto& rec : records_) {
            if (rec.segmentGeneration >= r.startGen && rec.segmentGeneration <= r.endGen) {
                hasLive = true;
                break;
            }
        }
        if (!hasLive) {
            return CompactionRange{r.startGen, r.endGen};
        }

        std::uint32_t total = 0;
        std::uint32_t dead  = 0;
        for (const auto& s : stats) {
            if (s.generation < r.startGen || s.generation > r.endGen) continue;
            total += s.totalBytes;
            dead  += s.deadBytes();
        }
        if (total == 0) continue;
        const float ratio = static_cast<float>(dead) / static_cast<float>(total);
        if (ratio > bestRatio) {
            bestRatio = ratio;
            bestIdx   = i;
        }
    }

    if (bestRatio <= 0.0f || bestIdx >= manifestRanges_.size()) return std::nullopt;
    const auto& best = manifestRanges_[bestIdx];
    return CompactionRange{best.startGen, best.endGen};
}

Status AppendLogStore::collectLiveRecords(const CompactionRange& range,
                                          std::vector<CompactionLiveRecord>& out) const {
    out.clear();
    for (const SegmentRecord& sr : records_) {
        if (sr.segmentGeneration < range.startGen || sr.segmentGeneration > range.endGen) {
            continue;
        }
        CompactionLiveRecord lr;
        lr.sequence = sr.sequence;
        Status st = fs_->readAt(segmentName(sr.segmentGeneration),
                                sr.payloadOffset, sr.payloadBytes, lr.payload);
        if (!st.ok()) return st;
        out.push_back(std::move(lr));
    }
    return Status::success();
}

std::vector<AppendLogStore::SegmentStat> AppendLogStore::segmentStats() const {
    std::vector<SegmentStat> result;

    std::unordered_map<std::uint32_t, std::uint32_t> liveByGen;
    for (const auto& sr : records_) {
        liveByGen[sr.segmentGeneration] += kEnqueueOverheadBytes + sr.payloadBytes;
    }

    for (const auto& range : manifestRanges_) {
        for (std::uint32_t gen = range.startGen; gen <= range.endGen; ++gen) {
            SegmentStat stat;
            stat.generation = gen;
            auto it = liveByGen.find(gen);
            stat.liveBytes = kSegmentHeaderBytes + (it != liveByGen.end() ? it->second : 0);
            std::uint64_t size = 0;
            fs_->fileSize(segmentName(gen), size);
            stat.totalBytes = static_cast<std::uint32_t>(size);
            result.push_back(stat);
        }
    }

    return result;
}

Status AppendLogStore::compactRange(const CompactionRange& range, std::uint32_t* outputSegCount) {
    auto it = std::find_if(manifestRanges_.begin(), manifestRanges_.end(),
        [&](const ManifestRange& r) {
            return r.startGen == range.startGen && r.endGen == range.endGen;
        });
    if (it == manifestRanges_.end()) return Status::noOp();
    const std::size_t rangeIdx = static_cast<std::size_t>(it - manifestRanges_.begin());

    std::vector<CompactionLiveRecord> liveRecords;
    Status st = collectLiveRecords(range, liveRecords);
    if (!st.ok()) return st;

    if (liveRecords.empty()) {
        std::vector<ManifestRange> newRanges = manifestRanges_;
        newRanges.erase(newRanges.begin() + static_cast<std::ptrdiff_t>(rangeIdx));
        ManifestData md;
        md.ranges         = std::move(newRanges);
        md.tailGeneration = activeGeneration_;
        md.nextGeneration = nextGeneration_;
        if (outputSegCount) *outputSegCount = 0;
        return publishManifest(md);
    }

    struct OutputSeg {
        std::uint32_t gen;
        std::vector<std::size_t> indices;
    };
    std::vector<OutputSeg> outputSegs;
    {
        std::uint32_t gen = nextGeneration_;
        std::uint32_t segBytes = kSegmentHeaderBytes;
        outputSegs.push_back({gen, {}});
        for (std::size_t i = 0; i < liveRecords.size(); ++i) {
            const std::uint32_t recBytes = kEnqueueOverheadBytes +
                static_cast<std::uint32_t>(liveRecords[i].payload.size());
            if (!outputSegs.back().indices.empty() && segBytes + recBytes > config_.maxSegmentBytes) {
                outputSegs.push_back({++gen, {}});
                segBytes = kSegmentHeaderBytes;
            }
            outputSegs.back().indices.push_back(i);
            segBytes += recBytes;
        }
    }

    const std::size_t inputSegCount = static_cast<std::size_t>(range.endGen - range.startGen + 1);

    // Compute total live bytes across all output segments.
    std::uint32_t totalLiveBytes = 0;
    for (const auto& seg : outputSegs) {
        totalLiveBytes += kSegmentHeaderBytes;
        for (std::size_t idx : seg.indices) {
            totalLiveBytes += kEnqueueOverheadBytes +
                static_cast<std::uint32_t>(liveRecords[idx].payload.size());
        }
    }
    // Compute total input bytes from disk.
    std::uint32_t totalInputBytes = 0;
    for (std::uint32_t gen = range.startGen; gen <= range.endGen; ++gen) {
        std::uint64_t sz = 0;
        Status sizeSt = fs()->fileSize(segmentName(gen), sz);
        if (!sizeSt.ok()) return sizeSt;
        totalInputBytes += static_cast<std::uint32_t>(sz);
    }
    const bool hasDeadBytes = totalLiveBytes < totalInputBytes;

    // Never allow compaction to increase segment count — that worsens manifest pressure.
    if (outputSegs.size() > inputSegCount) return Status::noOp();
    // Same-count compaction is only useful when dead bytes exist to reclaim.
    if (outputSegs.size() == inputSegCount && !hasDeadBytes) return Status::noOp();
    if (outputSegCount) *outputSegCount = static_cast<std::uint32_t>(outputSegs.size());

    const std::uint32_t firstNewGen = outputSegs.front().gen;
    const std::uint32_t lastNewGen  = outputSegs.back().gen;

    std::vector<SegmentRecord> replacements;
    replacements.reserve(liveRecords.size());

    for (const auto& seg : outputSegs) {
        std::uint32_t writeOffset = kSegmentHeaderBytes;
        std::string segData = serializeSegmentHeader(seg.gen, liveRecords[seg.indices.front()].sequence);
        for (std::size_t idx : seg.indices) {
            const auto& lr = liveRecords[idx];
            SegmentRecord sr;
            sr.sequence          = lr.sequence;
            sr.segmentGeneration = seg.gen;
            sr.payloadOffset     = writeOffset + kEnqueueHeaderBytes;
            sr.payloadBytes      = static_cast<std::uint32_t>(lr.payload.size());
            replacements.push_back(sr);
            segData += serializeEnqueueEvent(lr.sequence, lr.payload);
            writeOffset += kEnqueueOverheadBytes + sr.payloadBytes;
        }
        st = writeSegmentFileTracked(segmentName(seg.gen), segData);
        if (!st.ok()) return st;
    }

    std::vector<ManifestRange> newRanges = manifestRanges_;
    newRanges[rangeIdx] = {firstNewGen, lastNewGen};
    // Merge with following range if contiguous.
    if (rangeIdx + 1 < newRanges.size() && newRanges[rangeIdx].endGen + 1 == newRanges[rangeIdx + 1].startGen) {
        newRanges[rangeIdx].endGen = newRanges[rangeIdx + 1].endGen;
        newRanges.erase(newRanges.begin() + static_cast<std::ptrdiff_t>(rangeIdx) + 1);
    }
    // Merge with preceding range if contiguous.
    if (rangeIdx > 0 && newRanges[rangeIdx - 1].endGen + 1 == newRanges[rangeIdx].startGen) {
        newRanges[rangeIdx - 1].endGen = newRanges[rangeIdx].endGen;
        newRanges.erase(newRanges.begin() + static_cast<std::ptrdiff_t>(rangeIdx));
    }

    ManifestData md;
    md.ranges         = std::move(newRanges);
    md.tailGeneration = activeGeneration_;
    md.nextGeneration = lastNewGen + 1;

    st = publishManifest(md);
    if (!st.ok()) return st;

    for (auto& r : records_) {
        if (r.segmentGeneration < range.startGen || r.segmentGeneration > range.endGen) continue;
        for (const auto& rep : replacements) {
            if (rep.sequence == r.sequence) { r = rep; break; }
        }
    }
    return Status::success();
}

Status AppendLogStore::compactOneSegment() {
    auto rangeOpt = chooseCompactionRange();
    if (!rangeOpt) return Status::noOp();
    return compactRange(*rangeOpt);
}

Status AppendLogStore::compactFull() {
    const std::size_t initialCount = manifestRanges_.size();
    for (std::size_t i = 0; i < initialCount; ++i) {
        Status st = compactOneSegment();
        if (!st.ok()) return st;
        if (st.isNoOp()) break;
    }
    return Status::success();
}

bool AppendLogStore::needsCompaction() const {
    if (activeGenerations_.size() > config_.maxSegments) return true;

    const std::uint64_t free = fs_ ? fs_->freeBytes() : 0;
    if (free < config_.minFreeBytes) return true;

    return false;
}

void AppendLogStore::cleanupOneDanglingSegment() {
    auto f = fs();
    if (!f) return;

    std::vector<std::string> files;
    if (!f->listFiles(files).ok()) return;

    for (const auto& name : files) {
        std::uint32_t gen = 0;
        if (!isSegmentName(name, gen)) continue;
        const bool live = std::find(activeGenerations_.begin(), activeGenerations_.end(), gen)
                          != activeGenerations_.end();
        if (live) continue;
        std::uint64_t sz = 0;
        f->fileSize(name, sz);
        if (f->removeFile(name).ok()) {
            totalOnDiskBytes_ -= static_cast<std::uint32_t>(sz);
        }
        return;
    }
}

} // namespace pqueue

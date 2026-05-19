#include "pqueue/append_log_store.h"
#include "pqueue/append_log_common.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
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
    // Reject unknown ranges immediately, before any preflight work.
    {
        bool found = false;
        for (const auto& r : manifestRanges_) {
            if (r.startGen == range.startGen && r.endGen == range.endGen) { found = true; break; }
        }
        if (!found) return Status::noOp();
    }

    // Rotate-before-compact: when the target is the last manifest range and the active
    // tail is contiguous with it, seal the tail into the range first. This prevents
    // generation gap fragmentation: without the rotate, compaction output gens are
    // numerically above the former tail gen, making the former tail an un-mergeable
    // orphan range that accumulates until range pressure triggers a massive compaction.
    // Only rotate when the tail will merge (not add a new range), preserving range count.
    const bool selectedIsLastRange =
        !manifestRanges_.empty() &&
        range.startGen == manifestRanges_.back().startGen &&
        range.endGen   == manifestRanges_.back().endGen;
    const bool tailCanMergeWithLastRange =
        activeSegmentBytes_ > kSegmentHeaderBytes &&
        !manifestRanges_.empty() &&
        manifestRanges_.back().endGen + 1 == activeGeneration_;
    const bool wouldRotate = selectedIsLastRange && tailCanMergeWithLastRange;

    // Preflight: compute usefulness on the hypothetical effective range (including the
    // tail if we would rotate) WITHOUT mutating store state. This ensures we never
    // rotate and then return noOp — which would be a state-mutating no-op.
    const std::uint32_t hypoStartGen = range.startGen;
    const std::uint32_t hypoEndGen   = wouldRotate ? activeGeneration_ : range.endGen;

    // Gather live record sizes from RAM (no disk read needed for preflight).
    std::vector<std::uint32_t> hypoPayloadBytes;
    for (const auto& sr : records_) {
        if (sr.segmentGeneration >= hypoStartGen && sr.segmentGeneration <= hypoEndGen) {
            hypoPayloadBytes.push_back(sr.payloadBytes);
        }
    }

    // Bin-pack live records to estimate output segment count.
    std::uint32_t hypoOutputSegs = 0;
    if (!hypoPayloadBytes.empty()) {
        std::uint32_t segBytes = kSegmentHeaderBytes;
        hypoOutputSegs = 1;
        for (const std::uint32_t pay : hypoPayloadBytes) {
            const std::uint32_t recBytes = kEnqueueOverheadBytes + pay;
            if (segBytes + recBytes > config_.maxSegmentBytes) {
                ++hypoOutputSegs;
                segBytes = kSegmentHeaderBytes;
            }
            segBytes += recBytes;
        }
    }

    const std::uint32_t hypoInputSegs = hypoEndGen - hypoStartGen + 1;

    // Compute hypothetical input bytes (sealed segments from disk + active tail from RAM).
    std::uint32_t hypoInputBytes = 0;
    for (std::uint32_t gen = hypoStartGen; gen <= hypoEndGen; ++gen) {
        if (gen == activeGeneration_) {
            hypoInputBytes += activeSegmentBytes_;
        } else {
            std::uint64_t sz = 0;
            const Status szSt = fs()->fileSize(segmentName(gen), sz);
            if (!szSt.ok()) return szSt;
            hypoInputBytes += static_cast<std::uint32_t>(sz);
        }
    }

    // Compute hypothetical live bytes (what would be written if compaction ran).
    std::uint32_t hypoLiveBytes = 0;
    if (!hypoPayloadBytes.empty()) {
        hypoLiveBytes = hypoOutputSegs * kSegmentHeaderBytes;
        for (const std::uint32_t pay : hypoPayloadBytes) {
            hypoLiveBytes += kEnqueueOverheadBytes + pay;
        }
    }
    const bool hypoHasDeadBytes = hypoLiveBytes < hypoInputBytes;

    // Apply no-op gate before any mutation. Dead range (no live records) is always useful.
    if (!hypoPayloadBytes.empty()) {
        if (hypoOutputSegs > hypoInputSegs) return Status::noOp();
        if (hypoOutputSegs == hypoInputSegs && !hypoHasDeadBytes) return Status::noOp();
    }

    // Compaction is useful. Rotate first if needed (only when range has live records;
    // dead-range removal produces no output so there is no generation gap to prevent).
    // RangeLimitExceeded is unreachable here: rotateSegment() merges the contiguous
    // tail into the last range before checking the limit, so count stays the same.
    if (wouldRotate && !hypoPayloadBytes.empty()) {
        Status rotSt = rotateSegment();
        if (!rotSt.ok()) return rotSt;
    }

    // Re-resolve effective range: the rotate above may have extended the last range
    // by merging the former tail into it (e.g. {1,59} + tail-60 → {1,60}).
    CompactionRange effectiveRange = range;
    for (const auto& r : manifestRanges_) {
        if (r.startGen <= range.startGen && range.startGen <= r.endGen) {
            effectiveRange = {r.startGen, r.endGen};
            break;
        }
    }

    auto it = std::find_if(manifestRanges_.begin(), manifestRanges_.end(),
        [&](const ManifestRange& r) {
            return r.startGen == effectiveRange.startGen && r.endGen == effectiveRange.endGen;
        });
    if (it == manifestRanges_.end()) return Status::noOp();
    const std::size_t rangeIdx = static_cast<std::size_t>(it - manifestRanges_.begin());

    std::vector<CompactionLiveRecord> liveRecords;
    Status st = collectLiveRecords(effectiveRange, liveRecords);
    if (!st.ok()) return st;

    if (liveRecords.empty()) {
        const std::uint32_t inputSegCount = effectiveRange.endGen - effectiveRange.startGen + 1;
        std::vector<ManifestRange> newRanges = manifestRanges_;
        newRanges.erase(newRanges.begin() + static_cast<std::ptrdiff_t>(rangeIdx));
        ManifestData md;
        md.ranges         = std::move(newRanges);
        md.tailGeneration = activeGeneration_;
        md.nextGeneration = nextGeneration_;
        if (outputSegCount) *outputSegCount = 0;
        st = publishManifest(md);
        if (!st.ok()) return st;
        for (std::uint32_t i = 1; i < inputSegCount; ++i) cleanupOneDanglingSegment();
        return Status::success();
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

    const std::uint32_t inputSegCount = effectiveRange.endGen - effectiveRange.startGen + 1;

    st = publishManifest(md);
    if (!st.ok()) return st;

    // publishManifest cleans one dangling segment. Clean up all remaining
    // input segments (now dangling) beyond the one it already handled.
    for (std::uint32_t i = 1; i < inputSegCount; ++i) cleanupOneDanglingSegment();

    for (auto& r : records_) {
        if (r.segmentGeneration < effectiveRange.startGen ||
            r.segmentGeneration > effectiveRange.endGen) continue;
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

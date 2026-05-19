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
            fs_->fileSize(segmentName(gen), size); // missing file == 0 bytes
            stat.totalBytes = static_cast<std::uint32_t>(size);
            result.push_back(stat);
        }
    }

    return result;
}

Status AppendLogStore::compactRange(const CompactionRange& range, std::uint32_t* outputSegCount) {
#ifdef ARDUINO
    const std::uint32_t t_start = millis();
    std::uint32_t ms_exist = 0, ms_pre_scan = 0, ms_pre_size = 0;
    std::uint32_t ms_rotate = 0, ms_resolve = 0, ms_collect = 0;
    std::uint32_t ms_pack = 0, ms_write = 0, ms_publish = 0;
    std::uint32_t ms_cleanup = 0, ms_replace = 0;
    std::uint32_t eff_s = range.startGen, eff_e = range.endGen;
    std::uint32_t hypo_e = range.endGen;
    bool did_rotate = false;
    std::uint32_t n_live = 0, n_in = 0, n_out = 0;
    auto logLine = [&](const char* status) {
        Serial.printf(
            "[compactRange] req=[%u,%u] hypoEnd=%u eff=[%u,%u] rotate=%u "
            "liveRecs=%u inSegs=%u outSegs=%u "
            "exist_ms=%u pre_scan_ms=%u pre_size_ms=%u "
            "rotate_ms=%u resolve_ms=%u collect_ms=%u "
            "pack_ms=%u write_ms=%u publish_ms=%u "
            "cleanup_ms=%u replace_ms=%u total_ms=%u %s\n",
            range.startGen, range.endGen, hypo_e, eff_s, eff_e,
            static_cast<unsigned>(did_rotate),
            n_live, n_in, n_out,
            ms_exist, ms_pre_scan, ms_pre_size,
            ms_rotate, ms_resolve, ms_collect,
            ms_pack, ms_write, ms_publish,
            ms_cleanup, ms_replace, millis() - t_start, status);
        Serial.flush();
    };
    #define CR_T0(var) const std::uint32_t _t0_##var = millis()
    #define CR_T1(var) var = millis() - _t0_##var
#else
    #define CR_T0(var)
    #define CR_T1(var)
#endif

    // Phase: range existence check.
    CR_T0(ms_exist);
    {
        bool found = false;
        for (const auto& r : manifestRanges_) {
            if (r.startGen == range.startGen && r.endGen == range.endGen) { found = true; break; }
        }
        if (!found) {
            CR_T1(ms_exist);
#ifdef ARDUINO
            logLine("noOp(notFound)");
#endif
            return Status::noOp();
        }
    }
    CR_T1(ms_exist);

    const bool selectedIsLastRange =
        !manifestRanges_.empty() &&
        range.startGen == manifestRanges_.back().startGen &&
        range.endGen   == manifestRanges_.back().endGen;
    const bool tailCanMergeWithLastRange =
        activeSegmentBytes_ > kSegmentHeaderBytes &&
        !manifestRanges_.empty() &&
        manifestRanges_.back().endGen + 1 == activeGeneration_;
    const bool wouldRotate = selectedIsLastRange && tailCanMergeWithLastRange;

    const std::uint32_t hypoStartGen = range.startGen;
    const std::uint32_t hypoEndGen   = wouldRotate ? activeGeneration_ : range.endGen;
#ifdef ARDUINO
    hypo_e = hypoEndGen;
#endif

    // Phase: preflight live scan (RAM only).
    CR_T0(ms_pre_scan);
    std::vector<std::uint32_t> hypoPayloadBytes;
    for (const auto& sr : records_) {
        if (sr.segmentGeneration >= hypoStartGen && sr.segmentGeneration <= hypoEndGen) {
            hypoPayloadBytes.push_back(sr.payloadBytes);
        }
    }
    CR_T1(ms_pre_scan);

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

    // Phase: preflight fileSize per input generation.
    CR_T0(ms_pre_size);
    std::uint32_t hypoInputBytes = 0;
    for (std::uint32_t gen = hypoStartGen; gen <= hypoEndGen; ++gen) {
        if (gen == activeGeneration_) {
            hypoInputBytes += activeSegmentBytes_;
        } else {
            std::uint64_t sz = 0;
            fs()->fileSize(segmentName(gen), sz);
            hypoInputBytes += static_cast<std::uint32_t>(sz);
        }
    }
    CR_T1(ms_pre_size);

    std::uint32_t hypoLiveBytes = 0;
    if (!hypoPayloadBytes.empty()) {
        hypoLiveBytes = hypoOutputSegs * kSegmentHeaderBytes;
        for (const std::uint32_t pay : hypoPayloadBytes) {
            hypoLiveBytes += kEnqueueOverheadBytes + pay;
        }
    }
    const bool hypoHasDeadBytes = hypoLiveBytes < hypoInputBytes;

    if (!hypoPayloadBytes.empty()) {
        if (hypoOutputSegs > hypoInputSegs) {
#ifdef ARDUINO
            logLine("noOp(wouldExpand)");
#endif
            return Status::noOp();
        }
        if (hypoOutputSegs == hypoInputSegs && !hypoHasDeadBytes) {
#ifdef ARDUINO
            logLine("noOp(noDead)");
#endif
            return Status::noOp();
        }
    }

    // Phase: rotateSegment.
    CR_T0(ms_rotate);
    if (wouldRotate && !hypoPayloadBytes.empty()) {
        Status rotSt = rotateSegment();
        if (!rotSt.ok()) {
            CR_T1(ms_rotate);
#ifdef ARDUINO
            logLine("err(rotate)");
#endif
            return rotSt;
        }
#ifdef ARDUINO
        did_rotate = true;
#endif
    }
    CR_T1(ms_rotate);

    // Phase: re-resolve effective range.
    CR_T0(ms_resolve);
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
    CR_T1(ms_resolve);
    if (it == manifestRanges_.end()) {
#ifdef ARDUINO
        eff_s = effectiveRange.startGen; eff_e = effectiveRange.endGen;
        logLine("noOp(lostRange)");
#endif
        return Status::noOp();
    }
    const std::size_t rangeIdx = static_cast<std::size_t>(it - manifestRanges_.begin());
#ifdef ARDUINO
    eff_s = effectiveRange.startGen; eff_e = effectiveRange.endGen;
    n_in  = effectiveRange.endGen - effectiveRange.startGen + 1;
#endif

    // Phase: collectLiveRecords.
    CR_T0(ms_collect);
    std::vector<CompactionLiveRecord> liveRecords;
    Status st = collectLiveRecords(effectiveRange, liveRecords);
    CR_T1(ms_collect);
    if (!st.ok()) {
#ifdef ARDUINO
        logLine("err(collect)");
#endif
        return st;
    }
#ifdef ARDUINO
    n_live = static_cast<std::uint32_t>(liveRecords.size());
#endif

    if (liveRecords.empty()) {
        const std::uint32_t inputSegCount = effectiveRange.endGen - effectiveRange.startGen + 1;
        std::vector<ManifestRange> newRanges = manifestRanges_;
        newRanges.erase(newRanges.begin() + static_cast<std::ptrdiff_t>(rangeIdx));
        ManifestData md;
        md.ranges         = std::move(newRanges);
        md.tailGeneration = activeGeneration_;
        md.nextGeneration = nextGeneration_;
        if (outputSegCount) *outputSegCount = 0;
        CR_T0(ms_publish);
        st = publishManifest(md);
        CR_T1(ms_publish);
        if (!st.ok()) {
#ifdef ARDUINO
            logLine("err(publish/dead)");
#endif
            return st;
        }
        CR_T0(ms_cleanup);
        for (std::uint32_t i = 1; i < inputSegCount; ++i) cleanupOneDanglingSegment();
        CR_T1(ms_cleanup);
#ifdef ARDUINO
        logLine("ok(dead)");
#endif
        return Status::success();
    }

    // Phase: pack output segments (in-memory bin-packing).
    CR_T0(ms_pack);
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
    CR_T1(ms_pack);

    if (outputSegCount) *outputSegCount = static_cast<std::uint32_t>(outputSegs.size());
#ifdef ARDUINO
    n_out = static_cast<std::uint32_t>(outputSegs.size());
#endif

    const std::uint32_t firstNewGen = outputSegs.front().gen;
    const std::uint32_t lastNewGen  = outputSegs.back().gen;

    std::vector<SegmentRecord> replacements;
    replacements.reserve(liveRecords.size());

    // Phase: write output segment files.
    CR_T0(ms_write);
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
        st = writeSegmentFileTracked(segmentName(seg.gen), segData, SegmentWriteDisposition::MustBeNew);
        if (!st.ok()) {
            CR_T1(ms_write);
#ifdef ARDUINO
            logLine("err(write)");
#endif
            return st;
        }
    }
    CR_T1(ms_write);

    std::vector<ManifestRange> newRanges = manifestRanges_;
    newRanges[rangeIdx] = {firstNewGen, lastNewGen};
    if (rangeIdx + 1 < newRanges.size() && newRanges[rangeIdx].endGen + 1 == newRanges[rangeIdx + 1].startGen) {
        newRanges[rangeIdx].endGen = newRanges[rangeIdx + 1].endGen;
        newRanges.erase(newRanges.begin() + static_cast<std::ptrdiff_t>(rangeIdx) + 1);
    }
    if (rangeIdx > 0 && newRanges[rangeIdx - 1].endGen + 1 == newRanges[rangeIdx].startGen) {
        newRanges[rangeIdx - 1].endGen = newRanges[rangeIdx].endGen;
        newRanges.erase(newRanges.begin() + static_cast<std::ptrdiff_t>(rangeIdx));
    }

    ManifestData md;
    md.ranges         = std::move(newRanges);
    md.tailGeneration = activeGeneration_;
    md.nextGeneration = lastNewGen + 1;

    const std::uint32_t inputSegCount = effectiveRange.endGen - effectiveRange.startGen + 1;

    // Phase: publishManifest.
    CR_T0(ms_publish);
    st = publishManifest(md);
    CR_T1(ms_publish);
    if (!st.ok()) {
#ifdef ARDUINO
        logLine("err(publish)");
#endif
        return st;
    }

    // Phase: cleanup dangling input segments.
    CR_T0(ms_cleanup);
    for (std::uint32_t i = 1; i < inputSegCount; ++i) cleanupOneDanglingSegment();
    CR_T1(ms_cleanup);

    // Phase: update in-RAM records to point at new segment generations.
    CR_T0(ms_replace);
    for (auto& r : records_) {
        if (r.segmentGeneration < effectiveRange.startGen ||
            r.segmentGeneration > effectiveRange.endGen) continue;
        for (const auto& rep : replacements) {
            if (rep.sequence == r.sequence) { r = rep; break; }
        }
    }
    CR_T1(ms_replace);

#ifdef ARDUINO
    logLine("ok");
#endif

#undef CR_T0
#undef CR_T1

    return Status::success();
}

#ifndef ARDUINO
// Silence unused-macro warnings on Posix where CR_T0/CR_T1 expand to nothing
// and are #undef-ed inside compactRange above.
#endif

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

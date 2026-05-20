#include "pqueue/append_log_store.h"
#include "pqueue/append_log_common.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
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

    // Group live records by segment so we read each segment file once.
    std::map<std::uint32_t, std::vector<const SegmentRecord*>> byGen;
    for (const SegmentRecord& sr : records_) {
        if (sr.segmentGeneration >= range.startGen && sr.segmentGeneration <= range.endGen)
            byGen[sr.segmentGeneration].push_back(&sr);
    }

    for (auto& [gen, recs] : byGen) {
        std::string buf;
        Status st = fs_->readFile(segmentName(gen), buf);
        if (!st.ok()) return st;
        for (const SegmentRecord* sr : recs) {
            if (sr->payloadOffset + sr->payloadBytes > buf.size())
                return Status::failure(StatusCode::DataCorrupt, "payload out of bounds in segment");
            CompactionLiveRecord lr;
            lr.sequence = sr->sequence;
            lr.payload  = buf.substr(sr->payloadOffset, sr->payloadBytes);
            out.push_back(std::move(lr));
        }
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
            auto sit = sealedSegmentBytes_.find(gen);
            stat.totalBytes = sit != sealedSegmentBytes_.end() ? sit->second : 0;
            result.push_back(stat);
        }
    }

    return result;
}

std::size_t AppendLogStore::findParentRangeIdx(std::uint32_t startGen, std::uint32_t endGen) const {
    if (startGen > endGen) return manifestRanges_.size();
    for (std::size_t i = 0; i < manifestRanges_.size(); ++i) {
        const auto& r = manifestRanges_[i];
        if (r.startGen <= startGen && endGen <= r.endGen)
            return i;
    }
    return manifestRanges_.size();
}

Status AppendLogStore::compactRange(const CompactionRange& range,
                                    std::uint32_t* outputSegCount,
                                    AllowFullRangeFallback allowFallback) {
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

    // Phase: find parent range and classify the input as prefix/suffix/middle/exact.
    CR_T0(ms_exist);
    std::size_t rangeIdx = findParentRangeIdx(range.startGen, range.endGen);
    if (rangeIdx >= manifestRanges_.size()) {
        CR_T1(ms_exist);
#ifdef ARDUINO
        logLine("noOp(notFound)");
#endif
        return Status::noOp();
    }

    ManifestRange parent = manifestRanges_[rangeIdx];
    bool hasLeft  = (range.startGen > parent.startGen);
    bool hasRight = (range.endGen   < parent.endGen);

    // Quick RAM scan: does the subrange contain any live records?
    // Dead prefix/suffix costs 0 extra ranges; dead middle costs +1.
    // Using the live delta for dead subranges would block dead-range cleanup
    // at range count 4 even though no new ranges would be created.
    const bool rangeHasLive = std::any_of(records_.begin(), records_.end(),
        [&](const SegmentRecord& sr) {
            return sr.segmentGeneration >= range.startGen &&
                   sr.segmentGeneration <= range.endGen;
        });
    const std::uint32_t liveDelta = (hasLeft && hasRight) ? 2u : (hasLeft || hasRight) ? 1u : 0u;
    const std::uint32_t deadDelta = (hasLeft && hasRight) ? 1u : 0u;
    const std::uint32_t gateDelta = rangeHasLive ? liveDelta : deadDelta;

    // Range-count gate: if the split would exceed kManifestMaxRanges, fall back to
    // the full parent (maintenance path) or return noOp (hot path).
    CompactionRange inputRange = range;
    if (manifestRanges_.size() + gateDelta > kManifestMaxRanges) {
        if (allowFallback == AllowFullRangeFallback::no) {
            CR_T1(ms_exist);
#ifdef ARDUINO
            logLine("noOp(splitLimit)");
#endif
            return Status::noOp();
        }
        inputRange = {parent.startGen, parent.endGen};
        hasLeft  = false;
        hasRight = false;
    }
    CR_T1(ms_exist);

    const bool subrangeReachesLastGen =
        !manifestRanges_.empty() &&
        inputRange.endGen == manifestRanges_.back().endGen;
    const bool tailCanMergeWithLastRange =
        activeSegmentBytes_ > kSegmentHeaderBytes &&
        !manifestRanges_.empty() &&
        manifestRanges_.back().endGen + 1 == activeGeneration_;
    const bool tailDepsContained =
        activeTailDependenciesTracked_ &&
        std::all_of(activeTailAffectedGenerations_.begin(), activeTailAffectedGenerations_.end(),
            [&](std::uint32_t gen) {
                return gen >= inputRange.startGen && gen <= activeGeneration_;
            });
    const bool wouldRotate = subrangeReachesLastGen && tailCanMergeWithLastRange && tailDepsContained;

    // Pre-rotate range-count gate — evaluated before preflight so that a fallback
    // expansion of inputRange is reflected in the hypo calculations below.
    // When wouldRotate, the parent extends rightward to activeGeneration_
    // (parent.startGen unchanged), hasRight becomes false, hasLeft is unchanged.
    // postDelta = hasLeft ? 1 : 0.
    // Gate only applies when the hypothetical extended range has live records:
    // if there are no live records, rotate will not fire (guarded below by
    // !hypoPayloadBytes.empty()), so dead-subrange cleanup must not be blocked.
    // activeSegmentBytes_ > kSegmentHeaderBytes means the tail has events, but
    // those could all be tombstones — use a cheap RAM scan instead.
    const std::uint32_t hypoCheckEnd = wouldRotate ? activeGeneration_ : inputRange.endGen;
    const bool hypoHasLive = std::any_of(records_.begin(), records_.end(),
        [&](const SegmentRecord& sr) {
            return sr.segmentGeneration >= inputRange.startGen &&
                   sr.segmentGeneration <= hypoCheckEnd;
        });
    if (wouldRotate && hypoHasLive && hasLeft) {
        if (manifestRanges_.size() + 1 > kManifestMaxRanges) {
            if (allowFallback == AllowFullRangeFallback::no) {
#ifdef ARDUINO
                logLine("noOp(splitLimit/preRotate)");
#endif
                return Status::noOp();
            }
            inputRange = {parent.startGen, parent.endGen};
            hasLeft  = false;
            hasRight = false;
        }
    }

    const std::uint32_t hypoStartGen = inputRange.startGen;
    const std::uint32_t hypoEndGen   = wouldRotate ? activeGeneration_ : inputRange.endGen;
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
            auto sit = sealedSegmentBytes_.find(gen);
            hypoInputBytes += sit != sealedSegmentBytes_.end() ? sit->second : 0;
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

    // Phase: re-derive parent range after potential rotate (defensive reclassification).
    CR_T0(ms_resolve);
    rangeIdx = findParentRangeIdx(inputRange.startGen, inputRange.endGen);
    if (rangeIdx >= manifestRanges_.size()) {
        CR_T1(ms_resolve);
#ifdef ARDUINO
        logLine("noOp(lostRange)");
#endif
        return Status::noOp();
    }
    parent = manifestRanges_[rangeIdx];
    if (wouldRotate && !hypoPayloadBytes.empty()) {
        inputRange.endGen = parent.endGen; // extend to include merged tail
    }
    hasLeft  = (inputRange.startGen > parent.startGen);
    hasRight = (inputRange.endGen   < parent.endGen);
    CR_T1(ms_resolve);
#ifdef ARDUINO
    eff_s = inputRange.startGen; eff_e = inputRange.endGen;
    n_in  = inputRange.endGen - inputRange.startGen + 1;
#endif

    // Phase: collectLiveRecords.
    CR_T0(ms_collect);
    std::vector<CompactionLiveRecord> liveRecords;
    Status st = collectLiveRecords(inputRange, liveRecords);
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
        // Dead subrange: drop it and leave any remainder fragments in place.
        std::vector<ManifestRange> newRanges = manifestRanges_;
        newRanges.erase(newRanges.begin() + static_cast<std::ptrdiff_t>(rangeIdx));
        std::size_t insertAt = rangeIdx;
        if (hasLeft)
            newRanges.insert(newRanges.begin() + insertAt++,
                             ManifestRange{parent.startGen, inputRange.startGen - 1});
        if (hasRight)
            newRanges.insert(newRanges.begin() + insertAt,
                             ManifestRange{inputRange.endGen + 1, parent.endGen});
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
        cleanupInputSegments(inputRange);
        CR_T1(ms_cleanup);
        for (std::uint32_t g = inputRange.startGen; g <= inputRange.endGen; ++g)
            activeTailAffectedGenerations_.erase(g);
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

    // Build manifest: erase parent, splice in left remainder + output + right remainder.
    std::vector<ManifestRange> newRanges = manifestRanges_;
    newRanges.erase(newRanges.begin() + static_cast<std::ptrdiff_t>(rangeIdx));
    std::size_t insertAt = rangeIdx;
    if (hasLeft)
        newRanges.insert(newRanges.begin() + insertAt++,
                         ManifestRange{parent.startGen, inputRange.startGen - 1});
    const std::size_t outputIdx = insertAt;
    newRanges.insert(newRanges.begin() + insertAt++, ManifestRange{firstNewGen, lastNewGen});
    if (hasRight)
        newRanges.insert(newRanges.begin() + insertAt,
                         ManifestRange{inputRange.endGen + 1, parent.endGen});

    // Two-sided contiguity merge on the output range.
    if (outputIdx + 1 < newRanges.size() &&
        newRanges[outputIdx].endGen + 1 == newRanges[outputIdx + 1].startGen) {
        newRanges[outputIdx].endGen = newRanges[outputIdx + 1].endGen;
        newRanges.erase(newRanges.begin() + static_cast<std::ptrdiff_t>(outputIdx) + 1);
    }
    if (outputIdx > 0 &&
        newRanges[outputIdx - 1].endGen + 1 == newRanges[outputIdx].startGen) {
        newRanges[outputIdx - 1].endGen = newRanges[outputIdx].endGen;
        newRanges.erase(newRanges.begin() + static_cast<std::ptrdiff_t>(outputIdx));
    }

    ManifestData md;
    md.ranges         = std::move(newRanges);
    md.tailGeneration = activeGeneration_;
    md.nextGeneration = lastNewGen + 1;

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

    // Phase: cleanup input segments (known retired range, no listFiles needed).
    CR_T0(ms_cleanup);
    cleanupInputSegments(inputRange);
    CR_T1(ms_cleanup);
    for (std::uint32_t g = inputRange.startGen; g <= inputRange.endGen; ++g)
        activeTailAffectedGenerations_.erase(g);

    // Phase: update in-RAM records to point at new segment generations.
    CR_T0(ms_replace);
    for (auto& r : records_) {
        if (r.segmentGeneration < inputRange.startGen ||
            r.segmentGeneration > inputRange.endGen) continue;
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

AppendLogStore::CompactionRange AppendLogStore::narrowRange(
    const CompactionRange& range, std::uint8_t maxOutputSegs) const
{
    const auto allStats = segmentStats();
    std::vector<SegmentStat> segs;
    for (const auto& s : allStats) {
        if (s.generation >= range.startGen && s.generation <= range.endGen)
            segs.push_back(s);
    }
    if (segs.empty()) return range;
    std::sort(segs.begin(), segs.end(),
        [](const SegmentStat& a, const SegmentStat& b) { return a.generation < b.generation; });

    const auto predictOut = [&](std::uint32_t liveBytes) -> std::uint32_t {
        if (liveBytes == 0) return 0;
        return (liveBytes + config_.maxSegmentBytes - 1) / config_.maxSegmentBytes;
    };

    // Only narrow when the full range exceeds the output budget.
    std::uint32_t totalRangeLive = 0;
    for (const auto& s : segs) totalRangeLive += s.liveBytes;
    if (predictOut(totalRangeLive) <= maxOutputSegs) return range;

    if (predictOut(segs[0].liveBytes) > maxOutputSegs) {
        // Even a single segment exceeds budget -- compact it anyway (minimum unit).
        return {segs[0].generation, segs[0].generation};
    }

    CompactionRange best = {segs[0].generation, segs[0].generation};
    float bestDead = -1.0f;
    for (std::size_t i = 0; i < segs.size(); ++i) {
        std::uint32_t live = 0, total = 0;
        for (std::size_t j = i; j < segs.size(); ++j) {
            live  += segs[j].liveBytes;
            total += segs[j].totalBytes;
            if (predictOut(live) > maxOutputSegs) break;
            const float dr = total > 0
                ? static_cast<float>(total - live) / static_cast<float>(total) : 0.0f;
            if (dr > bestDead) {
                bestDead = dr;
                best = {segs[i].generation, segs[j].generation};
            }
        }
    }
    return best;
}

Status AppendLogStore::compactOneSegment(AllowFullRangeFallback allowFallback) {
    auto rangeOpt = chooseCompactionRange();
    if (!rangeOpt) return Status::noOp();
    const CompactionRange range = config_.maxOutputSegments > 0
        ? narrowRange(*rangeOpt, config_.maxOutputSegments)
        : *rangeOpt;
    return compactRange(range, nullptr, allowFallback);
}

CompactIdleResult AppendLogStore::compactIdle(std::size_t maxSteps) {
    CompactIdleResult result{};
    result.status = Status::success();
    for (std::size_t i = 0; i < maxSteps; ++i) {
        Status st = compactOneSegment(AllowFullRangeFallback::yes);
        ++result.stepsRun;
        if (!st.ok()) {
            result.status = st;
            return result;
        }
        if (st.isNoOp()) {
            ++result.noOps;
            return result; // moreWorkLikely stays false
        }
        ++result.compactions;
    }
    result.moreWorkLikely = (result.compactions > 0);
    return result;
}

Status AppendLogStore::compactFull() {
    const std::size_t initialCount = manifestRanges_.size();
    const CompactIdleResult result = compactIdle(initialCount);
    return result.status;
}

bool AppendLogStore::needsCompaction() const {
    if (activeGenerations_.size() > config_.maxSegments) return true;

    const std::uint64_t free = fs_ ? fs_->freeBytes() : 0;
    if (free < config_.minFreeBytes) return true;

    return false;
}

void AppendLogStore::cleanupInputSegments(const CompactionRange& effectiveRange) {
    auto f = fs();
    if (!f) return;
#ifdef ARDUINO
    std::uint32_t nDeleted = 0, nFailed = 0, bytesFreed = 0;
#endif
    for (std::uint32_t gen = effectiveRange.startGen; gen <= effectiveRange.endGen; ++gen) {
        const std::string name = segmentName(gen);
        auto sit = sealedSegmentBytes_.find(gen);
        const std::uint32_t sz = sit != sealedSegmentBytes_.end() ? sit->second : 0;
#ifdef ARDUINO
        if (sz == 0 && sit == sealedSegmentBytes_.end()) {
            Serial.printf("[cleanup] warn: gen=%u not in sealedSegmentBytes_\n", gen);
        }
#endif
        if (f->removeFile(name).ok()) {
            totalOnDiskBytes_ -= sz;
            sealedSegmentBytes_.erase(gen);
#ifdef ARDUINO
            ++nDeleted; bytesFreed += sz;
#endif
        }
#ifdef ARDUINO
        else { ++nFailed; }
#endif
    }
#ifdef ARDUINO
    Serial.printf("[cleanup] deleted=%u failed=%u bytes=%u range=[%u,%u]\n",
        nDeleted, nFailed, bytesFreed, effectiveRange.startGen, effectiveRange.endGen);
    Serial.flush();
#endif
}

void AppendLogStore::cleanupOneDanglingSegment() {
    auto f = fs();
    if (!f) return;

#ifdef ARDUINO
    const std::uint32_t t_cl_start = millis();
    std::uint32_t ms_list = 0, ms_scan = 0, ms_size = 0, ms_remove = 0;
    std::uint32_t _t0 = millis();
#endif

    std::vector<std::string> files;
    if (!f->listFiles(files).ok()) return;
#ifdef ARDUINO
    ms_list = millis() - _t0;
    _t0 = millis();
#endif

    // Find the first dangling segment (not in activeGenerations_).
    std::string dangling;
    for (const auto& name : files) {
        std::uint32_t gen = 0;
        if (!isSegmentName(name, gen)) continue;
        const bool live = std::find(activeGenerations_.begin(), activeGenerations_.end(), gen)
                          != activeGenerations_.end();
        if (!live) { dangling = name; break; }
    }
#ifdef ARDUINO
    ms_scan = millis() - _t0;
#endif

    if (dangling.empty()) {
#ifdef ARDUINO
        const std::uint32_t ms_total = millis() - t_cl_start;
        if (ms_total > 50) {
            Serial.printf("[cleanup] none list_ms=%u scan_ms=%u total_ms=%u\n",
                ms_list, ms_scan, ms_total);
            Serial.flush();
        }
#endif
        return;
    }

#ifdef ARDUINO
    _t0 = millis();
#endif
    std::uint64_t sz = 0;
    f->fileSize(dangling, sz);
#ifdef ARDUINO
    ms_size = millis() - _t0;
    _t0 = millis();
#endif
    const bool removed = f->removeFile(dangling).ok();
#ifdef ARDUINO
    ms_remove = millis() - _t0;
#endif
    if (removed) {
        totalOnDiskBytes_ -= static_cast<std::uint32_t>(sz);
        std::uint32_t gen = 0;
        if (isSegmentName(dangling, gen)) sealedSegmentBytes_.erase(gen);
    }

#ifdef ARDUINO
    const std::uint32_t ms_total = millis() - t_cl_start;
    if (ms_total > 50 || removed) {
        if (removed) {
            Serial.printf("[cleanup] deleted=%s list_ms=%u scan_ms=%u size_ms=%u remove_ms=%u total_ms=%u\n",
                dangling.c_str(), ms_list, ms_scan, ms_size, ms_remove, ms_total);
        } else {
            Serial.printf("[cleanup] rm-failed=%s list_ms=%u scan_ms=%u size_ms=%u remove_ms=%u total_ms=%u\n",
                dangling.c_str(), ms_list, ms_scan, ms_size, ms_remove, ms_total);
        }
        Serial.flush();
    }
#endif
}

void AppendLogStore::cleanupAllDanglingSegments() {
    auto f = fs();
    if (!f) return;

    std::vector<std::string> files;
    if (!f->listFiles(files).ok()) return;

    for (const auto& name : files) {
        std::uint32_t gen = 0;
        if (!isSegmentName(name, gen)) continue;
        const bool active = std::find(activeGenerations_.begin(), activeGenerations_.end(), gen)
                            != activeGenerations_.end();
        if (active) continue;

        std::uint64_t sz = 0;
        f->fileSize(name, sz);
        const bool removed = f->removeFile(name).ok();
        if (removed) {
            totalOnDiskBytes_ -= static_cast<std::uint32_t>(sz);
            sealedSegmentBytes_.erase(gen);
        }
#ifdef ARDUINO
        if (removed) {
            Serial.printf("[cleanup] deleted=%s\n", name.c_str());
        } else {
            Serial.printf("[cleanup] rm-failed=%s\n", name.c_str());
        }
        Serial.flush();
#endif
    }
}

} // namespace pqueue

#include "append_log_store.h"
#include "append_log_common.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>
#include <unordered_map>
#include <vector>

namespace pqueue {

using namespace append_log_detail;

namespace {

constexpr const char* kSegmentPrefix  = "seg-";
constexpr const char* kSegmentSuffix  = ".bin";
constexpr const char* kManifestSlotA  = "manifest-a.bin";
constexpr const char* kManifestSlotB  = "manifest-b.bin";

inline std::uint32_t readLE32(const std::string& buf, std::size_t offset) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[offset]))
         | static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[offset + 1])) << 8
         | static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[offset + 2])) << 16
         | static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[offset + 3])) << 24;
}

std::string formatGeneration(std::uint32_t gen) {
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned>(gen));
    return {buf, 8};
}

// Returns the manifest slot to write to, or nullptr when at least one slot file
// exists but no slot is parseable (caller should surface DataCorrupt).
const char* chooseInactiveSlot(bool existsA, bool validA, std::uint32_t epochA,
                                bool existsB, bool validB, std::uint32_t epochB) {
    if (!existsA && !existsB) return kManifestSlotA;  // fresh store
    if (!validA && !validB)   return nullptr;           // file(s) exist but none parse
    if (!validA)              return kManifestSlotA;
    if (!validB)              return kManifestSlotB;
    // Both valid: write to the lower-epoch slot (it becomes the new winner next read)
    return (epochB <= epochA) ? kManifestSlotB : kManifestSlotA;
}

// Selects the winning manifest from two slots. Returns true and sets out if at
// least one slot is valid; returns false if neither is valid.
bool chooseWinningSlot(bool validA, const ManifestData& mdA,
                       bool validB, const ManifestData& mdB,
                       ManifestData& out) {
    if (!validA && !validB) return false;
    if (validA && !validB)  { out = mdA; return true; }
    if (!validA)            { out = mdB; return true; }
    // Both valid: higher epoch wins; equal epoch → slot A
    out = (mdB.epoch > mdA.epoch) ? mdB : mdA;
    return true;
}

} // namespace

AppendLogStore::AppendLogStore(AppendLogConfig config)
    : config_(std::move(config)), fs_(config_.fileSystem) {}

StorageBackend AppendLogStore::resolvedBackend() const {
    if (config_.backend != StorageBackend::Default) {
        return config_.backend;
    }
#ifdef ARDUINO
    return StorageBackend::LittleFS;
#else
    return StorageBackend::Posix;
#endif
}

std::shared_ptr<FileSystem> AppendLogStore::fs() {
    if (fs_) return fs_;
    switch (resolvedBackend()) {
    case StorageBackend::Posix:
        fs_ = makePosixFileSystem();
        break;
    case StorageBackend::LittleFS:
        fs_ = makeLittleFsFileSystem();
        break;
    default:
        break;
    }
    return fs_;
}

Status AppendLogStore::emit(Event event) const {
    config_.events.emit(event);
    return event.status;
}

Status AppendLogStore::diagnostic(Severity severity, Status status, const char* operation) const {
    return emit(Event{EventKind::Diagnostic, severity, status, "AppendLogStore", operation});
}

std::string AppendLogStore::segmentName(std::uint32_t generation) const {
    return std::string(kSegmentPrefix) + formatGeneration(generation) + kSegmentSuffix;
}

bool AppendLogStore::isSegmentName(const std::string& name, std::uint32_t& generationOut) const {
    const std::string prefix = kSegmentPrefix;
    const std::string suffix = kSegmentSuffix;
    if (name.size() != prefix.size() + 8 + suffix.size()) return false;
    if (name.substr(0, prefix.size()) != prefix) return false;
    if (name.substr(name.size() - suffix.size()) != suffix) return false;
    const std::string hexPart = name.substr(prefix.size(), 8);
    for (char c : hexPart) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    generationOut = static_cast<std::uint32_t>(std::stoul(hexPart, nullptr, 16));
    return true;
}

FileStoreIndex AppendLogStore::indexFromRecords() const {
    FileStoreIndex idx;
    if (records_.empty()) {
        idx.head = nextSequence_;
        idx.tail = nextSequence_;
        return idx;
    }
    idx.head  = records_.front().sequence;
    idx.tail  = records_.back().sequence + 1;
    idx.count = static_cast<std::uint32_t>(records_.size());
    return idx;
}

Status AppendLogStore::ensureMounted() {
    if (mounted_) return Status::success();
    return mount();
}

Status AppendLogStore::mount() {
    auto f = fs();
    if (!f) {
        return diagnostic(Severity::Error,
            Status::failure(StatusCode::BackendUnavailable, "append-log file system backend unavailable"),
            "mount");
    }
    Status st = f->mount(config_.basePath);
    if (!st.ok()) {
        return diagnostic(Severity::Error, st, "mount");
    }
    st = scanSegments();
    if (!st.ok()) {
        return diagnostic(Severity::Error, st, "mount");
    }
    mounted_ = true;
    return Status::success();
}

Status AppendLogStore::scanSegments() {
    auto f = fs();

    std::vector<std::string> files;
    Status st = f->listFiles(files);
    if (!st.ok()) return st;

    std::vector<std::uint32_t> sortedGenerations;
    for (const auto& name : files) {
        std::uint32_t gen = 0;
        if (isSegmentName(name, gen)) {
            sortedGenerations.push_back(gen);
        }
    }
    std::sort(sortedGenerations.begin(), sortedGenerations.end());

    records_.clear();
    activeGenerations_.clear();
    activeGeneration_ = 0;
    activeSegmentBytes_ = 0;
    nextGeneration_ = 1;
    nextSequence_ = 0;

    ManifestData manifest;
    if (readManifest(manifest)) {
        applyManifestToRam(manifest);
    } else if (!sortedGenerations.empty()) {
        return Status::failure(StatusCode::DataCorrupt,
            "segment files exist without a valid manifest");
    }

    // Advance nextGeneration_ past every segment file on disk, including dangling ones,
    // to prevent generation reuse after partial operations.
    for (std::uint32_t g : sortedGenerations) {
        if (g + 1 > nextGeneration_) nextGeneration_ = g + 1;
    }

    for (std::size_t segIdx = 0; segIdx < activeGenerations_.size(); ++segIdx) {
        const std::uint32_t gen = activeGenerations_[segIdx];
        const bool isLastSegment = (segIdx + 1 == activeGenerations_.size());
        const std::string name = segmentName(gen);

        std::uint64_t fileSize = 0;
        if (!f->fileSize(name, fileSize).ok() || fileSize < kSegmentHeaderBytes) {
            return Status::failure(StatusCode::DataCorrupt, "segment missing or too small");
        }

        std::string headerBytes;
        if (!f->readAt(name, 0, kSegmentHeaderBytes, headerBytes).ok()) {
            return Status::failure(StatusCode::DataCorrupt, "cannot read segment header");
        }
        SegmentHeader segHeader;
        if (!parseSegmentHeader(headerBytes, segHeader)) {
            return Status::failure(StatusCode::DataCorrupt, "corrupt segment header");
        }
        if (segHeader.generation != gen) {
            return Status::failure(StatusCode::DataCorrupt, "segment generation mismatch");
        }

        bool corrupt = false;
        std::uint32_t offset = kSegmentHeaderBytes;
        std::uint32_t lastGoodOffset = kSegmentHeaderBytes;

        while (offset < static_cast<std::uint32_t>(fileSize)) {
            const std::uint32_t remaining = static_cast<std::uint32_t>(fileSize) - offset;
            if (remaining < 4) break; // too few bytes for magic: potential torn tail

            std::string headerBuf;
            const std::uint32_t toRead = std::min(remaining, static_cast<std::uint32_t>(kEnqueueHeaderBytes));
            if (!f->readAt(name, offset, toRead, headerBuf).ok()) break;
            if (headerBuf.size() < 4) break;

            const std::uint32_t magic = readLE32(headerBuf, 0);

            if (magic == kEnqueueMagic || magic == kRewriteMagic) {
                if (toRead < kEnqueueHeaderBytes) break; // partial header: torn tail

                EnqueueHeader eh;
                if (!parseEnqueueHeader(headerBuf, eh)) { corrupt = true; break; }

                // Guard: payloadBytes within configured limit (prevents huge allocation on corruption)
                if (eh.payloadBytes > config_.maxRecordBytes) { corrupt = true; break; }

                const std::uint32_t totalEventBytes =
                    kEnqueueHeaderBytes + eh.payloadBytes + kEventTrailerBytes;

                if (totalEventBytes > remaining) break; // torn event (overflow-safe)

                std::string payloadAndTrailer;
                if (!f->readAt(name, offset + kEnqueueHeaderBytes,
                               eh.payloadBytes + kEventTrailerBytes, payloadAndTrailer).ok()) break;
                if (payloadAndTrailer.size() < eh.payloadBytes + kEventTrailerBytes) break;

                const std::uint32_t storedCrc    = readLE32(payloadAndTrailer, eh.payloadBytes);
                const std::uint32_t storedFooter = readLE32(payloadAndTrailer, eh.payloadBytes + 4);
                const std::string   payload      = payloadAndTrailer.substr(0, eh.payloadBytes);
                const std::uint32_t expectedCrc  = enqueueEventCrc(eh, payload);

                if (storedCrc != expectedCrc || storedFooter != kFooterMagic) { corrupt = true; break; }

                if (magic == kEnqueueMagic) {
                    SegmentRecord sr;
                    sr.sequence = eh.sequence;
                    sr.segmentGeneration = gen;
                    sr.payloadOffset = offset + kEnqueueHeaderBytes;
                    sr.payloadBytes = eh.payloadBytes;
                    records_.push_back(sr);
                } else {
                    for (auto& r : records_) {
                        if (r.sequence == eh.sequence) {
                            r.segmentGeneration = gen;
                            r.payloadOffset = offset + kEnqueueHeaderBytes;
                            r.payloadBytes = eh.payloadBytes;
                            break;
                        }
                    }
                }
                if (eh.sequence + 1 > nextSequence_) nextSequence_ = eh.sequence + 1;
                offset += totalEventBytes;
                lastGoodOffset = offset;

            } else if (magic == kPopMagic) {
                if (remaining < kPopEventBytes) break; // torn pop event

                std::string popBuf;
                if (!f->readAt(name, offset, kPopEventBytes, popBuf).ok()) break;
                PopEvent pe;
                if (!parsePopEvent(popBuf, pe)) { corrupt = true; break; }

                if (!records_.empty() && records_.front().sequence == pe.sequence) {
                    records_.pop_front();
                }
                if (pe.sequence + 1 > nextSequence_) nextSequence_ = pe.sequence + 1;
                offset += kPopEventBytes;
                lastGoodOffset = offset;

            } else {
                // Unknown magic: data corruption
                corrupt = true;
                break;
            }
        }

        activeGeneration_ = gen;

        // Corruption in the last segment is treated as a torn-tail: everything from
        // lastGoodOffset onward is discarded. This covers power-loss mid-write (partial
        // CRC/footer), but also silently drops any valid events that follow a bad one.
        // With a sequential append-only log the bytes after a bad event are of unknown
        // validity, so discarding from lastGoodOffset is the safest option.
        if (corrupt && isLastSegment) {
            corrupt = false;
        }

        if (corrupt) {
            return Status::failure(StatusCode::DataCorrupt, "corrupt event data in segment");
        }

        if (offset < static_cast<std::uint32_t>(fileSize)) {
            // Scan stopped before EOF: torn tail (or I/O error, treated the same way).
            // Only acceptable on the active (last) segment; elsewhere it means data was lost.
            if (!isLastSegment) {
                return Status::failure(StatusCode::DataCorrupt,
                    "incomplete event in non-active segment");
            }
            diagnostic(Severity::Warning,
                Status::failure(StatusCode::DataCorrupt, "discarding active-segment tail after last good event"),
                "mount");
            // Truncate to remove partial bytes. If truncation fails, recovery remains
            // deterministic: future remounts will discard the same tail again.
            f->resizeFile(name, lastGoodOffset);
            activeSegmentBytes_ = lastGoodOffset;
        } else {
            activeSegmentBytes_ = offset;
        }
    }

    cleanupOneDanglingSegment();
    return Status::success();
}

Status AppendLogStore::publishManifest(const ManifestData& manifest) {
    auto f = fs();
    if (!f) return Status::failure(StatusCode::BackendUnavailable, "no file system");

    auto fileExists = [&](const char* name) -> bool {
        std::uint64_t dummy;
        return f->fileSize(name, dummy).ok();
    };
    auto tryRead = [&](const char* name, ManifestData& md) -> bool {
        std::string data;
        if (!f->readFile(name, data).ok()) return false;
        return parseManifest(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), md);
    };

    const bool existsA = fileExists(kManifestSlotA);
    const bool existsB = fileExists(kManifestSlotB);
    ManifestData mdA, mdB;
    const bool validA = existsA && tryRead(kManifestSlotA, mdA);
    const bool validB = existsB && tryRead(kManifestSlotB, mdB);

    const char* writeSlot = chooseInactiveSlot(existsA, validA, mdA.epoch,
                                                existsB, validB, mdB.epoch);
    if (!writeSlot) {
        return Status::failure(StatusCode::DataCorrupt, "manifest slot(s) exist but none are valid");
    }

    ManifestData winning;
    const std::uint32_t winningEpoch =
        chooseWinningSlot(validA, mdA, validB, mdB, winning) ? winning.epoch : 0;

    ManifestData toWrite = manifest;
    toWrite.epoch = winningEpoch + 1;

    std::vector<std::uint8_t> bytes;
    serialiseManifest(toWrite, bytes);
    const std::string data(reinterpret_cast<const char*>(bytes.data()), bytes.size());

    // Only the inactive slot is written. A partial write leaves that slot corrupt
    // (fails CRC on next parse); the active slot remains the election winner on remount.
    Status st = f->writeFile(writeSlot, data);
    if (!st.ok()) return st;

    applyManifestToRam(toWrite);
    cleanupOneDanglingSegment();
    return Status::success();
}

bool AppendLogStore::readManifest(ManifestData& out) {
    auto f = fs();
    if (!f) return false;

    auto tryRead = [&](const char* name, ManifestData& md) -> bool {
        std::string data;
        if (!f->readFile(name, data).ok()) return false;
        return parseManifest(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), md);
    };

    ManifestData mdA, mdB;
    const bool validA = tryRead(kManifestSlotA, mdA);
    const bool validB = tryRead(kManifestSlotB, mdB);
    return chooseWinningSlot(validA, mdA, validB, mdB, out);
}

void AppendLogStore::applyManifestToRam(const ManifestData& md) {
    manifestRanges_ = md.ranges;
    activeGenerations_.clear();
    for (const auto& r : md.ranges) {
        for (std::uint32_t g = r.startGen; g <= r.endGen; ++g) {
            activeGenerations_.push_back(g);
        }
    }
    if (md.tailGeneration != 0) {
        activeGenerations_.push_back(md.tailGeneration);
    }
    nextGeneration_ = md.nextGeneration;
}

Status AppendLogStore::createSegment(std::uint32_t generation, std::uint32_t startSeq) {
    const std::string headerBytes = serializeSegmentHeader(generation, startSeq);
    return fs()->writeFile(segmentName(generation), headerBytes);
}

Status AppendLogStore::rotateSegment() {
    const std::uint32_t oldTailGen = activeGeneration_;
    const std::uint32_t newGen    = nextGeneration_;

    // Build new full-range list: promote current tail to a closed range, then
    // merge with the preceding range if contiguous. Check limit before any I/O.
    std::vector<ManifestRange> newRanges = manifestRanges_;
    if (oldTailGen != 0) {
        ManifestRange r{oldTailGen, oldTailGen};
        if (!newRanges.empty() && newRanges.back().endGen + 1 == r.startGen) {
            newRanges.back().endGen = r.endGen;
        } else {
            newRanges.push_back(r);
        }
    }
    if (newRanges.size() > kManifestMaxRanges) {
        return Status::failure(StatusCode::RangeLimitExceeded,
            "segment range limit exceeded; compaction required before rollover");
    }

    const std::uint32_t baseSeq = records_.empty() ? 0 : records_.back().sequence + 1;
    Status st = createSegment(newGen, baseSeq);
    if (!st.ok()) return st;

    ManifestData manifest;
    manifest.ranges         = std::move(newRanges);
    manifest.tailGeneration = newGen;
    manifest.nextGeneration = newGen + 1;
    st = publishManifest(manifest);
    if (!st.ok()) return st;

    // RAM updated only after durable manifest publish.
    // publishManifest -> applyManifestToRam already set activeGenerations_ and nextGeneration_.
    activeGeneration_    = newGen;
    activeSegmentBytes_  = kSegmentHeaderBytes;
    return Status::success();
}

Status AppendLogStore::ensureActiveSegment(std::uint32_t baseSeq) {
    if (activeSegmentBytes_ == 0) {
        const std::uint32_t newGen = nextGeneration_;
        Status st = createSegment(newGen, baseSeq);
        if (!st.ok()) return st;

        ManifestData manifest;
        manifest.ranges         = manifestRanges_;
        manifest.tailGeneration = newGen;
        manifest.nextGeneration = newGen + 1;
        st = publishManifest(manifest);
        if (!st.ok()) return st;

        // RAM updated only after durable manifest publish.
        activeGeneration_   = newGen;
        activeSegmentBytes_ = kSegmentHeaderBytes;
        return Status::success();
    }
    return Status::success();
}

Status AppendLogStore::appendEnqueueEventBytes(const std::string& eventBytes) {
    Status st = fs()->writeAt(segmentName(activeGeneration_), activeSegmentBytes_, eventBytes);
    if (st.ok()) {
        activeSegmentBytes_ += static_cast<std::uint32_t>(eventBytes.size());
    }
    return st;
}

Status AppendLogStore::appendPopEvent(std::uint32_t sequence) {
    if (activeSegmentBytes_ > kSegmentHeaderBytes &&
        activeSegmentBytes_ + kPopEventBytes > config_.maxSegmentBytes) {
        Status st = rotateSegment();
        if (!st.ok()) return st;
    }
    Status st = ensureActiveSegment(sequence);
    if (!st.ok()) return st;

    const std::string eventBytes = serializePopEvent(sequence);
    st = fs()->writeAt(segmentName(activeGeneration_), activeSegmentBytes_, eventBytes);
    if (st.ok()) {
        activeSegmentBytes_ += static_cast<std::uint32_t>(eventBytes.size());
    }
    return st;
}

Status AppendLogStore::appendRewriteEvent(std::uint32_t sequence, const std::string& record) {
    const std::uint32_t totalBytes = kEnqueueOverheadBytes + static_cast<std::uint32_t>(record.size());
    if (activeSegmentBytes_ > kSegmentHeaderBytes &&
        activeSegmentBytes_ + totalBytes > config_.maxSegmentBytes) {
        Status st = rotateSegment();
        if (!st.ok()) return st;
    }
    Status ensureSt = ensureActiveSegment(sequence);
    if (!ensureSt.ok()) return ensureSt;

    const std::uint32_t payloadOffset = activeSegmentBytes_ + kEnqueueHeaderBytes;
    const std::string eventBytes = serializeRewriteEvent(sequence, record);
    Status st = fs()->writeAt(segmentName(activeGeneration_), activeSegmentBytes_, eventBytes);
    if (!st.ok()) return st;
    activeSegmentBytes_ += static_cast<std::uint32_t>(eventBytes.size());

    // Update RAM state for rewritten record
    for (auto& r : records_) {
        if (r.sequence == sequence) {
            r.segmentGeneration = activeGeneration_;
            r.payloadOffset = payloadOffset;
            r.payloadBytes = static_cast<std::uint32_t>(record.size());
            break;
        }
    }
    return Status::success();
}

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
        st = fs()->writeFile(segmentName(seg.gen), segData);
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
        f->removeFile(name); // best-effort; crash leaves an extra file, safe on remount
        return;
    }
}


// --- Store interface implementation ---

Status AppendLogStore::readIndex(FileStoreIndex& out) {
    Status st = ensureMounted();
    if (!st.ok()) return st;
    out = indexFromRecords();
    return Status::success();
}

Status AppendLogStore::readIndexFromDisk(FileStoreIndex& out) {
    return readIndex(out);
}

Status AppendLogStore::writeRecord(std::uint32_t sequence, const std::string& record) {
    Status st = ensureMounted();
    if (!st.ok()) return st;

    if (record.size() > config_.maxRecordBytes) {
        return diagnostic(Severity::Warning,
            Status::failure(StatusCode::RecordTooLarge, "record exceeds append-log maximum record size"),
            "writeRecord");
    }

    if (sequence == std::numeric_limits<std::uint32_t>::max()) {
        return diagnostic(Severity::Error,
            Status::failure(StatusCode::SequenceExhausted, "sequence space exhausted; format() required"),
            "writeRecord");
    }

    const std::uint32_t eventBytes = kEnqueueOverheadBytes + static_cast<std::uint32_t>(record.size());

    // Ensure we have an active segment, rotating or compacting if needed
    if (activeSegmentBytes_ > kSegmentHeaderBytes &&
        activeSegmentBytes_ + eventBytes > config_.maxSegmentBytes) {
        if (needsCompaction()) {
            st = compactOneSegment();
            if (!st.ok()) return diagnostic(Severity::Error, st, "writeRecord");
        }
        if (activeSegmentBytes_ > kSegmentHeaderBytes &&
            activeSegmentBytes_ + eventBytes > config_.maxSegmentBytes) {
            st = rotateSegment();
            if (!st.ok()) return diagnostic(Severity::Error, st, "writeRecord");
        }
    }
    st = ensureActiveSegment(sequence);
    if (!st.ok()) return diagnostic(Severity::Error, st, "writeRecord");

    // payloadOffset is computed after segment exists so activeSegmentBytes_ is valid
    const std::uint32_t payloadOffset = activeSegmentBytes_ + kEnqueueHeaderBytes;

    const std::string eventData = serializeEnqueueEvent(sequence, record);

    st = appendEnqueueEventBytes(eventData);
    if (!st.ok()) {
        return diagnostic(Severity::Error, st, "writeRecord");
    }

    pendingRecord_.sequence = sequence;
    pendingRecord_.segmentGeneration = activeGeneration_;
    pendingRecord_.payloadOffset = payloadOffset;
    pendingRecord_.payloadBytes = static_cast<std::uint32_t>(record.size());
    hasPendingEnqueue_ = true;
    return Status::success();
}

Status AppendLogStore::writeIndex(const FileStoreIndex& index) {
    Status st = ensureMounted();
    if (!st.ok()) return st;

    if (hasPendingEnqueue_) {
        // Commit the enqueue that was just written by writeRecord
        records_.push_back(pendingRecord_);
        hasPendingEnqueue_ = false;
        return Status::success();
    }

    // Detect pop: head advanced
    const FileStoreIndex current = indexFromRecords();
    if (index.head > current.head && !records_.empty()) {
        const std::uint32_t poppedSeq = records_.front().sequence;
        st = appendPopEvent(poppedSeq);
        if (!st.ok()) {
            return diagnostic(Severity::Error, st, "writeIndex");
        }
        records_.pop_front();
    }

    return Status::success();
}

Status AppendLogStore::rewriteRecord(std::uint32_t sequence, const std::string& record) {
    Status st = ensureMounted();
    if (!st.ok()) return st;

    if (record.size() > config_.maxRecordBytes) {
        return diagnostic(Severity::Warning,
            Status::failure(StatusCode::RecordTooLarge, "record exceeds append-log maximum record size"),
            "rewriteRecord");
    }

    st = appendRewriteEvent(sequence, record);
    if (!st.ok()) {
        return diagnostic(Severity::Error, st, "rewriteRecord");
    }
    return Status::success();
}

Status AppendLogStore::readRecord(std::uint32_t sequence, std::string& out) {
    Status st = ensureMounted();
    if (!st.ok()) return st;

    if (records_.empty()) {
        return Status::failure(StatusCode::QueueEmpty, "queue is empty");
    }

    const std::uint32_t head = records_.front().sequence;
    if (sequence < head || sequence >= head + static_cast<std::uint32_t>(records_.size())) {
        return Status::failure(StatusCode::InvalidRecord, "sequence not in live record range");
    }

    const std::uint32_t ordinal = sequence - head;
    const SegmentRecord& sr = records_[ordinal];

    st = fs()->readAt(segmentName(sr.segmentGeneration), sr.payloadOffset, sr.payloadBytes, out);
    if (!st.ok()) {
        return diagnostic(Severity::Error, st, "readRecord");
    }
    return Status::success();
}

Status AppendLogStore::removeRecord(std::uint32_t) {
    // No-op: POP events are written by writeIndex.
    return Status::success();
}

Status AppendLogStore::tryAcquireLockFile(const std::string& name, const std::string& contents) {
    Status st = ensureMounted();
    if (!st.ok()) return st;
    return fs()->tryAcquireLockFile(name, contents);
}

Status AppendLogStore::releaseLockFile(const std::string& name, const std::string& expectedContents) {
    auto f = fs();
    if (!f) return Status::success();
    return f->releaseLockFile(name, expectedContents);
}

Status AppendLogStore::recoverStaleLockFile(const std::string& name, const std::string& currentContents) {
    Status st = ensureMounted();
    if (!st.ok()) return st;
    return fs()->recoverStaleLockFile(name, currentContents);
}

std::uint64_t AppendLogStore::freeBytes() const {
    return fs_ ? fs_->freeBytes() : 0;
}

bool AppendLogStore::canEnqueue(std::size_t recordSize, std::uint32_t /*currentCount*/) const {
    const auto overhead = static_cast<std::uint64_t>(append_log_detail::kEnqueueOverheadBytes);
    return freeBytes() >= config_.minFreeBytes + recordSize + overhead;
}

Status AppendLogStore::format() {
    auto f = fs();
    if (!f) {
        f = const_cast<AppendLogStore*>(this)->fs();
    }
    if (!f) {
        return diagnostic(Severity::Error,
            Status::failure(StatusCode::BackendUnavailable, "file system backend unavailable"),
            "format");
    }
    Status st = f->mount(config_.basePath);
    if (!st.ok()) return diagnostic(Severity::Error, st, "format");

    std::vector<std::string> files;
    st = f->listFiles(files);
    if (!st.ok()) return diagnostic(Severity::Error, st, "format");

    for (const auto& name : files) {
        std::uint32_t gen = 0;
        if (isSegmentName(name, gen)) {
            f->removeFile(name);
        }
    }
    f->removeFile(kManifestSlotA);
    f->removeFile(kManifestSlotB);
    manifestRanges_.clear();
    activeGenerations_.clear();
    records_.clear();
    hasPendingEnqueue_ = false;
    activeGeneration_ = 0;
    activeSegmentBytes_ = 0;
    nextGeneration_ = 1;
    nextSequence_ = 0;
    mounted_ = true;
    return Status::success();
}

Status AppendLogStore::rebuildMetadata() {
    // For append-log, rebuilding means re-scanning all segments from scratch.
    activeGenerations_.clear();
    records_.clear();
    hasPendingEnqueue_ = false;
    mounted_ = false;
    return mount();
}

ValidationResult AppendLogStore::validateUnlocked(const ValidationOptions& options) {
    ValidationResult result;

    auto addErr = [&](ValidationIssueCode code, std::string msg) {
        result.ok = false;
        if (result.errors.size() < options.maxErrors) {
            ValidationIssue issue;
            issue.code = code;
            issue.message = std::move(msg);
            result.errors.push_back(std::move(issue));
        } else {
            result.stoppedEarly = true;
        }
    };

    auto f = fs();
    if (!f) {
        addErr(ValidationIssueCode::InvalidConfig, "file system backend unavailable");
        return result;
    }

    Status st = f->mount(config_.basePath);
    if (!st.ok()) {
        addErr(ValidationIssueCode::InvalidConfig, st.message);
        return result;
    }

    std::vector<std::string> files;
    st = f->listFiles(files);
    if (!st.ok()) {
        addErr(ValidationIssueCode::InvalidConfig, st.message);
        return result;
    }

    std::vector<std::uint32_t> sortedGenerations;
    for (const auto& name : files) {
        std::uint32_t gen = 0;
        if (isSegmentName(name, gen)) {
            sortedGenerations.push_back(gen);
        }
    }
    std::sort(sortedGenerations.begin(), sortedGenerations.end());

    for (std::uint32_t gen : sortedGenerations) {
        if (result.stoppedEarly) break;
        const std::string name = segmentName(gen);

        std::uint64_t fileSize = 0;
        if (!f->fileSize(name, fileSize).ok() || fileSize < kSegmentHeaderBytes) {
            addErr(ValidationIssueCode::MetadataCorrupt,
                "segment " + name + " is missing or too small");
            continue;
        }

        std::string headerBytes;
        if (!f->readAt(name, 0, kSegmentHeaderBytes, headerBytes).ok()) {
            addErr(ValidationIssueCode::MetadataCorrupt,
                "cannot read segment header for " + name);
            continue;
        }

        SegmentHeader segHeader;
        if (!parseSegmentHeader(headerBytes, segHeader)) {
            addErr(ValidationIssueCode::MetadataCorrupt,
                "corrupt segment header in " + name);
            continue;
        }
        if (segHeader.generation != gen) {
            addErr(ValidationIssueCode::MetadataCorrupt,
                "segment generation mismatch in " + name);
            continue;
        }

        // Scan events
        std::uint32_t offset = kSegmentHeaderBytes;
        while (offset < static_cast<std::uint32_t>(fileSize)) {
            if (result.stoppedEarly) break;
            const std::uint32_t remaining = static_cast<std::uint32_t>(fileSize) - offset;
            if (remaining < 4) break;

            std::string headerBuf;
            const std::uint32_t toRead = std::min(remaining, static_cast<std::uint32_t>(kEnqueueHeaderBytes));
            if (!f->readAt(name, offset, toRead, headerBuf).ok()) break;
            if (headerBuf.size() < 4) break;

            const std::uint32_t magic = readLE32(headerBuf, 0);

            if (magic == kEnqueueMagic || magic == kRewriteMagic) {
                if (toRead < kEnqueueHeaderBytes) break;
                EnqueueHeader eh;
                if (!parseEnqueueHeader(headerBuf, eh)) {
                    addErr(ValidationIssueCode::JournalCorrupt,
                        "invalid enqueue header in " + name);
                    break;
                }
                if (eh.payloadBytes > config_.maxRecordBytes) {
                    addErr(ValidationIssueCode::JournalCorrupt,
                        "payloadBytes exceeds maxRecordBytes at offset " + std::to_string(offset) + " in " + name);
                    break;
                }
                const std::uint32_t totalEventBytes = kEnqueueOverheadBytes + eh.payloadBytes;
                if (totalEventBytes > remaining) break; // overflow-safe

                std::string payloadAndTrailer;
                if (!f->readAt(name, offset + kEnqueueHeaderBytes,
                               eh.payloadBytes + kEventTrailerBytes, payloadAndTrailer).ok()) break;
                if (payloadAndTrailer.size() < eh.payloadBytes + kEventTrailerBytes) break;

                const std::string   payload     = payloadAndTrailer.substr(0, eh.payloadBytes);
                const std::uint32_t expectedCrc = enqueueEventCrc(eh, payload);
                const std::uint32_t storedCrc   = readLE32(payloadAndTrailer, eh.payloadBytes);
                const std::uint32_t storedFooter = readLE32(payloadAndTrailer, eh.payloadBytes + 4);

                if (storedCrc != expectedCrc) {
                    addErr(ValidationIssueCode::SlotCrcMismatch,
                        "CRC mismatch for event at offset " + std::to_string(offset) + " in " + name);
                    break;
                }
                if (storedFooter != kFooterMagic) {
                    addErr(ValidationIssueCode::JournalCorrupt,
                        "missing commit footer at offset " + std::to_string(offset) + " in " + name);
                    break;
                }

                ++result.checkedRecords;
                offset += totalEventBytes;

            } else if (magic == kPopMagic) {
                if (remaining < kPopEventBytes) break;
                std::string popBuf;
                if (!f->readAt(name, offset, kPopEventBytes, popBuf).ok()) break;
                PopEvent pe;
                if (!parsePopEvent(popBuf, pe)) {
                    addErr(ValidationIssueCode::JournalCorrupt,
                        "invalid pop event at offset " + std::to_string(offset) + " in " + name);
                    break;
                }
                ++result.checkedRecords;
                offset += kPopEventBytes;

            } else {
                break;
            }
        }
    }

    return result;
}

} // namespace pqueue

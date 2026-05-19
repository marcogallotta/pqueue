#include "pqueue/append_log_store.h"
#include "pqueue/append_log_common.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pqueue {

using namespace append_log_detail;

namespace {

constexpr const char* kSegmentPrefix  = "seg-";
constexpr const char* kSegmentSuffix  = ".bin";
// Duplicated from manifest.cpp for format() cleanup; do not create a shared header for these.
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
    sealedSegmentBytes_.clear();
    cachedWrittenSlot_ = nullptr;
    cachedWrittenEpoch_ = 0;
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

    // Initialize RAM footprint counter from all segment files on disk (including dangling).
    totalOnDiskBytes_ = 0;
    for (std::uint32_t gen : sortedGenerations) {
        std::uint64_t sz = 0;
        Status szSt = f->fileSize(segmentName(gen), sz);
        if (!szSt.ok()) return szSt;
        const auto szU = static_cast<std::uint32_t>(sz);
        totalOnDiskBytes_ += szU;
        sealedSegmentBytes_[gen] = szU;
    }

    cleanupOneDanglingSegment();
    return Status::success();
}

Status AppendLogStore::createSegment(std::uint32_t generation, std::uint32_t startSeq) {
    const std::string headerBytes = serializeSegmentHeader(generation, startSeq);
    return writeSegmentFileTracked(segmentName(generation), headerBytes, SegmentWriteDisposition::MustBeNew);
}

Status AppendLogStore::rotateSegment() {
#ifdef ARDUINO
    const std::uint32_t t_rot_start = millis();
#endif
    const std::uint32_t oldTailGen = activeGeneration_;
    const std::uint32_t newGen    = nextGeneration_;

    std::vector<ManifestRange> newRanges = manifestRanges_;
    bool merged = false;
    if (oldTailGen != 0) {
        ManifestRange r{oldTailGen, oldTailGen};
        if (!newRanges.empty() && newRanges.back().endGen + 1 == r.startGen) {
            newRanges.back().endGen = r.endGen;
            merged = true;
        } else {
            newRanges.push_back(r);
        }
    }
    if (newRanges.size() > kManifestMaxRanges) {
        return Status::failure(StatusCode::RangeLimitExceeded,
            "segment range limit exceeded; compaction required before rollover");
    }

    if (oldTailGen != 0) sealedSegmentBytes_[oldTailGen] = activeSegmentBytes_;

    const std::uint32_t baseSeq = records_.empty() ? 0 : records_.back().sequence + 1;
#ifdef ARDUINO
    const std::uint32_t t_create = millis();
#endif
    Status st = createSegment(newGen, baseSeq);
#ifdef ARDUINO
    const std::uint32_t ms_create = millis() - t_create;
#endif
    if (!st.ok()) return st;

    ManifestData manifest;
    manifest.ranges         = std::move(newRanges);
    manifest.tailGeneration = newGen;
    manifest.nextGeneration = newGen + 1;
    st = publishManifest(manifest);
#ifdef ARDUINO
    Serial.printf("[rotate] old=%u new=%u merged=%u create_ms=%u probe_ms=%u write_ms=%u total_ms=%u\n",
        oldTailGen, newGen, static_cast<unsigned>(merged),
        ms_create, dbgLastProbeMs_, dbgLastWriteMs_, millis() - t_rot_start);
    Serial.flush();
#endif
    if (!st.ok()) return st;

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
        const auto n = static_cast<std::uint32_t>(eventBytes.size());
        activeSegmentBytes_ += n;
        totalOnDiskBytes_   += n;
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
        const auto n = static_cast<std::uint32_t>(eventBytes.size());
        activeSegmentBytes_ += n;
        totalOnDiskBytes_   += n;
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
    const auto n = static_cast<std::uint32_t>(eventBytes.size());
    activeSegmentBytes_ += n;
    totalOnDiskBytes_   += n;

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

Status AppendLogStore::writeSegmentFileTracked(const std::string& name, const std::string& data,
                                                SegmentWriteDisposition disposition) {
    std::uint64_t oldSize = 0;
    if (disposition == SegmentWriteDisposition::MayOverwrite) {
        fs()->fileSize(name, oldSize);
    }
    Status st = fs()->writeFile(name, data);
    if (!st.ok()) return st;
    const auto newSz = static_cast<std::uint32_t>(data.size());
    const auto oldSz = static_cast<std::uint32_t>(oldSize);
    if (newSz >= oldSz) totalOnDiskBytes_ += newSz - oldSz;
    else                totalOnDiskBytes_ -= oldSz - newSz;
    std::uint32_t gen = 0;
    if (isSegmentName(name, gen)) sealedSegmentBytes_[gen] = newSz;
    return Status::success();
}

std::uint32_t AppendLogStore::appendGrowthBytes(std::uint32_t recordSize) const {
    const std::uint32_t eventBytes = kEnqueueOverheadBytes + recordSize;
    const bool needsNewSegment =
        activeSegmentBytes_ == 0 ||
        (activeSegmentBytes_ > kSegmentHeaderBytes &&
         activeSegmentBytes_ + eventBytes > config_.maxSegmentBytes);
    return eventBytes + (needsNewSegment ? kSegmentHeaderBytes : 0);
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
#ifdef ARDUINO
    const std::uint32_t t_wr_start = millis();
    std::uint32_t ms_ensure = 0, ms_compact = 0, ms_rotate = 0, ms_ensure_seg = 0, ms_append = 0;
    bool did_rotate = false, did_compact = false;
#endif

#ifdef ARDUINO
    const std::uint32_t _t_ensure = millis();
#endif
    Status st = ensureMounted();
#ifdef ARDUINO
    ms_ensure = millis() - _t_ensure;
#endif
    if (!st.ok()) return st;

    if (record.size() > config_.maxRecordBytes) {
        return diagnostic(Severity::Warning,
            Status::failure(StatusCode::RecordTooLarge, "record exceeds append-log maximum record size"),
            "writeRecord");
    }
    if (kSegmentHeaderBytes + kEnqueueOverheadBytes + record.size() > config_.maxSegmentBytes) {
        return diagnostic(Severity::Warning,
            Status::failure(StatusCode::RecordTooLarge, "record too large to fit in a segment"),
            "writeRecord");
    }

    if (sequence == std::numeric_limits<std::uint32_t>::max()) {
        return diagnostic(Severity::Error,
            Status::failure(StatusCode::SequenceExhausted, "sequence space exhausted; format() required"),
            "writeRecord");
    }

    const std::uint32_t eventBytes = kEnqueueOverheadBytes + static_cast<std::uint32_t>(record.size());

    if (config_.maxTotalBytes > 0) {
#ifdef ARDUINO
        const std::uint32_t _tc = millis();
#endif
        while (totalOnDiskBytes() + appendGrowthBytes(static_cast<std::uint32_t>(record.size())) > config_.maxTotalBytes) {
            Status compact = compactOneSegment();
            if (!compact.ok()) return diagnostic(Severity::Error, compact, "writeRecord");
            if (compact.isNoOp()) {
                return diagnostic(Severity::Warning,
                    Status::failure(StatusCode::QueueFull, "queue footprint limit reached"),
                    "writeRecord");
            }
#ifdef ARDUINO
            did_compact = true;
#endif
        }
#ifdef ARDUINO
        ms_compact = millis() - _tc;
#endif
    }

    if (config_.minFreeBytes > 0) {
        const std::uint64_t free = freeBytes();
        const std::uint32_t growth = appendGrowthBytes(static_cast<std::uint32_t>(record.size()));
        if (free < static_cast<std::uint64_t>(config_.minFreeBytes) + growth) {
            return diagnostic(Severity::Warning,
                Status::failure(StatusCode::QueueFull, "insufficient filesystem free space"),
                "writeRecord");
        }
    }

    if (activeSegmentBytes_ > kSegmentHeaderBytes &&
        activeSegmentBytes_ + eventBytes > config_.maxSegmentBytes) {
        if (needsCompaction()) {
            Status cst = compactOneSegment();
            if (!cst.ok()) return diagnostic(Severity::Error, cst, "writeRecord");
#ifdef ARDUINO
            did_compact = true;
#endif
        }
        if (activeSegmentBytes_ > kSegmentHeaderBytes &&
            activeSegmentBytes_ + eventBytes > config_.maxSegmentBytes) {
#ifdef ARDUINO
            const std::uint32_t _tr = millis();
#endif
            Status rst = rotateSegment();
#ifdef ARDUINO
            ms_rotate = millis() - _tr;
            did_rotate = true;
#endif
            if (!rst.ok()) return diagnostic(Severity::Error, rst, "writeRecord");
        }
    }

#ifdef ARDUINO
    const std::uint32_t _t_ensure_seg = millis();
#endif
    Status est = ensureActiveSegment(sequence);
#ifdef ARDUINO
    ms_ensure_seg = millis() - _t_ensure_seg;
#endif
    if (!est.ok()) return diagnostic(Severity::Error, est, "writeRecord");

    const std::uint32_t payloadOffset = activeSegmentBytes_ + kEnqueueHeaderBytes;
    const std::string eventData = serializeEnqueueEvent(sequence, record);

#ifdef ARDUINO
    const std::uint32_t _t_append = millis();
#endif
    Status ast = appendEnqueueEventBytes(eventData);
#ifdef ARDUINO
    ms_append = millis() - _t_append;
#endif
    if (!ast.ok()) return diagnostic(Severity::Error, ast, "writeRecord");

#ifdef ARDUINO
    const std::uint32_t ms_total = millis() - t_wr_start;
    if (ms_total > 500) {
        Serial.printf("[writeRecord] seq=%u recBytes=%u rotate=%u compact=%u "
            "ensure_ms=%u compact_ms=%u rotate_ms=%u ensure_seg_ms=%u append_ms=%u total_ms=%u\n",
            sequence, static_cast<unsigned>(record.size()),
            static_cast<unsigned>(did_rotate), static_cast<unsigned>(did_compact),
            ms_ensure, ms_compact, ms_rotate, ms_ensure_seg, ms_append, ms_total);
        Serial.flush();
    }
#endif

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
    if (kSegmentHeaderBytes + kEnqueueOverheadBytes + record.size() > config_.maxSegmentBytes) {
        return diagnostic(Severity::Warning,
            Status::failure(StatusCode::RecordTooLarge, "record too large to fit in a segment"),
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

bool AppendLogStore::canEnqueue(std::size_t /*recordSize*/, std::uint32_t /*currentCount*/) const {
    // Only checks the hard FS floor. Finer admission (maxTotalBytes cap, required record
    // headroom) is enforced inside writeRecord() after a compaction attempt. Checking any
    // of that here would block the write that triggers compaction.
    return freeBytes() >= config_.minFreeBytes;
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
    sealedSegmentBytes_.clear();
    records_.clear();
    hasPendingEnqueue_ = false;
    cachedWrittenSlot_ = nullptr;
    cachedWrittenEpoch_ = 0;
    activeGeneration_ = 0;
    activeSegmentBytes_ = 0;
    totalOnDiskBytes_ = 0;
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

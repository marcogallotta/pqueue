#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "append_log_common.h"
#include "events.h"
#include "file_store.h"
#include "file_system.h"
#include "status.h"

namespace pqueue {

struct AppendLogConfig {
    std::string basePath;
    StorageBackend backend = StorageBackend::Default;
    std::shared_ptr<FileSystem> fileSystem;
    std::uint32_t maxSegmentBytes = 4096;
    std::uint32_t maxTotalBytes = 128 * 1024; // 0 = unlimited
    std::uint32_t minFreeBytes = 32 * 1024;
    std::uint8_t maxSegments = 16;
    std::uint8_t maxOutputSegments = 8; // max output segments per compactOneSegment call
    std::size_t maxRecordBytes = 4096;
    EventOptions events;
};

class AppendLogStore : public Store {
public:
    explicit AppendLogStore(AppendLogConfig config = AppendLogConfig{});

    Status mount() override;
    Status readIndex(FileStoreIndex& out) override;
    Status readIndexFromDisk(FileStoreIndex& out) override;
    Status writeIndex(const FileStoreIndex& index) override;

    Status writeRecord(std::uint32_t sequence, const std::string& record) override;
    Status rewriteRecord(std::uint32_t sequence, const std::string& record) override;
    Status readRecord(std::uint32_t sequence, std::string& out) override;
    Status removeRecord(std::uint32_t sequence) override;

    Status publishManifest(const append_log_detail::ManifestData& manifest);
    bool readManifest(append_log_detail::ManifestData& out);

    struct CompactionRange {
        std::uint32_t startGen = 0;
        std::uint32_t endGen   = 0;
    };
    std::optional<CompactionRange> chooseCompactionRange() const;
    CompactionRange narrowRange(const CompactionRange& range, std::uint8_t maxOutputSegs) const;

    struct CompactionLiveRecord {
        std::uint32_t sequence = 0;
        std::string   payload;
    };
    Status collectLiveRecords(const CompactionRange& range,
                              std::vector<CompactionLiveRecord>& out) const;

    Status compactRange(const CompactionRange& range, std::uint32_t* outputSegCount = nullptr);
    Status compactOneSegment();
    Status compactFull();
    CompactIdleResult compactIdle(std::size_t maxSteps) override;

    struct SegmentStat {
        std::uint32_t generation = 0;
        std::uint32_t totalBytes = 0;
        std::uint32_t liveBytes  = 0;
        std::uint32_t deadBytes() const { return totalBytes > liveBytes ? totalBytes - liveBytes : 0; }
        float deadRatio() const { return totalBytes > 0 ? static_cast<float>(deadBytes()) / static_cast<float>(totalBytes) : 0.0f; }
    };
    std::vector<SegmentStat> segmentStats() const;
    std::uint32_t totalOnDiskBytes() const { return totalOnDiskBytes_; }

    const std::vector<append_log_detail::ManifestRange>& manifestRanges() const { return manifestRanges_; }
    std::uint32_t tailGeneration() const { return activeGeneration_; }

    Status tryAcquireLockFile(const std::string& name, const std::string& contents) override;
    Status releaseLockFile(const std::string& name, const std::string& expectedContents) override;
    Status recoverStaleLockFile(const std::string& name, const std::string& currentContents) override;

    std::uint64_t freeBytes() const override;
    bool canEnqueue(std::size_t recordSize, std::uint32_t currentCount) const override;

    Status format() override;
    Status rebuildMetadata() override;
    ValidationResult validateUnlocked(const ValidationOptions& options = ValidationOptions{}) override;

private:
    struct SegmentRecord {
        std::uint32_t sequence         = 0;
        std::uint32_t segmentGeneration = 0;
        std::uint32_t payloadOffset    = 0; // offset of payload within segment file
        std::uint32_t payloadBytes     = 0;
    };

    StorageBackend resolvedBackend() const;
    std::shared_ptr<FileSystem> fs();
    Status ensureMounted();
    Status emit(Event event) const;
    Status diagnostic(Severity severity, Status status, const char* operation) const;

    enum class SegmentWriteDisposition { MustBeNew, MayOverwrite };

    std::string segmentName(std::uint32_t generation) const;
    bool isSegmentName(const std::string& name, std::uint32_t& generationOut) const;
    std::uint32_t appendGrowthBytes(std::uint32_t recordSize) const;
    Status writeSegmentFileTracked(const std::string& name, const std::string& data,
                                   SegmentWriteDisposition disposition = SegmentWriteDisposition::MayOverwrite);

    Status createSegment(std::uint32_t generation, std::uint32_t startSeq);
    Status rotateSegment();
    Status ensureActiveSegment(std::uint32_t baseSeq);
    Status appendEnqueueEventBytes(const std::string& eventBytes);
    Status appendPopEvent(std::uint32_t sequence);
    Status appendRewriteEvent(std::uint32_t sequence, const std::string& record);

    Status scanSegments();

    bool needsCompaction() const;
    void cleanupInputSegments(const CompactionRange& effectiveRange);
    void cleanupOneDanglingSegment();
    void cleanupAllDanglingSegments();

    // Applies manifest fields to RAM state (activeGenerations_, nextGeneration_).
    // Called by publishManifest() after a successful write, and by scanSegments()
    // in Stage 3b after readManifest() returns the winning slot. One path avoids
    // divergence between publish and mount reconstruction.
    void applyManifestToRam(const append_log_detail::ManifestData& manifest);

    FileStoreIndex indexFromRecords() const;

    AppendLogConfig config_;
    mutable std::shared_ptr<FileSystem> fs_;

    bool mounted_ = false;

    // Active write segment
    std::uint32_t activeGeneration_ = 0;
    std::uint32_t activeSegmentBytes_ = 0;
    std::uint32_t totalOnDiskBytes_ = 0; // all seg-*.bin files, including dangling
    std::uint32_t nextGeneration_ = 1;

    // Total bytes per sealed segment generation. Populated at mount, updated on
    // rotate/compaction output/cleanup. Lets segmentStats() avoid fileSize() calls.
    std::unordered_map<std::uint32_t, std::uint32_t> sealedSegmentBytes_;

    // Cached manifest slot state: after a successful publishManifest(), remember
    // which slot was written and its epoch so the next call can skip the 2x fileSize
    // + 2x readFile probe. Cleared on mount/format to force a fresh read from disk.
    const char* cachedWrittenSlot_ = nullptr; // points to one of the kManifest* constants
    std::uint32_t cachedWrittenEpoch_ = 0;

#ifdef ARDUINO
    // Scratch timing set by publishManifest(), read by its callers for consolidated logs.
    std::uint32_t dbgLastProbeMs_ = 0;
    std::uint32_t dbgLastWriteMs_ = 0;
#endif

    // Logical active segment order (matches replay order from scanSegments)
    std::vector<std::uint32_t> activeGenerations_;

    // Full-segment ranges from the active manifest (mirrors ManifestData::ranges).
    // Updated by applyManifestToRam() on every successful publishManifest() or mount.
    std::vector<append_log_detail::ManifestRange> manifestRanges_;

    // In-order live records (front = queue head)
    std::deque<SegmentRecord> records_;

    // Set false on mount (history unknown). Set true with empty set by rotateSegment /
    // ensureActiveSegment. Guards whether activeTailAffectedGenerations_ is trustworthy.
    bool activeTailDependenciesTracked_ = false;
    // Source segment generations touched by POP/REWRITE events in the active tail.
    std::unordered_set<std::uint32_t> activeTailAffectedGenerations_;

    // One past the highest sequence seen during replay; preserved so an emptied
    // queue does not reset to sequence 0 on remount.
    std::uint32_t nextSequence_ = 0;

    // Pending enqueue state (set in writeRecord, cleared in writeIndex)
    bool hasPendingEnqueue_ = false;
    SegmentRecord pendingRecord_;
};

} // namespace pqueue

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "events.h"
#include "file_system.h"
#include "status.h"

namespace pqueue {

enum class StorageBackend {
    Default,
    Posix,
    LittleFS,
};

#ifdef ARDUINO
inline constexpr const char* kDefaultBasePath = "/pqueue_spool";
#else
inline constexpr const char* kDefaultBasePath = "build/pqueue-spools/pqueue_spool";
#endif

struct FileStoreConfig {
    std::string basePath = kDefaultBasePath;
    StorageBackend backend = StorageBackend::Default;
    std::shared_ptr<FileSystem> fileSystem;
    std::uint32_t reservedBytes = 128 * 1024;
    std::size_t recordSizeBytes = 4096;
    std::uint32_t journalBytes = 4096;
    std::uint32_t checkpointEveryOps = 64;
    EventOptions events;
};

struct FileStoreIndex {
    std::uint32_t head = 0;
    std::uint32_t tail = 0;
    std::uint32_t count = 0;
};

enum class ValidationRepairAction {
    None,
    Format,
    DropFrontIfCorrupt,
    RebuildMetadata,
};

enum class ValidationIssueCode {
    InvalidConfig,
    MetadataMissing,
    MetadataCorrupt,
    JournalCorrupt,
    ConfigMismatch,
    SpoolMissing,
    SpoolSizeMismatch,
    InvalidRingState,
    SlotReadFailed,
    SlotHeaderInvalid,
    SlotCrcMismatch,
    QueueLoadFailed,
    QueueIndexMismatch,
    OutboxEnvelopeInvalid,
    HttpRequestEnvelopeInvalid,
};

struct ValidationIssue {
    ValidationIssueCode code = ValidationIssueCode::InvalidConfig;
    std::string message;
    std::uint32_t slotIndex = 0;
    std::uint32_t expectedSequence = 0;
    std::uint32_t actualSequence = 0;
    bool hasSlotIndex = false;
    bool hasExpectedSequence = false;
    bool hasActualSequence = false;
    ValidationRepairAction repairAction = ValidationRepairAction::None;
};

struct ValidationResult {
    bool ok = true;
    std::uint32_t checkedRecords = 0;
    bool stoppedEarly = false;
    std::vector<ValidationIssue> errors;
};

struct ValidationOptions {
    std::size_t maxErrors = 100;
};

struct FileStoreRuntimeState {
    bool mounted = false;
    bool loaded = false;
    FileStoreIndex index;
    std::uint32_t checkpointGeneration = 0;
    std::uint32_t journalUsedBytes = 0;
    std::uint32_t journalOps = 0;
    std::uint32_t capacityRecords = 0;
    std::uint32_t recordSizeBytes = 0;
    std::uint32_t reservedBytes = 0;
    std::uint32_t journalBytes = 0;
    std::uint32_t checkpointEveryOps = 0;
    std::uint32_t slotSizeBytes = 0;
    std::uint32_t checkpointBytes = 0;
    std::uint32_t recordRegionOffset = 0;
    std::uint32_t spoolBytes = 0;
};

class FileStore {
    friend class Queue;

public:
    explicit FileStore(FileStoreConfig config = FileStoreConfig{});
    explicit FileStore(std::string basePath);

    Status mount();
    Status readIndex(FileStoreIndex& out);
    Status writeIndex(const FileStoreIndex& index);

    Status writeRecord(std::uint32_t sequence, const std::string& record);
    Status rewriteRecord(std::uint32_t sequence, const std::string& record);
    Status readRecord(std::uint32_t sequence, std::string& out);
    Status removeRecord(std::uint32_t sequence);

    Status tryAcquireLockFile(const std::string& name, const std::string& contents);
    Status releaseLockFile(const std::string& name, const std::string& expectedContents);
    Status recoverStaleLockFile(const std::string& name, const std::string& currentContents);

    std::uint64_t freeBytes() const;

private:
    StorageBackend resolvedBackend() const;
    std::shared_ptr<FileSystem> fileSystem() const;
    Status emit(Event event) const;
    Status diagnostic(Severity severity, Status status, const char* operation, std::uint32_t sequence = kNoSequence, const char* path = "") const;
    ValidationResult validateUnlocked(const ValidationOptions& options = ValidationOptions{});
    Status readIndexFromDisk(FileStoreIndex& out);
    Status format();
    Status rebuildMetadata();

    FileStoreConfig config_;
    mutable std::shared_ptr<FileSystem> fileSystem_;
    FileStoreRuntimeState runtime_;
};

} // namespace pqueue

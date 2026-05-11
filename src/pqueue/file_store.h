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

enum class StoreLayout {
    FixedSlot,
    AppendLog,
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

// Abstract storage interface used by Queue. Both FileStore (fixed-slot) and
// AppendLogStore implement this interface.
class Store {
public:
    virtual ~Store() = default;

    virtual Status mount() = 0;
    virtual Status readIndex(FileStoreIndex& out) = 0;
    virtual Status readIndexFromDisk(FileStoreIndex& out) = 0;
    virtual Status writeIndex(const FileStoreIndex& index) = 0;

    virtual Status writeRecord(std::uint32_t sequence, const std::string& record) = 0;
    virtual Status rewriteRecord(std::uint32_t sequence, const std::string& record) = 0;
    virtual Status readRecord(std::uint32_t sequence, std::string& out) = 0;
    virtual Status removeRecord(std::uint32_t sequence) = 0;

    virtual Status tryAcquireLockFile(const std::string& name, const std::string& contents) = 0;
    virtual Status releaseLockFile(const std::string& name, const std::string& expectedContents) = 0;
    virtual Status recoverStaleLockFile(const std::string& name, const std::string& currentContents) = 0;

    virtual std::uint64_t freeBytes() const = 0;
    virtual bool canEnqueue(std::size_t recordSize, std::uint32_t currentCount) const = 0;

    virtual Status format() = 0;
    virtual Status rebuildMetadata() = 0;
    virtual ValidationResult validateUnlocked(const ValidationOptions& options = ValidationOptions{}) = 0;
};

class FileStore : public Store {
public:
    explicit FileStore(FileStoreConfig config = FileStoreConfig{});
    explicit FileStore(std::string basePath);

    Status mount() override;
    Status readIndex(FileStoreIndex& out) override;
    Status readIndexFromDisk(FileStoreIndex& out) override;
    Status writeIndex(const FileStoreIndex& index) override;

    Status writeRecord(std::uint32_t sequence, const std::string& record) override;
    Status rewriteRecord(std::uint32_t sequence, const std::string& record) override;
    Status readRecord(std::uint32_t sequence, std::string& out) override;
    Status removeRecord(std::uint32_t sequence) override;

    Status tryAcquireLockFile(const std::string& name, const std::string& contents) override;
    Status releaseLockFile(const std::string& name, const std::string& expectedContents) override;
    Status recoverStaleLockFile(const std::string& name, const std::string& currentContents) override;

    std::uint64_t freeBytes() const override;
    bool canEnqueue(std::size_t recordSize, std::uint32_t currentCount) const override;

    Status format() override;
    Status rebuildMetadata() override;
    ValidationResult validateUnlocked(const ValidationOptions& options = ValidationOptions{}) override;

private:
    StorageBackend resolvedBackend() const;
    std::shared_ptr<FileSystem> fileSystem() const;
    Status emit(Event event) const;
    Status diagnostic(Severity severity, Status status, const char* operation, std::uint32_t sequence = kNoSequence, const char* path = "") const;

    FileStoreConfig config_;
    mutable std::shared_ptr<FileSystem> fileSystem_;
    FileStoreRuntimeState runtime_;
};

} // namespace pqueue

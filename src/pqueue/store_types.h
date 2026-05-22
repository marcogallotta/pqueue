#pragma once

#include <cstdint>
#include <cstddef>
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

struct QueueIndex {
    std::uint32_t head = 0;
    std::uint32_t tail = 0;
    std::uint32_t count = 0;
};

enum class ValidationRepairAction {
    None,
    Format,
    DropFrontIfCorrupt,
};

enum class ValidationIssueCode {
    InvalidConfig,
    MetadataCorrupt,
    JournalCorrupt,
    ConfigMismatch,
    RecordCrcMismatch,
    QueueLoadFailed,
    QueueIndexMismatch,
    OutboxEnvelopeInvalid,
    HttpRequestEnvelopeInvalid,
};

inline const char* validationIssueCodeName(ValidationIssueCode code) {
    switch (code) {
        case ValidationIssueCode::InvalidConfig:              return "invalid_config";
        case ValidationIssueCode::MetadataCorrupt:            return "metadata_corrupt";
        case ValidationIssueCode::JournalCorrupt:             return "journal_corrupt";
        case ValidationIssueCode::ConfigMismatch:             return "config_mismatch";
        case ValidationIssueCode::RecordCrcMismatch:          return "record_crc_mismatch";
        case ValidationIssueCode::QueueLoadFailed:            return "queue_load_failed";
        case ValidationIssueCode::QueueIndexMismatch:         return "queue_index_mismatch";
        case ValidationIssueCode::OutboxEnvelopeInvalid:      return "outbox_envelope_invalid";
        case ValidationIssueCode::HttpRequestEnvelopeInvalid: return "http_request_envelope_invalid";
    }
    return "unknown";
}

struct ValidationIssue {
    ValidationIssueCode code = ValidationIssueCode::InvalidConfig;
    std::string message;
    std::uint32_t recordIndex = 0;
    std::uint32_t expectedSequence = 0;
    std::uint32_t actualSequence = 0;
    bool hasRecordIndex = false;
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

struct CompactIdleResult {
    Status status;
    std::size_t stepsRun = 0;
    std::size_t compactions = 0;
    std::size_t noOps = 0;
    // True when the loop stopped because maxSteps was exhausted after at least
    // one successful compaction -- meaning more candidates likely remain.
    // False when the loop stopped due to a noOp or when maxSteps was 0.
    bool moreWorkLikely = false;
    std::uint32_t bytesReclaimed = 0;      // totalOnDiskBytes before - after
    std::uint32_t deadBytesBefore = 0;     // dead bytes across referenced sealed segments before the call
    std::uint32_t remainingDeadBytes = 0;  // dead bytes across referenced sealed segments after the call
    std::uint32_t inputSegments = 0;       // total sealed segments consumed across all compaction steps
    std::uint32_t outputSegments = 0;      // total segments written across all compaction steps
};

class Store {
public:
    virtual ~Store() = default;

    virtual Status mount() = 0;
    virtual Status readIndex(QueueIndex& out) = 0;

    virtual Status commitEnqueue(std::uint32_t sequence, const std::string& record) = 0;
    virtual Status commitPop(std::uint32_t expectedSequence) = 0;
    virtual Status rewriteRecord(std::uint32_t sequence, const std::string& record) = 0;
    virtual Status readRecord(std::uint32_t sequence, std::string& out) = 0;

    virtual Status tryAcquireLockFile(const std::string& name, const std::string& contents) = 0;
    virtual Status releaseLockFile(const std::string& name, const std::string& expectedContents) = 0;
    virtual Status recoverStaleLockFile(const std::string& name, const std::string& currentContents) = 0;

    virtual std::uint64_t freeBytes() const = 0;
    virtual bool canEnqueue(std::size_t recordSize) const = 0;

    virtual Status format() = 0;
    virtual Status rebuildMetadata() = 0;
    virtual ValidationResult validateUnlocked(const ValidationOptions& options = ValidationOptions{}) = 0;
    virtual CompactIdleResult compactIdle(std::size_t maxSteps) = 0;
};

} // namespace pqueue

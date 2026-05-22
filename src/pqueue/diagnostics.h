#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "append_log_store.h"
#include "file_store.h"
#include "status.h"

namespace pqueue {

enum class CheckpointSlotState {
    ReadFailed,
    Zero,
    ParseFailed,
    InvalidMagic,
    InvalidShape,
    ConfigMismatch,
    Usable,
};

struct CheckpointSlotDiagnostic {
    std::uint32_t slot = 0;
    CheckpointSlotState state = CheckpointSlotState::ReadFailed;
    Status readStatus = Status::success();
    bool allZero = false;
    bool parsed = false;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint32_t generation = 0;
    std::uint32_t head = 0;
    std::uint32_t tail = 0;
    std::uint32_t count = 0;
    std::uint32_t capacityRecords = 0;
    std::uint32_t recordSizeBytes = 0;
    std::uint32_t reservedBytes = 0;
    std::uint32_t journalBytes = 0;
    std::uint32_t journalUsedBytes = 0;
    std::uint16_t checkpointBytes = 0;
    std::uint32_t storedCrc = 0;
    std::uint32_t computedCrc = 0;
};

struct FileStoreLayoutDiagnostic {
    bool valid = false;
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

struct FileStoreDiagnostic {
    std::string basePath;
    StorageBackend backend = StorageBackend::Default;
    Status mountStatus = Status::success();
    Status listStatus = Status::success();
    Status spoolSizeStatus = Status::success();
    FileStoreLayoutDiagnostic layout;
    std::uint64_t freeBytes = 0;
    std::vector<std::string> files;
    bool spoolExists = false;
    bool spoolListed = false;
    std::uint64_t spoolSizeBytes = 0;
    bool spoolSizeMatches = false;
    bool legacyDotLockListed = false;
    bool legacyNamedLockListed = false;
    std::string firstBytesHex;
    std::vector<CheckpointSlotDiagnostic> checkpointSlots;
    bool hasUsableCheckpoint = false;
    bool hasConfigMismatch = false;
};

FileStoreDiagnostic diagnoseFileStore(
    const FileStoreConfig& config,
    std::size_t firstBytesToHex = 192
);

const char* checkpointSlotStateName(CheckpointSlotState state);

// --- AppendLog diagnostic ---

struct AppendLogDiagnosticRange {
    std::uint32_t startGen = 0;
    std::uint32_t endGen   = 0;
};

struct AppendLogManifestSlotDiagnostic {
    bool exists = false;
    bool valid  = false;
    std::uint32_t epoch = 0;
};

struct AppendLogSegmentDiagnostic {
    std::uint32_t generation = 0;
    std::uint64_t sizeBytes  = 0;
    bool referenced = false; // in manifest ranges or tail
    bool isTail     = false;
};

struct AppendLogStoreDiagnostic {
    std::string basePath;
    Status mountStatus = Status::success();
    Status listStatus  = Status::success();
    std::uint64_t freeBytes = 0;

    AppendLogManifestSlotDiagnostic slotA;
    AppendLogManifestSlotDiagnostic slotB;
    bool hasWinner            = false;
    std::uint32_t winnerEpoch = 0;

    std::vector<AppendLogDiagnosticRange> ranges;
    std::uint32_t tailGeneration  = 0;
    std::uint32_t nextGeneration  = 0;

    std::vector<AppendLogSegmentDiagnostic> segments; // all on disk, sorted by generation
    std::size_t danglingSegments = 0;
};

AppendLogStoreDiagnostic diagnoseAppendLogStore(const AppendLogConfig& config);

} // namespace pqueue

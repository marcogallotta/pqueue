#include "diagnostics.h"

#include "file_system.h"
#include "storage_common.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace pqueue {
namespace {

using namespace storage_detail;

constexpr const char* kSpoolName = "pqueue.spool";
constexpr const char* kLegacyDotLockName = ".pqueue.lock";
constexpr const char* kLegacyNamedLockName = "pqueue.lock";

bool bytesAllZero(const std::string& bytes) {
    for (char c : bytes) {
        if (c != '\0') {
            return false;
        }
    }
    return true;
}

std::string hexEncode(const std::string& bytes) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    if (bytes.empty()) {
        return out;
    }
    out.reserve(bytes.size() * 3 - 1);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto value = static_cast<unsigned char>(bytes[i]);
        if (i != 0) {
            out.push_back(' ');
        }
        out.push_back(kHex[(value >> 4U) & 0x0FU]);
        out.push_back(kHex[value & 0x0FU]);
    }
    return out;
}

StorageBackend resolveBackend(StorageBackend configured) {
    if (configured != StorageBackend::Default) {
        return configured;
    }
#ifdef ARDUINO
    return StorageBackend::LittleFS;
#else
    return StorageBackend::Posix;
#endif
}

std::shared_ptr<FileSystem> resolveFileSystem(const FileStoreConfig& config) {
    if (config.fileSystem) {
        return config.fileSystem;
    }

    switch (resolveBackend(config.backend)) {
    case StorageBackend::Posix:
        return makePosixFileSystem();
    case StorageBackend::LittleFS:
        return makeLittleFsFileSystem();
    case StorageBackend::Default:
        break;
    }

    return nullptr;
}

bool makeLayout(const FileStoreConfig& config, FileStoreLayoutDiagnostic& out) {
    out = FileStoreLayoutDiagnostic{};
    if (config.recordSizeBytes == 0 ||
        config.reservedBytes == 0 ||
        config.journalBytes < kJournalEntryBytes ||
        config.checkpointEveryOps == 0) {
        return false;
    }
    if (config.recordSizeBytes > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    const auto slotSize = static_cast<std::uint64_t>(kRecordHeaderBytes) + config.recordSizeBytes;
    if (slotSize > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    const auto capacity = config.reservedBytes / slotSize;
    if (capacity == 0 || capacity > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    const auto checkpointBytes = static_cast<std::uint64_t>(kCheckpointSlots) * kCheckpointRecordBytes;
    const auto recordRegionOffset = checkpointBytes + config.journalBytes;
    const auto spoolBytes = recordRegionOffset + capacity * slotSize;
    if (spoolBytes > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    out.valid = true;
    out.capacityRecords = static_cast<std::uint32_t>(capacity);
    out.recordSizeBytes = static_cast<std::uint32_t>(config.recordSizeBytes);
    out.reservedBytes = config.reservedBytes;
    out.journalBytes = config.journalBytes;
    out.checkpointEveryOps = config.checkpointEveryOps;
    out.slotSizeBytes = static_cast<std::uint32_t>(slotSize);
    out.checkpointBytes = static_cast<std::uint32_t>(checkpointBytes);
    out.recordRegionOffset = static_cast<std::uint32_t>(recordRegionOffset);
    out.spoolBytes = static_cast<std::uint32_t>(spoolBytes);
    return true;
}

std::uint64_t checkpointOffset(std::uint32_t slot) {
    return static_cast<std::uint64_t>(slot) * kCheckpointRecordBytes;
}

bool containsFile(const std::vector<std::string>& files, const char* name) {
    return std::find(files.begin(), files.end(), name) != files.end();
}

CheckpointSlotDiagnostic diagnoseCheckpointSlot(
    FileSystem& fs,
    const FileStoreLayoutDiagnostic& layout,
    std::uint32_t slot
) {
    CheckpointSlotDiagnostic out;
    out.slot = slot;

    std::string bytes;
    out.readStatus = fs.readAt(kSpoolName, checkpointOffset(slot), kCheckpointRecordBytes, bytes);
    if (!out.readStatus.ok() || bytes.size() != kCheckpointRecordBytes) {
        out.state = CheckpointSlotState::ReadFailed;
        return out;
    }

    out.allZero = bytesAllZero(bytes);
    if (out.allZero) {
        out.state = CheckpointSlotState::Zero;
        return out;
    }

    CheckpointRecord record;
    out.parsed = parseCheckpointRecord(bytes, record);
    if (!out.parsed) {
        out.state = CheckpointSlotState::ParseFailed;
        return out;
    }

    out.magic = record.magic;
    out.version = record.version;
    out.generation = record.generation;
    out.head = record.head;
    out.tail = record.tail;
    out.count = record.count;
    out.capacityRecords = record.capacityRecords;
    out.recordSizeBytes = record.recordSizeBytes;
    out.reservedBytes = record.reservedBytes;
    out.journalBytes = record.journalBytes;
    out.journalUsedBytes = record.journalUsedBytes;
    out.checkpointBytes = record.checkpointBytes;
    out.storedCrc = record.crc;
    out.computedCrc = checkpointCrc(record);

    if (record.magic != kFileStoreCheckpointMagic) {
        out.state = CheckpointSlotState::InvalidMagic;
        return out;
    }

    if (!validCheckpointShape(record)) {
        out.state = CheckpointSlotState::InvalidShape;
        return out;
    }

    if (!validCheckpointForConfig(record, layout.capacityRecords, layout.recordSizeBytes, layout.reservedBytes, layout.journalBytes)) {
        out.state = CheckpointSlotState::ConfigMismatch;
        return out;
    }

    out.state = CheckpointSlotState::Usable;
    return out;
}

} // namespace

FileStoreDiagnostic diagnoseFileStore(const FileStoreConfig& config, std::size_t firstBytesToHex) {
    FileStoreDiagnostic out;
    out.basePath = config.basePath;
    out.backend = resolveBackend(config.backend);
    makeLayout(config, out.layout);

    const auto fs = resolveFileSystem(config);
    if (!fs) {
        out.mountStatus = Status::failure(StatusCode::BackendUnavailable, "file system backend unavailable");
        return out;
    }

    out.mountStatus = fs->mount(config.basePath);
    if (!out.mountStatus.ok()) {
        return out;
    }

    out.freeBytes = fs->freeBytes();
    out.listStatus = fs->listFiles(out.files);
    if (out.listStatus.ok()) {
        out.spoolListed = containsFile(out.files, kSpoolName);
        out.legacyDotLockListed = containsFile(out.files, kLegacyDotLockName);
        out.legacyNamedLockListed = containsFile(out.files, kLegacyNamedLockName);
    }

    out.spoolSizeStatus = fs->fileSize(kSpoolName, out.spoolSizeBytes);
    out.spoolExists = out.spoolSizeStatus.ok();
    out.spoolSizeMatches = out.spoolExists && out.layout.valid && out.spoolSizeBytes == out.layout.spoolBytes;

    if (out.spoolExists && firstBytesToHex != 0) {
        std::string bytes;
        const auto readSize = static_cast<std::size_t>(std::min<std::uint64_t>(firstBytesToHex, out.spoolSizeBytes));
        if (readSize != 0 && fs->readAt(kSpoolName, 0, readSize, bytes).ok()) {
            out.firstBytesHex = hexEncode(bytes);
        }
    }

    if (out.spoolExists && out.layout.valid) {
        out.checkpointSlots.reserve(kCheckpointSlots);
        for (std::uint32_t slot = 0; slot < kCheckpointSlots; ++slot) {
            CheckpointSlotDiagnostic slotDiag = diagnoseCheckpointSlot(*fs, out.layout, slot);
            if (slotDiag.state == CheckpointSlotState::Usable) {
                out.hasUsableCheckpoint = true;
            }
            if (slotDiag.state == CheckpointSlotState::ConfigMismatch) {
                out.hasConfigMismatch = true;
            }
            out.checkpointSlots.push_back(std::move(slotDiag));
        }
    }

    return out;
}

const char* checkpointSlotStateName(CheckpointSlotState state) {
    switch (state) {
    case CheckpointSlotState::ReadFailed:
        return "read_failed";
    case CheckpointSlotState::Zero:
        return "zero";
    case CheckpointSlotState::ParseFailed:
        return "parse_failed";
    case CheckpointSlotState::InvalidMagic:
        return "invalid_magic";
    case CheckpointSlotState::InvalidShape:
        return "invalid_shape";
    case CheckpointSlotState::ConfigMismatch:
        return "config_mismatch";
    case CheckpointSlotState::Usable:
        return "usable";
    }
    return "unknown";
}

} // namespace pqueue

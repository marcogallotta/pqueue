#include "diagnostics.h"

#include "append_log_common.h"
#include "file_system.h"
#include "storage_common.h"

#include <algorithm>
#include <cctype>
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

// ---------------------------------------------------------------------------
// AppendLog diagnostic
// ---------------------------------------------------------------------------

namespace {

constexpr const char* kDiagManifestSlotA = "manifest-a.bin";
constexpr const char* kDiagManifestSlotB = "manifest-b.bin";
constexpr const char* kDiagSegmentPrefix  = "seg-";
constexpr const char* kDiagSegmentSuffix  = ".bin";

std::shared_ptr<FileSystem> resolveAppendLogFs(const AppendLogConfig& config) {
    if (config.fileSystem) return config.fileSystem;
    switch (resolveBackend(config.backend)) {
    case StorageBackend::Posix:    return makePosixFileSystem();
    case StorageBackend::LittleFS: return makeLittleFsFileSystem();
    case StorageBackend::Default:  break;
    }
    return nullptr;
}

bool parseSegmentGen(const std::string& name, std::uint32_t& genOut) {
    const std::size_t prefixLen = std::string(kDiagSegmentPrefix).size();
    const std::size_t suffixLen = std::string(kDiagSegmentSuffix).size();
    if (name.size() != prefixLen + 8 + suffixLen) return false;
    if (name.compare(0, prefixLen, kDiagSegmentPrefix) != 0) return false;
    if (name.compare(name.size() - suffixLen, suffixLen, kDiagSegmentSuffix) != 0) return false;
    const std::string hex = name.substr(prefixLen, 8);
    for (char c : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    genOut = static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
    return true;
}

// Read and parse a manifest slot; returns true if valid.
bool readManifestSlot(FileSystem& fs, const char* name,
                      append_log_detail::ManifestData& mdOut) {
    std::string data;
    if (!fs.readFile(name, data).ok()) return false;
    return append_log_detail::parseManifest(
        reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), mdOut);
}

} // namespace

AppendLogStoreDiagnostic diagnoseAppendLogStore(const AppendLogConfig& config) {
    AppendLogStoreDiagnostic out;
    out.basePath = config.basePath;

    auto f = resolveAppendLogFs(config);
    if (!f) {
        out.mountStatus = Status::failure(StatusCode::BackendUnavailable,
                                          "file system backend unavailable");
        return out;
    }

    out.mountStatus = f->mount(config.basePath);
    if (!out.mountStatus.ok()) return out;

    out.freeBytes = f->freeBytes();

    // Read both manifest slots
    append_log_detail::ManifestData mdA, mdB;
    {
        std::uint64_t sz = 0;
        out.slotA.exists = f->fileSize(kDiagManifestSlotA, sz).ok();
        out.slotA.valid  = out.slotA.exists && readManifestSlot(*f, kDiagManifestSlotA, mdA);
        if (out.slotA.valid) out.slotA.epoch = mdA.epoch;

        out.slotB.exists = f->fileSize(kDiagManifestSlotB, sz).ok();
        out.slotB.valid  = out.slotB.exists && readManifestSlot(*f, kDiagManifestSlotB, mdB);
        if (out.slotB.valid) out.slotB.epoch = mdB.epoch;
    }

    // Elect winning slot (same logic as manifest.cpp chooseWinningSlot)
    append_log_detail::ManifestData winner;
    if (out.slotA.valid || out.slotB.valid) {
        out.hasWinner = true;
        if (out.slotA.valid && out.slotB.valid)
            winner = (mdB.epoch > mdA.epoch) ? mdB : mdA;
        else
            winner = out.slotA.valid ? mdA : mdB;
        out.winnerEpoch    = winner.epoch;
        out.ranges.reserve(winner.ranges.size());
        for (const auto& r : winner.ranges)
            out.ranges.push_back({r.startGen, r.endGen});
        out.tailGeneration = winner.tailGeneration;
        out.nextGeneration = winner.nextGeneration;
    }

    // Classify a generation against the winning manifest without range expansion.
    auto isReferenced = [&](std::uint32_t gen) -> bool {
        if (!out.hasWinner) return false;
        if (winner.tailGeneration != 0 && gen == winner.tailGeneration) return true;
        for (const auto& r : winner.ranges)
            if (gen >= r.startGen && gen <= r.endGen) return true;
        return false;
    };

    // List all segment files on disk
    std::vector<std::string> files;
    out.listStatus = f->listFiles(files);
    for (const auto& name : files) {
        std::uint32_t gen = 0;
        if (!parseSegmentGen(name, gen)) continue;
        AppendLogSegmentDiagnostic seg;
        seg.generation = gen;
        f->fileSize(name, seg.sizeBytes);
        seg.referenced = isReferenced(gen);
        seg.isTail     = out.hasWinner && (gen == winner.tailGeneration);
        out.segments.push_back(seg);
        if (!seg.referenced) ++out.danglingSegments;
    }

    std::sort(out.segments.begin(), out.segments.end(),
        [](const AppendLogSegmentDiagnostic& a, const AppendLogSegmentDiagnostic& b) {
            return a.generation < b.generation;
        });

    return out;
}

} // namespace pqueue

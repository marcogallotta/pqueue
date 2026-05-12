#include "file_store.h"
#include "storage_common.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace pqueue {
namespace {

using namespace storage_detail;

constexpr const char* kSpoolName = "pqueue.spool";
constexpr std::uint32_t kJournalFullPercent = 75;

bool fileExists(FileSystem& fs, const std::string& name) {
    std::vector<std::string> files;
    const Status listStatus = fs.listFiles(files);
    if (listStatus.ok()) {
        return std::find(files.begin(), files.end(), name) != files.end();
    }

    std::uint64_t ignored = 0;
    return fs.fileSize(name, ignored).ok();
}

struct Layout {
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

bool makeLayout(const FileStoreConfig& config, Layout& out) {
    if (config.recordSizeBytes == 0 || config.reservedBytes == 0 || config.journalBytes < kJournalEntryBytes || config.checkpointEveryOps == 0) {
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

std::uint64_t journalOffset(const Layout& layout, std::uint32_t usedBytes) {
    return static_cast<std::uint64_t>(layout.checkpointBytes) + usedBytes;
}

std::uint64_t slotOffset(const Layout& layout, std::uint32_t sequence) {
    return static_cast<std::uint64_t>(layout.recordRegionOffset) +
           static_cast<std::uint64_t>(sequence % layout.capacityRecords) * layout.slotSizeBytes;
}

bool ringStateMatches(const CheckpointRecord& record) {
    return record.count <= record.capacityRecords &&
           record.tail >= record.head &&
           record.count == record.tail - record.head;
}

bool bytesAllZero(const std::string& bytes) {
    for (char c : bytes) {
        if (c != '\0') {
            return false;
        }
    }
    return true;
}

bool regionHasNonZero(FileSystem& fs, const Layout& layout, std::uint32_t startOffset) {
    for (std::uint32_t offset = startOffset; offset + kJournalEntryBytes <= layout.journalBytes; offset += kJournalEntryBytes) {
        std::string bytes;
        if (!fs.readAt(kSpoolName, journalOffset(layout, offset), kJournalEntryBytes, bytes).ok()) {
            return true;
        }
        if (!bytesAllZero(bytes)) {
            return true;
        }
    }
    return false;
}

bool spoolIsAllZero(FileSystem& fs, const Layout& layout) {
    constexpr std::size_t kReadChunkBytes = 256;

    std::uint64_t offset = 0;
    while (offset < layout.spoolBytes) {
        const auto remaining = static_cast<std::size_t>(layout.spoolBytes - offset);
        const std::size_t bytesToRead = std::min(kReadChunkBytes, remaining);

        std::string bytes;
        if (!fs.readAt(kSpoolName, offset, bytesToRead, bytes).ok()) {
            return false;
        }
        if (!bytesAllZero(bytes)) {
            return false;
        }

        offset += bytesToRead;
    }

    return true;
}

void addValidationError(ValidationResult& result, const ValidationOptions& options, ValidationIssue issue) {
    result.ok = false;
    if (result.errors.size() < options.maxErrors) {
        result.errors.push_back(std::move(issue));
    } else {
        result.stoppedEarly = true;
    }
}

ValidationIssue makeIssue(ValidationIssueCode code, std::string message) {
    ValidationIssue issue;
    issue.code = code;
    issue.message = std::move(message);
    return issue;
}

ValidationIssue makeSlotIssue(ValidationIssueCode code, std::string message, std::uint32_t slotIndex, std::uint32_t expectedSequence) {
    ValidationIssue issue = makeIssue(code, std::move(message));
    issue.slotIndex = slotIndex;
    issue.expectedSequence = expectedSequence;
    issue.hasSlotIndex = true;
    issue.hasExpectedSequence = true;
    return issue;
}

struct StoredState {
    bool foundCheckpoint = false;
    bool configMismatch = false;
    bool readFailed = false;
    Status readFailedStatus = Status::success();
    CheckpointRecord checkpoint;
    FileStoreIndex index;
    std::uint32_t journalUsedBytes = 0;
    std::uint32_t journalOps = 0;
};


Layout layoutFromRuntime(const FileStoreRuntimeState& runtime) {
    Layout layout;
    layout.capacityRecords = runtime.capacityRecords;
    layout.recordSizeBytes = runtime.recordSizeBytes;
    layout.reservedBytes = runtime.reservedBytes;
    layout.journalBytes = runtime.journalBytes;
    layout.checkpointEveryOps = runtime.checkpointEveryOps;
    layout.slotSizeBytes = runtime.slotSizeBytes;
    layout.checkpointBytes = runtime.checkpointBytes;
    layout.recordRegionOffset = runtime.recordRegionOffset;
    layout.spoolBytes = runtime.spoolBytes;
    return layout;
}

StoredState storedStateFromRuntime(const FileStoreRuntimeState& runtime) {
    StoredState state;
    state.foundCheckpoint = runtime.loaded;
    state.index = runtime.index;
    state.journalUsedBytes = runtime.journalUsedBytes;
    state.journalOps = runtime.journalOps;
    state.checkpoint.generation = runtime.checkpointGeneration;
    return state;
}

void setRuntime(FileStoreRuntimeState& runtime, const Layout& layout, const StoredState& state) {
    runtime.mounted = true;
    runtime.loaded = state.foundCheckpoint;
    runtime.index = state.index;
    runtime.checkpointGeneration = state.checkpoint.generation;
    runtime.journalUsedBytes = state.journalUsedBytes;
    runtime.journalOps = state.journalOps;
    runtime.capacityRecords = layout.capacityRecords;
    runtime.recordSizeBytes = layout.recordSizeBytes;
    runtime.reservedBytes = layout.reservedBytes;
    runtime.journalBytes = layout.journalBytes;
    runtime.checkpointEveryOps = layout.checkpointEveryOps;
    runtime.slotSizeBytes = layout.slotSizeBytes;
    runtime.checkpointBytes = layout.checkpointBytes;
    runtime.recordRegionOffset = layout.recordRegionOffset;
    runtime.spoolBytes = layout.spoolBytes;
}

void setRuntimeFresh(FileStoreRuntimeState& runtime, const Layout& layout, std::uint32_t checkpointGeneration) {
    StoredState state;
    state.foundCheckpoint = true;
    state.checkpoint.generation = checkpointGeneration;
    state.index = FileStoreIndex{};
    setRuntime(runtime, layout, state);
}

bool applyJournalEntry(const JournalEntry& entry, FileStoreIndex& index) {
    switch (entry.op) {
    case JournalOp::Enqueue:
        if (entry.sequence != index.tail) {
            return false;
        }
        ++index.tail;
        ++index.count;
        return true;
    case JournalOp::Pop:
        if (index.count == 0 || entry.sequence != index.head) {
            return false;
        }
        ++index.head;
        --index.count;
        return true;
    case JournalOp::RewriteFront:
        return index.count > 0 && entry.sequence >= index.head && entry.sequence < index.tail;
    }
    return false;
}

StoredState loadStoredState(FileSystem& fs, const Layout& layout) {
    StoredState out;

    const std::size_t metadataBytes = layout.checkpointBytes + layout.journalBytes;
    std::string metadata;
    const Status readStatus = fs.readAt(kSpoolName, 0, metadataBytes, metadata);
    if (!readStatus.ok()) {
        out.readFailed = true;
        out.readFailedStatus = readStatus;
        return out;
    }

    for (std::uint32_t slot = 0; slot < kCheckpointSlots; ++slot) {
        const std::size_t offset = static_cast<std::size_t>(slot) * kCheckpointRecordBytes;
        CheckpointRecord candidate;
        if (!parseCheckpointRecord(metadata.substr(offset, kCheckpointRecordBytes), candidate) || !validCheckpointShape(candidate)) {
            continue;
        }
        if (!validCheckpointForConfig(candidate, layout.capacityRecords, layout.recordSizeBytes, layout.reservedBytes, layout.journalBytes)) {
            out.configMismatch = true;
            continue;
        }
        if (!out.foundCheckpoint || candidate.generation > out.checkpoint.generation) {
            out.foundCheckpoint = true;
            out.checkpoint = candidate;
        }
    }

    if (!out.foundCheckpoint) {
        return out;
    }

    out.index = fromCheckpointRecord(out.checkpoint);
    std::uint32_t offset = 0;
    std::uint32_t nextGeneration = out.checkpoint.generation + 1;
    while (offset + kJournalEntryBytes <= layout.journalBytes) {
        const std::size_t bufOffset = layout.checkpointBytes + offset;
        JournalEntry entry;
        if (!parseJournalEntry(metadata.substr(bufOffset, kJournalEntryBytes), entry) || !validJournalEntryShape(entry) || entry.generation != nextGeneration) {
            break;
        }
        if (!applyJournalEntry(entry, out.index)) {
            break;
        }
        offset += kJournalEntryBytes;
        ++nextGeneration;
        ++out.journalOps;
    }
    out.journalUsedBytes = offset;
    return out;
}

Status writeCheckpoint(FileSystem& fs, const Layout& layout, const FileStoreIndex& index, std::uint32_t generation) {
    const CheckpointRecord record = toCheckpointRecord(index, generation, layout.capacityRecords, layout.recordSizeBytes, layout.reservedBytes, layout.journalBytes, 0);
    const std::uint32_t slot = generation % kCheckpointSlots;
    return fs.writeAt(kSpoolName, checkpointOffset(slot), serializeCheckpointRecord(record));
}

bool shouldCheckpoint(const Layout& layout, const StoredState& state) {
    const std::uint32_t nextUsed = state.journalUsedBytes + kJournalEntryBytes;
    if (nextUsed > layout.journalBytes) {
        return true;
    }
    if (state.journalOps + 1 >= layout.checkpointEveryOps) {
        return true;
    }
    return nextUsed * 100U >= layout.journalBytes * kJournalFullPercent;
}

Status commitIndexTransition(FileSystem& fs, const Layout& layout, FileStoreRuntimeState& runtime, const FileStoreIndex& next) {
    StoredState state = storedStateFromRuntime(runtime);
    JournalEntry entry;
    bool knownTransition = true;

    if (next.head == state.index.head && next.tail == state.index.tail + 1 && next.count == state.index.count + 1) {
        entry.op = JournalOp::Enqueue;
        entry.sequence = state.index.tail;
    } else if (next.head == state.index.head + 1 && next.tail == state.index.tail && next.count + 1 == state.index.count) {
        entry.op = JournalOp::Pop;
        entry.sequence = state.index.head;
    } else if (next.head == state.index.head && next.tail == state.index.tail && next.count == state.index.count) {
        return Status::success();
    } else {
        knownTransition = false;
    }

    const std::uint32_t nextGeneration = state.checkpoint.generation + state.journalOps + 1;
    if (!knownTransition || shouldCheckpoint(layout, state)) {
        Status st = writeCheckpoint(fs, layout, next, nextGeneration);
        if (st.ok()) {
            runtime.index = next;
            runtime.checkpointGeneration = nextGeneration;
            runtime.journalUsedBytes = 0;
            runtime.journalOps = 0;
        }
        return st;
    }

    entry.generation = nextGeneration;
    entry.crc = journalCrc(entry);
    Status st = fs.writeAt(kSpoolName, journalOffset(layout, state.journalUsedBytes), serializeJournalEntry(entry));
    if (st.ok()) {
        runtime.index = next;
        runtime.journalUsedBytes += kJournalEntryBytes;
        runtime.journalOps += 1;
    }
    return st;
}

Status commitRewrite(FileSystem& fs, const Layout& layout, FileStoreRuntimeState& runtime, std::uint32_t sequence) {
    StoredState state = storedStateFromRuntime(runtime);
    const std::uint32_t nextGeneration = state.checkpoint.generation + state.journalOps + 1;
    if (shouldCheckpoint(layout, state)) {
        Status st = writeCheckpoint(fs, layout, state.index, nextGeneration);
        if (st.ok()) {
            runtime.checkpointGeneration = nextGeneration;
            runtime.journalUsedBytes = 0;
            runtime.journalOps = 0;
        }
        return st;
    }
    JournalEntry entry;
    entry.op = JournalOp::RewriteFront;
    entry.sequence = sequence;
    entry.generation = nextGeneration;
    entry.crc = journalCrc(entry);
    Status st = fs.writeAt(kSpoolName, journalOffset(layout, state.journalUsedBytes), serializeJournalEntry(entry));
    if (st.ok()) {
        runtime.journalUsedBytes += kJournalEntryBytes;
        runtime.journalOps += 1;
    }
    return st;
}


Status rawMount(const FileStoreConfig& config, std::shared_ptr<FileSystem>& fileSystem, StorageBackend backend) {
    if (!fileSystem) {
        switch (backend) {
        case StorageBackend::Posix:
            fileSystem = makePosixFileSystem();
            break;
        case StorageBackend::LittleFS:
            fileSystem = makeLittleFsFileSystem();
            break;
        case StorageBackend::Default:
            break;
        }
    }
    if (!fileSystem) {
        return Status::failure(StatusCode::BackendUnavailable, "file system backend unavailable");
    }
    return fileSystem->mount(config.basePath);
}

Status zeroFile(FileSystem& fs, const Layout& layout) {
    constexpr std::uint32_t kChunkBytes = 256;
    const std::string zeros(kChunkBytes, '\0');
    std::uint32_t offset = 0;
    while (offset < layout.spoolBytes) {
        const std::uint32_t bytes = std::min(kChunkBytes, layout.spoolBytes - offset);
        Status st = fs.writeAt(kSpoolName, offset, zeros.substr(0, bytes));
        if (!st.ok()) {
            return st;
        }
        offset += bytes;
    }
    return Status::success();
}

} // namespace

FileStore::FileStore(FileStoreConfig config)
    : config_(std::move(config)), fileSystem_(config_.fileSystem) {}

FileStore::FileStore(std::string basePath) {
    config_.basePath = std::move(basePath);
}

StorageBackend FileStore::resolvedBackend() const {
    if (config_.backend != StorageBackend::Default) {
        return config_.backend;
    }
#ifdef ARDUINO
    return StorageBackend::LittleFS;
#else
    return StorageBackend::Posix;
#endif
}

std::shared_ptr<FileSystem> FileStore::fileSystem() const {
    if (fileSystem_) {
        return fileSystem_;
    }

    switch (resolvedBackend()) {
    case StorageBackend::Posix:
        fileSystem_ = makePosixFileSystem();
        break;
    case StorageBackend::LittleFS:
        fileSystem_ = makeLittleFsFileSystem();
        break;
    case StorageBackend::Default:
        break;
    }

    return fileSystem_;
}

Status FileStore::emit(Event event) const {
    config_.events.emit(event);
    return event.status;
}

Status FileStore::diagnostic(Severity severity, Status status, const char* operation, std::uint32_t sequence, const char* path) const {
    return emit(Event{
        EventKind::Diagnostic,
        severity,
        status,
        "FileStore",
        operation,
        sequence,
        path,
    });
}

Status FileStore::mount() {
    if (runtime_.mounted && runtime_.loaded) {
        return Status::success();
    }

    const auto fs = fileSystem();
    if (!fs) {
        return diagnostic(Severity::Error, Status::failure(StatusCode::BackendUnavailable, "file system backend unavailable"), "mount");
    }
    Status st = fs->mount(config_.basePath);
    if (!st.ok()) {
        return diagnostic(Severity::Error, st, "mount");
    }
    Layout layout;
    if (!makeLayout(config_, layout)) {
        return diagnostic(Severity::Error, Status::failure(StatusCode::InvalidArgument, "invalid pqueue storage config: reservedBytes must fit at least one recordSizeBytes slot and journalBytes must fit at least one entry"), "mount");
    }

    const bool hasSpool = fileExists(*fs, kSpoolName);
    if (!hasSpool) {
        st = fs->resizeFile(kSpoolName, layout.spoolBytes);
        if (!st.ok()) {
            return diagnostic(Severity::Error, st, "mount", kNoSequence, kSpoolName);
        }
        constexpr std::uint32_t kInitialGeneration = 1;
        st = writeCheckpoint(*fs, layout, FileStoreIndex{}, kInitialGeneration);
        if (!st.ok()) {
            return diagnostic(Severity::Error, st, "mount", kNoSequence, kSpoolName);
        }
        setRuntimeFresh(runtime_, layout, kInitialGeneration);
        return Status::success();
    }

    st = [&] {
        std::uint64_t actual = 0;
        Status sizeStatus = fs->fileSize(kSpoolName, actual);
        if (!sizeStatus.ok()) {
            return Status::failure(StatusCode::ReadFailed, "pqueue spool file is missing");
        }
        if (actual != layout.spoolBytes) {
            return Status::failure(StatusCode::InvalidIndex, "pqueue spool size does not match configured storage layout");
        }
        return Status::success();
    }();
    if (!st.ok()) {
        return diagnostic(Severity::Error, st, "mount", kNoSequence, kSpoolName);
    }

    const StoredState state = loadStoredState(*fs, layout);
    if (state.readFailed) {
        return diagnostic(Severity::Error, state.readFailedStatus, "mount");
    }
    if (state.configMismatch) {
        return diagnostic(Severity::Error, Status::failure(StatusCode::InvalidIndex, "pqueue storage config changed; delete or reformat queue files to continue"), "mount");
    }
    if (!state.foundCheckpoint) {
        if (spoolIsAllZero(*fs, layout)) {
            constexpr std::uint32_t kInitialGeneration = 1;
            st = writeCheckpoint(*fs, layout, FileStoreIndex{}, kInitialGeneration);
            if (!st.ok()) {
                return diagnostic(Severity::Error, st, "mount", kNoSequence, kSpoolName);
            }
            setRuntimeFresh(runtime_, layout, kInitialGeneration);
            return Status::success();
        }

        return diagnostic(Severity::Error, Status::failure(StatusCode::InvalidIndex, "pqueue checkpoint metadata is corrupt or missing; delete or repair queue files to continue"), "mount");
    }
    setRuntime(runtime_, layout, state);
    return Status::success();
}

ValidationResult FileStore::validateUnlocked(const ValidationOptions& options) {
    ValidationResult result;

    const auto fs = fileSystem();
    if (!fs) {
        addValidationError(result, options, makeIssue(ValidationIssueCode::InvalidConfig, "file system backend unavailable"));
        return result;
    }

    Status st = fs->mount(config_.basePath);
    if (!st.ok()) {
        addValidationError(result, options, makeIssue(ValidationIssueCode::InvalidConfig, st.message));
        return result;
    }

    Layout layout;
    if (!makeLayout(config_, layout)) {
        addValidationError(result, options, makeIssue(ValidationIssueCode::InvalidConfig, "invalid pqueue storage config"));
        return result;
    }

    if (!fileExists(*fs, kSpoolName)) {
        addValidationError(result, options, makeIssue(ValidationIssueCode::SpoolMissing, "pqueue spool file is missing"));
        return result;
    }

    std::uint64_t actualSpoolBytes = 0;
    st = fs->fileSize(kSpoolName, actualSpoolBytes);
    if (!st.ok()) {
        addValidationError(result, options, makeIssue(ValidationIssueCode::SpoolMissing, "pqueue spool file is missing"));
        return result;
    }
    if (actualSpoolBytes != layout.spoolBytes) {
        addValidationError(result, options, makeIssue(ValidationIssueCode::SpoolSizeMismatch, "pqueue spool size does not match configured storage layout"));
        return result;
    }

    bool foundCheckpoint = false;
    bool configMismatch = false;
    bool anyNonZeroCheckpoint = false;
    CheckpointRecord checkpoint;

    for (std::uint32_t slot = 0; slot < kCheckpointSlots; ++slot) {
        std::string bytes;
        st = fs->readAt(kSpoolName, checkpointOffset(slot), kCheckpointRecordBytes, bytes);
        if (!st.ok() || bytes.size() != kCheckpointRecordBytes) {
            anyNonZeroCheckpoint = true;
            addValidationError(result, options, makeIssue(ValidationIssueCode::MetadataCorrupt, "failed to read pqueue checkpoint slot"));
            if (result.stoppedEarly) {
                return result;
            }
            continue;
        }
        if (bytesAllZero(bytes)) {
            continue;
        }
        anyNonZeroCheckpoint = true;

        CheckpointRecord candidate;
        if (!parseCheckpointRecord(bytes, candidate) || candidate.magic != kFileStoreCheckpointMagic) {
            addValidationError(result, options, makeIssue(ValidationIssueCode::MetadataCorrupt, "pqueue checkpoint slot is not a valid checkpoint record"));
            if (result.stoppedEarly) {
                return result;
            }
            continue;
        }
        if (!validCheckpointShape(candidate)) {
            addValidationError(result, options, makeIssue(ValidationIssueCode::MetadataCorrupt, "pqueue checkpoint slot is corrupt or has unsupported format"));
            if (result.stoppedEarly) {
                return result;
            }
            continue;
        }
        if (!validCheckpointForConfig(candidate, layout.capacityRecords, layout.recordSizeBytes, layout.reservedBytes, layout.journalBytes)) {
            configMismatch = true;
            addValidationError(result, options, makeIssue(ValidationIssueCode::ConfigMismatch, "pqueue checkpoint config does not match current storage config"));
            if (result.stoppedEarly) {
                return result;
            }
            continue;
        }
        if (!foundCheckpoint || candidate.generation > checkpoint.generation) {
            foundCheckpoint = true;
            checkpoint = candidate;
        }
    }

    if (configMismatch && !foundCheckpoint) {
        return result;
    }
    if (!foundCheckpoint) {
        if (anyNonZeroCheckpoint) {
            addValidationError(result, options, makeIssue(ValidationIssueCode::MetadataCorrupt, "no usable pqueue checkpoint found"));
        } else {
            addValidationError(result, options, makeIssue(ValidationIssueCode::MetadataMissing, "pqueue checkpoint region is empty"));
        }
        return result;
    }

    FileStoreIndex replayed = fromCheckpointRecord(checkpoint);
    std::uint32_t journalUsedBytes = 0;
    std::uint32_t nextGeneration = checkpoint.generation + 1;
    for (std::uint32_t offset = 0; offset + kJournalEntryBytes <= layout.journalBytes; offset += kJournalEntryBytes) {
        std::string bytes;
        st = fs->readAt(kSpoolName, journalOffset(layout, offset), kJournalEntryBytes, bytes);
        if (!st.ok() || bytes.size() != kJournalEntryBytes) {
            addValidationError(result, options, makeIssue(ValidationIssueCode::JournalCorrupt, "failed to read pqueue journal entry"));
            return result;
        }
        if (bytesAllZero(bytes)) {
            break;
        }

        JournalEntry entry;
        if (!parseJournalEntry(bytes, entry) || !validJournalEntryShape(entry)) {
            if (regionHasNonZero(*fs, layout, offset + kJournalEntryBytes)) {
                addValidationError(result, options, makeIssue(ValidationIssueCode::JournalCorrupt, "pqueue journal has corrupt entry before non-empty trailing data"));
            }
            break;
        }

        if (entry.generation <= checkpoint.generation) {
            break;
        }
        if (entry.generation != nextGeneration) {
            addValidationError(result, options, makeIssue(ValidationIssueCode::JournalCorrupt, "pqueue journal generation is not contiguous after checkpoint"));
            break;
        }
        if (!applyJournalEntry(entry, replayed)) {
            addValidationError(result, options, makeIssue(ValidationIssueCode::JournalCorrupt, "pqueue journal entry cannot be applied to checkpoint state"));
            break;
        }
        journalUsedBytes = offset + kJournalEntryBytes;
        ++nextGeneration;
    }

    CheckpointRecord active = checkpoint;
    active.head = replayed.head;
    active.tail = replayed.tail;
    active.count = replayed.count;
    active.journalUsedBytes = journalUsedBytes;
    if (!ringStateMatches(active)) {
        addValidationError(result, options, makeIssue(ValidationIssueCode::InvalidRingState, "pqueue metadata ring state is invalid after journal replay"));
        return result;
    }

    if (!result.ok) {
        return result;
    }

    for (std::uint32_t sequence = replayed.head; sequence < replayed.tail; ++sequence) {
        const std::uint32_t slotIndex = sequence % layout.capacityRecords;
        std::string bytes;
        st = fs->readAt(kSpoolName, slotOffset(layout, sequence), layout.slotSizeBytes, bytes);
        if (!st.ok() || bytes.size() != layout.slotSizeBytes || bytes.size() < kRecordHeaderBytes) {
            addValidationError(result, options, makeSlotIssue(ValidationIssueCode::SlotReadFailed, "failed to read active pqueue spool slot", slotIndex, sequence));
            if (result.stoppedEarly) {
                return result;
            }
            continue;
        }

        RecordHeader header;
        if (!parseRecordHeader(bytes, header) ||
            header.magic != kFileStoreRecordMagic ||
            header.version != kFormatVersion ||
            header.headerBytes != kRecordHeaderBytes ||
            header.sequence != sequence ||
            header.recordBytes > layout.recordSizeBytes) {
            auto issue = makeSlotIssue(ValidationIssueCode::SlotHeaderInvalid, "active pqueue spool slot header is invalid", slotIndex, sequence);
            issue.actualSequence = header.sequence;
            issue.hasActualSequence = true;
            addValidationError(result, options, std::move(issue));
            if (result.stoppedEarly) {
                return result;
            }
            continue;
        }

        const std::string record = bytes.substr(kRecordHeaderBytes, header.recordBytes);
        if (header.crc != recordCrc(header, record)) {
            addValidationError(result, options, makeSlotIssue(ValidationIssueCode::SlotCrcMismatch, "active pqueue spool slot CRC mismatch", slotIndex, sequence));
            if (result.stoppedEarly) {
                return result;
            }
            continue;
        }

        ++result.checkedRecords;
    }

    return result;
}


Status FileStore::format() {
    const StorageBackend backend = resolvedBackend();
    Status st = rawMount(config_, fileSystem_, backend);
    if (!st.ok()) {
        return diagnostic(Severity::Error, st, "format");
    }

    Layout layout;
    if (!makeLayout(config_, layout)) {
        return diagnostic(Severity::Error, Status::failure(StatusCode::InvalidArgument, "invalid pqueue storage config: reservedBytes must fit at least one recordSizeBytes slot and journalBytes must fit at least one entry"), "format");
    }

    st = fileSystem_->resizeFile(kSpoolName, layout.spoolBytes);
    if (!st.ok()) {
        return diagnostic(Severity::Error, st, "format", kNoSequence, kSpoolName);
    }

    st = zeroFile(*fileSystem_, layout);
    if (!st.ok()) {
        return diagnostic(Severity::Error, st, "format", kNoSequence, kSpoolName);
    }

    constexpr std::uint32_t kInitialGeneration = 1;
    st = writeCheckpoint(*fileSystem_, layout, FileStoreIndex{}, kInitialGeneration);
    if (!st.ok()) {
        return diagnostic(Severity::Error, st, "format", kNoSequence, kSpoolName);
    }

    setRuntimeFresh(runtime_, layout, kInitialGeneration);
    return Status::success();
}

Status FileStore::rebuildMetadata() {
    const StorageBackend backend = resolvedBackend();
    Status st = rawMount(config_, fileSystem_, backend);
    if (!st.ok()) {
        return diagnostic(Severity::Error, st, "rebuildMetadata");
    }

    Layout layout;
    if (!makeLayout(config_, layout)) {
        return diagnostic(Severity::Error, Status::failure(StatusCode::InvalidArgument, "invalid pqueue storage config"), "rebuildMetadata");
    }

    std::uint64_t actualSize = 0;
    st = fileSystem_->fileSize(kSpoolName, actualSize);
    if (!st.ok() || actualSize != layout.spoolBytes) {
        return diagnostic(Severity::Error, Status::failure(StatusCode::ReadFailed, "pqueue spool missing or wrong size; use format to reinitialize"), "rebuildMetadata");
    }

    std::vector<std::uint32_t> candidates;
    for (std::uint32_t i = 0; i < layout.capacityRecords; ++i) {
        std::string bytes;
        st = fileSystem_->readAt(kSpoolName, layout.recordRegionOffset + static_cast<std::uint64_t>(i) * layout.slotSizeBytes, layout.slotSizeBytes, bytes);
        if (!st.ok() || bytes.size() < kRecordHeaderBytes) {
            continue;
        }
        RecordHeader header;
        if (!parseRecordHeader(bytes, header)) {
            continue;
        }
        if (header.magic != kFileStoreRecordMagic ||
            header.version != kFormatVersion ||
            header.headerBytes != kRecordHeaderBytes ||
            header.recordBytes > layout.recordSizeBytes ||
            header.sequence % layout.capacityRecords != i) {
            continue;
        }
        candidates.push_back(header.sequence);
    }

    FileStoreIndex rebuilt;
    if (!candidates.empty()) {
        const auto [minIt, maxIt] = std::minmax_element(candidates.begin(), candidates.end());
        const std::uint32_t minSeq = *minIt;
        const std::uint32_t maxSeq = *maxIt;
        if (maxSeq - minSeq + 1 != static_cast<std::uint32_t>(candidates.size())) {
            return diagnostic(Severity::Error, Status::failure(StatusCode::InvalidIndex, "gap in active slot sequence numbers; cannot safely rebuild metadata"), "rebuildMetadata");
        }
        rebuilt.head = minSeq;
        rebuilt.tail = maxSeq + 1;
        rebuilt.count = static_cast<std::uint32_t>(candidates.size());
    }

    constexpr std::uint32_t kChunkBytes = 256;
    const std::string zeros(kChunkBytes, '\0');
    const std::uint32_t metadataBytes = layout.checkpointBytes + layout.journalBytes;
    std::uint32_t offset = 0;
    while (offset < metadataBytes) {
        const std::uint32_t toWrite = std::min(kChunkBytes, metadataBytes - offset);
        st = fileSystem_->writeAt(kSpoolName, offset, zeros.substr(0, toWrite));
        if (!st.ok()) {
            return diagnostic(Severity::Error, st, "rebuildMetadata", kNoSequence, kSpoolName);
        }
        offset += toWrite;
    }

    constexpr std::uint32_t kRebuildGeneration = 1;
    st = writeCheckpoint(*fileSystem_, layout, rebuilt, kRebuildGeneration);
    if (!st.ok()) {
        return diagnostic(Severity::Error, st, "rebuildMetadata", kNoSequence, kSpoolName);
    }

    StoredState state;
    state.foundCheckpoint = true;
    state.checkpoint.generation = kRebuildGeneration;
    state.index = rebuilt;
    setRuntime(runtime_, layout, state);
    return Status::success();
}

Status FileStore::readIndex(FileStoreIndex& out) {
    Status st = mount();
    if (!st.ok()) {
        return st;
    }
    out = runtime_.index;
    return Status::success();
}

Status FileStore::readIndexFromDisk(FileStoreIndex& out) {
    Status st = mount();
    if (!st.ok()) {
        return st;
    }
    const Layout layout = layoutFromRuntime(runtime_);
    const StoredState state = loadStoredState(*fileSystem(), layout);
    if (state.readFailed) {
        return diagnostic(Severity::Error, state.readFailedStatus, "readIndexFromDisk");
    }
    if (state.configMismatch) {
        return diagnostic(Severity::Error, Status::failure(StatusCode::InvalidIndex, "pqueue storage config changed; delete or reformat queue files to continue"), "readIndexFromDisk");
    }
    if (!state.foundCheckpoint) {
        return diagnostic(Severity::Error, Status::failure(StatusCode::InvalidIndex, "pqueue checkpoint metadata is corrupt or missing"), "readIndexFromDisk");
    }
    setRuntime(runtime_, layout, state);
    out = runtime_.index;
    return Status::success();
}

Status FileStore::writeIndex(const FileStoreIndex& index) {
    Status st = mount();
    if (!st.ok()) {
        return st;
    }
    const Layout layout = layoutFromRuntime(runtime_);
    if (index.tail < index.head || index.count != index.tail - index.head || index.count > layout.capacityRecords) {
        return diagnostic(Severity::Error, Status::failure(StatusCode::InvalidIndex, "invalid queue index"), "writeIndex");
    }
    const auto fs = fileSystem();
    st = commitIndexTransition(*fs, layout, runtime_, index);
    if (!st.ok()) {
        return diagnostic(Severity::Error, st, "writeIndex", kNoSequence, kSpoolName);
    }
    return Status::success();
}

Status FileStore::writeRecord(std::uint32_t sequence, const std::string& record) {
    Status st = mount();
    if (!st.ok()) {
        return st;
    }
    const Layout layout = layoutFromRuntime(runtime_);
    if (record.size() > layout.recordSizeBytes) {
        return diagnostic(Severity::Warning, Status::failure(StatusCode::RecordTooLarge, "record exceeds configured pqueue slot size"), "writeRecord", sequence);
    }

    RecordHeader header;
    header.sequence = sequence;
    header.recordBytes = static_cast<std::uint32_t>(record.size());
    header.crc = recordCrc(header, record);

    std::string bytes = serializeRecordHeader(header);
    bytes.append(record);
    bytes.resize(layout.slotSizeBytes, '\0');

    st = fileSystem()->writeAt(kSpoolName, slotOffset(layout, sequence), bytes);
    if (!st.ok()) {
        return diagnostic(Severity::Error, st, "writeRecord", sequence, kSpoolName);
    }
    return Status::success();
}

Status FileStore::rewriteRecord(std::uint32_t sequence, const std::string& record) {
    Status st = writeRecord(sequence, record);
    if (!st.ok()) {
        return st;
    }
    const Layout layout = layoutFromRuntime(runtime_);
    const auto fs = fileSystem();
    st = commitRewrite(*fs, layout, runtime_, sequence);
    if (!st.ok()) {
        return diagnostic(Severity::Error, st, "rewriteRecord", sequence, kSpoolName);
    }
    return Status::success();
}

Status FileStore::readRecord(std::uint32_t sequence, std::string& out) {
    Status st = mount();
    if (!st.ok()) {
        return st;
    }
    const Layout layout = layoutFromRuntime(runtime_);

    std::string bytes;
    st = fileSystem()->readAt(kSpoolName, slotOffset(layout, sequence), layout.slotSizeBytes, bytes);
    if (!st.ok()) {
        return diagnostic(Severity::Error, st, "readRecord", sequence, kSpoolName);
    }
    if (bytes.size() != layout.slotSizeBytes || bytes.size() < kRecordHeaderBytes) {
        return diagnostic(Severity::Error, Status::failure(StatusCode::InvalidRecord, "spool slot is truncated"), "readRecord", sequence, kSpoolName);
    }

    RecordHeader header;
    if (!parseRecordHeader(bytes, header) ||
        header.magic != kFileStoreRecordMagic ||
        header.version != kFormatVersion ||
        header.headerBytes != kRecordHeaderBytes ||
        header.sequence != sequence ||
        header.recordBytes > layout.recordSizeBytes) {
        // TODO: expose an fsck/recovery path that can log and repair/drop corrupt front records.
        return diagnostic(Severity::Error, Status::failure(StatusCode::InvalidRecord, "spool slot header is invalid"), "readRecord", sequence, kSpoolName);
    }

    std::string record = bytes.substr(kRecordHeaderBytes, header.recordBytes);
    if (header.crc != recordCrc(header, record)) {
        // TODO: expose an fsck/recovery path that can log and repair/drop corrupt front records.
        return diagnostic(Severity::Error, Status::failure(StatusCode::CrcMismatch, "spool slot CRC mismatch"), "readRecord", sequence, kSpoolName);
    }

    out = std::move(record);
    return Status::success();
}

Status FileStore::removeRecord(std::uint32_t sequence) {
    Status st = mount();
    if (!st.ok()) {
        return st;
    }
    const Layout layout = layoutFromRuntime(runtime_);
    const std::string empty(layout.slotSizeBytes, '\0');
    st = fileSystem()->writeAt(kSpoolName, slotOffset(layout, sequence), empty);
    if (!st.ok()) {
        return diagnostic(Severity::Error, st, "removeRecord", sequence, kSpoolName);
    }
    return Status::success();
}

Status FileStore::tryAcquireLockFile(const std::string& name, const std::string& contents) {
    const StorageBackend backend = resolvedBackend();
    Status st = rawMount(config_, fileSystem_, backend);
    if (!st.ok()) {
        return diagnostic(Severity::Error, st, "tryAcquireLockFile");
    }
    return fileSystem_->tryAcquireLockFile(name, contents);
}

Status FileStore::releaseLockFile(const std::string& name, const std::string& expectedContents) {
    const StorageBackend backend = resolvedBackend();
    Status st = rawMount(config_, fileSystem_, backend);
    if (!st.ok()) {
        return diagnostic(Severity::Error, st, "releaseLockFile");
    }
    return fileSystem_->releaseLockFile(name, expectedContents);
}

Status FileStore::recoverStaleLockFile(const std::string& name, const std::string& currentContents) {
    const StorageBackend backend = resolvedBackend();
    Status st = rawMount(config_, fileSystem_, backend);
    if (!st.ok()) {
        return diagnostic(Severity::Error, st, "recoverStaleLockFile");
    }
    return fileSystem_->recoverStaleLockFile(name, currentContents);
}

bool FileStore::canEnqueue(std::size_t /*recordSize*/, std::uint32_t currentCount) const {
    const auto slotSize = static_cast<std::uint32_t>(storage_detail::kRecordHeaderBytes + config_.recordSizeBytes);
    const auto capacity = slotSize > 0 ? config_.reservedBytes / slotSize : 0u;
    return capacity > 0 && currentCount < capacity;
}

std::uint64_t FileStore::freeBytes() const {
    const auto fs = fileSystem();
    if (!fs || !fs->mount(config_.basePath).ok()) {
        return 0;
    }
    return fs->freeBytes();
}

} // namespace pqueue

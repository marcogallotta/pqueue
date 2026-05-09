#pragma once

#include "file_store.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace pqueue::storage_detail {

constexpr std::uint32_t kFileStoreCheckpointMagic = 0x5051434B; // PQCK
constexpr std::uint32_t kFileStoreRecordMagic = 0x50515243;     // PQRC
constexpr std::uint32_t kFileStoreJournalMagic = 0x50514A4E;    // PQJN

// On-disk pqueue storage format.
// 0 = unreleased/unstable development format.
// 1+ = public stable formats.
// Only exact-version reads are supported for now; future versions may add upgrade paths,
// but never automatic downgrades.
constexpr std::uint16_t kFormatVersion = 0;
constexpr std::size_t kMaxPathBytes = 160;

constexpr std::uint16_t kCheckpointSlots = 4;
constexpr std::uint16_t kCheckpointRecordBytes = 48;
constexpr std::uint16_t kJournalEntryBytes = 24;
constexpr std::uint16_t kRecordHeaderBytes = 20;

enum class JournalOp : std::uint32_t {
    Enqueue = 1,
    Pop = 2,
    RewriteFront = 3,
};

struct CheckpointRecord {
    std::uint32_t magic = kFileStoreCheckpointMagic;
    std::uint16_t version = kFormatVersion;
    std::uint16_t checkpointBytes = kCheckpointRecordBytes;
    std::uint32_t capacityRecords = 0;
    std::uint32_t recordSizeBytes = 0;
    std::uint32_t reservedBytes = 0;
    std::uint32_t journalBytes = 0;
    std::uint32_t head = 0;
    std::uint32_t tail = 0;
    std::uint32_t count = 0;
    std::uint32_t generation = 0;
    std::uint32_t journalUsedBytes = 0;
    std::uint32_t crc = 0;
};

struct JournalEntry {
    std::uint32_t magic = kFileStoreJournalMagic;
    std::uint16_t version = kFormatVersion;
    std::uint16_t entryBytes = kJournalEntryBytes;
    JournalOp op = JournalOp::Enqueue;
    std::uint32_t sequence = 0;
    std::uint32_t generation = 0;
    std::uint32_t crc = 0;
};

struct RecordHeader {
    std::uint32_t magic = kFileStoreRecordMagic;
    std::uint16_t version = kFormatVersion;
    std::uint16_t headerBytes = kRecordHeaderBytes;
    std::uint32_t sequence = 0;
    std::uint32_t recordBytes = 0;
    std::uint32_t crc = 0;
};

std::uint32_t crc32Update(std::uint32_t crc, const void* data, std::size_t len);
std::uint32_t checkpointCrc(const CheckpointRecord& record);
std::uint32_t journalCrc(const JournalEntry& entry);
std::uint32_t recordCrc(const RecordHeader& header, const std::string& record);

std::string serializeCheckpointRecord(const CheckpointRecord& record);
bool parseCheckpointRecord(const std::string& bytes, CheckpointRecord& out);
std::string serializeJournalEntry(const JournalEntry& entry);
bool parseJournalEntry(const std::string& bytes, JournalEntry& out);
std::string serializeRecordHeader(const RecordHeader& header);
bool parseRecordHeader(const std::string& bytes, RecordHeader& out);

bool validCheckpointShape(const CheckpointRecord& record);
bool validCheckpointForConfig(const CheckpointRecord& record, std::uint32_t capacityRecords, std::uint32_t recordSizeBytes, std::uint32_t reservedBytes, std::uint32_t journalBytes);
bool validJournalEntryShape(const JournalEntry& entry);
CheckpointRecord toCheckpointRecord(const FileStoreIndex& index, std::uint32_t generation, std::uint32_t capacityRecords, std::uint32_t recordSizeBytes, std::uint32_t reservedBytes, std::uint32_t journalBytes, std::uint32_t journalUsedBytes);
FileStoreIndex fromCheckpointRecord(const CheckpointRecord& record);

// Backwards-compatible aliases for older tests/helpers during pre-release churn.
using IndexRecord = CheckpointRecord;
std::uint32_t indexCrc(const IndexRecord& record);
bool validIndexShape(const IndexRecord& record);
bool validIndexForConfig(const IndexRecord& record, std::uint32_t capacityRecords, std::uint32_t recordSizeBytes, std::uint32_t reservedBytes);
IndexRecord toRecord(const FileStoreIndex& index, std::uint32_t generation, std::uint32_t capacityRecords, std::uint32_t recordSizeBytes, std::uint32_t reservedBytes);
FileStoreIndex fromRecord(const IndexRecord& record);

} // namespace pqueue::storage_detail

#include "storage_common.h"

#include <cstddef>

namespace pqueue::storage_detail {
namespace {

void writeLe16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFFU));
    out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
}

void writeLe32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFFU));
    out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<char>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<char>((value >> 24U) & 0xFFU));
}

std::uint16_t readLe16(const std::string& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint8_t>(bytes[offset]) |
        (static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[offset + 1])) << 8U));
}

std::uint32_t readLe32(const std::string& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset])) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 1])) << 8U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 2])) << 16U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 3])) << 24U);
}

std::string serializeCheckpointPrefix(const CheckpointRecord& record) {
    std::string out;
    out.reserve(kCheckpointRecordBytes);
    writeLe32(out, record.magic);
    writeLe16(out, record.version);
    writeLe16(out, record.checkpointBytes);
    writeLe32(out, record.capacityRecords);
    writeLe32(out, record.recordSizeBytes);
    writeLe32(out, record.reservedBytes);
    writeLe32(out, record.journalBytes);
    writeLe32(out, record.head);
    writeLe32(out, record.tail);
    writeLe32(out, record.count);
    writeLe32(out, record.generation);
    writeLe32(out, record.journalUsedBytes);
    writeLe32(out, 0); // reserved
    writeLe32(out, 0); // reserved
    writeLe32(out, 0); // reserved
    writeLe32(out, 0); // reserved
    return out;
}

std::string serializeJournalPrefix(const JournalEntry& entry) {
    std::string out;
    out.reserve(kJournalEntryBytes);
    writeLe32(out, entry.magic);
    writeLe16(out, entry.version);
    writeLe16(out, entry.entryBytes);
    writeLe32(out, static_cast<std::uint32_t>(entry.op));
    writeLe32(out, entry.sequence);
    writeLe32(out, entry.generation);
    writeLe32(out, 0); // reserved
    writeLe32(out, 0); // reserved
    return out;
}

std::string serializeRecordHeaderPrefix(const RecordHeader& header) {
    std::string out;
    out.reserve(kRecordHeaderBytes);
    writeLe32(out, header.magic);
    writeLe16(out, header.version);
    writeLe16(out, header.headerBytes);
    writeLe32(out, header.sequence);
    writeLe32(out, header.recordBytes);
    return out;
}

} // namespace

std::uint32_t crc32Update(std::uint32_t crc, const void* data, std::size_t len) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    crc = ~crc;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

std::uint32_t checkpointCrc(const CheckpointRecord& record) {
    const std::string prefix = serializeCheckpointPrefix(record);
    return crc32Update(0, prefix.data(), prefix.size());
}

std::uint32_t journalCrc(const JournalEntry& entry) {
    const std::string prefix = serializeJournalPrefix(entry);
    return crc32Update(0, prefix.data(), prefix.size());
}

std::uint32_t recordCrc(const RecordHeader& header, const std::string& record) {
    const std::string prefix = serializeRecordHeaderPrefix(header);
    std::uint32_t crc = crc32Update(0, prefix.data(), prefix.size());
    return crc32Update(crc, record.data(), record.size());
}

std::string serializeCheckpointRecord(const CheckpointRecord& record) {
    std::string out = serializeCheckpointPrefix(record);
    writeLe32(out, record.crc);
    return out;
}

bool parseCheckpointRecord(const std::string& bytes, CheckpointRecord& out) {
    if (bytes.size() != kCheckpointRecordBytes) {
        return false;
    }
    out.magic = readLe32(bytes, 0);
    out.version = readLe16(bytes, 4);
    out.checkpointBytes = readLe16(bytes, 6);
    out.capacityRecords = readLe32(bytes, 8);
    out.recordSizeBytes = readLe32(bytes, 12);
    out.reservedBytes = readLe32(bytes, 16);
    out.journalBytes = readLe32(bytes, 20);
    out.head = readLe32(bytes, 24);
    out.tail = readLe32(bytes, 28);
    out.count = readLe32(bytes, 32);
    out.generation = readLe32(bytes, 36);
    out.journalUsedBytes = readLe32(bytes, 40);
    // bytes 44–59: reserved
    out.crc = readLe32(bytes, 60);
    return true;
}

std::string serializeJournalEntry(const JournalEntry& entry) {
    std::string out = serializeJournalPrefix(entry);
    writeLe32(out, entry.crc);
    return out;
}

bool parseJournalEntry(const std::string& bytes, JournalEntry& out) {
    if (bytes.size() != kJournalEntryBytes) {
        return false;
    }
    out.magic = readLe32(bytes, 0);
    out.version = readLe16(bytes, 4);
    out.entryBytes = readLe16(bytes, 6);
    out.op = static_cast<JournalOp>(readLe32(bytes, 8));
    out.sequence = readLe32(bytes, 12);
    out.generation = readLe32(bytes, 16);
    // bytes 20–27: reserved
    out.crc = readLe32(bytes, 28);
    return true;
}

std::string serializeRecordHeader(const RecordHeader& header) {
    std::string out = serializeRecordHeaderPrefix(header);
    writeLe32(out, header.crc);
    return out;
}

bool parseRecordHeader(const std::string& bytes, RecordHeader& out) {
    if (bytes.size() < kRecordHeaderBytes) {
        return false;
    }
    out.magic = readLe32(bytes, 0);
    out.version = readLe16(bytes, 4);
    out.headerBytes = readLe16(bytes, 6);
    out.sequence = readLe32(bytes, 8);
    out.recordBytes = readLe32(bytes, 12);
    out.crc = readLe32(bytes, 16);
    return true;
}

bool validCheckpointShape(const CheckpointRecord& record) {
    return record.magic == kFileStoreCheckpointMagic &&
           record.version == kFormatVersion &&
           record.checkpointBytes == kCheckpointRecordBytes &&
           record.crc == checkpointCrc(record) &&
           record.capacityRecords > 0 &&
           record.recordSizeBytes > 0 &&
           record.journalBytes >= kJournalEntryBytes &&
           record.journalUsedBytes <= record.journalBytes &&
           record.journalUsedBytes % kJournalEntryBytes == 0 &&
           record.count <= record.capacityRecords &&
           record.tail >= record.head &&
           record.count == record.tail - record.head;
}

bool validCheckpointForConfig(const CheckpointRecord& record, std::uint32_t capacityRecords, std::uint32_t recordSizeBytes, std::uint32_t reservedBytes, std::uint32_t journalBytes) {
    return validCheckpointShape(record) &&
           record.capacityRecords == capacityRecords &&
           record.recordSizeBytes == recordSizeBytes &&
           record.reservedBytes == reservedBytes &&
           record.journalBytes == journalBytes;
}

bool validJournalEntryShape(const JournalEntry& entry) {
    return entry.magic == kFileStoreJournalMagic &&
           entry.version == kFormatVersion &&
           entry.entryBytes == kJournalEntryBytes &&
           entry.crc == journalCrc(entry) &&
           (entry.op == JournalOp::Enqueue || entry.op == JournalOp::Pop || entry.op == JournalOp::RewriteFront);
}

CheckpointRecord toCheckpointRecord(const FileStoreIndex& index, std::uint32_t generation, std::uint32_t capacityRecords, std::uint32_t recordSizeBytes, std::uint32_t reservedBytes, std::uint32_t journalBytes, std::uint32_t journalUsedBytes) {
    CheckpointRecord out;
    out.capacityRecords = capacityRecords;
    out.recordSizeBytes = recordSizeBytes;
    out.reservedBytes = reservedBytes;
    out.journalBytes = journalBytes;
    out.head = index.head;
    out.tail = index.tail;
    out.count = index.count;
    out.generation = generation;
    out.journalUsedBytes = journalUsedBytes;
    out.crc = checkpointCrc(out);
    return out;
}

FileStoreIndex fromCheckpointRecord(const CheckpointRecord& record) {
    FileStoreIndex out;
    out.head = record.head;
    out.tail = record.tail;
    out.count = record.count;
    return out;
}

std::uint32_t indexCrc(const IndexRecord& record) {
    return checkpointCrc(record);
}

bool validIndexShape(const IndexRecord& record) {
    return validCheckpointShape(record);
}

bool validIndexForConfig(const IndexRecord& record, std::uint32_t capacityRecords, std::uint32_t recordSizeBytes, std::uint32_t reservedBytes) {
    return validCheckpointForConfig(record, capacityRecords, recordSizeBytes, reservedBytes, record.journalBytes);
}

IndexRecord toRecord(const FileStoreIndex& index, std::uint32_t generation, std::uint32_t capacityRecords, std::uint32_t recordSizeBytes, std::uint32_t reservedBytes) {
    return toCheckpointRecord(index, generation, capacityRecords, recordSizeBytes, reservedBytes, 4096, 0);
}

FileStoreIndex fromRecord(const IndexRecord& record) {
    return fromCheckpointRecord(record);
}

} // namespace pqueue::storage_detail

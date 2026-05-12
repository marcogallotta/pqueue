#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace pqueue::append_log_detail {

// Magic numbers (byte values written to disk in order; LE storage)
constexpr std::uint32_t kSegmentMagic  = 0x47535150; // "PQSG"
constexpr std::uint32_t kEnqueueMagic  = 0x51455150; // "PQEQ"
constexpr std::uint32_t kPopMagic      = 0x45505150; // "PQPE"
constexpr std::uint32_t kRewriteMagic  = 0x45525150; // "PQRE"
constexpr std::uint32_t kFooterMagic           = 0x214B4F50; // "POK!"
constexpr std::uint32_t kCompactionMagic        = 0x4A435150; // "PQCJ"

constexpr std::uint16_t kFormatVersion = 0;

constexpr std::uint16_t kSegmentHeaderBytes           = 20;
constexpr std::uint16_t kCompactionJournalRecordBytes = 36;
constexpr std::uint16_t kEnqueueHeaderBytes  = 16; // fixed header only: magic(4)+version(2)+headerBytes(2)+sequence(4)+payloadBytes(4)
constexpr std::uint16_t kPopEventBytes       = 20;
constexpr std::uint16_t kEventTrailerBytes   = 8;  // crc (4) + footer (4)

// kEnqueueOverheadBytes: fixed cost added to every ENQUEUE (or REWRITE) event beyond payload
constexpr std::uint32_t kEnqueueOverheadBytes = kEnqueueHeaderBytes + kEventTrailerBytes;
// kPopEventBytes is the total size of a POP event (no payload)
static_assert(kPopEventBytes == 20, "pop event must be 20 bytes");

// Segment header (20 bytes, at offset 0 of each segment file)
struct SegmentHeader {
    std::uint32_t magic       = kSegmentMagic;
    std::uint16_t version     = kFormatVersion;
    std::uint16_t headerBytes = kSegmentHeaderBytes;
    std::uint32_t generation  = 0;
    std::uint32_t baseSequence = 0; // informational: first enqueue sequence expected
    std::uint32_t headerCrc   = 0;
};

// Fixed prefix shared by ENQUEUE and REWRITE events (payload follows immediately after)
struct EnqueueHeader {
    std::uint32_t magic        = kEnqueueMagic;
    std::uint16_t version      = kFormatVersion;
    std::uint16_t headerBytes  = kEnqueueHeaderBytes; // 16
    std::uint32_t sequence     = 0;
    std::uint32_t payloadBytes = 0;
};

// Complete POP event (fixed size)
struct PopEvent {
    std::uint32_t magic       = kPopMagic;
    std::uint16_t version     = kFormatVersion;
    std::uint16_t headerBytes = kPopEventBytes;
    std::uint32_t sequence    = 0;
    std::uint32_t eventCrc    = 0;
    std::uint32_t footer      = kFooterMagic;
};

// Compaction journal record (36 bytes); appended to pqueue-compact.bin on each committed compaction
struct CompactionJournalRecord {
    std::uint32_t magic       = kCompactionMagic;
    std::uint16_t version     = kFormatVersion;
    std::uint16_t headerBytes = kCompactionJournalRecordBytes;
    std::uint32_t commitSeq   = 0;
    std::uint32_t oldStart    = 0; // first generation of replaced range (inclusive)
    std::uint32_t oldEnd      = 0; // last generation of replaced range (inclusive)
    std::uint32_t newStart    = 0; // first generation of compacted range (inclusive)
    std::uint32_t newEnd      = 0; // last generation of compacted range (inclusive)
    std::uint32_t crc         = 0; // CRC32 over all preceding fields
    std::uint32_t footer      = kFooterMagic;
};

std::uint32_t crc32(std::uint32_t crc, const void* data, std::size_t len);

std::uint32_t enqueueEventCrc(const EnqueueHeader& h, const std::string& payload);

std::string serializeSegmentHeader(std::uint32_t generation, std::uint32_t baseSequence);
bool parseSegmentHeader(const std::string& bytes, SegmentHeader& out);

std::string serializeEnqueueEvent(std::uint32_t sequence, const std::string& payload);
std::string serializeRewriteEvent(std::uint32_t sequence, const std::string& payload);
std::string serializePopEvent(std::uint32_t sequence);

bool parseEnqueueHeader(const std::string& bytes, EnqueueHeader& out);
bool parsePopEvent(const std::string& bytes, PopEvent& out);

std::string serializeCompactionJournalRecord(const CompactionJournalRecord& r);
bool parseCompactionJournalRecord(const std::string& bytes, CompactionJournalRecord& out);

} // namespace pqueue::append_log_detail

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pqueue::append_log_detail {

// Magic numbers (byte values written to disk in order; LE storage)
constexpr std::uint32_t kSegmentMagic  = 0x47535150; // "PQSG"
constexpr std::uint32_t kEnqueueMagic  = 0x51455150; // "PQEQ"
constexpr std::uint32_t kPopMagic      = 0x45505150; // "PQPE"
constexpr std::uint32_t kRewriteMagic  = 0x45525150; // "PQRE"
constexpr std::uint32_t kFooterMagic   = 0x214B4F50; // "POK!"
constexpr std::uint32_t kManifestMagic = 0x464D5150; // "PQMF"

constexpr std::uint16_t kFormatVersion   = 1;
constexpr std::uint16_t kManifestVersion = 1;

// Manifest binary layout sizes
// Fixed overhead: magic(4)+version(2)+headerBytes(2)+epoch(4)+nextGeneration(4)+
//                 rangeCount(2)+tailGeneration(4)+crc(4)+footer(4) = 30 bytes
constexpr std::uint16_t kManifestFixedBytes = 30;
// Hard cap on manifest range count. Sized so slot-jam is impossible before flash
// bytes run out (assuming >=512 B sealed segments). Idle compaction targets <=4
// ranges to stay within the 64-byte LittleFS inline threshold; the hard cap is
// only approached under pathological fragmentation.
constexpr std::uint16_t kManifestMaxRanges = 1024;
// Safety ceiling on total active generation count (sum of all range spans + tail).
// Validated before applyManifestToRam expands gen-by-gen to prevent RAM explosion
// from a corrupt-but-CRC-valid manifest with a pathologically wide span.
// Derived from deployment assumptions: maxTotalBytes=512 KiB and tolerated min
// sealed segment ~512 B -> max segments = 512K/512 = 1024 = kManifestMaxRanges;
// add 1 for the tail. Larger deployments should raise both caps together.
constexpr std::uint32_t kManifestMaxTotalGenerations = kManifestMaxRanges + 1;

constexpr std::uint16_t kSegmentHeaderBytes  = 20;
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
    std::uint32_t startSeq    = 0; // informational: first enqueue sequence expected
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

std::uint32_t crc32(std::uint32_t crc, const void* data, std::size_t len);

std::uint32_t enqueueEventCrc(const EnqueueHeader& h, const std::string& payload);

std::string serializeSegmentHeader(std::uint32_t generation, std::uint32_t startSeq);
bool parseSegmentHeader(const std::string& bytes, SegmentHeader& out);

std::string serializeEnqueueEvent(std::uint32_t sequence, const std::string& payload);
std::string serializeRewriteEvent(std::uint32_t sequence, const std::string& payload);
std::string serializePopEvent(std::uint32_t sequence);

bool parseEnqueueHeader(const std::string& bytes, EnqueueHeader& out);
bool parsePopEvent(const std::string& bytes, PopEvent& out);

// Manifest binary format

struct ManifestRange {
    std::uint32_t startGen = 0;
    std::uint32_t endGen   = 0;
};

struct ManifestData {
    std::uint32_t epoch          = 0;
    std::uint32_t nextGeneration = 1;
    std::vector<ManifestRange> ranges; // up to kManifestMaxRanges entries; idle compaction targets <=4
    std::uint32_t tailGeneration = 0;
};

void serialiseManifest(const ManifestData& manifest, std::vector<std::uint8_t>& out);
bool parseManifest(const std::uint8_t* data, std::size_t size, ManifestData& out);

} // namespace pqueue::append_log_detail

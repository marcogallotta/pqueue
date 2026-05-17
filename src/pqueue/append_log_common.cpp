#include "append_log_common.h"

#include <array>
#include <cassert>

namespace pqueue::append_log_detail {

std::uint32_t crc32(std::uint32_t crc, const void* data, std::size_t len) {
    static constexpr auto kTable = []() {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int j = 0; j < 8; ++j)
                c = (c >> 1) ^ (c & 1u ? 0xEDB88320u : 0u);
            t[i] = c;
        }
        return t;
    }();
    const auto* p = static_cast<const std::uint8_t*>(data);
    crc = ~crc;
    while (len--)
        crc = kTable[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

namespace {

void writeU32(std::string& buf, std::uint32_t v) {
    buf.push_back(static_cast<char>((v      ) & 0xFF));
    buf.push_back(static_cast<char>((v >>  8) & 0xFF));
    buf.push_back(static_cast<char>((v >> 16) & 0xFF));
    buf.push_back(static_cast<char>((v >> 24) & 0xFF));
}

void writeU16(std::string& buf, std::uint16_t v) {
    buf.push_back(static_cast<char>((v     ) & 0xFF));
    buf.push_back(static_cast<char>((v >> 8) & 0xFF));
}

bool readU32(const std::string& buf, std::size_t offset, std::uint32_t& out) {
    if (offset + 4 > buf.size()) return false;
    out = static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[offset])      )
        | static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[offset + 1])) <<  8
        | static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[offset + 2])) << 16
        | static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[offset + 3])) << 24;
    return true;
}

bool readU16(const std::string& buf, std::size_t offset, std::uint16_t& out) {
    if (offset + 2 > buf.size()) return false;
    out = static_cast<std::uint16_t>(static_cast<std::uint8_t>(buf[offset])     )
        | static_cast<std::uint16_t>(static_cast<std::uint8_t>(buf[offset + 1])) << 8;
    return true;
}

std::uint32_t segmentHeaderCrc(const SegmentHeader& h) {
    std::string buf;
    buf.reserve(16);
    writeU32(buf, h.magic);
    writeU16(buf, h.version);
    writeU16(buf, h.headerBytes);
    writeU32(buf, h.generation);
    writeU32(buf, h.startSeq);
    return crc32(0, buf.data(), buf.size());
}

std::uint32_t popEventCrc(const PopEvent& e) {
    std::string buf;
    buf.reserve(12);
    writeU32(buf, e.magic);
    writeU16(buf, e.version);
    writeU16(buf, e.headerBytes);
    writeU32(buf, e.sequence);
    return crc32(0, buf.data(), buf.size());
}

} // namespace

std::uint32_t enqueueEventCrc(const EnqueueHeader& h, const std::string& payload) {
    std::string buf;
    buf.reserve(16 + payload.size());
    writeU32(buf, h.magic);
    writeU16(buf, h.version);
    writeU16(buf, h.headerBytes);
    writeU32(buf, h.sequence);
    writeU32(buf, h.payloadBytes);
    buf.append(payload);
    return crc32(0, buf.data(), buf.size());
}

std::string serializeSegmentHeader(std::uint32_t generation, std::uint32_t startSeq) {
    SegmentHeader h;
    h.generation = generation;
    h.startSeq = startSeq;
    h.headerCrc = segmentHeaderCrc(h);
    std::string buf;
    buf.reserve(kSegmentHeaderBytes);
    writeU32(buf, h.magic);
    writeU16(buf, h.version);
    writeU16(buf, h.headerBytes);
    writeU32(buf, h.generation);
    writeU32(buf, h.startSeq);
    writeU32(buf, h.headerCrc);
    return buf;
}

bool parseSegmentHeader(const std::string& bytes, SegmentHeader& out) {
    if (bytes.size() < kSegmentHeaderBytes) return false;
    std::size_t o = 0;
    if (!readU32(bytes, o, out.magic))        { return false; } o += 4;
    if (!readU16(bytes, o, out.version))      { return false; } o += 2;
    if (!readU16(bytes, o, out.headerBytes))  { return false; } o += 2;
    if (!readU32(bytes, o, out.generation))   { return false; } o += 4;
    if (!readU32(bytes, o, out.startSeq))     { return false; } o += 4;
    if (!readU32(bytes, o, out.headerCrc))    { return false; }
    return out.magic == kSegmentMagic
        && out.version == kFormatVersion
        && out.headerBytes == kSegmentHeaderBytes
        && out.headerCrc == segmentHeaderCrc(out);
}

std::string serializeEnqueueEvent(std::uint32_t sequence, const std::string& payload) {
    EnqueueHeader h;
    h.sequence = sequence;
    h.payloadBytes = static_cast<std::uint32_t>(payload.size());
    const std::uint32_t crc = enqueueEventCrc(h, payload);
    std::string buf;
    buf.reserve(kEnqueueHeaderBytes + payload.size() + kEventTrailerBytes);
    writeU32(buf, h.magic);
    writeU16(buf, h.version);
    writeU16(buf, h.headerBytes);
    writeU32(buf, h.sequence);
    writeU32(buf, h.payloadBytes);
    buf.append(payload);
    writeU32(buf, crc);
    writeU32(buf, kFooterMagic);
    return buf;
}

std::string serializeRewriteEvent(std::uint32_t sequence, const std::string& payload) {
    EnqueueHeader h;
    h.magic = kRewriteMagic;
    h.sequence = sequence;
    h.payloadBytes = static_cast<std::uint32_t>(payload.size());
    const std::uint32_t crc = enqueueEventCrc(h, payload);
    std::string buf;
    buf.reserve(kEnqueueHeaderBytes + payload.size() + kEventTrailerBytes);
    writeU32(buf, h.magic);
    writeU16(buf, h.version);
    writeU16(buf, h.headerBytes);
    writeU32(buf, h.sequence);
    writeU32(buf, h.payloadBytes);
    buf.append(payload);
    writeU32(buf, crc);
    writeU32(buf, kFooterMagic);
    return buf;
}

std::string serializePopEvent(std::uint32_t sequence) {
    PopEvent p;
    p.sequence = sequence;
    p.eventCrc = popEventCrc(p);
    std::string buf;
    buf.reserve(kPopEventBytes);
    writeU32(buf, p.magic);
    writeU16(buf, p.version);
    writeU16(buf, p.headerBytes);
    writeU32(buf, p.sequence);
    writeU32(buf, p.eventCrc);
    writeU32(buf, p.footer);
    return buf;
}

bool parseEnqueueHeader(const std::string& bytes, EnqueueHeader& out) {
    if (bytes.size() < kEnqueueHeaderBytes) return false;
    std::size_t o = 0;
    if (!readU32(bytes, o, out.magic))        { return false; } o += 4;
    if (!readU16(bytes, o, out.version))      { return false; } o += 2;
    if (!readU16(bytes, o, out.headerBytes))  { return false; } o += 2;
    if (!readU32(bytes, o, out.sequence))     { return false; } o += 4;
    if (!readU32(bytes, o, out.payloadBytes)) { return false; }
    return (out.magic == kEnqueueMagic || out.magic == kRewriteMagic)
        && out.version == kFormatVersion
        && out.headerBytes == kEnqueueHeaderBytes;
}

bool parsePopEvent(const std::string& bytes, PopEvent& out) {
    if (bytes.size() < kPopEventBytes) return false;
    std::size_t o = 0;
    if (!readU32(bytes, o, out.magic))       { return false; } o += 4;
    if (!readU16(bytes, o, out.version))     { return false; } o += 2;
    if (!readU16(bytes, o, out.headerBytes)) { return false; } o += 2;
    if (!readU32(bytes, o, out.sequence))    { return false; } o += 4;
    if (!readU32(bytes, o, out.eventCrc))    { return false; } o += 4;
    if (!readU32(bytes, o, out.footer))      { return false; }
    return out.magic == kPopMagic
        && out.version == kFormatVersion
        && out.headerBytes == kPopEventBytes
        && out.footer == kFooterMagic
        && out.eventCrc == popEventCrc(out);
}

} // namespace pqueue::append_log_detail

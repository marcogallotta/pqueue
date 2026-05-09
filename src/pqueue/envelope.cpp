#include "envelope.h"

#include <cstddef>
#include <limits>

namespace pqueue::envelope {
namespace {

constexpr std::uint32_t kOutboxEnvelopeMagic = 0x50514f42; // PQOB
constexpr std::uint8_t kOutboxEnvelopeVersion = 1;
constexpr std::size_t kOutboxEnvelopeHeaderSize = sizeof(std::uint32_t) + sizeof(std::uint8_t) + sizeof(std::uint8_t) + sizeof(std::uint32_t);
constexpr std::size_t kChecksumSize = sizeof(std::uint32_t);

const std::uint32_t* crc32Table() {
    // TODO: use a precomputed PROGMEM table for very small Arduino targets.
    static std::uint32_t table[256];
    static bool initialized = false;
    if (!initialized) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value >> 1) ^ (0xEDB88320U & (0U - (value & 1U)));
            }
            table[i] = value;
        }
        initialized = true;
    }
    return table;
}

std::uint32_t crc32(const void* bytes, std::size_t byteCount) {
    const auto* cursor = static_cast<const unsigned char*>(bytes);
    std::uint32_t checksum = 0xffffffffU;
    for (std::size_t i = 0; i < byteCount; ++i) {
        checksum = (checksum >> 8) ^ crc32Table()[(checksum ^ cursor[i]) & 0xffU];
    }
    return ~checksum;
}

void appendByte(std::string& encoded, std::uint8_t value) {
    encoded.push_back(static_cast<char>(value));
}

void appendLittleEndian32(std::string& encoded, std::uint32_t value) {
    for (int byte = 0; byte < 4; ++byte) {
        encoded.push_back(static_cast<char>((value >> (8 * byte)) & 0xffU));
    }
}

bool readByte(const std::string& encoded, std::size_t& offset, std::uint8_t& value) {
    if (offset + sizeof(std::uint8_t) > encoded.size()) {
        return false;
    }
    value = static_cast<std::uint8_t>(static_cast<unsigned char>(encoded[offset]));
    offset += sizeof(std::uint8_t);
    return true;
}

bool readLittleEndian32(const std::string& encoded, std::size_t& offset, std::uint32_t& value) {
    if (offset + sizeof(std::uint32_t) > encoded.size()) {
        return false;
    }
    value = 0;
    for (int byte = 0; byte < 4; ++byte) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(encoded[offset + byte])) << (8 * byte);
    }
    offset += sizeof(std::uint32_t);
    return true;
}

bool hasValidChecksum(const std::string& encoded) {
    if (encoded.size() < kOutboxEnvelopeHeaderSize + kChecksumSize) {
        return false;
    }

    const std::size_t checksumOffset = encoded.size() - kChecksumSize;
    std::size_t offset = checksumOffset;
    std::uint32_t storedChecksum = 0;
    if (!readLittleEndian32(encoded, offset, storedChecksum) || offset != encoded.size()) {
        return false;
    }

    return storedChecksum == crc32(encoded.data(), checksumOffset);
}

} // namespace

bool encodeEnvelope(std::uint8_t attempts, const std::string& payload, std::string& encoded) {
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    encoded.clear();
    encoded.reserve(kOutboxEnvelopeHeaderSize + payload.size() + kChecksumSize);
    appendLittleEndian32(encoded, kOutboxEnvelopeMagic);
    appendByte(encoded, kOutboxEnvelopeVersion);
    appendByte(encoded, attempts);
    appendLittleEndian32(encoded, static_cast<std::uint32_t>(payload.size()));
    encoded.append(payload.data(), payload.size());
    appendLittleEndian32(encoded, crc32(encoded.data(), encoded.size()));
    return true;
}

bool decodeEnvelope(const std::string& encoded, DecodedEnvelope& decoded) {
    if (!hasValidChecksum(encoded)) {
        return false;
    }

    const std::size_t payloadEnd = encoded.size() - kChecksumSize;
    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint8_t version = 0;
    std::uint8_t attempts = 0;
    std::uint32_t payloadLength = 0;

    if (!readLittleEndian32(encoded, offset, magic) ||
        !readByte(encoded, offset, version) ||
        !readByte(encoded, offset, attempts) ||
        !readLittleEndian32(encoded, offset, payloadLength)) {
        return false;
    }

    if (magic != kOutboxEnvelopeMagic || version != kOutboxEnvelopeVersion) {
        return false;
    }
    if (offset + payloadLength != payloadEnd) {
        return false;
    }

    decoded.attempts = attempts;
    decoded.payload.assign(encoded.data() + offset, payloadLength);
    return true;
}

} // namespace pqueue::envelope

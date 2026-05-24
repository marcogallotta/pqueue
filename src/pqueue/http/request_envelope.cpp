#include "request_envelope.h"

#include <cstddef>
#include <limits>

namespace pqueue::http {
namespace {

constexpr std::uint32_t kHttpRequestEnvelopeMagic = 0x50514852; // PQHR
constexpr std::uint8_t kHttpRequestEnvelopeVersion = 2;
constexpr std::uint8_t kHttpRequestEnvelopeV1 = 1;
// v2 header size used only for reserve() hint: magic(4)+version(1)+method(1)+encoding(1)+path_len(2)+body_len(4)=13
constexpr std::size_t kHttpRequestEnvelopeHeaderSize =
    sizeof(std::uint32_t) + sizeof(std::uint8_t) + sizeof(std::uint8_t) + sizeof(std::uint8_t) + sizeof(std::uint16_t) + sizeof(std::uint32_t);

void appendByte(std::string& encoded, std::uint8_t value) {
    encoded.push_back(static_cast<char>(value));
}

void appendLittleEndian16(std::string& encoded, std::uint16_t value) {
    for (int byte = 0; byte < 2; ++byte) {
        encoded.push_back(static_cast<char>((value >> (8 * byte)) & 0xffU));
    }
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

bool readLittleEndian16(const std::string& encoded, std::size_t& offset, std::uint16_t& value) {
    if (offset + sizeof(std::uint16_t) > encoded.size()) {
        return false;
    }
    value = 0;
    for (int byte = 0; byte < 2; ++byte) {
        value |= static_cast<std::uint16_t>(static_cast<unsigned char>(encoded[offset + byte])) << (8 * byte);
    }
    offset += sizeof(std::uint16_t);
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

bool methodIsSupported(std::uint8_t method) {
    return method == static_cast<std::uint8_t>(Method::Post);
}

bool encodingIsSupported(std::uint8_t encoding) {
    return encoding == static_cast<std::uint8_t>(BodyEncoding::Raw) ||
           encoding == static_cast<std::uint8_t>(BodyEncoding::Compact);
}

} // namespace

bool encodeRequestEnvelope(const RequestEnvelope& request, std::string& encoded) {
    if (request.method != Method::Post) {
        return false;
    }
    const auto encoding = static_cast<std::uint8_t>(request.encoding);
    if (!encodingIsSupported(encoding)) {
        return false;
    }
    if (request.path.size() > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    if (request.body.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    encoded.clear();
    encoded.reserve(kHttpRequestEnvelopeHeaderSize + request.path.size() + request.body.size());
    appendLittleEndian32(encoded, kHttpRequestEnvelopeMagic);
    appendByte(encoded, kHttpRequestEnvelopeVersion);
    appendByte(encoded, static_cast<std::uint8_t>(request.method));
    appendByte(encoded, encoding);
    appendLittleEndian16(encoded, static_cast<std::uint16_t>(request.path.size()));
    appendLittleEndian32(encoded, static_cast<std::uint32_t>(request.body.size()));
    encoded.append(request.path.data(), request.path.size());
    encoded.append(request.body.data(), request.body.size());
    return true;
}

bool decodeRequestEnvelope(const std::string& encoded, RequestEnvelope& request) {
    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint8_t version = 0;
    std::uint8_t method = 0;

    if (!readLittleEndian32(encoded, offset, magic) ||
        !readByte(encoded, offset, version) ||
        !readByte(encoded, offset, method)) {
        return false;
    }

    if (magic != kHttpRequestEnvelopeMagic || !methodIsSupported(method)) {
        return false;
    }

    std::uint8_t encodingByte = static_cast<std::uint8_t>(BodyEncoding::Raw);

    if (version == kHttpRequestEnvelopeV1) {
        // no encoding byte; Raw is the default
    } else if (version == kHttpRequestEnvelopeVersion) {
        if (!readByte(encoded, offset, encodingByte)) {
            return false;
        }
        if (!encodingIsSupported(encodingByte)) {
            return false;
        }
    } else {
        return false;
    }

    std::uint16_t pathLength = 0;
    std::uint32_t bodyLength = 0;

    if (!readLittleEndian16(encoded, offset, pathLength) ||
        !readLittleEndian32(encoded, offset, bodyLength)) {
        return false;
    }

    request.method = static_cast<Method>(method);
    request.encoding = static_cast<BodyEncoding>(encodingByte);

    if (pathLength > encoded.size() - offset) {
        return false;
    }
    request.path.assign(encoded.data() + offset, pathLength);
    offset += pathLength;

    if (bodyLength > encoded.size() - offset) {
        return false;
    }
    request.body.assign(encoded.data() + offset, bodyLength);
    offset += bodyLength;

    return offset == encoded.size();
}

} // namespace pqueue::http

#pragma once

#include <cstdint>
#include <string>

namespace pqueue::http {

enum class Method : std::uint8_t {
    Post = 1,
};

enum class BodyEncoding : std::uint8_t {
    Raw     = 0,
    Compact = 1,
};

struct RequestEnvelope {
    Method method = Method::Post;
    BodyEncoding encoding = BodyEncoding::Raw;
    std::string path;
    std::string body;
};

bool encodeRequestEnvelope(const RequestEnvelope& request, std::string& encoded);
bool decodeRequestEnvelope(const std::string& encoded, RequestEnvelope& request);

} // namespace pqueue::http

#pragma once

#include <cstdint>
#include <string>

namespace pqueue::http {

enum class Method : std::uint8_t {
    Post = 1,
};

struct RequestEnvelope {
    Method method = Method::Post;
    std::string path;
    std::string body;
};

bool encodeRequestEnvelope(const RequestEnvelope& request, std::string& encoded);
bool decodeRequestEnvelope(const std::string& encoded, RequestEnvelope& request);

} // namespace pqueue::http

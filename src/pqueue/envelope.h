#pragma once

#include <cstdint>
#include <string>

namespace pqueue::envelope {

struct DecodedEnvelope {
    std::uint8_t attempts = 0;
    std::string payload;
};

bool encodeEnvelope(std::uint8_t attempts, const std::string& payload, std::string& encoded);
bool decodeEnvelope(const std::string& encoded, DecodedEnvelope& decoded);

} // namespace pqueue::envelope

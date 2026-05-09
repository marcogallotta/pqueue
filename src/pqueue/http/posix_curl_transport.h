#pragma once

#include "outbox.h"

#include <string>

namespace pqueue::http {

struct PosixCurlTransportConfig {
    TransportConfig common;
    std::string caBundlePath;
};

class PosixCurlTransport final : public Transport {
public:
    explicit PosixCurlTransport(PosixCurlTransportConfig config = PosixCurlTransportConfig{});

    Response post(
        const char* url,
        const Header* headers,
        std::size_t headerCount,
        const std::uint8_t* body,
        std::size_t bodySize
    ) override;

private:
    PosixCurlTransportConfig config_;
};

} // namespace pqueue::http

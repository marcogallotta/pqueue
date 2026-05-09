#pragma once

#include "outbox.h"

#ifdef ARDUINO

#include <cstdint>
#include <string>

namespace fs {
class FS;
}

class WiFiClientSecure;

namespace pqueue::http {

using NetworkReadyCallback = bool (*)(void* context, std::uint32_t timeoutMs);

struct Esp32ArduinoTransportConfig {
    TransportConfig common;

    const char* caCertPath = nullptr;
    fs::FS* caCertFileSystem = nullptr;

    void* networkReadyContext = nullptr;
    NetworkReadyCallback networkReady = nullptr;
};

class Esp32ArduinoTransport final : public Transport {
public:
    explicit Esp32ArduinoTransport(Esp32ArduinoTransportConfig config = Esp32ArduinoTransportConfig{});

    Response post(
        const char* url,
        const Header* headers,
        std::size_t headerCount,
        const std::uint8_t* body,
        std::size_t bodySize
    ) override;

private:
    bool isNetworkReady() const;
    TransportError mapHttpClientError(int code) const;
    bool configureTlsClient(WiFiClientSecure& client, std::string& caCertStorage) const;

    Esp32ArduinoTransportConfig config_;
};

} // namespace pqueue::http

#endif // ARDUINO

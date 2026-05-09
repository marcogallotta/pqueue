#include "esp32_arduino_transport.h"

#ifdef ARDUINO

#include <Arduino.h>
#include <FS.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <string>

namespace pqueue::http {
namespace {

void emitTransportEvent(
    const TransportConfig& config,
    Severity severity,
    Status status,
    const char* operation,
    const char* url,
    std::size_t headerCount,
    std::size_t bodySize,
    int httpStatus = 0
) {
    Event event;
    event.kind = EventKind::Diagnostic;
    event.severity = severity;
    event.status = status;
    event.component = "Esp32ArduinoTransport";
    event.operation = operation;
    event.method = "POST";
    event.path = url == nullptr ? "" : url;
    event.bodyBytes = static_cast<std::uint32_t>(bodySize);
    event.headerCount = static_cast<std::uint32_t>(headerCount);
    event.timeoutMs = config.timeoutMs;
    event.httpStatus = httpStatus;
    config.events.emit(event);
}

std::string readFileToString(fs::FS* fileSystem, const char* path) {
    if (fileSystem == nullptr || path == nullptr || path[0] == '\0') {
        return {};
    }

    File file = fileSystem->open(path, "r");
    if (!file) {
        return {};
    }

    std::string contents;
    contents.reserve(static_cast<std::size_t>(file.size()));
    while (file.available()) {
        contents.push_back(static_cast<char>(file.read()));
    }
    file.close();
    return contents;
}

} // namespace

Esp32ArduinoTransport::Esp32ArduinoTransport(Esp32ArduinoTransportConfig config)
    : config_(config) {}

Response Esp32ArduinoTransport::post(
    const char* url,
    const Header* headers,
    std::size_t headerCount,
    const std::uint8_t* body,
    std::size_t bodySize
) {
    if (url == nullptr) {
        emitTransportEvent(
            config_.common,
            Severity::Error,
            Status::failure(StatusCode::InvalidArgument, "HTTP POST URL was null"),
            "post_invalid_url",
            nullptr,
            headerCount,
            bodySize);
        return {kNoStatusCode, TransportError::Unknown};
    }

    emitTransportEvent(
        config_.common,
        Severity::Debug,
        Status::success(),
        config_.caCertPath == nullptr ? "post_start_no_ca_cert_path" : "post_start_with_ca_cert_path",
        url,
        headerCount,
        bodySize);

    if (!isNetworkReady()) {
        emitTransportEvent(
            config_.common,
            Severity::Info,
            Status::failure(StatusCode::BackendUnavailable, "network was not ready before HTTP POST"),
            "network_not_ready",
            url,
            headerCount,
            bodySize);
        return {kNoStatusCode, TransportError::Network};
    }

    WiFiClientSecure client;
    std::string caCertStorage;
    if (!configureTlsClient(client, caCertStorage)) {
        emitTransportEvent(
            config_.common,
            Severity::Error,
            Status::failure(StatusCode::SendFailed, "failed to configure TLS client"),
            "configure_tls",
            url,
            headerCount,
            bodySize);
        return {kNoStatusCode, TransportError::Tls};
    }

    HTTPClient http;
    if (!http.begin(client, url)) {
        emitTransportEvent(
            config_.common,
            Severity::Error,
            Status::failure(StatusCode::SendFailed, "HTTPClient begin failed"),
            "http_begin",
            url,
            headerCount,
            bodySize);
        return {kNoStatusCode, TransportError::Unknown};
    }

    http.setTimeout(static_cast<std::uint16_t>(config_.common.timeoutMs));
    if (config_.common.userAgent != nullptr) {
        http.setUserAgent(config_.common.userAgent);
    }
    http.setFollowRedirects(config_.common.followRedirects ? HTTPC_STRICT_FOLLOW_REDIRECTS : HTTPC_DISABLE_FOLLOW_REDIRECTS);

    for (std::size_t i = 0; i < headerCount; ++i) {
        if (headers[i].name == nullptr || headers[i].value == nullptr) {
            continue;
        }
        http.addHeader(headers[i].name, headers[i].value);
    }

    emitTransportEvent(
        config_.common,
        Severity::Debug,
        Status::success(),
        config_.common.allowInsecureTls ? "http_post_start_insecure_tls" : "http_post_start_verify_tls",
        url,
        headerCount,
        bodySize);

    int code = http.POST(const_cast<std::uint8_t*>(body), bodySize);
    Response response;
    if (code > 0) {
        response.statusCode = code;
        response.error = TransportError::None;
        response.body = http.getString().c_str();
    } else {
        response.statusCode = kNoStatusCode;
        response.error = mapHttpClientError(code);
    }

    emitTransportEvent(
        config_.common,
        code > 0 ? Severity::Debug : Severity::Info,
        code > 0
            ? Status::success()
            : Status::failure(StatusCode::SendFailed, "HTTPClient POST returned an error", code),
        "http_post_complete",
        url,
        headerCount,
        bodySize,
        response.statusCode);

    http.end();
    return response;
}

bool Esp32ArduinoTransport::isNetworkReady() const {
    if (config_.networkReady != nullptr) {
        return config_.networkReady(config_.networkReadyContext, config_.common.timeoutMs);
    }
    return WiFi.status() == WL_CONNECTED;
}

TransportError Esp32ArduinoTransport::mapHttpClientError(int code) const {
    switch (code) {
        case HTTPC_ERROR_READ_TIMEOUT:
            return TransportError::Timeout;
        case HTTPC_ERROR_CONNECTION_REFUSED:
        case HTTPC_ERROR_CONNECTION_LOST:
        case HTTPC_ERROR_NOT_CONNECTED:
            return TransportError::Network;
        default:
            return TransportError::Unknown;
    }
}

bool Esp32ArduinoTransport::configureTlsClient(WiFiClientSecure& client, std::string& caCertStorage) const {
    if (config_.common.allowInsecureTls) {
        client.setInsecure();
        return true;
    }

    caCertStorage = readFileToString(config_.caCertFileSystem, config_.caCertPath);
    if (caCertStorage.empty()) {
        return false;
    }

    client.setCACert(caCertStorage.c_str());
    return true;
}

} // namespace pqueue::http

#endif // ARDUINO

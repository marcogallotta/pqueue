#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "pqueue/outbox.h"
#include "pqueue/http/request_envelope.h"


namespace pqueue::http {

constexpr int kNoStatusCode = 0;

struct Header {
    const char* name = nullptr;
    const char* value = nullptr;
};

enum class TransportError {
    None,
    Timeout,
    Network,
    Tls,
    Dns,
    Unknown,
};

struct Response {
    Response() = default;
    Response(int statusCode, TransportError error, std::string body = {}, std::uint32_t retryAfterMs = 0)
        : statusCode(statusCode), error(error), body(body), retryAfterMs(retryAfterMs) {}

    int statusCode = kNoStatusCode;
    TransportError error = TransportError::Unknown;
    std::string body;
    std::uint32_t retryAfterMs = 0; // populated by transport when server sends Retry-After
};

struct TransportConfig {
    std::uint32_t timeoutMs = 15000;
    const char* userAgent = "pqueue-http/1.0";
    bool followRedirects = true;
    bool allowInsecureTls = false;
    EventOptions events;
};

using PostCallback = Response (*)(
    void* context,
    const char* url,
    const Header* headers,
    std::size_t headerCount,
    const std::uint8_t* body,
    std::size_t bodySize
);

class Transport {
public:
    virtual ~Transport() = default;

    virtual Response post(
        const char* url,
        const Header* headers,
        std::size_t headerCount,
        const std::uint8_t* body,
        std::size_t bodySize
    ) = 0;
};

class CallbackTransport final : public Transport {
public:
    CallbackTransport(PostCallback post, void* context);

    Response post(
        const char* url,
        const Header* headers,
        std::size_t headerCount,
        const std::uint8_t* body,
        std::size_t bodySize
    ) override;

private:
    PostCallback post_ = nullptr;
    void* context_ = nullptr;
};

using ClassifyResponseCallback = SendDecision (*)(void* context, const Response& response);
using ResponseCallback = void (*)(void* context, const RequestEnvelope& request, const Response& response);

enum class DropReason {
    DecodeFailed,
    ServerRejected,
};

using DropCallback = void (*)(
    void* context,
    const RequestEnvelope* request,
    DropReason reason,
    const Response* response
);

struct Config {
    pqueue::Config queue;
    pqueue::OutboxConfig outbox;
    pqueue::FullQueuePolicy fullQueuePolicy = pqueue::FullQueuePolicy::DropOldest;

    const char* baseUrl = "";
    const Header* headers = nullptr;
    std::size_t headerCount = 0;

    void* classifyContext = nullptr;
    ClassifyResponseCallback classify = nullptr;

    void* responseContext = nullptr;
    ResponseCallback onResponse = nullptr;

    void* dropContext = nullptr;
    DropCallback onDrop = nullptr;
};

class Outbox {
public:
    Outbox(
        Config httpConfig,
        Transport& transport,
        ClockCallback clock,
        void* clockContext
    );

    pqueue::SubmitResult submitPost(const std::string& path, const std::string& body);
    pqueue::DrainResult drain();
    pqueue::DrainResult drainUpTo(std::uint16_t maxDrainAttempts);
    pqueue::CompactIdleResult compactIdle(std::size_t maxSteps);
    ValidationResult validate(const ValidationOptions& options = ValidationOptions{});
    Stats stats();

private:
    static pqueue::Config resolveQueueConfig(const Config& config);
    static SendResult sendStoredRequest(void* context, const std::string& encodedRequest, const RetryState& retry);
    SendResult sendStoredRequest(const std::string& encodedRequest, const RetryState& retry);
    void notifyResponse(const RequestEnvelope& request, const Response& response) const;
    void notifyDrop(const RequestEnvelope* request, DropReason reason, const Response* response) const;
    void emitDiagnostic(Severity severity, Status status, const char* operation, const RequestEnvelope* request = nullptr, const Response* response = nullptr, std::uint8_t attempt = 0) const;
    SendDecision classifyResponse(const Response& response) const;
    std::string buildUrl(const std::string& path) const;

    Config httpConfig_;
    Transport& transport_;
    pqueue::Outbox outbox_;
};

SendDecision defaultClassifyResponse(const Response& response);

} // namespace pqueue::http

#include "outbox.h"

#include "request_envelope.h"

#include <cstdint>

namespace pqueue::http {
namespace {

bool isSuccessfulStatus(int statusCode) {
    return statusCode >= 200 && statusCode < 300;
}


bool validateHttpRequestPayload(void*, const std::string& payload, ValidationIssue& issue) {
    RequestEnvelope request;
    if (decodeRequestEnvelope(payload, request)) {
        return true;
    }

    issue.code = ValidationIssueCode::HttpRequestEnvelopeInvalid;
    issue.message = "stored HTTP request envelope could not be decoded";
    return false;
}

bool isRetryableStatus(int statusCode) {
    // Simple v1 policy: retry all server-side failures, plus explicit client-side throttling/timeouts.
    // TODO: respect Retry-After for 429 and 503.
    // TODO: add exponential backoff with jitter for repeated 5xx/transport failures.
    // TODO: consider special handling/alerts for 501/505/506/508/510 server capability/config errors.
    // TODO: consider app-configurable handling for 409 and oversized payloads such as 413.
    return statusCode == 408 ||
           statusCode == 429 ||
           (statusCode >= 500 && statusCode < 600);
}

} // namespace

CallbackTransport::CallbackTransport(PostCallback post, void* context)
    : post_(post), context_(context) {}

Response CallbackTransport::post(
    const char* url,
    const Header* headers,
    std::size_t headerCount,
    const std::uint8_t* body,
    std::size_t bodySize
) {
    if (post_ == nullptr) {
        return {kNoStatusCode, TransportError::Unknown};
    }
    return post_(context_, url, headers, headerCount, body, bodySize);
}

pqueue::Config Outbox::resolveQueueConfig(const Config& config) {
    pqueue::Config q = config.queue;
    q.fullQueuePolicy = config.fullQueuePolicy;
    return q;
}

Outbox::Outbox(
    Config httpConfig,
    Transport& transport,
    ClockCallback clock,
    void* clockContext
) : httpConfig_(httpConfig),
    transport_(transport),
    outbox_(resolveQueueConfig(httpConfig), httpConfig.outbox, sendStoredRequest, this, clock, clockContext) {}

pqueue::SubmitResult Outbox::submitPost(const std::string& path, const std::string& body) {
    RequestEnvelope request;
    request.method = Method::Post;
    request.path = path;
    request.body = body;

    std::string encoded;
    if (!encodeRequestEnvelope(request, encoded)) {
        const Status status = Status::failure(
            StatusCode::EncodeFailed,
            "failed to encode HTTP request envelope");
        emitDiagnostic(
            Severity::Error,
            status,
            "submitPost",
            &request);
        return {pqueue::SubmitStatus::SendError, status};
    }

    emitDiagnostic(Severity::Debug, Status::success(), "submitPost", &request);
    return outbox_.submit(encoded);
}

pqueue::DrainResult Outbox::drain() {
    return outbox_.drain();
}

pqueue::DrainResult Outbox::drainUpTo(std::uint16_t maxDrainAttempts) {
    return outbox_.drainUpTo(maxDrainAttempts);
}

pqueue::CompactIdleResult Outbox::compactIdle(std::size_t maxSteps) {
    return outbox_.compactIdle(maxSteps);
}

ValidationResult Outbox::validate(const ValidationOptions& options) {
    return outbox_.validatePayloads(validateHttpRequestPayload, nullptr, options);
}

Stats Outbox::stats() {
    return outbox_.stats();
}

SendResult Outbox::sendStoredRequest(void* context, const std::string& encodedRequest, const RetryState& retry) {
    return static_cast<Outbox*>(context)->sendStoredRequest(encodedRequest, retry);
}

SendResult Outbox::sendStoredRequest(const std::string& encodedRequest, const RetryState& retry) {
    RequestEnvelope request;
    if (!decodeRequestEnvelope(encodedRequest, request) || request.method != Method::Post) {
        emitDiagnostic(
            Severity::Error,
            Status::failure(StatusCode::DecodeFailed, "failed to decode HTTP request envelope"),
            "sendStoredRequest",
            nullptr,
            nullptr,
            retry.attempts);
        notifyDrop(nullptr, DropReason::DecodeFailed, nullptr);
        return {SendDecision::Drop};
    }

    const std::string url = buildUrl(request.path);
    emitDiagnostic(
        Severity::Debug,
        Status::success(),
        "transport_post_start",
        &request,
        nullptr,
        retry.attempts);
    const Response response = transport_.post(
        url.c_str(),
        httpConfig_.headers,
        httpConfig_.headerCount,
        reinterpret_cast<const std::uint8_t*>(request.body.data()),
        request.body.size()
    );
    notifyResponse(request, response);
    emitDiagnostic(
        response.error == TransportError::None ? Severity::Debug : Severity::Warning,
        response.error == TransportError::None
            ? Status::success()
            : Status::failure(StatusCode::SendFailed, "HTTP transport returned an error", static_cast<int>(response.error)),
        "transport_post_complete",
        &request,
        &response,
        retry.attempts);

    const SendDecision decision = classifyResponse(response);
    if (decision == SendDecision::Drop) {
        notifyDrop(&request, DropReason::ServerRejected, &response);
    }

    return {decision};
}

void Outbox::notifyResponse(const RequestEnvelope& request, const Response& response) const {
    if (httpConfig_.onResponse != nullptr) {
        httpConfig_.onResponse(httpConfig_.responseContext, request, response);
    }
}

void Outbox::notifyDrop(const RequestEnvelope* request, DropReason reason, const Response* response) const {
    if (httpConfig_.onDrop != nullptr) {
        httpConfig_.onDrop(httpConfig_.dropContext, request, reason, response);
    }
}

void Outbox::emitDiagnostic(
    Severity severity,
    Status status,
    const char* operation,
    const RequestEnvelope* request,
    const Response* response,
    std::uint8_t attempt
) const {
    Event event;
    event.kind = EventKind::Diagnostic;
    event.severity = severity;
    event.status = status;
    event.component = "HttpOutbox";
    event.operation = operation;
    event.path = request == nullptr ? "" : request->path.c_str();
    event.method = request == nullptr ? "" : "POST";
    event.bodyBytes = request == nullptr ? 0U : static_cast<std::uint32_t>(request->body.size());
    event.httpStatus = response == nullptr ? 0 : response->statusCode;
    event.attempt = attempt;
    httpConfig_.outbox.events.emit(event);
}

SendDecision Outbox::classifyResponse(const Response& response) const {
    if (httpConfig_.classify != nullptr) {
        return httpConfig_.classify(httpConfig_.classifyContext, response);
    }
    return defaultClassifyResponse(response);
}

std::string Outbox::buildUrl(const std::string& path) const {
    const std::string baseUrl = httpConfig_.baseUrl == nullptr ? std::string{} : std::string{httpConfig_.baseUrl};
    if (baseUrl.empty()) {
        return path;
    }
    if (path.empty()) {
        return baseUrl;
    }

    const bool baseEndsWithSlash = baseUrl.back() == '/';
    const bool pathStartsWithSlash = path.front() == '/';
    if (baseEndsWithSlash && pathStartsWithSlash) {
        return baseUrl + path.substr(1);
    }
    if (!baseEndsWithSlash && !pathStartsWithSlash) {
        return baseUrl + '/' + path;
    }
    return baseUrl + path;
}

SendDecision defaultClassifyResponse(const Response& response) {
    if (response.error != TransportError::None) {
        return SendDecision::RetryLater;
    }
    if (isSuccessfulStatus(response.statusCode)) {
        return SendDecision::Sent;
    }
    if (isRetryableStatus(response.statusCode)) {
        return SendDecision::RetryLater;
    }

    return SendDecision::Drop;
}

} // namespace pqueue::http

#pragma once

#include <cstdint>
#include <string>

#include "events.h"
#include "queue.h"
#include "status.h"
#include "types.h"

namespace pqueue {
namespace http { class Outbox; }

enum class SendDecision {
    Sent,
    RetryLater,
    Drop,
};

struct RetryState {
    std::uint8_t attempts = 0;
};

struct SendResult {
    SendDecision decision = SendDecision::Drop;
};

// The payload string is treated as opaque bytes, not text. It may contain NULs.
using SendCallback = SendResult (*)(void* context, const std::string& payload, const RetryState& retry);
using RawSendCallback = SendResult (*)(void* context, Span payload, const RetryState& retry);

// Must return monotonic milliseconds. Do not use wall/NTP time here.
// ESP32 callers should use esp_timer_get_time() / 1000.
using ClockCallback = std::uint64_t (*)(void* context);
using OutboxPayloadValidator = bool (*)(void* context, const std::string& payload, ValidationIssue& issue);

// Persistent-only v1 outbox config.
// Retry attempt count is persisted in the envelope; retry cooldown timing is RAM-only.
struct OutboxConfig {
    // Retryable sends are retried indefinitely. attempts is retained only as a saturated diagnostic counter.
    std::uint16_t maxDrainAttemptsPerSecond = 5;
    std::uint32_t retryDelayMs = 10000;
    // Proven-corrupt front records may be skipped so one bad slot does not brick the outbox.
    // 0 disables automatic corrupt-record dropping; the default stops after a small cluster.
    std::uint16_t maxCorruptDropsPerLifetime = 3;
    EventOptions events;
    // TODO: make CRC configurable once the envelope has a published compatibility policy.
    // TODO: consider burst drain/backoff strategies after the strict FIFO v1 settles.
};

enum class SubmitStatus {
    Sent,
    Queued,
    Dropped,
    QueueFull,
    SendError,
};

struct SubmitResult {
    SubmitStatus status = SubmitStatus::SendError;
    Status detail = Status::failure(StatusCode::SendFailed, "send failed");
};

struct DrainResult {
    std::uint16_t attempts = 0;
    std::uint16_t sent = 0;
    std::uint16_t dropped = 0;
    std::uint16_t corruptDropped = 0;
    // Sum of raw pqueue record bytes for records successfully removed from the queue.
    // If a corrupt front record was dropped but the raw record was unreadable, the
    // record count is incremented but 0 bytes are added here.
    std::uint32_t removedQueuedBytes = 0;
    bool rateLimited = false;
    bool notDue = false;
    bool queueError = false;
    bool sendError = false;
    Status detail = Status::success();
};

// Generic store-and-forward lifecycle over Queue.
// This class is transport-agnostic: no HTTP, JSON, API keys, or app-specific logging.
// TODO: add an advanced constructor for dependency-injected Queue/storage in tests or custom backends.
class Outbox {
public:
    Outbox(
        Config queueConfig,
        OutboxConfig config,
        SendCallback send,
        void* sendContext,
        ClockCallback clock,
        void* clockContext
    );
    Outbox(
        Config queueConfig,
        OutboxConfig config,
        RawSendCallback send,
        void* sendContext,
        ClockCallback clock,
        void* clockContext
    );

    SubmitResult submit(Span payload);
    SubmitResult submit(const std::string& payload);
    DrainResult drain();
    DrainResult drainUpTo(std::uint16_t maxDrainAttempts);
    CompactIdleResult compactIdle(std::size_t maxSteps);
    ValidationResult validate(const ValidationOptions& options = ValidationOptions{});
    Stats stats();

private:
    friend class http::Outbox;

    ValidationResult validatePayloads(
        OutboxPayloadValidator payloadValidator,
        void* payloadValidatorContext,
        const ValidationOptions& options = ValidationOptions{}
    );

    SubmitResult enqueueRecord(const std::string& payload, std::uint8_t attempts);
    DrainResult drainOne(bool enforceRateLimit);
    void setFrontCooldown(std::uint64_t nextAttemptMs);
    void clearFrontCooldown();
    void emit(Event event) const;
    void emitDiagnostic(Severity severity, Status status, const char* operation) const;
    void emitRequestEvent(
        EventKind kind,
        Severity severity,
        Status status,
        const char* operation,
        std::uint8_t attempt = 0,
        std::uint32_t remainingMs = 0,
        std::uint32_t queueCount = 0
    );
    bool frontIsCoolingDown(std::uint64_t nowMs) const;
    bool drainRateAllows(std::uint64_t nowMs) const;
    void recordDrainAttempt(std::uint64_t nowMs);
    std::uint32_t drainRateRemainingMs(std::uint64_t nowMs) const;
    bool canDropCorruptFrontRecord() const;
    DrainResult dropCorruptFrontRecord(Status corruptStatus, std::uint32_t recordBytes = 0);
    std::uint16_t maxDrainAttemptsPerSecond() const;

    SendResult dispatchSend(const std::string& payload, const RetryState& retry);

    Queue queue_;
    OutboxConfig config_;
    SendCallback send_ = nullptr;
    void* sendContext_ = nullptr;
    RawSendCallback rawSend_ = nullptr;
    void* rawSendContext_ = nullptr;
    ClockCallback clock_ = nullptr;
    void* clockContext_ = nullptr;
    std::uint64_t drainWindowStartMs_ = 0;
    std::uint16_t drainAttemptsInWindow_ = 0;
    bool hasDrainWindow_ = false;
    std::uint64_t frontNextAttemptMs_ = 0;
    bool hasFrontCooldown_ = false;
    std::uint16_t corruptDropsThisLifetime_ = 0;
};

} // namespace pqueue

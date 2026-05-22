#include "outbox.h"

#include <limits>
#include <utility>

#include "envelope.h"

namespace pqueue {
namespace {

SubmitResult submitResult(SubmitStatus submitStatus, Status detail = Status::success()) {
    return {submitStatus, detail};
}

void addOutboxValidationError(ValidationResult& result, const ValidationOptions& options, ValidationIssue issue) {
    result.ok = false;
    if (result.errors.size() < options.maxErrors) {
        result.errors.push_back(std::move(issue));
    } else {
        result.stoppedEarly = true;
    }
}

ValidationIssue makeOutboxIssue(ValidationIssueCode code, const char* message, std::uint32_t sequence) {
    ValidationIssue issue;
    issue.code = code;
    issue.message = message;
    issue.expectedSequence = sequence;
    issue.hasExpectedSequence = true;
    return issue;
}

struct OutboxValidationContext {
    ValidationResult* result = nullptr;
    const ValidationOptions* options = nullptr;
    OutboxPayloadValidator payloadValidator = nullptr;
    void* payloadValidatorContext = nullptr;
};

bool isCorruptFrontRecordStatus(StatusCode code) {
    return code == StatusCode::InvalidRecord || code == StatusCode::CrcMismatch;
}

bool validateOutboxRecord(void* rawContext, const std::string& record, std::uint32_t sequence, std::uint32_t) {
    auto* context = static_cast<OutboxValidationContext*>(rawContext);

    envelope::DecodedEnvelope decoded;
    if (!envelope::decodeEnvelope(record, decoded)) {
        addOutboxValidationError(
            *context->result,
            *context->options,
            makeOutboxIssue(ValidationIssueCode::OutboxEnvelopeInvalid, "stored outbox envelope could not be decoded", sequence));
        return !context->result->stoppedEarly;
    }

    if (context->payloadValidator != nullptr) {
        ValidationIssue issue;
        if (!context->payloadValidator(context->payloadValidatorContext, decoded.payload, issue)) {
            if (!issue.hasExpectedSequence) {
                issue.expectedSequence = sequence;
                issue.hasExpectedSequence = true;
            }
            addOutboxValidationError(*context->result, *context->options, std::move(issue));
            return !context->result->stoppedEarly;
        }
    }

    return true;
}

} // namespace

Outbox::Outbox(
    Config queueConfig,
    OutboxConfig config,
    SendCallback send,
    void* sendContext,
    ClockCallback clock,
    void* clockContext
) : queue_(queueConfig),
    config_(config),
    send_(send),
    sendContext_(sendContext),
    clock_(clock),
    clockContext_(clockContext) {}

void Outbox::emit(Event event) const {
    config_.events.emit(event);
}

void Outbox::emitDiagnostic(Severity severity, Status status, const char* operation) const {
    emit(Event{
        EventKind::Diagnostic,
        severity,
        status,
        "Outbox",
        operation,
    });
}

void Outbox::emitRequestEvent(
    EventKind kind,
    Severity severity,
    Status status,
    const char* operation,
    std::uint8_t attempt,
    std::uint32_t remainingMs,
    std::uint32_t queueCount
) {
    Event event;
    event.kind = kind;
    event.severity = severity;
    event.status = status;
    event.component = "Outbox";
    event.operation = operation;
    event.attempt = attempt;
    event.queueCount = queueCount;
    event.remainingMs = remainingMs;
    emit(event);
}

SubmitResult Outbox::submit(const std::string& payload) {
    if (send_ == nullptr || clock_ == nullptr) {
        const Status st = Status::failure(StatusCode::SendFailed, "outbox is not configured for sending");
        emitDiagnostic(Severity::Error, st, "submit");
        return submitResult(SubmitStatus::SendError, st);
    }

    const StatsResult queueStats = queue_.statsResult();
    if (queueStats.status.ok() && queueStats.stats.count > 0) {
        return enqueueRecord(payload, 0);
    }
    if (!queueStats.status.ok()) {
        emitDiagnostic(Severity::Warning, queueStats.status, "submit_queue_stats_failed_live_send_fallback");
    }

    const SendResult result = send_(sendContext_, payload, RetryState{});
    switch (result.decision) {
        case SendDecision::Sent:
            emitRequestEvent(EventKind::RequestSent, Severity::Debug, Status::success(), "submit", 0, 0, 0);
            return submitResult(SubmitStatus::Sent);
        case SendDecision::Drop: {
            const Status st = Status::failure(StatusCode::Dropped, "request was dropped by send policy");
            emitRequestEvent(EventKind::RequestDropped, Severity::Warning, st, "submit", 0, 0, 0);
            return submitResult(SubmitStatus::Dropped, st);
        }
        case SendDecision::RetryLater: {
            const auto queued = enqueueRecord(payload, 1);
            if (queued.status == SubmitStatus::Queued) {
                setFrontCooldown(clock_(clockContext_) + config_.retryDelayMs);
                emitRequestEvent(EventKind::RequestRetried, Severity::Info, Status::success(), "submit", 1, config_.retryDelayMs, 1);
            }
            return queued;
        }
    }

    const Status st = Status::failure(StatusCode::SendFailed, "unknown send decision");
    emitDiagnostic(Severity::Error, st, "submit");
    return submitResult(SubmitStatus::SendError, st);
}

DrainResult Outbox::drain() {
    return drainOne(true);
}

DrainResult Outbox::drainUpTo(std::uint16_t maxDrainAttempts) {
    DrainResult total;
    if (maxDrainAttempts == 0) {
        maxDrainAttempts = 1;
    }

    for (std::uint16_t i = 0; i < maxDrainAttempts; ++i) {
        const DrainResult current = drainOne(true);
        total.attempts += current.attempts;
        total.sent += current.sent;
        total.dropped += current.dropped;
        total.corruptDropped += current.corruptDropped;
        total.rateLimited = total.rateLimited || current.rateLimited;
        total.notDue = total.notDue || current.notDue;
        total.queueError = total.queueError || current.queueError;
        total.sendError = total.sendError || current.sendError;
        if (!current.detail.ok()) {
            total.detail = current.detail;
        }

        const bool madeProgress = current.sent != 0 || current.dropped != 0 || current.corruptDropped != 0;
        const bool shouldStop = !madeProgress || current.notDue || current.rateLimited || current.queueError || current.sendError;
        if (shouldStop) {
            break;
        }
    }

    return total;
}

DrainResult Outbox::drainOne(bool enforceRateLimit) {
    DrainResult result;
    if (send_ == nullptr || clock_ == nullptr) {
        result.sendError = true;
        result.detail = Status::failure(StatusCode::SendFailed, "outbox is not configured for sending");
        emitDiagnostic(Severity::Error, result.detail, "drain");
        return result;
    }

    const std::uint64_t nowMs = clock_(clockContext_);
    std::string record;
    Status st = queue_.peek(record);
    if (!st.ok()) {
        if (st.code == StatusCode::QueueEmpty) {
            clearFrontCooldown();
            return result;
        }
        if (isCorruptFrontRecordStatus(st.code)) {
            return dropCorruptFrontRecord(st);
        }
        result.queueError = true;
        result.detail = st;
        emitDiagnostic(Severity::Error, st, "drain");
        return result;
    }

    envelope::DecodedEnvelope decoded;
    if (!envelope::decodeEnvelope(record, decoded)) {
        return dropCorruptFrontRecord(
            Status::failure(StatusCode::DecodeFailed, "stored outbox envelope could not be decoded"));
    }

    if (frontIsCoolingDown(nowMs)) {
        result.notDue = true;
        emitRequestEvent(
            EventKind::Diagnostic,
            Severity::Debug,
            Status::failure(StatusCode::SendFailed, "front request retry cooldown not due"),
            "drain",
            decoded.attempts,
            static_cast<std::uint32_t>(frontNextAttemptMs_ - nowMs));
        return result;
    }

    if (enforceRateLimit && !drainRateAllows(nowMs)) {
        result.rateLimited = true;
        emitRequestEvent(
            EventKind::Diagnostic,
            Severity::Debug,
            Status::failure(StatusCode::SendFailed, "drain rate limit not due"),
            "drain",
            decoded.attempts,
            drainRateRemainingMs(nowMs));
        return result;
    }
    recordDrainAttempt(nowMs);

    result.attempts += 1;
    emitRequestEvent(
        EventKind::Diagnostic,
        Severity::Debug,
        Status::success(),
        "drain_send_start",
        decoded.attempts,
        0);
    const SendResult sendResult = send_(sendContext_, decoded.payload, RetryState{decoded.attempts});
    switch (sendResult.decision) {
        case SendDecision::Sent:
            st = queue_.pop();
            if (!st.ok()) {
                result.queueError = true;
                result.detail = st;
                emitDiagnostic(Severity::Error, st, "drain");
                return result;
            }
            clearFrontCooldown();
            result.sent += 1;
            emitRequestEvent(EventKind::RequestSent, Severity::Debug, Status::success(), "drain", decoded.attempts, 0);
            return result;

        case SendDecision::Drop:
            st = queue_.pop();
            if (!st.ok()) {
                result.queueError = true;
                result.detail = st;
                emitDiagnostic(Severity::Error, st, "drain");
                return result;
            }
            clearFrontCooldown();
            result.dropped += 1;
            emitRequestEvent(
                EventKind::RequestDropped,
                Severity::Warning,
                Status::failure(StatusCode::Dropped, "request was dropped by send policy"),
                "drain",
                decoded.attempts,
                0);
            return result;

        case SendDecision::RetryLater: {
            const std::uint8_t nextAttempts = decoded.attempts == std::numeric_limits<std::uint8_t>::max()
                ? decoded.attempts
                : static_cast<std::uint8_t>(decoded.attempts + 1);

            if (!envelope::encodeEnvelope(nextAttempts, decoded.payload, record)) {
                result.queueError = true;
                result.detail = Status::failure(StatusCode::EncodeFailed, "failed to encode retry envelope");
                emitDiagnostic(Severity::Error, result.detail, "drain");
                return result;
            }
            st = queue_.rewriteFront(record);
            if (!st.ok()) {
                result.queueError = true;
                result.detail = st;
                emitDiagnostic(Severity::Error, st, "drain");
                return result;
            }
            setFrontCooldown(nowMs + config_.retryDelayMs);
            emitRequestEvent(EventKind::RequestRetried, Severity::Info, Status::success(), "drain", nextAttempts, config_.retryDelayMs);
            return result;
        }
    }

    result.sendError = true;
    result.detail = Status::failure(StatusCode::SendFailed, "unknown send decision");
    emitDiagnostic(Severity::Error, result.detail, "drain");
    return result;
}

CompactIdleResult Outbox::compactIdle(std::size_t maxSteps) {
    return queue_.compactIdle(maxSteps);
}

ValidationResult Outbox::validate(const ValidationOptions& options) {
    return validatePayloads(nullptr, nullptr, options);
}

ValidationResult Outbox::validatePayloads(
    OutboxPayloadValidator payloadValidator,
    void* payloadValidatorContext,
    const ValidationOptions& options
) {
    ValidationResult result = queue_.validate(options);
    if (!result.ok || result.stoppedEarly || result.errors.size() >= options.maxErrors) {
        return result;
    }

    OutboxValidationContext context;
    context.result = &result;
    context.options = &options;
    context.payloadValidator = payloadValidator;
    context.payloadValidatorContext = payloadValidatorContext;

    const Status st = queue_.visitRecords(validateOutboxRecord, &context);
    if (!st.ok() && result.errors.size() < options.maxErrors) {
        addOutboxValidationError(
            result,
            options,
            makeOutboxIssue(ValidationIssueCode::QueueLoadFailed, st.message, 0));
    }

    return result;
}

Stats Outbox::stats() {
    return queue_.stats();
}

SubmitResult Outbox::enqueueRecord(const std::string& payload, std::uint8_t attempts) {
    std::string record;
    if (!envelope::encodeEnvelope(attempts, payload, record)) {
        const Status st = Status::failure(StatusCode::EncodeFailed, "failed to encode outbox envelope");
        emitDiagnostic(Severity::Error, st, "enqueueRecord");
        return submitResult(SubmitStatus::SendError, st);
    }
    Status st = queue_.enqueue(record);
    if (st.ok()) {
        return submitResult(SubmitStatus::Queued);
    }
    if (st.code == StatusCode::QueueFull) {
        return submitResult(SubmitStatus::QueueFull, st);
    }
    return submitResult(SubmitStatus::SendError, st);
}

void Outbox::setFrontCooldown(std::uint64_t nextAttemptMs) {
    frontNextAttemptMs_ = nextAttemptMs;
    hasFrontCooldown_ = true;
}

void Outbox::clearFrontCooldown() {
    frontNextAttemptMs_ = 0;
    hasFrontCooldown_ = false;
}

bool Outbox::frontIsCoolingDown(std::uint64_t nowMs) const {
    return hasFrontCooldown_ && frontNextAttemptMs_ > nowMs;
}

bool Outbox::drainRateAllows(std::uint64_t nowMs) const {
    if (!hasDrainWindow_) {
        return true;
    }
    if (nowMs - drainWindowStartMs_ >= 1000U) {
        return true;
    }
    return drainAttemptsInWindow_ < maxDrainAttemptsPerSecond();
}

void Outbox::recordDrainAttempt(std::uint64_t nowMs) {
    if (!hasDrainWindow_ || nowMs - drainWindowStartMs_ >= 1000U) {
        drainWindowStartMs_ = nowMs;
        drainAttemptsInWindow_ = 0;
        hasDrainWindow_ = true;
    }
    if (drainAttemptsInWindow_ != std::numeric_limits<std::uint16_t>::max()) {
        drainAttemptsInWindow_ += 1;
    }
}

std::uint32_t Outbox::drainRateRemainingMs(std::uint64_t nowMs) const {
    if (!hasDrainWindow_ || nowMs - drainWindowStartMs_ >= 1000U) {
        return 0;
    }
    return static_cast<std::uint32_t>(1000U - (nowMs - drainWindowStartMs_));
}


bool Outbox::canDropCorruptFrontRecord() const {
    return config_.maxCorruptDropsPerLifetime != 0 &&
           corruptDropsThisLifetime_ < config_.maxCorruptDropsPerLifetime;
}

DrainResult Outbox::dropCorruptFrontRecord(Status corruptStatus) {
    DrainResult result;
    if (!canDropCorruptFrontRecord()) {
        result.queueError = true;
        result.detail = Status::failure(
            corruptStatus.code,
            "corrupt front record drop limit exceeded",
            corruptStatus.backendCode);
        emitDiagnostic(Severity::Error, result.detail, "drain_corrupt_front_limit");
        return result;
    }

    const Status popStatus = queue_.pop();
    if (!popStatus.ok()) {
        result.queueError = true;
        result.detail = popStatus;
        emitDiagnostic(Severity::Error, popStatus, "drain_corrupt_front_pop");
        return result;
    }

    clearFrontCooldown();
    corruptDropsThisLifetime_ += 1;
    result.corruptDropped += 1;
    result.detail = corruptStatus;
    emitRequestEvent(
        EventKind::RequestDropped,
        Severity::Error,
        corruptStatus,
        "drain_corrupt_front",
        0,
        0);
    return result;
}

std::uint16_t Outbox::maxDrainAttemptsPerSecond() const {
    return config_.maxDrainAttemptsPerSecond == 0 ? 1 : config_.maxDrainAttemptsPerSecond;
}

} // namespace pqueue

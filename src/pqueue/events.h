#pragma once

#include <cstdint>

#include "status.h"

namespace pqueue {

enum class Severity {
    Debug,
    Info,
    Warning,
    Error,
};

enum class EventKind {
    Diagnostic,
    RequestSent,
    RequestRetried,
    RequestDropped,
};

constexpr std::uint32_t kNoSequence = 0xffffffffU;

// String pointer fields in Event are borrowed.
// They are valid only for the duration of EventOptions::emit()/EventSink.
// Event sinks that retain events must copy any string data they need.
struct Event {
    EventKind kind = EventKind::Diagnostic;
    Severity severity = Severity::Debug;
    Status status = Status::success();
    const char* component = "";
    const char* operation = "";
    std::uint32_t sequence = kNoSequence;
    const char* path = "";
    int httpStatus = 0;
    const char* method = "";
    std::uint8_t attempt = 0;
    std::uint32_t queueCount = 0;
    std::uint32_t bodyBytes = 0;
    std::uint32_t remainingMs = 0;
    std::uint32_t timeoutMs = 0;
    std::uint32_t headerCount = 0;
};

using EventSink = void (*)(const Event& event, void* user);

struct EventOptions {
    EventSink sink = nullptr;
    void* user = nullptr;

    void emit(const Event& event) const {
        if (sink != nullptr) {
            sink(event, user);
        }
    }
};

} // namespace pqueue

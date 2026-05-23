#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "store_types.h"
#include "status.h"
#include "types.h"

namespace pqueue {

class Outbox;

class Queue {
public:
    explicit Queue(Config config = Config{});
    ~Queue();

    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    Status enqueue(Span record);
    Status enqueue(const std::string& record);
    Status peekSize(std::size_t& out);
    Status peek(MutableSpan out, std::size_t& written);
    Status peek(std::string& out);
    Status pop();
    Status format();
    CompactIdleResult compactIdle(std::size_t maxSteps);
    Status dropFrontIfCorrupt();
    Status recoverStaleLock();
    ValidationResult validate(const ValidationOptions& options = ValidationOptions{});
    StatsResult statsResult();
    Stats stats();

private:
    friend class Outbox;
    using RecordVisitor = bool (*)(void* context, const std::string& record, std::uint32_t sequence, std::uint32_t ordinal);

    Status rewriteFront(const std::string& record);

    class ScopedLock;

    Status loadLatestIndex();
    Status evictFront();
    Status acquireLock();
    void releaseLock();
    Status emit(Event event) const;
    Status visitRecords(RecordVisitor visitor, void* context);
    Status diagnostic(Severity severity, Status status, const char* operation) const;

    Config config_;
    std::unique_ptr<Store> store_;
    QueueIndex index_;
    std::string lockContents_;
    bool lockHeld_ = false;
};

} // namespace pqueue

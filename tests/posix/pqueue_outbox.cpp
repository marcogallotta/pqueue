#include "pqueue/outbox.h"
#include "pqueue/file_system.h"

#include "pqueue_append_log_support.h"
#include "doctest/doctest.h"

#ifndef ARDUINO
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>
#endif

namespace {

#ifndef ARDUINO
const std::filesystem::path kOutboxSpoolDir = "build/pqueue-spools/pqueue_outbox_test_spool";

void cleanOutboxSpool() {
    std::error_code ec;
    std::filesystem::remove_all(kOutboxSpoolDir, ec);
}

struct FakeClock {
    std::uint64_t nowMs = 1000;
};

std::uint64_t fakeClockNow(void* context) {
    return static_cast<FakeClock*>(context)->nowMs;
}

struct FakeSender {
    std::vector<pqueue::SendDecision> decisions;
    std::vector<std::string> payloads;
    std::vector<pqueue::RetryState> retries;
};

pqueue::SendResult fakeSend(void* context, const std::string& payload, const pqueue::RetryState& retry) {
    auto* sender = static_cast<FakeSender*>(context);
    sender->payloads.push_back(payload);
    sender->retries.push_back(retry);

    if (sender->decisions.empty()) {
        return {pqueue::SendDecision::Sent};
    }

    const pqueue::SendDecision decision = sender->decisions.front();
    sender->decisions.erase(sender->decisions.begin());
    return {decision};
}

void capturePqueueEvent(const pqueue::Event& event, void* user) {
    auto* events = static_cast<std::vector<pqueue::Event>*>(user);
    events->push_back(event);
}

pqueue::OutboxConfig testOutboxConfig() {
    pqueue::OutboxConfig config;
    config.retryDelayMs = 1000;
    config.maxDrainAttemptsPerSecond = 1;
    return config;
}

pqueue::Config makeOutboxQueueConfig() {
    pqueue::Config cfg;
    cfg.basePath = kOutboxSpoolDir.string();
    cfg.storeLayout = pqueue::StoreLayout::AppendLog;
    cfg.minFreeBytes = 0;
    return cfg;
}

pqueue::Outbox makeOutbox(
    FakeSender& sender,
    FakeClock& clock,
    pqueue::OutboxConfig outboxConfig
) {
    return pqueue::Outbox(makeOutboxQueueConfig(), outboxConfig, fakeSend, &sender, fakeClockNow, &clock);
}

pqueue::Outbox makeOutbox(
    const pqueue::Config& queueConfig,
    FakeSender& sender,
    FakeClock& clock,
    pqueue::OutboxConfig outboxConfig
) {
    return pqueue::Outbox(queueConfig, outboxConfig, fakeSend, &sender, fakeClockNow, &clock);
}

pqueue::Outbox makeOutbox(FakeSender& sender, FakeClock& clock) {
    return makeOutbox(sender, clock, testOutboxConfig());
}
#endif

} // namespace

TEST_CASE("pqueue outbox sends immediately when queue is empty") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeSender sender;
    FakeClock clock;

    auto outbox = makeOutbox(sender, clock);
    const auto result = outbox.submit("fresh");

    CHECK(result.status == pqueue::SubmitStatus::Sent);
    CHECK_EQ(sender.payloads.size(), 1U);
    CHECK_EQ(sender.payloads[0], "fresh");
    CHECK_EQ(outbox.stats().count, 0U);
#endif
}

TEST_CASE("pqueue outbox queues retryable fresh send failure") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeSender sender;
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    FakeClock clock;

    auto outbox = makeOutbox(sender, clock);
    const auto result = outbox.submit("fresh");

    CHECK(result.status == pqueue::SubmitStatus::Queued);
    CHECK_EQ(outbox.stats().count, 1U);
#endif
}


TEST_CASE("pqueue outbox supports multiple live objects on the same base path") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeSender firstSender;
    firstSender.decisions.push_back(pqueue::SendDecision::RetryLater);
    FakeSender secondSender;
    FakeClock clock;

    auto first = makeOutbox(firstSender, clock);
    auto second = makeOutbox(secondSender, clock);

    REQUIRE(first.submit("first").status == pqueue::SubmitStatus::Queued);
    CHECK_EQ(first.stats().count, 1U);

    // AppendLog mounts lazily: second mounts on first operation and sees first's
    // queued record, then enqueues its own directly without attempting a send.
    REQUIRE(second.submit("second").status == pqueue::SubmitStatus::Queued);
    CHECK_EQ(second.stats().count, 2U);
#endif
}

TEST_CASE("pqueue outbox preserves FIFO when backlog exists") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeSender sender;
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    FakeClock clock;

    auto outbox = makeOutbox(sender, clock);
    REQUIRE(outbox.submit("first").status == pqueue::SubmitStatus::Queued);
    CHECK(outbox.submit("second").status == pqueue::SubmitStatus::Queued);
    CHECK_EQ(sender.payloads.size(), 1U);

    clock.nowMs += 1000;
    sender.decisions.push_back(pqueue::SendDecision::Sent);
    auto drain = outbox.drain();

    CHECK_EQ(drain.sent, 1U);
    REQUIRE_EQ(sender.payloads.size(), 2U);
    CHECK_EQ(sender.payloads[1], "first");
    CHECK_EQ(outbox.stats().count, 1U);

    clock.nowMs += 1000;
    sender.decisions.push_back(pqueue::SendDecision::Sent);
    drain = outbox.drain();

    CHECK_EQ(drain.sent, 1U);
    REQUIRE_EQ(sender.payloads.size(), 3U);
    CHECK_EQ(sender.payloads[2], "second");
    CHECK_EQ(outbox.stats().count, 0U);
#endif
}

TEST_CASE("pqueue outbox respects retry delay") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeSender sender;
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    FakeClock clock;

    auto outbox = makeOutbox(sender, clock);
    REQUIRE(outbox.submit("fresh").status == pqueue::SubmitStatus::Queued);

    clock.nowMs += 500;
    auto drain = outbox.drain();

    CHECK(drain.notDue);
    CHECK_EQ(sender.payloads.size(), 1U);
    CHECK_EQ(outbox.stats().count, 1U);
#endif
}

TEST_CASE("pqueue outbox retries transient failures indefinitely") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeSender sender;
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    FakeClock clock;
    pqueue::OutboxConfig config;
    config.retryDelayMs = 1000;

    auto outbox = makeOutbox(sender, clock, config);
    REQUIRE(outbox.submit("fresh").status == pqueue::SubmitStatus::Queued);

    clock.nowMs += 1000;
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    outbox.drain();

    CHECK_EQ(outbox.stats().count, 1U);
#endif
}

TEST_CASE("pqueue outbox emits dropped event when stored envelope cannot be decoded") {
#ifndef ARDUINO
    cleanOutboxSpool();
    {
        pqueue::Config queueConfig = makeOutboxQueueConfig();
        pqueue::Queue queue(queueConfig);
        REQUIRE(queue.enqueue("not an outbox envelope").ok());
    }

    std::vector<pqueue::Event> events;
    FakeSender sender;
    FakeClock clock;
    pqueue::OutboxConfig config = testOutboxConfig();
    config.events = {capturePqueueEvent, &events};

    auto outbox = makeOutbox(sender, clock, config);
    auto drain = outbox.drain();

    CHECK_EQ(drain.corruptDropped, 1U);
    REQUIRE_FALSE(events.empty());
    const auto& event = events.back();
    CHECK(event.kind == pqueue::EventKind::RequestDropped);
    CHECK(event.severity == pqueue::Severity::Error);
    CHECK(event.status.code == pqueue::StatusCode::DecodeFailed);
#endif
}

TEST_CASE("pqueue outbox stops after corrupt front drop lifetime limit") {
#ifndef ARDUINO
    cleanOutboxSpool();
    {
        pqueue::Config queueConfig = makeOutboxQueueConfig();
        pqueue::Queue queue(queueConfig);
        REQUIRE(queue.enqueue("corrupt-0").ok());
        REQUIRE(queue.enqueue("corrupt-1").ok());
        REQUIRE(queue.enqueue("corrupt-2").ok());
        REQUIRE(queue.enqueue("corrupt-3").ok());
    }

    FakeSender sender;
    FakeClock clock;
    pqueue::OutboxConfig config = testOutboxConfig();
    config.maxCorruptDropsPerLifetime = 3;
    auto outbox = makeOutbox(sender, clock, config);
    const auto drain = outbox.drainUpTo(10);

    CHECK_EQ(drain.corruptDropped, 3U);
    CHECK(drain.queueError);
    CHECK(drain.detail.code == pqueue::StatusCode::DecodeFailed);
    CHECK_EQ(std::string(drain.detail.message), "corrupt front record drop limit exceeded");
    CHECK(sender.payloads.empty());
    CHECK_EQ(outbox.stats().count, 1U);
#endif
}

TEST_CASE("pqueue outbox keeps retry cooldown in RAM only") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeSender sender;
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    FakeClock clock;

    {
        auto outbox = makeOutbox(sender, clock);
        REQUIRE(outbox.submit("fresh").status == pqueue::SubmitStatus::Queued);

        clock.nowMs += 500;
        auto drain = outbox.drain();
        CHECK(drain.notDue);
    }

    sender.decisions.push_back(pqueue::SendDecision::Sent);
    auto restarted = makeOutbox(sender, clock);
    auto drain = restarted.drain();

    CHECK_FALSE(drain.notDue);
    CHECK_EQ(drain.sent, 1U);
    CHECK_EQ(restarted.stats().count, 0U);
#endif
}

TEST_CASE("pqueue outbox passes persisted attempts to sender") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeSender sender;
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    FakeClock clock;
    pqueue::OutboxConfig config = testOutboxConfig();
    config.retryDelayMs = 0;

    auto outbox = makeOutbox(sender, clock, config);
    REQUIRE(outbox.submit("fresh").status == pqueue::SubmitStatus::Queued);

    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    auto drain = outbox.drain();
    CHECK_EQ(drain.attempts, 1U);

    clock.nowMs += 1000;
    sender.decisions.push_back(pqueue::SendDecision::Sent);
    drain = outbox.drain();
    CHECK_EQ(drain.sent, 1U);

    REQUIRE_GE(sender.retries.size(), 3U);
    CHECK_EQ(sender.retries[1].attempts, 1U);
    CHECK_EQ(sender.retries[2].attempts, 2U);
#endif
}

TEST_CASE("pqueue outbox throttles drain attempts") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeSender sender;
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    FakeClock clock;
    pqueue::OutboxConfig config;
    config.retryDelayMs = 0;
    config.maxDrainAttemptsPerSecond = 1;

    auto outbox = makeOutbox(sender, clock, config);
    REQUIRE(outbox.submit("fresh").status == pqueue::SubmitStatus::Queued);

    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    auto first = outbox.drain();
    CHECK_EQ(first.attempts, 1U);

    auto second = outbox.drain();
    CHECK(second.rateLimited);
#endif
}

TEST_CASE("pqueue outbox allows first drain immediately") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeSender sender;
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    FakeClock clock;
    clock.nowMs = 0;
    pqueue::OutboxConfig config;
    config.retryDelayMs = 0;
    config.maxDrainAttemptsPerSecond = 1;

    auto outbox = makeOutbox(sender, clock, config);
    REQUIRE(outbox.submit("fresh").status == pqueue::SubmitStatus::Queued);

    sender.decisions.push_back(pqueue::SendDecision::Sent);
    auto drain = outbox.drain();

    CHECK_FALSE(drain.rateLimited);
    CHECK_EQ(drain.sent, 1U);
#endif
}

TEST_CASE("pqueue outbox drainUpTo sends multiple queued records within rate cap") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeSender sender;
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    FakeClock clock;
    pqueue::OutboxConfig config;
    config.retryDelayMs = 0;
    config.maxDrainAttemptsPerSecond = 3;

    auto outbox = makeOutbox(sender, clock, config);
    REQUIRE(outbox.submit("one").status == pqueue::SubmitStatus::Queued);
    REQUIRE(outbox.submit("two").status == pqueue::SubmitStatus::Queued);
    REQUIRE(outbox.submit("three").status == pqueue::SubmitStatus::Queued);
    REQUIRE(outbox.submit("four").status == pqueue::SubmitStatus::Queued);
    CHECK_EQ(outbox.stats().count, 4U);

    const auto first = outbox.drainUpTo(3);
    CHECK_EQ(first.attempts, 3U);
    CHECK_EQ(first.sent, 3U);
    CHECK_FALSE(first.rateLimited);
    CHECK_EQ(outbox.stats().count, 1U);

    clock.nowMs += 1000;
    const auto second = outbox.drainUpTo(3);
    CHECK_EQ(second.sent, 1U);
    CHECK_EQ(outbox.stats().count, 0U);
#endif
}


TEST_CASE("pqueue outbox drainUpTo respects per-second cap") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeSender sender;
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    FakeClock clock;
    pqueue::OutboxConfig config;
    config.retryDelayMs = 0;
    config.maxDrainAttemptsPerSecond = 2;

    auto outbox = makeOutbox(sender, clock, config);
    REQUIRE(outbox.submit("one").status == pqueue::SubmitStatus::Queued);
    REQUIRE(outbox.submit("two").status == pqueue::SubmitStatus::Queued);
    REQUIRE(outbox.submit("three").status == pqueue::SubmitStatus::Queued);

    const auto first = outbox.drainUpTo(3);
    CHECK_EQ(first.attempts, 2U);
    CHECK_EQ(first.sent, 2U);
    CHECK(first.rateLimited);
    CHECK_EQ(outbox.stats().count, 1U);

    const auto second = outbox.drainUpTo(3);
    CHECK_EQ(second.attempts, 0U);
    CHECK(second.rateLimited);
    CHECK_EQ(outbox.stats().count, 1U);

    clock.nowMs += 1000;
    const auto third = outbox.drainUpTo(3);
    CHECK_EQ(third.sent, 1U);
    CHECK_FALSE(third.rateLimited);
    CHECK_EQ(outbox.stats().count, 0U);
#endif
}

TEST_CASE("pqueue outbox validate accepts queued outbox envelopes") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeSender sender;
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    FakeClock clock;

    auto outbox = makeOutbox(sender, clock);
    REQUIRE(outbox.submit("fresh").status == pqueue::SubmitStatus::Queued);

    const auto result = outbox.validate();

    CHECK(result.ok);
    CHECK(result.errors.empty());
#endif
}

TEST_CASE("pqueue outbox validate rejects malformed outbox envelopes") {
#ifndef ARDUINO
    cleanOutboxSpool();
    {
        pqueue::Config queueConfig = makeOutboxQueueConfig();
        pqueue::Queue queue(queueConfig);
        REQUIRE(queue.enqueue("not an outbox envelope").ok());
    }

    FakeSender sender;
    FakeClock clock;
    auto outbox = makeOutbox(sender, clock);

    const auto result = outbox.validate();

    REQUIRE_FALSE(result.ok);
    REQUIRE_EQ(result.errors.size(), 1U);
    CHECK(result.errors[0].code == pqueue::ValidationIssueCode::OutboxEnvelopeInvalid);
#endif
}


TEST_CASE("pqueue outbox drops fresh request immediately when send policy drops") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeSender sender;
    sender.decisions.push_back(pqueue::SendDecision::Drop);
    FakeClock clock;

    auto outbox = makeOutbox(sender, clock);
    const auto result = outbox.submit("fresh");

    CHECK(result.status == pqueue::SubmitStatus::Dropped);
    CHECK_EQ(sender.payloads.size(), 1U);
    CHECK_EQ(sender.payloads[0], "fresh");
    CHECK_EQ(outbox.stats().count, 0U);
#endif
}

TEST_CASE("pqueue outbox drops queued request when send policy drops during drain") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeSender sender;
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    FakeClock clock;
    pqueue::OutboxConfig config = testOutboxConfig();
    config.retryDelayMs = 0;

    auto outbox = makeOutbox(sender, clock, config);
    REQUIRE(outbox.submit("queued").status == pqueue::SubmitStatus::Queued);

    sender.decisions.push_back(pqueue::SendDecision::Drop);
    const auto drain = outbox.drain();

    CHECK_EQ(drain.dropped, 1U);
    CHECK_EQ(drain.sent, 0U);
    CHECK_EQ(outbox.stats().count, 0U);
    REQUIRE_GE(sender.payloads.size(), 2U);
    CHECK_EQ(sender.payloads[1], "queued");
#endif
}

TEST_CASE("pqueue outbox drainUpTo stops when front request asks to retry") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeSender sender;
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    FakeClock clock;
    pqueue::OutboxConfig config = testOutboxConfig();
    config.retryDelayMs = 0;
    config.maxDrainAttemptsPerSecond = 1;

    auto outbox = makeOutbox(sender, clock, config);
    REQUIRE(outbox.submit("one").status == pqueue::SubmitStatus::Queued);
    REQUIRE(outbox.submit("two").status == pqueue::SubmitStatus::Queued);
    REQUIRE(outbox.submit("three").status == pqueue::SubmitStatus::Queued);

    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    const auto drain = outbox.drainUpTo(3);

    CHECK_EQ(drain.attempts, 1U);
    CHECK_EQ(drain.sent, 0U);
    CHECK_EQ(outbox.stats().count, 3U);
    REQUIRE_EQ(sender.payloads.size(), 2U);
    CHECK_EQ(sender.payloads[1], "one");
#endif
}

TEST_CASE("pqueue outbox reports send error when sender is not configured") {
#ifndef ARDUINO
    cleanOutboxSpool();
    std::vector<pqueue::Event> events;
    FakeClock clock;

    pqueue::Config queueConfig = makeOutboxQueueConfig();
    pqueue::OutboxConfig config = testOutboxConfig();
    config.events = {capturePqueueEvent, &events};

    pqueue::Outbox outbox(queueConfig, config, nullptr, nullptr, fakeClockNow, &clock);

    const auto submitted = outbox.submit("fresh");
    CHECK(submitted.status == pqueue::SubmitStatus::SendError);
    CHECK(submitted.detail.code == pqueue::StatusCode::SendFailed);

    const auto drained = outbox.drain();
    CHECK(drained.sendError);
    CHECK(drained.detail.code == pqueue::StatusCode::SendFailed);

    REQUIRE_GE(events.size(), 2U);
    CHECK(events[0].kind == pqueue::EventKind::Diagnostic);
    CHECK(events[0].severity == pqueue::Severity::Error);
    CHECK(events[0].status.code == pqueue::StatusCode::SendFailed);
#endif
}

TEST_CASE("pqueue outbox reports send error when clock is not configured") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeSender sender;

    pqueue::Config queueConfig = makeOutboxQueueConfig();
    pqueue::Outbox outbox(queueConfig, testOutboxConfig(), fakeSend, &sender, nullptr, nullptr);

    const auto submitted = outbox.submit("fresh");
    CHECK(submitted.status == pqueue::SubmitStatus::SendError);
    CHECK(submitted.detail.code == pqueue::StatusCode::SendFailed);
    CHECK(sender.payloads.empty());

    const auto drained = outbox.drain();
    CHECK(drained.sendError);
    CHECK(drained.detail.code == pqueue::StatusCode::SendFailed);
#endif
}

TEST_CASE("pqueue outbox returns QueueFull when retry enqueue cannot fit") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeSender sender;
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    FakeClock clock;

    // Capacity for exactly 1 record: 20 (seg header) + 24 (record overhead) + 17 (envelope for "one") = 61 bytes.
    pqueue::Config queueConfig = makeOutboxQueueConfig();
    queueConfig.maxSegmentBytes = 128;
    queueConfig.reservedBytes = 61;

    pqueue::OutboxConfig config = testOutboxConfig();
    config.retryDelayMs = 0;

    pqueue::Outbox outbox(queueConfig, config, fakeSend, &sender, fakeClockNow, &clock);

    const auto first = outbox.submit("one");
    REQUIRE(first.status == pqueue::SubmitStatus::Queued);
    CHECK_EQ(outbox.stats().count, 1U);

    const auto second = outbox.submit("two");
    CHECK(second.status == pqueue::SubmitStatus::QueueFull);
    CHECK(second.detail.code == pqueue::StatusCode::QueueFull);
    CHECK_EQ(outbox.stats().count, 1U);
    CHECK_EQ(sender.payloads.size(), 1U);
#endif
}

TEST_CASE("pqueue outbox reports SendError when retry enqueue fails for non-full reason") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeSender sender;
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    FakeClock clock;

    // maxSegmentBytes=1 forces RecordTooLarge on any enqueue attempt.
    pqueue::Config queueConfig = makeOutboxQueueConfig();
    queueConfig.maxSegmentBytes = 1;

    pqueue::OutboxConfig config = testOutboxConfig();
    config.retryDelayMs = 0;

    pqueue::Outbox outbox(queueConfig, config, fakeSend, &sender, fakeClockNow, &clock);
    const auto result = outbox.submit("one");

    CHECK(result.status == pqueue::SubmitStatus::SendError);
    CHECK(result.detail.code == pqueue::StatusCode::RecordTooLarge);
    CHECK_EQ(outbox.stats().count, 0U);
#endif
}

TEST_CASE("pqueue outbox reports SendError when retry enqueue cannot acquire queue lock") {
#ifndef ARDUINO
    cleanOutboxSpool();
    std::filesystem::create_directories(kOutboxSpoolDir);
    {
        std::ofstream lockFile(kOutboxSpoolDir / ".pqueue.lock", std::ios::binary | std::ios::trunc);
        lockFile << "pqueue-lock-v1\n";
        lockFile << "pid=" << static_cast<long>(::getpid()) << "\n";
        lockFile << "token=active-test-lock\n";
    }

    FakeSender sender;
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    FakeClock clock;
    auto outbox = makeOutbox(sender, clock, testOutboxConfig());

    const auto result = outbox.submit("retry-later");

    CHECK(result.status == pqueue::SubmitStatus::SendError);
    CHECK(result.detail.code == pqueue::StatusCode::LockTimeout);
    REQUIRE_EQ(sender.payloads.size(), 1U);
    CHECK_EQ(sender.payloads[0], "retry-later");
#endif
}

TEST_CASE("pqueue outbox reports SendError when retry enqueue hits storage write failure") {
#ifndef ARDUINO
    cleanOutboxSpool();
    auto inner = pqueue::makePosixFileSystem();
    auto fs = std::make_shared<FaultInjectingFs>(inner);

    pqueue::Config queueConfig = makeOutboxQueueConfig();
    queueConfig.fileSystem = fs;

    FakeSender sender;
    FakeClock clock;
    auto outbox = makeOutbox(queueConfig, sender, clock, testOutboxConfig());
    REQUIRE_EQ(outbox.stats().count, 0U);

    fs->failNextWriteFileTo = "seg-";
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);

    const auto result = outbox.submit("retry-later");

    CHECK(result.status == pqueue::SubmitStatus::SendError);
    CHECK(result.detail.code == pqueue::StatusCode::WriteFailed);
    REQUIRE_EQ(sender.payloads.size(), 1U);
    CHECK_EQ(sender.payloads[0], "retry-later");
    CHECK_EQ(outbox.stats().count, 0U);
#endif
}

TEST_CASE("pqueue outbox reports queueError when sent queued request cannot be popped") {
#ifndef ARDUINO
    cleanOutboxSpool();
    auto inner = pqueue::makePosixFileSystem();
    auto fs = std::make_shared<FaultInjectingFs>(inner);

    pqueue::Config queueConfig = makeOutboxQueueConfig();
    queueConfig.fileSystem = fs;

    FakeSender sender;
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    FakeClock clock;
    pqueue::OutboxConfig config = testOutboxConfig();
    config.retryDelayMs = 0;

    auto outbox = makeOutbox(queueConfig, sender, clock, config);
    REQUIRE(outbox.submit("queued").status == pqueue::SubmitStatus::Queued);

    fs->failNextWriteAtTo = "seg-";
    sender.decisions.push_back(pqueue::SendDecision::Sent);

    const auto drain = outbox.drain();

    CHECK_EQ(drain.attempts, 1U);
    CHECK_EQ(drain.sent, 0U);
    CHECK(drain.queueError);
    CHECK(drain.detail.code == pqueue::StatusCode::WriteFailed);
    REQUIRE_GE(sender.payloads.size(), 2U);
    CHECK_EQ(sender.payloads[1], "queued");
#endif
}

TEST_CASE("pqueue outbox treats zero drain rate cap as one attempt per second") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeSender sender;
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    FakeClock clock;
    pqueue::OutboxConfig config = testOutboxConfig();
    config.retryDelayMs = 0;
    config.maxDrainAttemptsPerSecond = 0;

    auto outbox = makeOutbox(sender, clock, config);
    REQUIRE(outbox.submit("one").status == pqueue::SubmitStatus::Queued);
    REQUIRE(outbox.submit("two").status == pqueue::SubmitStatus::Queued);

    const auto first = outbox.drainUpTo(2);
    CHECK_EQ(first.attempts, 1U);
    CHECK_EQ(first.sent, 1U);
    CHECK(first.rateLimited);
    CHECK_EQ(outbox.stats().count, 1U);
#endif
}

TEST_CASE("pqueue outbox drainUpTo zero still performs one drain attempt") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeSender sender;
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    FakeClock clock;
    pqueue::OutboxConfig config = testOutboxConfig();
    config.retryDelayMs = 0;

    auto outbox = makeOutbox(sender, clock, config);
    REQUIRE(outbox.submit("queued").status == pqueue::SubmitStatus::Queued);

    const auto drain = outbox.drainUpTo(0);
    CHECK_EQ(drain.attempts, 1U);
    CHECK_EQ(drain.sent, 1U);
    CHECK_EQ(outbox.stats().count, 0U);
#endif
}

TEST_CASE("pqueue outbox compactIdle removes dead sealed segments") {
#ifndef ARDUINO
    cleanOutboxSpool();
    // 3-char payloads: envelope=17 bytes, per-record segment cost=41 bytes.
    // maxSegmentBytes=100 holds exactly 1 record per sealed segment (20+41=61 < 100, 20+82=102 > 100).
    pqueue::Config queueConfig = makeOutboxQueueConfig();
    queueConfig.maxSegmentBytes = 100;

    FakeSender sender;
    FakeClock clock;
    // submit("r00") triggers a live send attempt; the rest enqueue directly.
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);

    pqueue::OutboxConfig outboxConfig = testOutboxConfig();
    outboxConfig.retryDelayMs = 1000;
    auto outbox = makeOutbox(queueConfig, sender, clock, outboxConfig);

    REQUIRE(outbox.submit("r00").status == pqueue::SubmitStatus::Queued);
    REQUIRE(outbox.submit("r01").status == pqueue::SubmitStatus::Queued);
    REQUIRE(outbox.submit("r02").status == pqueue::SubmitStatus::Queued);
    REQUIRE(outbox.submit("r03").status == pqueue::SubmitStatus::Queued);

    // Drain r00 and r01 into separate 1-second rate windows.
    clock.nowMs += 1000;
    sender.decisions.push_back(pqueue::SendDecision::Sent);
    outbox.drain();
    clock.nowMs += 1000;
    sender.decisions.push_back(pqueue::SendDecision::Sent);
    outbox.drain();
    CHECK_EQ(outbox.stats().count, 2U);

    // Two sealed segments are now fully dead; compactIdle should do real work.
    const auto result = outbox.compactIdle(16);
    CHECK(result.status.ok());
    CHECK(result.compactions > 0);
    CHECK(result.noOps <= 1);

    // Remaining records drain cleanly.
    clock.nowMs += 1000;
    sender.decisions.push_back(pqueue::SendDecision::Sent);
    outbox.drain();
    clock.nowMs += 1000;
    sender.decisions.push_back(pqueue::SendDecision::Sent);
    outbox.drain();
    CHECK_EQ(outbox.stats().count, 0U);
    cleanOutboxSpool();
#endif
}

TEST_CASE("pqueue outbox handles unknown send decisions as send errors") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeSender sender;
    sender.decisions.push_back(static_cast<pqueue::SendDecision>(255));
    FakeClock clock;

    auto outbox = makeOutbox(sender, clock);
    const auto submitted = outbox.submit("fresh");
    CHECK(submitted.status == pqueue::SubmitStatus::SendError);
    CHECK(submitted.detail.code == pqueue::StatusCode::SendFailed);

    cleanOutboxSpool();
    FakeSender drainSender;
    drainSender.decisions.push_back(pqueue::SendDecision::RetryLater);
    auto drainOutbox = makeOutbox(drainSender, clock);
    REQUIRE(drainOutbox.submit("queued").status == pqueue::SubmitStatus::Queued);

    drainSender.decisions.push_back(static_cast<pqueue::SendDecision>(255));
    clock.nowMs += 1000;
    const auto drained = drainOutbox.drain();
    CHECK(drained.sendError);
    CHECK(drained.detail.code == pqueue::StatusCode::SendFailed);
    CHECK_EQ(drainOutbox.stats().count, 1U);
#endif
}

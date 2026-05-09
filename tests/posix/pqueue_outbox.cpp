#include "pqueue/outbox.h"
#include "pqueue/storage_common.h"
#include "support/pqueue_file_store_support.h"

#include "doctest/doctest.h"

#ifndef ARDUINO
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#endif

namespace {

#ifndef ARDUINO
const std::filesystem::path kOutboxSpoolDir = "pqueue_outbox_test_spool";

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

pqueue::Outbox makeOutbox(
    FakeSender& sender,
    FakeClock& clock,
    pqueue::OutboxConfig outboxConfig
) {
    pqueue::Config queueConfig;
    queueConfig.basePath = kOutboxSpoolDir.string();
    return pqueue::Outbox(queueConfig, outboxConfig, fakeSend, &sender, fakeClockNow, &clock);
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

std::uint64_t outboxSlotOffset(std::uint32_t sequence) {
    pqueue::Config config;
    const std::uint64_t slotSize = pqueue::storage_detail::kRecordHeaderBytes + config.recordSizeBytes;
    const std::uint64_t capacity = config.reservedBytes / slotSize;
    return static_cast<std::uint64_t>(pqueue::storage_detail::kCheckpointSlots) *
               pqueue::storage_detail::kCheckpointRecordBytes +
           config.journalBytes +
           (sequence % capacity) * slotSize;
}

void flipSpoolByte(std::uint64_t offset) {
    const auto path = kOutboxSpoolDir / "pqueue.spool";
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(file.good());
    file.seekg(static_cast<std::streamoff>(offset));
    char byte = 0;
    file.read(&byte, 1);
    REQUIRE(file.good());
    byte ^= static_cast<char>(0xff);
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(&byte, 1);
    REQUIRE(file.good());
}

void corruptOutboxSlotHeader(std::uint32_t sequence) {
    flipSpoolByte(outboxSlotOffset(sequence));
}

void corruptOutboxSlotPayload(std::uint32_t sequence) {
    flipSpoolByte(outboxSlotOffset(sequence) + pqueue::storage_detail::kRecordHeaderBytes);
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

    const auto secondSubmit = second.submit("second");
    CHECK(secondSubmit.status == pqueue::SubmitStatus::Queued);
    CHECK(secondSender.payloads.empty());
    CHECK_EQ(first.stats().count, 2U);
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

TEST_CASE("pqueue outbox drops corrupt front records") {
#ifndef ARDUINO
    cleanOutboxSpool();
    {
        pqueue::Config queueConfig;
        queueConfig.basePath = kOutboxSpoolDir.string();
        pqueue::Queue queue(queueConfig);
        REQUIRE(queue.enqueue("not an outbox envelope").ok());
    }

    FakeSender sender;
    FakeClock clock;

    auto outbox = makeOutbox(sender, clock);
    auto drain = outbox.drain();

    CHECK_EQ(drain.corruptDropped, 1U);
    CHECK_EQ(outbox.stats().count, 0U);
    CHECK(sender.payloads.empty());
#endif
}

TEST_CASE("pqueue outbox emits dropped event when stored envelope cannot be decoded") {
#ifndef ARDUINO
    cleanOutboxSpool();
    {
        pqueue::Config queueConfig;
        queueConfig.basePath = kOutboxSpoolDir.string();
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


TEST_CASE("pqueue outbox drops front record with corrupt storage payload CRC") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeClock clock;
    {
        FakeSender sender;
        sender.decisions.push_back(pqueue::SendDecision::RetryLater);
        auto outbox = makeOutbox(sender, clock);
        REQUIRE(outbox.submit("corrupt-front").status == pqueue::SubmitStatus::Queued);
        REQUIRE(outbox.submit("valid-behind").status == pqueue::SubmitStatus::Queued);
    }

    corruptOutboxSlotPayload(0);

    FakeSender sender;
    auto outbox = makeOutbox(sender, clock);
    const auto drain = outbox.drainUpTo(2);

    CHECK_EQ(drain.corruptDropped, 1U);
    CHECK_EQ(drain.sent, 1U);
    CHECK_FALSE(drain.queueError);
    REQUIRE_EQ(sender.payloads.size(), 1U);
    CHECK_EQ(sender.payloads[0], "valid-behind");
    CHECK_EQ(outbox.stats().count, 0U);
#endif
}

TEST_CASE("pqueue outbox drops front record with corrupt storage header") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeClock clock;
    {
        FakeSender sender;
        sender.decisions.push_back(pqueue::SendDecision::RetryLater);
        auto outbox = makeOutbox(sender, clock);
        REQUIRE(outbox.submit("corrupt-front").status == pqueue::SubmitStatus::Queued);
        REQUIRE(outbox.submit("valid-behind").status == pqueue::SubmitStatus::Queued);
    }

    corruptOutboxSlotHeader(0);

    FakeSender sender;
    auto outbox = makeOutbox(sender, clock);
    const auto drain = outbox.drainUpTo(2);

    CHECK_EQ(drain.corruptDropped, 1U);
    CHECK_EQ(drain.sent, 1U);
    CHECK_FALSE(drain.queueError);
    REQUIRE_EQ(sender.payloads.size(), 1U);
    CHECK_EQ(sender.payloads[0], "valid-behind");
    CHECK_EQ(outbox.stats().count, 0U);
#endif
}

TEST_CASE("pqueue outbox stops after corrupt front drop lifetime limit") {
#ifndef ARDUINO
    cleanOutboxSpool();
    FakeClock clock;
    {
        FakeSender sender;
        sender.decisions.push_back(pqueue::SendDecision::RetryLater);
        auto outbox = makeOutbox(sender, clock);
        REQUIRE(outbox.submit("corrupt-0").status == pqueue::SubmitStatus::Queued);
        REQUIRE(outbox.submit("corrupt-1").status == pqueue::SubmitStatus::Queued);
        REQUIRE(outbox.submit("corrupt-2").status == pqueue::SubmitStatus::Queued);
        REQUIRE(outbox.submit("corrupt-3").status == pqueue::SubmitStatus::Queued);
    }

    corruptOutboxSlotPayload(0);
    corruptOutboxSlotPayload(1);
    corruptOutboxSlotPayload(2);
    corruptOutboxSlotPayload(3);

    FakeSender sender;
    pqueue::OutboxConfig config = testOutboxConfig();
    config.maxCorruptDropsPerLifetime = 3;
    auto outbox = makeOutbox(sender, clock, config);
    const auto drain = outbox.drainUpTo(10);

    CHECK_EQ(drain.corruptDropped, 3U);
    CHECK(drain.queueError);
    CHECK(drain.detail.code == pqueue::StatusCode::CrcMismatch);
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
        pqueue::Config queueConfig;
        queueConfig.basePath = kOutboxSpoolDir.string();
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

    pqueue::Config queueConfig;
    queueConfig.basePath = kOutboxSpoolDir.string();
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

    pqueue::Config queueConfig;
    queueConfig.basePath = kOutboxSpoolDir.string();
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

    pqueue::Config queueConfig;
    queueConfig.basePath = kOutboxSpoolDir.string();
    queueConfig.recordSizeBytes = 32;
    queueConfig.reservedBytes = pqueue::storage_detail::kRecordHeaderBytes + queueConfig.recordSizeBytes;

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

    pqueue::Config queueConfig;
    queueConfig.basePath = kOutboxSpoolDir.string();
    queueConfig.recordSizeBytes = 32;
    queueConfig.reservedBytes = 1;

    pqueue::OutboxConfig config = testOutboxConfig();
    config.retryDelayMs = 0;

    pqueue::Outbox outbox(queueConfig, config, fakeSend, &sender, fakeClockNow, &clock);
    const auto result = outbox.submit("one");

    CHECK(result.status == pqueue::SubmitStatus::SendError);
    CHECK(result.detail.code == pqueue::StatusCode::InvalidArgument);
    CHECK_EQ(outbox.stats().count, 0U);
#endif
}


TEST_CASE("pqueue outbox reports SendError when retry enqueue cannot acquire queue lock") {
#ifndef ARDUINO
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    fileSystem->files[".pqueue.lock"] = "owned by another queue";

    FakeSender sender;
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    FakeClock clock;

    auto queueConfig = pqueue_test::makeQueueConfig(fileSystem, 4096, 512);
    auto outbox = makeOutbox(queueConfig, sender, clock, testOutboxConfig());

    const auto result = outbox.submit("retry-later");

    CHECK(result.status == pqueue::SubmitStatus::SendError);
    CHECK(result.detail.code == pqueue::StatusCode::LockTimeout);
    REQUIRE_EQ(sender.payloads.size(), 1U);
    CHECK_EQ(sender.payloads[0], "retry-later");
#endif
}

TEST_CASE("pqueue outbox reports SendError when retry enqueue hits storage write failure") {
#ifndef ARDUINO
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    FakeSender sender;
    FakeClock clock;

    auto queueConfig = pqueue_test::makeQueueConfig(fileSystem, 4096, 512);
    auto outbox = makeOutbox(queueConfig, sender, clock, testOutboxConfig());
    REQUIRE_EQ(outbox.stats().count, 0U);

    fileSystem->failNextWrite();
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
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    FakeSender sender;
    sender.decisions.push_back(pqueue::SendDecision::RetryLater);
    FakeClock clock;
    pqueue::OutboxConfig config = testOutboxConfig();
    config.retryDelayMs = 0;

    auto queueConfig = pqueue_test::makeQueueConfig(fileSystem, 4096, 512);
    auto outbox = makeOutbox(queueConfig, sender, clock, config);
    REQUIRE(outbox.submit("queued").status == pqueue::SubmitStatus::Queued);

    fileSystem->failNextWrite();
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

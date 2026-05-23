#include "pqueue/http/outbox.h"
#include "pqueue/envelope.h"

#include "doctest/doctest.h"

#ifndef ARDUINO
#include <filesystem>
#include <string>
#include <vector>
#endif

namespace {

#ifndef ARDUINO
const std::filesystem::path kHttpOutboxSpoolDir = "build/pqueue-spools/pqueue_http_outbox_test_spool";

void cleanHttpOutboxSpool() {
    std::error_code ec;
    std::filesystem::remove_all(kHttpOutboxSpoolDir, ec);
}

struct FakeClock {
    std::uint64_t nowMs = 1000;
};

std::uint64_t fakeClockNow(void* context) {
    return static_cast<FakeClock*>(context)->nowMs;
}

struct PostedRequest {
    std::string url;
    std::vector<pqueue::http::Header> headers;
    std::string body;
};

struct FakeHttpTransport : pqueue::http::Transport {
    std::vector<pqueue::http::Response> responses;
    std::vector<PostedRequest> posts;

    pqueue::http::Response post(
        const char* url,
        const pqueue::http::Header* headers,
        std::size_t headerCount,
        const std::uint8_t* body,
        std::size_t bodySize
    ) override {
        PostedRequest request;
        request.url = url == nullptr ? std::string{} : std::string{url};
        for (std::size_t i = 0; i < headerCount; ++i) {
            request.headers.push_back(headers[i]);
        }
        request.body.assign(reinterpret_cast<const char*>(body), bodySize);
        posts.push_back(request);

        if (responses.empty()) {
            return {200, pqueue::http::TransportError::None};
        }

        const pqueue::http::Response response = responses.front();
        responses.erase(responses.begin());
        return response;
    }
};

pqueue::http::Response callbackPost(
    void* context,
    const char* url,
    const pqueue::http::Header* headers,
    std::size_t headerCount,
    const std::uint8_t* body,
    std::size_t bodySize
) {
    return static_cast<FakeHttpTransport*>(context)->post(url, headers, headerCount, body, bodySize);
}

pqueue::http::Outbox makeHttpOutbox(
    FakeHttpTransport& transport,
    FakeClock& clock,
    const pqueue::http::Header* headers = nullptr,
    std::size_t headerCount = 0
) {
    pqueue::http::Config httpConfig;
    httpConfig.queue.basePath = kHttpOutboxSpoolDir.string();
    httpConfig.outbox.initialRetryDelayMs = 1000;
    httpConfig.outbox.maxDrainAttemptsPerSecond = 1;
    httpConfig.baseUrl = "https://example.test/api";
    httpConfig.headers = headers;
    httpConfig.headerCount = headerCount;

    return pqueue::http::Outbox(httpConfig, transport, fakeClockNow, &clock);
}


struct SeenResponse {
    std::string path;
    int statusCode = pqueue::http::kNoStatusCode;
    pqueue::http::TransportError error = pqueue::http::TransportError::Unknown;
    std::string body;
};

struct ResponseObserver {
    std::vector<SeenResponse> seen;
};

void onResponse(void* context, const pqueue::http::RequestEnvelope& request, const pqueue::http::Response& response) {
    auto* observer = static_cast<ResponseObserver*>(context);
    observer->seen.push_back({request.path, response.statusCode, response.error, response.body});
}

struct SeenDrop {
    bool hasRequest = false;
    std::string path;
    pqueue::http::DropReason reason = pqueue::http::DropReason::DecodeFailed;
    bool hasResponse = false;
    int statusCode = pqueue::http::kNoStatusCode;
};

struct DropObserver {
    std::vector<SeenDrop> seen;
};

void onDrop(
    void* context,
    const pqueue::http::RequestEnvelope* request,
    pqueue::http::DropReason reason,
    const pqueue::http::Response* response
) {
    auto* observer = static_cast<DropObserver*>(context);
    SeenDrop drop;
    drop.reason = reason;
    if (request != nullptr) {
        drop.hasRequest = true;
        drop.path = request->path;
    }
    if (response != nullptr) {
        drop.hasResponse = true;
        drop.statusCode = response->statusCode;
    }
    observer->seen.push_back(drop);
}

struct CustomClassifier {
    pqueue::SendDecision decision = pqueue::SendDecision::Drop;
    std::vector<int> statuses;
};

pqueue::SendDecision customClassify(void* context, const pqueue::http::Response& response) {
    auto* classifier = static_cast<CustomClassifier*>(context);
    classifier->statuses.push_back(response.statusCode);
    return classifier->decision;
}
#endif

} // namespace

TEST_CASE("pqueue http outbox returns encode failure detail") {
#ifndef ARDUINO
    cleanHttpOutboxSpool();
    FakeHttpTransport transport;
    FakeClock clock;
    auto outbox = makeHttpOutbox(transport, clock);

    const auto result = outbox.submitPost(std::string(65536, 'x'), "body");

    CHECK(result.status == pqueue::SubmitStatus::SendError);
    CHECK(result.detail.code == pqueue::StatusCode::EncodeFailed);
    CHECK(std::string(result.detail.message) == "failed to encode HTTP request envelope");
    CHECK(transport.posts.empty());
#endif
}

TEST_CASE("pqueue http outbox posts immediately when queue is empty") {
#ifndef ARDUINO
    cleanHttpOutboxSpool();
    FakeHttpTransport transport;
    FakeClock clock;
    pqueue::http::Header headers[] = {{"X-API-Key", "secret"}, {"Content-Type", "application/json"}};

    auto outbox = makeHttpOutbox(transport, clock, headers, 2);
    const auto result = outbox.submitPost("/switchbot/reading", "{\"ok\":true}");

    CHECK(result.status == pqueue::SubmitStatus::Sent);
    REQUIRE_EQ(transport.posts.size(), 1U);
    CHECK_EQ(transport.posts[0].url, "https://example.test/api/switchbot/reading");
    CHECK_EQ(transport.posts[0].body, "{\"ok\":true}");
    REQUIRE_EQ(transport.posts[0].headers.size(), 2U);
    CHECK_EQ(std::string{transport.posts[0].headers[0].name}, "X-API-Key");
    CHECK_EQ(std::string{transport.posts[0].headers[0].value}, "secret");
    CHECK_EQ(outbox.stats().count, 0U);
#endif
}

TEST_CASE("pqueue http outbox queues retryable response and drains later") {
#ifndef ARDUINO
    cleanHttpOutboxSpool();
    FakeHttpTransport transport;
    transport.responses.push_back({503, pqueue::http::TransportError::None});
    FakeClock clock;

    auto outbox = makeHttpOutbox(transport, clock);
    auto submit = outbox.submitPost("/xiaomi/reading", "body");
    CHECK(submit.status == pqueue::SubmitStatus::Queued);
    CHECK_EQ(outbox.stats().count, 1U);

    clock.nowMs += 1000;
    transport.responses.push_back({200, pqueue::http::TransportError::None});
    auto drain = outbox.drain();

    CHECK_EQ(drain.sent, 1U);
    CHECK_EQ(outbox.stats().count, 0U);
    REQUIRE_EQ(transport.posts.size(), 2U);
    CHECK_EQ(transport.posts[1].url, "https://example.test/api/xiaomi/reading");
#endif
}

TEST_CASE("pqueue http outbox drops permanent status") {
#ifndef ARDUINO
    cleanHttpOutboxSpool();
    FakeHttpTransport transport;
    transport.responses.push_back({422, pqueue::http::TransportError::None});
    FakeClock clock;

    auto outbox = makeHttpOutbox(transport, clock);
    auto submit = outbox.submitPost("/bad", "body");

    CHECK(submit.status == pqueue::SubmitStatus::Dropped);
    CHECK_EQ(outbox.stats().count, 0U);
#endif
}


TEST_CASE("pqueue http outbox notifies when a request is dropped by classification") {
#ifndef ARDUINO
    cleanHttpOutboxSpool();
    FakeHttpTransport transport;
    transport.responses.push_back({422, pqueue::http::TransportError::None});
    FakeClock clock;
    DropObserver observer;

    pqueue::http::Config httpConfig;
    httpConfig.queue.basePath = kHttpOutboxSpoolDir.string();
    httpConfig.outbox.initialRetryDelayMs = 1000;
    httpConfig.baseUrl = "https://example.test";
    httpConfig.dropContext = &observer;
    httpConfig.onDrop = onDrop;

    pqueue::http::Outbox outbox(httpConfig, transport, fakeClockNow, &clock);
    const auto submit = outbox.submitPost("/bad", "body");

    CHECK(submit.status == pqueue::SubmitStatus::Dropped);
    REQUIRE_EQ(observer.seen.size(), 1U);
    CHECK(observer.seen[0].reason == pqueue::http::DropReason::ServerRejected);
    CHECK(observer.seen[0].hasRequest);
    CHECK_EQ(observer.seen[0].path, "/bad");
    CHECK(observer.seen[0].hasResponse);
    CHECK_EQ(observer.seen[0].statusCode, 422);
#endif
}

TEST_CASE("pqueue http outbox retries 503 without max-attempt drop") {
#ifndef ARDUINO
    cleanHttpOutboxSpool();
    FakeHttpTransport transport;
    transport.responses.push_back({503, pqueue::http::TransportError::None});
    FakeClock clock;
    DropObserver observer;

    pqueue::http::Config httpConfig;
    httpConfig.queue.basePath = kHttpOutboxSpoolDir.string();
    httpConfig.outbox.initialRetryDelayMs = 1000;
    httpConfig.baseUrl = "https://example.test";
    httpConfig.dropContext = &observer;
    httpConfig.onDrop = onDrop;

    pqueue::http::Outbox outbox(httpConfig, transport, fakeClockNow, &clock);
    const auto submit = outbox.submitPost("/retry", "body");

    CHECK(submit.status == pqueue::SubmitStatus::Queued);
    CHECK(observer.seen.empty());
    CHECK_EQ(outbox.stats().count, 1U);
#endif
}

TEST_CASE("pqueue http outbox allows classifier override") {
#ifndef ARDUINO
    cleanHttpOutboxSpool();
    FakeHttpTransport transport;
    transport.responses.push_back({422, pqueue::http::TransportError::None});
    FakeClock clock;
    CustomClassifier classifier;
    classifier.decision = pqueue::SendDecision::RetryLater;

    pqueue::http::Config httpConfig;
    httpConfig.queue.basePath = kHttpOutboxSpoolDir.string();
    httpConfig.outbox.initialRetryDelayMs = 1000;
    httpConfig.baseUrl = "https://example.test";
    httpConfig.classify = customClassify;
    httpConfig.classifyContext = &classifier;

    pqueue::http::Outbox outbox(httpConfig, transport, fakeClockNow, &clock);
    auto submit = outbox.submitPost("/bad", "body");

    CHECK(submit.status == pqueue::SubmitStatus::Queued);
    REQUIRE_EQ(classifier.statuses.size(), 1U);
    CHECK_EQ(classifier.statuses[0], 422);
#endif
}

TEST_CASE("pqueue http callback transport adapts a post callback") {
#ifndef ARDUINO
    FakeHttpTransport fake;
    fake.responses.push_back({204, pqueue::http::TransportError::None});
    pqueue::http::CallbackTransport transport(callbackPost, &fake);

    const auto response = transport.post(
        "https://example.test/api",
        nullptr,
        0,
        reinterpret_cast<const std::uint8_t*>("body"),
        4
    );

    CHECK_EQ(response.statusCode, 204);
    REQUIRE_EQ(fake.posts.size(), 1U);
    CHECK_EQ(fake.posts[0].url, "https://example.test/api");
    CHECK_EQ(fake.posts[0].body, "body");
#endif
}


TEST_CASE("pqueue http outbox calls response callback with request and response") {
#ifndef ARDUINO
    cleanHttpOutboxSpool();
    FakeHttpTransport transport;
    transport.responses.push_back({201, pqueue::http::TransportError::None, "{\"status\":\"created\"}"});
    FakeClock clock;
    ResponseObserver observer;

    pqueue::http::Config httpConfig;
    httpConfig.queue.basePath = kHttpOutboxSpoolDir.string();
    httpConfig.outbox.initialRetryDelayMs = 1000;
    httpConfig.baseUrl = "https://example.test";
    httpConfig.responseContext = &observer;
    httpConfig.onResponse = onResponse;

    pqueue::http::Outbox outbox(httpConfig, transport, fakeClockNow, &clock);
    const auto result = outbox.submitPost("/reading", "body");

    CHECK(result.status == pqueue::SubmitStatus::Sent);
    REQUIRE_EQ(observer.seen.size(), 1U);
    CHECK_EQ(observer.seen[0].path, "/reading");
    CHECK_EQ(observer.seen[0].statusCode, 201);
    CHECK(observer.seen[0].error == pqueue::http::TransportError::None);
    CHECK_EQ(observer.seen[0].body, "{\"status\":\"created\"}");
#endif
}

TEST_CASE("pqueue http default classifier handles representative outcomes") {
    CHECK(pqueue::http::defaultClassifyResponse({204, pqueue::http::TransportError::None}) == pqueue::SendDecision::Sent);
    CHECK(pqueue::http::defaultClassifyResponse({408, pqueue::http::TransportError::None}) == pqueue::SendDecision::RetryLater);
    CHECK(pqueue::http::defaultClassifyResponse({429, pqueue::http::TransportError::None}) == pqueue::SendDecision::RetryLater);
    CHECK(pqueue::http::defaultClassifyResponse({500, pqueue::http::TransportError::None}) == pqueue::SendDecision::RetryLater);
    CHECK(pqueue::http::defaultClassifyResponse({502, pqueue::http::TransportError::None}) == pqueue::SendDecision::RetryLater);
    CHECK(pqueue::http::defaultClassifyResponse({507, pqueue::http::TransportError::None}) == pqueue::SendDecision::RetryLater);
    CHECK(pqueue::http::defaultClassifyResponse({511, pqueue::http::TransportError::None}) == pqueue::SendDecision::RetryLater);
    CHECK(pqueue::http::defaultClassifyResponse({pqueue::http::kNoStatusCode, pqueue::http::TransportError::Timeout}) == pqueue::SendDecision::RetryLater);
    CHECK(pqueue::http::defaultClassifyResponse({404, pqueue::http::TransportError::None}) == pqueue::SendDecision::Drop);
    CHECK(pqueue::http::defaultClassifyResponse({501, pqueue::http::TransportError::None}) == pqueue::SendDecision::Drop);
    CHECK(pqueue::http::defaultClassifyResponse({505, pqueue::http::TransportError::None}) == pqueue::SendDecision::Drop);
    CHECK(pqueue::http::defaultClassifyResponse({508, pqueue::http::TransportError::None}) == pqueue::SendDecision::Drop);
}

TEST_CASE("pqueue http outbox drains queued backlog after transport recovers") {
#ifndef ARDUINO
    cleanHttpOutboxSpool();
    FakeHttpTransport transport;
    transport.responses.push_back({pqueue::http::kNoStatusCode, pqueue::http::TransportError::Network});
    FakeClock clock;

    auto outbox = makeHttpOutbox(transport, clock);
    CHECK(outbox.submitPost("/first", "one").status == pqueue::SubmitStatus::Queued);
    CHECK(outbox.submitPost("/second", "two").status == pqueue::SubmitStatus::Queued);
    CHECK(outbox.submitPost("/third", "three").status == pqueue::SubmitStatus::Queued);
    CHECK_EQ(outbox.stats().count, 3U);
    REQUIRE_EQ(transport.posts.size(), 1U);
    CHECK_EQ(transport.posts[0].url, "https://example.test/api/first");

    clock.nowMs += 1000;
    auto drain = outbox.drain();
    CHECK_EQ(drain.sent, 1U);
    CHECK_FALSE(drain.notDue);
    CHECK_FALSE(drain.rateLimited);
    CHECK_EQ(outbox.stats().count, 2U);

    clock.nowMs += 1000;
    drain = outbox.drain();
    CHECK_EQ(drain.sent, 1U);
    CHECK_EQ(outbox.stats().count, 1U);

    clock.nowMs += 1000;
    drain = outbox.drain();
    CHECK_EQ(drain.sent, 1U);
    CHECK_EQ(outbox.stats().count, 0U);

    REQUIRE_EQ(transport.posts.size(), 4U);
    CHECK_EQ(transport.posts[1].url, "https://example.test/api/first");
    CHECK_EQ(transport.posts[2].url, "https://example.test/api/second");
    CHECK_EQ(transport.posts[3].url, "https://example.test/api/third");
#endif
}

TEST_CASE("pqueue http outbox does not extend front retry cooldown when more requests are queued") {
#ifndef ARDUINO
    cleanHttpOutboxSpool();
    FakeHttpTransport transport;
    transport.responses.push_back({pqueue::http::kNoStatusCode, pqueue::http::TransportError::Network});
    FakeClock clock;

    auto outbox = makeHttpOutbox(transport, clock);
    REQUIRE(outbox.submitPost("/first", "one").status == pqueue::SubmitStatus::Queued);

    clock.nowMs += 250;
    CHECK(outbox.submitPost("/second", "two").status == pqueue::SubmitStatus::Queued);
    clock.nowMs += 250;
    CHECK(outbox.submitPost("/third", "three").status == pqueue::SubmitStatus::Queued);

    clock.nowMs = 1999;
    auto drain = outbox.drain();
    CHECK(drain.notDue);
    CHECK_EQ(transport.posts.size(), 1U);

    clock.nowMs = 2000;
    drain = outbox.drain();
    CHECK_EQ(drain.sent, 1U);
    CHECK_FALSE(drain.notDue);
    CHECK_EQ(outbox.stats().count, 2U);
    REQUIRE_EQ(transport.posts.size(), 2U);
    CHECK_EQ(transport.posts[1].url, "https://example.test/api/first");
#endif
}

TEST_CASE("pqueue http outbox keeps retryable front request instead of dropping after max attempts") {
#ifndef ARDUINO
    cleanHttpOutboxSpool();
    FakeHttpTransport transport;
    transport.responses.push_back({pqueue::http::kNoStatusCode, pqueue::http::TransportError::Network});
    transport.responses.push_back({pqueue::http::kNoStatusCode, pqueue::http::TransportError::Network});
    FakeClock clock;

    pqueue::http::Config httpConfig;
    httpConfig.queue.basePath = kHttpOutboxSpoolDir.string();
    // initial=500 so delays are 500ms, 1000ms, 2000ms... fits within +1000ms clock steps.
    httpConfig.outbox.initialRetryDelayMs = 500;
    httpConfig.outbox.maxDrainAttemptsPerSecond = 1;
    httpConfig.baseUrl = "https://example.test/api";

    pqueue::http::Outbox outbox(httpConfig, transport, fakeClockNow, &clock);
    CHECK(outbox.submitPost("/first", "one").status == pqueue::SubmitStatus::Queued);
    CHECK(outbox.submitPost("/second", "two").status == pqueue::SubmitStatus::Queued);
    CHECK(outbox.submitPost("/third", "three").status == pqueue::SubmitStatus::Queued);
    CHECK_EQ(outbox.stats().count, 3U);

    // Attempt 0 delay = 500ms; cooldown expires at clock=1500. Due at +1000ms.
    clock.nowMs += 1000;
    auto drain = outbox.drain();
    CHECK_EQ(drain.sent, 0U);
    CHECK_EQ(outbox.stats().count, 3U);

    // Attempt 1 delay = 1000ms; cooldown expires at clock=3000. Due at +1000ms.
    clock.nowMs += 1000;
    drain = outbox.drain();
    CHECK_EQ(drain.sent, 1U);
    CHECK_EQ(outbox.stats().count, 2U);

    clock.nowMs += 1000;
    drain = outbox.drain();
    CHECK_EQ(drain.sent, 1U);
    CHECK_EQ(outbox.stats().count, 1U);

    REQUIRE_EQ(transport.posts.size(), 4U);
    CHECK_EQ(transport.posts[0].url, "https://example.test/api/first");
    CHECK_EQ(transport.posts[1].url, "https://example.test/api/first");
    CHECK_EQ(transport.posts[2].url, "https://example.test/api/first");
    CHECK_EQ(transport.posts[3].url, "https://example.test/api/second");
#endif
}

TEST_CASE("pqueue http outbox drainUpTo sends multiple queued requests within rate cap") {
#ifndef ARDUINO
    cleanHttpOutboxSpool();
    FakeHttpTransport transport;
    transport.responses.push_back({pqueue::http::kNoStatusCode, pqueue::http::TransportError::Network});
    FakeClock clock;

    pqueue::http::Config httpConfig;
    httpConfig.queue.basePath = kHttpOutboxSpoolDir.string();
    httpConfig.outbox.initialRetryDelayMs = 0;
    httpConfig.outbox.maxDrainAttemptsPerSecond = 3;
    httpConfig.baseUrl = "https://example.test/api";

    pqueue::http::Outbox outbox(httpConfig, transport, fakeClockNow, &clock);
    REQUIRE(outbox.submitPost("/one", "one").status == pqueue::SubmitStatus::Queued);
    REQUIRE(outbox.submitPost("/two", "two").status == pqueue::SubmitStatus::Queued);
    REQUIRE(outbox.submitPost("/three", "three").status == pqueue::SubmitStatus::Queued);
    REQUIRE(outbox.submitPost("/four", "four").status == pqueue::SubmitStatus::Queued);

    const auto first = outbox.drainUpTo(3);
    CHECK_EQ(first.attempts, 3U);
    CHECK_EQ(first.sent, 3U);
    CHECK_GT(first.removedQueuedBytes, 0U);
    CHECK_FALSE(first.rateLimited);
    CHECK_EQ(outbox.stats().count, 1U);

    REQUIRE_EQ(transport.posts.size(), 4U);
    CHECK_EQ(transport.posts[1].url, "https://example.test/api/one");
    CHECK_EQ(transport.posts[2].url, "https://example.test/api/two");
    CHECK_EQ(transport.posts[3].url, "https://example.test/api/three");
#endif
}

TEST_CASE("pqueue http outbox validate accepts queued request envelopes") {
#ifndef ARDUINO
    cleanHttpOutboxSpool();
    FakeHttpTransport transport;
    transport.responses.push_back({503, pqueue::http::TransportError::None});
    FakeClock clock;

    auto outbox = makeHttpOutbox(transport, clock);
    REQUIRE(outbox.submitPost("/switchbot/reading", "{\"ok\":true}").status == pqueue::SubmitStatus::Queued);

    const auto result = outbox.validate();

    CHECK(result.ok);
    CHECK(result.errors.empty());
#endif
}

TEST_CASE("pqueue http outbox validate rejects malformed request envelopes") {
#ifndef ARDUINO
    cleanHttpOutboxSpool();
    std::string encodedOutbox;
    REQUIRE(pqueue::envelope::encodeEnvelope(0, "not an HTTP request envelope", encodedOutbox));
    {
        pqueue::Config queueConfig;
        queueConfig.basePath = kHttpOutboxSpoolDir.string();
        pqueue::Queue queue(queueConfig);
        REQUIRE(queue.enqueue(encodedOutbox).ok());
    }

    FakeHttpTransport transport;
    FakeClock clock;
    auto outbox = makeHttpOutbox(transport, clock);

    const auto result = outbox.validate();

    REQUIRE_FALSE(result.ok);
    REQUIRE_EQ(result.errors.size(), 1U);
    CHECK(result.errors[0].code == pqueue::ValidationIssueCode::HttpRequestEnvelopeInvalid);
#endif
}


TEST_CASE("pqueue http outbox compactIdle removes dead sealed segments") {
#ifndef ARDUINO
    cleanHttpOutboxSpool();
    // path="/t" body="b": request envelope=15 bytes, outbox envelope=29 bytes, per-record segment cost=53 bytes.
    // maxSegmentBytes=100 holds exactly 1 record per sealed segment (20+53=73 < 100, 20+106=126 > 100).
    pqueue::http::Config httpConfig;
    httpConfig.queue.basePath = kHttpOutboxSpoolDir.string();
    httpConfig.queue.maxSegmentBytes = 100;
    httpConfig.queue.minFreeBytes = 0;
    httpConfig.outbox.initialRetryDelayMs = 1000;
    httpConfig.outbox.maxDrainAttemptsPerSecond = 1;
    httpConfig.baseUrl = "https://example.test";

    FakeHttpTransport transport;
    FakeClock clock;
    // submitPost triggers a live send attempt for the first record; 503 queues it.
    transport.responses.push_back({503, pqueue::http::TransportError::None});

    pqueue::http::Outbox outbox(httpConfig, transport, fakeClockNow, &clock);
    REQUIRE(outbox.submitPost("/t", "b").status == pqueue::SubmitStatus::Queued);
    REQUIRE(outbox.submitPost("/t", "b").status == pqueue::SubmitStatus::Queued);
    REQUIRE(outbox.submitPost("/t", "b").status == pqueue::SubmitStatus::Queued);
    REQUIRE(outbox.submitPost("/t", "b").status == pqueue::SubmitStatus::Queued);

    // Drain r0 and r1 into separate 1-second rate windows.
    clock.nowMs += 1000;
    transport.responses.push_back({200, pqueue::http::TransportError::None});
    outbox.drain();
    clock.nowMs += 1000;
    transport.responses.push_back({200, pqueue::http::TransportError::None});
    outbox.drain();
    CHECK_EQ(outbox.stats().count, 2U);

    // Two sealed segments are now fully dead; compactIdle should do real work.
    const auto result = outbox.compactIdle(16);
    CHECK(result.status.ok());
    CHECK(result.compactions > 0);
    CHECK(result.noOps <= 1);
    CHECK_GT(result.deadBytesBefore, 0U);
    CHECK_LT(result.remainingDeadBytes, result.deadBytesBefore);
    CHECK_GT(result.bytesReclaimed, 0U);
    CHECK_GT(result.inputSegments, 0U);
    CHECK_LE(result.outputSegments, result.inputSegments);

    // Remaining records drain cleanly.
    clock.nowMs += 1000;
    transport.responses.push_back({200, pqueue::http::TransportError::None});
    outbox.drain();
    clock.nowMs += 1000;
    transport.responses.push_back({200, pqueue::http::TransportError::None});
    outbox.drain();
    CHECK_EQ(outbox.stats().count, 0U);
    cleanHttpOutboxSpool();
#endif
}

TEST_CASE("pqueue http outbox permanently drops 501/505/508 server capability errors") {
#ifndef ARDUINO
    for (const int status : {501, 505, 508}) {
        cleanHttpOutboxSpool();
        FakeHttpTransport transport;
        transport.responses.push_back({status, pqueue::http::TransportError::None});
        FakeClock clock;

        auto outbox = makeHttpOutbox(transport, clock);
        const auto submit = outbox.submitPost("/test", "body");

        CHECK(submit.status == pqueue::SubmitStatus::Dropped);
        CHECK_EQ(outbox.stats().count, 0U);
    }
#endif
}

TEST_CASE("pqueue http outbox uses Retry-After hint from transport response") {
#ifndef ARDUINO
    cleanHttpOutboxSpool();
    FakeHttpTransport transport;
    // 503 with a 3000ms Retry-After hint.
    transport.responses.push_back({503, pqueue::http::TransportError::None, {}, 3000});
    FakeClock clock;

    pqueue::http::Config httpConfig;
    httpConfig.queue.basePath = kHttpOutboxSpoolDir.string();
    httpConfig.outbox.initialRetryDelayMs = 1000;  // computed backoff would be 1000ms
    httpConfig.outbox.maxRetryDelayMs = 60000;
    httpConfig.outbox.maxDrainAttemptsPerSecond = 100;
    httpConfig.baseUrl = "https://example.test";

    pqueue::http::Outbox outbox(httpConfig, transport, fakeClockNow, &clock);
    REQUIRE(outbox.submitPost("/test", "body").status == pqueue::SubmitStatus::Queued);
    // Hint of 3000ms overrides computed 1000ms. Cooldown at clock=4000.

    clock.nowMs += 2999;
    CHECK(outbox.drain().notDue);   // would be due at 2000 with computed backoff

    clock.nowMs += 1;               // clock=4000: due
    transport.responses.push_back({200, pqueue::http::TransportError::None});
    CHECK_EQ(outbox.drain().sent, 1U);
#endif
}

TEST_CASE("pqueue http outbox caps Retry-After hint at maxRetryDelayMs") {
#ifndef ARDUINO
    cleanHttpOutboxSpool();
    FakeHttpTransport transport;
    // 429 with a very long Retry-After hint.
    transport.responses.push_back({429, pqueue::http::TransportError::None, {}, 60000});
    FakeClock clock;

    pqueue::http::Config httpConfig;
    httpConfig.queue.basePath = kHttpOutboxSpoolDir.string();
    httpConfig.outbox.initialRetryDelayMs = 1000;
    httpConfig.outbox.maxRetryDelayMs = 2000;
    httpConfig.outbox.maxDrainAttemptsPerSecond = 100;
    httpConfig.baseUrl = "https://example.test";

    pqueue::http::Outbox outbox(httpConfig, transport, fakeClockNow, &clock);
    REQUIRE(outbox.submitPost("/test", "body").status == pqueue::SubmitStatus::Queued);
    // Hint of 60000ms capped at maxRetryDelayMs=2000. Cooldown at 3000.

    clock.nowMs += 1999;
    CHECK(outbox.drain().notDue);

    clock.nowMs += 1;               // clock=3000: due; uncapped hint would set 61000
    transport.responses.push_back({200, pqueue::http::TransportError::None});
    CHECK_EQ(outbox.drain().sent, 1U);
#endif
}

TEST_CASE("pqueue http default classifier covers retryable and permanent statuses") {
#ifndef ARDUINO
    const int retryableStatuses[] = {408, 429, 500, 502, 503, 504, 599};
    for (const int status : retryableStatuses) {
        CHECK(pqueue::http::defaultClassifyResponse({status, pqueue::http::TransportError::None}) == pqueue::SendDecision::RetryLater);
    }

    const int permanentStatuses[] = {300, 301, 400, 401, 404, 409, 413, 422, 499, 501, 505, 508};
    for (const int status : permanentStatuses) {
        CHECK(pqueue::http::defaultClassifyResponse({status, pqueue::http::TransportError::None}) == pqueue::SendDecision::Drop);
    }

    CHECK(pqueue::http::defaultClassifyResponse({200, pqueue::http::TransportError::None}) == pqueue::SendDecision::Sent);
    CHECK(pqueue::http::defaultClassifyResponse({204, pqueue::http::TransportError::None}) == pqueue::SendDecision::Sent);
    CHECK(pqueue::http::defaultClassifyResponse({pqueue::http::kNoStatusCode, pqueue::http::TransportError::Network}) == pqueue::SendDecision::RetryLater);
    CHECK(pqueue::http::defaultClassifyResponse({pqueue::http::kNoStatusCode, pqueue::http::TransportError::Tls}) == pqueue::SendDecision::RetryLater);
#endif
}

#include "pqueue/queue.h"
#include "pqueue/append_log_store.h"

#include "doctest/doctest.h"

#ifndef ARDUINO
#include <filesystem>
#include <string>
#include <vector>
#endif

namespace {

#ifndef ARDUINO
const std::filesystem::path kSpoolDir = "build/pqueue-spools/pqueue_append_log_spool";

void cleanSpool() {
    std::error_code ec;
    std::filesystem::remove_all(kSpoolDir, ec);
}

pqueue::Config makeConfig() {
    pqueue::Config cfg;
    cfg.basePath = kSpoolDir.string();
    cfg.storeLayout = pqueue::StoreLayout::AppendLog;
    cfg.recordSizeBytes = 256;
    cfg.reservedBytes = 64 * 1024;
    cfg.maxSegmentBytes = 1024; // small to force rotation in tests
    return cfg;
}
#endif

} // namespace

TEST_CASE("append-log: starts empty") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Queue q(makeConfig());
    std::string out;
    CHECK_FALSE(q.peek(out).ok());
    CHECK_EQ(q.stats().count, 0U);
#endif
}

TEST_CASE("append-log: basic enqueue and peek") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Queue q(makeConfig());
    CHECK(q.enqueue("hello").ok());
    std::string out;
    CHECK(q.peek(out).ok());
    CHECK_EQ(out, "hello");
    CHECK_EQ(q.stats().count, 1U);
#endif
}

TEST_CASE("append-log: FIFO order preserved") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Queue q(makeConfig());
    CHECK(q.enqueue("one").ok());
    CHECK(q.enqueue("two").ok());
    CHECK(q.enqueue("three").ok());
    CHECK_EQ(q.stats().count, 3U);

    std::string out;
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "one");
    CHECK(q.pop().ok());
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "two");
    CHECK(q.pop().ok());
    CHECK(q.peek(out).ok()); CHECK_EQ(out, "three");
    CHECK(q.pop().ok());
    CHECK_EQ(q.stats().count, 0U);
#endif
}

TEST_CASE("append-log: persistence across remount") {
#ifndef ARDUINO
    cleanSpool();
    auto cfg = makeConfig();
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("alpha").ok());
        CHECK(q.enqueue("beta").ok());
    }
    {
        pqueue::Queue q(cfg);
        std::string out;
        CHECK(q.peek(out).ok());
        CHECK_EQ(out, "alpha");
        CHECK(q.pop().ok());
        CHECK(q.peek(out).ok());
        CHECK_EQ(out, "beta");
        CHECK(q.pop().ok());
        CHECK_EQ(q.stats().count, 0U);
    }
#endif
}

TEST_CASE("append-log: pop persists across remount") {
#ifndef ARDUINO
    cleanSpool();
    auto cfg = makeConfig();
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("keep").ok());
        CHECK(q.enqueue("discard").ok());
        CHECK(q.pop().ok()); // FIFO: pops "keep", "discard" remains
    }
    {
        pqueue::Queue q(cfg);
        std::string out;
        CHECK(q.peek(out).ok());
        CHECK_EQ(out, "discard");
        CHECK_EQ(q.stats().count, 1U);
    }
#endif
}

TEST_CASE("append-log: segment rotation") {
#ifndef ARDUINO
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 256; // very small to force rotation
    pqueue::Queue q(cfg);

    // Enqueue enough records to force multiple segment rotations
    for (int i = 0; i < 20; ++i) {
        CHECK(q.enqueue("record-" + std::to_string(i)).ok());
    }
    CHECK_EQ(q.stats().count, 20U);

    // Verify all records are readable
    for (int i = 0; i < 20; ++i) {
        std::string out;
        CHECK(q.peek(out).ok());
        CHECK_EQ(out, "record-" + std::to_string(i));
        CHECK(q.pop().ok());
    }
    CHECK_EQ(q.stats().count, 0U);
#endif
}

TEST_CASE("append-log: segment rotation persists") {
#ifndef ARDUINO
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 256;
    {
        pqueue::Queue q(cfg);
        for (int i = 0; i < 10; ++i) {
            CHECK(q.enqueue("msg-" + std::to_string(i)).ok());
        }
    }
    {
        pqueue::Queue q(cfg);
        CHECK_EQ(q.stats().count, 10U);
        for (int i = 0; i < 10; ++i) {
            std::string out;
            CHECK(q.peek(out).ok());
            CHECK_EQ(out, "msg-" + std::to_string(i));
            CHECK(q.pop().ok());
        }
    }
#endif
}

TEST_CASE("append-log: rewriteFront persists") {
#ifndef ARDUINO
    cleanSpool();
    auto cfg = makeConfig();
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("original").ok());
        CHECK(q.rewriteFront("updated").ok());
    }
    {
        pqueue::Queue q(cfg);
        std::string out;
        CHECK(q.peek(out).ok());
        CHECK_EQ(out, "updated");
    }
#endif
}

TEST_CASE("append-log: format clears all records") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Queue q(makeConfig());
    CHECK(q.enqueue("a").ok());
    CHECK(q.enqueue("b").ok());
    CHECK(q.format().ok());
    CHECK_EQ(q.stats().count, 0U);
    std::string out;
    CHECK_FALSE(q.peek(out).ok());
#endif
}

TEST_CASE("append-log: format then enqueue works") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Queue q(makeConfig());
    CHECK(q.enqueue("before").ok());
    CHECK(q.format().ok());
    CHECK(q.enqueue("after").ok());
    std::string out;
    CHECK(q.peek(out).ok());
    CHECK_EQ(out, "after");
#endif
}

TEST_CASE("append-log: validate returns ok for clean store") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Queue q(makeConfig());
    CHECK(q.enqueue("x").ok());
    const auto result = q.validate();
    CHECK(result.ok);
#endif
}

TEST_CASE("append-log: validate on empty store") {
#ifndef ARDUINO
    cleanSpool();
    pqueue::Queue q(makeConfig());
    const auto result = q.validate();
    CHECK(result.ok);
#endif
}

TEST_CASE("append-log: compaction triggered by segment count") {
#ifndef ARDUINO
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 128;
    cfg.maxSegments = 3;
    pqueue::Queue q(cfg);

    // Enqueue enough to trigger many rotations and compaction
    std::vector<std::string> expected;
    for (int i = 0; i < 30; ++i) {
        const std::string rec = "record-" + std::to_string(i);
        CHECK(q.enqueue(rec).ok());
        expected.push_back(rec);
    }
    CHECK_EQ(q.stats().count, 30U);

    // All records should still be readable
    for (int i = 0; i < 30; ++i) {
        std::string out;
        CHECK(q.peek(out).ok());
        CHECK_EQ(out, expected[i]);
        CHECK(q.pop().ok());
    }
#endif
}

TEST_CASE("append-log: mixed enqueue and pop with persistence") {
#ifndef ARDUINO
    cleanSpool();
    auto cfg = makeConfig();
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("a").ok());
        CHECK(q.enqueue("b").ok());
        CHECK(q.enqueue("c").ok());
        CHECK(q.pop().ok()); // remove "a"
    }
    {
        pqueue::Queue q(cfg);
        CHECK_EQ(q.stats().count, 2U);
        std::string out;
        CHECK(q.peek(out).ok());
        CHECK_EQ(out, "b");
        CHECK(q.pop().ok());
        CHECK(q.peek(out).ok());
        CHECK_EQ(out, "c");
    }
#endif
}

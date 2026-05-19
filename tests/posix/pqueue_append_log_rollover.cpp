#include "pqueue_append_log_support.h"

using namespace pqueue::append_log_detail;

TEST_CASE("rollover: manifest published after each rotation (critical)") {
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 128;

    constexpr int kCount = 20;
    {
        pqueue::Queue q(cfg);
        for (int i = 0; i < kCount; ++i)
            CHECK(q.enqueue("item-" + std::to_string(i)).ok());
        CHECK_EQ(q.stats().count, static_cast<std::uint32_t>(kCount));
    }
    {
        pqueue::Queue q(cfg);
        CHECK_EQ(q.stats().count, static_cast<std::uint32_t>(kCount));
        for (int i = 0; i < kCount; ++i) {
            std::string out;
            CHECK(q.peek(out).ok()); CHECK_EQ(out, "item-" + std::to_string(i));
            CHECK(q.pop().ok());
        }
        CHECK_EQ(q.stats().count, 0U);
    }
}

TEST_CASE("rollover: range limit exceeded returns failure") {
    // Verify that rotateSegment() fails cleanly when promoting the current tail
    // would push the range count past kManifestMaxRanges (4).
    // Setup: plant a manifest with 4 non-contiguous full ranges and tail=9.
    const std::uint32_t maxSeg =
        kSegmentHeaderBytes + kEnqueueHeaderBytes + 1 + kEventTrailerBytes;

    plantLayout({
        .ranges = {{1,1},{3,3},{5,5},{7,7}}, .tail = 9, .next = 10,
        .segments = {
            {.gen=1,.firstSeq=0,.body={}},{.gen=3,.firstSeq=0,.body={}},
            {.gen=5,.firstSeq=0,.body={}},{.gen=7,.firstSeq=0,.body={}},
            {.gen=9,.firstSeq=0,.body={}},
        },
    });

    auto storeCfg = makeStoreConfig();
    storeCfg.maxSegmentBytes = maxSeg;

    pqueue::AppendLogStore store(storeCfg);
    REQUIRE(store.mount().ok());

    pqueue::FileStoreIndex dummy{};
    REQUIRE(store.writeRecord(0, "x").ok());
    REQUIRE(store.writeIndex(dummy).ok()); // seg 9 now full

    const auto st = store.writeRecord(1, "y"); // triggers rotateSegment() → limit exceeded
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::RangeLimitExceeded);

    CHECK_FALSE(std::filesystem::exists(segmentPath(10)));

    ManifestData onDisk;
    REQUIRE(readManifestSlot('a', onDisk));
    CHECK_EQ(onDisk.tailGeneration, 9U);
    REQUIRE_EQ(onDisk.ranges.size(), 4U);
    CHECK_EQ(onDisk.ranges[0].startGen, 1U); CHECK_EQ(onDisk.ranges[0].endGen, 1U);
    CHECK_EQ(onDisk.ranges[3].startGen, 7U); CHECK_EQ(onDisk.ranges[3].endGen, 7U);
}


TEST_CASE("rollover: dangling new segment ignored when publish fails (critical)") {
    // If the segment file for the new tail is written but the manifest publish fails,
    // the old tail must remain authoritative on remount and the dangling segment ignored.
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = kSegmentHeaderBytes + kEnqueueHeaderBytes + 1 + kEventTrailerBytes;

    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("A").ok());
    }
    REQUIRE(std::filesystem::exists(segmentPath(1)));
    {
        ManifestData md;
        REQUIRE(readManifestSlot('a', md));
        CHECK_EQ(md.tailGeneration, 1U);
    }

    // Simulate crash: write seg 2 without updating the manifest.
    plantSegment(2, 1);

    {
        pqueue::Queue q(cfg);
        CHECK_EQ(q.stats().count, 1U);
        std::string out;
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "A");
    }
}

TEST_CASE("rollover: failed manifest publish does not poison live object (critical)") {
    // If the manifest write fails, the live object must not believe the new segment
    // is the active tail. Subsequent writes must succeed and remount must be clean.
    resetSpool();

    auto inner = pqueue::makePosixFileSystem();
    auto faultFs = std::make_shared<FaultInjectingFs>(inner);

    pqueue::AppendLogConfig cfg = makeStoreConfig();
    cfg.fileSystem = faultFs;

    {
        pqueue::AppendLogStore store(cfg);
        REQUIRE(store.mount().ok());

        faultFs->failNextWriteFileTo = "manifest";
        const auto st = store.writeRecord(0, "hello");
        CHECK_FALSE(st.ok());

        pqueue::FileStoreIndex dummy{};
        REQUIRE(store.writeRecord(0, "hello").ok());
        REQUIRE(store.writeIndex(dummy).ok());
    }

    {
        pqueue::AppendLogStore store2(makeStoreConfig());
        REQUIRE(store2.mount().ok());
        pqueue::FileStoreIndex idx;
        REQUIRE(store2.readIndex(idx).ok());
        CHECK_EQ(idx.count, 1U);
        std::string out;
        REQUIRE(store2.readRecord(0, out).ok()); CHECK_EQ(out, "hello");
    }
}

TEST_CASE("rollover: failed manifest publish during rotation does not poison live object (critical)") {
    // Same RAM-before-manifest bug, exercised through rotateSegment().
    // Write A into seg 1 (full). Inject manifest failure on next write so rotation to
    // seg 2 fails during publishManifest(). Clear fault, retry B — must succeed.
    // Fresh remount must find A then B.
    resetSpool();

    const std::uint32_t maxSeg = kSegmentHeaderBytes + kEnqueueOverheadBytes + 1;

    auto inner = pqueue::makePosixFileSystem();
    auto faultFs = std::make_shared<FaultInjectingFs>(inner);

    pqueue::AppendLogConfig cfg = makeStoreConfig();
    cfg.maxSegmentBytes = maxSeg;
    cfg.fileSystem = faultFs;

    {
        pqueue::AppendLogStore store(cfg);
        REQUIRE(store.mount().ok());

        pqueue::FileStoreIndex dummy{};
        REQUIRE(store.writeRecord(0, "A").ok());
        REQUIRE(store.writeIndex(dummy).ok()); // seg 1 full

        faultFs->failNextWriteFileTo = "manifest";
        const auto st = store.writeRecord(1, "B"); // rotation fails
        CHECK_FALSE(st.ok());

        REQUIRE(store.writeRecord(1, "B").ok()); // retry succeeds
        REQUIRE(store.writeIndex(dummy).ok());
    }

    {
        pqueue::AppendLogConfig cfg2 = makeStoreConfig();
        cfg2.maxSegmentBytes = maxSeg;
        pqueue::AppendLogStore store2(cfg2);
        REQUIRE(store2.mount().ok());
        pqueue::FileStoreIndex idx;
        REQUIRE(store2.readIndex(idx).ok());
        CHECK_EQ(idx.count, 2U);
        std::string out;
        REQUIRE(store2.readRecord(0, out).ok()); CHECK_EQ(out, "A");
        REQUIRE(store2.readRecord(1, out).ok()); CHECK_EQ(out, "B");
    }
}

TEST_CASE("rollover: merged range [1,2] replays both segments in FIFO order") {
    // After two rotations the manifest stores a merged range [1,2]. Verify
    // on-disk manifest structure and that remount replays both segments in FIFO order.
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = kSegmentHeaderBytes + kEnqueueHeaderBytes + 1 + kEventTrailerBytes;

    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("A").ok());
        CHECK(q.enqueue("B").ok());
        CHECK(q.enqueue("C").ok());

        ManifestData md;
        REQUIRE((readManifestSlot('a', md) || readManifestSlot('b', md)));
        ManifestData mdB;
        if (readManifestSlot('b', mdB) && mdB.epoch > md.epoch) md = mdB;
        REQUIRE_EQ(md.ranges.size(), 1U);
        CHECK_EQ(md.ranges[0].startGen, 1U);
        CHECK_EQ(md.ranges[0].endGen,   2U);
        CHECK_EQ(md.tailGeneration, 3U);
    }
    {
        pqueue::Queue q(cfg);
        CHECK_EQ(q.stats().count, 3U);
        std::string out;
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "A"); CHECK(q.pop().ok());
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "B"); CHECK(q.pop().ok());
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "C"); CHECK(q.pop().ok());
        CHECK_EQ(q.stats().count, 0U);
    }
}


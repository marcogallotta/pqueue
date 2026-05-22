#include "pqueue_append_log_support.h"

#include <functional>

using namespace pqueue::append_log_detail;

namespace {
void recomputeManifestCrc(std::vector<uint8_t>& buf) {
    const std::size_t headerBytes = std::size_t(buf[6]) | std::size_t(buf[7]) << 8;
    const std::size_t crcOffset   = headerBytes - 8;
    const uint32_t newCrc = crc32(0, buf.data(), crcOffset);
    buf[crcOffset + 0] = static_cast<uint8_t>((newCrc      ) & 0xFF);
    buf[crcOffset + 1] = static_cast<uint8_t>((newCrc >>  8) & 0xFF);
    buf[crcOffset + 2] = static_cast<uint8_t>((newCrc >> 16) & 0xFF);
    buf[crcOffset + 3] = static_cast<uint8_t>((newCrc >> 24) & 0xFF);
}
} // namespace

TEST_CASE("manifest: round-trip empty store (tailGen=0, no ranges)") {
    ManifestData m;
    m.epoch = 1; m.nextGeneration = 1; m.tailGeneration = 0;

    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);
    CHECK_EQ(buf.size(), kManifestFixedBytes);

    ManifestData out;
    CHECK(parseManifest(buf.data(), buf.size(), out));
    CHECK_EQ(out.epoch,          m.epoch);
    CHECK_EQ(out.nextGeneration, m.nextGeneration);
    CHECK_EQ(out.tailGeneration, m.tailGeneration);
    CHECK(out.ranges.empty());
}

TEST_CASE("manifest: round-trip with two ranges") {
    ManifestData m;
    m.epoch = 7; m.nextGeneration = 10; m.tailGeneration = 9;
    m.ranges = {{1, 3}, {5, 7}};

    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);
    CHECK_EQ(buf.size(), std::size_t(kManifestFixedBytes + 2 * 8));

    ManifestData out;
    CHECK(parseManifest(buf.data(), buf.size(), out));
    CHECK_EQ(out.epoch,          m.epoch);
    CHECK_EQ(out.nextGeneration, m.nextGeneration);
    CHECK_EQ(out.tailGeneration, m.tailGeneration);
    REQUIRE_EQ(out.ranges.size(), 2u);
    CHECK_EQ(out.ranges[0].startGen, 1u); CHECK_EQ(out.ranges[0].endGen, 3u);
    CHECK_EQ(out.ranges[1].startGen, 5u); CHECK_EQ(out.ranges[1].endGen, 7u);
}

TEST_CASE("manifest: binary layout field offsets") {
    ManifestData m;
    m.epoch = 0x11223344; m.nextGeneration = 0xAABBCCDD; m.tailGeneration = 0x55667788;

    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);
    REQUIRE_EQ(buf.size(), std::size_t(30));

    CHECK_EQ(buf[0], 0x50u); CHECK_EQ(buf[1], 0x51u); // magic "PQMF"
    CHECK_EQ(buf[2], 0x4Du); CHECK_EQ(buf[3], 0x46u);
    CHECK_EQ(buf[4], 0x01u); CHECK_EQ(buf[5], 0x00u); // version = 1
    CHECK_EQ(buf[6], 0x1Eu); CHECK_EQ(buf[7], 0x00u); // headerBytes = 30
    CHECK_EQ(buf[8],  0x44u); CHECK_EQ(buf[9],  0x33u); // epoch
    CHECK_EQ(buf[10], 0x22u); CHECK_EQ(buf[11], 0x11u);
    CHECK_EQ(buf[12], 0xDDu); CHECK_EQ(buf[13], 0xCCu); // nextGeneration
    CHECK_EQ(buf[14], 0xBBu); CHECK_EQ(buf[15], 0xAAu);
    CHECK_EQ(buf[16], 0x00u); CHECK_EQ(buf[17], 0x00u); // rangeCount = 0
    CHECK_EQ(buf[18], 0x88u); CHECK_EQ(buf[19], 0x77u); // tailGeneration
    CHECK_EQ(buf[20], 0x66u); CHECK_EQ(buf[21], 0x55u);
    CHECK_EQ(buf[26], 0x50u); CHECK_EQ(buf[27], 0x4Fu); // footer "POK!"
    CHECK_EQ(buf[28], 0x4Bu); CHECK_EQ(buf[29], 0x21u);
}

TEST_CASE("manifest: corrupted CRC is rejected") {
    ManifestData m;
    m.epoch = 3; m.nextGeneration = 5; m.tailGeneration = 4;
    m.ranges = {{1, 3}};

    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);
    CHECK(buf.size() == std::size_t(kManifestFixedBytes + 8));
    buf[buf.size() - 8] ^= 0xFF; // flip a byte in the CRC field

    ManifestData out;
    CHECK_FALSE(parseManifest(buf.data(), buf.size(), out));
}

TEST_CASE("manifest: structural byte violations are rejected") {
    ManifestData base;
    base.epoch = 1; base.nextGeneration = 1; base.tailGeneration = 0;
    auto run = [&](const char* label, auto mutate) {
        CAPTURE(label);
        std::vector<uint8_t> buf;
        serialiseManifest(base, buf);
        mutate(buf);
        ManifestData out;
        CHECK_FALSE(parseManifest(buf.data(), buf.size(), out));
    };
    run("wrong magic",       [](auto& b){ b[0] ^= 0xFF; });
    run("wrong version",     [](auto& b){ b[4] = 0x02; });
    run("wrong headerBytes", [](auto& b){ b[6] = 0xFF; });
    run("wrong footer",      [](auto& b){ b[b.size()-1] ^= 0xFF; });
    run("rangeCount > 4",    [](auto& b){ b[16] = 0x05; });
    run("buffer too small",  [](auto& b){ b.pop_back(); });
    run("trailing bytes",    [](auto& b){ b.push_back(0x00); });
}

TEST_CASE("manifest: semantic field violations are rejected") {
    // CRC is recomputed so the parser reaches semantic checks.
    // 1-range manifest byte offsets: tailGen=[26..29], startGen=[18..21], endGen=[22..25].
    // base.ranges = {{3,5}} so that endGen=1 < startGen=3 in the last case.
    ManifestData base;
    base.epoch = 2; base.nextGeneration = 5; base.tailGeneration = 4;
    base.ranges = {{3, 5}};
    auto run = [&](const char* label, auto mutate) {
        CAPTURE(label);
        std::vector<uint8_t> buf;
        serialiseManifest(base, buf);
        mutate(buf);
        ManifestData out;
        CHECK_FALSE(parseManifest(buf.data(), buf.size(), out));
    };
    run("tailGen==0 with ranges", [](auto& b){ b[26]=b[27]=b[28]=b[29]=0; recomputeManifestCrc(b); });
    run("startGen == 0",          [](auto& b){ b[18]=b[19]=b[20]=b[21]=0; recomputeManifestCrc(b); });
    run("endGen < startGen",      [](auto& b){ b[22]=1; b[23]=b[24]=b[25]=0; recomputeManifestCrc(b); });
}

// --- Manifest publish tests ---

TEST_CASE("manifest-publish: slot election") {
    // Each row: pre-seed slots via writeManifestSlotDirect, then publish once,
    // and verify both slot epochs (0 = absent/not checked).
    struct Case {
        const char* label;
        std::function<void()> setup;
        bool expectAPresent; uint32_t expectAEpoch;
        bool expectBPresent; uint32_t expectBEpoch;
    };
    const Case cases[] = {
        {"fresh (no slots)",   [](){},                                                               true,1,  false,0},
        {"A=1 B=absent",       [](){ writeManifestSlotDirect('a', 1); },                            true,1,  true, 2},
        {"A=absent B=5",       [](){ writeManifestSlotDirect('b', 5); },                            true,6,  true, 5},
        {"A=7 B=3 (B lower)",  [](){ writeManifestSlotDirect('a',7); writeManifestSlotDirect('b',3); }, true,7, true,8},
    };
    ManifestData md; md.nextGeneration = 1; md.tailGeneration = 0;
    for (const auto& [label, setup, expectAPresent, expectAEpoch, expectBPresent, expectBEpoch] : cases) {
        CAPTURE(label);
        cleanSpool();
        pqueue::AppendLogStore store(makeStoreConfig());
        REQUIRE(store.mount().ok());
        setup();
        CHECK(store.publishManifest(md).ok());
        ManifestData slotA, slotB;
        CHECK_EQ(readManifestSlot('a', slotA), expectAPresent);
        CHECK_EQ(readManifestSlot('b', slotB), expectBPresent);
        if (expectAPresent) CHECK_EQ(slotA.epoch, expectAEpoch);
        if (expectBPresent) CHECK_EQ(slotB.epoch, expectBEpoch);
    }
}

TEST_CASE("manifest-publish: corrupt slots return DataCorrupt") {
    struct Case { const char* label; std::function<void()> setup; };
    const Case cases[] = {
        {"corrupt A absent B", [](){ corruptSlot('a'); }},
        {"absent A corrupt B", [](){ corruptSlot('b'); }},
        {"both corrupt",       [](){ corruptSlot('a'); corruptSlot('b'); }},
    };
    ManifestData md; md.nextGeneration = 1; md.tailGeneration = 0;
    for (const auto& [label, setup] : cases) {
        CAPTURE(label);
        cleanSpool();
        pqueue::AppendLogStore store(makeStoreConfig());
        REQUIRE(store.mount().ok());
        setup();
        const auto st = store.publishManifest(md);
        CHECK_FALSE(st.ok());
        CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
    }
}

TEST_CASE("manifest-publish: corrupt slot A is treated as missing; slot B wins election") {
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    writeManifestSlotDirect('b', 7);
    corruptSlot('a');

    ManifestData md;
    md.nextGeneration = 1; md.tailGeneration = 0;
    CHECK(store.publishManifest(md).ok()); // A invalid → writes A, epoch=8

    ManifestData slotA, slotB;
    CHECK(readManifestSlot('a', slotA)); CHECK_EQ(slotA.epoch, 8U);
    CHECK(readManifestSlot('b', slotB)); CHECK_EQ(slotB.epoch, 7U);
}

TEST_CASE("manifest-publish: corrupt slot B leaves slot A as the valid winner") {
    // Critical: inactive-slot-first write order means corrupting the most recent
    // slot (B) leaves the previous slot (A) as the sole valid survivor.
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    ManifestData md;
    md.nextGeneration = 1; md.tailGeneration = 0;
    REQUIRE(store.publishManifest(md).ok()); // writes A, epoch=1
    REQUIRE(store.publishManifest(md).ok()); // writes B, epoch=2

    corruptSlot('b');

    ManifestData slotA, slotB;
    CHECK(readManifestSlot('a', slotA));
    CHECK_EQ(slotA.epoch, 1U);
    CHECK_FALSE(readManifestSlot('b', slotB));
}

// --- Manifest read tests ---

TEST_CASE("manifest-read: slot election") {
    struct Case {
        const char* label;
        std::function<void()> setup;
        bool found;
        uint32_t epoch;
    };
    const Case cases[] = {
        {"both missing",        [](){},                                                                    false, 0},
        {"A valid B missing",   [](){ writeManifestSlotDirect('a', 3); },                                  true,  3},
        {"A missing B valid",   [](){ writeManifestSlotDirect('b', 5); },                                  true,  5},
        {"both valid A higher", [](){ writeManifestSlotDirect('a', 9); writeManifestSlotDirect('b', 4); }, true,  9},
        {"both valid B higher", [](){ writeManifestSlotDirect('a', 2); writeManifestSlotDirect('b', 8); }, true,  8},
        {"corrupt A valid B",   [](){ corruptSlot('a'); writeManifestSlotDirect('b', 4); },                true,  4},
        {"valid A corrupt B",   [](){ writeManifestSlotDirect('a', 2); corruptSlot('b'); },                true,  2},
    };
    for (const auto& [label, setup, found, epoch] : cases) {
        CAPTURE(label);
        cleanSpool();
        pqueue::AppendLogStore store(makeStoreConfig());
        REQUIRE(store.mount().ok());
        setup();
        ManifestData out;
        if (found) {
            CHECK(store.readManifest(out));
            CHECK_EQ(out.epoch, epoch);
        } else {
            CHECK_FALSE(store.readManifest(out));
        }
    }
}

TEST_CASE("manifest-read: higher-epoch slot with corrupt CRC loses to valid lower-epoch slot") {
    // Critical: a partially-written higher-epoch slot must never beat a valid lower-epoch slot.
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    writeManifestSlotDirect('a', 3);

    // Slot B: epoch 10 but CRC-corrupted (simulates a crash mid-write).
    ManifestData mdB; mdB.epoch = 10; mdB.nextGeneration = 1; mdB.tailGeneration = 0;
    std::vector<std::uint8_t> bytes;
    serialiseManifest(mdB, bytes);
    bytes[bytes.size() - 8] ^= 0xFF; // flip a CRC byte
    std::ofstream fb(manifestSlotPath('b'), std::ios::binary | std::ios::trunc);
    fb.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));

    ManifestData out;
    CHECK(store.readManifest(out));
    CHECK_EQ(out.epoch, 3U);
}

// --- Manifest wired into mount ---

TEST_CASE("manifest-mount: referenced segment missing returns DataCorrupt") {
    cleanSpool();
    auto storeCfg = makeStoreConfig();
    {
        pqueue::AppendLogStore store(storeCfg);
        REQUIRE(store.mount().ok());

        pqueue::QueueIndex dummy{};
        REQUIRE(store.writeRecord(0, "A").ok());
        REQUIRE(store.writeIndex(dummy).ok());

        ManifestData md;
        md.tailGeneration = 1; md.nextGeneration = 2;
        REQUIRE(store.publishManifest(md).ok());
    }
    std::filesystem::remove(segmentPath(1));
    {
        pqueue::AppendLogStore store(storeCfg);
        const auto st = store.mount();
        CHECK_FALSE(st.ok());
        CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
    }
}

TEST_CASE("manifest-mount: segment files without manifest return DataCorrupt") {
    // Simulate an older store that never published a manifest by planting a
    // segment file directly.
    resetSpool();
    plantSegment(1);
    {
        pqueue::Queue q(makeConfig());
        std::string out;
        const auto st = q.peek(out);
        CHECK_FALSE(st.ok());
        CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
    }
}

TEST_CASE("manifest-mount: normal mount with manifest recovers records") {
    cleanSpool();
    auto storeCfg = makeStoreConfig();
    {
        pqueue::AppendLogStore store(storeCfg);
        REQUIRE(store.mount().ok());

        pqueue::QueueIndex dummy{};
        REQUIRE(store.writeRecord(0, "alpha").ok());
        REQUIRE(store.writeIndex(dummy).ok());
        REQUIRE(store.writeRecord(1, "beta").ok());
        REQUIRE(store.writeIndex(dummy).ok());
        REQUIRE(store.writeRecord(2, "gamma").ok());
        REQUIRE(store.writeIndex(dummy).ok());

        ManifestData md;
        md.tailGeneration = 1; md.nextGeneration = 2;
        REQUIRE(store.publishManifest(md).ok());
    }
    {
        pqueue::AppendLogStore store(storeCfg);
        REQUIRE(store.mount().ok());
        pqueue::QueueIndex idx;
        REQUIRE(store.readIndex(idx).ok());
        CHECK_EQ(idx.count, 3U);
        std::string out;
        REQUIRE(store.readRecord(0, out).ok()); CHECK_EQ(out, "alpha");
        REQUIRE(store.readRecord(1, out).ok()); CHECK_EQ(out, "beta");
        REQUIRE(store.readRecord(2, out).ok()); CHECK_EQ(out, "gamma");
    }
}

TEST_CASE("manifest-mount: corrupt inactive slot does not affect mount (critical)") {
    // After two publishes (A then B), truncating B simulates a crash mid-write.
    // Remount must recover all records via slot A.
    cleanSpool();
    auto storeCfg = makeStoreConfig();
    {
        pqueue::AppendLogStore store(storeCfg);
        REQUIRE(store.mount().ok());

        pqueue::QueueIndex dummy{};
        REQUIRE(store.writeRecord(0, "one").ok());
        REQUIRE(store.writeIndex(dummy).ok());
        REQUIRE(store.writeRecord(1, "two").ok());
        REQUIRE(store.writeIndex(dummy).ok());
        REQUIRE(store.writeRecord(2, "three").ok());
        REQUIRE(store.writeIndex(dummy).ok());

        ManifestData md;
        md.tailGeneration = 1; md.nextGeneration = 2;
        REQUIRE(store.publishManifest(md).ok()); // slot A, epoch 1
        REQUIRE(store.publishManifest(md).ok()); // slot B, epoch 2

        std::filesystem::resize_file(manifestSlotPath('b'), 5); // simulate crash mid-write
    }
    {
        pqueue::AppendLogStore store(storeCfg);
        REQUIRE(store.mount().ok());
        pqueue::QueueIndex idx;
        REQUIRE(store.readIndex(idx).ok());
        CHECK_EQ(idx.count, 3U);
        std::string out;
        REQUIRE(store.readRecord(0, out).ok()); CHECK_EQ(out, "one");
        REQUIRE(store.readRecord(1, out).ok()); CHECK_EQ(out, "two");
        REQUIRE(store.readRecord(2, out).ok()); CHECK_EQ(out, "three");
    }
}

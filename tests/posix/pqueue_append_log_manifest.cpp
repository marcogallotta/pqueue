#include "pqueue_append_log_support.h"

#ifndef ARDUINO

TEST_CASE("manifest: round-trip empty store (tailGen=0, no ranges)") {
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch          = 1;
    m.nextGeneration = 1;
    m.tailGeneration = 0;

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
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch          = 7;
    m.nextGeneration = 10;
    m.tailGeneration = 9;
    m.ranges         = {{1, 3}, {5, 7}};

    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);
    CHECK_EQ(buf.size(), std::size_t(kManifestFixedBytes + 2 * 8));

    ManifestData out;
    CHECK(parseManifest(buf.data(), buf.size(), out));
    CHECK_EQ(out.epoch,          m.epoch);
    CHECK_EQ(out.nextGeneration, m.nextGeneration);
    CHECK_EQ(out.tailGeneration, m.tailGeneration);
    REQUIRE_EQ(out.ranges.size(), 2u);
    CHECK_EQ(out.ranges[0].startGen, 1u);
    CHECK_EQ(out.ranges[0].endGen,   3u);
    CHECK_EQ(out.ranges[1].startGen, 5u);
    CHECK_EQ(out.ranges[1].endGen,   7u);
}

TEST_CASE("manifest: binary layout field offsets") {
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch          = 0x11223344;
    m.nextGeneration = 0xAABBCCDD;
    m.tailGeneration = 0x55667788;
    // No ranges: headerBytes = 30

    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);
    REQUIRE_EQ(buf.size(), std::size_t(30));

    // magic at 0 (LE: 0x50, 0x51, 0x4D, 0x46 = "PQMF")
    CHECK_EQ(buf[0], 0x50u);
    CHECK_EQ(buf[1], 0x51u);
    CHECK_EQ(buf[2], 0x4Du);
    CHECK_EQ(buf[3], 0x46u);
    // version at 4 = 1 (LE)
    CHECK_EQ(buf[4], 0x01u);
    CHECK_EQ(buf[5], 0x00u);
    // headerBytes at 6 = 30 (LE)
    CHECK_EQ(buf[6], 0x1Eu);
    CHECK_EQ(buf[7], 0x00u);
    // epoch at 8
    CHECK_EQ(buf[8],  0x44u);
    CHECK_EQ(buf[9],  0x33u);
    CHECK_EQ(buf[10], 0x22u);
    CHECK_EQ(buf[11], 0x11u);
    // nextGeneration at 12
    CHECK_EQ(buf[12], 0xDDu);
    CHECK_EQ(buf[13], 0xCCu);
    CHECK_EQ(buf[14], 0xBBu);
    CHECK_EQ(buf[15], 0xAAu);
    // rangeCount at 16 = 0
    CHECK_EQ(buf[16], 0x00u);
    CHECK_EQ(buf[17], 0x00u);
    // tailGeneration at 18
    CHECK_EQ(buf[18], 0x88u);
    CHECK_EQ(buf[19], 0x77u);
    CHECK_EQ(buf[20], 0x66u);
    CHECK_EQ(buf[21], 0x55u);
    // footer at 26 (LE: 0x50, 0x4F, 0x4B, 0x21 = "POK!")
    CHECK_EQ(buf[26], 0x50u);
    CHECK_EQ(buf[27], 0x4Fu);
    CHECK_EQ(buf[28], 0x4Bu);
    CHECK_EQ(buf[29], 0x21u);
}

TEST_CASE("manifest: corrupted CRC is rejected") {
    // Critical: a valid manifest with one CRC byte corrupted must not parse.
    // This is the entire basis of crash-safety on mount.
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch          = 3;
    m.nextGeneration = 5;
    m.tailGeneration = 4;
    m.ranges         = {{1, 3}};

    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);

    // Corrupt one byte of the CRC field (4th from end: footer(4) then crc(4)).
    // With 1 range: total = 38 bytes. CRC is at buf[30..33], footer at buf[34..37].
    CHECK(buf.size() == std::size_t(kManifestFixedBytes + 8));
    buf[buf.size() - 8] ^= 0xFF; // flip a byte in the CRC field

    ManifestData out;
    CHECK_FALSE(parseManifest(buf.data(), buf.size(), out));
}

TEST_CASE("manifest: wrong magic is rejected") {
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch = 1; m.nextGeneration = 1; m.tailGeneration = 0;
    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);
    buf[0] ^= 0xFF; // corrupt magic byte 0
    ManifestData out;
    CHECK_FALSE(parseManifest(buf.data(), buf.size(), out));
}

TEST_CASE("manifest: wrong version is rejected") {
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch = 1; m.nextGeneration = 1; m.tailGeneration = 0;
    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);
    buf[4] = 0x02; // version = 2 instead of 1
    ManifestData out;
    CHECK_FALSE(parseManifest(buf.data(), buf.size(), out));
}

TEST_CASE("manifest: wrong headerBytes is rejected") {
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch = 1; m.nextGeneration = 1; m.tailGeneration = 0;
    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);
    buf[6] = 0xFF; // corrupt headerBytes low byte
    ManifestData out;
    CHECK_FALSE(parseManifest(buf.data(), buf.size(), out));
}

TEST_CASE("manifest: wrong footer is rejected") {
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch = 1; m.nextGeneration = 1; m.tailGeneration = 0;
    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);
    buf[buf.size() - 1] ^= 0xFF; // corrupt last footer byte
    ManifestData out;
    CHECK_FALSE(parseManifest(buf.data(), buf.size(), out));
}

TEST_CASE("manifest: rangeCount > 4 is rejected") {
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch = 1; m.nextGeneration = 1; m.tailGeneration = 0;
    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);
    // Force rangeCount to 5 (at offset 16, LE)
    buf[16] = 0x05;
    ManifestData out;
    CHECK_FALSE(parseManifest(buf.data(), buf.size(), out));
}

TEST_CASE("manifest: tailGen==0 with non-zero rangeCount is rejected") {
    using namespace pqueue::append_log_detail;
    // Build a manifest with rangeCount=1 but tailGeneration=0 — this is malformed.
    // We can't construct this via serialiseManifest (which is well-behaved), so
    // we serialise a valid one and then patch tailGeneration to 0 in the raw bytes.
    ManifestData m;
    m.epoch          = 2;
    m.nextGeneration = 5;
    m.tailGeneration = 4;
    m.ranges         = {{1, 3}};
    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);

    // tailGeneration is at offset 18 + 1*8 = 26 (after 1 range)
    // layout: magic(4)+ver(2)+hdr(2)+epoch(4)+nextGen(4)+rangeCount(2)+range(8)+tailGen(4)+crc(4)+footer(4)
    // tailGen starts at offset: 4+2+2+4+4+2+8 = 26
    buf[26] = 0x00;
    buf[27] = 0x00;
    buf[28] = 0x00;
    buf[29] = 0x00; // tailGeneration = 0

    // Recompute CRC since we changed the data (otherwise we'd fail CRC first, not the tailGen check)
    // CRC covers bytes 0 through 29 (offset of crc field = 30)
    const uint32_t newCrc = crc32(0, buf.data(), 30);
    buf[30] = static_cast<uint8_t>((newCrc      ) & 0xFF);
    buf[31] = static_cast<uint8_t>((newCrc >>  8) & 0xFF);
    buf[32] = static_cast<uint8_t>((newCrc >> 16) & 0xFF);
    buf[33] = static_cast<uint8_t>((newCrc >> 24) & 0xFF);

    ManifestData out;
    CHECK_FALSE(parseManifest(buf.data(), buf.size(), out));
}

TEST_CASE("manifest: buffer too small returns false") {
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch = 1; m.nextGeneration = 1; m.tailGeneration = 0;
    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);
    ManifestData out;
    CHECK_FALSE(parseManifest(buf.data(), buf.size() - 1, out));
    CHECK_FALSE(parseManifest(buf.data(), 0, out));
}

TEST_CASE("manifest: trailing bytes rejected") {
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch = 1; m.nextGeneration = 1; m.tailGeneration = 0;
    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);
    buf.push_back(0x00); // one extra byte
    ManifestData out;
    CHECK_FALSE(parseManifest(buf.data(), buf.size(), out));
}

namespace {
// Patch bytes in buf then recompute and overwrite the CRC field.
// CRC covers bytes [0, crcOffset), which equals headerBytes - 8 (crc(4)+footer(4)).
void recomputeManifestCrc(std::vector<uint8_t>& buf) {
    using namespace pqueue::append_log_detail;
    // headerBytes is at offset 6 (LE u16)
    const std::size_t headerBytes = std::size_t(buf[6]) | std::size_t(buf[7]) << 8;
    const std::size_t crcOffset   = headerBytes - 8; // crc(4) + footer(4)
    const uint32_t newCrc = crc32(0, buf.data(), crcOffset);
    buf[crcOffset + 0] = static_cast<uint8_t>((newCrc      ) & 0xFF);
    buf[crcOffset + 1] = static_cast<uint8_t>((newCrc >>  8) & 0xFF);
    buf[crcOffset + 2] = static_cast<uint8_t>((newCrc >> 16) & 0xFF);
    buf[crcOffset + 3] = static_cast<uint8_t>((newCrc >> 24) & 0xFF);
}
} // namespace

TEST_CASE("manifest: startGen == 0 in range is rejected") {
    // CRC is recomputed so the failure proves range validation, not CRC.
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch = 2; m.nextGeneration = 5; m.tailGeneration = 4;
    m.ranges = {{1, 3}};
    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);

    // startGen of first range is at offset 18 (4+2+2+4+4+2 = 18)
    buf[18] = 0x00; buf[19] = 0x00; buf[20] = 0x00; buf[21] = 0x00;
    recomputeManifestCrc(buf);

    ManifestData out;
    CHECK_FALSE(parseManifest(buf.data(), buf.size(), out));
}

TEST_CASE("manifest: endGen < startGen in range is rejected") {
    // CRC is recomputed so the failure proves range validation, not CRC.
    using namespace pqueue::append_log_detail;
    ManifestData m;
    m.epoch = 2; m.nextGeneration = 5; m.tailGeneration = 4;
    m.ranges = {{3, 5}};
    std::vector<uint8_t> buf;
    serialiseManifest(m, buf);

    // endGen of first range is at offset 22 (startGen at 18, endGen at 22)
    buf[22] = 0x01; buf[23] = 0x00; buf[24] = 0x00; buf[25] = 0x00; // endGen = 1 < startGen = 3
    recomputeManifestCrc(buf);

    ManifestData out;
    CHECK_FALSE(parseManifest(buf.data(), buf.size(), out));
}

// --- Manifest publish tests ---

TEST_CASE("manifest-publish: first publish writes slot A with epoch 1") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    ManifestData md;
    md.nextGeneration = 1;
    md.tailGeneration = 0;
    CHECK(store.publishManifest(md).ok());

    ManifestData slotA, slotB;
    CHECK(readManifestSlot('a', slotA));
    CHECK_EQ(slotA.epoch, 1U);
    CHECK_FALSE(readManifestSlot('b', slotB)); // slot B not written yet
}

TEST_CASE("manifest-publish: second publish writes slot B; A valid B missing") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    ManifestData md;
    md.nextGeneration = 1;
    md.tailGeneration = 0;
    REQUIRE(store.publishManifest(md).ok()); // writes A, epoch=1
    REQUIRE(store.publishManifest(md).ok()); // B missing → writes B, epoch=2

    ManifestData slotA, slotB;
    CHECK(readManifestSlot('a', slotA));
    CHECK_EQ(slotA.epoch, 1U); // A untouched
    CHECK(readManifestSlot('b', slotB));
    CHECK_EQ(slotB.epoch, 2U);
}

TEST_CASE("manifest-publish: A missing B valid writes slot A") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    // Plant slot B (epoch 5) directly; slot A absent.
    writeManifestSlotDirect('b', 5);

    ManifestData md;
    md.nextGeneration = 1;
    md.tailGeneration = 0;
    CHECK(store.publishManifest(md).ok()); // A missing → writes A, epoch=6

    ManifestData slotA, slotB;
    CHECK(readManifestSlot('a', slotA));
    CHECK_EQ(slotA.epoch, 6U);
    CHECK(readManifestSlot('b', slotB));
    CHECK_EQ(slotB.epoch, 5U); // B untouched
}

TEST_CASE("manifest-publish: both valid writes lower-epoch slot") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    // A has epoch 7, B has epoch 3 — B is lower → overwrite B.
    writeManifestSlotDirect('a', 7);
    writeManifestSlotDirect('b', 3);

    ManifestData md;
    md.nextGeneration = 1;
    md.tailGeneration = 0;
    CHECK(store.publishManifest(md).ok());

    ManifestData slotA, slotB;
    CHECK(readManifestSlot('a', slotA));
    CHECK_EQ(slotA.epoch, 7U); // A untouched
    CHECK(readManifestSlot('b', slotB));
    CHECK_EQ(slotB.epoch, 8U); // B overwritten with epoch = max(7,3)+1
}

TEST_CASE("manifest-publish: equal epochs writes slot B (tiebreaker)") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    writeManifestSlotDirect('a', 4);
    writeManifestSlotDirect('b', 4);

    ManifestData md;
    md.nextGeneration = 1;
    md.tailGeneration = 0;
    CHECK(store.publishManifest(md).ok());

    ManifestData slotA, slotB;
    CHECK(readManifestSlot('a', slotA));
    CHECK_EQ(slotA.epoch, 4U); // A untouched
    CHECK(readManifestSlot('b', slotB));
    CHECK_EQ(slotB.epoch, 5U); // B overwritten
}

TEST_CASE("manifest-publish: corrupt A, absent B returns DataCorrupt") {
    // Slot A exists but is corrupt; slot B absent. The existing file is evidence
    // of a committed layout — this is not a fresh store. Must fail loudly.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    {
        std::ofstream fa(manifestSlotPath('a'), std::ios::binary | std::ios::trunc);
        fa.write("GARBAGE", 7);
    }

    ManifestData md;
    md.nextGeneration = 1;
    md.tailGeneration = 0;
    const auto st = store.publishManifest(md);
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
}

TEST_CASE("manifest-publish: absent A, corrupt B returns DataCorrupt") {
    // Slot B exists but is corrupt; slot A absent. Same argument as above.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    {
        std::ofstream fb(manifestSlotPath('b'), std::ios::binary | std::ios::trunc);
        fb.write("GARBAGE", 7);
    }

    ManifestData md;
    md.nextGeneration = 1;
    md.tailGeneration = 0;
    const auto st = store.publishManifest(md);
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
}

TEST_CASE("manifest-publish: both slots corrupt returns DataCorrupt") {
    // Both slot files exist but fail CRC. Fail loudly — do not overwrite.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    {
        std::ofstream fa(manifestSlotPath('a'), std::ios::binary | std::ios::trunc);
        fa.write("GARBAGE_A", 9);
        std::ofstream fb(manifestSlotPath('b'), std::ios::binary | std::ios::trunc);
        fb.write("GARBAGE_B", 9);
    }

    ManifestData md;
    md.nextGeneration = 1;
    md.tailGeneration = 0;
    const auto st = store.publishManifest(md);
    CHECK_FALSE(st.ok());
    CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
}

TEST_CASE("manifest-publish: corrupt slot A is treated as missing; slot B wins election") {
    // Slot A corrupt (fails CRC), slot B valid with epoch 7.
    // Publish must treat A as invalid/missing and overwrite it with epoch 8.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    writeManifestSlotDirect('b', 7);
    {
        std::ofstream f(manifestSlotPath('a'), std::ios::binary | std::ios::trunc);
        f.write("GARBAGE", 7);
    }

    ManifestData md;
    md.nextGeneration = 1;
    md.tailGeneration = 0;
    CHECK(store.publishManifest(md).ok()); // A invalid → writes A, epoch = 7+1 = 8

    ManifestData slotA, slotB;
    CHECK(readManifestSlot('a', slotA));
    CHECK_EQ(slotA.epoch, 8U);
    CHECK(readManifestSlot('b', slotB));
    CHECK_EQ(slotB.epoch, 7U); // B untouched
}

TEST_CASE("manifest-publish: corrupt slot B leaves slot A as the valid winner") {
    // Critical: first publish writes A (inactive = A when both missing).
    // Second publish writes B. After corrupting B, A (lower epoch) must be
    // the only valid slot — confirming inactive-slot-first write order.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    ManifestData md;
    md.nextGeneration = 1;
    md.tailGeneration = 0;
    REQUIRE(store.publishManifest(md).ok()); // writes A, epoch=1
    REQUIRE(store.publishManifest(md).ok()); // writes B, epoch=2

    // Corrupt slot B
    {
        std::ofstream f(manifestSlotPath('b'), std::ios::binary | std::ios::trunc);
        f.write("GARBAGE", 7);
    }

    ManifestData slotA, slotB;
    CHECK(readManifestSlot('a', slotA));
    CHECK_EQ(slotA.epoch, 1U);       // A intact with epoch 1
    CHECK_FALSE(readManifestSlot('b', slotB)); // B corrupt — rejected by parseManifest
}

// --- Manifest read tests (Stage 3a) ---

TEST_CASE("manifest-read: both slots missing returns false") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    ManifestData out;
    CHECK_FALSE(store.readManifest(out));
}

TEST_CASE("manifest-read: slot A valid, slot B missing returns A") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    writeManifestSlotDirect('a', 3);

    ManifestData out;
    CHECK(store.readManifest(out));
    CHECK_EQ(out.epoch, 3U);
}

TEST_CASE("manifest-read: slot A missing, slot B valid returns B") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    writeManifestSlotDirect('b', 5);

    ManifestData out;
    CHECK(store.readManifest(out));
    CHECK_EQ(out.epoch, 5U);
}

TEST_CASE("manifest-read: both valid, A higher epoch wins") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    writeManifestSlotDirect('a', 9);
    writeManifestSlotDirect('b', 4);

    ManifestData out;
    CHECK(store.readManifest(out));
    CHECK_EQ(out.epoch, 9U);
}

TEST_CASE("manifest-read: both valid, B higher epoch wins") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    writeManifestSlotDirect('a', 2);
    writeManifestSlotDirect('b', 8);

    ManifestData out;
    CHECK(store.readManifest(out));
    CHECK_EQ(out.epoch, 8U);
}

TEST_CASE("manifest-read: equal epochs, slot A wins (tiebreaker)") {
    // Slots have the same epoch but different nextGeneration values so we can
    // tell from the returned manifest which slot actually won the election.
    // Spec says: equal epoch → slot A.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    auto writeSlot = [](char slot, uint32_t epoch, uint32_t nextGen) {
        ManifestData md;
        md.epoch = epoch;
        md.nextGeneration = nextGen;
        md.tailGeneration = 0;
        std::vector<std::uint8_t> bytes;
        serialiseManifest(md, bytes);
        std::ofstream f(manifestSlotPath(slot), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    };

    writeSlot('a', 6, 10); // A: epoch 6, nextGeneration 10
    writeSlot('b', 6, 20); // B: epoch 6, nextGeneration 20 — same epoch, different payload

    ManifestData out;
    CHECK(store.readManifest(out));
    CHECK_EQ(out.epoch, 6U);
    CHECK_EQ(out.nextGeneration, 10U); // slot A's payload — proves A won, not just epoch match
}

TEST_CASE("manifest-read: corrupt slot A, valid slot B returns B") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    {
        std::ofstream fa(manifestSlotPath('a'), std::ios::binary | std::ios::trunc);
        fa.write("GARBAGE", 7);
    }
    writeManifestSlotDirect('b', 4);

    ManifestData out;
    CHECK(store.readManifest(out));
    CHECK_EQ(out.epoch, 4U);
}

TEST_CASE("manifest-read: valid slot A, corrupt slot B returns A") {
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    writeManifestSlotDirect('a', 2);
    {
        std::ofstream fb(manifestSlotPath('b'), std::ios::binary | std::ios::trunc);
        fb.write("GARBAGE", 7);
    }

    ManifestData out;
    CHECK(store.readManifest(out));
    CHECK_EQ(out.epoch, 2U);
}

TEST_CASE("manifest-read: higher-epoch slot with corrupt CRC loses to valid lower-epoch slot") {
    // Critical: a partially-written higher-epoch slot must never beat a valid lower-epoch slot.
    // This is the key invariant that makes inactive-slot writes crash-safe.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    pqueue::AppendLogStore store(makeStoreConfig());
    REQUIRE(store.mount().ok());

    // Slot A: valid, epoch 3.
    writeManifestSlotDirect('a', 3);

    // Slot B: written with epoch 10 but then CRC-corrupted (simulates a crash mid-write).
    {
        ManifestData md;
        md.epoch = 10;
        md.nextGeneration = 1;
        md.tailGeneration = 0;
        std::vector<std::uint8_t> bytes;
        serialiseManifest(md, bytes);
        bytes[bytes.size() - 8] ^= 0xFF; // flip a CRC byte
        std::ofstream fb(manifestSlotPath('b'), std::ios::binary | std::ios::trunc);
        fb.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    }

    ManifestData out;
    CHECK(store.readManifest(out));
    CHECK_EQ(out.epoch, 3U); // slot A wins; corrupt B is discarded
}

// --- Stage 3b: manifest wired into mount ---

TEST_CASE("manifest-mount: referenced segment missing returns DataCorrupt") {
    // If the manifest names a segment that does not exist on disk, mount must fail.
    // The manifest is now authoritative metadata; a missing referenced file is true corruption.
    cleanSpool();
    auto storeCfg = makeStoreConfig();
    {
        pqueue::AppendLogStore store(storeCfg);
        REQUIRE(store.mount().ok());

        pqueue::FileStoreIndex dummy{};
        REQUIRE(store.writeRecord(0, "A").ok());
        REQUIRE(store.writeIndex(dummy).ok());

        pqueue::append_log_detail::ManifestData md;
        md.tailGeneration = 1;
        md.nextGeneration = 2;
        REQUIRE(store.publishManifest(md).ok()); // manifest references seg-00000001.bin
    }
    // Delete the referenced segment — manifest is now pointing at a ghost
    std::filesystem::remove(segmentPath(1));
    {
        pqueue::AppendLogStore store(storeCfg);
        const auto st = store.mount();
        CHECK_FALSE(st.ok());
        CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
    }
}

TEST_CASE("manifest-mount: segment files without manifest return DataCorrupt") {
    // A store cannot have segments without a manifest. Simulate a segment written
    // by an older implementation that never published a manifest by planting a
    // valid segment file directly — bypassing Queue, which always publishes.
    using namespace pqueue::append_log_detail;
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);
    {
        const std::string header = serializeSegmentHeader(1, 0);
        std::ofstream f(segmentPath(1), std::ios::binary | std::ios::trunc);
        f.write(header.data(), static_cast<std::streamsize>(header.size()));
    }
    {
        pqueue::Queue q(makeConfig());
        std::string out;
        const auto st = q.peek(out);
        CHECK_FALSE(st.ok());
        CHECK_EQ(st.code, pqueue::StatusCode::DataCorrupt);
    }
}

TEST_CASE("manifest-mount: normal mount with manifest recovers records") {
    // Write records to a segment, publish a manifest, remount — all records readable.
    cleanSpool();
    auto storeCfg = makeStoreConfig();
    {
        pqueue::AppendLogStore store(storeCfg);
        REQUIRE(store.mount().ok());

        pqueue::FileStoreIndex dummy{};
        REQUIRE(store.writeRecord(0, "alpha").ok());
        REQUIRE(store.writeIndex(dummy).ok());
        REQUIRE(store.writeRecord(1, "beta").ok());
        REQUIRE(store.writeIndex(dummy).ok());
        REQUIRE(store.writeRecord(2, "gamma").ok());
        REQUIRE(store.writeIndex(dummy).ok());

        pqueue::append_log_detail::ManifestData md;
        md.tailGeneration = 1;
        md.nextGeneration = 2;
        REQUIRE(store.publishManifest(md).ok());
    }
    {
        pqueue::AppendLogStore store(storeCfg);
        REQUIRE(store.mount().ok());
        pqueue::FileStoreIndex idx;
        REQUIRE(store.readIndex(idx).ok());
        CHECK_EQ(idx.count, 3U);
        std::string out;
        REQUIRE(store.readRecord(0, out).ok()); CHECK_EQ(out, "alpha");
        REQUIRE(store.readRecord(1, out).ok()); CHECK_EQ(out, "beta");
        REQUIRE(store.readRecord(2, out).ok()); CHECK_EQ(out, "gamma");
    }
}

TEST_CASE("manifest-mount: corrupt inactive slot does not affect mount (critical)") {
    // Critical crash-safety test. After two publishes (A then B), truncate B to
    // simulate a crash mid-write. Remount must recover all records via slot A.
    cleanSpool();
    auto storeCfg = makeStoreConfig();
    {
        pqueue::AppendLogStore store(storeCfg);
        REQUIRE(store.mount().ok());

        pqueue::FileStoreIndex dummy{};
        REQUIRE(store.writeRecord(0, "one").ok());
        REQUIRE(store.writeIndex(dummy).ok());
        REQUIRE(store.writeRecord(1, "two").ok());
        REQUIRE(store.writeIndex(dummy).ok());
        REQUIRE(store.writeRecord(2, "three").ok());
        REQUIRE(store.writeIndex(dummy).ok());

        pqueue::append_log_detail::ManifestData md;
        md.tailGeneration = 1;
        md.nextGeneration = 2;
        REQUIRE(store.publishManifest(md).ok()); // slot A, epoch 1
        REQUIRE(store.publishManifest(md).ok()); // slot B, epoch 2

        // Simulate crash mid-write to slot B
        std::filesystem::resize_file(manifestSlotPath('b'), 5);
    }
    {
        pqueue::AppendLogStore store(storeCfg);
        REQUIRE(store.mount().ok()); // slot A (epoch 1) is the surviving slot
        pqueue::FileStoreIndex idx;
        REQUIRE(store.readIndex(idx).ok());
        CHECK_EQ(idx.count, 3U);
        std::string out;
        REQUIRE(store.readRecord(0, out).ok()); CHECK_EQ(out, "one");
        REQUIRE(store.readRecord(1, out).ok()); CHECK_EQ(out, "two");
        REQUIRE(store.readRecord(2, out).ok()); CHECK_EQ(out, "three");
    }
}

#endif // !ARDUINO

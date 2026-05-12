#include "pqueue/append_log_common.h"

#include "doctest/doctest.h"

using namespace pqueue::append_log_detail;

namespace {

CompactionJournalRecord makeRecord(std::uint32_t commitSeq = 1,
                                   std::uint32_t oldStart  = 1,
                                   std::uint32_t oldEnd    = 3,
                                   std::uint32_t newStart  = 4,
                                   std::uint32_t newEnd    = 5) {
    CompactionJournalRecord r;
    r.commitSeq = commitSeq;
    r.oldStart  = oldStart;
    r.oldEnd    = oldEnd;
    r.newStart  = newStart;
    r.newEnd    = newEnd;
    return r;
}

} // namespace

TEST_CASE("compaction journal: round-trip") {
    const auto r = makeRecord();
    CompactionJournalRecord out{};
    CHECK(parseCompactionJournalRecord(serializeCompactionJournalRecord(r), out));
    CHECK_EQ(out.magic,       kCompactionMagic);
    CHECK_EQ(out.version,     kFormatVersion);
    CHECK_EQ(out.headerBytes, kCompactionJournalRecordBytes);
    CHECK_EQ(out.commitSeq,   r.commitSeq);
    CHECK_EQ(out.oldStart,    r.oldStart);
    CHECK_EQ(out.oldEnd,      r.oldEnd);
    CHECK_EQ(out.newStart,    r.newStart);
    CHECK_EQ(out.newEnd,      r.newEnd);
    CHECK_EQ(out.footer,      kFooterMagic);
}

TEST_CASE("compaction journal: serialized size is exactly kCompactionJournalRecordBytes") {
    CHECK_EQ(serializeCompactionJournalRecord(makeRecord()).size(),
             kCompactionJournalRecordBytes);
}

TEST_CASE("compaction journal: serializer forces magic/version/headerBytes/crc/footer") {
    auto r = makeRecord();
    r.magic       = 0xDEADBEEF;
    r.version     = 0xFF;
    r.headerBytes = 0;
    r.crc         = 0xDEADBEEF;
    r.footer      = 0x12345678;
    CompactionJournalRecord out{};
    CHECK(parseCompactionJournalRecord(serializeCompactionJournalRecord(r), out));
    CHECK_EQ(out.magic,       kCompactionMagic);
    CHECK_EQ(out.version,     kFormatVersion);
    CHECK_EQ(out.headerBytes, kCompactionJournalRecordBytes);
    CHECK_EQ(out.footer,      kFooterMagic);
}

TEST_CASE("compaction journal: reject empty buffer") {
    CompactionJournalRecord out{};
    CHECK_FALSE(parseCompactionJournalRecord("", out));
}

TEST_CASE("compaction journal: reject buffer one byte short") {
    const auto bytes = serializeCompactionJournalRecord(makeRecord());
    CompactionJournalRecord out{};
    CHECK_FALSE(parseCompactionJournalRecord(bytes.substr(0, kCompactionJournalRecordBytes - 1), out));
}

TEST_CASE("compaction journal: reject wrong magic") {
    auto bytes = serializeCompactionJournalRecord(makeRecord());
    bytes[0] ^= 0xFF;
    CompactionJournalRecord out{};
    CHECK_FALSE(parseCompactionJournalRecord(bytes, out));
}

TEST_CASE("compaction journal: reject wrong version") {
    auto bytes = serializeCompactionJournalRecord(makeRecord());
    bytes[4] ^= 0x01; // version field at offset 4
    CompactionJournalRecord out{};
    CHECK_FALSE(parseCompactionJournalRecord(bytes, out));
}

TEST_CASE("compaction journal: reject wrong headerBytes") {
    auto bytes = serializeCompactionJournalRecord(makeRecord());
    bytes[6] ^= 0x01; // headerBytes field at offset 6
    CompactionJournalRecord out{};
    CHECK_FALSE(parseCompactionJournalRecord(bytes, out));
}

TEST_CASE("compaction journal: reject corrupt footer") {
    auto bytes = serializeCompactionJournalRecord(makeRecord());
    bytes[kCompactionJournalRecordBytes - 1] ^= 0xFF;
    CompactionJournalRecord out{};
    CHECK_FALSE(parseCompactionJournalRecord(bytes, out));
}

TEST_CASE("compaction journal: reject corrupt CRC") {
    auto bytes = serializeCompactionJournalRecord(makeRecord());
    bytes[28] ^= 0xFF; // crc field at offset 28
    CompactionJournalRecord out{};
    CHECK_FALSE(parseCompactionJournalRecord(bytes, out));
}

TEST_CASE("compaction journal: reject oldStart == 0") {
    CompactionJournalRecord out{};
    CHECK_FALSE(parseCompactionJournalRecord(
        serializeCompactionJournalRecord(makeRecord(1, 0, 3, 4, 5)), out));
}

TEST_CASE("compaction journal: reject newStart == 0") {
    CompactionJournalRecord out{};
    CHECK_FALSE(parseCompactionJournalRecord(
        serializeCompactionJournalRecord(makeRecord(1, 1, 3, 0, 5)), out));
}

TEST_CASE("compaction journal: reject oldStart > oldEnd") {
    CompactionJournalRecord out{};
    CHECK_FALSE(parseCompactionJournalRecord(
        serializeCompactionJournalRecord(makeRecord(1, 5, 3, 6, 8)), out));
}

TEST_CASE("compaction journal: reject newStart > newEnd") {
    CompactionJournalRecord out{};
    CHECK_FALSE(parseCompactionJournalRecord(
        serializeCompactionJournalRecord(makeRecord(1, 1, 3, 8, 6)), out));
}

TEST_CASE("compaction journal: single-segment old range round-trips") {
    CompactionJournalRecord out{};
    CHECK(parseCompactionJournalRecord(
        serializeCompactionJournalRecord(makeRecord(1, 3, 3, 4, 5)), out));
    CHECK_EQ(out.oldStart, 3U);
    CHECK_EQ(out.oldEnd,   3U);
}

TEST_CASE("compaction journal: single-segment new range round-trips") {
    CompactionJournalRecord out{};
    CHECK(parseCompactionJournalRecord(
        serializeCompactionJournalRecord(makeRecord(1, 1, 3, 4, 4)), out));
    CHECK_EQ(out.newStart, 4U);
    CHECK_EQ(out.newEnd,   4U);
}

TEST_CASE("compaction journal: commitSeq 0 is accepted") {
    CompactionJournalRecord out{};
    CHECK(parseCompactionJournalRecord(
        serializeCompactionJournalRecord(makeRecord(0)), out));
    CHECK_EQ(out.commitSeq, 0U);
}

TEST_CASE("compaction journal: commitSeq UINT32_MAX round-trips") {
    CompactionJournalRecord out{};
    CHECK(parseCompactionJournalRecord(
        serializeCompactionJournalRecord(makeRecord(0xFFFFFFFFu)), out));
    CHECK_EQ(out.commitSeq, 0xFFFFFFFFu);
}

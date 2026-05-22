#pragma once

#include "pqueue/queue.h"
#include "pqueue/append_log_store.h"
#include "pqueue/append_log_common.h"
#include "pqueue/status.h"

#include "support/pqueue_queue_support.h"

#include "doctest/doctest.h"

#ifndef ARDUINO
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

inline const std::filesystem::path kSpoolDir = "build/pqueue-spools/pqueue_append_log_spool";

inline std::filesystem::path manifestSlotPath(char slot) {
    return kSpoolDir / (std::string("manifest-") + slot + ".bin");
}

inline bool readManifestSlot(char slot, pqueue::append_log_detail::ManifestData& out) {
    using namespace pqueue::append_log_detail;
    std::ifstream f(manifestSlotPath(slot), std::ios::binary);
    if (!f) return false;
    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(f)),
        std::istreambuf_iterator<char>()
    );
    return parseManifest(bytes.data(), bytes.size(), out);
}

inline void writeManifestSlotDirect(char slot, uint32_t epoch) {
    using namespace pqueue::append_log_detail;
    ManifestData md;
    md.epoch = epoch;
    md.nextGeneration = 1;
    md.tailGeneration = 0;
    std::vector<std::uint8_t> bytes;
    serialiseManifest(md, bytes);
    std::ofstream f(manifestSlotPath(slot), std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

inline pqueue::AppendLogConfig makeStoreConfig() {
    pqueue::AppendLogConfig cfg;
    cfg.basePath = kSpoolDir.string();
    cfg.maxSegmentBytes = 1024;
    return cfg;
}

inline std::filesystem::path segmentPath(std::uint32_t gen) {
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", gen);
    return kSpoolDir / ("seg-" + std::string(buf, 8) + ".bin");
}

inline void patchFile(const std::filesystem::path& path, std::uintmax_t offset,
               std::initializer_list<std::uint8_t> bytes) {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(static_cast<std::streamoff>(offset));
    for (std::uint8_t b : bytes) {
        const char c = static_cast<char>(b);
        f.write(&c, 1);
    }
}

inline void cleanSpool() {
    std::error_code ec;
    std::filesystem::remove_all(kSpoolDir, ec);
}

inline void resetSpool() {
    cleanSpool();
    std::filesystem::create_directories(kSpoolDir);
}

inline void plantManifest(const pqueue::append_log_detail::ManifestData& md, char slot = 'a') {
    using namespace pqueue::append_log_detail;
    std::vector<std::uint8_t> bytes;
    serialiseManifest(md, bytes);
    std::ofstream f(manifestSlotPath(slot), std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

inline void corruptSlot(char slot) {
    std::ofstream f(manifestSlotPath(slot), std::ios::binary | std::ios::trunc);
    f.write("GARBAGE", 7);
}

inline void plantSegment(std::uint32_t gen, std::uint32_t firstSeq = 0, const std::string& body = "") {
    using namespace pqueue::append_log_detail;
    std::string seg = serializeSegmentHeader(gen, firstSeq);
    seg += body;
    std::ofstream f(segmentPath(gen), std::ios::binary | std::ios::trunc);
    f.write(seg.data(), static_cast<std::streamsize>(seg.size()));
}

struct SegmentSpec {
    std::uint32_t gen;
    std::uint32_t firstSeq = 0;
    std::string body;
};

struct LayoutSpec {
    std::uint32_t epoch = 1;
    std::vector<pqueue::append_log_detail::ManifestRange> ranges;
    std::uint32_t tail = 0;
    std::uint32_t next = 0;
    std::vector<SegmentSpec> segments;
};

inline void plantLayout(const LayoutSpec& spec) {
    resetSpool();
    pqueue::append_log_detail::ManifestData md;
    md.epoch = spec.epoch;
    md.ranges = spec.ranges;
    md.tailGeneration = spec.tail;
    md.nextGeneration = spec.next;
    plantManifest(md);
    for (const auto& seg : spec.segments)
        plantSegment(seg.gen, seg.firstSeq, seg.body);
}

inline void storeEnqueue(pqueue::AppendLogStore& store, std::uint32_t seq, const std::string& payload) {
    CHECK(store.commitEnqueue(seq, payload).ok());
}

inline void storePop(pqueue::AppendLogStore& store) {
    pqueue::QueueIndex idx;
    CHECK(store.readIndex(idx).ok());
    CHECK(store.commitPop(idx.head).ok());
}

inline void expectRecord(pqueue::AppendLogStore& store, std::uint32_t seq, const std::string& payload) {
    std::string out;
    CHECK(store.readRecord(seq, out).ok());
    CHECK_EQ(out, payload);
}

inline void expectRecords(pqueue::AppendLogStore& store,
    std::initializer_list<std::pair<std::uint32_t, std::string>> expected) {
    for (const auto& [seq, payload] : expected)
        expectRecord(store, seq, payload);
}

inline void expectRecordsAfterRemount(const pqueue::AppendLogConfig& cfg,
    std::initializer_list<std::pair<std::uint32_t, std::string>> expected) {
    pqueue::AppendLogStore store(cfg);
    REQUIRE(store.mount().ok());
    for (const auto& [seq, payload] : expected)
        expectRecord(store, seq, payload);
}

inline pqueue::Config makeConfig() {
    pqueue::Config cfg;
    cfg.basePath = kSpoolDir.string();
    cfg.recordSizeBytes = 256;
    cfg.reservedBytes = 64 * 1024;
    cfg.maxSegmentBytes = 1024; // small to force rotation in tests
    return cfg;
}

#endif // !ARDUINO

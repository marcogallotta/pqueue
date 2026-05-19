#pragma once

#include "pqueue/queue.h"
#include "pqueue/append_log_store.h"
#include "pqueue/append_log_common.h"
#include "pqueue/status.h"

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

inline void plantManifest(const pqueue::append_log_detail::ManifestData& md) {
    using namespace pqueue::append_log_detail;
    std::vector<std::uint8_t> bytes;
    serialiseManifest(md, bytes);
    std::ofstream f(manifestSlotPath('a'), std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

inline void plantSegment(std::uint32_t gen, std::uint32_t firstSeq = 0, const std::string& body = "") {
    using namespace pqueue::append_log_detail;
    std::string seg = serializeSegmentHeader(gen, firstSeq);
    seg += body;
    std::ofstream f(segmentPath(gen), std::ios::binary | std::ios::trunc);
    f.write(seg.data(), static_cast<std::streamsize>(seg.size()));
}

inline void storeEnqueue(pqueue::AppendLogStore& store, std::uint32_t seq, const std::string& payload) {
    CHECK(store.writeRecord(seq, payload).ok());
    pqueue::FileStoreIndex idx;
    CHECK(store.readIndex(idx).ok());
    CHECK(store.writeIndex(idx).ok());
}

inline void storePop(pqueue::AppendLogStore& store) {
    pqueue::FileStoreIndex idx;
    CHECK(store.readIndex(idx).ok());
    idx.head++;
    CHECK(store.writeIndex(idx).ok());
}

class FaultInjectingFs final : public pqueue::FileSystem {
public:
    explicit FaultInjectingFs(std::shared_ptr<pqueue::FileSystem> inner)
        : inner_(std::move(inner)) {}

    // Fail the next writeFile whose name contains this substring. Cleared on match.
    std::string failNextWriteFileTo;

    pqueue::Status mount(const std::string& p) override { return inner_->mount(p); }
    pqueue::Status readFile(const std::string& n, std::string& o) override { return inner_->readFile(n, o); }
    pqueue::Status writeFile(const std::string& name, const std::string& data) override {
        if (!failNextWriteFileTo.empty() && name.find(failNextWriteFileTo) != std::string::npos) {
            failNextWriteFileTo.clear();
            return pqueue::Status::failure(pqueue::StatusCode::WriteFailed, "injected write failure");
        }
        return inner_->writeFile(name, data);
    }
    pqueue::Status readAt(const std::string& n, std::uint64_t o, std::size_t s, std::string& out) override { return inner_->readAt(n, o, s, out); }
    pqueue::Status writeAt(const std::string& n, std::uint64_t o, const std::string& d) override { return inner_->writeAt(n, o, d); }
    pqueue::Status resizeFile(const std::string& n, std::uint64_t s) override { return inner_->resizeFile(n, s); }
    pqueue::Status fileSize(const std::string& n, std::uint64_t& o) override { return inner_->fileSize(n, o); }
    pqueue::Status removeFile(const std::string& n) override { return inner_->removeFile(n); }
    pqueue::Status renameFile(const std::string& f, const std::string& t) override { return inner_->renameFile(f, t); }
    pqueue::Status listFiles(std::vector<std::string>& o) override { return inner_->listFiles(o); }
    pqueue::Status tryAcquireLockFile(const std::string& n, const std::string& c) override { return inner_->tryAcquireLockFile(n, c); }
    pqueue::Status releaseLockFile(const std::string& n, const std::string& c) override { return inner_->releaseLockFile(n, c); }
    pqueue::Status recoverStaleLockFile(const std::string& n, const std::string& c) override { return inner_->recoverStaleLockFile(n, c); }
    std::uint64_t freeBytes() const override { return inner_->freeBytes(); }

private:
    std::shared_ptr<pqueue::FileSystem> inner_;
};

inline pqueue::Config makeConfig() {
    pqueue::Config cfg;
    cfg.basePath = kSpoolDir.string();
    cfg.storeLayout = pqueue::StoreLayout::AppendLog;
    cfg.recordSizeBytes = 256;
    cfg.reservedBytes = 64 * 1024;
    cfg.maxSegmentBytes = 1024; // small to force rotation in tests
    return cfg;
}

#endif // !ARDUINO

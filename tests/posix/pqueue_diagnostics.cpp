#include "doctest/doctest.h"

#include "pqueue/diagnostics.h"
#include "pqueue/queue.h"
#include "pqueue/storage_common.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path tempBasePath(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

void cleanDir(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

std::uint32_t slotSize(std::size_t recordSizeBytes) {
    return static_cast<std::uint32_t>(pqueue::storage_detail::kRecordHeaderBytes + recordSizeBytes);
}

pqueue::FileStoreConfig storeConfig(const std::filesystem::path& path) {
    pqueue::FileStoreConfig config;
    config.basePath = path.string();
    config.backend = pqueue::StorageBackend::Posix;
    config.recordSizeBytes = 32;
    config.reservedBytes = slotSize(config.recordSizeBytes) * 4;
    return config;
}

pqueue::Config queueConfig(const std::filesystem::path& path) {
    pqueue::Config config;
    config.basePath = path.string();
    config.storageBackend = pqueue::StorageBackend::Posix;
    config.recordSizeBytes = 32;
    config.reservedBytes = slotSize(config.recordSizeBytes) * 4;
    return config;
}

void writeZeroSpool(const std::filesystem::path& base, std::uint64_t bytes) {
    std::filesystem::create_directories(base);
    std::ofstream out(base / "pqueue.spool", std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    std::string zeros(256, '\0');
    while (bytes != 0) {
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(bytes, zeros.size()));
        out.write(zeros.data(), static_cast<std::streamsize>(n));
        bytes -= n;
    }
}

} // namespace

TEST_CASE("pqueue diagnostics reports missing spool without creating one") {
    const auto base = tempBasePath("pqueue_diag_missing_spool");
    cleanDir(base);

    const pqueue::FileStoreDiagnostic diag = pqueue::diagnoseFileStore(storeConfig(base));

    CHECK(diag.mountStatus.ok());
    CHECK(diag.layout.valid);
    CHECK_FALSE(diag.spoolExists);
    CHECK_FALSE(diag.hasUsableCheckpoint);

    cleanDir(base);
}

TEST_CASE("pqueue diagnostics reports valid checkpoint after queue creation") {
    const auto base = tempBasePath("pqueue_diag_valid_checkpoint");
    cleanDir(base);

    {
        pqueue::Queue queue(queueConfig(base));
        REQUIRE(queue.enqueue("hello").ok());
    }

    const pqueue::FileStoreDiagnostic diag = pqueue::diagnoseFileStore(storeConfig(base));

    CHECK(diag.mountStatus.ok());
    CHECK(diag.spoolExists);
    CHECK(diag.spoolSizeMatches);
    CHECK(diag.hasUsableCheckpoint);
    REQUIRE(diag.checkpointSlots.size() == pqueue::storage_detail::kCheckpointSlots);

    cleanDir(base);
}

TEST_CASE("pqueue diagnostics reports zeroed checkpoint metadata") {
    const auto base = tempBasePath("pqueue_diag_zero_checkpoint");
    cleanDir(base);

    const pqueue::FileStoreDiagnostic emptyDiag = pqueue::diagnoseFileStore(storeConfig(base));
    REQUIRE(emptyDiag.layout.valid);
    writeZeroSpool(base, emptyDiag.layout.spoolBytes);

    const pqueue::FileStoreDiagnostic diag = pqueue::diagnoseFileStore(storeConfig(base));

    CHECK(diag.mountStatus.ok());
    CHECK(diag.spoolExists);
    CHECK(diag.spoolSizeMatches);
    CHECK_FALSE(diag.hasUsableCheckpoint);
    REQUIRE(diag.checkpointSlots.size() == pqueue::storage_detail::kCheckpointSlots);
    for (const auto& slot : diag.checkpointSlots) {
        CHECK(slot.state == pqueue::CheckpointSlotState::Zero);
    }

    cleanDir(base);
}

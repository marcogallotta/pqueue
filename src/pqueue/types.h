#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

#include "events.h"
#include "file_store.h"

namespace pqueue {

using Record = std::string;

struct Config {
    std::string basePath = kDefaultBasePath;
    StorageBackend storageBackend = StorageBackend::Default;
    std::uint32_t reservedBytes = 128 * 1024;
    std::size_t recordSizeBytes = 4096;
    std::uint32_t journalBytes = 4096;
    std::uint32_t checkpointEveryOps = 64;
    EventOptions events;
    // Optional filesystem injection for tests/profiling/custom backends.
    // Production users normally leave this unset and select storageBackend instead.
    std::shared_ptr<FileSystem> fileSystem;
    // TODO: make full-buffer behavior configurable. Options: reject newest, drop oldest, overwrite oldest.
};

struct Stats {
    std::uint32_t count = 0;
    std::uint64_t freeBytes = 0;
};

struct StatsResult {
    Status status = Status::success();
    Stats stats;
};

} // namespace pqueue

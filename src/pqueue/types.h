#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

#include "events.h"
#include "store_types.h"

namespace pqueue {

using Record = std::string;

enum class FullQueuePolicy {
    RejectNewest,
    DropOldest,
};

struct Config {
    std::string basePath = kDefaultBasePath;
    StorageBackend storageBackend = StorageBackend::Default;
    std::uint32_t reservedBytes = 128 * 1024;
    std::size_t recordSizeBytes = 492;
    FullQueuePolicy fullQueuePolicy = FullQueuePolicy::RejectNewest;
    EventOptions events;
    // Optional filesystem injection for tests/profiling/custom backends.
    // Production users normally leave this unset and select storageBackend instead.
    std::shared_ptr<FileSystem> fileSystem;
    std::uint32_t maxSegmentBytes = 4096;
    std::uint32_t minFreeBytes    = 32 * 1024;
    std::uint8_t  maxSegments     = 16;
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

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "append_log_store.h"
#include "status.h"

namespace pqueue {

// --- AppendLog diagnostic ---

struct AppendLogDiagnosticRange {
    std::uint32_t startGen = 0;
    std::uint32_t endGen   = 0;
};

struct AppendLogManifestSlotDiagnostic {
    bool exists = false;
    bool valid  = false;
    std::uint32_t epoch = 0;
};

struct AppendLogSegmentDiagnostic {
    std::uint32_t generation = 0;
    std::uint64_t sizeBytes  = 0;
    bool referenced = false; // in manifest ranges or tail
    bool isTail     = false;
};

struct AppendLogStoreDiagnostic {
    std::string basePath;
    Status mountStatus = Status::success();
    Status listStatus  = Status::success();
    std::uint64_t freeBytes = 0;

    AppendLogManifestSlotDiagnostic slotA;
    AppendLogManifestSlotDiagnostic slotB;
    bool hasWinner            = false;
    std::uint32_t winnerEpoch = 0;

    std::vector<AppendLogDiagnosticRange> ranges;
    std::uint32_t tailGeneration  = 0;
    std::uint32_t nextGeneration  = 0;

    std::vector<AppendLogSegmentDiagnostic> segments; // all on disk, sorted by generation
    std::size_t danglingSegments = 0;
};

AppendLogStoreDiagnostic diagnoseAppendLogStore(const AppendLogConfig& config);

} // namespace pqueue

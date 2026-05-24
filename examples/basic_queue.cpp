// basic_queue -- enqueue / drain / compact lifecycle on a POSIX filesystem.
//
// Demonstrates the three operating phases:
//   1. Enqueue  -- records accumulate when the backend is unavailable
//   2. Drain    -- peek/pop loop empties the queue when the backend recovers
//   3. Compact  -- compactIdle reclaims flash from tombstoned records
//
// Build:  make -j12 examples
// Run:    ./build/examples/basic-queue

#include <cstdio>
#include <filesystem>
#include <string>

#include "pqueue/queue.h"

static const std::filesystem::path kSpoolDir = "build/examples/spools/basic-queue";

int main() {
    std::error_code ec;
    std::filesystem::remove_all(kSpoolDir, ec);
    std::filesystem::create_directories(kSpoolDir);

    pqueue::Config cfg;
    cfg.basePath        = kSpoolDir.string();
    cfg.maxSegmentBytes = 256;   // small so this example spans several segment files
    cfg.reservedBytes   = 32768;
    cfg.minFreeBytes    = 0;     // no FS floor on desktop

    pqueue::Queue queue(cfg);

    // --- Enqueue phase ---
    // In production: call enqueue() each time a sensor reading or event arrives
    // and the backend is unreachable. Records persist across power cycles.
    printf("=== enqueue ===\n");
    const char* records[] = {
        R"({"sensor":"temp","v":21.1})",
        R"({"sensor":"temp","v":21.3})",
        R"({"sensor":"humidity","v":58})",
        R"({"sensor":"temp","v":21.5})",
        R"({"sensor":"co2","v":412})",
        R"({"sensor":"temp","v":21.7})",
        R"({"sensor":"humidity","v":59})",
        R"({"sensor":"temp","v":21.9})",
        R"({"sensor":"temp","v":22.1})",
        R"({"sensor":"co2","v":415})",
    };
    for (const char* r : records) {
        auto st = queue.enqueue(r);
        printf("  enqueue %-34s -> %s\n", r, st.ok() ? "ok" : st.message);
    }
    printf("  %u records queued\n", queue.stats().count);

    // --- Drain phase ---
    // peek() reads the front record without removing it. pop() appends a
    // tombstone marking it dead. Dead bytes stay on flash until compaction.
    printf("\n=== drain ===\n");
    std::string rec;
    int sent = 0;
    while (sent < 7 && queue.peek(rec).ok()) {
        printf("  send %-34s -> ok\n", rec.c_str());
        auto st = queue.pop();
        if (!st.ok()) { printf("  pop failed: %s\n", st.message); break; }
        ++sent;
    }
    printf("  %u records remaining\n", queue.stats().count);

    // --- Idle compaction phase ---
    // Run after drain (or during a reconnect delay) to reclaim flash from
    // tombstoned records. Desktop example: loop to completion so the output is
    // easy to read. On firmware, call one step per idle window instead.
    printf("\n=== compact ===\n");
    pqueue::CompactIdleResult cr;
    std::size_t totalSteps = 0;
    std::size_t totalNoOps = 0;
    do {
        cr = queue.compactIdle(1);
        totalSteps += cr.stepsRun;
        totalNoOps += cr.noOps;
        if (cr.compactions > 0) {
            printf("  step: %u bytes reclaimed, %u dead bytes remaining\n",
                   (unsigned)cr.bytesReclaimed, (unsigned)cr.remainingDeadBytes);
        }
    } while (cr.compactions > 0);
    printf("  done  steps=%zu  noOps=%zu\n", totalSteps, totalNoOps);

    return 0;
}

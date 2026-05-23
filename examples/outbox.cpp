// outbox -- store-and-forward with automatic retry over a custom transport.
//
// pqueue::Outbox wraps Queue with a SendCallback, retry backoff, and rate
// limiting. This example simulates a backend that is offline during an enqueue
// burst, then recovers. The compact phase reclaims the dead bytes left behind
// by the drained records.
//
// Build:  make -j12 examples
// Run:    ./build/examples/outbox

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include "pqueue/outbox.h"

static const std::filesystem::path kSpoolDir = "build/examples/spools/outbox";

// --- Simulated backend ---

struct Backend { bool online = false; };

static pqueue::SendResult trySend(void* ctx, const std::string& payload, const pqueue::RetryState& retry) {
    auto* b = static_cast<Backend*>(ctx);
    if (!b->online) {
        printf("    backend FAIL  attempt=%-2u  (backend offline)\n", retry.attempts);
        return {pqueue::SendDecision::RetryLater};
    }
    printf("    backend SENT  %s\n", payload.c_str());
    return {pqueue::SendDecision::Sent};
}

// Monotonic milliseconds. Advancing by initialRetryDelayMs+1 per call ensures
// cooldowns always expire between drain cycles in this example.
// On ESP32 use: esp_timer_get_time() / 1000.
static std::uint64_t clockMs(void*) {
    static std::uint64_t ms = 0;
    ms += 11000;
    return ms;
}

int main() {
    std::error_code ec;
    std::filesystem::remove_all(kSpoolDir, ec);
    std::filesystem::create_directories(kSpoolDir);

    pqueue::Config qcfg;
    qcfg.basePath         = kSpoolDir.string();
    qcfg.reservedBytes    = 32768;
    qcfg.minFreeBytes     = 0;
    qcfg.maxSegmentBytes  = 256;  // small segments so the burst spans several sealed ones

    pqueue::OutboxConfig ocfg;
    ocfg.initialRetryDelayMs = 10000;

    Backend backend;
    pqueue::Outbox outbox(qcfg, ocfg, trySend, &backend, clockMs, nullptr);

    // --- Submit (backend offline) ---
    // All records are queued. Enough records are submitted to fill several
    // segments so there is real dead data to reclaim after the drain.
    printf("=== submit (backend offline) ===\n");
    const char* payloads[] = {
        R"({"sensor":"temp","v":21.0})",
        R"({"sensor":"temp","v":21.2})",
        R"({"sensor":"temp","v":21.4})",
        R"({"sensor":"temp","v":21.6})",
        R"({"sensor":"temp","v":21.8})",
        R"({"sensor":"temp","v":22.0})",
        R"({"sensor":"temp","v":22.2})",
        R"({"sensor":"temp","v":22.4})",
        R"({"sensor":"temp","v":22.6})",
        R"({"sensor":"temp","v":22.8})",
        R"({"sensor":"temp","v":23.0})",
        R"({"sensor":"temp","v":23.2})",
    };
    for (const char* p : payloads) {
        auto r = outbox.submit(p);
        const char* s = r.status == pqueue::SubmitStatus::Sent   ? "sent"
                      : r.status == pqueue::SubmitStatus::Queued ? "queued"
                      :                                             "error";
        printf("  submit %-32s -> %s\n", p, s);
    }

    // --- Drain (backend online) ---
    // drainUpTo() attempts up to N sends per call, stopping on failure or when
    // the queue is empty. Call it on each main-loop tick until it drains cleanly.
    printf("\n=== drain (backend online) ===\n");
    backend.online = true;
    for (int cycle = 1; cycle <= 20; ++cycle) {
        auto dr = outbox.drainUpTo(10);
        if (dr.attempts == 0 && !dr.rateLimited) break;
        printf("  cycle %-2d  attempts=%-2u  sent=%-2u  rateLimited=%s\n",
               cycle, dr.attempts, dr.sent, dr.rateLimited ? "yes" : "no");
    }

    // --- Compact ---
    // After all records are sent, each sealed segment still holds the original
    // ENQUEUE bytes as dead data. compactIdle rewrites those segments to reclaim
    // the space; the tail (never a compaction candidate) is left alone.
    printf("\n=== compact ===\n");
    pqueue::CompactIdleResult cr;
    std::size_t totalSteps = 0;
    std::uint32_t totalReclaimed = 0;
    do {
        cr = outbox.compactIdle(1);
        totalSteps += cr.stepsRun;
        totalReclaimed += cr.bytesReclaimed;
        if (cr.compactions > 0) {
            printf("  step %-2zu  reclaimed=%u bytes\n", totalSteps, cr.bytesReclaimed);
        }
    } while (cr.moreWorkLikely);
    printf("  done  steps=%zu  reclaimed=%u bytes total\n", totalSteps, totalReclaimed);

    return 0;
}

// outbox -- store-and-forward with automatic retry over a custom transport.
//
// pqueue::Outbox wraps Queue with a SendCallback, retry backoff, and rate
// limiting. This example simulates a backend that is offline when records are
// submitted, then recovers during the drain phase.
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

struct Backend { int failsRemaining = 2; };

static pqueue::SendResult trySend(void* ctx, const std::string& payload, const pqueue::RetryState& retry) {
    auto* b = static_cast<Backend*>(ctx);
    if (b->failsRemaining > 0) {
        --b->failsRemaining;
        printf("    backend FAIL  attempt=%-2u  (%d fail(s) remaining in sim)\n",
               retry.attempts, b->failsRemaining);
        return {pqueue::SendDecision::RetryLater};
    }
    printf("    backend SENT  %s\n", payload.c_str());
    return {pqueue::SendDecision::Sent};
}

// Monotonic milliseconds. Advancing by retryDelayMs+1 per call ensures
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
    qcfg.basePath      = kSpoolDir.string();
    qcfg.reservedBytes = 32768;
    qcfg.minFreeBytes  = 0;

    pqueue::OutboxConfig ocfg;
    ocfg.retryDelayMs = 10000;

    Backend backend;
    pqueue::Outbox outbox(qcfg, ocfg, trySend, &backend, clockMs, nullptr);

    // --- Submit ---
    // If the queue is empty, submit() attempts an immediate send. On failure it
    // queues the record and starts a retry cooldown. Subsequent submits while
    // the queue is non-empty are queued directly without a send attempt.
    printf("=== submit ===\n");
    const char* payloads[] = {
        R"({"sensor":"temp","v":22.1})",
        R"({"sensor":"temp","v":22.3})",
        R"({"sensor":"temp","v":22.5})",
    };
    for (const char* p : payloads) {
        auto r = outbox.submit(p);
        const char* s = r.status == pqueue::SubmitStatus::Sent   ? "sent"
                      : r.status == pqueue::SubmitStatus::Queued ? "queued"
                      :                                             "error";
        printf("  submit %-32s -> %s\n", p, s);
    }

    // --- Drain ---
    // drainUpTo() attempts up to N sends per call, stopping on failure or when
    // the queue is empty. Call it on each main-loop tick until it drains cleanly.
    printf("\n=== drain ===\n");
    for (int cycle = 1; cycle <= 4; ++cycle) {
        printf("  cycle %d:\n", cycle);
        auto dr = outbox.drainUpTo(10);
        printf("    attempts=%-2u  sent=%-2u  rateLimited=%s\n",
               dr.attempts, dr.sent, dr.rateLimited ? "yes" : "no");
        if (dr.attempts == 0) break; // queue empty
    }

    // --- Compact ---
    printf("\n=== compact ===\n");
    pqueue::CompactIdleResult cr;
    std::size_t totalSteps = 0;
    std::uint32_t totalReclaimed = 0;
    do {
        cr = outbox.compactIdle(1);
        totalSteps += cr.stepsRun;
        totalReclaimed += cr.bytesReclaimed;
    } while (cr.compactions > 0);
    printf("  done  steps=%zu  reclaimed=%u bytes\n", totalSteps, totalReclaimed);

    return 0;
}

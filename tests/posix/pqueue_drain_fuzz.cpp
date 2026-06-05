#ifndef ARDUINO

#include <cstdint>
#include <filesystem>
#include <string>

// Access Queue::store_ and Queue::rewriteFront (both private).
#define private public
#include "pqueue/queue.h"
#undef private

#include "pqueue/append_log_store.h"
#include "pqueue/status.h"

#include "doctest/doctest.h"

namespace {

const std::filesystem::path kFuzzSpoolDir =
    "build/pqueue-spools/pqueue_drain_fuzz_spool";

void cleanFuzzSpool() {
    std::error_code ec;
    std::filesystem::remove_all(kFuzzSpoolDir, ec);
}

// Production dynamics, scaled to 128 KiB so the test runs quickly.
// maxSegmentBytes / recordSize / drainReserveBytes match the deployment values.
pqueue::Config makeFuzzConfig(pqueue::FullQueuePolicy policy = pqueue::FullQueuePolicy::RejectNewest) {
    pqueue::Config cfg;
    cfg.basePath          = kFuzzSpoolDir.string();
    cfg.maxSegmentBytes   = 4096;
    cfg.reservedBytes     = 128 * 1024;
    cfg.drainReserveBytes = 4096;
    cfg.minFreeBytes      = 0;
    cfg.recordSizeBytes   = 492;
    cfg.fullQueuePolicy   = policy;
    return cfg;
}

std::size_t rangeCount(pqueue::Queue& q) {
    auto* s = dynamic_cast<pqueue::AppendLogStore*>(q.store_.get());
    return s ? s->manifestRanges().size() : 0;
}

// Drain to empty. Returns false and sets error on the first failing pop.
bool drainToEmpty(pqueue::Queue& q, pqueue::Status& error) {
    while (q.stats().count > 0) {
        const auto st = q.pop();
        if (!st.ok()) { error = st; return false; }
    }
    return true;
}

// Simple LCG — same constants as pqueue_compaction_sim for cross-test reproducibility.
struct Rng {
    std::uint32_t state;
    explicit Rng(std::uint32_t seed) : state(seed) {}
    double next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<double>(state) / 4294967295.0;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Scenario 1: fill → rewrite storm → drain
//
// Fills the queue to capacity with live records, then hammers rewriteFront to
// accumulate dead copies in old segments (the retry-storm pattern that caused
// the original incident). Interleaves compactIdle to drive range fragmentation.
// Finally drains to empty.
//
// Invariant: every pop succeeds — no RangeLimitExceeded, no WriteFailed.
// The drain reserve must cover the tombstones needed to reach the first
// fully-dead front segment.
// ---------------------------------------------------------------------------
TEST_CASE("fuzz: fill then rewrite-storm then drain") {
    cleanFuzzSpool();
    pqueue::Queue queue(makeFuzzConfig());

    const std::string payload(492, 'x');
    const std::string rewritten(492, 'r');
    std::size_t maxRangesObserved = 0;

    // Phase 1: fill to capacity.
    while (queue.enqueue(payload).ok()) {}
    REQUIRE_GT(queue.stats().count, 0U);

    // Phase 2: pop ~25% to create dead bytes in old segments.
    // Without dead bytes rewriteFront returns QueueFull immediately (compaction
    // is a no-op on all-live data). The interesting case requires mixed live/dead.
    const std::uint32_t toPop = queue.stats().count / 4;
    for (std::uint32_t i = 0; i < toPop; ++i) REQUIRE(queue.pop().ok());

    // Phase 3: rewrite storm — hammer the head to accumulate dead copies in the tail.
    // rewriteRecord's admission loop compacts dead bytes as needed before each write.
    // QueueFull is the expected exit once compaction can no longer reclaim enough.
    for (int i = 0; i < 200; ++i) {
        maxRangesObserved = std::max(maxRangesObserved, rangeCount(queue));
        const auto st = queue.rewriteFront(rewritten);
        if (st.code == pqueue::StatusCode::QueueFull) break;
        REQUIRE(st.ok());
        if (i % 10 == 9) queue.compactIdle(4);
    }
    maxRangesObserved = std::max(maxRangesObserved, rangeCount(queue));

    // Phase 4: drain to empty.
    pqueue::Status err;
    REQUIRE_MESSAGE(drainToEmpty(queue, err),
        "pop failed during drain: code=" << static_cast<int>(err.code)
        << " msg=" << err.message
        << " ranges_seen=" << maxRangesObserved);

    CHECK_EQ(queue.stats().count, 0U);
    MESSAGE("max sealed ranges observed: " << maxRangesObserved);
}

// ---------------------------------------------------------------------------
// Scenario 2: random interleaved enqueue / rewriteFront / pop near capacity
//
// DropOldest keeps the queue near-full throughout a 500-step seeded random
// walk mixing enqueue (50%), rewriteFront (30%), pop (15%), and compactIdle
// (5%). Drains the remainder after the loop.
//
// Invariant: every pop succeeds.
// ---------------------------------------------------------------------------
TEST_CASE("fuzz: random interleaved near-capacity - pop always succeeds") {
    cleanFuzzSpool();
    pqueue::Queue queue(makeFuzzConfig(pqueue::FullQueuePolicy::DropOldest));

    const std::string payload(492, 'x');
    const std::string rewritten(492, 'r');
    std::size_t maxRangesObserved = 0;
    int unexpectedErrors = 0;

    Rng rng(0xdeadbeef);
    for (int i = 0; i < 500; ++i) {
        const double r = rng.next();
        if (r < 0.50) {
            // enqueue: DropOldest evicts if full so this should never block.
            const auto st = queue.enqueue(payload);
            if (!st.ok()) {
                ++unexpectedErrors;
                MESSAGE("unexpected enqueue error at step " << i
                    << ": code=" << static_cast<int>(st.code)
                    << " msg=" << st.message);
            }
        } else if (r < 0.80) {
            if (queue.stats().count > 0) {
                const auto st = queue.rewriteFront(rewritten);
                // QueueFull is expected when all-live data fills the budget.
                if (!st.ok() && st.code != pqueue::StatusCode::QueueFull) {
                    ++unexpectedErrors;
                    MESSAGE("unexpected rewriteFront error at step " << i
                        << ": code=" << static_cast<int>(st.code));
                }
            }
        } else if (r < 0.95) {
            if (queue.stats().count > 0) {
                const auto st = queue.pop();
                CHECK_MESSAGE(st.ok(),
                    "pop failed at step " << i
                    << ": code=" << static_cast<int>(st.code)
                    << " msg=" << st.message);
                if (!st.ok()) ++unexpectedErrors;
            }
        } else {
            queue.compactIdle(2);
        }
        maxRangesObserved = std::max(maxRangesObserved, rangeCount(queue));
    }

    // Drain remainder.
    pqueue::Status err;
    const bool drained = drainToEmpty(queue, err);
    CHECK_MESSAGE(drained,
        "drain failed: code=" << static_cast<int>(err.code)
        << " msg=" << err.message);

    CHECK_EQ(unexpectedErrors, 0);
    CHECK_EQ(queue.stats().count, 0U);
    MESSAGE("max sealed ranges observed: " << maxRangesObserved);
}

#endif // !ARDUINO

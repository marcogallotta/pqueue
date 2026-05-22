#ifndef ARDUINO

// Open private members of Queue before any STL filesystem/sstream headers are
// pulled in. The pragma once guard on queue.h prevents a second include when
// pqueue_append_log_support.h is included below.
#define private public
#include "pqueue/queue.h"
#undef private

#include "pqueue_append_log_support.h"

#include <limits>
#include <string>
#include <vector>

using namespace pqueue::append_log_detail;

static constexpr std::uint32_t kMax = std::numeric_limits<std::uint32_t>::max();

namespace {

struct VisitCtx {
    std::vector<std::string> records;
    std::vector<std::uint32_t> sequences;
    std::vector<std::uint32_t> ordinals;
};

bool captureVisitor(void* ctx, const std::string& rec, std::uint32_t seq, std::uint32_t ord) {
    auto* c = static_cast<VisitCtx*>(ctx);
    c->records.push_back(rec);
    c->sequences.push_back(seq);
    c->ordinals.push_back(ord);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Group 1: near-UINT32_MAX append-log behavior
// ---------------------------------------------------------------------------

TEST_CASE("seq-edges: near-UINT32_MAX records survive remount and drain in FIFO order") {
    // Plant two records at the highest valid sequences (kMax-2, kMax-1) in a
    // single tail segment, remount through Queue, and verify pop order.
    plantLayout({
        .ranges   = {},
        .tail     = 1u,
        .next     = 2u,
        .segments = {{1u, kMax - 2,
            serializeEnqueueEvent(kMax - 2, "penultimate") +
            serializeEnqueueEvent(kMax - 1, "last")}},
    });

    auto cfg = makeConfig();
    {
        pqueue::Queue q(cfg);
        CHECK_EQ(q.stats().count, 2U);
        std::string out;
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "penultimate");
        CHECK(q.pop().ok());
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "last");
        CHECK(q.pop().ok());
        CHECK_EQ(q.stats().count, 0U);
    }
}

TEST_CASE("seq-edges: next enqueue after draining near-UINT32_MAX queue fails with SequenceExhausted") {
    // After both records are popped index_.tail == kMax.
    // commitEnqueue(kMax, ...) must return SequenceExhausted.
    plantLayout({
        .ranges   = {},
        .tail     = 1u,
        .next     = 2u,
        .segments = {{1u, kMax - 2,
            serializeEnqueueEvent(kMax - 2, "penultimate") +
            serializeEnqueueEvent(kMax - 1, "last")}},
    });

    auto cfg = makeConfig();
    {
        pqueue::Queue q(cfg);
        CHECK(q.pop().ok()); // kMax-2
        CHECK(q.pop().ok()); // kMax-1

        const auto st = q.enqueue("overflow");
        CHECK_FALSE(st.ok());
        CHECK_EQ(st.code, pqueue::StatusCode::SequenceExhausted);
        CHECK_EQ(q.stats().count, 0U); // failed enqueue left no junk in RAM
    }
    {
        pqueue::Queue q(cfg);
        std::string out;
        CHECK_EQ(q.stats().count, 0U); // no durable junk after remount
        CHECK_FALSE(q.peek(out).ok());
    }
}

// ---------------------------------------------------------------------------
// Group 2: near-UINT32_MAX rewrite
// ---------------------------------------------------------------------------

TEST_CASE("seq-edges: rewriteFront near UINT32_MAX persists across remount with correct order") {
    // Plant kMax-2 and kMax-1, rewrite the front, remount, verify.
    plantLayout({
        .ranges   = {},
        .tail     = 1u,
        .next     = 2u,
        .segments = {{1u, kMax - 2,
            serializeEnqueueEvent(kMax - 2, "original") +
            serializeEnqueueEvent(kMax - 1, "second")}},
    });

    auto cfg = makeConfig();
    {
        pqueue::Queue q(cfg);
        CHECK(q.rewriteFront("rewritten").ok());
    }
    {
        pqueue::Queue q(cfg);
        std::string out;
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "rewritten");
        CHECK(q.pop().ok());
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "second");
        CHECK_EQ(q.stats().count, 1U);
    }
}

// ---------------------------------------------------------------------------
// Group 3: near-UINT32_MAX compaction
// ---------------------------------------------------------------------------

TEST_CASE("seq-edges: compaction with near-UINT32_MAX sequences preserves ordering and payloads") {
    // Layout: gen=1 (sealed range) holds kMax-3 (dead) and kMax-2 (live).
    //         gen=2 (tail) holds kMax-1 (live) and a POP tombstone for kMax-3.
    // Dead bytes in gen=1 from the popped kMax-3 record make it the compaction candidate.
    // After compaction and remount, kMax-2 and kMax-1 must survive in order.
    const std::string p1(10, 'a'); // kMax-3, dead after pop
    const std::string p2(10, 'b'); // kMax-2, live
    const std::string p3(10, 'c'); // kMax-1, live

    plantLayout({
        .ranges   = {{1u, 1u}},
        .tail     = 2u,
        .next     = 3u,
        .segments = {
            {1u, kMax - 3,
                serializeEnqueueEvent(kMax - 3, p1) +
                serializeEnqueueEvent(kMax - 2, p2)},
            {2u, kMax - 1,
                serializeEnqueueEvent(kMax - 1, p3) +
                serializePopEvent(kMax - 3)},
        },
    });

    auto cfg = makeStoreConfig();
    cfg.maxSegmentBytes = 4096;

    {
        pqueue::AppendLogStore store(cfg);
        REQUIRE(store.mount().ok());

        std::string out;
        CHECK_FALSE(store.readRecord(kMax - 3, out).ok());
        CHECK(store.readRecord(kMax - 2, out).ok()); CHECK_EQ(out, p2);
        CHECK(store.readRecord(kMax - 1, out).ok()); CHECK_EQ(out, p3);

        const auto range = store.chooseCompactionRange();
        REQUIRE(range.has_value());
        CHECK_EQ(range->startGen, 1u);

        auto st = store.compactOneSegment();
        CHECK(st.ok());
        CHECK_FALSE(st.isNoOp());

        CHECK_FALSE(store.readRecord(kMax - 3, out).ok());
        CHECK(store.readRecord(kMax - 2, out).ok()); CHECK_EQ(out, p2);
        CHECK(store.readRecord(kMax - 1, out).ok()); CHECK_EQ(out, p3);
    }

    {
        pqueue::AppendLogStore store(cfg);
        REQUIRE(store.mount().ok());
        std::string out;
        CHECK_FALSE(store.readRecord(kMax - 3, out).ok());
        CHECK(store.readRecord(kMax - 2, out).ok()); CHECK_EQ(out, p2);
        CHECK(store.readRecord(kMax - 1, out).ok()); CHECK_EQ(out, p3);
    }

    // Verify the user-facing pop order through Queue after compaction.
    {
        auto qcfg = makeConfig();
        pqueue::Queue q(qcfg);
        std::string out;
        CHECK(q.peek(out).ok()); CHECK_EQ(out, p2);
        CHECK(q.pop().ok());
        CHECK(q.peek(out).ok()); CHECK_EQ(out, p3);
        CHECK(q.pop().ok());
        CHECK_EQ(q.stats().count, 0U);
        CHECK_FALSE(q.peek(out).ok());
    }
}

// ---------------------------------------------------------------------------
// Group 4: nonzero-head rewrite/index
// ---------------------------------------------------------------------------

TEST_CASE("seq-edges: rewriteFront with nonzero head targets the correct sequence after remount") {
    // Enqueue A/B/C, pop A (head moves to 1), rewriteFront B→X, remount.
    // Front must be X (seq=1), then C (seq=2). Verifies that a nonzero index
    // head does not cause the rewrite to target the wrong sequence.
    cleanSpool();
    auto cfg = makeConfig();
    {
        pqueue::Queue q(cfg);
        CHECK(q.enqueue("A").ok()); // seq=0
        CHECK(q.enqueue("B").ok()); // seq=1
        CHECK(q.enqueue("C").ok()); // seq=2
        CHECK(q.pop().ok());        // pops seq=0; head becomes 1
        CHECK(q.rewriteFront("X").ok()); // rewrites seq=1
    }
    {
        pqueue::Queue q(cfg);
        std::string out;
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "X");
        CHECK(q.pop().ok());
        CHECK(q.peek(out).ok()); CHECK_EQ(out, "C");
        CHECK(q.pop().ok());
        CHECK_EQ(q.stats().count, 0U);
    }
}

// ---------------------------------------------------------------------------
// Group 5: visitRecords after pop/rewrite/compaction
// ---------------------------------------------------------------------------

TEST_CASE("seq-edges: visitRecords reports correct records, sequences, and ordinals after pop/rewrite/compaction") {
    // maxSegmentBytes=70 fits exactly two 1-byte records (20+25+25=70).
    // A and B fill gen=1; C goes to gen=2 after rotation.
    // After pop(A) and rewrite(B→X), gen=1 is fully dead (A popped, B rewritten).
    // compactIdle removes gen=1 and rotates/compacts the live records.
    // visitRecords must then yield: X (seq=1, ordinal=0) and C (seq=2, ordinal=1).
    cleanSpool();
    auto cfg = makeConfig();
    cfg.maxSegmentBytes = 70;

    pqueue::Queue q(cfg);
    CHECK(q.enqueue("A").ok()); // seq=0 → gen=1
    CHECK(q.enqueue("B").ok()); // seq=1 → gen=1 full; rotation creates gen=2
    CHECK(q.enqueue("C").ok()); // seq=2 → gen=2 tail
    CHECK(q.pop().ok());        // pops seq=0; head=1, count=2
    CHECK(q.rewriteFront("X").ok()); // rewrites seq=1 → dead bytes in gen=1

    const auto result = q.compactIdle(4);
    CHECK(result.status.ok());

    VisitCtx ctx;
    CHECK(q.visitRecords(captureVisitor, &ctx).ok());

    REQUIRE_EQ(ctx.records.size(), 2U);
    CHECK_EQ(ctx.records[0],   "X"); CHECK_EQ(ctx.sequences[0], 1U); CHECK_EQ(ctx.ordinals[0], 0U);
    CHECK_EQ(ctx.records[1],   "C"); CHECK_EQ(ctx.sequences[1], 2U); CHECK_EQ(ctx.ordinals[1], 1U);
}

#endif // !ARDUINO

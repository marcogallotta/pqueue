#include "counting_file_system.h"
#include "pqueue/queue.h"
#include "pqueue/outbox.h"
#include "pqueue/http/outbox.h"
#include "pqueue/storage_common.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Timings {
    std::vector<double> us;

    void add(Clock::time_point start, Clock::time_point end) {
        us.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    }

    double avg() const {
        if (us.empty()) return 0.0;
        return std::accumulate(us.begin(), us.end(), 0.0) / static_cast<double>(us.size());
    }

    double p95() const {
        if (us.empty()) return 0.0;
        auto copy = us;
        std::sort(copy.begin(), copy.end());
        const std::size_t index = static_cast<std::size_t>((copy.size() - 1) * 0.95);
        return copy[index];
    }

    double max() const {
        if (us.empty()) return 0.0;
        return *std::max_element(us.begin(), us.end());
    }
};

struct ScenarioResult {
    std::string scenario;
    std::uint32_t records = 0;
    std::uint32_t payloadBytes = 0;
    Timings timings;
    FsCounters fs;
    pqueue::Status status = pqueue::Status::success();
};

std::string tempBase(const std::string& name) {
    auto root = std::filesystem::temp_directory_path() / "pqueue_profiling" / name;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root.string();
}

std::string payload(std::size_t size, char fill = 'x') {
    std::string out(size, fill);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>('a' + (i % 26));
    }
    return out;
}

pqueue::Config queueConfig(const std::string& basePath, std::shared_ptr<CountingFileSystem> fs, std::uint32_t recordSizeBytes, std::uint32_t requestedRecords) {
    pqueue::Config cfg;
    cfg.basePath = basePath;
    cfg.recordSizeBytes = recordSizeBytes;
    cfg.reservedBytes = requestedRecords * static_cast<std::uint32_t>(pqueue::storage_detail::kRecordHeaderBytes + recordSizeBytes);
    cfg.storageBackend = pqueue::StorageBackend::Posix;
    cfg.fileSystem = fs;
    return cfg;
}

void recordTiming(Timings& timings, const std::function<pqueue::Status()>& fn, pqueue::Status& last) {
    const auto start = Clock::now();
    last = fn();
    const auto end = Clock::now();
    timings.add(start, end);
}

ScenarioResult scenarioQueueEnqueue(std::uint32_t records, std::uint32_t payloadBytes) {
    auto fs = std::make_shared<CountingFileSystem>(pqueue::makePosixFileSystem());
    auto cfg = queueConfig(tempBase("queue_enqueue"), fs, 1024, records + 8);
    pqueue::Queue q(cfg);
    (void)q.stats();
    const auto data = payload(payloadBytes);
    fs->resetCounters();

    ScenarioResult result{};
    result.scenario = "queue_enqueue";
    result.records = records;
    result.payloadBytes = payloadBytes;
    for (std::uint32_t i = 0; i < records; ++i) {
        recordTiming(result.timings, [&] { return q.enqueue(data); }, result.status);
        if (!result.status.ok()) break;
    }
    result.fs = fs->counters();
    return result;
}

ScenarioResult scenarioQueuePeekPop(std::uint32_t records, std::uint32_t payloadBytes) {
    auto fs = std::make_shared<CountingFileSystem>(pqueue::makePosixFileSystem());
    auto cfg = queueConfig(tempBase("queue_peek_pop"), fs, 1024, records + 8);
    pqueue::Queue q(cfg);
    const auto data = payload(payloadBytes);
    for (std::uint32_t i = 0; i < records; ++i) {
        auto st = q.enqueue(data);
        if (!st.ok()) return {"queue_peek_pop_setup", records, payloadBytes, {}, fs->counters(), st};
    }
    fs->resetCounters();

    ScenarioResult result{};
    result.scenario = "queue_peek_pop";
    result.records = records;
    result.payloadBytes = payloadBytes;
    std::string out;
    for (std::uint32_t i = 0; i < records; ++i) {
        recordTiming(result.timings, [&] {
            auto st = q.peek(out);
            if (!st.ok()) return st;
            return q.pop();
        }, result.status);
        if (!result.status.ok()) break;
    }
    result.fs = fs->counters();
    return result;
}

ScenarioResult scenarioValidateFull(std::uint32_t records, std::uint32_t payloadBytes) {
    auto fs = std::make_shared<CountingFileSystem>(pqueue::makePosixFileSystem());
    auto cfg = queueConfig(tempBase("validate_full"), fs, 1024, records + 8);
    pqueue::Queue q(cfg);
    const auto data = payload(payloadBytes);
    for (std::uint32_t i = 0; i < records; ++i) {
        auto st = q.enqueue(data);
        if (!st.ok()) return {"validate_full_setup", records, payloadBytes, {}, fs->counters(), st};
    }
    fs->resetCounters();

    ScenarioResult result{};
    result.scenario = "validate_full";
    result.records = records;
    result.payloadBytes = payloadBytes;
    recordTiming(result.timings, [&] {
        const auto validation = q.validate();
        return validation.ok ? pqueue::Status::success() : pqueue::Status::failure(pqueue::StatusCode::InvalidIndex, "validation failed");
    }, result.status);
    result.fs = fs->counters();
    return result;
}

struct FakeClock {
    std::uint64_t now = 0;
};

std::uint64_t fakeClock(void* context) {
    return static_cast<FakeClock*>(context)->now;
}

struct FakeTransport final : public pqueue::http::Transport {
    bool online = true;

    pqueue::http::Response post(
        const char*,
        const pqueue::http::Header*,
        std::size_t,
        const std::uint8_t*,
        std::size_t
    ) override {
        if (!online) {
            return {pqueue::http::kNoStatusCode, pqueue::http::TransportError::Network};
        }
        return {200, pqueue::http::TransportError::None};
    }
};

struct PreparedHttpOutbox {
    std::shared_ptr<CountingFileSystem> fs;
    std::unique_ptr<pqueue::http::Outbox> outbox;
    std::unique_ptr<FakeTransport> transport;
    std::unique_ptr<FakeClock> clock;
    pqueue::Status status = pqueue::Status::success();
};

PreparedHttpOutbox prepareHttpOutboxBacklog(std::uint32_t records, std::uint32_t payloadBytes, const std::string& baseName) {
    PreparedHttpOutbox prepared;
    prepared.fs = std::make_shared<CountingFileSystem>(pqueue::makePosixFileSystem());
    prepared.transport = std::make_unique<FakeTransport>();
    prepared.clock = std::make_unique<FakeClock>();

    pqueue::http::Config cfg;
    cfg.queue = queueConfig(tempBase(baseName), prepared.fs, 2048, records + 8);
    cfg.outbox.maxDrainAttemptsPerSecond = 1000;
    cfg.outbox.retryDelayMs = 1;
    cfg.baseUrl = "https://example.test";

    prepared.outbox = std::make_unique<pqueue::http::Outbox>(cfg, *prepared.transport, fakeClock, prepared.clock.get());
    const auto body = payload(payloadBytes);
    prepared.transport->online = false;
    for (std::uint32_t i = 0; i < records; ++i) {
        const auto submit = prepared.outbox->submitPost("/readings", body);
        if (submit.status != pqueue::SubmitStatus::Queued) {
            prepared.status = submit.detail;
            break;
        }
        prepared.clock->now += 2;
    }
    return prepared;
}

ScenarioResult scenarioHttpOutboxOfflineSubmit(std::uint32_t records, std::uint32_t payloadBytes) {
    auto fs = std::make_shared<CountingFileSystem>(pqueue::makePosixFileSystem());
    auto transport = std::make_unique<FakeTransport>();
    auto clock = std::make_unique<FakeClock>();

    pqueue::http::Config cfg;
    cfg.queue = queueConfig(tempBase("http_outbox_offline_submit"), fs, 2048, records + 8);
    cfg.outbox.maxDrainAttemptsPerSecond = 1000;
    cfg.outbox.retryDelayMs = 1;
    cfg.baseUrl = "https://example.test";

    pqueue::http::Outbox outbox(cfg, *transport, fakeClock, clock.get());
    (void)outbox.stats();
    const auto body = payload(payloadBytes);
    transport->online = false;
    fs->resetCounters();

    ScenarioResult result{};
    result.scenario = "http_offline_submit";
    result.records = records;
    result.payloadBytes = payloadBytes;
    for (std::uint32_t i = 0; i < records; ++i) {
        const auto start = Clock::now();
        const auto submit = outbox.submitPost("/readings", body);
        const auto end = Clock::now();
        result.timings.add(start, end);
        if (submit.status != pqueue::SubmitStatus::Queued) {
            result.status = submit.detail;
            break;
        }
        clock->now += 2;
    }
    result.fs = fs->counters();
    return result;
}

ScenarioResult scenarioHttpOutboxDrainBacklog(std::uint32_t records, std::uint32_t payloadBytes) {
    auto prepared = prepareHttpOutboxBacklog(records, payloadBytes, "http_outbox_drain_backlog");
    ScenarioResult result{};
    result.scenario = "http_drain_backlog";
    result.records = records;
    result.payloadBytes = payloadBytes;
    if (!prepared.status.ok()) {
        result.status = prepared.status;
        result.fs = prepared.fs->counters();
        return result;
    }

    prepared.transport->online = true;
    prepared.fs->resetCounters();
    for (std::uint32_t i = 0; i < records; ++i) {
        const auto start = Clock::now();
        const auto drain = prepared.outbox->drainUpTo(1);
        const auto end = Clock::now();
        result.timings.add(start, end);
        if (drain.queueError || drain.sendError || drain.sent != 1) {
            result.status = drain.detail;
            break;
        }
        prepared.clock->now += 2;
    }
    result.fs = prepared.fs->counters();
    return result;
}

void printHeader() {
    std::cout << std::left
              << std::setw(24) << "scenario"
              << std::right
              << std::setw(8) << "records"
              << std::setw(9) << "payload"
              << std::setw(10) << "avg_us"
              << std::setw(10) << "p95_us"
              << std::setw(10) << "max_us"
              << std::setw(8) << "readAt"
              << std::setw(8) << "writeAt"
              << std::setw(10) << "writeFile"
              << std::setw(8) << "rename"
              << std::setw(8) << "remove"
              << std::setw(12) << "bytesW"
              << std::setw(12) << "bytesR"
              << std::setw(12) << "status"
              << "\n";
}

void printResult(const ScenarioResult& r) {
    std::cout << std::left
              << std::setw(24) << r.scenario
              << std::right
              << std::setw(8) << r.records
              << std::setw(9) << r.payloadBytes
              << std::setw(10) << static_cast<std::uint64_t>(r.timings.avg())
              << std::setw(10) << static_cast<std::uint64_t>(r.timings.p95())
              << std::setw(10) << static_cast<std::uint64_t>(r.timings.max())
              << std::setw(8) << r.fs.readAt
              << std::setw(8) << r.fs.writeAt
              << std::setw(10) << r.fs.writeFile
              << std::setw(8) << r.fs.renameFile
              << std::setw(8) << r.fs.removeFile
              << std::setw(12) << r.fs.bytesWritten
              << std::setw(12) << r.fs.bytesRead
              << std::setw(12) << pqueue::statusCodeName(r.status.code)
              << "\n";
}

void printRedFlags(const std::vector<ScenarioResult>& results) {
    std::cout << "\nred-flag notes:\n";
    for (const auto& r : results) {
        if (!r.status.ok()) {
            std::cout << "- " << r.scenario << " failed: " << pqueue::statusCodeName(r.status.code)
                      << " (" << r.status.message << ")\n";
        }
        if ((r.scenario == "queue_enqueue" || r.scenario == "http_offline_submit") && r.fs.writeAt > r.records * 3ULL + 16ULL) {
            std::cout << "- " << r.scenario << " has unexpectedly high writeAt count; expected slot + journal/checkpoint writes only\n";
        }
        if (r.scenario == "queue_enqueue" && r.fs.readAt > r.records * 2ULL + 8ULL) {
            std::cout << "- " << r.scenario << " has unexpected readAt activity on enqueue hot path\n";
        }
        if (r.scenario == "http_offline_submit" && r.fs.readAt > r.records * 3ULL + 8ULL) {
            std::cout << "- " << r.scenario << " has unexpected readAt activity on http submit hot path\n";
        }
        if (r.scenario == "validate_full" && r.fs.writeAt != 0) {
            std::cout << "- validate_full wrote to storage; validation should be read-only\n";
        }
        if (r.records > 0 && r.fs.metadataOps() > r.records * 10ULL) {
            std::cout << "- " << r.scenario << " has high metadata op count; inspect atomic metadata path\n";
        }
    }
}

void usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [queue|validate|outbox|all] [records] [payloadBytes]\n";
}

} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc >= 2 ? argv[1] : "all";
    const std::uint32_t records = argc >= 3 ? static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10)) : 500;
    const std::uint32_t payloadBytes = argc >= 4 ? static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10)) : 256;

    if (mode != "queue" && mode != "validate" && mode != "outbox" && mode != "all") {
        usage(argv[0]);
        return 2;
    }

    std::vector<ScenarioResult> results;
    if (mode == "queue" || mode == "all") {
        results.push_back(scenarioQueueEnqueue(records, payloadBytes));
        results.push_back(scenarioQueuePeekPop(records, payloadBytes));
    }
    if (mode == "validate" || mode == "all") {
        results.push_back(scenarioValidateFull(records, payloadBytes));
    }
    if (mode == "outbox" || mode == "all") {
        results.push_back(scenarioHttpOutboxOfflineSubmit(records, payloadBytes));
        results.push_back(scenarioHttpOutboxDrainBacklog(records, payloadBytes));
    }

    printHeader();
    for (const auto& r : results) {
        printResult(r);
    }
    printRedFlags(results);

    const bool failed = std::any_of(results.begin(), results.end(), [](const ScenarioResult& r) { return !r.status.ok(); });
    return failed ? 1 : 0;
}

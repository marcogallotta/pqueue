#include "counting_file_system.h"
#include "pqueue/append_log_common.h"
#include "pqueue/http/outbox.h"
#include "pqueue/queue.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Benchmark-wide store config constants.
// maxTotalBytes is sized for N=1000 records at the largest recordSizeBytes
// in the test matrix (2048B), so no scenario rejects enqueues by capacity.
// At 2048B payload only one record fits per 4096B segment, so each record
// pays the full 20B segment header cost. Formula accounts for that:
//   kSegmentHeaderBytes=20, kEnqueueOverheadBytes=24
//   (1000+8) * (20 + 24 + 2048) = 1008 * 2092 = 2,108,736
// ---------------------------------------------------------------------------

static constexpr std::uint32_t kBenchmarkN           = 1000;
static constexpr std::uint32_t kMaxRecordSizeBytes   = 2048;
static constexpr std::uint32_t kBenchmarkMaxTotalBytes =
    (kBenchmarkN + 8u) * (20u + 24u + kMaxRecordSizeBytes);

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

inline FsLatency littleFsSimLatency() {
    FsLatency lat;
    lat.readFileUs         = 345;
    lat.readAtUs           = 345;
    lat.writeFileFixedUs   = 1380;
    lat.writeFilePerKbUs   = 518;
    lat.writeAtUs          = 138;
    lat.removeFileUs       = 166;
    lat.listFilesBaseUs    = 690;
    lat.listFilesPerFileUs = 86;
    return lat;
}

std::string getGitHash() {
    FILE* f = popen("git rev-parse --short HEAD 2>/dev/null", "r");
    if (!f) return "unknown";
    char buf[64] = {};
    if (!fgets(buf, sizeof(buf), f)) { pclose(f); return "unknown"; }
    pclose(f);
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s.empty() ? "unknown" : s;
}

std::string tempBase(const std::string& name) {
    auto root = std::filesystem::temp_directory_path() / "pqueue_benchmark" / name;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root.string();
}

std::string makePayload(std::size_t size) {
    std::string out(size, 'x');
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<char>('a' + (i % 26));
    return out;
}

// reservedBytes uses kBenchmarkMaxTotalBytes so the reported config matches
// the actual capacity used across all benchmark runs.
pqueue::Config queueConfig(const std::string& basePath,
                            std::shared_ptr<CountingFileSystem> fs,
                            std::uint32_t recordSizeBytes) {
    pqueue::Config cfg;
    cfg.basePath        = basePath;
    cfg.recordSizeBytes = recordSizeBytes;
    cfg.reservedBytes   = kBenchmarkMaxTotalBytes;
    cfg.maxSegments     = 200;
    cfg.storageBackend  = pqueue::StorageBackend::Posix;
    cfg.fileSystem      = fs;
    return cfg;
}

struct FakeClock { std::uint64_t now = 0; };
std::uint64_t fakeClock(void* ctx) { return static_cast<FakeClock*>(ctx)->now; }

struct FakeTransport final : public pqueue::http::Transport {
    bool online = true;
    pqueue::http::Response post(const char*, const pqueue::http::Header*, std::size_t,
                                 const std::uint8_t*, std::size_t) override {
        return online
            ? pqueue::http::Response{200, pqueue::http::TransportError::None}
            : pqueue::http::Response{pqueue::http::kNoStatusCode, pqueue::http::TransportError::Network};
    }
};

// ---------------------------------------------------------------------------
// Sample collection
// ---------------------------------------------------------------------------

struct SampleSet {
    std::vector<double> wallUs;
    std::vector<double> simUs;

    void add(double wall, double sim) { wallUs.push_back(wall); simUs.push_back(sim); }

    static double pct(std::vector<double> v, double p) {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        return v[static_cast<std::size_t>((v.size() - 1) * p)];
    }
    std::uint64_t wallP50() const { return static_cast<std::uint64_t>(pct(wallUs, 0.50)); }
    std::uint64_t wallP90() const { return static_cast<std::uint64_t>(pct(wallUs, 0.90)); }
    std::uint64_t wallP99() const { return static_cast<std::uint64_t>(pct(wallUs, 0.99)); }
    std::uint64_t wallMax() const {
        if (wallUs.empty()) return 0;
        return static_cast<std::uint64_t>(*std::max_element(wallUs.begin(), wallUs.end()));
    }
    double simP99Ms() const { return pct(simUs, 0.99) / 1000.0; }
    double simMaxMs() const {
        if (simUs.empty()) return 0.0;
        return *std::max_element(simUs.begin(), simUs.end()) / 1000.0;
    }
};

// ---------------------------------------------------------------------------
// Result types
// ---------------------------------------------------------------------------

struct BenchmarkConfig {
    std::string   gitHash;
    // AppendLogConfig defaults (maxSegments, maxSegmentBytes, maxOutputSegments)
    // plus the shared maxTotalBytes = kBenchmarkMaxTotalBytes, which is what
    // every scenario passes as reservedBytes.
    std::uint32_t maxSegmentBytes    = 4096;
    std::uint32_t maxSegments        = 200;
    std::uint32_t maxTotalBytes      = kBenchmarkMaxTotalBytes;
    std::uint32_t maxOutputSegments  = 8;
    std::string   platform           = "posix";
};

struct BenchmarkResult {
    std::string   scenario;
    std::uint32_t payloadBytes = 0;
    std::uint32_t records      = 0;
    std::uint32_t repeat       = 1;
    // host wall-clock percentiles (us)
    std::uint64_t p50_us = 0, p90_us = 0, p99_us = 0, max_us = 0;
    // simulated per-op latency (ms)
    double sim_p99_ms = 0.0, sim_max_ms = 0.0;
    // bytes written/read per payload byte / per record
    double write_amp = 0.0, read_bpp = 0.0;
    // I/O op counts, per-run average across repeats
    std::uint64_t writeFile = 0, writeAt = 0, readAt = 0, remove = 0;
    bool ok = true;
};

// ---------------------------------------------------------------------------
// Strict-mode invariant checks
// ---------------------------------------------------------------------------

// Returns a list of violation messages. Empty = all invariants hold.
std::vector<std::string> strictViolations(const BenchmarkResult& r) {
    std::vector<std::string> v;
    if (!r.ok)
        v.push_back("ok=false: not all operations succeeded");
    if (r.scenario == "enqueue") {
        if (r.writeAt != r.records)
            v.push_back("writeAt != records: expected exactly one writeAt per enqueue");
        if (r.readAt != 0)
            v.push_back("readAt != 0: unexpected reads on the enqueue hot path");
        if (r.read_bpp != 0.0)
            v.push_back("read_bpp != 0: unexpected reads on the enqueue hot path");
        if (r.remove != 0)
            v.push_back("remove != 0: unexpected segment removal on enqueue-only path");
    }
    if (r.scenario == "peek_pop") {
        if (r.writeAt != r.records)
            v.push_back("writeAt != records: expected exactly one writeAt (POP event) per pop");
        if (r.readAt != r.records)
            v.push_back("readAt != records: expected exactly one readAt (peek) per record");
        if (r.read_bpp != static_cast<double>(r.payloadBytes))
            v.push_back("read_bpp != payloadBytes: bytes read per record does not match payload size");
        if (r.remove != 0)
            v.push_back("remove != 0: unexpected segment removal on pop-only path");
    }
    if (r.scenario == "outbox_offline_submit") {
        if (r.writeAt != r.records)
            v.push_back("writeAt != records: expected exactly one writeAt per enqueue");
        if (r.read_bpp != 0.0)
            v.push_back("read_bpp != 0: unexpected reads on the enqueue-only path");
        if (r.remove != 0)
            v.push_back("remove != 0: unexpected segment removal on enqueue-only path");
    }
    return v;
}

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------

BenchmarkResult scenarioEnqueue(std::uint32_t payloadBytes,
                                 std::uint32_t records,
                                 std::uint32_t repeat) {
    const auto lat  = littleFsSimLatency();
    const auto data = makePayload(payloadBytes);
    SampleSet samples;

    std::uint64_t totalDataBytesWritten = 0, totalBytesRead = 0;
    std::uint64_t totalWriteFile = 0, totalWriteAt = 0, totalReadAt = 0, totalRemove = 0;
    bool anyFail = false;

    for (std::uint32_t r = 0; r < repeat; ++r) {
        auto fs = std::make_shared<CountingFileSystem>(pqueue::makePosixFileSystem());
        fs->setLatency(lat);

        pqueue::Queue q(queueConfig(tempBase("enqueue"), fs, payloadBytes));
        (void)q.stats();  // trigger mount
        const auto before = fs->counters();

        for (std::uint32_t i = 0; i < records; ++i) {
            const std::uint64_t simBefore = fs->counters().simLatencyUs;
            const auto t0 = Clock::now();
            const auto st = q.enqueue(data);
            const auto t1 = Clock::now();
            samples.add(std::chrono::duration<double, std::micro>(t1 - t0).count(),
                        static_cast<double>(fs->counters().simLatencyUs - simBefore));
            if (!st.ok()) { anyFail = true; break; }
        }

        const auto after = fs->counters();
        totalDataBytesWritten += (after.bytesWritten - after.lockBytesWritten)
                               - (before.bytesWritten - before.lockBytesWritten);
        totalBytesRead    += after.bytesRead    - before.bytesRead;
        totalWriteFile    += after.writeFile    - before.writeFile;
        totalWriteAt      += after.writeAt      - before.writeAt;
        totalReadAt       += after.readAt       - before.readAt;
        totalRemove       += after.removeFile   - before.removeFile;
    }

    const auto totalOps = static_cast<std::uint64_t>(repeat) * records;

    BenchmarkResult res;
    res.scenario     = "enqueue";
    res.payloadBytes = payloadBytes;
    res.records      = records;
    res.repeat       = repeat;
    res.p50_us       = samples.wallP50();
    res.p90_us       = samples.wallP90();
    res.p99_us       = samples.wallP99();
    res.max_us       = samples.wallMax();
    res.sim_p99_ms   = samples.simP99Ms();
    res.sim_max_ms   = samples.simMaxMs();
    res.write_amp    = (totalOps > 0 && payloadBytes > 0)
                     ? static_cast<double>(totalDataBytesWritten) / static_cast<double>(totalOps * payloadBytes)
                     : 0.0;
    res.read_bpp     = totalOps > 0
                     ? static_cast<double>(totalBytesRead) / static_cast<double>(totalOps)
                     : 0.0;
    res.writeFile    = totalWriteFile / repeat;
    res.writeAt      = totalWriteAt   / repeat;
    res.readAt       = totalReadAt    / repeat;
    res.remove       = totalRemove    / repeat;
    res.ok           = !anyFail;
    return res;
}

BenchmarkResult scenarioPeekPop(std::uint32_t payloadBytes,
                                 std::uint32_t records,
                                 std::uint32_t repeat) {
    const auto lat  = littleFsSimLatency();
    const auto data = makePayload(payloadBytes);
    SampleSet samples;

    std::uint64_t totalDataBytesWritten = 0, totalBytesRead = 0;
    std::uint64_t totalWriteFile = 0, totalWriteAt = 0, totalReadAt = 0, totalRemove = 0;
    bool anyFail = false;

    for (std::uint32_t r = 0; r < repeat; ++r) {
        auto fs = std::make_shared<CountingFileSystem>(pqueue::makePosixFileSystem());
        fs->setLatency(lat);

        pqueue::Queue q(queueConfig(tempBase("peek_pop"), fs, payloadBytes));
        for (std::uint32_t i = 0; i < records; ++i) {
            if (!q.enqueue(data).ok()) { anyFail = true; break; }
        }
        if (anyFail) break;
        const auto before = fs->counters();

        std::string out;
        for (std::uint32_t i = 0; i < records; ++i) {
            const std::uint64_t simBefore = fs->counters().simLatencyUs;
            const auto t0 = Clock::now();
            auto st = q.peek(out);
            if (st.ok()) st = q.pop();
            const auto t1 = Clock::now();
            samples.add(std::chrono::duration<double, std::micro>(t1 - t0).count(),
                        static_cast<double>(fs->counters().simLatencyUs - simBefore));
            if (!st.ok()) { anyFail = true; break; }
        }

        const auto after = fs->counters();
        totalDataBytesWritten += (after.bytesWritten - after.lockBytesWritten)
                               - (before.bytesWritten - before.lockBytesWritten);
        totalBytesRead    += after.bytesRead    - before.bytesRead;
        totalWriteFile    += after.writeFile    - before.writeFile;
        totalWriteAt      += after.writeAt      - before.writeAt;
        totalReadAt       += after.readAt       - before.readAt;
        totalRemove       += after.removeFile   - before.removeFile;
    }

    const auto totalOps = static_cast<std::uint64_t>(repeat) * records;

    BenchmarkResult res;
    res.scenario     = "peek_pop";
    res.payloadBytes = payloadBytes;
    res.records      = records;
    res.repeat       = repeat;
    res.p50_us       = samples.wallP50();
    res.p90_us       = samples.wallP90();
    res.p99_us       = samples.wallP99();
    res.max_us       = samples.wallMax();
    res.sim_p99_ms   = samples.simP99Ms();
    res.sim_max_ms   = samples.simMaxMs();
    res.write_amp    = (totalOps > 0 && payloadBytes > 0)
                     ? static_cast<double>(totalDataBytesWritten) / static_cast<double>(totalOps * payloadBytes)
                     : 0.0;
    res.read_bpp     = totalOps > 0
                     ? static_cast<double>(totalBytesRead) / static_cast<double>(totalOps)
                     : 0.0;
    res.writeFile    = totalWriteFile / repeat;
    res.writeAt      = totalWriteAt   / repeat;
    res.readAt       = totalReadAt    / repeat;
    res.remove       = totalRemove    / repeat;
    res.ok           = !anyFail;
    return res;
}

BenchmarkResult scenarioOutboxOfflineSubmit(std::uint32_t payloadBytes,
                                             std::uint32_t records,
                                             std::uint32_t repeat) {
    const auto lat  = littleFsSimLatency();
    const auto body = makePayload(payloadBytes);
    SampleSet samples;

    std::uint64_t totalDataBytesWritten = 0, totalBytesRead = 0;
    std::uint64_t totalWriteFile = 0, totalWriteAt = 0, totalReadAt = 0, totalRemove = 0;
    bool anyFail = false;

    for (std::uint32_t r = 0; r < repeat; ++r) {
        auto fs = std::make_shared<CountingFileSystem>(pqueue::makePosixFileSystem());
        fs->setLatency(lat);

        FakeTransport transport;
        FakeClock clock;
        pqueue::http::Config cfg;
        cfg.queue = queueConfig(tempBase("outbox_offline_submit"), fs, kMaxRecordSizeBytes);
        cfg.outbox.maxDrainAttemptsPerSecond = 1000;
        cfg.outbox.initialRetryDelayMs       = 1;
        cfg.baseUrl = "https://example.test";

        pqueue::http::Outbox outbox(cfg, transport, fakeClock, &clock);
        (void)outbox.stats();  // trigger mount
        transport.online = false;
        const auto before = fs->counters();

        for (std::uint32_t i = 0; i < records; ++i) {
            const std::uint64_t simBefore = fs->counters().simLatencyUs;
            const auto t0  = Clock::now();
            const auto sub = outbox.submitPost("/readings", body);
            const auto t1  = Clock::now();
            samples.add(std::chrono::duration<double, std::micro>(t1 - t0).count(),
                        static_cast<double>(fs->counters().simLatencyUs - simBefore));
            clock.now += 2;
            if (sub.status != pqueue::SubmitStatus::Queued) { anyFail = true; break; }
        }

        const auto after = fs->counters();
        totalDataBytesWritten += (after.bytesWritten - after.lockBytesWritten)
                               - (before.bytesWritten - before.lockBytesWritten);
        totalBytesRead    += after.bytesRead    - before.bytesRead;
        totalWriteFile    += after.writeFile    - before.writeFile;
        totalWriteAt      += after.writeAt      - before.writeAt;
        totalReadAt       += after.readAt       - before.readAt;
        totalRemove       += after.removeFile   - before.removeFile;
    }

    const auto totalOps = static_cast<std::uint64_t>(repeat) * records;

    BenchmarkResult res;
    res.scenario     = "outbox_offline_submit";
    res.payloadBytes = payloadBytes;
    res.records      = records;
    res.repeat       = repeat;
    res.p50_us       = samples.wallP50();
    res.p90_us       = samples.wallP90();
    res.p99_us       = samples.wallP99();
    res.max_us       = samples.wallMax();
    res.sim_p99_ms   = samples.simP99Ms();
    res.sim_max_ms   = samples.simMaxMs();
    res.write_amp    = (totalOps > 0 && payloadBytes > 0)
                     ? static_cast<double>(totalDataBytesWritten) / static_cast<double>(totalOps * payloadBytes)
                     : 0.0;
    res.read_bpp     = totalOps > 0
                     ? static_cast<double>(totalBytesRead) / static_cast<double>(totalOps)
                     : 0.0;
    // Per-run averages so counts are independent of --repeat K.
    res.writeFile    = totalWriteFile / repeat;
    res.writeAt      = totalWriteAt   / repeat;
    res.readAt       = totalReadAt    / repeat;
    res.remove       = totalRemove    / repeat;
    res.ok           = !anyFail;
    return res;
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

void emitJson(const std::vector<BenchmarkResult>& results, const BenchmarkConfig& cfg) {
    std::printf("{\n");
    std::printf("  \"config\": {\n");
    std::printf("    \"gitHash\": \"%s\",\n", cfg.gitHash.c_str());
    std::printf("    \"maxSegmentBytes\": %u,\n", cfg.maxSegmentBytes);
    std::printf("    \"maxSegments\": %u,\n", cfg.maxSegments);
    std::printf("    \"maxTotalBytes\": %u,\n", cfg.maxTotalBytes);
    std::printf("    \"maxOutputSegments\": %u,\n", cfg.maxOutputSegments);
    std::printf("    \"platform\": \"%s\"\n", cfg.platform.c_str());
    std::printf("  },\n");
    std::printf("  \"results\": [\n");
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        const char* comma = (i + 1 < results.size()) ? "," : "";
        std::printf("    {\n");
        std::printf("      \"scenario\": \"%s\",\n", r.scenario.c_str());
        std::printf("      \"payloadBytes\": %u,\n", r.payloadBytes);
        std::printf("      \"records\": %u,\n", r.records);
        std::printf("      \"repeat\": %u,\n", r.repeat);
        std::printf("      \"p50_us\": %llu, \"p90_us\": %llu, \"p99_us\": %llu, \"max_us\": %llu,\n",
                    (unsigned long long)r.p50_us, (unsigned long long)r.p90_us,
                    (unsigned long long)r.p99_us, (unsigned long long)r.max_us);
        std::printf("      \"sim_p99_ms\": %.3f, \"sim_max_ms\": %.3f,\n", r.sim_p99_ms, r.sim_max_ms);
        std::printf("      \"write_amp\": %.3f, \"read_bpp\": %.1f,\n", r.write_amp, r.read_bpp);
        // I/O counts are per-run averages (independent of --repeat).
        std::printf("      \"writeFile\": %llu, \"writeAt\": %llu, \"readAt\": %llu, \"remove\": %llu,\n",
                    (unsigned long long)r.writeFile, (unsigned long long)r.writeAt,
                    (unsigned long long)r.readAt, (unsigned long long)r.remove);
        std::printf("      \"ok\": %s\n", r.ok ? "true" : "false");
        std::printf("    }%s\n", comma);
    }
    std::printf("  ]\n");
    std::printf("}\n");
}

void emitMarkdown(const std::vector<BenchmarkResult>& results, const BenchmarkConfig& cfg) {
    std::printf("# Benchmark Results\n\n");
    std::printf("git: `%s`  "
                "maxSegmentBytes: %u  maxSegments: %u  maxTotalBytes: %u  "
                "maxOutputSegments: %u  platform: %s\n\n",
                cfg.gitHash.c_str(), cfg.maxSegmentBytes, cfg.maxSegments,
                cfg.maxTotalBytes, cfg.maxOutputSegments, cfg.platform.c_str());
    std::printf("I/O counts are per-run averages (independent of --repeat).\n\n");

    std::printf("| scenario | payload | N | repeat"
                " | p50_us | p90_us | p99_us | max_us"
                " | sim_p99_ms | sim_max_ms"
                " | write_amp | read_bpp"
                " | writeFile | writeAt | readAt | remove | ok |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n");
    for (const auto& r : results) {
        std::printf("| %s | %uB | %u | %u"
                    " | %llu | %llu | %llu | %llu"
                    " | %.3f | %.3f"
                    " | %.3f | %.1f"
                    " | %llu | %llu | %llu | %llu | %s |\n",
                    r.scenario.c_str(), r.payloadBytes, r.records, r.repeat,
                    (unsigned long long)r.p50_us, (unsigned long long)r.p90_us,
                    (unsigned long long)r.p99_us, (unsigned long long)r.max_us,
                    r.sim_p99_ms, r.sim_max_ms,
                    r.write_amp, r.read_bpp,
                    (unsigned long long)r.writeFile, (unsigned long long)r.writeAt,
                    (unsigned long long)r.readAt, (unsigned long long)r.remove,
                    r.ok ? "yes" : "NO");
    }
    std::printf("\n*sim_\\* columns: multiply by 100 for predicted on-device ms "
                "(ESP32S3, QSPI flash, default calibration).*\n");
}

} // namespace

int main(int argc, char** argv) {
    bool doJson     = false;
    bool doMarkdown = false;
    bool strict     = false;
    std::uint32_t repeat = 1;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if      (arg == "--json")     { doJson = true; }
        else if (arg == "--markdown") { doMarkdown = true; }
        else if (arg == "--strict")   { strict = true; }
        else if (arg == "--repeat" && i + 1 < argc) {
            repeat = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            if (repeat == 0) repeat = 1;
        }
    }
    if (!doJson && !doMarkdown) doMarkdown = true;

    BenchmarkConfig cfg;
    cfg.gitHash = getGitHash();

    std::vector<BenchmarkResult> results;
    for (std::uint32_t payload : {64u, 256u, 1024u, 2048u})
        results.push_back(scenarioEnqueue(payload, kBenchmarkN, repeat));
    for (std::uint32_t payload : {64u, 256u, 1024u, 2048u})
        results.push_back(scenarioPeekPop(payload, kBenchmarkN, repeat));
    for (std::uint32_t payload : {256u, 1024u})
        results.push_back(scenarioOutboxOfflineSubmit(payload, kBenchmarkN, repeat));

    if (doJson)     emitJson(results, cfg);
    if (doMarkdown) emitMarkdown(results, cfg);

    if (strict) {
        bool fail = false;
        for (const auto& r : results) {
            for (const auto& msg : strictViolations(r)) {
                std::fprintf(stderr, "STRICT FAIL [%s payload=%uB]: %s\n",
                             r.scenario.c_str(), r.payloadBytes, msg.c_str());
                fail = true;
            }
        }
        if (fail) return 1;
    }
    return 0;
}

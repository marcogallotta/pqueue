#ifdef ARDUINO

#include <Arduino.h>
#include <LittleFS.h>
#include <memory>
#include <string>

#include "counting_file_system.h"
#include "pqueue/http/outbox.h"
#include "pqueue/outbox.h"
#include "pqueue/queue.h"
#include "pqueue/append_log_common.h"

namespace {

static constexpr uint32_t kRecordsPerScenario = 20;
static constexpr uint32_t kRealisticBodyBytes = 152;
static constexpr const char* kBasePath = "/pqueue_prof";

static const uint32_t kRecordSizes[] = {492};

struct DeviceTimings {
    uint64_t sumUs = 0;
    uint64_t minUs = UINT64_MAX;
    uint64_t maxUs = 0;
    uint32_t count = 0;

    void add(uint64_t us) {
        sumUs += us;
        if (us < minUs) minUs = us;
        if (us > maxUs) maxUs = us;
        ++count;
    }

    uint64_t avg() const { return count > 0 ? sumUs / count : 0; }
};

struct ScenarioResult {
    const char* name = "";
    uint32_t recordSizeBytes = 0;
    DeviceTimings timings;
    FsCounters fs;
    bool failed = false;
    const char* failReason = "";
};

std::shared_ptr<CountingFileSystem> makeCfs() {
    return std::make_shared<CountingFileSystem>(pqueue::makeLittleFsFileSystem());
}

pqueue::Config makeQueueConfig(std::shared_ptr<CountingFileSystem> fs, uint32_t recordSizeBytes) {
    pqueue::Config cfg;
    cfg.basePath = kBasePath;
    cfg.recordSizeBytes = recordSizeBytes;
    cfg.reservedBytes = (kRecordsPerScenario + 8) *
        static_cast<uint32_t>(pqueue::append_log_detail::kEnqueueOverheadBytes + recordSizeBytes);
    cfg.storageBackend = pqueue::StorageBackend::LittleFS;
    cfg.fileSystem = fs;
    return cfg;
}

std::string makePayload(uint32_t size) {
    std::string out(size, 'x');
    for (uint32_t i = 0; i < size; ++i) {
        out[i] = static_cast<char>('a' + (i % 26));
    }
    return out;
}

bool reformatLittleFS() {
    if (!LittleFS.format()) {
        Serial.println("ERROR: LittleFS.format() failed");
        return false;
    }
    if (!LittleFS.begin()) {
        Serial.println("ERROR: LittleFS.begin() after format failed");
        return false;
    }
    return true;
}

ScenarioResult runQueueEnqueue(uint32_t recordSizeBytes) {
    ScenarioResult result;
    result.name = "queue_enqueue";
    result.recordSizeBytes = recordSizeBytes;

    if (!reformatLittleFS()) { result.failed = true; result.failReason = "format"; return result; }

    auto fs = makeCfs();
    pqueue::Queue q(makeQueueConfig(fs, recordSizeBytes));
    (void)q.stats();
    const auto data = makePayload(recordSizeBytes);
    fs->resetCounters();

    for (uint32_t i = 0; i < kRecordsPerScenario; ++i) {
        const int64_t t0 = esp_timer_get_time();
        const auto st = q.enqueue(data);
        const uint64_t us = static_cast<uint64_t>(esp_timer_get_time() - t0);
        result.timings.add(us);
        if (!st.ok()) { result.failed = true; result.failReason = st.message; break; }
    }
    result.fs = fs->counters();
    return result;
}

ScenarioResult runQueuePeekPop(uint32_t recordSizeBytes) {
    ScenarioResult result;
    result.name = "queue_peek_pop";
    result.recordSizeBytes = recordSizeBytes;

    if (!reformatLittleFS()) { result.failed = true; result.failReason = "format"; return result; }

    auto fs = makeCfs();
    pqueue::Queue q(makeQueueConfig(fs, recordSizeBytes));
    const auto data = makePayload(recordSizeBytes);
    for (uint32_t i = 0; i < kRecordsPerScenario; ++i) {
        if (!q.enqueue(data).ok()) { result.failed = true; result.failReason = "setup_enqueue"; return result; }
    }
    fs->resetCounters();

    std::string out;
    for (uint32_t i = 0; i < kRecordsPerScenario; ++i) {
        const int64_t t0 = esp_timer_get_time();
        auto st = q.peek(out);
        if (st.ok()) st = q.pop();
        const uint64_t us = static_cast<uint64_t>(esp_timer_get_time() - t0);
        result.timings.add(us);
        if (!st.ok()) { result.failed = true; result.failReason = st.message; break; }
    }
    result.fs = fs->counters();
    return result;
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

struct FakeClock {
    std::uint64_t now = 0;
};

std::uint64_t fakeClockCb(void* ctx) {
    return static_cast<FakeClock*>(ctx)->now;
}

pqueue::http::Config makeHttpConfig(std::shared_ptr<CountingFileSystem> fs, uint32_t recordSizeBytes) {
    pqueue::http::Config cfg;
    cfg.queue = makeQueueConfig(fs, recordSizeBytes);
    cfg.outbox.maxDrainAttemptsPerSecond = 1000;
    cfg.outbox.retryDelayMs = 1;
    cfg.baseUrl = "https://example.test";
    return cfg;
}

ScenarioResult runHttpOfflineSubmit(uint32_t recordSizeBytes) {
    ScenarioResult result;
    result.name = "http_offline_submit";
    result.recordSizeBytes = recordSizeBytes;

    if (!reformatLittleFS()) { result.failed = true; result.failReason = "format"; return result; }

    auto fs = makeCfs();
    FakeTransport transport;
    FakeClock clock;
    pqueue::http::Outbox outbox(makeHttpConfig(fs, recordSizeBytes), transport, fakeClockCb, &clock);
    (void)outbox.stats();
    const auto body = makePayload(kRealisticBodyBytes);
    transport.online = false;
    fs->resetCounters();

    for (uint32_t i = 0; i < kRecordsPerScenario; ++i) {
        const int64_t t0 = esp_timer_get_time();
        const auto submit = outbox.submitPost("/readings", body);
        const uint64_t us = static_cast<uint64_t>(esp_timer_get_time() - t0);
        result.timings.add(us);
        if (submit.status != pqueue::SubmitStatus::Queued) {
            result.failed = true; result.failReason = submit.detail.message; break;
        }
        clock.now += 2;
    }
    result.fs = fs->counters();
    return result;
}

ScenarioResult runHttpDrainBacklog(uint32_t recordSizeBytes) {
    ScenarioResult result;
    result.name = "http_drain_backlog";
    result.recordSizeBytes = recordSizeBytes;

    if (!reformatLittleFS()) { result.failed = true; result.failReason = "format"; return result; }

    auto fs = makeCfs();
    FakeTransport transport;
    FakeClock clock;
    pqueue::http::Outbox outbox(makeHttpConfig(fs, recordSizeBytes), transport, fakeClockCb, &clock);
    const auto body = makePayload(kRealisticBodyBytes);
    transport.online = false;
    for (uint32_t i = 0; i < kRecordsPerScenario; ++i) {
        const auto submit = outbox.submitPost("/readings", body);
        if (submit.status != pqueue::SubmitStatus::Queued) {
            result.failed = true; result.failReason = "setup_submit"; return result;
        }
        clock.now += 2;
    }

    transport.online = true;
    clock.now += 100000;
    fs->resetCounters();

    for (uint32_t i = 0; i < kRecordsPerScenario; ++i) {
        const int64_t t0 = esp_timer_get_time();
        const auto drain = outbox.drainUpTo(1);
        const uint64_t us = static_cast<uint64_t>(esp_timer_get_time() - t0);
        result.timings.add(us);
        if (drain.queueError || drain.sendError || drain.sent != 1) {
            result.failed = true; result.failReason = drain.detail.message; break;
        }
        clock.now += 2;
    }
    result.fs = fs->counters();
    return result;
}

void runFlushCurve() {
    struct SizePoint { uint32_t bytes; const char* label; };
    static const SizePoint kSizes[] = {
        {512,   "512B"},
        {1024,  "1KB"},
        {2048,  "2KB"},
        {4096,  "4KB"},
        {8192,  "8KB"},
        {12288, "12KB"},
        {16384, "16KB"},
        {32768, "32KB"},
    };
    const uint32_t kIter = 20;
    const uint32_t kWriteSize = 128;
    const char* kFile = "/pqueue_prof_fc";
    const std::string writeData(kWriteSize, 'x');
    const std::string zeros(256, '\0');

    Serial.printf("flush cost curve (%u iters, %uB append-like write):\n", kIter, kWriteSize);
    Serial.printf("  %-6s  %9s  %9s  %9s  %9s  %9s\n",
        "size", "open_us", "flush_us", "close_us", "total_us", "flush_pers");

    for (const auto& sp : kSizes) {
        {
            File f = LittleFS.open(kFile, "w");
            if (!f) { Serial.printf("  %-6s  CREATE FAILED\n", sp.label); continue; }
            uint32_t written = 0;
            while (written < sp.bytes) {
                const uint32_t chunk = std::min(256u, sp.bytes - written);
                f.write(reinterpret_cast<const uint8_t*>(zeros.data()), chunk);
                written += chunk;
            }
            f.flush();
            f.close();
        }

        const uint32_t writeOffset = sp.bytes >= kWriteSize ? sp.bytes - kWriteSize : 0;
        uint64_t openUs = 0, flushUs = 0, closeUs = 0;

        for (uint32_t i = 0; i < kIter; ++i) {
            int64_t t0, t1;

            t0 = esp_timer_get_time();
            File f = LittleFS.open(kFile, "r+");
            t1 = esp_timer_get_time();
            openUs += static_cast<uint64_t>(t1 - t0);

            if (!f) { Serial.printf("  %-6s  OPEN FAILED\n", sp.label); break; }
            f.seek(writeOffset, SeekSet);
            f.write(reinterpret_cast<const uint8_t*>(writeData.data()), kWriteSize);

            t0 = esp_timer_get_time();
            f.flush();
            t1 = esp_timer_get_time();
            flushUs += static_cast<uint64_t>(t1 - t0);

            t0 = esp_timer_get_time();
            f.close();
            t1 = esp_timer_get_time();
            closeUs += static_cast<uint64_t>(t1 - t0);
        }

        // Persistent handle: open once, measure only flush
        uint64_t flushPersUs = 0;
        {
            File f = LittleFS.open(kFile, "r+");
            if (f) {
                for (uint32_t i = 0; i < kIter; ++i) {
                    f.seek(writeOffset, SeekSet);
                    f.write(reinterpret_cast<const uint8_t*>(writeData.data()), kWriteSize);
                    const int64_t t0 = esp_timer_get_time();
                    f.flush();
                    flushPersUs += static_cast<uint64_t>(esp_timer_get_time() - t0);
                }
                f.close();
            }
        }

        LittleFS.remove(kFile);

        Serial.printf("  %-6s  %9llu  %9llu  %9llu  %9llu  %9llu\n",
            sp.label,
            openUs / kIter,
            flushUs / kIter,
            closeUs / kIter,
            (openUs + flushUs + closeUs) / kIter,
            flushPersUs / kIter);
    }
}

void runMountBreakdown() {
    const char* kDir = "/pqueue_prof_mtest";
    const uint32_t kIter = 20;

    LittleFS.mkdir(kDir);

    uint64_t beginUs = 0, mkdirUs = 0, openDirUs = 0;

    for (uint32_t i = 0; i < kIter; ++i) {
        int64_t t0, t1;

        t0 = esp_timer_get_time();
        LittleFS.begin(false);
        t1 = esp_timer_get_time();
        beginUs += static_cast<uint64_t>(t1 - t0);

        t0 = esp_timer_get_time();
        LittleFS.mkdir(kDir);
        t1 = esp_timer_get_time();
        mkdirUs += static_cast<uint64_t>(t1 - t0);

        t0 = esp_timer_get_time();
        File d = LittleFS.open(kDir, "r");
        t1 = esp_timer_get_time();
        openDirUs += static_cast<uint64_t>(t1 - t0);
        if (d) d.close();
    }

    LittleFS.rmdir(kDir);

    Serial.printf("mount (rawMount) breakdown (%u iters):\n", kIter);
    Serial.printf("  LittleFS.begin(false) avg=%llu us\n", beginUs / kIter);
    Serial.printf("  mkdir (existing)      avg=%llu us\n", mkdirUs / kIter);
    Serial.printf("  open dir              avg=%llu us\n", openDirUs / kIter);
    Serial.printf("  total (per rawMount)  avg=%llu us\n", (beginUs + mkdirUs + openDirUs) / kIter);
}

void runWriteAtLargeFile() {
    const char* kFile = "/pqueue_prof_ltest";
    const uint32_t kIter = 20;
    const uint32_t kFileSize = 18688;
    const uint32_t kWriteSize = 512;

    {
        File f = LittleFS.open(kFile, "w");
        if (!f) { Serial.println("writeAt large: create failed"); return; }
        const std::string zeros(256, '\0');
        uint32_t written = 0;
        while (written < kFileSize) {
            const uint32_t chunk = std::min(256u, kFileSize - written);
            f.write(reinterpret_cast<const uint8_t*>(zeros.data()), chunk);
            written += chunk;
        }
        f.flush();
        f.close();
    }

    uint64_t openUs = 0, writeUs = 0, flushUs = 0, closeUs = 0;
    const std::string data(kWriteSize, 'x');
    const uint32_t kMidOffset = kFileSize / 2;

    for (uint32_t i = 0; i < kIter; ++i) {
        int64_t t0, t1;

        t0 = esp_timer_get_time();
        File f = LittleFS.open(kFile, "r+");
        t1 = esp_timer_get_time();
        openUs += static_cast<uint64_t>(t1 - t0);

        if (!f) { Serial.println("writeAt large: open failed"); LittleFS.remove(kFile); return; }
        f.seek(kMidOffset, SeekSet);

        t0 = esp_timer_get_time();
        f.write(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        t1 = esp_timer_get_time();
        writeUs += static_cast<uint64_t>(t1 - t0);

        t0 = esp_timer_get_time();
        f.flush();
        t1 = esp_timer_get_time();
        flushUs += static_cast<uint64_t>(t1 - t0);

        t0 = esp_timer_get_time();
        f.close();
        t1 = esp_timer_get_time();
        closeUs += static_cast<uint64_t>(t1 - t0);
    }

    LittleFS.remove(kFile);

    Serial.printf("writeAt large-file breakdown (%u iters, %u B file, %u B write at mid):\n", kIter, kFileSize, kWriteSize);
    Serial.printf("  open  avg=%llu us\n", openUs / kIter);
    Serial.printf("  write avg=%llu us\n", writeUs / kIter);
    Serial.printf("  flush avg=%llu us\n", flushUs / kIter);
    Serial.printf("  close avg=%llu us\n", closeUs / kIter);
    Serial.printf("  total avg=%llu us\n", (openUs + writeUs + flushUs + closeUs) / kIter);
}

void runWriteAtPhaseBreakdown() {
    const char* kFile = "/pqueue_prof_wtest";
    const uint32_t kIter = 20;
    const uint32_t kWriteSize = 512;

    {
        File f = LittleFS.open(kFile, "w");
        if (!f) { Serial.println("writeAt breakdown: create failed"); return; }
        const std::string zeros(kWriteSize, '\0');
        f.write(reinterpret_cast<const uint8_t*>(zeros.data()), zeros.size());
        f.flush();
        f.close();
    }

    uint64_t openUs = 0, writeUs = 0, flushUs = 0, closeUs = 0;
    const std::string data(kWriteSize, 'x');

    for (uint32_t i = 0; i < kIter; ++i) {
        int64_t t0, t1;

        t0 = esp_timer_get_time();
        File f = LittleFS.open(kFile, "r+");
        t1 = esp_timer_get_time();
        openUs += static_cast<uint64_t>(t1 - t0);

        if (!f) { Serial.println("writeAt breakdown: open failed"); LittleFS.remove(kFile); return; }
        f.seek(0, SeekSet);

        t0 = esp_timer_get_time();
        f.write(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        t1 = esp_timer_get_time();
        writeUs += static_cast<uint64_t>(t1 - t0);

        t0 = esp_timer_get_time();
        f.flush();
        t1 = esp_timer_get_time();
        flushUs += static_cast<uint64_t>(t1 - t0);

        t0 = esp_timer_get_time();
        f.close();
        t1 = esp_timer_get_time();
        closeUs += static_cast<uint64_t>(t1 - t0);
    }

    LittleFS.remove(kFile);

    Serial.printf("writeAt phase breakdown (%u iters, %u B):\n", kIter, kWriteSize);
    Serial.printf("  open  avg=%llu us\n", openUs / kIter);
    Serial.printf("  write avg=%llu us\n", writeUs / kIter);
    Serial.printf("  flush avg=%llu us\n", flushUs / kIter);
    Serial.printf("  close avg=%llu us\n", closeUs / kIter);
    Serial.printf("  total avg=%llu us\n", (openUs + writeUs + flushUs + closeUs) / kIter);
}

void runReadAtPhaseBreakdown() {
    const char* kFile = "/pqueue_prof_rtest";
    const uint32_t kIter = 20;
    const uint32_t kReadSize = 512;

    {
        File f = LittleFS.open(kFile, "w");
        if (!f) { Serial.println("readAt breakdown: create failed"); return; }
        const std::string data(kReadSize, 'x');
        f.write(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        f.flush();
        f.close();
    }

    uint64_t openUs = 0, readUs = 0, closeUs = 0;
    std::string buf(kReadSize, '\0');

    for (uint32_t i = 0; i < kIter; ++i) {
        int64_t t0, t1;

        t0 = esp_timer_get_time();
        File f = LittleFS.open(kFile, "r");
        t1 = esp_timer_get_time();
        openUs += static_cast<uint64_t>(t1 - t0);

        if (!f) { Serial.println("readAt breakdown: open failed"); LittleFS.remove(kFile); return; }
        f.seek(0, SeekSet);

        t0 = esp_timer_get_time();
        f.read(reinterpret_cast<uint8_t*>(buf.data()), kReadSize);
        t1 = esp_timer_get_time();
        readUs += static_cast<uint64_t>(t1 - t0);

        t0 = esp_timer_get_time();
        f.close();
        t1 = esp_timer_get_time();
        closeUs += static_cast<uint64_t>(t1 - t0);
    }

    LittleFS.remove(kFile);

    Serial.printf("readAt phase breakdown (%u iters, %u B):\n", kIter, kReadSize);
    Serial.printf("  open  avg=%llu us\n", openUs / kIter);
    Serial.printf("  read  avg=%llu us\n", readUs / kIter);
    Serial.printf("  close avg=%llu us\n", closeUs / kIter);
    Serial.printf("  total avg=%llu us\n", (openUs + readUs + closeUs) / kIter);
}

void runRotationCostCurve() {
    // Measures the marginal cost of one rotation under the dual-root design:
    //   segment create  = open("w") + write 20B header + flush + close
    //   root publish    = open("w") + write N bytes + flush + close, alternating A/B slots
    // Amortized per-enqueue cost = rotation_total / records_per_segment.

    const uint32_t kIter = 20;
    static const uint32_t kSegHeaderBytes = 20;
    static const uint32_t kRecordBytes = 492;
    static const uint32_t kEventBytes = kRecordBytes + 24; // payload + kEnqueueOverheadBytes

    struct RootSize { uint32_t bytes; const char* label; };
    static const RootSize kRootSizes[] = {
        { 64,  "64B"  },
        { 128, "128B" },
        { 256, "256B" },
    };
    static const uint32_t kSegSizes[] = {4096, 8192, 16384, 32768};
    static const uint32_t kNumRootSizes = 3;

    const char* kSegFile = "/pqueue_prof_rot_s";
    const char* kRootA   = "/pqueue_prof_rot_a";
    const char* kRootB   = "/pqueue_prof_rot_b";

    const std::string segHeader(kSegHeaderBytes, '\0');

    uint64_t segCreateUs = 0;
    for (uint32_t i = 0; i < kIter; ++i) {
        const int64_t t0 = esp_timer_get_time();
        File f = LittleFS.open(kSegFile, "w");
        if (f) {
            f.write(reinterpret_cast<const uint8_t*>(segHeader.data()), kSegHeaderBytes);
            f.flush();
            f.close();
        }
        segCreateUs += static_cast<uint64_t>(esp_timer_get_time() - t0);
        LittleFS.remove(kSegFile);
    }
    const uint64_t segCreateAvg = segCreateUs / kIter;

    uint64_t rootAvgs[kNumRootSizes] = {};
    for (uint32_t ri = 0; ri < kNumRootSizes; ++ri) {
        const std::string rootData(kRootSizes[ri].bytes, '\0');
        uint64_t rootUs = 0;
        for (uint32_t i = 0; i < kIter; ++i) {
            const char* kRoot = (i % 2 == 0) ? kRootA : kRootB;
            const int64_t t0 = esp_timer_get_time();
            File f = LittleFS.open(kRoot, "w");
            if (f) {
                f.write(reinterpret_cast<const uint8_t*>(rootData.data()), kRootSizes[ri].bytes);
                f.flush();
                f.close();
            }
            rootUs += static_cast<uint64_t>(esp_timer_get_time() - t0);
        }
        LittleFS.remove(kRootA);
        LittleFS.remove(kRootB);
        rootAvgs[ri] = rootUs / kIter;
    }

    Serial.printf("rotation cost (%u iters, %uB record, 24B overhead):\n", kIter, kRecordBytes);
    Serial.printf("  segment create avg=%llu us\n\n", segCreateAvg);
    Serial.printf("  %-6s  %12s  %12s\n", "root", "root_pub_us", "rotation_us");
    for (uint32_t ri = 0; ri < kNumRootSizes; ++ri) {
        Serial.printf("  %-6s  %12llu  %12llu\n",
            kRootSizes[ri].label, rootAvgs[ri], segCreateAvg + rootAvgs[ri]);
    }

    // Amortization table using 128B root as representative candidate
    const uint64_t rotAvg = segCreateAvg + rootAvgs[1];
    Serial.printf("\namortized rotation overhead per enqueue (128B root, rotation=%llu us):\n", rotAvg);
    Serial.printf("  %-8s  %12s  %14s\n", "seg_size", "recs/seg", "amort_us/enq");
    for (const uint32_t segSize : kSegSizes) {
        if (segSize <= kSegHeaderBytes) continue;
        const uint32_t recsPerSeg = (segSize - kSegHeaderBytes) / kEventBytes;
        if (recsPerSeg == 0) continue;
        Serial.printf("  %-8u  %12u  %14llu\n", segSize, recsPerSeg, rotAvg / recsPerSeg);
    }
}

void printResult(const ScenarioResult& r) {
    Serial.printf("%-22s %4uB  avg=%6llu us  min=%6llu us  max=%6llu us\n",
        r.name, r.recordSizeBytes, r.timings.avg(),
        r.timings.count > 0 ? r.timings.minUs : 0ULL, r.timings.maxUs);
    Serial.printf("  readAt=%-4llu writeAt=%-4llu writeFile=%-4llu rename=%-4llu remove=%-4llu lock=%-4llu mount=%-4llu\n",
        r.fs.readAt, r.fs.writeAt, r.fs.writeFile, r.fs.renameFile, r.fs.removeFile, r.fs.lockAcquire, r.fs.mount);
    Serial.printf("  bytesW=%-10llu bytesR=%-10llu\n",
        r.fs.bytesWritten, r.fs.bytesRead);
    if (r.failed) {
        Serial.printf("  FAILED: %s\n", r.failReason);
    }
}

} // namespace

void setup() {
    Serial.begin(115200);
    delay(2000);

    // Initialize partitionLabel_ so format() calls work later.
    // formatOnFail=true handles a corrupted FS from a previous interrupted run.
    LittleFS.begin(true);

    Serial.println("=== pqueue on-device LittleFS profiler ===");
    Serial.printf("records per scenario: %u  body: %u B\n\n", kRecordsPerScenario, kRealisticBodyBytes);

    runFlushCurve();
    Serial.println();
    runMountBreakdown();
    Serial.println();
    runWriteAtPhaseBreakdown();
    Serial.println();
    runWriteAtLargeFile();
    Serial.println();
    runReadAtPhaseBreakdown();
    Serial.println();
    runRotationCostCurve();
    Serial.println();

    for (const uint32_t recordSize : kRecordSizes) {
        Serial.printf("--- record_size=%u B ---\n", recordSize);

        printResult(runQueueEnqueue(recordSize));
        printResult(runQueuePeekPop(recordSize));
        printResult(runHttpOfflineSubmit(recordSize));
        printResult(runHttpDrainBacklog(recordSize));

        Serial.println();
    }

    Serial.println("=== done ===");
}

void loop() {
    delay(60000);
}

#endif // ARDUINO

#ifdef ARDUINO

#include <Arduino.h>
#include <LittleFS.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

static constexpr uint32_t kIter     = 40;
static constexpr uint32_t kReadSize = 512;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint64_t medianOf(std::vector<uint64_t>& v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

struct LinearFit { int64_t intercept; int64_t slope; };

static LinearFit fitLine(const double* xs, const double* ys, uint32_t n) {
    double sumx = 0, sumy = 0, sumxx = 0, sumxy = 0;
    for (uint32_t i = 0; i < n; ++i) {
        sumx  += xs[i]; sumy  += ys[i];
        sumxx += xs[i] * xs[i]; sumxy += xs[i] * ys[i];
    }
    const double denom = n * sumxx - sumx * sumx;
    const double b = denom != 0.0 ? (n * sumxy - sumx * sumy) / denom : 0.0;
    const double a = (sumy - b * sumx) / n;
    return { static_cast<int64_t>(a), static_cast<int64_t>(b) };
}

// ---------------------------------------------------------------------------
// readAt: open + read + close on a small file
// ---------------------------------------------------------------------------

static uint64_t measureReadAt() {
    const char* kPath = "/pq_cal_r";
    {
        File f = LittleFS.open(kPath, "w");
        if (!f) { Serial.println("readAt: create failed"); return 0; }
        uint8_t buf[kReadSize] = {};
        f.write(buf, kReadSize);
        f.flush(); f.close();
    }
    std::vector<uint64_t> s; s.reserve(kIter);
    uint8_t buf[kReadSize];
    for (uint32_t i = 0; i < kIter; ++i) {
        const int64_t t0 = esp_timer_get_time();
        File f = LittleFS.open(kPath, "r");
        if (!f) { Serial.println("readAt: open failed in loop"); continue; }
        const auto n = f.read(buf, kReadSize);
        f.close();
        if (n != kReadSize) { Serial.println("readAt: short read"); continue; }
        s.push_back(static_cast<uint64_t>(esp_timer_get_time() - t0));
    }
    LittleFS.remove(kPath);
    if (s.empty()) { Serial.println("readAt: no valid samples"); return 0; }
    return medianOf(s);
}

// ---------------------------------------------------------------------------
// writeAt: open("r+") + seek + write + flush + close on a small file
// ---------------------------------------------------------------------------

static uint64_t measureWriteAt() {
    const char* kPath = "/pq_cal_wa";
    {
        File f = LittleFS.open(kPath, "w");
        if (!f) { Serial.println("writeAt: create failed"); return 0; }
        uint8_t buf[kReadSize] = {};
        f.write(buf, kReadSize);
        f.flush(); f.close();
    }
    std::vector<uint64_t> s; s.reserve(kIter);
    const uint8_t kData[128] = {};
    for (uint32_t i = 0; i < kIter; ++i) {
        const int64_t t0 = esp_timer_get_time();
        File f = LittleFS.open(kPath, "r+");
        if (!f) { Serial.println("writeAt: open failed in loop"); continue; }
        if (!f.seek(0, SeekSet)) { f.close(); continue; }
        const auto n = f.write(kData, sizeof(kData));
        f.flush(); f.close();
        if (n != sizeof(kData)) { Serial.println("writeAt: short write"); continue; }
        s.push_back(static_cast<uint64_t>(esp_timer_get_time() - t0));
    }
    LittleFS.remove(kPath);
    if (s.empty()) { Serial.println("writeAt: no valid samples"); return 0; }
    return medianOf(s);
}

// ---------------------------------------------------------------------------
// writeFile: open("w") + write N bytes + flush + close
// Measured across multiple sizes. Linear fit applied to sizes <= 4096 B
// (the pqueue operating range). Sizes above 4096 printed for reference only.
// ---------------------------------------------------------------------------

struct WriteFileFit { uint64_t fixed_us; uint64_t per_kb_us; };

static WriteFileFit measureWriteFile() {
    const char* kPath = "/pq_cal_wf";

    static const uint32_t kSizes[]  = { 64, 128, 256, 512, 1024, 2048, 4096,
                                         8192, 16384, 32768 };
    static const uint32_t kNSizes   = sizeof(kSizes) / sizeof(kSizes[0]);
    static const uint32_t kFitLimit = 4096;

    Serial.println("  writeFile curve:");
    Serial.println("    size_B   median_us");

    double xs[kNSizes], ys[kNSizes];
    uint32_t nFit = 0;

    for (uint32_t si = 0; si < kNSizes; ++si) {
        const uint32_t sz = kSizes[si];
        std::vector<uint8_t> data(sz, 0xAB);
        std::vector<uint64_t> s; s.reserve(kIter);

        for (uint32_t i = 0; i < kIter; ++i) {
            const int64_t t0 = esp_timer_get_time();
            File f = LittleFS.open(kPath, "w");
            if (!f) { Serial.printf("writeFile: open failed at %u B\n", sz); continue; }
            const auto n = f.write(data.data(), sz);
            f.flush(); f.close();
            LittleFS.remove(kPath);
            if (n != sz) { Serial.printf("writeFile: short write at %u B\n", sz); continue; }
            s.push_back(static_cast<uint64_t>(esp_timer_get_time() - t0));
        }

        if (s.empty()) {
            Serial.printf("    %6u   FAILED\n", sz);
            continue;
        }
        const uint64_t med = medianOf(s);
        Serial.printf("    %6u   %6llu\n", sz, med);

        if (sz <= kFitLimit) {
            xs[nFit] = static_cast<double>(sz) / 1024.0;
            ys[nFit] = static_cast<double>(med);
            ++nFit;
        }
    }

    if (nFit < 2) {
        Serial.println("writeFile: not enough points to fit");
        return { 0, 0 };
    }
    const LinearFit fit = fitLine(xs, ys, nFit);
    const uint64_t fixed  = fit.intercept < 0 ? 0 : static_cast<uint64_t>(fit.intercept);
    const uint64_t per_kb = fit.slope     < 0 ? 0 : static_cast<uint64_t>(fit.slope);

    Serial.printf("  fit (<=4096B): fixed=%llu us  per_kb=%llu us\n", fixed, per_kb);
    return { fixed, per_kb };
}

// ---------------------------------------------------------------------------
// removeFile
// ---------------------------------------------------------------------------

static uint64_t measureRemoveFile() {
    const char* kPath = "/pq_cal_rm";
    const uint8_t kData[64] = {};
    std::vector<uint64_t> s; s.reserve(kIter);
    for (uint32_t i = 0; i < kIter; ++i) {
        {
            File f = LittleFS.open(kPath, "w");
            if (!f) { Serial.println("removeFile: create failed in loop"); continue; }
            f.write(kData, sizeof(kData));
            f.flush(); f.close();
        }
        const int64_t t0 = esp_timer_get_time();
        LittleFS.remove(kPath);
        s.push_back(static_cast<uint64_t>(esp_timer_get_time() - t0));
    }
    if (s.empty()) { Serial.println("removeFile: no valid samples"); return 0; }
    return medianOf(s);
}

// ---------------------------------------------------------------------------
// listFiles: open dir + iterate entries + close
// Measured at several file counts; linear fit for base + per_file cost.
// ---------------------------------------------------------------------------

struct ListFilesFit { uint64_t base_us; uint64_t per_file_us; };

static ListFilesFit measureListFiles() {
    const char* kDir = "/pq_cal_ls";
    LittleFS.mkdir(kDir);

    static const uint32_t kCounts[] = { 0, 1, 5, 10, 20 };
    static const uint32_t kNCounts  = sizeof(kCounts) / sizeof(kCounts[0]);

    Serial.println("  listFiles curve:");
    Serial.println("    files   median_us");

    double xs[kNCounts], ys[kNCounts];
    uint32_t nFit = 0;
    uint32_t prevCount = 0;

    for (uint32_t ci = 0; ci < kNCounts; ++ci) {
        const uint32_t count = kCounts[ci];
        const uint8_t kData[1] = { 'x' };
        char path[48];

        for (uint32_t i = prevCount; i < count; ++i) {
            snprintf(path, sizeof(path), "%s/f%02u", kDir, i);
            File f = LittleFS.open(path, "w");
            if (!f) { Serial.printf("listFiles: setup create failed at file %u\n", i); break; }
            f.write(kData, 1); f.flush(); f.close();
            prevCount = i + 1;
        }

        std::vector<uint64_t> s; s.reserve(kIter);
        for (uint32_t i = 0; i < kIter; ++i) {
            const int64_t t0 = esp_timer_get_time();
            File d = LittleFS.open(kDir);
            if (!d) { Serial.println("listFiles: dir open failed in loop"); continue; }
            File entry = d.openNextFile();
            while (entry) { entry.close(); entry = d.openNextFile(); }
            d.close();
            s.push_back(static_cast<uint64_t>(esp_timer_get_time() - t0));
        }

        if (s.empty()) {
            Serial.printf("    %5u   FAILED\n", count);
            continue;
        }
        const uint64_t med = medianOf(s);
        Serial.printf("    %5u   %6llu\n", prevCount, med);
        xs[nFit] = static_cast<double>(prevCount);
        ys[nFit] = static_cast<double>(med);
        ++nFit;
    }

    for (uint32_t i = 0; i < prevCount; ++i) {
        char path[48];
        snprintf(path, sizeof(path), "%s/f%02u", kDir, i);
        LittleFS.remove(path);
    }
    LittleFS.rmdir(kDir);

    if (nFit < 2) {
        Serial.println("listFiles: not enough points to fit");
        return { 0, 0 };
    }
    const LinearFit fit    = fitLine(xs, ys, nFit);
    const uint64_t base    = fit.intercept < 0 ? 0 : static_cast<uint64_t>(fit.intercept);
    const uint64_t perFile = fit.slope     < 0 ? 0 : static_cast<uint64_t>(fit.slope);

    Serial.printf("  fit: base=%llu us  per_file=%llu us\n", base, perFile);
    return { base, perFile };
}

} // namespace

void setup() {
    Serial.begin(115200);
    delay(2000);

    if (!LittleFS.begin(true))  { Serial.println("LittleFS.begin failed");              return; }
    if (!LittleFS.format())     { Serial.println("LittleFS.format failed");             return; }
    if (!LittleFS.begin(true))  { Serial.println("LittleFS.begin after format failed"); return; }

    Serial.println("=== pqueue LittleFS calibration ===");
    Serial.println();

    Serial.println("readAt...");
    const uint64_t readAt = measureReadAt();

    Serial.println("writeAt...");
    const uint64_t writeAt = measureWriteAt();

    Serial.println("writeFile...");
    const auto wf = measureWriteFile();

    Serial.println("removeFile...");
    const uint64_t removeFile = measureRemoveFile();

    Serial.println("listFiles...");
    const auto lf = measureListFiles();

    Serial.println();
    Serial.println("=== result ===");
    Serial.println("{");
    Serial.printf("  \"device\": \"esp32s3\",\n");
    Serial.printf("  \"flash\": \"qspi\",\n");
    Serial.printf("  \"readAt_us\": %llu,\n",              readAt);
    Serial.printf("  \"readFile_us\": %llu,\n",            readAt);
    Serial.printf("  \"writeFile_fixed_us\": %llu,\n",     wf.fixed_us);
    Serial.printf("  \"writeFile_per_kb_us\": %llu,\n",    wf.per_kb_us);
    Serial.printf("  \"writeAt_us\": %llu,\n",             writeAt);
    Serial.printf("  \"removeFile_us\": %llu,\n",          removeFile);
    Serial.printf("  \"listFiles_base_us\": %llu,\n",      lf.base_us);
    Serial.printf("  \"listFiles_per_file_us\": %llu\n",   lf.per_file_us);
    Serial.println("}");
    Serial.println("=== done ===");
}

void loop() {
    delay(60000);
}

#endif // ARDUINO

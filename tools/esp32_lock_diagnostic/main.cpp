#ifdef ARDUINO

#include <Arduino.h>
#include <LittleFS.h>

#include <cstdint>
#include <string>

#include "pqueue/file_store.h"
#include "pqueue/internal/lock_owner.h"
#include "pqueue/queue.h"
#include "pqueue/status.h"

namespace {

constexpr const char* kDiagBasePath = "/pqueue_lock_diag_spool";
constexpr const char* kLockFileName = ".pqueue.lock";

void printLine(const std::string& line) {
    Serial.println(line.c_str());
}

std::string statusSummary(const pqueue::Status& status) {
    std::string out = status.ok() ? "ok" : "failed";
    out += " code=";
    out += pqueue::statusCodeName(status.code);
    if (status.backendCode != 0) {
        out += " backend=";
        out += std::to_string(status.backendCode);
    }
    if (status.message != nullptr && status.message[0] != '\0') {
        out += " message=\"";
        out += status.message;
        out += "\"";
    }
    return out;
}

bool expectStatus(const char* label, const pqueue::Status& status, bool shouldBeOk) {
    const bool passed = status.ok() == shouldBeOk;
    printLine(std::string(passed ? "PASS: " : "FAIL: ") + label + " -> " + statusSummary(status));
    return passed;
}

std::string lockPath() {
    return std::string(kDiagBasePath) + "/" + kLockFileName;
}

bool pathExists(const std::string& path) {
    return LittleFS.exists(path.c_str());
}

bool removeLockFile() {
    const std::string path = lockPath();
    if (!pathExists(path)) {
        return true;
    }
    return LittleFS.remove(path.c_str());
}

bool writeLockFile(const std::string& contents) {
    File file = LittleFS.open(lockPath().c_str(), "w");
    if (!file) {
        return false;
    }
    const std::size_t written = file.write(reinterpret_cast<const std::uint8_t*>(contents.data()), contents.size());
    file.flush();
    file.close();
    return written == contents.size();
}

std::string makeLockContents(const char* owner, const std::string& bootId, const char* token) {
    std::string out;
    out += "pqueue-lock-v2\n";
    out += "owner=";
    out += owner;
    out += "\n";
    out += "pid=0\n";
    out += "boot_id=";
    out += bootId;
    out += "\n";
    out += "token=";
    out += token;
    out += "\n";
    return out;
}

pqueue::Config queueConfig() {
    pqueue::Config config;
    config.basePath = kDiagBasePath;
    config.storageBackend = pqueue::StorageBackend::LittleFS;
    config.reservedBytes = 64 * 1024;
    config.recordSizeBytes = 512;
    config.journalBytes = 4096;
    return config;
}

pqueue::FileStoreConfig fileStoreConfig() {
    pqueue::FileStoreConfig config;
    config.basePath = kDiagBasePath;
    config.backend = pqueue::StorageBackend::LittleFS;
    config.reservedBytes = 64 * 1024;
    config.recordSizeBytes = 512;
    config.journalBytes = 4096;
    return config;
}

bool runLockDiagnostic() {
    bool ok = true;
    const std::string bootId = pqueue::lock_detail::currentBootId();
    const std::string currentLock = makeLockContents("diag", bootId, "current");
    const std::string currentLock2 = makeLockContents("diag", bootId, "current-2");
    const std::string staleLock = makeLockContents("diag", "previous-boot-for-diagnostic", "stale");

    printLine("pqueue_lock_diag: base_path=" + std::string(kDiagBasePath));
    printLine("pqueue_lock_diag: boot_id=" + bootId);

    {
        pqueue::Queue queue(queueConfig());
        const pqueue::Status recover = queue.recoverStaleLock();
        printLine("startup stale-lock recovery: " + statusSummary(recover));
    }

    if (!removeLockFile()) {
        printLine("FAIL: cleanup old diagnostic lock file");
        return false;
    }

    printLine("pqueue_lock_diag: skipping queue format; lock tests do not need a spool file");

    pqueue::FileStore first(fileStoreConfig());
    pqueue::FileStore second(fileStoreConfig());

    ok = expectStatus("first lock acquire", first.tryAcquireLockFile(kLockFileName, currentLock), true) && ok;
    ok = expectStatus("second lock acquire while first holds lock", second.tryAcquireLockFile(kLockFileName, currentLock2), false) && ok;
    ok = expectStatus("first lock release", first.releaseLockFile(kLockFileName, currentLock), true) && ok;
    ok = expectStatus("second lock acquire after release", second.tryAcquireLockFile(kLockFileName, currentLock2), true) && ok;
    ok = expectStatus("second lock release", second.releaseLockFile(kLockFileName, currentLock2), true) && ok;

    if (!writeLockFile(staleLock)) {
        printLine("FAIL: write simulated stale lock file");
        ok = false;
    } else {
        pqueue::Queue queue(queueConfig());
        ok = expectStatus("recover simulated previous-boot lock", queue.recoverStaleLock(), true) && ok;
        const bool removed = !pathExists(lockPath());
        printLine(std::string(removed ? "PASS: " : "FAIL: ") + "stale lock file removed");
        ok = removed && ok;
    }

    if (!writeLockFile(currentLock)) {
        printLine("FAIL: write simulated current-boot lock file");
        ok = false;
    } else {
        pqueue::Queue queue(queueConfig());
        ok = expectStatus("refuse same-boot lock recovery", queue.recoverStaleLock(), false) && ok;
        const bool stillExists = pathExists(lockPath());
        printLine(std::string(stillExists ? "PASS: " : "FAIL: ") + "same-boot lock file preserved");
        ok = stillExists && ok;
        removeLockFile();
    }

    return ok;
}

} // namespace

void setup() {
    Serial.begin(115200);
    delay(1000);

    printLine("pqueue_lock_diag: start");

    if (!LittleFS.begin(false)) {
        printLine("FAIL: LittleFS mount failed");
        printLine("pqueue_lock_diag: done ok=no");
        return;
    }

    printLine(
        "littlefs: mounted total_bytes=" + std::to_string(LittleFS.totalBytes()) +
        " used_bytes=" + std::to_string(LittleFS.usedBytes())
    );

    const bool ok = runLockDiagnostic();
    printLine(std::string("pqueue_lock_diag: done ok=") + (ok ? "yes" : "no"));
}

void loop() {
    delay(1000);
}

#endif // ARDUINO

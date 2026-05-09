#include "file_system.h"
#include "internal/lock_owner.h"

#ifdef ARDUINO

#include <LittleFS.h>

#if defined(ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pqueue {
namespace {

std::string normalizeBasePath(const std::string& basePath) {
    if (basePath.empty() || basePath == "/") {
        return "/";
    }
    std::string out = basePath.front() == '/' ? basePath : "/" + basePath;
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

std::string joinPath(const std::string& base, const std::string& name) {
    if (base == "/") {
        return "/" + name;
    }
    return base + "/" + name;
}

std::string baseName(const char* path) {
    if (path == nullptr) {
        return {};
    }
    const char* slash = std::strrchr(path, '/');
    return slash == nullptr ? std::string(path) : std::string(slash + 1);
}

Status ensureDirectory(const std::string& path) {
    if (path == "/") {
        return Status::success();
    }
    if (LittleFS.mkdir(path.c_str())) {
        return Status::success();
    }

    File dir = LittleFS.open(path.c_str(), "r");
    if (dir && dir.isDirectory()) {
        dir.close();
        return Status::success();
    }
    if (dir) {
        dir.close();
    }
    return Status::failure(StatusCode::MountFailed, "failed to create LittleFS directory");
}


#if defined(ESP32)

struct NamedLittleFsMutex {
    std::string key;
    SemaphoreHandle_t mutex = nullptr;
};

std::vector<NamedLittleFsMutex>& littleFsMutexes() {
    static auto* mutexes = new std::vector<NamedLittleFsMutex>();
    return *mutexes;
}

SemaphoreHandle_t littleFsMutexRegistryLock() {
    static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    return mutex;
}

SemaphoreHandle_t littleFsMutexForKey(const std::string& key) {
    SemaphoreHandle_t registryLock = littleFsMutexRegistryLock();
    if (registryLock == nullptr) {
        return nullptr;
    }

    if (xSemaphoreTake(registryLock, portMAX_DELAY) != pdTRUE) {
        return nullptr;
    }

    auto& mutexes = littleFsMutexes();
    for (const auto& entry : mutexes) {
        if (entry.key == key) {
            xSemaphoreGive(registryLock);
            return entry.mutex;
        }
    }

    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    if (mutex != nullptr) {
        mutexes.push_back(NamedLittleFsMutex{key, mutex});
    }

    xSemaphoreGive(registryLock);
    return mutex;
}

class LittleFsMutexLock final : public Lock {
public:
    explicit LittleFsMutexLock(std::string basePath) : basePath_(std::move(basePath)) {}

    Status acquire(const std::string& name, const std::string& contents) override {
        SemaphoreHandle_t mutex = littleFsMutexForKey(path(name));
        if (mutex == nullptr) {
            return Status::failure(StatusCode::LockTimeout, "failed to create LittleFS queue mutex");
        }

        if (xSemaphoreTake(mutex, 0) != pdTRUE) {
            return Status::failure(StatusCode::LockTimeout, "LittleFS queue mutex is busy");
        }

        const Status recoveryStatus = recoverStaleFileLocked(name, contents);
        if (!recoveryStatus.ok() && recoveryStatus.code != StatusCode::LockTimeout) {
            xSemaphoreGive(mutex);
            return recoveryStatus;
        }
        if (lockFileExists(name)) {
            xSemaphoreGive(mutex);
            return Status::failure(StatusCode::LockTimeout, "LittleFS queue lock already exists");
        }

        Status st = writeLockFile(name, contents);
        if (!st.ok()) {
            xSemaphoreGive(mutex);
            return st;
        }

        acquired_ = true;
        acquiredMutex_ = mutex;
        lockName_ = name;
        lockContents_ = contents;

        removeLegacyLockPath("pqueue.lock");

        return Status::success();
    }

    Status release(const std::string& name, const std::string& expectedContents) override {
        if (!acquired_) {
            return Status::success();
        }

        Status result = Status::success();
        std::string actual;
        Status st = readLockFile(name, actual);
        if (st.ok()) {
            if (actual == expectedContents) {
                result = removeLockFile(name);
            } else {
                result = Status::failure(StatusCode::LockTimeout, "LittleFS queue lock is owned by another session");
            }
        } else {
            result = st;
        }

        acquired_ = false;
        xSemaphoreGive(acquiredMutex_);
        acquiredMutex_ = nullptr;
        lockName_.clear();
        lockContents_.clear();
        return result;
    }

    Status recoverStale(const std::string& name, const std::string& currentContents) override {
        SemaphoreHandle_t mutex = littleFsMutexForKey(path(name));
        if (mutex == nullptr) {
            return Status::failure(StatusCode::LockTimeout, "failed to create LittleFS queue mutex");
        }
        if (xSemaphoreTake(mutex, 0) != pdTRUE) {
            return Status::failure(StatusCode::LockTimeout, "LittleFS queue mutex is busy");
        }
        Status st = recoverStaleFileLocked(name, currentContents);
        xSemaphoreGive(mutex);
        return st;
    }

private:
    std::string path(const std::string& name) const {
        return joinPath(basePath_, name);
    }

    bool entryExists(const std::string& dirPath, const std::string& name) const {
        File dir = LittleFS.open(dirPath.c_str(), "r");
        if (!dir || !dir.isDirectory()) {
            if (dir) {
                dir.close();
            }
            return false;
        }

        File file = dir.openNextFile();
        while (file) {
            const bool match = baseName(file.name()) == name;
            file.close();
            if (match) {
                dir.close();
                return true;
            }
            file = dir.openNextFile();
        }
        dir.close();
        return false;
    }

    bool baseEntryExists(const std::string& name) const {
        return entryExists(basePath_, name);
    }

    void removeLegacyLockPath(const std::string& name) const {
        if (!baseEntryExists(name)) {
            return;
        }

        const std::string fullPath = path(name);
        if (entryExists(fullPath, "owner")) {
            LittleFS.remove((fullPath + "/owner").c_str());
        }
        LittleFS.remove(fullPath.c_str());
        LittleFS.rmdir(fullPath.c_str());
    }

    bool lockFileExists(const std::string& name) const {
        return baseEntryExists(name);
    }

    Status readLockFile(const std::string& name, std::string& out) const {
        File file = LittleFS.open(path(name).c_str(), "r");
        if (!file) {
            return Status::failure(StatusCode::ReadFailed, "failed to open LittleFS lock file for read");
        }
        out.clear();
        while (file.available()) {
            char buffer[96];
            const std::size_t bytesRead = file.read(reinterpret_cast<std::uint8_t*>(buffer), sizeof(buffer));
            if (bytesRead == 0) {
                file.close();
                return Status::failure(StatusCode::ReadFailed, "failed to read LittleFS lock file");
            }
            out.append(buffer, bytesRead);
        }
        file.close();
        return Status::success();
    }

    Status writeLockFile(const std::string& name, const std::string& contents) const {
        File file = LittleFS.open(path(name).c_str(), "w");
        if (!file) {
            return Status::failure(StatusCode::WriteFailed, "failed to open LittleFS lock file for write");
        }
        const std::size_t written = file.write(reinterpret_cast<const std::uint8_t*>(contents.data()), contents.size());
        file.flush();
        file.close();
        if (written != contents.size()) {
            LittleFS.remove(path(name).c_str());
            return Status::failure(StatusCode::WriteFailed, "failed to write LittleFS lock file");
        }
        return Status::success();
    }

    Status removeLockFile(const std::string& name) const {
        if (!LittleFS.remove(path(name).c_str())) {
            return Status::failure(StatusCode::RemoveFailed, "failed to remove LittleFS lock file");
        }
        return Status::success();
    }

    Status recoverStaleFileLocked(const std::string& name, const std::string& currentContents) const {
        if (!lockFileExists(name)) {
            return Status::success();
        }
        std::string existing;
        Status st = readLockFile(name, existing);
        if (!st.ok()) {
            return Status::failure(StatusCode::LockTimeout, "LittleFS queue lock exists but cannot be proven stale");
        }
        if (!lock_detail::lockHasDifferentBootId(existing, currentContents)) {
            return Status::failure(StatusCode::LockTimeout, "LittleFS queue lock is not stale");
        }
        return removeLockFile(name);
    }

    std::string basePath_;
    bool acquired_ = false;
    SemaphoreHandle_t acquiredMutex_ = nullptr;
    std::string lockName_;
    std::string lockContents_;
};

#else

class LittleFsMutexLock final : public Lock {
public:
    explicit LittleFsMutexLock(std::string) {}

    Status acquire(const std::string&, const std::string&) override {
        return Status::success();
    }

    Status release(const std::string&, const std::string&) override {
        return Status::success();
    }

    Status recoverStale(const std::string&, const std::string&) override {
        return Status::success();
    }
};

#endif

class LittleFsFileSystem final : public FileSystem {
public:
    Status mount(const std::string& basePath) override {
        basePath_ = normalizeBasePath(basePath);
        if (!lock_) {
            lock_ = std::make_unique<LittleFsMutexLock>(basePath_);
        }

        // Never format automatically. Users must make that decision explicitly outside PQUEUE.
        if (!LittleFS.begin(false)) {
            return Status::failure(StatusCode::MountFailed, "failed to mount LittleFS");
        }

        return ensureDirectory(basePath_);
    }

    Status readFile(const std::string& name, std::string& out) override {
        File file = LittleFS.open(path(name).c_str(), "r");
        if (!file) {
            return Status::failure(StatusCode::ReadFailed, "failed to open LittleFS file for read");
        }

        out.clear();
        while (file.available()) {
            char buffer[128];
            const std::size_t bytesRead = file.read(reinterpret_cast<std::uint8_t*>(buffer), sizeof(buffer));
            if (bytesRead == 0) {
                file.close();
                return Status::failure(StatusCode::ReadFailed, "failed to read LittleFS file");
            }
            out.append(buffer, bytesRead);
        }
        file.close();
        return Status::success();
    }

    Status writeFile(const std::string& name, const std::string& data) override {
        const std::string fullPath = path(name);
        File file = LittleFS.open(fullPath.c_str(), "w");
        if (!file) {
            return Status::failure(StatusCode::WriteFailed, "failed to open LittleFS file for write");
        }
        const std::size_t bytesWritten = file.write(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
        file.flush();
        file.close();
        if (bytesWritten != data.size()) {
            LittleFS.remove(fullPath.c_str());
            return Status::failure(StatusCode::WriteFailed, "failed to write complete LittleFS file");
        }
        return Status::success();
    }


    Status readAt(const std::string& name, std::uint64_t offset, std::size_t size, std::string& out) override {
        File file = LittleFS.open(path(name).c_str(), "r");
        if (!file) {
            return Status::failure(StatusCode::ReadFailed, "failed to open LittleFS file for positioned read");
        }
        if (!file.seek(static_cast<std::uint32_t>(offset), SeekSet)) {
            file.close();
            return Status::failure(StatusCode::ReadFailed, "failed to seek LittleFS file for read");
        }
        out.assign(size, '\0');
        const std::size_t bytesRead = file.read(reinterpret_cast<std::uint8_t*>(out.data()), size);
        file.close();
        if (bytesRead != size) {
            return Status::failure(StatusCode::ReadFailed, "failed to read complete LittleFS range");
        }
        return Status::success();
    }

    Status writeAt(const std::string& name, std::uint64_t offset, const std::string& data) override {
        File file = LittleFS.open(path(name).c_str(), "r+");
        if (!file) {
            return Status::failure(StatusCode::WriteFailed, "failed to open LittleFS file for positioned write");
        }
        if (!file.seek(static_cast<std::uint32_t>(offset), SeekSet)) {
            file.close();
            return Status::failure(StatusCode::WriteFailed, "failed to seek LittleFS file for write");
        }
        const std::size_t bytesWritten = file.write(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
        file.flush();
        file.close();
        if (bytesWritten != data.size()) {
            return Status::failure(StatusCode::WriteFailed, "failed to write complete LittleFS range");
        }
        return Status::success();
    }

    Status resizeFile(const std::string& name, std::uint64_t size) override {
        const std::string fullPath = path(name);
        File file = fileExistsQuiet(name)
            ? LittleFS.open(fullPath.c_str(), "r+")
            : LittleFS.open(fullPath.c_str(), "w");
        if (!file) {
            return Status::failure(StatusCode::WriteFailed, "failed to open LittleFS file for resize");
        }
        const std::uint64_t current = file.size();
        if (current > size) {
            file.close();
            LittleFS.remove(fullPath.c_str());
            file = LittleFS.open(fullPath.c_str(), "w");
            if (!file) {
                return Status::failure(StatusCode::WriteFailed, "failed to recreate LittleFS file for resize");
            }
        } else if (current < size) {
            if (!file.seek(static_cast<std::uint32_t>(current), SeekSet)) {
                file.close();
                return Status::failure(StatusCode::WriteFailed, "failed to seek LittleFS file for resize");
            }
        }
        char zeros[128] = {};
        std::uint64_t written = current > size ? 0 : current;
        while (written < size) {
            const std::uint64_t remaining = size - written;
            const std::size_t chunk = remaining > sizeof(zeros) ? sizeof(zeros) : static_cast<std::size_t>(remaining);
            if (file.write(reinterpret_cast<const std::uint8_t*>(zeros), chunk) != chunk) {
                file.close();
                return Status::failure(StatusCode::WriteFailed, "failed to size LittleFS file");
            }
            written += chunk;
        }
        file.flush();
        file.close();
        return Status::success();
    }

    Status fileSize(const std::string& name, std::uint64_t& out) override {
        File file = LittleFS.open(path(name).c_str(), "r");
        if (!file) {
            return Status::failure(StatusCode::ReadFailed, "failed to stat LittleFS file");
        }
        out = static_cast<std::uint64_t>(file.size());
        file.close();
        return Status::success();
    }

    Status removeFile(const std::string& name) override {
        if (!LittleFS.remove(path(name).c_str())) {
            return Status::failure(StatusCode::RemoveFailed, "failed to remove LittleFS file");
        }
        return Status::success();
    }

    Status renameFile(const std::string& fromName, const std::string& toName) override {
        if (!LittleFS.rename(path(fromName).c_str(), path(toName).c_str())) {
            return Status::failure(StatusCode::RenameFailed, "failed to rename LittleFS file");
        }
        return Status::success();
    }

    Status listFiles(std::vector<std::string>& out) override {
        File dir = LittleFS.open(basePath_.c_str());
        if (!dir || !dir.isDirectory()) {
            if (dir) {
                dir.close();
            }
            return Status::failure(StatusCode::ListFailed, "failed to open LittleFS directory");
        }
        {
            File file = dir.openNextFile();
            while (file) {
                if (!file.isDirectory()) {
                    out.push_back(baseName(file.name()));
                }
                file.close();
                file = dir.openNextFile();
            }
            dir.close();
        }
        return Status::success();
    }

    Status tryAcquireLockFile(const std::string& name, const std::string& contents) override {
        if (!lock_) {
            return Status::failure(StatusCode::BackendUnavailable, "LittleFS lock backend is not mounted");
        }
        return lock_->acquire(name, contents);
    }

    Status releaseLockFile(const std::string& name, const std::string& expectedContents) override {
        if (!lock_) {
            return Status::failure(StatusCode::BackendUnavailable, "LittleFS lock backend is not mounted");
        }
        return lock_->release(name, expectedContents);
    }

    Status recoverStaleLockFile(const std::string& name, const std::string& currentContents) override {
        if (!lock_) {
            return Status::failure(StatusCode::BackendUnavailable, "LittleFS lock backend is not mounted");
        }
        return lock_->recoverStale(name, currentContents);
    }

    std::uint64_t freeBytes() const override {
        const auto total = LittleFS.totalBytes();
        const auto used = LittleFS.usedBytes();
        return used <= total ? static_cast<std::uint64_t>(total - used) : 0;
    }

private:
    std::string path(const std::string& name) const {
        return joinPath(basePath_, name);
    }

    bool fileExistsQuiet(const std::string& name) const {
        File dir = LittleFS.open(basePath_.c_str(), "r");
        if (!dir || !dir.isDirectory()) {
            if (dir) {
                dir.close();
            }
            return false;
        }

        File file = dir.openNextFile();
        while (file) {
            const bool match = baseName(file.name()) == name;
            file.close();
            if (match) {
                dir.close();
                return true;
            }
            file = dir.openNextFile();
        }
        dir.close();
        return false;
    }

    std::string basePath_ = "/pqueue_spool";
    std::unique_ptr<Lock> lock_;
};

} // namespace

std::shared_ptr<FileSystem> makeLittleFsFileSystem() {
    return std::make_shared<LittleFsFileSystem>();
}

std::shared_ptr<FileSystem> makePosixFileSystem() {
    return nullptr;
}

} // namespace pqueue

#endif // ARDUINO

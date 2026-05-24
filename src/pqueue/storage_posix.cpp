#include "file_system.h"
#include "internal/lock_owner.h"

#ifndef ARDUINO

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <unistd.h>
#include <cstdlib>
#include <signal.h>

namespace pqueue {
namespace {


class PosixFileLock final : public Lock {
public:
    explicit PosixFileLock(std::string basePath) : basePath_(std::move(basePath)) {}

    Status acquire(const std::string& name, const std::string& contents) override {
        for (int attempt = 0; attempt < 2; ++attempt) {
            errno = 0;
            const int fd = ::open(path(name).c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd >= 0) {
                const ssize_t written = ::write(fd, contents.data(), contents.size());
                const int writeErrno = errno;
                ::close(fd);
                if (written != static_cast<ssize_t>(contents.size())) {
                    removeFile(name);
                    return Status::failure(StatusCode::WriteFailed, "failed to write queue lock file", writeErrno);
                }
                return Status::success();
            }
            if (errno != EEXIST) {
                return Status::failure(StatusCode::WriteFailed, "failed to create queue lock file", errno);
            }
            if (attempt == 0 && removeStalePosixLock(name)) {
                continue;
            }
            return Status::failure(StatusCode::LockTimeout, "queue lock already exists", errno);
        }
        return Status::failure(StatusCode::LockTimeout, "queue lock already exists", EEXIST);
    }

    Status release(const std::string& name, const std::string& expectedContents) override {
        std::string actual;
        Status st = readFile(name, actual);
        if (!st.ok()) {
            return st;
        }
        if (actual != expectedContents) {
            return Status::failure(StatusCode::LockTimeout, "queue lock is owned by another process");
        }
        return removeFile(name);
    }

    Status recoverStale(const std::string& name, const std::string&) override {
        std::error_code ec;
        if (!std::filesystem::exists(path(name), ec)) {
            return Status::noOp();
        }
        if (ec) {
            return Status::failure(StatusCode::ReadFailed, "failed to inspect queue lock file", ec.value());
        }
        if (removeStalePosixLock(name)) {
            return Status::success();
        }
        return Status::failure(StatusCode::LockTimeout, "queue lock is not stale");
    }

private:
    Status readFile(const std::string& name, std::string& out) const {
        errno = 0;
        std::ifstream file(path(name), std::ios::binary);
        if (!file) {
            return Status::failure(StatusCode::ReadFailed, "failed to open file for read", errno);
        }
        out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        if (!file.good() && !file.eof()) {
            return Status::failure(StatusCode::ReadFailed, "failed to read file", errno);
        }
        return Status::success();
    }

    Status removeFile(const std::string& name) const {
        std::error_code ec;
        std::filesystem::remove(path(name), ec);
        if (ec) {
            return Status::failure(StatusCode::RemoveFailed, "failed to remove file", ec.value());
        }
        return Status::success();
    }

    bool removeStalePosixLock(const std::string& name) const {
        std::string contents;
        if (!readFile(name, contents).ok()) {
            return false;
        }
        const long pid = lock_detail::lockPid(contents);
        if (pid <= 0) {
            return false;
        }
        errno = 0;
        if (::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM) {
            return false;
        }
        if (errno != ESRCH) {
            return false;
        }
        return removeFile(name).ok();
    }

    std::filesystem::path path(const std::string& name) const {
        return std::filesystem::path(basePath_) / name;
    }

    std::string basePath_;
};

class PosixFileSystem final : public FileSystem {
public:
    Status mount(const std::string& basePath) override {
        basePath_ = basePath;
        lock_ = std::make_unique<PosixFileLock>(basePath_);
        std::error_code ec;
        std::filesystem::create_directories(basePath_, ec);
        if (ec) {
            return Status::failure(StatusCode::MountFailed, "failed to create storage directory", ec.value());
        }
        return Status::success();
    }

    Status readFile(const std::string& name, std::string& out) override {
        errno = 0;
        std::ifstream file(path(name), std::ios::binary);
        if (!file) {
            return Status::failure(StatusCode::ReadFailed, "failed to open file for read", errno);
        }
        out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        if (!file.good() && !file.eof()) {
            return Status::failure(StatusCode::ReadFailed, "failed to read file", errno);
        }
        return Status::success();
    }

    Status writeFile(const std::string& name, const std::string& data) override {
        errno = 0;
        std::ofstream file(path(name), std::ios::binary | std::ios::trunc);
        if (!file) {
            return Status::failure(StatusCode::WriteFailed, "failed to open file for write", errno);
        }
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!file.good()) {
            return Status::failure(StatusCode::WriteFailed, "failed to write file", errno);
        }
        return Status::success();
    }


    Status readAt(const std::string& name, std::uint64_t offset, std::size_t size, std::string& out) override {
        errno = 0;
        std::ifstream file(path(name), std::ios::binary);
        if (!file) {
            return Status::failure(StatusCode::ReadFailed, "failed to open file for positioned read", errno);
        }
        file.seekg(static_cast<std::streamoff>(offset));
        if (!file.good()) {
            return Status::failure(StatusCode::ReadFailed, "failed to seek file for read", errno);
        }
        out.assign(size, '\0');
        file.read(out.data(), static_cast<std::streamsize>(size));
        if (file.gcount() != static_cast<std::streamsize>(size)) {
            return Status::failure(StatusCode::ReadFailed, "failed to read complete file range", errno);
        }
        return Status::success();
    }

    Status writeAt(const std::string& name, std::uint64_t offset, const std::string& data) override {
        errno = 0;
        std::fstream file(path(name), std::ios::binary | std::ios::in | std::ios::out);
        if (!file) {
            return Status::failure(StatusCode::WriteFailed, "failed to open file for positioned write", errno);
        }
        file.seekp(static_cast<std::streamoff>(offset));
        if (!file.good()) {
            return Status::failure(StatusCode::WriteFailed, "failed to seek file for write", errno);
        }
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!file.good()) {
            return Status::failure(StatusCode::WriteFailed, "failed to write complete file range", errno);
        }
        return Status::success();
    }

    Status resizeFile(const std::string& name, std::uint64_t size) override {
        std::error_code ec;
        const auto fullPath = path(name);
        if (!std::filesystem::exists(fullPath, ec)) {
            std::ofstream create(fullPath, std::ios::binary);
            if (!create) {
                return Status::failure(StatusCode::WriteFailed, "failed to create file before resize", errno);
            }
        }
        std::filesystem::resize_file(fullPath, size, ec);
        if (ec) {
            return Status::failure(StatusCode::WriteFailed, "failed to resize file", ec.value());
        }
        return Status::success();
    }

    Status fileSize(const std::string& name, std::uint64_t& out) override {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path(name), ec);
        if (ec) {
            return Status::failure(StatusCode::ReadFailed, "failed to stat file", ec.value());
        }
        out = static_cast<std::uint64_t>(size);
        return Status::success();
    }

    Status removeFile(const std::string& name) override {
        std::error_code ec;
        std::filesystem::remove(path(name), ec);
        if (ec) {
            return Status::failure(StatusCode::RemoveFailed, "failed to remove file", ec.value());
        }
        return Status::success();
    }

    Status listFiles(std::vector<std::string>& out) override {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(basePath_, ec)) {
            if (ec) {
                return Status::failure(StatusCode::ListFailed, "failed to list storage directory", ec.value());
            }
            out.push_back(entry.path().filename().string());
        }
        if (ec) {
            return Status::failure(StatusCode::ListFailed, "failed to list storage directory", ec.value());
        }
        return Status::success();
    }

    Status tryAcquireLockFile(const std::string& name, const std::string& contents) override {
        if (!lock_) {
            return Status::failure(StatusCode::BackendUnavailable, "POSIX lock backend is not mounted");
        }
        return lock_->acquire(name, contents);
    }

    Status releaseLockFile(const std::string& name, const std::string& expectedContents) override {
        if (!lock_) {
            return Status::failure(StatusCode::BackendUnavailable, "POSIX lock backend is not mounted");
        }
        return lock_->release(name, expectedContents);
    }

    Status recoverStaleLockFile(const std::string& name, const std::string& currentContents) override {
        if (!lock_) {
            return Status::failure(StatusCode::BackendUnavailable, "POSIX lock backend is not mounted");
        }
        return lock_->recoverStale(name, currentContents);
    }

    std::uint64_t freeBytes() const override {
        std::error_code ec;
        const auto info = std::filesystem::space(basePath_, ec);
        return ec ? 0 : static_cast<std::uint64_t>(info.available);
    }

private:


    std::filesystem::path path(const std::string& name) const {
        return std::filesystem::path(basePath_) / name;
    }

    std::string basePath_;
    std::unique_ptr<Lock> lock_;
};

} // namespace

std::shared_ptr<FileSystem> makePosixFileSystem() {
    return std::make_shared<PosixFileSystem>();
}

std::shared_ptr<FileSystem> makeLittleFsFileSystem() {
    return nullptr;
}

} // namespace pqueue

#endif // !ARDUINO

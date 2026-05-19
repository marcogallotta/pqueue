#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pqueue/file_system.h"
#include "pqueue/status.h"

struct FsCounters {
    std::uint64_t mount = 0;
    std::uint64_t readFile = 0;
    std::uint64_t writeFile = 0;
    std::uint64_t readAt = 0;
    std::uint64_t writeAt = 0;
    std::uint64_t resizeFile = 0;
    std::uint64_t fileSize = 0;
    std::uint64_t removeFile = 0;
    std::uint64_t renameFile = 0;
    std::uint64_t listFiles = 0;
    std::uint64_t lockAcquire = 0;
    std::uint64_t lockRelease = 0;
    std::uint64_t bytesRead = 0;
    std::uint64_t bytesWritten = 0;
#ifdef ARDUINO
    std::uint32_t msFileSize   = 0;
    std::uint32_t msReadAt     = 0;
    std::uint32_t msWriteAt    = 0;
    std::uint32_t msReadFile   = 0;
    std::uint32_t msWriteFile  = 0;
    std::uint32_t msRemoveFile = 0;
    std::uint32_t msListFiles  = 0;
#endif

    void reset() { *this = FsCounters{}; }

    std::uint64_t metadataOps() const {
        return writeFile + renameFile + removeFile;
    }
};

class CountingFileSystem final : public pqueue::FileSystem {
public:
    explicit CountingFileSystem(std::shared_ptr<pqueue::FileSystem> inner)
        : inner_(std::move(inner)) {}

    const FsCounters& counters() const { return counters_; }
    void resetCounters() { counters_.reset(); }

    pqueue::Status mount(const std::string& basePath) override {
        ++counters_.mount;
        return inner_->mount(basePath);
    }

    pqueue::Status readFile(const std::string& name, std::string& out) override {
        ++counters_.readFile;
#ifdef ARDUINO
        const std::uint32_t _t = millis();
#endif
        auto st = inner_->readFile(name, out);
#ifdef ARDUINO
        counters_.msReadFile += millis() - _t;
#endif
        if (st.ok()) counters_.bytesRead += out.size();
        return st;
    }

    pqueue::Status writeFile(const std::string& name, const std::string& data) override {
        ++counters_.writeFile;
        counters_.bytesWritten += data.size();
#ifdef ARDUINO
        const std::uint32_t _t = millis();
#endif
        auto st = inner_->writeFile(name, data);
#ifdef ARDUINO
        counters_.msWriteFile += millis() - _t;
#endif
        return st;
    }

    pqueue::Status readAt(const std::string& name, std::uint64_t offset, std::size_t size, std::string& out) override {
        ++counters_.readAt;
#ifdef ARDUINO
        const std::uint32_t _t = millis();
#endif
        auto st = inner_->readAt(name, offset, size, out);
#ifdef ARDUINO
        counters_.msReadAt += millis() - _t;
#endif
        if (st.ok()) counters_.bytesRead += out.size();
        return st;
    }

    pqueue::Status writeAt(const std::string& name, std::uint64_t offset, const std::string& data) override {
        ++counters_.writeAt;
        counters_.bytesWritten += data.size();
#ifdef ARDUINO
        const std::uint32_t _t = millis();
#endif
        auto st = inner_->writeAt(name, offset, data);
#ifdef ARDUINO
        counters_.msWriteAt += millis() - _t;
#endif
        return st;
    }

    pqueue::Status resizeFile(const std::string& name, std::uint64_t size) override {
        ++counters_.resizeFile;
        counters_.bytesWritten += size;
        return inner_->resizeFile(name, size);
    }

    pqueue::Status fileSize(const std::string& name, std::uint64_t& out) override {
        ++counters_.fileSize;
#ifdef ARDUINO
        const std::uint32_t _t = millis();
#endif
        auto st = inner_->fileSize(name, out);
#ifdef ARDUINO
        counters_.msFileSize += millis() - _t;
#endif
        return st;
    }

    pqueue::Status removeFile(const std::string& name) override {
        ++counters_.removeFile;
#ifdef ARDUINO
        const std::uint32_t _t = millis();
#endif
        auto st = inner_->removeFile(name);
#ifdef ARDUINO
        counters_.msRemoveFile += millis() - _t;
#endif
        return st;
    }

    pqueue::Status renameFile(const std::string& fromName, const std::string& toName) override {
        ++counters_.renameFile;
        return inner_->renameFile(fromName, toName);
    }

    pqueue::Status listFiles(std::vector<std::string>& out) override {
        ++counters_.listFiles;
#ifdef ARDUINO
        const std::uint32_t _t = millis();
#endif
        auto st = inner_->listFiles(out);
#ifdef ARDUINO
        counters_.msListFiles += millis() - _t;
#endif
        return st;
    }

    pqueue::Status tryAcquireLockFile(const std::string& name, const std::string& contents) override {
        ++counters_.lockAcquire;
        counters_.bytesWritten += contents.size();
        return inner_->tryAcquireLockFile(name, contents);
    }

    pqueue::Status releaseLockFile(const std::string& name, const std::string& expectedContents) override {
        ++counters_.lockRelease;
        return inner_->releaseLockFile(name, expectedContents);
    }

    pqueue::Status recoverStaleLockFile(const std::string& name, const std::string& currentContents) override {
        return inner_->recoverStaleLockFile(name, currentContents);
    }

    std::uint64_t freeBytes() const override {
        return inner_->freeBytes();
    }

private:
    std::shared_ptr<pqueue::FileSystem> inner_;
    FsCounters counters_;
};

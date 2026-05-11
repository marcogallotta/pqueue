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
        auto st = inner_->readFile(name, out);
        if (st.ok()) counters_.bytesRead += out.size();
        return st;
    }

    pqueue::Status writeFile(const std::string& name, const std::string& data) override {
        ++counters_.writeFile;
        counters_.bytesWritten += data.size();
        return inner_->writeFile(name, data);
    }

    pqueue::Status readAt(const std::string& name, std::uint64_t offset, std::size_t size, std::string& out) override {
        ++counters_.readAt;
        auto st = inner_->readAt(name, offset, size, out);
        if (st.ok()) counters_.bytesRead += out.size();
        return st;
    }

    pqueue::Status writeAt(const std::string& name, std::uint64_t offset, const std::string& data) override {
        ++counters_.writeAt;
        counters_.bytesWritten += data.size();
        return inner_->writeAt(name, offset, data);
    }

    pqueue::Status resizeFile(const std::string& name, std::uint64_t size) override {
        ++counters_.resizeFile;
        counters_.bytesWritten += size;
        return inner_->resizeFile(name, size);
    }

    pqueue::Status fileSize(const std::string& name, std::uint64_t& out) override {
        ++counters_.fileSize;
        return inner_->fileSize(name, out);
    }

    pqueue::Status removeFile(const std::string& name) override {
        ++counters_.removeFile;
        return inner_->removeFile(name);
    }

    pqueue::Status renameFile(const std::string& fromName, const std::string& toName) override {
        ++counters_.renameFile;
        return inner_->renameFile(fromName, toName);
    }

    pqueue::Status listFiles(std::vector<std::string>& out) override {
        ++counters_.listFiles;
        return inner_->listFiles(out);
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

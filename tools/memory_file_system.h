#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "pqueue/file_system.h"
#include "pqueue/status.h"

// In-memory FileSystem implementation for simulation use. All I/O stays in
// RAM; no disk access at all. Reset by constructing a new instance per run.
// totalBytes: simulated FS capacity; 0 means effectively unlimited (4 GB).
class MemoryFileSystem final : public pqueue::FileSystem {
public:
    explicit MemoryFileSystem(std::uint64_t totalBytes = 0)
        : totalBytes_(totalBytes == 0 ? (1ULL << 32) : totalBytes) {}

    pqueue::Status mount(const std::string&) override {
        files_.clear();
        return pqueue::Status::success();
    }

    pqueue::Status readFile(const std::string& name, std::string& out) override {
        auto it = files_.find(name);
        if (it == files_.end())
            return pqueue::Status::failure(pqueue::StatusCode::ReadFailed, "file not found");
        out.assign(it->second.begin(), it->second.end());
        return pqueue::Status::success();
    }

    pqueue::Status writeFile(const std::string& name, const std::string& data) override {
        files_[name].assign(data.begin(), data.end());
        return pqueue::Status::success();
    }

    pqueue::Status readAt(const std::string& name, std::uint64_t offset, std::size_t size, std::string& out) override {
        auto it = files_.find(name);
        if (it == files_.end())
            return pqueue::Status::failure(pqueue::StatusCode::ReadFailed, "file not found");
        const auto& buf = it->second;
        if (offset + size > buf.size())
            return pqueue::Status::failure(pqueue::StatusCode::ReadFailed, "read out of range");
        out.assign(buf.begin() + static_cast<std::ptrdiff_t>(offset),
                   buf.begin() + static_cast<std::ptrdiff_t>(offset + size));
        return pqueue::Status::success();
    }

    pqueue::Status writeAt(const std::string& name, std::uint64_t offset, const std::string& data) override {
        auto it = files_.find(name);
        if (it == files_.end())
            return pqueue::Status::failure(pqueue::StatusCode::WriteFailed, "file not found for writeAt");
        auto& buf = it->second;
        const std::size_t end = static_cast<std::size_t>(offset) + data.size();
        if (end > buf.size())
            buf.resize(end, 0);
        std::copy(data.begin(), data.end(), buf.begin() + static_cast<std::ptrdiff_t>(offset));
        return pqueue::Status::success();
    }

    pqueue::Status resizeFile(const std::string& name, std::uint64_t size) override {
        files_[name].resize(static_cast<std::size_t>(size), 0);
        return pqueue::Status::success();
    }

    pqueue::Status fileSize(const std::string& name, std::uint64_t& out) override {
        auto it = files_.find(name);
        if (it == files_.end())
            return pqueue::Status::failure(pqueue::StatusCode::ReadFailed, "file not found");
        out = static_cast<std::uint64_t>(it->second.size());
        return pqueue::Status::success();
    }

    pqueue::Status removeFile(const std::string& name) override {
        files_.erase(name);
        return pqueue::Status::success();
    }

    pqueue::Status renameFile(const std::string& fromName, const std::string& toName) override {
        auto it = files_.find(fromName);
        if (it == files_.end())
            return pqueue::Status::failure(pqueue::StatusCode::RenameFailed, "file not found for rename");
        files_[toName] = std::move(it->second);
        files_.erase(it);
        return pqueue::Status::success();
    }

    pqueue::Status listFiles(std::vector<std::string>& out) override {
        out.clear();
        for (const auto& kv : files_)
            out.push_back(kv.first);
        return pqueue::Status::success();
    }

    pqueue::Status tryAcquireLockFile(const std::string& name, const std::string& contents) override {
        if (locks_.count(name))
            return pqueue::Status::failure(pqueue::StatusCode::LockTimeout, "lock already held");
        locks_[name] = contents;
        return pqueue::Status::success();
    }

    pqueue::Status releaseLockFile(const std::string& name, const std::string& expectedContents) override {
        auto it = locks_.find(name);
        if (it == locks_.end() || it->second != expectedContents)
            return pqueue::Status::failure(pqueue::StatusCode::LockTimeout, "lock not held or contents mismatch");
        locks_.erase(it);
        return pqueue::Status::success();
    }

    pqueue::Status recoverStaleLockFile(const std::string& name, const std::string&) override {
        locks_.erase(name);
        return pqueue::Status::success();
    }

    std::uint64_t freeBytes() const override {
        std::uint64_t used = 0;
        for (const auto& kv : files_) used += kv.second.size();
        return totalBytes_ > used ? totalBytes_ - used : 0;
    }

private:
    std::uint64_t                               totalBytes_;
    std::map<std::string, std::vector<uint8_t>> files_;
    std::map<std::string, std::string>          locks_;
};

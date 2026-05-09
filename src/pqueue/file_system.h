#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "status.h"

namespace pqueue {

class Lock {
public:
    virtual ~Lock() = default;

    virtual Status acquire(const std::string& name, const std::string& contents) = 0;
    virtual Status release(const std::string& name, const std::string& expectedContents) = 0;
    virtual Status recoverStale(const std::string& name, const std::string& currentContents) = 0;
};

class FileSystem {
public:
    virtual ~FileSystem() = default;

    virtual Status mount(const std::string& basePath) = 0;
    virtual Status readFile(const std::string& name, std::string& out) = 0;
    virtual Status writeFile(const std::string& name, const std::string& data) = 0;
    virtual Status readAt(const std::string& name, std::uint64_t offset, std::size_t size, std::string& out) = 0;
    virtual Status writeAt(const std::string& name, std::uint64_t offset, const std::string& data) = 0;
    virtual Status resizeFile(const std::string& name, std::uint64_t size) = 0;
    virtual Status fileSize(const std::string& name, std::uint64_t& out) = 0;
    virtual Status removeFile(const std::string& name) = 0;
    virtual Status renameFile(const std::string& fromName, const std::string& toName) = 0;
    virtual Status listFiles(std::vector<std::string>& out) = 0;
    virtual Status tryAcquireLockFile(const std::string& name, const std::string& contents) = 0;
    virtual Status releaseLockFile(const std::string& name, const std::string& expectedContents) = 0;
    virtual Status recoverStaleLockFile(const std::string& name, const std::string& currentContents) = 0;
    virtual std::uint64_t freeBytes() const = 0;
};

std::shared_ptr<FileSystem> makePosixFileSystem();
std::shared_ptr<FileSystem> makeLittleFsFileSystem();

} // namespace pqueue

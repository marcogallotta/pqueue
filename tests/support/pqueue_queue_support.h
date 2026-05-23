#pragma once

#include "pqueue/file_system.h"
#include "pqueue/queue.h"
#include "pqueue/status.h"

#ifndef ARDUINO

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

inline pqueue::Config makeAppendLogQueueConfig(const std::string& basePath) {
    pqueue::Config cfg;
    cfg.basePath = basePath;
    cfg.maxSegmentBytes = 1024;
    cfg.minFreeBytes = 0;
    return cfg;
}

class FaultInjectingFs final : public pqueue::FileSystem {
public:
    explicit FaultInjectingFs(std::shared_ptr<pqueue::FileSystem> inner)
        : inner_(std::move(inner)) {}

    // Fail the next writeFile whose name contains this substring. Cleared on match.
    std::string failNextWriteFileTo;
    // Fail the next writeAt whose name contains this substring. Cleared on match.
    std::string failNextWriteAtTo;

    pqueue::Status mount(const std::string& p) override { return inner_->mount(p); }
    pqueue::Status readFile(const std::string& n, std::string& o) override { return inner_->readFile(n, o); }
    pqueue::Status writeFile(const std::string& name, const std::string& data) override {
        if (!failNextWriteFileTo.empty() && name.find(failNextWriteFileTo) != std::string::npos) {
            failNextWriteFileTo.clear();
            return pqueue::Status::failure(pqueue::StatusCode::WriteFailed, "injected write failure");
        }
        return inner_->writeFile(name, data);
    }
    pqueue::Status readAt(const std::string& n, std::uint64_t o, std::size_t s, std::string& out) override {
        return inner_->readAt(n, o, s, out);
    }
    pqueue::Status writeAt(const std::string& name, std::uint64_t o, const std::string& d) override {
        if (!failNextWriteAtTo.empty() && name.find(failNextWriteAtTo) != std::string::npos) {
            failNextWriteAtTo.clear();
            return pqueue::Status::failure(pqueue::StatusCode::WriteFailed, "injected writeAt failure");
        }
        return inner_->writeAt(name, o, d);
    }
    pqueue::Status resizeFile(const std::string& n, std::uint64_t s) override { return inner_->resizeFile(n, s); }
    pqueue::Status fileSize(const std::string& n, std::uint64_t& o) override { return inner_->fileSize(n, o); }
    pqueue::Status removeFile(const std::string& n) override { return inner_->removeFile(n); }
    pqueue::Status listFiles(std::vector<std::string>& o) override { return inner_->listFiles(o); }
    pqueue::Status tryAcquireLockFile(const std::string& n, const std::string& c) override { return inner_->tryAcquireLockFile(n, c); }
    pqueue::Status releaseLockFile(const std::string& n, const std::string& c) override { return inner_->releaseLockFile(n, c); }
    pqueue::Status recoverStaleLockFile(const std::string& n, const std::string& c) override { return inner_->recoverStaleLockFile(n, c); }
    std::uint64_t freeBytes() const override { return inner_->freeBytes(); }

private:
    std::shared_ptr<pqueue::FileSystem> inner_;
};

#endif // !ARDUINO

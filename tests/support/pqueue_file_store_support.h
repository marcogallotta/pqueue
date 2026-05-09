#pragma once

#include "pqueue/file_store.h"
#include "pqueue/queue.h"
#include "pqueue/storage_common.h"

#include "doctest/doctest.h"

#ifndef ARDUINO

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pqueue_test {

class FakeFileSystem final : public pqueue::FileSystem {
public:
    pqueue::Status mount(const std::string& basePath) override {
        mountedBasePath = basePath;
        if (failMountOnce.consume()) {
            return pqueue::Status::failure(pqueue::StatusCode::MountFailed, "fake mount failed");
        }
        return pqueue::Status::success();
    }

    pqueue::Status readFile(const std::string& name, std::string& out) override {
        if (failReadOnce.consume()) {
            return pqueue::Status::failure(pqueue::StatusCode::ReadFailed, "fake read failed");
        }
        const auto it = files.find(name);
        if (it == files.end()) {
            return pqueue::Status::failure(pqueue::StatusCode::ReadFailed, "fake file missing");
        }
        out = it->second;
        return pqueue::Status::success();
    }

    pqueue::Status writeFile(const std::string& name, const std::string& data) override {
        if (failWriteOnce.consume()) {
            return pqueue::Status::failure(pqueue::StatusCode::WriteFailed, "fake write failed");
        }
        const auto existing = files.find(name);
        const std::size_t existingSize = existing == files.end() ? 0 : existing->second.size();
        if (usedBytes() - existingSize + data.size() > capacityBytes) {
            return pqueue::Status::failure(pqueue::StatusCode::WriteFailed, "fake capacity exceeded");
        }
        if (partialWriteFileOnce.armed) {
            const auto bytes = std::min(partialWriteFileOnce.bytes, data.size());
            files[name] = data.substr(0, bytes);
            partialWriteFileOnce.clear();
            return pqueue::Status::failure(pqueue::StatusCode::WriteFailed, "fake partial write failed");
        }
        files[name] = data;
        if (writeFileThenFailOnce.consume()) {
            return pqueue::Status::failure(pqueue::StatusCode::WriteFailed, "fake write reported failure after writing");
        }
        return pqueue::Status::success();
    }

    pqueue::Status readAt(const std::string& name, std::uint64_t offset, std::size_t size, std::string& out) override {
        if (failReadOnce.consume()) {
            return pqueue::Status::failure(pqueue::StatusCode::ReadFailed, "fake readAt failed");
        }
        const auto it = files.find(name);
        if (it == files.end() || offset + size > it->second.size()) {
            return pqueue::Status::failure(pqueue::StatusCode::ReadFailed, "fake readAt range missing");
        }
        out = it->second.substr(static_cast<std::size_t>(offset), size);
        return pqueue::Status::success();
    }

    pqueue::Status writeAt(const std::string& name, std::uint64_t offset, const std::string& data) override {
        if (failWriteOnce.consume()) {
            return pqueue::Status::failure(pqueue::StatusCode::WriteFailed, "fake writeAt failed");
        }
        auto it = files.find(name);
        if (it == files.end() || offset + data.size() > it->second.size()) {
            return pqueue::Status::failure(pqueue::StatusCode::WriteFailed, "fake writeAt range missing");
        }
        if (partialWriteAtOnce.armed) {
            const auto bytes = std::min(partialWriteAtOnce.bytes, data.size());
            it->second.replace(static_cast<std::size_t>(offset), bytes, data.substr(0, bytes));
            partialWriteAtOnce.clear();
            return pqueue::Status::failure(pqueue::StatusCode::WriteFailed, "fake partial writeAt failed");
        }
        it->second.replace(static_cast<std::size_t>(offset), data.size(), data);
        if (writeAtThenFailOnce.consume()) {
            return pqueue::Status::failure(pqueue::StatusCode::WriteFailed, "fake writeAt reported failure after writing");
        }
        return pqueue::Status::success();
    }

    pqueue::Status resizeFile(const std::string& name, std::uint64_t size) override {
        if (failWriteOnce.consume()) {
            return pqueue::Status::failure(pqueue::StatusCode::WriteFailed, "fake resize failed");
        }
        const auto existing = files.find(name);
        const std::size_t existingSize = existing == files.end() ? 0 : existing->second.size();
        if (usedBytes() - existingSize + size > capacityBytes) {
            return pqueue::Status::failure(pqueue::StatusCode::WriteFailed, "fake capacity exceeded");
        }
        files[name].resize(static_cast<std::size_t>(size), '\0');
        return pqueue::Status::success();
    }

    pqueue::Status fileSize(const std::string& name, std::uint64_t& out) override {
        const auto it = files.find(name);
        if (it == files.end()) {
            return pqueue::Status::failure(pqueue::StatusCode::ReadFailed, "fake stat missing");
        }
        out = static_cast<std::uint64_t>(it->second.size());
        return pqueue::Status::success();
    }

    pqueue::Status removeFile(const std::string& name) override {
        if (failRemoveOnce.consume()) {
            return pqueue::Status::failure(pqueue::StatusCode::RemoveFailed, "fake remove failed");
        }
        files.erase(name);
        return pqueue::Status::success();
    }

    pqueue::Status renameFile(const std::string& fromName, const std::string& toName) override {
        if (failRenameOnce.consume()) {
            return pqueue::Status::failure(pqueue::StatusCode::RenameFailed, "fake rename failed");
        }
        const auto it = files.find(fromName);
        if (it == files.end()) {
            return pqueue::Status::failure(pqueue::StatusCode::RenameFailed, "fake source missing");
        }
        files[toName] = it->second;
        files.erase(it);
        return pqueue::Status::success();
    }

    pqueue::Status listFiles(std::vector<std::string>& out) override {
        for (const auto& entry : files) {
            out.push_back(entry.first);
        }
        return pqueue::Status::success();
    }

    pqueue::Status tryAcquireLockFile(const std::string& name, const std::string& contents) override {
        if (files.find(name) != files.end()) {
            return pqueue::Status::failure(pqueue::StatusCode::LockTimeout, "fake lock exists");
        }
        files[name] = contents;
        return pqueue::Status::success();
    }

    pqueue::Status releaseLockFile(const std::string& name, const std::string& expectedContents) override {
        const auto it = files.find(name);
        if (it == files.end()) {
            return pqueue::Status::failure(pqueue::StatusCode::ReadFailed, "fake lock missing");
        }
        if (it->second != expectedContents) {
            return pqueue::Status::failure(pqueue::StatusCode::LockTimeout, "fake lock owned by another queue");
        }
        files.erase(it);
        return pqueue::Status::success();
    }

    std::uint64_t freeBytes() const override {
        const auto used = usedBytes();
        return used < capacityBytes ? capacityBytes - used : 0;
    }

    void failNextMount() { failMountOnce.arm(); }
    void failNextRead() { failReadOnce.arm(); }
    void failNextWrite() { failWriteOnce.arm(); }
    void failNextRemove() { failRemoveOnce.arm(); }
    void failNextRename() { failRenameOnce.arm(); }
    void partialNextWriteFile(std::size_t bytes) { partialWriteFileOnce.arm(bytes); }
    void partialNextWriteAt(std::size_t bytes) { partialWriteAtOnce.arm(bytes); }
    void writeFileThenFail() { writeFileThenFailOnce.arm(); }
    void writeAtThenFail() { writeAtThenFailOnce.arm(); }

    void setCapacityBytes(std::size_t bytes) { capacityBytes = bytes; }

    bool exists(const std::string& name) const {
        return files.find(name) != files.end();
    }

    void corruptFile(const std::string& name) {
        auto it = files.find(name);
        REQUIRE(it != files.end());
        REQUIRE_FALSE(it->second.empty());
        it->second[it->second.size() / 2] ^= static_cast<char>(0xff);
    }

    void corruptSlotHeader(std::uint32_t slot, std::size_t slotSize) {
        auto it = files.find("pqueue.spool");
        REQUIRE(it != files.end());
        const auto offset = pqueue::storage_detail::kCheckpointSlots * pqueue::storage_detail::kCheckpointRecordBytes + 4096 + slot * slotSize;
        REQUIRE(it->second.size() >= offset + 1);
        it->second[offset] ^= static_cast<char>(0xff);
    }

    void corruptSlotPayload(std::uint32_t slot, std::size_t slotSize) {
        auto it = files.find("pqueue.spool");
        REQUIRE(it != files.end());
        const auto offset = pqueue::storage_detail::kCheckpointSlots * pqueue::storage_detail::kCheckpointRecordBytes + 4096 + slot * slotSize + pqueue::storage_detail::kRecordHeaderBytes;
        REQUIRE(it->second.size() > offset);
        it->second[offset] ^= static_cast<char>(0xff);
    }

    std::map<std::string, std::string> files;
    std::string mountedBasePath;

private:
    struct OneShotFailure {
        void arm() { armed = true; }
        bool consume() {
            if (!armed) {
                return false;
            }
            armed = false;
            return true;
        }
        bool armed = false;
    };

    struct OneShotPartialWrite {
        void arm(std::size_t prefixBytes) {
            armed = true;
            bytes = prefixBytes;
        }
        void clear() {
            armed = false;
            bytes = 0;
        }
        bool armed = false;
        std::size_t bytes = 0;
    };

    std::size_t usedBytes() const {
        std::size_t out = 0;
        for (const auto& entry : files) {
            out += entry.second.size();
        }
        return out;
    }

    OneShotFailure failMountOnce;
    OneShotFailure failReadOnce;
    OneShotFailure failWriteOnce;
    OneShotFailure failRemoveOnce;
    OneShotFailure failRenameOnce;
    OneShotFailure writeFileThenFailOnce;
    OneShotFailure writeAtThenFailOnce;
    OneShotPartialWrite partialWriteFileOnce;
    OneShotPartialWrite partialWriteAtOnce;
    std::size_t capacityBytes = std::numeric_limits<std::size_t>::max();
};

struct CapturedEvent {
    pqueue::EventKind kind = pqueue::EventKind::Diagnostic;
    pqueue::Severity severity = pqueue::Severity::Debug;
    pqueue::StatusCode code = pqueue::StatusCode::Ok;
};

inline void captureEvent(const pqueue::Event& event, void* user) {
    auto* events = static_cast<std::vector<CapturedEvent>*>(user);
    events->push_back(CapturedEvent{event.kind, event.severity, event.status.code});
}

inline std::shared_ptr<FakeFileSystem> makeFakeFileSystem() {
    return std::make_shared<FakeFileSystem>();
}

inline pqueue::FileStore makeStore(
    const std::shared_ptr<FakeFileSystem>& fileSystem,
    pqueue::EventOptions events = {},
    std::uint32_t reservedBytes = 160,
    std::size_t recordSizeBytes = 32,
    std::uint32_t checkpointEveryOps = 64
) {
    pqueue::FileStoreConfig config;
    config.basePath = "/fake-pqueue";
    config.fileSystem = fileSystem;
    config.events = events;
    config.reservedBytes = reservedBytes;
    config.recordSizeBytes = recordSizeBytes;
    config.checkpointEveryOps = checkpointEveryOps;
    return pqueue::FileStore(config);
}

inline pqueue::Config makeQueueConfig(
    const std::shared_ptr<FakeFileSystem>& fileSystem,
    std::uint32_t reservedBytes = 160,
    std::size_t recordSizeBytes = 32
) {
    pqueue::Config config;
    config.basePath = "/fake-pqueue";
    config.fileSystem = fileSystem;
    config.reservedBytes = reservedBytes;
    config.recordSizeBytes = recordSizeBytes;
    return config;
}

inline std::size_t slotSize(std::size_t recordSizeBytes = 32) {
    return sizeof(pqueue::storage_detail::RecordHeader) + recordSizeBytes;
}

} // namespace pqueue_test

#endif // !ARDUINO

#ifndef ARDUINO

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#define private public
#include "pqueue/queue.h"
#undef private

#include "pqueue/storage_common.h"
#include "pqueue/internal/lock_owner.h"

#include "doctest/doctest.h"

namespace {

class FakeFileSystem final : public pqueue::FileSystem {
public:
    pqueue::Status mount(const std::string& basePath) override {
        mountedBasePath = basePath;
        return pqueue::Status::success();
    }

    pqueue::Status readFile(const std::string& name, std::string& out) override {
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
        files[name] = data;
        return pqueue::Status::success();
    }

    pqueue::Status readAt(const std::string& name, std::uint64_t offset, std::size_t size, std::string& out) override {
        if (failReadAtFromOffsetArmed && name == failReadAtName && offset >= failReadAtMinOffset) {
            failReadAtFromOffsetArmed = false;
            return pqueue::Status::failure(pqueue::StatusCode::ReadFailed, "fake targeted readAt failed");
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
        it->second.replace(static_cast<std::size_t>(offset), data.size(), data);
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
        files.erase(name);
        return pqueue::Status::success();
    }

    pqueue::Status renameFile(const std::string& fromName, const std::string& toName) override {
        const auto it = files.find(fromName);
        if (it == files.end()) {
            return pqueue::Status::failure(pqueue::StatusCode::RenameFailed, "fake source missing");
        }
        files[toName] = it->second;
        files.erase(it);
        return pqueue::Status::success();
    }

    pqueue::Status listFiles(std::vector<std::string>& out) override {
        out.clear();
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

    pqueue::Status recoverStaleLockFile(const std::string& name, const std::string& currentContents) override {
        const auto it = files.find(name);
        if (it == files.end()) {
            return pqueue::Status::success();
        }
        if (!pqueue::lock_detail::lockHasDifferentBootId(it->second, currentContents)) {
            return pqueue::Status::failure(pqueue::StatusCode::LockTimeout, "fake lock is not stale");
        }
        files.erase(it);
        return pqueue::Status::success();
    }

    std::uint64_t freeBytes() const override {
        const auto used = usedBytes();
        return used < capacityBytes ? capacityBytes - used : 0;
    }

    void failNextWrite() { failWriteOnce.arm(); }

    void failNextReadAtFromOffset(const std::string& name, std::uint64_t minOffset) {
        failReadAtFromOffsetArmed = true;
        failReadAtName = name;
        failReadAtMinOffset = minOffset;
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

    std::size_t usedBytes() const {
        std::size_t out = 0;
        for (const auto& entry : files) {
            out += entry.second.size();
        }
        return out;
    }

    OneShotFailure failWriteOnce;
    bool failReadAtFromOffsetArmed = false;
    std::string failReadAtName;
    std::uint64_t failReadAtMinOffset = 0;
    std::size_t capacityBytes = std::numeric_limits<std::size_t>::max();
};

std::shared_ptr<FakeFileSystem> makeFakeFileSystem() {
    return std::make_shared<FakeFileSystem>();
}

pqueue::Config makeQueueConfig(
    const std::shared_ptr<FakeFileSystem>& fileSystem,
    std::uint32_t reservedBytes = 160,
    std::size_t recordSizeBytes = 32,
    std::uint32_t checkpointEveryOps = 64
) {
    pqueue::Config config;
    config.basePath = "/fake-pqueue";
    config.fileSystem = fileSystem;
    config.reservedBytes = reservedBytes;
    config.recordSizeBytes = recordSizeBytes;
    config.checkpointEveryOps = checkpointEveryOps;
    return config;
}

std::uint64_t recordRegionOffset(const pqueue::Config& config) {
    return static_cast<std::uint64_t>(pqueue::storage_detail::kCheckpointSlots) *
               pqueue::storage_detail::kCheckpointRecordBytes +
           config.journalBytes;
}

struct VisitContext {
    std::vector<std::string> records;
    std::vector<std::uint32_t> sequences;
    std::vector<std::uint32_t> ordinals;
    std::size_t stopAfter = std::numeric_limits<std::size_t>::max();
};

bool capturingVisitor(void* rawContext, const std::string& record, std::uint32_t sequence, std::uint32_t ordinal) {
    auto* context = static_cast<VisitContext*>(rawContext);
    context->records.push_back(record);
    context->sequences.push_back(sequence);
    context->ordinals.push_back(ordinal);
    return context->records.size() < context->stopAfter;
}

} // namespace

TEST_CASE("pqueue reports invalid storage config") {
    auto fs = makeFakeFileSystem();
    pqueue::Queue queue(makeQueueConfig(fs, 1, 32));

    const auto status = queue.enqueue("x");

    CHECK_FALSE(status.ok());
    CHECK(status.code == pqueue::StatusCode::InvalidArgument);
}

TEST_CASE("pqueue rewriteFront rejects oversized record and keeps front unchanged") {
    auto fs = makeFakeFileSystem();
    pqueue::Queue queue(makeQueueConfig(fs, 160, 4));

    REQUIRE(queue.enqueue("1234").ok());
    const auto status = queue.rewriteFront("12345");

    CHECK_FALSE(status.ok());
    CHECK(status.code == pqueue::StatusCode::RecordTooLarge);

    std::string out;
    REQUIRE(queue.peek(out).ok());
    CHECK_EQ(out, "1234");
}

TEST_CASE("pqueue visitRecords rejects null visitor") {
    auto fs = makeFakeFileSystem();
    pqueue::Queue queue(makeQueueConfig(fs));
    REQUIRE(queue.enqueue("one").ok());

    const auto status = queue.visitRecords(nullptr, nullptr);

    CHECK_FALSE(status.ok());
    CHECK(status.code == pqueue::StatusCode::InvalidArgument);
}

TEST_CASE("pqueue visitRecords stops when visitor returns false") {
    auto fs = makeFakeFileSystem();
    pqueue::Queue queue(makeQueueConfig(fs));
    REQUIRE(queue.enqueue("one").ok());
    REQUIRE(queue.enqueue("two").ok());
    REQUIRE(queue.enqueue("three").ok());

    VisitContext context;
    context.stopAfter = 1;
    const auto status = queue.visitRecords(capturingVisitor, &context);

    CHECK(status.ok());
    REQUIRE_EQ(context.records.size(), 1U);
    CHECK_EQ(context.records[0], "one");
    CHECK_EQ(context.sequences[0], 0U);
    CHECK_EQ(context.ordinals[0], 0U);
}

TEST_CASE("pqueue visitRecords returns read failure from active record") {
    auto fs = makeFakeFileSystem();
    const auto config = makeQueueConfig(fs);
    pqueue::Queue queue(config);
    REQUIRE(queue.enqueue("one").ok());
    REQUIRE(queue.enqueue("two").ok());

    fs->failNextReadAtFromOffset("pqueue.spool", recordRegionOffset(config));

    VisitContext context;
    const auto status = queue.visitRecords(capturingVisitor, &context);

    CHECK_FALSE(status.ok());
    CHECK(status.code == pqueue::StatusCode::ReadFailed);
    CHECK(context.records.empty());
}

TEST_CASE("pqueue pop preserves front when index write fails") {
    auto fs = makeFakeFileSystem();
    pqueue::Queue queue(makeQueueConfig(fs));
    REQUIRE(queue.enqueue("one").ok());
    REQUIRE(queue.enqueue("two").ok());

    fs->failNextWrite();
    const auto status = queue.pop();

    CHECK_FALSE(status.ok());
    CHECK(status.code == pqueue::StatusCode::WriteFailed);

    std::string out;
    REQUIRE(queue.peek(out).ok());
    CHECK_EQ(out, "one");
    CHECK_EQ(queue.stats().count, 2U);
}

#endif // !ARDUINO

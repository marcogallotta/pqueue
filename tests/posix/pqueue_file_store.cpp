#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#define private public
#include "pqueue/file_store.h"
#undef private

#include "pqueue/outbox.h"
#include "pqueue/queue.h"
#include "pqueue/storage_common.h"
#include "pqueue/internal/lock_owner.h"

#include "doctest/doctest.h"

#ifndef ARDUINO

namespace {

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
        files[name] = data;
        return pqueue::Status::success();
    }

    pqueue::Status readAt(const std::string& name, std::uint64_t offset, std::size_t size, std::string& out) override {
        if (failReadOnce.consume()) {
            return pqueue::Status::failure(pqueue::StatusCode::ReadFailed, "fake readAt failed");
        }
        if (targetedReadAtOnce.consume(name, offset)) {
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
        if (partialWriteAtOnce.armed) {
            const auto bytes = std::min(partialWriteAtOnce.bytes, data.size());
            it->second.replace(static_cast<std::size_t>(offset), bytes, data.substr(0, bytes));
            partialWriteAtOnce.clear();
            return pqueue::Status::failure(pqueue::StatusCode::WriteFailed, "fake partial writeAt failed");
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

    void failNextMount() { failMountOnce.arm(); }
    void failNextRead() { failReadOnce.arm(); }
    void failNextWrite() { failWriteOnce.arm(); }
    void failNextRemove() { failRemoveOnce.arm(); }
    void failNextRename() { failRenameOnce.arm(); }
    void partialNextWriteAt(std::size_t bytes) { partialWriteAtOnce.arm(bytes); }
    void failNextReadAt(const std::string& name, std::uint64_t offset) { targetedReadAtOnce.arm(name, offset); }

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

    struct TargetedReadAtFailure {
        void arm(const std::string& targetName, std::uint64_t targetOffset) {
            armed = true;
            name = targetName;
            offset = targetOffset;
        }

        bool consume(const std::string& actualName, std::uint64_t actualOffset) {
            if (!armed || name != actualName || offset != actualOffset) {
                return false;
            }
            armed = false;
            return true;
        }

        bool armed = false;
        std::string name;
        std::uint64_t offset = 0;
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
    OneShotPartialWrite partialWriteAtOnce;
    TargetedReadAtFailure targetedReadAtOnce;
    std::size_t capacityBytes = std::numeric_limits<std::size_t>::max();
};

struct CapturedEvent {
    pqueue::EventKind kind = pqueue::EventKind::Diagnostic;
    pqueue::Severity severity = pqueue::Severity::Debug;
    pqueue::StatusCode code = pqueue::StatusCode::Ok;
};

void captureEvent(const pqueue::Event& event, void* user) {
    auto* events = static_cast<std::vector<CapturedEvent>*>(user);
    events->push_back(CapturedEvent{event.kind, event.severity, event.status.code});
}

std::shared_ptr<FakeFileSystem> makeFakeFileSystem() {
    return std::make_shared<FakeFileSystem>();
}

pqueue::FileStore makeStore(const std::shared_ptr<FakeFileSystem>& fileSystem, pqueue::EventOptions events = {}, std::uint32_t reservedBytes = 160, std::size_t recordSizeBytes = 32, std::uint32_t checkpointEveryOps = 64) {
    pqueue::FileStoreConfig config;
    config.basePath = "/fake-pqueue";
    config.fileSystem = fileSystem;
    config.events = events;
    config.reservedBytes = reservedBytes;
    config.recordSizeBytes = recordSizeBytes;
    config.checkpointEveryOps = checkpointEveryOps;
    return pqueue::FileStore(config);
}

pqueue::Config makeQueueConfig(const std::shared_ptr<FakeFileSystem>& fileSystem, pqueue::EventOptions events = {}, std::uint32_t reservedBytes = 160, std::size_t recordSizeBytes = 32, std::uint32_t checkpointEveryOps = 64) {
    pqueue::Config config;
    config.basePath = "/fake-pqueue";
    config.fileSystem = fileSystem;
    config.events = events;
    config.reservedBytes = reservedBytes;
    config.recordSizeBytes = recordSizeBytes;
    config.checkpointEveryOps = checkpointEveryOps;
    return config;
}

struct FakeClock {
    std::uint64_t nowMs = 1000;
};

std::uint64_t fakeClockNow(void* context) {
    return static_cast<FakeClock*>(context)->nowMs;
}

struct FakeSender {
    std::vector<pqueue::SendDecision> decisions;
    std::vector<std::string> payloads;
    std::vector<pqueue::RetryState> retries;
};

pqueue::SendResult fakeSend(void* context, const std::string& payload, const pqueue::RetryState& retry) {
    auto* sender = static_cast<FakeSender*>(context);
    sender->payloads.push_back(payload);
    sender->retries.push_back(retry);

    if (sender->decisions.empty()) {
        return {pqueue::SendDecision::Sent};
    }

    const pqueue::SendDecision decision = sender->decisions.front();
    sender->decisions.erase(sender->decisions.begin());
    return {decision};
}

pqueue::OutboxConfig makeOutboxConfig() {
    pqueue::OutboxConfig config;
    config.retryDelayMs = 0;
    config.maxDrainAttemptsPerSecond = 10;
    return config;
}

pqueue::Outbox makeOutbox(
    const std::shared_ptr<FakeFileSystem>& fileSystem,
    FakeSender& sender,
    FakeClock& clock,
    pqueue::OutboxConfig outboxConfig = makeOutboxConfig()
) {
    return pqueue::Outbox(makeQueueConfig(fileSystem), outboxConfig, fakeSend, &sender, fakeClockNow, &clock);
}

std::size_t slotSize(std::size_t recordSizeBytes = 32) {
    return pqueue::storage_detail::kRecordHeaderBytes + recordSizeBytes;
}

std::size_t spoolSize(std::uint32_t reservedBytes = 160, std::size_t recordSizeBytes = 32, std::uint32_t journalBytes = 4096) {
    const auto slots = reservedBytes / slotSize(recordSizeBytes);
    return pqueue::storage_detail::kCheckpointSlots * pqueue::storage_detail::kCheckpointRecordBytes +
           journalBytes +
           slots * slotSize(recordSizeBytes);
}

std::size_t checkpointOffset(std::uint32_t slot) {
    return slot * pqueue::storage_detail::kCheckpointRecordBytes;
}

std::size_t journalOffset(std::uint32_t journalEntryIndex) {
    return pqueue::storage_detail::kCheckpointSlots * pqueue::storage_detail::kCheckpointRecordBytes +
           journalEntryIndex * pqueue::storage_detail::kJournalEntryBytes;
}

std::size_t activeSlotOffset(std::uint32_t sequence, std::uint32_t reservedBytes = 160, std::size_t recordSizeBytes = 32, std::uint32_t journalBytes = 4096) {
    const auto slots = reservedBytes / slotSize(recordSizeBytes);
    return pqueue::storage_detail::kCheckpointSlots * pqueue::storage_detail::kCheckpointRecordBytes +
           journalBytes +
           (sequence % slots) * slotSize(recordSizeBytes);
}

bool hasErrorCode(const pqueue::ValidationResult& result, pqueue::ValidationIssueCode code) {
    return std::any_of(result.errors.begin(), result.errors.end(), [code](const pqueue::ValidationIssue& issue) {
        return issue.code == code;
    });
}

} // namespace


TEST_CASE("FileStore validate detects config mismatch") {
    auto fileSystem = makeFakeFileSystem();
    {
        auto store = makeStore(fileSystem, {}, 160, 32);
        REQUIRE(store.writeIndex({0, 1, 1}).ok());
    }

    auto changedConfig = makeStore(fileSystem, {}, 160, 58);
    const auto result = changedConfig.validateUnlocked();

    REQUIRE_FALSE(result.ok);
    CHECK(hasErrorCode(result, pqueue::ValidationIssueCode::ConfigMismatch));
}

TEST_CASE("FileStore validate detects missing spool") {
    auto fileSystem = makeFakeFileSystem();
    auto store = makeStore(fileSystem);

    const auto result = store.validateUnlocked();

    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(result.errors.empty());
    CHECK(result.errors[0].code == pqueue::ValidationIssueCode::SpoolMissing);
}

TEST_CASE("FileStore validate detects wrong spool size") {
    auto fileSystem = makeFakeFileSystem();
    fileSystem->files["pqueue.spool"] = std::string(17, '\0');
    auto store = makeStore(fileSystem);

    const auto result = store.validateUnlocked();

    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(result.errors.empty());
    CHECK(result.errors[0].code == pqueue::ValidationIssueCode::SpoolSizeMismatch);
}

TEST_CASE("FileStore validate stops early when corrupt checkpoints exceed max errors") {
    auto fileSystem = makeFakeFileSystem();
    {
        auto store = makeStore(fileSystem, {}, 160, 32, 1);
        REQUIRE(store.writeIndex({0, 1, 1}).ok());
        REQUIRE(store.writeIndex({0, 2, 2}).ok());
        REQUIRE(store.writeIndex({0, 3, 3}).ok());
    }

    for (std::uint32_t slot = 0; slot < pqueue::storage_detail::kCheckpointSlots; ++slot) {
        fileSystem->files["pqueue.spool"][checkpointOffset(slot)] ^= static_cast<char>(0xff);
    }

    pqueue::ValidationOptions options;
    options.maxErrors = 1;
    auto store = makeStore(fileSystem, {}, 160, 32, 1);
    const auto result = store.validateUnlocked(options);

    REQUIRE_FALSE(result.ok);
    CHECK(result.stoppedEarly);
    CHECK_EQ(result.errors.size(), 1U);
    CHECK(result.errors[0].code == pqueue::ValidationIssueCode::MetadataCorrupt);
}

TEST_CASE("FileStore validate detects non-contiguous journal entry") {
    auto fileSystem = makeFakeFileSystem();
    {
        auto store = makeStore(fileSystem, {}, 160, 32, 64);
        REQUIRE(store.writeRecord(0, "one").ok());
        REQUIRE(store.writeIndex({0, 1, 1}).ok());
        REQUIRE(store.writeRecord(1, "two").ok());
        REQUIRE(store.writeIndex({0, 2, 2}).ok());
    }

    std::string bytes = fileSystem->files["pqueue.spool"].substr(journalOffset(1), pqueue::storage_detail::kJournalEntryBytes);
    pqueue::storage_detail::JournalEntry entry;
    REQUIRE(pqueue::storage_detail::parseJournalEntry(bytes, entry));
    entry.generation += 1;
    entry.crc = pqueue::storage_detail::journalCrc(entry);
    fileSystem->files["pqueue.spool"].replace(journalOffset(1), pqueue::storage_detail::kJournalEntryBytes, pqueue::storage_detail::serializeJournalEntry(entry));

    auto store = makeStore(fileSystem, {}, 160, 32, 64);
    const auto result = store.validateUnlocked();

    REQUIRE_FALSE(result.ok);
    CHECK(hasErrorCode(result, pqueue::ValidationIssueCode::JournalCorrupt));
}

TEST_CASE("FileStore validate detects active slot read failure") {
    auto fileSystem = makeFakeFileSystem();
    {
        auto store = makeStore(fileSystem);
        REQUIRE(store.writeRecord(0, "payload").ok());
        REQUIRE(store.writeIndex({0, 1, 1}).ok());
    }

    fileSystem->failNextReadAt("pqueue.spool", activeSlotOffset(0));
    auto store = makeStore(fileSystem);
    const auto result = store.validateUnlocked();

    REQUIRE_FALSE(result.ok);
    CHECK(hasErrorCode(result, pqueue::ValidationIssueCode::SlotReadFailed));
}

TEST_CASE("FileStore mounts and preallocates one spool file") {
    auto fileSystem = makeFakeFileSystem();
    auto store = makeStore(fileSystem);

    REQUIRE(store.mount().ok());
    CHECK_EQ(fileSystem->mountedBasePath, "/fake-pqueue");
    REQUIRE(fileSystem->exists("pqueue.spool"));
    CHECK_EQ(fileSystem->files["pqueue.spool"].size(), spoolSize());
}

TEST_CASE("FileStore write/read uses fixed ring slots") {
    auto fileSystem = makeFakeFileSystem();
    auto store = makeStore(fileSystem);

    REQUIRE(store.writeRecord(0, "zero").ok());
    REQUIRE(store.writeRecord(3, "three").ok());

    std::string out;
    REQUIRE(store.readRecord(3, out).ok());
    CHECK_EQ(out, "three");
    CHECK_FALSE(store.readRecord(0, out).ok());
}

TEST_CASE("FileStore rejects records larger than the configured slot payload") {
    auto fileSystem = makeFakeFileSystem();
    auto store = makeStore(fileSystem, {}, 96, 8);

    CHECK_FALSE(store.writeRecord(0, "123456789").ok());
}

TEST_CASE("FileStore emits diagnostic event when slot write fails") {
    auto fileSystem = makeFakeFileSystem();
    std::vector<CapturedEvent> events;
    auto store = makeStore(fileSystem, pqueue::EventOptions{captureEvent, &events});

    REQUIRE(store.mount().ok());
    fileSystem->failNextWrite();

    const auto status = store.writeRecord(0, "payload");
    REQUIRE_FALSE(status.ok());
    REQUIRE_EQ(events.size(), 1U);
    CHECK(events[0].kind == pqueue::EventKind::Diagnostic);
    CHECK(events[0].severity == pqueue::Severity::Error);
    CHECK(events[0].code == pqueue::StatusCode::WriteFailed);
}

TEST_CASE("FileStore rejects corrupt spool slot") {
    auto fileSystem = makeFakeFileSystem();
    auto store = makeStore(fileSystem);

    REQUIRE(store.writeRecord(0, "payload").ok());
    fileSystem->corruptSlotHeader(0, slotSize());

    std::string out;
    CHECK_FALSE(store.readRecord(0, out).ok());
}

TEST_CASE("FileStore keeps the older valid checkpoint when the latest is corrupt") {
    auto fileSystem = makeFakeFileSystem();
    auto store = makeStore(fileSystem, {}, 160, 32, 1);

    REQUIRE(store.writeIndex({0, 1, 1}).ok());
    REQUIRE(store.writeIndex({0, 2, 2}).ok());
    REQUIRE(fileSystem->exists("pqueue.spool"));

    const auto latestCheckpointOffset = 3U * pqueue::storage_detail::kCheckpointRecordBytes;
    fileSystem->files["pqueue.spool"][latestCheckpointOffset] ^= static_cast<char>(0xff);

    auto reopened = makeStore(fileSystem, {}, 160, 32, 1);
    pqueue::FileStoreIndex out;
    REQUIRE(reopened.readIndex(out).ok());
    CHECK_EQ(out.head, 0U);
    CHECK_EQ(out.tail, 1U);
    CHECK_EQ(out.count, 1U);
}

TEST_CASE("FileStore starts with an empty index when no metadata exists") {
    auto fileSystem = makeFakeFileSystem();
    auto store = makeStore(fileSystem);

    pqueue::FileStoreIndex out;
    REQUIRE(store.readIndex(out).ok());
    CHECK_EQ(out.head, 0U);
    CHECK_EQ(out.tail, 0U);
    CHECK_EQ(out.count, 0U);
}

TEST_CASE("FileStore fails loudly when storage config changes") {
    auto fileSystem = makeFakeFileSystem();
    {
        auto store = makeStore(fileSystem, {}, 160, 32);
        REQUIRE(store.writeIndex({0, 1, 1}).ok());
    }

    auto reopenedWithDifferentSlotSize = makeStore(fileSystem, {}, 160, 64);
    pqueue::FileStoreIndex out;
    const auto status = reopenedWithDifferentSlotSize.readIndex(out);
    REQUIRE_FALSE(status.ok());
    CHECK(status.code == pqueue::StatusCode::InvalidIndex);
}

TEST_CASE("FileStore rounds reserved bytes down to whole slots") {
    auto fileSystem = makeFakeFileSystem();
    auto store = makeStore(fileSystem, {}, static_cast<std::uint32_t>(slotSize() * 2 + 7), 32);

    REQUIRE(store.mount().ok());
    CHECK_EQ(fileSystem->files["pqueue.spool"].size(), spoolSize(static_cast<std::uint32_t>(slotSize() * 2 + 7), 32));
}

TEST_CASE("FileStore rejects configs that cannot fit one slot") {
    auto fileSystem = makeFakeFileSystem();
    auto store = makeStore(fileSystem, {}, 4, 32);

    CHECK_FALSE(store.mount().ok());
}


TEST_CASE("Queue does not advance index after torn record write") {
    auto fileSystem = makeFakeFileSystem();

    {
        auto store = makeStore(fileSystem);
        REQUIRE(store.mount().ok());
    }

    {
        auto queue = pqueue::Queue(makeQueueConfig(fileSystem));
        fileSystem->partialNextWriteAt(pqueue::storage_detail::kRecordHeaderBytes / 2);
        const auto status = queue.enqueue("first");
        REQUIRE_FALSE(status.ok());
        CHECK(status.code == pqueue::StatusCode::WriteFailed);
    }

    {
        auto store = makeStore(fileSystem);
        pqueue::FileStoreIndex index;
        REQUIRE(store.readIndex(index).ok());
        CHECK_EQ(index.head, 0U);
        CHECK_EQ(index.tail, 0U);
        CHECK_EQ(index.count, 0U);
    }

    {
        auto queue = pqueue::Queue(makeQueueConfig(fileSystem));
        std::string out;
        CHECK(queue.peek(out).code == pqueue::StatusCode::QueueEmpty);

        REQUIRE(queue.enqueue("second").ok());
        REQUIRE(queue.peek(out).ok());
        CHECK_EQ(out, "second");

        const auto validation = queue.validate();
        CHECK(validation.ok);
        CHECK_EQ(validation.checkedRecords, 1U);
    }
}

TEST_CASE("Outbox drops corrupt front record when index already points at it") {
    auto fileSystem = makeFakeFileSystem();
    FakeClock clock;

    {
        FakeSender sender;
        sender.decisions.push_back(pqueue::SendDecision::RetryLater);
        auto outbox = makeOutbox(fileSystem, sender, clock);

        REQUIRE(outbox.submit("corrupt-front").status == pqueue::SubmitStatus::Queued);
        REQUIRE(outbox.submit("valid-behind").status == pqueue::SubmitStatus::Queued);
    }

    {
        auto store = makeStore(fileSystem);
        pqueue::FileStoreIndex index;
        REQUIRE(store.readIndex(index).ok());
        CHECK_EQ(index.head, 0U);
        CHECK_EQ(index.tail, 2U);
        CHECK_EQ(index.count, 2U);
    }

    fileSystem->corruptSlotPayload(0, slotSize());

    FakeSender sender;
    auto outbox = makeOutbox(fileSystem, sender, clock);
    const auto drain = outbox.drainUpTo(2);

    CHECK_EQ(drain.corruptDropped, 1U);
    CHECK_EQ(drain.sent, 1U);
    CHECK_FALSE(drain.queueError);
    REQUIRE_EQ(sender.payloads.size(), 1U);
    CHECK_EQ(sender.payloads[0], "valid-behind");
    CHECK_EQ(outbox.stats().count, 0U);
}


TEST_CASE("FileStore initializes an all-zero spool left by interrupted first format") {
    auto fileSystem = makeFakeFileSystem();
    fileSystem->files["pqueue.spool"] = std::string(spoolSize(), '\0');
    auto store = makeStore(fileSystem);

    REQUIRE(store.mount().ok());

    pqueue::FileStoreIndex out;
    REQUIRE(store.readIndex(out).ok());
    CHECK_EQ(out.head, 0U);
    CHECK_EQ(out.tail, 0U);
    CHECK_EQ(out.count, 0U);
}

TEST_CASE("FileStore fails loudly when metadata is missing but spool is not empty") {
    auto fileSystem = makeFakeFileSystem();
    fileSystem->files["pqueue.spool"] = std::string(spoolSize(), '\0');
    fileSystem->files["pqueue.spool"][pqueue::storage_detail::kCheckpointSlots * pqueue::storage_detail::kCheckpointRecordBytes] = static_cast<char>(0x7f);
    auto store = makeStore(fileSystem);

    const auto status = store.mount();
    REQUIRE_FALSE(status.ok());
    CHECK(status.code == pqueue::StatusCode::InvalidIndex);
}

TEST_CASE("FileStore recreates fresh storage when the single spool file is missing") {
    auto fileSystem = makeFakeFileSystem();
    {
        auto store = makeStore(fileSystem);
        REQUIRE(store.writeIndex({0, 1, 1}).ok());
    }
    REQUIRE(fileSystem->exists("pqueue.spool"));
    fileSystem->files.erase("pqueue.spool");

    auto reopened = makeStore(fileSystem);
    REQUIRE(reopened.mount().ok());
    pqueue::FileStoreIndex out;
    REQUIRE(reopened.readIndex(out).ok());
    CHECK_EQ(out.count, 0U);
}

TEST_CASE("FileStore fails loudly when spool size does not match layout") {
    auto fileSystem = makeFakeFileSystem();
    {
        auto store = makeStore(fileSystem);
        REQUIRE(store.writeIndex({0, 1, 1}).ok());
    }
    fileSystem->files["pqueue.spool"].resize(slotSize() * 2);

    auto reopened = makeStore(fileSystem);
    const auto status = reopened.mount();
    REQUIRE_FALSE(status.ok());
    CHECK(status.code == pqueue::StatusCode::InvalidIndex);
}



TEST_CASE("FileStore fails loudly when all checkpoint slots are corrupt") {
    auto fileSystem = makeFakeFileSystem();
    {
        auto store = makeStore(fileSystem, {}, 160, 32, 1);
        REQUIRE(store.writeIndex({0, 1, 1}).ok());
        REQUIRE(store.writeIndex({0, 2, 2}).ok());
        REQUIRE(store.writeIndex({0, 3, 3}).ok());
    }

    REQUIRE(fileSystem->exists("pqueue.spool"));
    for (std::uint32_t slot = 0; slot < pqueue::storage_detail::kCheckpointSlots; ++slot) {
        const auto offset = slot * pqueue::storage_detail::kCheckpointRecordBytes;
        fileSystem->files["pqueue.spool"][offset] ^= static_cast<char>(0xff);
    }

    auto reopened = makeStore(fileSystem, {}, 160, 32, 1);
    pqueue::FileStoreIndex out;
    const auto status = reopened.readIndex(out);

    REQUIRE_FALSE(status.ok());
    CHECK(status.code == pqueue::StatusCode::InvalidIndex);
}

TEST_CASE("FileStore rejects index transition when checkpoint write fails") {
    auto fileSystem = makeFakeFileSystem();
    auto store = makeStore(fileSystem, {}, 160, 32, 1);

    REQUIRE(store.writeIndex({0, 1, 1}).ok());
    pqueue::FileStoreIndex before;
    REQUIRE(store.readIndex(before).ok());
    CHECK_EQ(before.count, 1U);

    fileSystem->failNextWrite();
    const auto status = store.writeIndex({0, 2, 2});

    REQUIRE_FALSE(status.ok());
    CHECK(status.code == pqueue::StatusCode::WriteFailed);

    pqueue::FileStoreIndex after;
    REQUIRE(store.readIndex(after).ok());
    CHECK_EQ(after.head, before.head);
    CHECK_EQ(after.tail, before.tail);
    CHECK_EQ(after.count, before.count);
}

TEST_CASE("FileStore rejects index transition when journal append fails") {
    auto fileSystem = makeFakeFileSystem();
    auto store = makeStore(fileSystem, {}, 160, 32, 64);

    REQUIRE(store.writeIndex({0, 1, 1}).ok());
    pqueue::FileStoreIndex before;
    REQUIRE(store.readIndex(before).ok());
    CHECK_EQ(before.count, 1U);

    fileSystem->failNextWrite();
    const auto status = store.writeIndex({0, 2, 2});

    REQUIRE_FALSE(status.ok());
    CHECK(status.code == pqueue::StatusCode::WriteFailed);

    pqueue::FileStoreIndex after;
    REQUIRE(store.readIndex(after).ok());
    CHECK_EQ(after.head, before.head);
    CHECK_EQ(after.tail, before.tail);
    CHECK_EQ(after.count, before.count);
}


TEST_CASE("FileStore ignores partially written journal entry after reboot") {
    auto fileSystem = makeFakeFileSystem();
    {
        auto store = makeStore(fileSystem, {}, 160, 32, 64);
        REQUIRE(store.writeRecord(0, "one").ok());
        REQUIRE(store.writeIndex({0, 1, 1}).ok());

        REQUIRE(store.writeRecord(1, "two").ok());
        fileSystem->partialNextWriteAt(pqueue::storage_detail::kJournalEntryBytes / 2);
        const auto status = store.writeIndex({0, 2, 2});
        REQUIRE_FALSE(status.ok());
        CHECK(status.code == pqueue::StatusCode::WriteFailed);
    }

    auto reopened = makeStore(fileSystem, {}, 160, 32, 64);
    pqueue::FileStoreIndex index;
    REQUIRE(reopened.readIndex(index).ok());
    CHECK_EQ(index.head, 0U);
    CHECK_EQ(index.tail, 1U);
    CHECK_EQ(index.count, 1U);

    std::string out;
    REQUIRE(reopened.readRecord(0, out).ok());
    CHECK_EQ(out, "one");
}

TEST_CASE("FileStore ignores partially written checkpoint after reboot") {
    auto fileSystem = makeFakeFileSystem();
    {
        auto store = makeStore(fileSystem, {}, 160, 32, 1);
        REQUIRE(store.writeRecord(0, "one").ok());
        REQUIRE(store.writeIndex({0, 1, 1}).ok());

        REQUIRE(store.writeRecord(1, "two").ok());
        fileSystem->partialNextWriteAt(pqueue::storage_detail::kCheckpointRecordBytes / 2);
        const auto status = store.writeIndex({0, 2, 2});
        REQUIRE_FALSE(status.ok());
        CHECK(status.code == pqueue::StatusCode::WriteFailed);
    }

    auto reopened = makeStore(fileSystem, {}, 160, 32, 1);
    pqueue::FileStoreIndex index;
    REQUIRE(reopened.readIndex(index).ok());
    CHECK_EQ(index.head, 0U);
    CHECK_EQ(index.tail, 1U);
    CHECK_EQ(index.count, 1U);

    std::string out;
    REQUIRE(reopened.readRecord(0, out).ok());
    CHECK_EQ(out, "one");
}

TEST_CASE("FileStore replays committed journal entries after reboot") {
    auto fileSystem = makeFakeFileSystem();
    {
        auto store = makeStore(fileSystem, {}, 160, 32, 64);
        REQUIRE(store.writeRecord(0, "one").ok());
        REQUIRE(store.writeIndex({0, 1, 1}).ok());
        REQUIRE(store.writeRecord(1, "two").ok());
        REQUIRE(store.writeIndex({0, 2, 2}).ok());
    }

    auto reopened = makeStore(fileSystem, {}, 160, 32, 64);
    pqueue::FileStoreIndex index;
    REQUIRE(reopened.readIndex(index).ok());
    CHECK_EQ(index.head, 0U);
    CHECK_EQ(index.tail, 2U);
    CHECK_EQ(index.count, 2U);

    std::string out;
    REQUIRE(reopened.readRecord(0, out).ok());
    CHECK_EQ(out, "one");
    REQUIRE(reopened.readRecord(1, out).ok());
    CHECK_EQ(out, "two");
}

TEST_CASE("Outbox reports queue error and preserves record when pop fails after send") {
    auto fileSystem = makeFakeFileSystem();
    FakeClock clock;

    {
        FakeSender sender;
        sender.decisions.push_back(pqueue::SendDecision::RetryLater);
        auto outbox = makeOutbox(fileSystem, sender, clock);
        REQUIRE(outbox.submit("maybe-duplicate").status == pqueue::SubmitStatus::Queued);
    }

    FakeSender sender;
    auto outbox = makeOutbox(fileSystem, sender, clock);
    fileSystem->failNextWrite();
    const auto drain = outbox.drain();

    CHECK_EQ(drain.attempts, 1U);
    CHECK_EQ(drain.sent, 0U);
    CHECK(drain.queueError);
    CHECK(drain.detail.code == pqueue::StatusCode::WriteFailed);
    REQUIRE_EQ(sender.payloads.size(), 1U);
    CHECK_EQ(sender.payloads[0], "maybe-duplicate");
    CHECK_EQ(outbox.stats().count, 1U);

    FakeSender reopenedSender;
    auto reopened = makeOutbox(fileSystem, reopenedSender, clock);
    const auto retryDrain = reopened.drain();

    CHECK_FALSE(retryDrain.queueError);
    CHECK_EQ(retryDrain.sent, 1U);
    REQUIRE_EQ(reopenedSender.payloads.size(), 1U);
    CHECK_EQ(reopenedSender.payloads[0], "maybe-duplicate");
    CHECK_EQ(reopened.stats().count, 0U);
}


#endif // !ARDUINO

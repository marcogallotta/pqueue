#include "pqueue/queue.h"
#include "pqueue/internal/lock_owner.h"
#include "support/pqueue_file_store_support.h"

#include "doctest/doctest.h"

#ifndef ARDUINO

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {

constexpr const char* kSpoolName = "pqueue.spool";
constexpr const char* kLockName = ".pqueue.lock";

std::string lockContentsForBoot(const std::string& bootId, long pid = 0) {
    std::ostringstream out;
    out << "pqueue-lock-v2\n";
    out << "owner=queue\n";
    out << "pid=" << pid << "\n";
    out << "boot_id=" << bootId << "\n";
    out << "token=test-token\n";
    return out.str();
}

std::filesystem::path lockRecoveryDir(const char* name) {
    return std::filesystem::path("build/pqueue-spools") / name;
}

} // namespace

TEST_CASE("queue format clears records and allows reuse") {
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    pqueue::Queue queue(pqueue_test::makeQueueConfig(fileSystem, 4096, 64));

    REQUIRE(queue.enqueue("one").ok());
    REQUIRE(queue.enqueue("two").ok());
    CHECK_EQ(queue.stats().count, 2U);

    CHECK(queue.format().ok());
    CHECK_EQ(queue.stats().count, 0U);

    std::string out;
    CHECK(queue.peek(out).code == pqueue::StatusCode::QueueEmpty);

    REQUIRE(queue.enqueue("after-format").ok());
    REQUIRE(queue.peek(out).ok());
    CHECK_EQ(out, "after-format");
}

TEST_CASE("queue format recovers corrupt metadata explicitly") {
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    pqueue::Config config = pqueue_test::makeQueueConfig(fileSystem, 4096, 64);

    {
        pqueue::Queue queue(config);
        REQUIRE(queue.enqueue("before-corruption").ok());
        REQUIRE(fileSystem->exists(kSpoolName));
    }

    fileSystem->files[kSpoolName].assign(fileSystem->files[kSpoolName].size(), static_cast<char>(0xff));

    pqueue::Queue queue(config);
    CHECK(queue.statsResult().status.code == pqueue::StatusCode::InvalidIndex);

    CHECK(queue.format().ok());
    CHECK_EQ(queue.stats().count, 0U);

    REQUIRE(queue.enqueue("after-format").ok());
    std::string out;
    REQUIRE(queue.peek(out).ok());
    CHECK_EQ(out, "after-format");
}


TEST_CASE("queue dropFrontIfCorrupt drops corrupt front record only") {
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    pqueue::Config config = pqueue_test::makeQueueConfig(fileSystem, 4096, 64);
    pqueue::Queue queue(config);

    REQUIRE(queue.enqueue("bad-front").ok());
    REQUIRE(queue.enqueue("good-second").ok());
    fileSystem->corruptSlotPayload(0, pqueue_test::slotSize(64));

    std::string out;
    CHECK(queue.peek(out).code == pqueue::StatusCode::CrcMismatch);

    CHECK(queue.dropFrontIfCorrupt().ok());
    CHECK_EQ(queue.stats().count, 1U);

    REQUIRE(queue.peek(out).ok());
    CHECK_EQ(out, "good-second");
}

TEST_CASE("queue dropFrontIfCorrupt refuses healthy front record") {
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    pqueue::Queue queue(pqueue_test::makeQueueConfig(fileSystem, 4096, 64));

    REQUIRE(queue.enqueue("healthy").ok());
    CHECK(queue.dropFrontIfCorrupt().code == pqueue::StatusCode::InvalidArgument);
    CHECK_EQ(queue.stats().count, 1U);

    std::string out;
    REQUIRE(queue.peek(out).ok());
    CHECK_EQ(out, "healthy");
}

TEST_CASE("queue dropFrontIfCorrupt refuses empty queue") {
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    pqueue::Queue queue(pqueue_test::makeQueueConfig(fileSystem, 4096, 64));

    CHECK(queue.dropFrontIfCorrupt().code == pqueue::StatusCode::QueueEmpty);
}


TEST_CASE("queue validate suggests format for corrupt metadata") {
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    pqueue::Config config = pqueue_test::makeQueueConfig(fileSystem, 4096, 64);

    {
        pqueue::Queue queue(config);
        REQUIRE(queue.enqueue("before-corruption").ok());
        REQUIRE(fileSystem->exists(kSpoolName));
    }

    fileSystem->files[kSpoolName].assign(fileSystem->files[kSpoolName].size(), static_cast<char>(0xff));

    pqueue::Queue queue(config);
    const auto validation = queue.validate();
    REQUIRE_FALSE(validation.ok);
    REQUIRE_FALSE(validation.errors.empty());
    CHECK(validation.errors[0].repairAction == pqueue::ValidationRepairAction::Format);
}

TEST_CASE("queue validate suggests dropFrontIfCorrupt only for corrupt front slot") {
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    pqueue::Config config = pqueue_test::makeQueueConfig(fileSystem, 4096, 64);
    pqueue::Queue queue(config);

    REQUIRE(queue.enqueue("bad-front").ok());
    REQUIRE(queue.enqueue("good-second").ok());
    fileSystem->corruptSlotPayload(0, pqueue_test::slotSize(64));

    const auto validation = queue.validate();
    REQUIRE_FALSE(validation.ok);
    REQUIRE_FALSE(validation.errors.empty());
    CHECK(validation.errors[0].code == pqueue::ValidationIssueCode::SlotCrcMismatch);
    CHECK(validation.errors[0].repairAction == pqueue::ValidationRepairAction::DropFrontIfCorrupt);
}

TEST_CASE("queue validate does not suggest front drop for later corrupt slot") {
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    pqueue::Config config = pqueue_test::makeQueueConfig(fileSystem, 4096, 64);
    pqueue::Queue queue(config);

    REQUIRE(queue.enqueue("good-front").ok());
    REQUIRE(queue.enqueue("bad-second").ok());
    fileSystem->corruptSlotPayload(1, pqueue_test::slotSize(64));

    const auto validation = queue.validate();
    REQUIRE_FALSE(validation.ok);
    REQUIRE_FALSE(validation.errors.empty());
    CHECK(validation.errors[0].code == pqueue::ValidationIssueCode::SlotCrcMismatch);
    CHECK(validation.errors[0].repairAction == pqueue::ValidationRepairAction::None);
}


TEST_CASE("queue recoverStaleLock removes previous-boot token lock") {
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    fileSystem->files[kLockName] = lockContentsForBoot("previous-boot");

    pqueue::Queue queue(pqueue_test::makeQueueConfig(fileSystem, 4096, 64));

    CHECK(queue.recoverStaleLock().ok());
    CHECK_FALSE(fileSystem->exists(kLockName));
    CHECK(queue.enqueue("after-recovery").ok());
}

TEST_CASE("queue recoverStaleLock refuses current-boot token lock") {
    auto fileSystem = pqueue_test::makeFakeFileSystem();
    fileSystem->files[kLockName] = lockContentsForBoot(pqueue::lock_detail::currentBootId());

    pqueue::Queue queue(pqueue_test::makeQueueConfig(fileSystem, 4096, 64));

    CHECK(queue.recoverStaleLock().code == pqueue::StatusCode::LockTimeout);
    CHECK(fileSystem->exists(kLockName));
}

TEST_CASE("queue recoverStaleLock removes stale POSIX pid lock") {
    const auto dir = lockRecoveryDir("stale-posix-pid");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    REQUIRE_FALSE(ec);

    const auto lockPath = dir / kLockName;
    {
        std::ofstream out(lockPath, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out << lockContentsForBoot("old-posix-process", 999999);
    }

    pqueue::Config config;
    config.basePath = dir.string();
    pqueue::Queue queue(config);

    CHECK(queue.recoverStaleLock().ok());
    CHECK_FALSE(std::filesystem::exists(lockPath));

    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("queue recoverStaleLock refuses live POSIX pid lock") {
    const auto dir = lockRecoveryDir("live-posix-pid");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    REQUIRE_FALSE(ec);

    const auto lockPath = dir / kLockName;
    {
        std::ofstream out(lockPath, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out << lockContentsForBoot("current-posix-process", static_cast<long>(::getpid()));
    }

    pqueue::Config config;
    config.basePath = dir.string();
    pqueue::Queue queue(config);

    CHECK(queue.recoverStaleLock().code == pqueue::StatusCode::LockTimeout);
    CHECK(std::filesystem::exists(lockPath));

    std::filesystem::remove_all(dir, ec);
}

#endif // !ARDUINO

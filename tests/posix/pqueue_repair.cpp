#include "pqueue/queue.h"
#include "pqueue/internal/lock_owner.h"

#include "doctest/doctest.h"

#ifndef ARDUINO

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {

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

std::filesystem::path repairDir(const char* name) {
    return std::filesystem::path("build/pqueue-spools") / name;
}

pqueue::Config makeRepairConfig(const std::filesystem::path& dir) {
    pqueue::Config cfg;
    cfg.basePath = dir.string();
    cfg.storeLayout = pqueue::StoreLayout::AppendLog;
    cfg.minFreeBytes = 0;
    return cfg;
}

} // namespace

TEST_CASE("queue format clears records and allows reuse") {
    const auto dir = repairDir("repair-format");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    pqueue::Queue queue(makeRepairConfig(dir));

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

    std::filesystem::remove_all(dir, ec);
}

// queue format recovers corrupt metadata explicitly -- deferred:
// requires manifest/segment file corruption on real POSIX FS; not ported blindly.

TEST_CASE("queue recoverStaleLock removes previous-boot token lock") {
    const auto dir = repairDir("repair-stale-boot-token");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    REQUIRE_FALSE(ec);

    {
        std::ofstream out(dir / kLockName, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out << lockContentsForBoot("previous-boot", 999999);
    }

    pqueue::Queue queue(makeRepairConfig(dir));

    CHECK(queue.recoverStaleLock().ok());
    CHECK_FALSE(std::filesystem::exists(dir / kLockName));
    CHECK(queue.enqueue("after-recovery").ok());

    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("queue recoverStaleLock refuses current-boot token lock") {
    const auto dir = repairDir("repair-current-boot-token");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    REQUIRE_FALSE(ec);

    {
        std::ofstream out(dir / kLockName, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out << lockContentsForBoot(pqueue::lock_detail::currentBootId(), static_cast<long>(::getpid()));
    }

    pqueue::Queue queue(makeRepairConfig(dir));

    CHECK(queue.recoverStaleLock().code == pqueue::StatusCode::LockTimeout);
    CHECK(std::filesystem::exists(dir / kLockName));

    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("queue recoverStaleLock removes stale POSIX pid lock") {
    const auto dir = repairDir("stale-posix-pid");
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

    pqueue::Queue queue(makeRepairConfig(dir));

    CHECK(queue.recoverStaleLock().ok());
    CHECK_FALSE(std::filesystem::exists(lockPath));

    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("queue recoverStaleLock refuses live POSIX pid lock") {
    const auto dir = repairDir("live-posix-pid");
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

    pqueue::Config config = makeRepairConfig(dir);
    pqueue::Queue queue(config);

    CHECK(queue.recoverStaleLock().code == pqueue::StatusCode::LockTimeout);
    CHECK(std::filesystem::exists(lockPath));

    std::filesystem::remove_all(dir, ec);
}

#endif // !ARDUINO

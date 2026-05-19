#include "pqueue/append_log_store.h"
#include "pqueue/append_log_common.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace pqueue {

using namespace append_log_detail;

namespace {

constexpr const char* kManifestSlotA = "manifest-a.bin";
constexpr const char* kManifestSlotB = "manifest-b.bin";

// Returns the manifest slot to write to, or nullptr when at least one slot file
// exists but no slot is parseable (caller should surface DataCorrupt).
const char* chooseInactiveSlot(bool existsA, bool validA, std::uint32_t epochA,
                                bool existsB, bool validB, std::uint32_t epochB) {
    if (!existsA && !existsB) return kManifestSlotA;  // fresh store
    if (!validA && !validB)   return nullptr;           // file(s) exist but none parse
    if (!validA)              return kManifestSlotA;
    if (!validB)              return kManifestSlotB;
    // Both valid: write to the lower-epoch slot (it becomes the new winner next read)
    return (epochB <= epochA) ? kManifestSlotB : kManifestSlotA;
}

// Selects the winning manifest from two slots. Returns true and sets out if at
// least one slot is valid; returns false if neither is valid.
bool chooseWinningSlot(bool validA, const ManifestData& mdA,
                       bool validB, const ManifestData& mdB,
                       ManifestData& out) {
    if (!validA && !validB) return false;
    if (validA && !validB)  { out = mdA; return true; }
    if (!validA)            { out = mdB; return true; }
    // Both valid: higher epoch wins; equal epoch → slot A
    out = (mdB.epoch > mdA.epoch) ? mdB : mdA;
    return true;
}

inline const char* otherSlot(const char* slot) {
    return (slot == kManifestSlotA) ? kManifestSlotB : kManifestSlotA;
}

} // namespace

Status AppendLogStore::publishManifest(const ManifestData& manifest) {
    auto f = fs();
    if (!f) return Status::failure(StatusCode::BackendUnavailable, "no file system");

#ifdef ARDUINO
    const std::uint32_t t_pub_start = millis();
    std::uint32_t ms_probe = 0, ms_serial = 0, ms_write = 0;
#endif

    const char* writeSlot = nullptr;
    std::uint32_t winningEpoch = 0;

    if (cachedWrittenSlot_) {
        // Fast path: skip the 2x fileSize + 2x readFile probe entirely.
        // The slot we last wrote is the current winner; write to the other one.
        writeSlot = otherSlot(cachedWrittenSlot_);
        winningEpoch = cachedWrittenEpoch_;
    } else {
        // Slow path: probe both slots from disk.
        auto fileExists = [&](const char* name) -> bool {
            std::uint64_t dummy;
            return f->fileSize(name, dummy).ok();
        };
        auto tryRead = [&](const char* name, ManifestData& md) -> bool {
            std::string data;
            if (!f->readFile(name, data).ok()) return false;
            return parseManifest(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), md);
        };

        const bool existsA = fileExists(kManifestSlotA);
        const bool existsB = fileExists(kManifestSlotB);
        ManifestData mdA, mdB;
        const bool validA = existsA && tryRead(kManifestSlotA, mdA);
        const bool validB = existsB && tryRead(kManifestSlotB, mdB);

        writeSlot = chooseInactiveSlot(existsA, validA, mdA.epoch,
                                       existsB, validB, mdB.epoch);
        if (!writeSlot) {
            return Status::failure(StatusCode::DataCorrupt, "manifest slot(s) exist but none are valid");
        }

        ManifestData winning;
        winningEpoch = chooseWinningSlot(validA, mdA, validB, mdB, winning) ? winning.epoch : 0;
    }

#ifdef ARDUINO
    ms_probe = millis() - t_pub_start;
#endif

    ManifestData toWrite = manifest;
    toWrite.epoch = winningEpoch + 1;

#ifdef ARDUINO
    const std::uint32_t t_serial = millis();
#endif
    std::vector<std::uint8_t> bytes;
    serialiseManifest(toWrite, bytes);
    const std::string data(reinterpret_cast<const char*>(bytes.data()), bytes.size());
#ifdef ARDUINO
    ms_serial = millis() - t_serial;
    const std::uint32_t t_write = millis();
#endif

    Status st = f->writeFile(writeSlot, data);
#ifdef ARDUINO
    ms_write = millis() - t_write;
#endif
    if (!st.ok()) return st;

    applyManifestToRam(toWrite);

    // Update cache: the slot we just wrote is now the winner.
    cachedWrittenSlot_ = writeSlot;
    cachedWrittenEpoch_ = toWrite.epoch;

#ifdef ARDUINO
    dbgLastProbeMs_ = ms_probe;
    dbgLastWriteMs_ = ms_write;
#endif

    return Status::success();
}

bool AppendLogStore::readManifest(ManifestData& out) {
    auto f = fs();
    if (!f) return false;

    auto tryRead = [&](const char* name, ManifestData& md) -> bool {
        std::string data;
        if (!f->readFile(name, data).ok()) return false;
        return parseManifest(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), md);
    };

    ManifestData mdA, mdB;
    const bool validA = tryRead(kManifestSlotA, mdA);
    const bool validB = tryRead(kManifestSlotB, mdB);
    return chooseWinningSlot(validA, mdA, validB, mdB, out);
}

void AppendLogStore::applyManifestToRam(const ManifestData& md) {
    manifestRanges_ = md.ranges;
    activeGenerations_.clear();
    for (const auto& r : md.ranges) {
        for (std::uint32_t g = r.startGen; g <= r.endGen; ++g) {
            activeGenerations_.push_back(g);
        }
    }
    if (md.tailGeneration != 0) {
        activeGenerations_.push_back(md.tailGeneration);
    }
    nextGeneration_ = md.nextGeneration;
}

} // namespace pqueue

#include "pqueue/append_log_store.h"
#include "pqueue/append_log_common.h"

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

} // namespace

Status AppendLogStore::publishManifest(const ManifestData& manifest) {
    auto f = fs();
    if (!f) return Status::failure(StatusCode::BackendUnavailable, "no file system");

    auto fileExists = [&](const char* name) -> bool {
        std::uint64_t dummy;
        return f->fileSize(name, dummy).ok();
    };
    auto tryRead = [&](const char* name, ManifestData& md) -> bool {
        std::string data;
        if (!f->readFile(name, data).ok()) return false;
        return parseManifest(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), md);
    };

#ifdef ARDUINO
    const std::uint32_t t_pub_start = millis();
    std::uint32_t ms_probe = 0, ms_serial = 0, ms_write = 0, ms_apply = 0, ms_cleanup = 0;
    std::uint32_t _t0 = millis();
#endif

    const bool existsA = fileExists(kManifestSlotA);
    const bool existsB = fileExists(kManifestSlotB);
    ManifestData mdA, mdB;
    const bool validA = existsA && tryRead(kManifestSlotA, mdA);
    const bool validB = existsB && tryRead(kManifestSlotB, mdB);

    const char* writeSlot = chooseInactiveSlot(existsA, validA, mdA.epoch,
                                                existsB, validB, mdB.epoch);
#ifdef ARDUINO
    ms_probe = millis() - _t0;
#endif
    if (!writeSlot) {
        return Status::failure(StatusCode::DataCorrupt, "manifest slot(s) exist but none are valid");
    }

    ManifestData winning;
    const std::uint32_t winningEpoch =
        chooseWinningSlot(validA, mdA, validB, mdB, winning) ? winning.epoch : 0;

    ManifestData toWrite = manifest;
    toWrite.epoch = winningEpoch + 1;

#ifdef ARDUINO
    _t0 = millis();
#endif
    std::vector<std::uint8_t> bytes;
    serialiseManifest(toWrite, bytes);
    const std::string data(reinterpret_cast<const char*>(bytes.data()), bytes.size());
#ifdef ARDUINO
    ms_serial = millis() - _t0;
    _t0 = millis();
#endif

    Status st = f->writeFile(writeSlot, data);
#ifdef ARDUINO
    ms_write = millis() - _t0;
#endif
    if (!st.ok()) return st;

#ifdef ARDUINO
    _t0 = millis();
#endif
    applyManifestToRam(toWrite);
#ifdef ARDUINO
    ms_apply = millis() - _t0;
    _t0 = millis();
#endif

    cleanupOneDanglingSegment();
#ifdef ARDUINO
    ms_cleanup = millis() - _t0;
    Serial.printf("[publishManifest] slot=%s probe_ms=%u serial_ms=%u write_ms=%u apply_ms=%u cleanup_ms=%u total_ms=%u\n",
        writeSlot, ms_probe, ms_serial, ms_write, ms_apply, ms_cleanup, millis() - t_pub_start);
    Serial.flush();
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

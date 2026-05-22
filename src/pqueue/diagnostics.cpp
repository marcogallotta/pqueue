#include "diagnostics.h"

#include "append_log_common.h"
#include "file_system.h"
#include "store_types.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace pqueue {
namespace {

StorageBackend resolveBackend(StorageBackend configured) {
    if (configured != StorageBackend::Default) {
        return configured;
    }
#ifdef ARDUINO
    return StorageBackend::LittleFS;
#else
    return StorageBackend::Posix;
#endif
}

constexpr const char* kDiagManifestSlotA = "manifest-a.bin";
constexpr const char* kDiagManifestSlotB = "manifest-b.bin";
constexpr const char* kDiagSegmentPrefix  = "seg-";
constexpr const char* kDiagSegmentSuffix  = ".bin";

std::shared_ptr<FileSystem> resolveAppendLogFs(const AppendLogConfig& config) {
    if (config.fileSystem) return config.fileSystem;
    switch (resolveBackend(config.backend)) {
    case StorageBackend::Posix:    return makePosixFileSystem();
    case StorageBackend::LittleFS: return makeLittleFsFileSystem();
    case StorageBackend::Default:  break;
    }
    return nullptr;
}

bool parseSegmentGen(const std::string& name, std::uint32_t& genOut) {
    const std::size_t prefixLen = std::string(kDiagSegmentPrefix).size();
    const std::size_t suffixLen = std::string(kDiagSegmentSuffix).size();
    if (name.size() != prefixLen + 8 + suffixLen) return false;
    if (name.compare(0, prefixLen, kDiagSegmentPrefix) != 0) return false;
    if (name.compare(name.size() - suffixLen, suffixLen, kDiagSegmentSuffix) != 0) return false;
    const std::string hex = name.substr(prefixLen, 8);
    for (char c : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    genOut = static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
    return true;
}

bool readManifestSlot(FileSystem& fs, const char* name,
                      append_log_detail::ManifestData& mdOut) {
    std::string data;
    if (!fs.readFile(name, data).ok()) return false;
    return append_log_detail::parseManifest(
        reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), mdOut);
}

} // namespace

AppendLogStoreDiagnostic diagnoseAppendLogStore(const AppendLogConfig& config) {
    AppendLogStoreDiagnostic out;
    out.basePath = config.basePath;

    auto f = resolveAppendLogFs(config);
    if (!f) {
        out.mountStatus = Status::failure(StatusCode::BackendUnavailable,
                                          "file system backend unavailable");
        return out;
    }

    out.mountStatus = f->mount(config.basePath);
    if (!out.mountStatus.ok()) return out;

    out.freeBytes = f->freeBytes();

    append_log_detail::ManifestData mdA, mdB;
    {
        std::uint64_t sz = 0;
        out.slotA.exists = f->fileSize(kDiagManifestSlotA, sz).ok();
        out.slotA.valid  = out.slotA.exists && readManifestSlot(*f, kDiagManifestSlotA, mdA);
        if (out.slotA.valid) out.slotA.epoch = mdA.epoch;

        out.slotB.exists = f->fileSize(kDiagManifestSlotB, sz).ok();
        out.slotB.valid  = out.slotB.exists && readManifestSlot(*f, kDiagManifestSlotB, mdB);
        if (out.slotB.valid) out.slotB.epoch = mdB.epoch;
    }

    append_log_detail::ManifestData winner;
    if (out.slotA.valid || out.slotB.valid) {
        out.hasWinner = true;
        if (out.slotA.valid && out.slotB.valid)
            winner = (mdB.epoch > mdA.epoch) ? mdB : mdA;
        else
            winner = out.slotA.valid ? mdA : mdB;
        out.winnerEpoch    = winner.epoch;
        out.ranges.reserve(winner.ranges.size());
        for (const auto& r : winner.ranges)
            out.ranges.push_back({r.startGen, r.endGen});
        out.tailGeneration = winner.tailGeneration;
        out.nextGeneration = winner.nextGeneration;
    }

    auto isReferenced = [&](std::uint32_t gen) -> bool {
        if (!out.hasWinner) return false;
        if (winner.tailGeneration != 0 && gen == winner.tailGeneration) return true;
        for (const auto& r : winner.ranges)
            if (gen >= r.startGen && gen <= r.endGen) return true;
        return false;
    };

    std::vector<std::string> files;
    out.listStatus = f->listFiles(files);
    for (const auto& name : files) {
        std::uint32_t gen = 0;
        if (!parseSegmentGen(name, gen)) continue;
        AppendLogSegmentDiagnostic seg;
        seg.generation = gen;
        f->fileSize(name, seg.sizeBytes);
        seg.referenced = isReferenced(gen);
        seg.isTail     = out.hasWinner && (gen == winner.tailGeneration);
        out.segments.push_back(seg);
        if (!seg.referenced) ++out.danglingSegments;
    }

    std::sort(out.segments.begin(), out.segments.end(),
        [](const AppendLogSegmentDiagnostic& a, const AppendLogSegmentDiagnostic& b) {
            return a.generation < b.generation;
        });

    return out;
}

} // namespace pqueue

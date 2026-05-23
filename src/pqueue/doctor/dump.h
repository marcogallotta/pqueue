// Off-device file dump protocol for pqueue_doctor.
//
// Protocol per file:
//   FILE_BEGIN name=<name> size=<bytes>
//   <hex-encoded lines, kDumpChunkBytes bytes per line>
//   FILE_END name=<name> crc=<crc32hex>
// On error:
//   FILE_ERROR name=<name> message=<error_code>
//
// dumpAll wraps the sequence with:
//   DUMP_BEGIN
//   ...
//   DUMP_END files=<n> errors=<n>
//
// All message values are machine-safe tokens (no spaces).
// Writer must expose: void write(const char* s)

#pragma once

#include "pqueue/file_system.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pqueue::doctor {

static constexpr std::size_t kDumpChunkBytes = 128;

namespace dump_detail {

inline std::uint32_t crc32Update(std::uint32_t crc, const void* data, std::size_t len) {
    static constexpr auto kTable = []() {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int j = 0; j < 8; ++j)
                c = (c >> 1) ^ (c & 1u ? 0xEDB88320u : 0u);
            t[i] = c;
        }
        return t;
    }();
    const auto* p = static_cast<const std::uint8_t*>(data);
    crc = ~crc;
    while (len--)
        crc = kTable[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

inline void toHex(const std::uint8_t* data, std::size_t len, char* out) {
    static constexpr char kHex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < len; ++i) {
        out[i * 2]     = kHex[data[i] >> 4];
        out[i * 2 + 1] = kHex[data[i] & 0xF];
    }
    out[len * 2] = '\0';
}

} // namespace dump_detail

// Returns true for names that pqueue_doctor is allowed to dump over the wire.
inline bool isValidDumpName(const std::string& name) {
    if (name == "manifest-a.bin" || name == "manifest-b.bin")
        return true;
    // seg-{8 hex digits}.bin
    if (name.size() == 16 &&
        name.rfind("seg-", 0) == 0 &&
        name.compare(12, 4, ".bin") == 0) {
        for (std::size_t i = 4; i < 12; ++i) {
            const char c = name[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return false;
        }
        return true;
    }
    return false;
}

// Dump one named file. Returns true on success, false on error.
template<typename Writer>
bool dumpFile(pqueue::FileSystem& fs, const std::string& name, Writer& w) {
    if (!isValidDumpName(name)) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "FILE_ERROR name=%s message=invalid_name\n",
                      name.c_str());
        w.write(buf);
        return false;
    }
    std::uint64_t size = 0;
    if (const auto st = fs.fileSize(name, size); !st.ok()) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "FILE_ERROR name=%s message=filesize_failed\n",
                      name.c_str());
        w.write(buf);
        return false;
    }

    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "FILE_BEGIN name=%s size=%llu\n",
                      name.c_str(), static_cast<unsigned long long>(size));
        w.write(buf);
    }

    std::uint32_t crc = 0;
    std::uint64_t offset = 0;
    // 2 hex chars per byte + newline + null
    char hexLine[kDumpChunkBytes * 2 + 2];

    while (offset < size) {
        const std::size_t toRead = static_cast<std::size_t>(
            std::min(static_cast<std::uint64_t>(kDumpChunkBytes), size - offset));
        std::string chunk;
        const auto st = fs.readAt(name, offset, toRead, chunk);
        if (!st.ok() || chunk.size() != toRead) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "FILE_ERROR name=%s message=read_failed_at_%llu\n",
                          name.c_str(), static_cast<unsigned long long>(offset));
            w.write(buf);
            return false;
        }
        crc = dump_detail::crc32Update(crc, chunk.data(), chunk.size());
        dump_detail::toHex(reinterpret_cast<const std::uint8_t*>(chunk.data()),
                           chunk.size(), hexLine);
        hexLine[chunk.size() * 2]     = '\n';
        hexLine[chunk.size() * 2 + 1] = '\0';
        w.write(hexLine);
        offset += chunk.size();
    }

    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "FILE_END name=%s crc=%08x\n",
                      name.c_str(), crc);
        w.write(buf);
    }
    return true;
}

// Dump all manifest-*.bin and seg-*.bin files in sorted order,
// framed by DUMP_BEGIN / DUMP_END.
template<typename Writer>
void dumpAll(pqueue::FileSystem& fs, Writer& w) {
    w.write("DUMP_BEGIN\n");

    std::vector<std::string> files;
    if (const auto st = fs.listFiles(files); !st.ok()) {
        w.write("LIST_ERROR message=listfiles_failed\n");
        w.write("DUMP_END files=0 errors=1\n");
        return;
    }

    std::sort(files.begin(), files.end());

    int fileCount = 0;
    int errorCount = 0;
    for (const auto& name : files) {
        if (!isValidDumpName(name))
            continue;
        if (dumpFile(fs, name, w))
            ++fileCount;
        else
            ++errorCount;
    }

    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "DUMP_END files=%d errors=%d\n",
                      fileCount, errorCount);
        w.write(buf);
    }
}

} // namespace pqueue::doctor

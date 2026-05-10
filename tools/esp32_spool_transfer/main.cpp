#ifdef ARDUINO

#include <Arduino.h>
#include <LittleFS.h>

#include <cstdint>
#include <string>

#include "pqueue/file_store.h"
#include "pqueue/internal/lock_owner.h"
#include "pqueue/status.h"

namespace {

constexpr const char* kSpoolBasePath = "/pqueue_spool";
constexpr const char* kSpoolFileName = "pqueue.spool";
constexpr const char* kSpoolTempFileName = "pqueue.spool.tmp";
constexpr const char* kLockFileName = ".pqueue.lock";
constexpr std::size_t kChunkBytes = 256;
constexpr unsigned long kCommandTimeoutMs = 300000; // 5 minutes

void printLine(const std::string& line) {
    Serial.println(line.c_str());
}

std::string makeLockContents(const std::string& bootId) {
    std::string out;
    out += "pqueue-lock-v2\n";
    out += "owner=spool-transfer\n";
    out += "pid=0\n";
    out += "boot_id=";
    out += bootId;
    out += "\n";
    out += "token=spool-transfer-";
    out += std::to_string(millis());
    out += "\n";
    return out;
}

std::string toHex(const std::uint8_t* data, std::size_t len) {
    static const char kHexChars[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out += kHexChars[(data[i] >> 4) & 0xF];
        out += kHexChars[data[i] & 0xF];
    }
    return out;
}

std::string offsetHex(std::uint32_t offset) {
    char buf[9];
    snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned>(offset));
    return std::string(buf);
}

uint8_t fromHexNibble(char c) {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
    return 0xff;
}

pqueue::FileStoreConfig spoolConfig() {
    pqueue::FileStoreConfig config;
    config.basePath = kSpoolBasePath;
    config.backend = pqueue::StorageBackend::LittleFS;
    config.reservedBytes = 128 * 1024;
    config.recordSizeBytes = 512;
    config.journalBytes = 4096;
    return config;
}

std::string spoolPath() {
    return std::string(kSpoolBasePath) + "/" + kSpoolFileName;
}

std::string spoolTempPath() {
    return std::string(kSpoolBasePath) + "/" + kSpoolTempFileName;
}

bool dumpSpool() {
    File f = LittleFS.open(spoolPath().c_str(), "r");
    if (!f) {
        printLine("ERROR:spool_open_failed");
        return false;
    }

    const std::uint32_t totalBytes = f.size();
    f.close();

    printLine("SPOOL_SIZE:" + std::to_string(totalBytes));

    f = LittleFS.open(spoolPath().c_str(), "r");
    if (!f) {
        printLine("ERROR:spool_reopen_failed");
        return false;
    }

    std::uint8_t buf[kChunkBytes];
    std::uint32_t offset = 0;

    while (offset < totalBytes) {
        const std::uint32_t remaining = totalBytes - offset;
        const std::size_t toRead = remaining < kChunkBytes ? static_cast<std::size_t>(remaining) : kChunkBytes;

        const std::size_t bytesRead = f.read(buf, toRead);
        if (bytesRead == 0) {
            f.close();
            printLine("ERROR:spool_read_failed offset=" + std::to_string(offset));
            return false;
        }

        printLine("CHUNK:" + offsetHex(offset) + ":" + toHex(buf, bytesRead));
        offset += static_cast<std::uint32_t>(bytesRead);
    }

    f.close();
    printLine("SPOOL_END");
    return true;
}

// Reads a line from Serial, blocking until newline or timeout.
// Returns false on timeout.
bool readSerialLine(std::string& out, unsigned long timeoutMs) {
    out.clear();
    const unsigned long deadline = millis() + timeoutMs;
    while (millis() < deadline) {
        if (Serial.available()) {
            const char c = static_cast<char>(Serial.read());
            if (c == '\n') {
                // Strip trailing CR if present
                if (!out.empty() && out.back() == '\r') {
                    out.pop_back();
                }
                return true;
            }
            out += c;
        }
    }
    return false;
}

// Receives spool data from host and writes to temp file.
// Protocol: host sends CHUNK lines in the same format as the dump,
// terminated by SPOOL_END. On success, atomically replaces pqueue.spool.
bool receiveSpool(std::uint32_t expectedBytes) {
    LittleFS.remove(spoolTempPath().c_str());

    File f = LittleFS.open(spoolTempPath().c_str(), "w");
    if (!f) {
        printLine("ERROR:upload_open_failed");
        return false;
    }

    std::uint32_t bytesWritten = 0;
    printLine("READY_UPLOAD");

    while (true) {
        std::string line;
        if (!readSerialLine(line, 30000)) {
            f.close();
            LittleFS.remove(spoolTempPath().c_str());
            printLine("ERROR:upload_timeout");
            return false;
        }

        if (line == "SPOOL_END") {
            break;
        }

        // Expect: CHUNK:<offset>:<hexdata>
        if (line.size() < 8 || line.substr(0, 6) != "CHUNK:") {
            f.close();
            LittleFS.remove(spoolTempPath().c_str());
            printLine("ERROR:upload_bad_line " + line.substr(0, 32));
            return false;
        }

        const std::size_t firstColon = 6;
        const std::size_t secondColon = line.find(':', firstColon);
        if (secondColon == std::string::npos) {
            f.close();
            LittleFS.remove(spoolTempPath().c_str());
            printLine("ERROR:upload_bad_chunk_format");
            return false;
        }

        const std::string hexData = line.substr(secondColon + 1);
        if (hexData.size() % 2 != 0) {
            f.close();
            LittleFS.remove(spoolTempPath().c_str());
            printLine("ERROR:upload_odd_hex_length");
            return false;
        }

        // Decode hex and write
        std::uint8_t buf[kChunkBytes];
        const std::size_t chunkBytes = hexData.size() / 2;
        if (chunkBytes > kChunkBytes) {
            f.close();
            LittleFS.remove(spoolTempPath().c_str());
            printLine("ERROR:upload_chunk_too_large");
            return false;
        }

        for (std::size_t i = 0; i < chunkBytes; ++i) {
            const uint8_t hi = fromHexNibble(hexData[i * 2]);
            const uint8_t lo = fromHexNibble(hexData[i * 2 + 1]);
            if (hi == 0xff || lo == 0xff) {
                f.close();
                LittleFS.remove(spoolTempPath().c_str());
                printLine("ERROR:upload_invalid_hex");
                return false;
            }
            buf[i] = static_cast<std::uint8_t>((hi << 4) | lo);
        }

        if (f.write(buf, chunkBytes) != chunkBytes) {
            f.close();
            LittleFS.remove(spoolTempPath().c_str());
            printLine("ERROR:upload_write_failed offset=" + std::to_string(bytesWritten));
            return false;
        }

        bytesWritten += static_cast<std::uint32_t>(chunkBytes);
    }

    f.flush();
    f.close();

    if (bytesWritten != expectedBytes) {
        LittleFS.remove(spoolTempPath().c_str());
        printLine("ERROR:upload_size_mismatch got=" + std::to_string(bytesWritten) +
                  " expected=" + std::to_string(expectedBytes));
        return false;
    }

    // Atomic replace
    LittleFS.remove(spoolPath().c_str());
    if (!LittleFS.rename(spoolTempPath().c_str(), spoolPath().c_str())) {
        printLine("ERROR:upload_rename_failed");
        return false;
    }

    printLine("UPLOAD_OK bytes=" + std::to_string(bytesWritten));
    return true;
}

void runSpoolTransfer() {
    const std::string bootId = pqueue::lock_detail::currentBootId();
    const std::string lockContents = makeLockContents(bootId);

    printLine("spool_transfer: base_path=" + std::string(kSpoolBasePath));
    printLine("spool_transfer: boot_id=" + bootId);

    pqueue::FileStore store(spoolConfig());

    const pqueue::Status mountSt = store.mount();
    if (!mountSt.ok()) {
        printLine(std::string("ERROR:mount_failed ") + (mountSt.message ? mountSt.message : ""));
        return;
    }

    const pqueue::Status lockSt = store.tryAcquireLockFile(kLockFileName, lockContents);
    if (!lockSt.ok()) {
        printLine(std::string("ERROR:lock_failed ") + (lockSt.message ? lockSt.message : ""));
        return;
    }

    printLine("LOCK_ACQUIRED");

    bool dumpOk = dumpSpool();
    if (!dumpOk) {
        store.releaseLockFile(kLockFileName, lockContents);
        printLine("LOCK_RELEASED");
        printLine("spool_transfer: done ok=no");
        return;
    }

    // Wait for host command
    printLine("READY");
    bool transferOk = true;

    std::string cmd;
    if (!readSerialLine(cmd, kCommandTimeoutMs)) {
        printLine("ERROR:command_timeout");
        transferOk = false;
    } else if (cmd == "DONE") {
        // Host just wanted the dump; release and exit
    } else if (cmd.substr(0, 7) == "UPLOAD ") {
        const std::uint32_t expectedBytes = static_cast<std::uint32_t>(std::stoul(cmd.substr(7)));
        transferOk = receiveSpool(expectedBytes);
    } else {
        printLine("ERROR:unknown_command " + cmd.substr(0, 32));
        transferOk = false;
    }

    store.releaseLockFile(kLockFileName, lockContents);
    printLine("LOCK_RELEASED");
    printLine(std::string("spool_transfer: done ok=") + (transferOk ? "yes" : "no"));
}

} // namespace

void setup() {
    Serial.begin(115200);
    delay(1000);

    printLine("spool_transfer: start");

    if (!LittleFS.begin(false)) {
        printLine("ERROR:littlefs_mount_failed");
        return;
    }

    printLine(
        "littlefs: mounted total_bytes=" + std::to_string(LittleFS.totalBytes()) +
        " used_bytes=" + std::to_string(LittleFS.usedBytes())
    );

    runSpoolTransfer();
}

void loop() {
    delay(1000);
}

#endif // ARDUINO

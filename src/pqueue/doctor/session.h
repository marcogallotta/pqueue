#pragma once
#ifdef ARDUINO

#include <Arduino.h>

#include "pqueue/diagnostics.h"
#include "pqueue/file_system.h"
#include "pqueue/queue.h"
#include "pqueue/status.h"
#include "pqueue/doctor/dump.h"

#include <cctype>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pqueue::doctor::session_detail {

struct CurrentTarget {
    std::string name;
    std::string basePath;
    bool selected = false;
    std::optional<std::uint32_t> reservedBytes;
    std::optional<std::uint32_t> recordSizeBytes;
    std::optional<std::uint32_t> maxSegmentBytes;
    std::optional<std::uint32_t> minFreeBytes;
    std::optional<std::uint8_t>  maxSegments;
};

inline pqueue::Config makeQueueConfig(const CurrentTarget& t) {
    pqueue::Config qConfig;
    qConfig.basePath = t.basePath;
    if (t.reservedBytes)   qConfig.reservedBytes   = *t.reservedBytes;
    if (t.recordSizeBytes) qConfig.recordSizeBytes = *t.recordSizeBytes;
    if (t.maxSegmentBytes) qConfig.maxSegmentBytes = *t.maxSegmentBytes;
    if (t.minFreeBytes)    qConfig.minFreeBytes    = *t.minFreeBytes;
    if (t.maxSegments)     qConfig.maxSegments     = *t.maxSegments;
    return qConfig;
}

struct SerialWriter {
    void write(const char* s) { Serial.print(s); }
};

inline std::string readLine(unsigned long timeoutMs = 30000) {
    std::string line;
    unsigned long deadline = millis() + timeoutMs;
    while (millis() < deadline) {
        if (!Serial.available()) { delay(1); continue; }
        const char c = static_cast<char>(Serial.read());
        if (c == '\r') continue;
        if (c == '\n') {
            if (!line.empty()) return line;
            continue;
        }
        line += c;
        deadline = millis() + timeoutMs;
    }
    return line;
}

inline void sprintln(const char* s)        { Serial.println(s); }
inline void sprintln(const std::string& s) { Serial.println(s.c_str()); }

inline void cmdInfo(const CurrentTarget& t) {
    pqueue::AppendLogConfig alConfig;
    alConfig.basePath = t.basePath;
    const auto diag = pqueue::diagnoseAppendLogStore(alConfig);
    Serial.print("fs_free=");
    Serial.println(static_cast<unsigned long>(diag.freeBytes));
    std::vector<std::string> files;
    auto fs = pqueue::makeLittleFsFileSystem();
    fs->mount(t.basePath);
    if (fs->listFiles(files).ok()) {
        for (const auto& name : files) {
            std::uint64_t sz = 0;
            fs->fileSize(name, sz);
            Serial.print("  ");
            Serial.print(name.c_str());
            Serial.print(" size=");
            Serial.println(static_cast<unsigned long>(sz));
        }
    }
    sprintln("RESULT command=INFO ok=1");
}

inline void cmdList(const CurrentTarget& t) {
    pqueue::AppendLogConfig alConfig;
    alConfig.basePath = t.basePath;
    const auto diag = pqueue::diagnoseAppendLogStore(alConfig);
    for (const auto& seg : diag.segments) {
        char buf[80];
        snprintf(buf, sizeof(buf), "seg-%08lx.bin size=%llu %s",
                 static_cast<unsigned long>(seg.generation),
                 static_cast<unsigned long long>(seg.sizeBytes),
                 seg.isTail ? "tail" : (seg.referenced ? "sealed" : "dangling"));
        sprintln(buf);
    }
    Serial.print("manifest_a exists="); Serial.print(diag.slotA.exists ? "yes" : "no");
    Serial.print(" valid=");            Serial.println(diag.slotA.valid ? "yes" : "no");
    Serial.print("manifest_b exists="); Serial.print(diag.slotB.exists ? "yes" : "no");
    Serial.print(" valid=");            Serial.println(diag.slotB.valid ? "yes" : "no");
    sprintln("RESULT command=LIST ok=1");
}

inline void cmdValidate(const CurrentTarget& t) {
    pqueue::Queue q(makeQueueConfig(t));
    const auto val = q.validate();
    Serial.println(val.ok ? "validate: OK" : "validate: FAILED");
    Serial.print("records_checked=");
    Serial.println(static_cast<unsigned long>(val.checkedRecords));
    for (const auto& issue : val.errors) {
        Serial.print("issue code=");
        Serial.print(pqueue::validationIssueCodeName(issue.code));
        Serial.print(" message=");
        Serial.print(issue.message.c_str());
        Serial.print(" repair_hint=");
        switch (issue.repairAction) {
            case pqueue::ValidationRepairAction::Format:             sprintln("format_queue"); break;
            case pqueue::ValidationRepairAction::DropFrontIfCorrupt: sprintln("drop_front_if_corrupt"); break;
            default:                                                  sprintln("none"); break;
        }
    }
    char result[64];
    snprintf(result, sizeof(result), "RESULT command=VALIDATE ok=%d issues=%zu",
             val.ok ? 1 : 0, val.errors.size());
    sprintln(result);
}

inline void cmdDiag(const CurrentTarget& t) {
    pqueue::AppendLogConfig alConfig;
    alConfig.basePath = t.basePath;
    const auto diag = pqueue::diagnoseAppendLogStore(alConfig);
    char buf[128];
    snprintf(buf, sizeof(buf), "mount=%s list=%s free=%llu",
             diag.mountStatus.ok() ? "ok" : pqueue::statusCodeName(diag.mountStatus.code),
             diag.listStatus.ok()  ? "ok" : pqueue::statusCodeName(diag.listStatus.code),
             static_cast<unsigned long long>(diag.freeBytes));
    sprintln(buf);
    Serial.print("slot_a exists="); Serial.print(diag.slotA.exists ? "yes" : "no");
    Serial.print(" valid=");        Serial.print(diag.slotA.valid  ? "yes" : "no");
    if (diag.slotA.valid) { Serial.print(" epoch="); Serial.print(static_cast<unsigned long>(diag.slotA.epoch)); }
    Serial.println();
    Serial.print("slot_b exists="); Serial.print(diag.slotB.exists ? "yes" : "no");
    Serial.print(" valid=");        Serial.print(diag.slotB.valid  ? "yes" : "no");
    if (diag.slotB.valid) { Serial.print(" epoch="); Serial.print(static_cast<unsigned long>(diag.slotB.epoch)); }
    Serial.println();
    if (diag.hasWinner) {
        snprintf(buf, sizeof(buf), "winner_epoch=%lu ranges=%zu tail_gen=%08lx next_gen=%08lx",
                 static_cast<unsigned long>(diag.winnerEpoch), diag.ranges.size(),
                 static_cast<unsigned long>(diag.tailGeneration),
                 static_cast<unsigned long>(diag.nextGeneration));
        sprintln(buf);
        for (std::size_t i = 0; i < diag.ranges.size(); ++i) {
            snprintf(buf, sizeof(buf), "  range[%zu] %08lx..%08lx", i,
                     static_cast<unsigned long>(diag.ranges[i].startGen),
                     static_cast<unsigned long>(diag.ranges[i].endGen));
            sprintln(buf);
        }
    }
    snprintf(buf, sizeof(buf), "segments=%zu dangling=%zu", diag.segments.size(), diag.danglingSegments);
    sprintln(buf);
    const bool diagOk = diag.mountStatus.ok() && diag.listStatus.ok();
    sprintln(diagOk ? "RESULT command=DIAG ok=1" : "RESULT command=DIAG ok=0");
}

inline void cmdDumpFile(const CurrentTarget& t, const std::string& name) {
    auto fs = pqueue::makeLittleFsFileSystem();
    if (const auto st = fs->mount(t.basePath); !st.ok()) {
        Serial.print("FILE_ERROR name="); Serial.print(name.c_str()); Serial.println(" message=mount_failed");
        return;
    }
    SerialWriter w;
    pqueue::doctor::dumpFile(*fs, name, w);
}

inline void cmdDumpAll(const CurrentTarget& t) {
    auto fs = pqueue::makeLittleFsFileSystem();
    if (const auto st = fs->mount(t.basePath); !st.ok()) {
        Serial.println("LIST_ERROR message=mount_failed");
        return;
    }
    SerialWriter w;
    pqueue::doctor::dumpAll(*fs, w);
}

inline void cmdCompact(const CurrentTarget& t, int steps) {
    pqueue::Queue q(makeQueueConfig(t));
    const auto cr = q.compactIdle(static_cast<std::size_t>(steps));
    char buf[128];
    snprintf(buf, sizeof(buf), "compact: status=%s steps_run=%zu compactions=%zu noops=%zu more_work=%s",
             cr.status.ok() ? "ok" : pqueue::statusCodeName(cr.status.code),
             cr.stepsRun, cr.compactions, cr.noOps, cr.moreWorkLikely ? "yes" : "no");
    sprintln(buf);
    snprintf(buf, sizeof(buf), "RESULT command=COMPACT ok=%d steps=%zu compactions=%zu more_work=%d",
             cr.status.ok() ? 1 : 0, cr.stepsRun, cr.compactions, cr.moreWorkLikely ? 1 : 0);
    sprintln(buf);
}

inline void cmdCompactAll(const CurrentTarget& t, int maxSteps) {
    pqueue::Queue q(makeQueueConfig(t));
    int totalSteps = 0, totalCompactions = 0;
    bool ok = true;
    while (totalSteps < maxSteps) {
        const auto cr = q.compactIdle(static_cast<std::size_t>(maxSteps - totalSteps));
        totalSteps       += static_cast<int>(cr.stepsRun);
        totalCompactions += static_cast<int>(cr.compactions);
        if (!cr.status.ok()) { ok = false; break; }
        if (!cr.moreWorkLikely) break;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "compact_all: total_steps=%d compactions=%d", totalSteps, totalCompactions);
    sprintln(buf);
    snprintf(buf, sizeof(buf), "RESULT command=COMPACT_ALL ok=%d steps=%d compactions=%d",
             ok ? 1 : 0, totalSteps, totalCompactions);
    sprintln(buf);
}

inline void cmdDropFrontIfCorrupt(const CurrentTarget& t) {
    pqueue::Queue q(makeQueueConfig(t));
    const auto st = q.dropFrontIfCorrupt();
    char result[96];
    if (st.ok()) {
        sprintln("drop_front: dropped");
        sprintln("RESULT command=DROP_FRONT_IF_CORRUPT ok=1 changed=1");
    } else if (st.code == pqueue::StatusCode::QueueEmpty) {
        sprintln("drop_front: queue_empty");
        sprintln("RESULT command=DROP_FRONT_IF_CORRUPT ok=1 changed=0 code=queue_empty");
    } else if (st.code == pqueue::StatusCode::InvalidArgument) {
        sprintln("drop_front: front_not_corrupt");
        sprintln("RESULT command=DROP_FRONT_IF_CORRUPT ok=1 changed=0 code=front_not_corrupt");
    } else {
        Serial.print("drop_front: error="); Serial.println(pqueue::statusCodeName(st.code));
        snprintf(result, sizeof(result), "RESULT command=DROP_FRONT_IF_CORRUPT ok=0 code=%s",
                 pqueue::statusCodeName(st.code));
        sprintln(result);
    }
}

inline void cmdRecoverStaleLock(const CurrentTarget& t) {
    pqueue::Queue q(makeQueueConfig(t));
    const auto st = q.recoverStaleLock();
    char result[80];
    if (st.ok()) {
        sprintln("recover_lock: ok");
        sprintln("RESULT command=RECOVER_STALE_LOCK ok=1 changed=1");
    } else if (st.code == pqueue::StatusCode::LockTimeout) {
        sprintln("recover_lock: lock_not_stale");
        sprintln("RESULT command=RECOVER_STALE_LOCK ok=1 changed=0 code=lock_not_stale");
    } else {
        Serial.print("recover_lock: error="); Serial.println(pqueue::statusCodeName(st.code));
        snprintf(result, sizeof(result), "RESULT command=RECOVER_STALE_LOCK ok=0 code=%s",
                 pqueue::statusCodeName(st.code));
        sprintln(result);
    }
}

inline void cmdFormat(const CurrentTarget& t) {
    pqueue::Queue q(makeQueueConfig(t));
    const auto st = q.format();
    if (st.ok()) {
        sprintln("format: ok");
        sprintln("RESULT command=FORMAT ok=1");
    } else {
        Serial.print("format: error="); Serial.println(pqueue::statusCodeName(st.code));
        char result[64];
        snprintf(result, sizeof(result), "RESULT command=FORMAT ok=0 code=%s",
                 pqueue::statusCodeName(st.code));
        sprintln(result);
    }
}

inline std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') ++i;
        if (i >= line.size()) break;
        std::size_t start = i;
        while (i < line.size() && line[i] != ' ') ++i;
        tokens.push_back(line.substr(start, i - start));
    }
    return tokens;
}

inline bool dispatch(const std::string& line, CurrentTarget& target) {
    const auto tokens = tokenize(line);
    if (tokens.empty()) return true;
    const std::string& verb = tokens[0];

    if (verb == "TARGET") {
        if (tokens.size() < 3) { sprintln("error: TARGET requires name and path"); return true; }

        // Validate target name: [A-Za-z0-9_-]+
        for (char c : tokens[1]) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
                sprintln("error: TARGET name contains invalid characters (use [A-Za-z0-9_-])");
                return true;
            }
        }
        if (tokens[1].empty()) { sprintln("error: TARGET name must not be empty"); return true; }

        // Validate all key=val tokens before touching target state.
        for (std::size_t i = 3; i < tokens.size(); ++i) {
            const auto eq = tokens[i].find('=');
            if (eq == std::string::npos) {
                Serial.print("error: TARGET config token missing '=': ");
                Serial.println(tokens[i].c_str());
                return true;
            }
            const std::string key = tokens[i].substr(0, eq);
            const char* valStr = tokens[i].c_str() + eq + 1;
            char* end = nullptr;
            const long val = strtol(valStr, &end, 10);
            if (end == valStr || *end != '\0') {
                Serial.print("error: TARGET config value is not a valid integer: ");
                Serial.println(valStr);
                return true;
            }
            if (key == "reservedBytes" || key == "recordSizeBytes" || key == "maxSegmentBytes") {
                if (val <= 0) {
                    Serial.print("error: TARGET config "); Serial.print(key.c_str()); sprintln(" must be > 0");
                    return true;
                }
            } else if (key == "minFreeBytes") {
                if (val < 0) { sprintln("error: TARGET config minFreeBytes must be >= 0"); return true; }
            } else if (key == "maxSegments") {
                if (val < 1 || val > 255) { sprintln("error: TARGET config maxSegments must be 1-255"); return true; }
            } else {
                Serial.print("error: TARGET unknown config key: "); Serial.println(key.c_str());
                return true;
            }
        }
        // All tokens valid -- reset and apply.
        target = CurrentTarget{};
        target.name     = tokens[1];
        target.basePath = tokens[2];
        for (std::size_t i = 3; i < tokens.size(); ++i) {
            const auto eq = tokens[i].find('=');
            const std::string key = tokens[i].substr(0, eq);
            const long val = strtol(tokens[i].c_str() + eq + 1, nullptr, 10);
            if      (key == "reservedBytes")   target.reservedBytes   = static_cast<std::uint32_t>(val);
            else if (key == "recordSizeBytes") target.recordSizeBytes = static_cast<std::uint32_t>(val);
            else if (key == "maxSegmentBytes") target.maxSegmentBytes = static_cast<std::uint32_t>(val);
            else if (key == "minFreeBytes")    target.minFreeBytes    = static_cast<std::uint32_t>(val);
            else if (key == "maxSegments")     target.maxSegments     = static_cast<std::uint8_t>(val);
        }
        target.selected = true;
        Serial.print("target: "); Serial.print(target.name.c_str());
        Serial.print(" path=");   Serial.print(target.basePath.c_str());
        if (target.reservedBytes)   { Serial.print(" reservedBytes=");   Serial.print(*target.reservedBytes); }
        if (target.recordSizeBytes) { Serial.print(" recordSizeBytes="); Serial.print(*target.recordSizeBytes); }
        if (target.maxSegmentBytes) { Serial.print(" maxSegmentBytes="); Serial.print(*target.maxSegmentBytes); }
        if (target.minFreeBytes)    { Serial.print(" minFreeBytes=");    Serial.print(*target.minFreeBytes); }
        if (target.maxSegments)     { Serial.print(" maxSegments=");     Serial.print(static_cast<int>(*target.maxSegments)); }
        Serial.println();
        return true;
    }

    if (verb == "DONE") return false;

    if (!target.selected) { sprintln("error: no target selected (use TARGET name path)"); return true; }

    if (verb == "INFO")     { cmdInfo(target);     return true; }
    if (verb == "LIST")     { cmdList(target);     return true; }
    if (verb == "VALIDATE") { cmdValidate(target); return true; }
    if (verb == "DIAG")     { cmdDiag(target);     return true; }
    if (verb == "DUMP_ALL") { cmdDumpAll(target);  return true; }

    if (verb == "DUMP_FILE") {
        if (tokens.size() < 2) { sprintln("error: DUMP_FILE requires a filename"); return true; }
        cmdDumpFile(target, tokens[1]);
        return true;
    }
    if (verb == "COMPACT") {
        const int steps = tokens.size() >= 2 ? atoi(tokens[1].c_str()) : 1;
        cmdCompact(target, steps > 0 ? steps : 1);
        return true;
    }
    if (verb == "COMPACT_ALL") {
        const int maxSteps = tokens.size() >= 2 ? atoi(tokens[1].c_str()) : 64;
        cmdCompactAll(target, maxSteps > 0 ? maxSteps : 64);
        return true;
    }
    if (verb == "DROP_FRONT_IF_CORRUPT") { cmdDropFrontIfCorrupt(target); return true; }
    if (verb == "RECOVER_STALE_LOCK")    { cmdRecoverStaleLock(target);   return true; }
    if (verb == "FORMAT") {
        if (tokens.size() < 3 || tokens[1] != "CONFIRM") {
            sprintln("error: FORMAT requires CONFIRM and target name: FORMAT CONFIRM <name>");
            return true;
        }
        if (tokens[2] != target.name) {
            Serial.print("error: FORMAT CONFIRM name mismatch: expected '");
            Serial.print(target.name.c_str());
            sprintln("'");
            return true;
        }
        sprintln("warning: dump first recommended (DUMP_ALL before FORMAT)");
        cmdFormat(target);
        return true;
    }

    Serial.print("error: unknown command: "); Serial.println(verb.c_str());
    return true;
}

} // namespace pqueue::doctor::session_detail

namespace pqueue::doctor {

// Blocks until DONE is received. Prints PQUEUE_DOCTOR_START + READY before
// entering the command loop. Caller should ESP.restart() after this returns.
inline void runSession() {
    using namespace session_detail;
    Serial.println("PQUEUE_DOCTOR_START");
    Serial.println("READY");
    CurrentTarget target;
    while (true) {
        const std::string line = readLine();
        if (line.empty()) continue;
        if (!dispatch(line, target)) break;
        Serial.println("READY");
    }
}

} // namespace pqueue::doctor

#endif // ARDUINO

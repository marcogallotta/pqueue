#pragma once
#ifdef ARDUINO

#include <Arduino.h>

#include "pqueue/diagnostics.h"
#include "pqueue/file_system.h"
#include "pqueue/queue.h"
#include "pqueue/status.h"
#include "pqueue_doctor_dump.h"

#include <memory>
#include <string>
#include <vector>

namespace pqueue::doctor::session_detail {

struct CurrentTarget {
    std::string name;
    std::string basePath;
    bool selected = false;
};

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
}

inline void cmdValidate(const CurrentTarget& t) {
    pqueue::Config qConfig;
    qConfig.basePath = t.basePath;
    pqueue::Queue q(qConfig);
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
    pqueue::Config qConfig;
    qConfig.basePath = t.basePath;
    pqueue::Queue q(qConfig);
    const auto cr = q.compactIdle(static_cast<std::size_t>(steps));
    char buf[128];
    snprintf(buf, sizeof(buf), "compact: status=%s steps_run=%zu compactions=%zu noops=%zu more_work=%s",
             cr.status.ok() ? "ok" : pqueue::statusCodeName(cr.status.code),
             cr.stepsRun, cr.compactions, cr.noOps, cr.moreWorkLikely ? "yes" : "no");
    sprintln(buf);
}

inline void cmdCompactAll(const CurrentTarget& t, int maxSteps) {
    pqueue::Config qConfig;
    qConfig.basePath = t.basePath;
    pqueue::Queue q(qConfig);
    int totalSteps = 0, totalCompactions = 0;
    while (totalSteps < maxSteps) {
        const auto cr = q.compactIdle(static_cast<std::size_t>(maxSteps - totalSteps));
        totalSteps       += static_cast<int>(cr.stepsRun);
        totalCompactions += static_cast<int>(cr.compactions);
        if (!cr.status.ok() || !cr.moreWorkLikely) break;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "compact_all: total_steps=%d compactions=%d", totalSteps, totalCompactions);
    sprintln(buf);
}

inline void cmdDropFrontIfCorrupt(const CurrentTarget& t) {
    pqueue::Config qConfig;
    qConfig.basePath = t.basePath;
    pqueue::Queue q(qConfig);
    const auto st = q.dropFrontIfCorrupt();
    if (st.ok())                                       sprintln("drop_front: dropped");
    else if (st.code == pqueue::StatusCode::QueueEmpty) sprintln("drop_front: queue_empty");
    else if (st.code == pqueue::StatusCode::InvalidArgument) sprintln("drop_front: front_not_corrupt");
    else { Serial.print("drop_front: error="); Serial.println(pqueue::statusCodeName(st.code)); }
}

inline void cmdRecoverStaleLock(const CurrentTarget& t) {
    pqueue::Config qConfig;
    qConfig.basePath = t.basePath;
    pqueue::Queue q(qConfig);
    const auto st = q.recoverStaleLock();
    if (st.ok())                                          sprintln("recover_lock: ok");
    else if (st.code == pqueue::StatusCode::LockTimeout)  sprintln("recover_lock: lock_not_stale");
    else { Serial.print("recover_lock: error="); Serial.println(pqueue::statusCodeName(st.code)); }
}

inline void cmdFormat(const CurrentTarget& t) {
    pqueue::Config qConfig;
    qConfig.basePath = t.basePath;
    pqueue::Queue q(qConfig);
    const auto st = q.format();
    if (st.ok()) sprintln("format: ok");
    else { Serial.print("format: error="); Serial.println(pqueue::statusCodeName(st.code)); }
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
        target.name     = tokens[1];
        target.basePath = tokens[2];
        target.selected = true;
        Serial.print("target: "); Serial.print(target.name.c_str());
        Serial.print(" path=");   Serial.println(target.basePath.c_str());
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
        if (tokens.size() < 2 || tokens[1] != "CONFIRM") { sprintln("error: FORMAT requires CONFIRM argument"); return true; }
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

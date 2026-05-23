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

// ---------------------------------------------------------------------------
// Runtime target (set by TARGET name path command)
// ---------------------------------------------------------------------------

namespace {

struct CurrentTarget {
    std::string name;
    std::string basePath;
    bool selected = false;
};

// ---------------------------------------------------------------------------
// Serial writer (satisfies pqueue_doctor_dump.h Writer concept)
// ---------------------------------------------------------------------------

struct SerialWriter {
    void write(const char* s) { Serial.print(s); }
};

// ---------------------------------------------------------------------------
// Serial helpers
// ---------------------------------------------------------------------------

std::string readLine(unsigned long timeoutMs = 30000) {
    std::string line;
    unsigned long deadline = millis() + timeoutMs;
    while (millis() < deadline) {
        if (!Serial.available()) {
            delay(1);
            continue;
        }
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

void println(const char* s) {
    Serial.println(s);
}

void println(const std::string& s) {
    Serial.println(s.c_str());
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------

void cmdInfo(const CurrentTarget& t) {
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

void cmdList(const CurrentTarget& t) {
    pqueue::AppendLogConfig alConfig;
    alConfig.basePath = t.basePath;
    const auto diag = pqueue::diagnoseAppendLogStore(alConfig);

    for (const auto& seg : diag.segments) {
        char buf[80];
        snprintf(buf, sizeof(buf), "seg-%08lx.bin size=%llu %s",
                 static_cast<unsigned long>(seg.generation),
                 static_cast<unsigned long long>(seg.sizeBytes),
                 seg.isTail ? "tail" : (seg.referenced ? "sealed" : "dangling"));
        println(buf);
    }
    Serial.print("manifest_a exists=");
    Serial.print(diag.slotA.exists ? "yes" : "no");
    Serial.print(" valid=");
    Serial.println(diag.slotA.valid ? "yes" : "no");
    Serial.print("manifest_b exists=");
    Serial.print(diag.slotB.exists ? "yes" : "no");
    Serial.print(" valid=");
    Serial.println(diag.slotB.valid ? "yes" : "no");
}

void cmdValidate(const CurrentTarget& t) {
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
            case pqueue::ValidationRepairAction::Format:
                println("format_queue");
                break;
            case pqueue::ValidationRepairAction::DropFrontIfCorrupt:
                println("drop_front_if_corrupt");
                break;
            default:
                println("none");
                break;
        }
    }
}

void cmdDiag(const CurrentTarget& t) {
    pqueue::AppendLogConfig alConfig;
    alConfig.basePath = t.basePath;
    const auto diag = pqueue::diagnoseAppendLogStore(alConfig);

    char buf[128];
    snprintf(buf, sizeof(buf), "mount=%s list=%s free=%llu",
             diag.mountStatus.ok() ? "ok" : pqueue::statusCodeName(diag.mountStatus.code),
             diag.listStatus.ok()  ? "ok" : pqueue::statusCodeName(diag.listStatus.code),
             static_cast<unsigned long long>(diag.freeBytes));
    println(buf);

    Serial.print("slot_a exists=");
    Serial.print(diag.slotA.exists ? "yes" : "no");
    Serial.print(" valid=");
    Serial.print(diag.slotA.valid ? "yes" : "no");
    if (diag.slotA.valid) {
        Serial.print(" epoch=");
        Serial.print(static_cast<unsigned long>(diag.slotA.epoch));
    }
    Serial.println();

    Serial.print("slot_b exists=");
    Serial.print(diag.slotB.exists ? "yes" : "no");
    Serial.print(" valid=");
    Serial.print(diag.slotB.valid ? "yes" : "no");
    if (diag.slotB.valid) {
        Serial.print(" epoch=");
        Serial.print(static_cast<unsigned long>(diag.slotB.epoch));
    }
    Serial.println();

    if (diag.hasWinner) {
        snprintf(buf, sizeof(buf), "winner_epoch=%lu ranges=%zu tail_gen=%08lx next_gen=%08lx",
                 static_cast<unsigned long>(diag.winnerEpoch),
                 diag.ranges.size(),
                 static_cast<unsigned long>(diag.tailGeneration),
                 static_cast<unsigned long>(diag.nextGeneration));
        println(buf);
        for (std::size_t i = 0; i < diag.ranges.size(); ++i) {
            snprintf(buf, sizeof(buf), "  range[%zu] %08lx..%08lx",
                     i,
                     static_cast<unsigned long>(diag.ranges[i].startGen),
                     static_cast<unsigned long>(diag.ranges[i].endGen));
            println(buf);
        }
    }

    snprintf(buf, sizeof(buf), "segments=%zu dangling=%zu",
             diag.segments.size(), diag.danglingSegments);
    println(buf);
}

void cmdDumpFile(const CurrentTarget& t, const std::string& name) {
    auto fs = pqueue::makeLittleFsFileSystem();
    if (const auto st = fs->mount(t.basePath); !st.ok()) {
        Serial.print("FILE_ERROR name=");
        Serial.print(name.c_str());
        Serial.println(" message=mount_failed");
        return;
    }
    SerialWriter w;
    pqueue::doctor::dumpFile(*fs, name, w);
}

void cmdDumpAll(const CurrentTarget& t) {
    auto fs = pqueue::makeLittleFsFileSystem();
    if (const auto st = fs->mount(t.basePath); !st.ok()) {
        Serial.println("LIST_ERROR message=mount_failed");
        return;
    }
    SerialWriter w;
    pqueue::doctor::dumpAll(*fs, w);
}

void cmdCompact(const CurrentTarget& t, int steps) {
    pqueue::Config qConfig;
    qConfig.basePath = t.basePath;
    pqueue::Queue q(qConfig);
    const auto cr = q.compactIdle(static_cast<std::size_t>(steps));
    char buf[128];
    snprintf(buf, sizeof(buf),
             "compact: status=%s steps_run=%zu compactions=%zu noops=%zu more_work=%s",
             cr.status.ok() ? "ok" : pqueue::statusCodeName(cr.status.code),
             cr.stepsRun, cr.compactions, cr.noOps,
             cr.moreWorkLikely ? "yes" : "no");
    println(buf);
}

void cmdCompactAll(const CurrentTarget& t, int maxSteps) {
    pqueue::Config qConfig;
    qConfig.basePath = t.basePath;
    pqueue::Queue q(qConfig);
    int totalSteps = 0;
    int totalCompactions = 0;
    while (totalSteps < maxSteps) {
        const int remaining = maxSteps - totalSteps;
        const auto cr = q.compactIdle(static_cast<std::size_t>(remaining));
        totalSteps       += static_cast<int>(cr.stepsRun);
        totalCompactions += static_cast<int>(cr.compactions);
        if (!cr.status.ok() || !cr.moreWorkLikely) break;
    }
    char buf[128];
    snprintf(buf, sizeof(buf),
             "compact_all: total_steps=%d compactions=%d",
             totalSteps, totalCompactions);
    println(buf);
}

void cmdDropFrontIfCorrupt(const CurrentTarget& t) {
    pqueue::Config qConfig;
    qConfig.basePath = t.basePath;
    pqueue::Queue q(qConfig);
    const auto st = q.dropFrontIfCorrupt();
    if (st.ok()) {
        println("drop_front: dropped");
    } else if (st.code == pqueue::StatusCode::QueueEmpty) {
        println("drop_front: queue_empty");
    } else if (st.code == pqueue::StatusCode::InvalidArgument) {
        println("drop_front: front_not_corrupt");
    } else {
        Serial.print("drop_front: error=");
        Serial.println(pqueue::statusCodeName(st.code));
    }
}

void cmdRecoverStaleLock(const CurrentTarget& t) {
    pqueue::Config qConfig;
    qConfig.basePath = t.basePath;
    pqueue::Queue q(qConfig);
    const auto st = q.recoverStaleLock();
    if (st.ok()) {
        println("recover_lock: ok");
    } else if (st.code == pqueue::StatusCode::LockTimeout) {
        println("recover_lock: lock_not_stale");
    } else {
        Serial.print("recover_lock: error=");
        Serial.println(pqueue::statusCodeName(st.code));
    }
}

void cmdFormat(const CurrentTarget& t) {
    pqueue::Config qConfig;
    qConfig.basePath = t.basePath;
    pqueue::Queue q(qConfig);
    const auto st = q.format();
    if (st.ok()) {
        println("format: ok");
    } else {
        Serial.print("format: error=");
        Serial.println(pqueue::statusCodeName(st.code));
    }
}

// ---------------------------------------------------------------------------
// Command dispatch
// ---------------------------------------------------------------------------

std::vector<std::string> tokenize(const std::string& line) {
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

bool dispatch(const std::string& line, CurrentTarget& target) {
    const auto tokens = tokenize(line);
    if (tokens.empty()) return true;

    const std::string& verb = tokens[0];

    if (verb == "TARGET") {
        if (tokens.size() < 3) { println("error: TARGET requires name and path"); return true; }
        target.name     = tokens[1];
        target.basePath = tokens[2];
        target.selected = true;
        Serial.print("target: ");
        Serial.print(target.name.c_str());
        Serial.print(" path=");
        Serial.println(target.basePath.c_str());
        return true;
    }

    if (verb == "DONE") return false;

    if (!target.selected) {
        println("error: no target selected (use TARGET name path)");
        return true;
    }

    if (verb == "INFO")     { cmdInfo(target);     return true; }
    if (verb == "LIST")     { cmdList(target);     return true; }
    if (verb == "VALIDATE") { cmdValidate(target); return true; }
    if (verb == "DIAG")     { cmdDiag(target);     return true; }
    if (verb == "DUMP_ALL") { cmdDumpAll(target);  return true; }

    if (verb == "DUMP_FILE") {
        if (tokens.size() < 2) { println("error: DUMP_FILE requires a filename"); return true; }
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
        if (tokens.size() < 2 || tokens[1] != "CONFIRM") {
            println("error: FORMAT requires CONFIRM argument");
            return true;
        }
        cmdFormat(target);
        return true;
    }

    Serial.print("error: unknown command: ");
    Serial.println(verb.c_str());
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(500);

    println("PQUEUE_DOCTOR_START");
    println("READY");

    CurrentTarget target;
    while (true) {
        const std::string line = readLine();
        if (line.empty()) continue;
        if (!dispatch(line, target)) break;
        println("READY");
    }

    println("DONE");
    delay(100);
    ESP.restart();
}

void loop() {}

#endif // ARDUINO

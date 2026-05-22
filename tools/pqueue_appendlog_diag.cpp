// AppendLog diagnostic tool.
//
// POSIX: use CLI flags (--base-path, --format).
// Arduino: define PQUEUE_APPENDLOG_DIAG_FORMAT at compile time to enable the
//          format-on-start path; adapt IO to Serial.print in the Arduino sketch.

#ifndef ARDUINO

#include "pqueue/diagnostics.h"
#include "pqueue/append_log_store.h"
#include "pqueue/queue.h"
#include "pqueue/status.h"
#include "pqueue/types.h"

#include <CLI/CLI.hpp>

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

const char* issueCodeName(pqueue::ValidationIssueCode code) {
    switch (code) {
        case pqueue::ValidationIssueCode::InvalidConfig:                return "invalid_config";
        case pqueue::ValidationIssueCode::MetadataCorrupt:             return "metadata_corrupt";
        case pqueue::ValidationIssueCode::JournalCorrupt:              return "journal_corrupt";
        case pqueue::ValidationIssueCode::ConfigMismatch:              return "config_mismatch";
        case pqueue::ValidationIssueCode::SlotCrcMismatch:             return "slot_crc_mismatch";
        case pqueue::ValidationIssueCode::QueueLoadFailed:             return "queue_load_failed";
        case pqueue::ValidationIssueCode::QueueIndexMismatch:          return "queue_index_mismatch";
        case pqueue::ValidationIssueCode::OutboxEnvelopeInvalid:       return "outbox_envelope_invalid";
        case pqueue::ValidationIssueCode::HttpRequestEnvelopeInvalid:  return "http_request_envelope_invalid";
        default:                                                        return "unknown";
    }
}

const char* repairActionName(pqueue::ValidationRepairAction action) {
    switch (action) {
        case pqueue::ValidationRepairAction::None:               return "none";
        case pqueue::ValidationRepairAction::Format:             return "format queue";
        case pqueue::ValidationRepairAction::DropFrontIfCorrupt: return "drop corrupt front record";
        default:                                                  return "unknown";
    }
}

const char* yesNo(bool v) { return v ? "yes" : "no"; }

std::string hex8(std::uint32_t v) {
    std::ostringstream ss;
    ss << std::hex << std::setw(8) << std::setfill('0') << v;
    return ss.str();
}

void printDiagnostic(const pqueue::AppendLogStoreDiagnostic& d) {
    std::cout << "=== AppendLog Diagnostic ===\n"
              << "base path : " << d.basePath << "\n"
              << "free bytes: " << d.freeBytes << "\n\n";

    std::cout << "--- Manifest Slots ---\n";
    auto printSlot = [](const char* label, const pqueue::AppendLogManifestSlotDiagnostic& s) {
        std::cout << label << ": present=" << yesNo(s.exists)
                  << "  valid=" << yesNo(s.valid);
        if (s.valid) std::cout << "  epoch=" << s.epoch;
        std::cout << "\n";
    };
    printSlot("slot A", d.slotA);
    printSlot("slot B", d.slotB);

    if (!d.hasWinner) {
        std::cout << "winner: none\n\n";
    } else {
        const char* winnerLabel =
            (d.slotA.valid && d.slotB.valid)
                ? (d.slotB.epoch > d.slotA.epoch ? "slot B" : "slot A")
                : (d.slotA.valid ? "slot A" : "slot B");
        std::cout << "winner: " << winnerLabel << "  epoch=" << d.winnerEpoch << "\n\n";

        std::cout << "--- Manifest Contents ---\n"
                  << "ranges        : " << d.ranges.size() << "\n";
        for (std::size_t i = 0; i < d.ranges.size(); ++i) {
            const auto& r = d.ranges[i];
            const std::uint64_t span = static_cast<std::uint64_t>(r.endGen) - r.startGen + 1;
            std::cout << "  [" << (i + 1) << "] gen "
                      << hex8(r.startGen) << ".." << hex8(r.endGen)
                      << " (" << span << " segment" << (span == 1 ? "" : "s") << ")\n";
        }
        std::cout << "tail generation: " << hex8(d.tailGeneration)
                  << (d.tailGeneration == 0 ? " (none)" : "") << "\n"
                  << "next generation: " << hex8(d.nextGeneration) << "\n\n";
    }

    if (!d.listStatus.ok()) {
        std::cout << "WARNING: file listing failed: "
                  << pqueue::statusCodeName(d.listStatus.code)
                  << ": " << d.listStatus.message << "\n";
    }
    std::cout << "--- Segments on Disk ---\n"
              << std::left << std::setw(12) << "generation"
              << std::setw(12) << "size"
              << "status\n";
    for (const auto& seg : d.segments) {
        std::string status;
        if (!seg.referenced) {
            status = "dangling";
        } else if (seg.isTail) {
            status = "tail";
        } else {
            status = "sealed";
        }
        std::cout << std::setw(12) << hex8(seg.generation)
                  << std::setw(12) << seg.sizeBytes
                  << status << "\n";
    }
    if (d.segments.empty()) std::cout << "(none)\n";
    std::cout << "dangling: " << d.danglingSegments << "\n";
}

int runValidation(const std::string& basePath) {
    pqueue::Config cfg;
    cfg.basePath     = basePath;
    pqueue::Queue queue(cfg);

    const auto val = queue.validate();
    std::cout << "\n--- Validation ---\n"
              << (val.ok ? "OK" : "FAILED") << "\n"
              << "records checked: " << val.checkedRecords << "\n"
              << "stopped early  : " << yesNo(val.stoppedEarly) << "\n"
              << "errors         : " << val.errors.size() << "\n";

    for (std::size_t i = 0; i < val.errors.size(); ++i) {
        const auto& issue = val.errors[i];
        std::cout << "\nIssue " << (i + 1) << ":\n"
                  << "  code        : " << issueCodeName(issue.code) << "\n"
                  << "  message     : " << issue.message << "\n"
                  << "  repair hint : " << repairActionName(issue.repairAction) << "\n";
    }

    return val.ok ? 0 : 1;
}

int runFormat(const std::string& basePath) {
    pqueue::Config cfg;
    cfg.basePath    = basePath;
    pqueue::Queue queue(cfg);

    const auto st = queue.format();
    if (st.ok()) {
        std::cout << "Queue formatted.\n";
        return 0;
    }
    std::cout << "Format failed: " << pqueue::statusCodeName(st.code)
              << ": " << st.message << "\n";
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    std::string basePath;
    bool doFormat = false;

    CLI::App app{"pqueue AppendLog diagnostic and repair tool"};
    app.add_option("--base-path", basePath, "Spool directory path")->required();
    app.add_flag("--format", doFormat,
        "Format the queue (destructive); skips diagnostic output");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    if (doFormat) return runFormat(basePath);

    pqueue::AppendLogConfig alConfig;
    alConfig.basePath = basePath;
    printDiagnostic(pqueue::diagnoseAppendLogStore(alConfig));
    return runValidation(basePath);
}

#endif // !ARDUINO

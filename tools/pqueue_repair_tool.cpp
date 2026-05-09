#include "pqueue/queue.h"
#include "pqueue/status.h"

#include <CLI/CLI.hpp>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

const char* issueCodeName(pqueue::ValidationIssueCode code) {
    switch (code) {
        case pqueue::ValidationIssueCode::InvalidConfig: return "invalid_config";
        case pqueue::ValidationIssueCode::MetadataMissing: return "metadata_missing";
        case pqueue::ValidationIssueCode::MetadataCorrupt: return "metadata_corrupt";
        case pqueue::ValidationIssueCode::JournalCorrupt: return "journal_corrupt";
        case pqueue::ValidationIssueCode::ConfigMismatch: return "config_mismatch";
        case pqueue::ValidationIssueCode::SpoolMissing: return "spool_missing";
        case pqueue::ValidationIssueCode::SpoolSizeMismatch: return "spool_size_mismatch";
        case pqueue::ValidationIssueCode::InvalidRingState: return "invalid_ring_state";
        case pqueue::ValidationIssueCode::SlotReadFailed: return "slot_read_failed";
        case pqueue::ValidationIssueCode::SlotHeaderInvalid: return "slot_header_invalid";
        case pqueue::ValidationIssueCode::SlotCrcMismatch: return "slot_crc_mismatch";
        case pqueue::ValidationIssueCode::QueueLoadFailed: return "queue_load_failed";
        case pqueue::ValidationIssueCode::QueueIndexMismatch: return "queue_index_mismatch";
        case pqueue::ValidationIssueCode::OutboxEnvelopeInvalid: return "outbox_envelope_invalid";
        case pqueue::ValidationIssueCode::HttpRequestEnvelopeInvalid: return "http_request_envelope_invalid";
    }
    return "unknown";
}

const char* repairActionHuman(pqueue::ValidationRepairAction action) {
    switch (action) {
        case pqueue::ValidationRepairAction::None: return "none";
        case pqueue::ValidationRepairAction::Format: return "format queue";
        case pqueue::ValidationRepairAction::DropFrontIfCorrupt: return "drop corrupt front record";
    }
    return "unknown";
}

const char* yesNo(bool value) {
    return value ? "yes" : "no";
}


int printFormatStatus(const pqueue::Status& status) {
    if (status.ok()) {
        std::cout << "Queue formatted.\n";
        return 0;
    }

    std::cout << "Format failed.\n"
              << "status: " << pqueue::statusCodeName(status.code) << "\n"
              << "message: " << status.message << "\n";
    return 2;
}

int printDropFrontStatus(const pqueue::Status& status) {
    if (status.ok()) {
        std::cout << "Front record was corrupt and was dropped.\n";
        return 0;
    }

    if (status.code == pqueue::StatusCode::QueueEmpty) {
        std::cout << "Queue is empty.\n"
                  << "Nothing to drop.\n";
        return 0;
    }

    if (status.code == pqueue::StatusCode::InvalidArgument
        && std::string(status.message) == "front record is not corrupt") {
        std::cout << "Front record is readable.\n"
                  << "Nothing to drop.\n";
        return 0;
    }

    std::cout << "Could not drop front record.\n"
              << "status: " << pqueue::statusCodeName(status.code) << "\n"
              << "message: " << status.message << "\n";
    return 2;
}

int runValidate(pqueue::Queue& queue) {
    const auto validation = queue.validate();
    std::cout << (validation.ok ? "Validation OK" : "Validation failed") << "\n"
              << "records checked: " << validation.checkedRecords << "\n"
              << "stopped early: " << yesNo(validation.stoppedEarly) << "\n"
              << "errors: " << validation.errors.size() << "\n";

    if (validation.errors.empty()) {
        std::cout << "repair hint: none\n";
        return 0;
    }

    for (std::size_t i = 0; i < validation.errors.size(); ++i) {
        const auto& issue = validation.errors[i];
        std::cout << "\nIssue " << (i + 1) << ":\n"
                  << "  code: " << issueCodeName(issue.code) << "\n"
                  << "  message: " << issue.message << "\n"
                  << "  repair hint: " << repairActionHuman(issue.repairAction) << "\n";
        if (issue.hasSlotIndex) {
            std::cout << "  slot: " << issue.slotIndex << "\n";
        }
        if (issue.hasExpectedSequence) {
            std::cout << "  expected sequence: " << issue.expectedSequence << "\n";
        }
        if (issue.hasActualSequence) {
            std::cout << "  actual sequence: " << issue.actualSequence << "\n";
        }
    }

    return 1;
}

enum class Command {
    None,
    Validate,
    Format,
    DropFrontIfCorrupt,
};

void addConfigOptions(CLI::App& app, pqueue::Config& config) {
    app.add_option("--base-path", config.basePath, "Queue spool directory")
        ->capture_default_str();
    app.add_option("--reserved-bytes", config.reservedBytes, "Reserved spool bytes")
        ->capture_default_str();
    app.add_option("--record-size-bytes", config.recordSizeBytes, "Record slot size in bytes")
        ->capture_default_str();
    app.add_option("--journal-bytes", config.journalBytes, "Journal region size in bytes")
        ->capture_default_str();
    app.add_option("--checkpoint-every-ops", config.checkpointEveryOps, "Checkpoint interval in queue operations")
        ->capture_default_str();
}

} // namespace

int main(int argc, char** argv) {
    pqueue::Config config;
    Command command = Command::None;

    CLI::App app{"pqueue spool validation and repair tool"};
    app.require_subcommand(1);
    addConfigOptions(app, config);

    auto* validate = app.add_subcommand("validate", "Validate queue spool and print repair hints");
    addConfigOptions(*validate, config);
    validate->callback([&command]() { command = Command::Validate; });

    auto* format = app.add_subcommand("format", "Destructively reinitialize the queue spool");
    addConfigOptions(*format, config);
    format->callback([&command]() { command = Command::Format; });

    auto* dropFront = app.add_subcommand("drop-front-if-corrupt", "Drop the front record only if corruption is proven");
    addConfigOptions(*dropFront, config);
    dropFront->callback([&command]() { command = Command::DropFrontIfCorrupt; });

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    pqueue::Queue queue(config);
    switch (command) {
        case Command::Validate:
            return runValidate(queue);
        case Command::Format:
            return printFormatStatus(queue.format());
        case Command::DropFrontIfCorrupt:
            return printDropFrontStatus(queue.dropFrontIfCorrupt());
        case Command::None:
            break;
    }

    std::cerr << "no command selected\n";
    return 2;
}

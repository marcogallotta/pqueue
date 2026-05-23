#include "pqueue/file_system.h"
#include "pqueue/status.h"
#include "pqueue/doctor/dump.h"

#include <CLI/CLI.hpp>

#include <iostream>
#include <string>

namespace {

struct StdoutWriter {
    void write(const char* s) { std::cout << s; }
};

} // namespace

int main(int argc, char** argv) {
    std::string basePath;
    std::string fileName;

    CLI::App app{"pqueue doctor dump -- transfer queue files to stdout in hex protocol"};
    app.add_option("--base-path", basePath, "Queue spool directory")->required();
    app.add_option("--file", fileName,
                   "Dump a single file by name (omit to dump all manifest and segment files)");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    auto fs = pqueue::makePosixFileSystem();
    if (const auto st = fs->mount(basePath); !st.ok()) {
        std::cerr << "mount failed: " << pqueue::statusCodeName(st.code)
                  << ": " << st.message << "\n";
        return 1;
    }

    StdoutWriter writer;
    if (!fileName.empty())
        return pqueue::doctor::dumpFile(*fs, fileName, writer) ? 0 : 1;

    pqueue::doctor::dumpAll(*fs, writer);
    return 0;
}

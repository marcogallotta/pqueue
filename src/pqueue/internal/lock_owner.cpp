#include "lock_owner.h"

#include <cstdint>
#include <cstdlib>
#include <sstream>

#ifdef ARDUINO
#include <Arduino.h>
#if defined(ESP32)
#include <esp_system.h>
#endif
#else
#include <chrono>
#include <unistd.h>
#endif

namespace pqueue {
namespace lock_detail {

std::string lockValue(const std::string& contents, const char* key) {
    const std::string prefix = std::string(key) + "=";
    const auto pos = contents.find(prefix);
    if (pos == std::string::npos) {
        return {};
    }
    const auto start = pos + prefix.size();
    const auto end = contents.find('\n', start);
    return contents.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

long lockPid(const std::string& contents) {
    const std::string value = lockValue(contents, "pid");
    if (value.empty()) {
        return -1;
    }
    char* parsedEnd = nullptr;
    const long pid = std::strtol(value.c_str(), &parsedEnd, 10);
    if (parsedEnd == value.c_str() || *parsedEnd != '\0') {
        return -1;
    }
    return pid;
}

std::string currentBootId() {
    static const std::string id = [] {
        std::ostringstream out;
#ifdef ARDUINO
#if defined(ESP32)
        out << "esp32-" << static_cast<unsigned long>(esp_random());
#else
        static int anchor = 0;
        out << "arduino-" << millis() << "-" << reinterpret_cast<std::uintptr_t>(&anchor);
#endif
#else
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        out << "posix-" << static_cast<long>(::getpid()) << "-" << now;
#endif
        return out.str();
    }();
    return id;
}

bool lockHasDifferentBootId(const std::string& existingContents, const std::string& currentContents) {
    const std::string existingBoot = lockValue(existingContents, "boot_id");
    const std::string currentBoot = lockValue(currentContents, "boot_id");
    return !existingBoot.empty() && !currentBoot.empty() && existingBoot != currentBoot;
}

} // namespace lock_detail
} // namespace pqueue

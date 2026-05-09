#pragma once

#include <string>

namespace pqueue {
namespace lock_detail {

std::string lockValue(const std::string& contents, const char* key);
long lockPid(const std::string& contents);
std::string currentBootId();
bool lockHasDifferentBootId(const std::string& existingContents, const std::string& currentContents);

} // namespace lock_detail
} // namespace pqueue

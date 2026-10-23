#pragma once

#include <string>

namespace yawa {

// Портативность: всё рядом с exe, включая ini и лог. Профиль пользователя и реестр
// не используются, поэтому папку с программой можно носить на флешке.
std::string executableDirectory();
std::string settingsPath();
std::string logPath();
std::string sessionsDirectory();

bool readTextFile(const std::string& path, std::string& content);
bool writeTextFile(const std::string& path, const std::string& content);
void ensureDirectory(const std::string& path);
void revealInExplorer(const std::string& path);

} // namespace yawa

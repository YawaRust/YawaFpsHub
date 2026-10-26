#pragma once

#include <string>

namespace yawa {

// Приложение — один exe: оно не устанавливается и не требует рядом ни библиотек,
// ни конфигурации. Пишем при этом в каталог пользователя (%LOCALAPPDATA%\YawaFpsHub),
// чтобы exe можно было запускать из Program Files, с сетевого диска или из «Загрузок».
// Если профиль недоступен, пути возвращают каталог рядом с exe — программа не падает.
// Реестр не используется ни для настроек, ни для автозапуска.
std::string executableDirectory();
std::string settingsPath();
std::string logPath();
std::string sessionsDirectory();

bool readTextFile(const std::string& path, std::string& content);
bool writeTextFile(const std::string& path, const std::string& content);
void ensureDirectory(const std::string& path);
void revealInExplorer(const std::string& path);

} // namespace yawa

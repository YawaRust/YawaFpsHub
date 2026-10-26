#pragma once

#include <string_view>

namespace yawa {

// Обычный запуск при необходимости сам запрашивает UAC и устанавливает встроенный пакет.
// Никаких дополнительных файлов для скачивания и команд пользователя не требуется.
bool prepareSensorsForLaunch(std::wstring_view arguments);

// -1 означает, что аргументы не относятся к служебным командам.
int runSensorCommandLine();

} // namespace yawa

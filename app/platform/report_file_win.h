#pragma once

#include <string>
#include <string_view>

namespace yawa {

// Общее чтение локального пакета для CLI и окна просмотра. Отказ для UNC;
// после чтения байты отдельно проверяет парсер формата.
bool readLocalReport(std::wstring_view path, std::string& archive, std::string& error);

} // namespace yawa

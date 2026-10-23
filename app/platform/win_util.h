#pragma once

#include <string>
#include <string_view>

namespace yawa {

std::string narrow(std::wstring_view text);
std::wstring widen(std::string_view text);

// Текст ошибки Win32 вида «Отказано в доступе (5)»: в логе нужен и код, и расшифровка.
std::string win32ErrorText(unsigned long errorCode);
std::string hresultText(long result);

} // namespace yawa

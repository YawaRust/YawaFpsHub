#pragma once

#include <string>
#include <string_view>

namespace yawa {

struct RtssExchange
{
    std::string layout;
    int resources = 0;
};

// Читает контейнер обмена в памяти. Вложенные файлы не извлекаются на диск.
bool unpackRtssExchange(std::string_view bytes, RtssExchange& exchange, std::string& error);

} // namespace yawa

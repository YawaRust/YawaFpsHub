#pragma once

#include <string>

namespace yawa {

// Текущий режим основного монитора: разрешение и герцовка. Приложение определяет их
// само, чтобы подписи и цель кадра совпадали с реальным экраном, а не с константой.
struct DisplayMode {
    int width = 0;
    int height = 0;
    int refreshHz = 0;
    std::wstring name;
};

DisplayMode currentDisplayMode();

} // namespace yawa

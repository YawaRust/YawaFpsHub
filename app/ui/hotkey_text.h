#pragma once

#include <windows.h>

#include <string>

namespace yawa {

// Имя виртуального кода клавиши (VK_*): используется и в настройках, и на экране оверлея,
// чтобы подпись совпадала с реально назначенной клавишей.
inline std::wstring hotkeyName(unsigned int key)
{
    if (key == 0) {
        return L"выключено";
    }
    if (key >= VK_F1 && key <= VK_F24) {
        return L"F" + std::to_wstring(key - VK_F1 + 1);
    }
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) {
        return std::wstring(1, static_cast<wchar_t>(key));
    }

    switch (key) {
    case VK_SPACE:
        return L"Пробел";
    case VK_INSERT:
        return L"Insert";
    case VK_DELETE:
        return L"Delete";
    case VK_HOME:
        return L"Home";
    case VK_END:
        return L"End";
    case VK_PRIOR:
        return L"Page Up";
    case VK_NEXT:
        return L"Page Down";
    case VK_SNAPSHOT:
        return L"Print Screen";
    case VK_PAUSE:
        return L"Pause";
    case VK_SCROLL:
        return L"Scroll Lock";
    default:
        break;
    }

    wchar_t buffer[16]{};
    swprintf_s(buffer, L"код %02X", key);
    return buffer;
}

} // namespace yawa

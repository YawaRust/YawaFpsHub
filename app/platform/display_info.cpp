#include "platform/display_info.h"

#include <windows.h>

namespace yawa {

DisplayMode currentDisplayMode()
{
    DisplayMode mode;

    DEVMODEW settings{};
    settings.dmSize = sizeof(settings);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &settings) == FALSE) {
        return mode;
    }

    mode.width = static_cast<int>(settings.dmPelsWidth);
    mode.height = static_cast<int>(settings.dmPelsHeight);
    mode.refreshHz = static_cast<int>(settings.dmDisplayFrequency);
    // 1 — «аппаратный режим по умолчанию», а не настоящая герцовка: тогда оставляем ноль.
    if (mode.refreshHz <= 1) {
        mode.refreshHz = 0;
    }

    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    if (EnumDisplayDevicesW(nullptr, 0, &device, 0) != FALSE) {
        mode.name = device.DeviceString;
    }
    return mode;
}

} // namespace yawa

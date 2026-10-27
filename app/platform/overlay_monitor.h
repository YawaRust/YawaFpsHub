#pragma once

#include "platform/win_util.h"
#include "ui/settings.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <string_view>

namespace yawa {

inline RECT overlayVirtualScreen()
{
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    return RECT { x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN), y + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
}

// Interface path survives most display rearrangements better than \\.\DISPLAYn.
// Some virtual displays don't expose it; then the display name is a best-effort fallback.
inline std::string monitorIdentity(const wchar_t* display)
{
    DISPLAY_DEVICEW device { sizeof(device) };
    if (EnumDisplayDevicesW(display, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME)
        && device.DeviceID[0] != L'\0') {
        return "interface:" + narrow(device.DeviceID);
    }
    return "display:" + narrow(display);
}

inline void rememberOverlayMonitor(OverlaySettings& overlay, const RECT& virtualScreen)
{
    if (overlay.anchorMode != OverlayAnchorMode::Free) {
        return;
    }
    const POINT point { virtualScreen.left + overlay.originX, virtualScreen.top + overlay.originY };
    const HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONULL);
    MONITORINFOEXW info {};
    info.cbSize = sizeof(info);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info))) {
        overlay.savedMonitorId.clear();
        return;
    }
    overlay.savedMonitorId = monitorIdentity(info.szDevice);
    if (overlay.savedMonitorId.size() > 256) {
        overlay.savedMonitorId.clear();
        return;
    }
    overlay.monitorOriginX = std::max(0L, point.x - info.rcMonitor.left);
    overlay.monitorOriginY = std::max(0L, point.y - info.rcMonitor.top);
}

// Не изменяет настройки: исчезнувший дисплей не должен перепривязаться к соседнему.
inline bool findOverlayMonitor(std::string_view id, RECT& result)
{
    if (id.empty()) {
        return false;
    }
    struct Search
    {
        std::string_view id;
        RECT rect {};
        bool found = false;
    } search { id };
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
            auto& state = *reinterpret_cast<Search*>(data);
            MONITORINFOEXW info {};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info))
                && monitorIdentity(info.szDevice) == state.id) {
                state.rect = info.rcMonitor;
                state.found = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    if (search.found) {
        result = search.rect;
    }
    return search.found;
}

} // namespace yawa

#pragma once

#include "metrics/metric_store.h"
#include "ui/settings.h"

#include <windows.h>

#include <functional>

namespace yawa {

// Отдельное окно: координаты подписей и значений в пикселях холста 520 px.
class OverlayEditor
{
public:
    ~OverlayEditor();
    bool open(HWND owner, HINSTANCE instance, OverlaySettings& settings, const MetricStore& store,
        std::function<void(bool)> onChange);
    void close();
    void refresh();

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    void paint();
    void move(int x, int y);
    HWND m_window = nullptr;
    OverlaySettings* m_settings = nullptr;
    const MetricStore* m_store = nullptr;
    std::function<void(bool)> m_onChange;
    MetricId m_selected = MetricId::Fps;
    bool m_value = false;
    bool m_dragging = false;
    int m_scroll = 0;
    int m_grabX = 0;
    int m_grabY = 0;
};

} // namespace yawa

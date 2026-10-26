#pragma once

#include "ui/app_model.h"

#include <windows.h>

#include <functional>
#include <vector>

namespace yawa {

// Живой редактор: предпросмотр, холст строк и настройки используют ту же модель,
// что и настоящий оверлей. Собственный Renderer не зависит от главного окна.
class OverlayEditor
{
public:
    ~OverlayEditor();
    bool open(HWND owner, HINSTANCE instance, AppModel& model, const Theme& theme,
        std::function<void(const UiHit&, float, bool)> onAction, std::function<void(wchar_t)> onText,
        std::function<void(bool)> onChange);
    void close();
    void refresh();

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    void paint();
    void layout();
    void syncFields();
    void select(MetricId id, bool value);
    void move(int x, int y);
    void preparePixelLayout();
    OverlayRowPosition canvasPosition(MetricId id) const;
    void editCoordinate(HWND field);
    void changed(bool finished);
    const UiHit* hitAt(float x, float y) const;
    std::vector<MetricId> catalogRows() const;

    HWND m_window = nullptr;
    HWND m_labelEdit = nullptr;
    HWND m_labelX = nullptr;
    HWND m_valueX = nullptr;
    HWND m_y = nullptr;
    HWND m_catalogSearch = nullptr;
    Renderer m_renderer;
    AppModel* m_model = nullptr;
    const Theme* m_theme = nullptr;
    std::function<void(const UiHit&, float, bool)> m_onAction;
    std::function<void(wchar_t)> m_onText;
    std::function<void(bool)> m_onChange;
    std::vector<UiHit> m_hits;
    Rect m_canvas;
    Rect m_inspector;
    Rect m_controls;
    Rect m_catalog;
    MetricId m_selected = MetricId::Fps;
    UiHit m_activeHit;
    bool m_value = false;
    bool m_dragging = false;
    bool m_syncing = false;
    int m_canvasScroll = 0;
    int m_panelScroll = 0;
    int m_catalogScroll = 0;
    bool m_catalogOpen = false;
    int m_grabX = 0;
    int m_grabY = 0;
    int m_dragStartX = 0;
    int m_dragStartY = 0;
    int m_offsetStartX = 0;
    int m_offsetStartY = 0;
    bool m_dragLine = false;
    std::vector<MetricId> m_selectedRows;
    std::vector<OverlayRowPosition> m_dragPositions;
};

} // namespace yawa

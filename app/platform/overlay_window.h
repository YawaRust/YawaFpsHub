#pragma once

#include "metrics/metric_store.h"
#include "metrics/statistics.h"
#include "render/theme.h"
#include "ui/settings.h"

#include <windows.h>
#include <d2d1.h>

#include <string>

namespace yawa {

// Оверлей поверх игры: отдельное topmost-окно с попиксельной прозрачностью.
// Оно не касается кадров игры и не инжектится в процесс, поэтому не влияет на FPS
// и не конфликтует с античитами.
class OverlayWindow {
public:
    OverlayWindow() = default;
    ~OverlayWindow();

    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    bool create(HINSTANCE instance, std::string& error);
    void destroy();

    void setSettings(const OverlaySettings& settings);
    void setContent(const MetricStore& store, const FpsSummary& fps, const std::wstring& gpuName);

    void show();
    void hide();
    void toggle();
    bool isVisible() const { return m_visible; }

    void repaint();

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    bool ensureRenderTarget(UINT width, UINT height);
    void render();
    void drawPlates(float scale);
    void drawLine(float scale);
    void drawBackground(const D2D1_RECT_F& frame, float scale);
    void drawRow(float y, float scale, const wchar_t* key, const std::wstring& value, const std::wstring& unit,
        bool emphasize);

    HWND m_window = nullptr;
    ID2D1DCRenderTarget* m_target = nullptr;
    ID2D1SolidColorBrush* m_brush = nullptr;
    IDWriteFactory* m_dwrite = nullptr;
    IDWriteTextFormat* m_keyFormat = nullptr;
    IDWriteTextFormat* m_valueFormat = nullptr;
    IDWriteTextFormat* m_smallFormat = nullptr;

    HDC m_memoryDc = nullptr;
    HBITMAP m_bitmap = nullptr;
    void* m_pixels = nullptr;
    UINT m_bitmapWidth = 0;
    UINT m_bitmapHeight = 0;

    OverlaySettings m_settings;
    Theme m_theme = makeTheme(Accent::Mint);

    MetricStore m_store;
    FpsSummary m_fps;
    std::wstring m_gpuName;
    bool m_visible = false;
    bool m_hasContent = false;
};

} // namespace yawa

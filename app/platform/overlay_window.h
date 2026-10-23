#pragma once

#include "metrics/metric_store.h"
#include "metrics/statistics.h"
#include "render/overlay_painter.h"
#include "render/theme.h"
#include "ui/settings.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <functional>
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
    void setAccent(Accent accent);
    void setContent(const MetricStore& store, const FpsSummary& fps, const std::wstring& gpuName);

    // show возвращает false и заполняет error, если поверхность не создалась:
    // окно тогда не должно считаться показанным.
    bool show(std::string* error = nullptr);
    void hide();
    void toggle(std::string* error = nullptr);
    bool isVisible() const { return m_visible; }

    void repaint();

    // Перетаскивание прямо по оверлею: Ctrl + левая кнопка мыши. Приложение получает
    // новые смещения и сохраняет их в ini (finished == true — отпустили кнопку).
    void setMoveHandler(std::function<void(int offsetX, int offsetY, bool finished)> handler);

    // Рисует текущее содержимое заново и сохраняет его в PNG. Нужно для скриншотов
    // в CI: layered-окно обычным PrintWindow не снимается.
    bool capturePng(const std::wstring& path, std::string& error);

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    bool ensureRenderTarget(UINT width, UINT height);
    bool render();
    // Монитор под курсором: оверлей должен появляться там, где игра, а не только на основном.
    RECT monitorRect() const;
    void drawScene(float scale);
    OverlayValues currentValues() const;
    void drawPlates(float scale);
    void drawLine(float scale);
    void drawBackground(const D2D1_RECT_F& frame, float scale);
    void drawRow(float y, float scale, const std::wstring& key, const std::wstring& value,
        const std::wstring& unit, bool emphasize);
    void beginDrag(int screenX, int screenY);
    void updateDrag(int screenX, int screenY);
    void endDrag();
    void startAnimation(const OverlaySettings& from);
    void tickAnimation();
    OverlaySettings animatedSettings() const;

    HWND m_window = nullptr;
    ID2D1DCRenderTarget* m_target = nullptr;
    ID2D1SolidColorBrush* m_brush = nullptr;
    IDWriteFactory* m_dwrite = nullptr;
    IDWriteTextFormat* m_keyFormat = nullptr;
    IDWriteTextFormat* m_valueFormat = nullptr;
    IDWriteTextFormat* m_smallFormat = nullptr;
    IDWriteTextFormat* m_monoFormat = nullptr;

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
    D2D1_RECT_F m_contentRectD2D{};
    bool m_visible = false;
    bool m_hasContent = false;
    RECT m_monitorRect{};

    // Перетаскивание и плавные переходы.
    std::function<void(int, int, bool)> m_onMove;
    bool m_dragging = false;
    POINT m_dragOrigin{};
    int m_dragOffsetX = 0;
    int m_dragOffsetY = 0;
    bool m_animating = false;
    float m_animProgress = 1.0f;
    // Появление содержимого: при смене стиля оверлей проявляется, а не «щёлкает».
    float m_fade = 1.0f;
    // Значения, между которыми идёт переход.
    float m_animFromScale = 1.0f;
    float m_animToScale = 1.0f;
    float m_animFromOpacity = 0.72f;
    float m_animToOpacity = 0.72f;
    float m_animFromRadius = -1.0f;
    float m_animToRadius = -1.0f;
};

} // namespace yawa

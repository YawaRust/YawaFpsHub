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
#include <optional>
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
    void setMenuSceneOpacity(std::optional<int> opacity);
    void setGameMonitor(const RECT* monitor);
    void setAccent(Accent accent);
    void setContent(const MetricStore& store, const FpsSummary& fps, const std::wstring& gpuName);

    // show возвращает false и заполняет error, если поверхность не создалась:
    // окно тогда не должно считаться показанным.
    bool show(std::string* error = nullptr);
    void hide();
    void toggle(std::string* error = nullptr);
    bool isVisible() const { return m_visible; }
    bool hasVisiblePixels() const;
    bool isDragging() const
    {
        return m_dragging || m_resizing || m_paddingDragging || m_layerDragging || m_rowDragging;
    }

    void repaint();

    // Перетаскивание прямо по оверлею: Ctrl + левая кнопка мыши. Координаты —
    // origin_x/y в свободном режиме, offset_x/y в угловом (finished — отпускание).
    void setMoveHandler(std::function<void(int offsetX, int offsetY, bool finished)> handler);
    void setDirectEditHandler(std::function<void(int scale, int padding, int rowGap, bool finished)> handler);
    void setLayerEditHandler(std::function<void(std::size_t index, int x, int y, bool finished)> handler);
    void setRowEditHandler(
        std::function<void(MetricId id, OverlayRowPosition position, bool finished)> handler);
    void setRowReorderHandler(std::function<void(MetricId moved, MetricId before, bool finished)> handler);
    void setStyledRowEditHandler(
        std::function<void(MetricId id, OverlayRowOffset offset, bool finished)> handler);
    // Окно приложения: правый клик по оверлею показывает его, чтобы не искать в трее.
    void setOwnerWindow(HWND owner) { m_owner = owner; }

    // Рисует текущее содержимое заново и сохраняет его в PNG. Нужно для скриншотов
    // в CI: layered-окно обычным PrintWindow не снимается.
    bool capturePng(const std::wstring& path, std::string& error);

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    bool ensureRenderTarget(UINT width, UINT height);
    bool render();
    // Весь рабочий стол целиком (все мониторы): так оверлей можно перетащить
    // на любой экран, а не только в пределах того, где он появился.
    RECT screenRect() const;
    void drawScene(float scale);
    void drawPlates(float scale);
    void drawLine(float scale);
    void drawBackground(const D2D1_RECT_F& frame, float scale);
    void drawRow(float y, float scale, const std::wstring& key, const std::wstring& value,
        const std::wstring& unit, bool emphasize);
    void beginDrag(int screenX, int screenY);
    void updateDrag(int screenX, int screenY);
    void endDrag();
    bool onResizeCorner(int x, int y) const;
    bool onPaddingGrip(int x, int y) const;
    void updateResize(int x, int y);
    void finishResize();
    void updatePadding(int x, int y);
    void finishPadding();
    int layerAt(int x, int y) const;
    void updateLayerDrag(int x, int y);
    void finishLayerDrag();
    std::optional<MetricId> rowAt(int x, int y) const;
    void updateRowDrag(int x, int y);
    void finishRowDrag();
    void startAnimation(const OverlaySettings& from, bool fadeContent);
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
    std::optional<int> m_menuSceneOpacity;
    Theme m_theme = makeTheme(Accent::Mint);

    MetricStore m_store;
    FpsSummary m_fps;
    std::wstring m_gpuName;
    D2D1_RECT_F m_contentRectD2D{};
    bool m_visible = false;
    bool m_hasContent = false;
    RECT m_monitorRect{};
    std::optional<RECT> m_gameMonitor;
    HWND m_owner = nullptr;

    // Перетаскивание и плавные переходы.
    std::function<void(int, int, bool)> m_onMove;
    std::function<void(int, int, int, bool)> m_onDirectEdit;
    std::function<void(std::size_t, int, int, bool)> m_onLayerEdit;
    std::vector<Rect> m_rtssLayerBounds;
    float m_rtssLayerScale = 1.0f;
    bool m_layerDragging = false;
    std::size_t m_layerIndex = 0;
    POINT m_layerDragOrigin{};
    int m_layerStartX = 0;
    int m_layerStartY = 0;
    float m_layerDragScale = 1.0f;
    std::function<void(MetricId, OverlayRowPosition, bool)> m_onRowEdit;
    std::function<void(MetricId, MetricId, bool)> m_onRowReorder;
    std::function<void(MetricId, OverlayRowOffset, bool)> m_onStyledRowEdit;
    std::vector<OverlayRowBounds> m_rowBounds;
    float m_rowUnit = 1.0f;
    bool m_rowDragging = false;
    bool m_rowReordering = false;
    MetricId m_rowDragId = MetricId::Fps;
    POINT m_rowDragOrigin { };
    OverlayRowPosition m_rowStart { };
    OverlayRowOffset m_styleRowStart { };
    Rect m_rowStartBounds { };
    float m_rowDragScale = 1.0f;
    bool m_resizing = false;
    POINT m_resizeOrigin{};
    int m_resizeScale = 100;
    float m_resizeWidth = 1.0f;
    float m_resizeHeight = 1.0f;
    bool m_paddingDragging = false;
    POINT m_paddingOrigin { };
    int m_paddingStart = 10;
    bool m_showEditHandles = false;
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

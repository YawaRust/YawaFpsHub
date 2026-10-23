#pragma once

#include "render/theme.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <string>
#include <string_view>
#include <vector>

namespace yawa {

struct Rect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    float width() const { return right - left; }
    float height() const { return bottom - top; }
    float centerX() const { return (left + right) * 0.5f; }
    float centerY() const { return (top + bottom) * 0.5f; }
    bool contains(float x, float y) const { return x >= left && x <= right && y >= top && y <= bottom; }
    Rect inset(float amount) const { return Rect{ left + amount, top + amount, right - amount, bottom - amount }; }
};

enum class TextRole { Display, Value, Heading, Body, Small, Tiny, Mono };
enum class TextAlign { Left, Center, Right };

struct ChartSeries {
    const float* values = nullptr;
    std::size_t count = 0;
    Color color;
    bool filled = false;
};

// Вся графика приложения — Direct2D и DirectWrite: без GDI-шрифтов, без картинок,
// поэтому exe остаётся маленьким, а текст чётким при любом DPI.
class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool initialize(std::string& error);
    void shutdown();

    bool begin(HWND window);
    bool end();
    void resize(UINT width, UINT height, float dpiScale);
    bool isReady() const { return m_target != nullptr; }

    float width() const { return m_width; }
    float height() const { return m_height; }
    float scale() const { return m_dpiScale; }
    float dip(UINT pixels) const { return static_cast<float>(pixels) / m_dpiScale; }

    void clear(const Color& color);
    void fillRect(const Rect& rect, const Color& color);
    void fillRoundRect(const Rect& rect, float radius, const Color& color);
    void strokeRoundRect(const Rect& rect, float radius, const Color& color, float thickness = 1.0f);
    void fillCircle(float centerX, float centerY, float radius, const Color& color);
    void drawLine(float x1, float y1, float x2, float y2, const Color& color, float thickness = 1.0f);
    void drawDashedLine(float x1, float y1, float x2, float y2, const Color& color, float thickness);

    // Дуга для кольцевых индикаторов: startAngle в радианах, против часовой стрелки от «12 часов».
    void drawArc(float centerX, float centerY, float radius, float thickness, float sweepFraction,
        const Color& color);

    void text(std::wstring_view value, const Rect& rect, TextRole role, const Color& color,
        TextAlign align = TextAlign::Left);
    float measureText(std::wstring_view value, TextRole role);

    void drawSeries(const ChartSeries& series, const Rect& rect, float minimum, float maximum, float thickness);

    ID2D1Factory* factory() const { return m_d2d; }

private:
    bool createDeviceResources(std::string& error);
    void createTextFormats();
    void releaseDeviceResources();
    IDWriteTextFormat* formatFor(TextRole role) const;
    ID2D1SolidColorBrush* brushFor(const Color& color);

    ID2D1Factory* m_d2d = nullptr;
    IDWriteFactory* m_dwrite = nullptr;
    ID2D1HwndRenderTarget* m_target = nullptr;
    ID2D1SolidColorBrush* m_brush = nullptr;

    IDWriteTextFormat* m_display = nullptr;
    IDWriteTextFormat* m_value = nullptr;
    IDWriteTextFormat* m_heading = nullptr;
    IDWriteTextFormat* m_body = nullptr;
    IDWriteTextFormat* m_small = nullptr;
    IDWriteTextFormat* m_tiny = nullptr;
    IDWriteTextFormat* m_mono = nullptr;

    HWND m_window = nullptr;
    float m_width = 0.0f;
    float m_height = 0.0f;
    float m_dpiScale = 1.0f;
    bool m_drawing = false;
    std::vector<ID2D1SolidColorBrush*> m_brushPool;
};

} // namespace yawa

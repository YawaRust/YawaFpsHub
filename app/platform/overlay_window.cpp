#include "platform/overlay_window.h"

#include <d2d1helper.h>
#include <dwrite.h>

#include <algorithm>
#include <cmath>

namespace yawa {
namespace {

constexpr wchar_t kClassName[] = L"YawaFpsHubOverlay";
constexpr float kMargin = 18.0f;
constexpr float kPlateHeight = 78.0f;

D2D1_COLOR_F colorOf(const Color& color)
{
    return D2D1::ColorF(color.r, color.g, color.b, color.a);
}

int alignFor(OverlayPosition position)
{
    switch (position) {
    case OverlayPosition::TopLeft:
    case OverlayPosition::BottomLeft:
        return 0;
    case OverlayPosition::TopRight:
    case OverlayPosition::BottomRight:
        return 1;
    }
    return 0;
}

bool isBottom(OverlayPosition position)
{
    return position == OverlayPosition::BottomLeft || position == OverlayPosition::BottomRight;
}

IDWriteTextFormat* createFormat(IDWriteFactory* factory, float size, DWRITE_FONT_WEIGHT weight,
    DWRITE_TEXT_ALIGNMENT alignment, const wchar_t* family)
{
    IDWriteTextFormat* format = nullptr;
    if (FAILED(factory->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            size, L"ru-RU", &format))
        || format == nullptr) {
        return nullptr;
    }
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetTextAlignment(alignment);
    return format;
}

} // namespace

OverlayWindow::~OverlayWindow()
{
    destroy();
}

LRESULT CALLBACK OverlayWindow::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool OverlayWindow::create(HINSTANCE instance, std::string& error)
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &OverlayWindow::windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kClassName;
    RegisterClassExW(&windowClass);

    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    m_window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW
            | WS_EX_NOACTIVATE,
        kClassName, L"YawaFpsHub overlay", WS_POPUP, 0, 0, screenWidth, screenHeight, nullptr, nullptr, instance,
        nullptr);

    if (m_window == nullptr) {
        error = "не удалось создать окно оверлея";
        return false;
    }

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&m_dwrite)))) {
        error = "DirectWrite не инициализировался для оверлея";
        return false;
    }

    m_keyFormat = createFormat(m_dwrite, 10.0f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,
        L"Segoe UI");
    m_valueFormat = createFormat(m_dwrite, 17.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING,
        L"Segoe UI");
    m_smallFormat = createFormat(m_dwrite, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING,
        L"Segoe UI");

    return true;
}

void OverlayWindow::destroy()
{
    if (m_target != nullptr) {
        m_target->Release();
        m_target = nullptr;
    }
    if (m_brush != nullptr) {
        m_brush->Release();
        m_brush = nullptr;
    }
    IDWriteTextFormat** formats[] = { &m_keyFormat, &m_valueFormat, &m_smallFormat };
    for (IDWriteTextFormat** format : formats) {
        if (*format != nullptr) {
            (*format)->Release();
            *format = nullptr;
        }
    }
    if (m_dwrite != nullptr) {
        m_dwrite->Release();
        m_dwrite = nullptr;
    }
    if (m_bitmap != nullptr) {
        DeleteObject(m_bitmap);
        m_bitmap = nullptr;
        m_pixels = nullptr;
    }
    if (m_memoryDc != nullptr) {
        DeleteDC(m_memoryDc);
        m_memoryDc = nullptr;
    }
    if (m_window != nullptr) {
        DestroyWindow(m_window);
        m_window = nullptr;
    }
}

void OverlayWindow::setSettings(const OverlaySettings& settings)
{
    m_settings = settings;
    repaint();
}

void OverlayWindow::setContent(const MetricStore& store, const FpsSummary& fps, const std::wstring& gpuName)
{
    m_store = store;
    m_fps = fps;
    m_gpuName = gpuName;
    m_hasContent = true;
}

void OverlayWindow::show()
{
    if (m_window == nullptr || m_visible) {
        return;
    }

    m_visible = true;
    ShowWindow(m_window, SW_SHOWNOACTIVATE);
    SetWindowPos(m_window, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    repaint();
}

void OverlayWindow::hide()
{
    if (m_window == nullptr) {
        return;
    }
    m_visible = false;
    ShowWindow(m_window, SW_HIDE);
}

void OverlayWindow::toggle()
{
    if (m_visible) {
        hide();
    } else {
        show();
    }
}

bool OverlayWindow::ensureRenderTarget(UINT width, UINT height)
{
    if (m_window == nullptr) {
        return false;
    }

    if (m_target == nullptr) {
        ID2D1Factory* factory = nullptr;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory))) {
            return false;
        }

        const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

        const HRESULT result = factory->CreateDCRenderTarget(&properties, &m_target);
        factory->Release();
        if (FAILED(result) || m_target == nullptr) {
            return false;
        }
    }

    if (m_bitmapWidth == width && m_bitmapHeight == height && m_memoryDc != nullptr) {
        return true;
    }

    if (m_bitmap != nullptr) {
        DeleteObject(m_bitmap);
        m_bitmap = nullptr;
        m_pixels = nullptr;
    }
    if (m_memoryDc == nullptr) {
        m_memoryDc = CreateCompatibleDC(nullptr);
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    m_bitmap = CreateDIBSection(m_memoryDc, &info, DIB_RGB_COLORS, &m_pixels, nullptr, 0);
    if (m_bitmap == nullptr) {
        return false;
    }

    SelectObject(m_memoryDc, m_bitmap);
    m_bitmapWidth = width;
    m_bitmapHeight = height;

    const RECT bounds{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    m_target->BindDC(m_memoryDc, &bounds);
    return true;
}

void OverlayWindow::repaint()
{
    if (!m_visible || !m_hasContent) {
        return;
    }
    render();
}

void OverlayWindow::drawBackground(const D2D1_RECT_F& frame, float scale)
{
    const float opacity = static_cast<float>(std::clamp(m_settings.opacityPercent, 0, 100)) / 100.0f;
    const bool isMinimal = m_settings.style == OverlayStyle::Minimal;
    const bool isCapsule = m_settings.style == OverlayStyle::Capsule;

    if (isMinimal) {
        return;
    }

    if (isCapsule) {
        ID2D1SolidColorBrush* brush = nullptr;
        const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(frame, frame.bottom - frame.top, frame.bottom - frame.top);
        if (SUCCEEDED(m_target->CreateSolidColorBrush(D2D1::ColorF(0.02f, 0.03f, 0.05f, opacity), &brush))) {
            m_target->FillRoundedRectangle(pill, brush);
            brush->Release();
        }
        return;
    }

    D2D1_COLOR_F fill = colorOf(m_theme.panel);
    fill.a = m_settings.style == OverlayStyle::Terminal ? 0.92f : opacity;
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(m_target->CreateSolidColorBrush(fill, &brush))) {
        m_target->FillRoundedRectangle(D2D1::RoundedRect(frame, 8.0f * scale, 8.0f * scale), brush);
        brush->Release();
    }

    D2D1_COLOR_F border = colorOf(m_settings.style == OverlayStyle::Neon ? m_theme.accent : m_theme.line);
    border.a = m_settings.style == OverlayStyle::Neon ? 0.85f : 0.5f;
    if (SUCCEEDED(m_target->CreateSolidColorBrush(border, &brush))) {
        m_target->DrawRoundedRectangle(D2D1::RoundedRect(frame, 8.0f * scale, 8.0f * scale), brush, 1.0f);
        brush->Release();
    }
}

void OverlayWindow::drawRow(float y, float scale, const wchar_t* key, const std::wstring& value,
    const std::wstring& unit, bool emphasize)
{
    const float left = kMargin + 10.0f * scale;
    const float right = m_bitmapWidth - kMargin - 10.0f * scale;

    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(m_target->CreateSolidColorBrush(colorOf(m_theme.accent), &brush))) {
        m_target->DrawTextW(key, static_cast<UINT32>(wcslen(key)), m_keyFormat,
            D2D1::RectF(left, y, left + 46.0f * scale, y + 16.0f * scale), brush);
        brush->Release();
    }

    if (SUCCEEDED(m_target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.94f), &brush))) {
        m_target->DrawTextW(value.data(), static_cast<UINT32>(value.size()), m_smallFormat,
            D2D1::RectF(right - 96.0f * scale, y, right, y + 16.0f * scale), brush);
        brush->Release();
    }

    if (!unit.empty() && SUCCEEDED(m_target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.45f), &brush))) {
        m_target->DrawTextW(unit.data(), static_cast<UINT32>(unit.size()), m_keyFormat,
            D2D1::RectF(right, y, right + 40.0f * scale, y + 16.0f * scale), brush);
        brush->Release();
    }

    (void)emphasize;
}

void OverlayWindow::render()
{
    if (m_window == nullptr) {
        return;
    }

    const UINT width = static_cast<UINT>(GetSystemMetrics(SM_CXSCREEN));
    const UINT height = static_cast<UINT>(GetSystemMetrics(SM_CYSCREEN));
    if (m_target == nullptr && !ensureRenderTarget(width, height)) {
        return;
    }

    const float scale = static_cast<float>(std::clamp(m_settings.scalePercent, 70, 160)) / 100.0f;
    m_theme = makeTheme(Accent::Mint);

    m_target->BeginDraw();
    m_target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    if (m_settings.layout == OverlayLayout::Line) {
        drawLine(scale);
    } else {
        drawPlates(scale);
    }

    m_target->EndDraw();

    HDC screen = GetDC(nullptr);
    POINT source{ 0, 0 };
    SIZE size{ static_cast<LONG>(m_bitmapWidth), static_cast<LONG>(m_bitmapHeight) };
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(m_window, screen, nullptr, &size, m_memoryDc, &source, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

void OverlayWindow::drawPlates(float scale)
{
    const float width = 190.0f * scale;
    const float height = kPlateHeight * scale;
    const float left = alignFor(m_settings.position) == 0 ? kMargin : m_bitmapWidth - width - kMargin;
    const float top = isBottom(m_settings.position) ? m_bitmapHeight - height - kMargin : kMargin;

    const D2D1_RECT_F frame = D2D1::RectF(left, top, left + width, top + height);
    drawBackground(frame, scale);

    float y = top + 8.0f * scale;
    const float lineHeight = 16.5f * scale;

    const std::wstring fps = std::to_wstring(static_cast<int>(std::lround(m_fps.currentFps)));
    drawRow(y, scale, m_settings.labels[0].c_str(), fps, L"fps", true);
    y += lineHeight + 4.0f * scale;

    drawRow(y, scale, m_settings.labels[1].c_str(),
        std::to_wstring(static_cast<int>(std::lround(m_store.value(MetricId::CpuLoad)))),
        std::to_wstring(static_cast<int>(std::lround(m_store.value(MetricId::CpuTemp)))) + L"°C", false);
    y += lineHeight;

    drawRow(y, scale, m_settings.labels[2].c_str(),
        std::to_wstring(static_cast<int>(std::lround(m_store.value(MetricId::GpuLoad)))),
        std::to_wstring(static_cast<int>(std::lround(m_store.value(MetricId::GpuTemp)))) + L"°C", false);
    y += lineHeight;

    wchar_t ram[32]{};
    swprintf_s(ram, L"%.1f", m_store.value(MetricId::RamUsed));
    drawRow(y, scale, m_settings.labels[3].c_str(), ram, L"ГБ", false);
}

void OverlayWindow::drawLine(float scale)
{
    const float width = 420.0f * scale;
    const float height = 30.0f * scale;
    const float left = alignFor(m_settings.position) == 0 ? kMargin : m_bitmapWidth - width - kMargin;
    const float top = isBottom(m_settings.position) ? m_bitmapHeight - height - kMargin : kMargin;

    drawBackground(D2D1::RectF(left, top, left + width, top + height), scale);

    wchar_t line[128]{};
    swprintf_s(line, L"%s %d fps   %s %d%%   %s %d%%   %s %.1f ГБ", m_settings.labels[0].c_str(),
        static_cast<int>(std::lround(m_fps.currentFps)), m_settings.labels[1].c_str(),
        static_cast<int>(std::lround(m_store.value(MetricId::CpuLoad))), m_settings.labels[2].c_str(),
        static_cast<int>(std::lround(m_store.value(MetricId::GpuLoad))), m_settings.labels[3].c_str(),
        m_store.value(MetricId::RamUsed));

    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(m_target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f), &brush))) {
        m_target->DrawTextW(line, static_cast<UINT32>(wcslen(line)), m_smallFormat,
            D2D1::RectF(left + 14.0f, top, left + width - 14.0f, top + height), brush);
        brush->Release();
    }
}

} // namespace yawa

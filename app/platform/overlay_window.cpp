#include "platform/overlay_window.h"
#include "platform/win_util.h"
#include "ui/layout.h"

#include <d2d1helper.h>
#include <dwrite.h>

#include <algorithm>
#include <wincodec.h>

#include <cstring>
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

    const bool isTerminal = m_settings.style == OverlayStyle::Terminal;
    const bool isNeon = m_settings.style == OverlayStyle::Neon;
    const float radius = isTerminal ? 2.0f * scale : 10.0f * scale;

    D2D1_COLOR_F fill = colorOf(isTerminal ? m_theme.background : m_theme.panel);
    fill.a = isTerminal ? 0.94f : opacity;
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(m_target->CreateSolidColorBrush(fill, &brush))) {
        m_target->FillRoundedRectangle(D2D1::RoundedRect(frame, radius, radius), brush);
        brush->Release();
    }

    D2D1_COLOR_F border = colorOf(isNeon ? m_theme.accent : m_theme.line);
    border.a = isNeon ? 0.9f : 0.45f;
    if (SUCCEEDED(m_target->CreateSolidColorBrush(border, &brush))) {
        m_target->DrawRoundedRectangle(D2D1::RoundedRect(frame, radius, radius), brush,
            isNeon ? 1.6f : 1.0f);
        brush->Release();
    }

    if (isNeon) {
        // Второй контур с малой прозрачностью даёт мягкое свечение без размытия.
        D2D1_COLOR_F glow = colorOf(m_theme.accent);
        glow.a = 0.18f;
        if (SUCCEEDED(m_target->CreateSolidColorBrush(glow, &brush))) {
            m_target->DrawRoundedRectangle(D2D1::RoundedRect(
                D2D1::RectF(frame.left - 2.0f, frame.top - 2.0f, frame.right + 2.0f, frame.bottom + 2.0f),
                radius + 2.0f, radius + 2.0f), brush, 2.0f);
            brush->Release();
        }
    }
}

void OverlayWindow::drawRow(float y, float scale, const std::wstring& key, const std::wstring& value,
    const std::wstring& secondary, bool emphasize)
{
    // Колонки считаются от рамки плитки, а не от края экрана: иначе подпись уходит
    // в левый угол, а число — в правый край монитора.
    // У капсулы скругления съедают край, поэтому текст отодвинут глубже.
    const float inset = (m_settings.style == OverlayStyle::Capsule ? 16.0f : 10.0f) * scale;
    const float left = m_contentRect.left + inset;
    const float right = m_contentRect.right - inset;
    const float secondaryWidth = secondary.empty() ? 0.0f : 58.0f * scale;
    const float valueRight = right - secondaryWidth;

    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(m_target->CreateSolidColorBrush(colorOf(m_theme.accent), &brush))) {
        m_target->DrawTextW(key.data(), static_cast<UINT32>(key.size()), m_keyFormat,
            D2D1::RectF(left, y, left + 52.0f * scale, y + 16.0f * scale), brush);
        brush->Release();
    }

    const float valueOpacity = emphasize ? 1.0f : 0.94f;
    if (SUCCEEDED(m_target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, valueOpacity), &brush))) {
        m_target->DrawTextW(value.data(), static_cast<UINT32>(value.size()), m_smallFormat,
            D2D1::RectF(valueRight - 108.0f * scale, y, valueRight, y + 16.0f * scale), brush);
        brush->Release();
    }

    if (!secondary.empty()
        && SUCCEEDED(m_target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.45f), &brush))) {
        m_target->DrawTextW(secondary.data(), static_cast<UINT32>(secondary.size()), m_keyFormat,
            D2D1::RectF(valueRight + 8.0f * scale, y, right, y + 16.0f * scale), brush);
        brush->Release();
    }
}

void OverlayWindow::drawScene(float scale)
{
    (void)scale;
    m_theme = makeTheme(Accent::Mint);

    m_target->BeginDraw();
    m_target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    if (m_settings.layout == OverlayLayout::Line) {
        drawLine(scale);
    } else {
        drawPlates(scale);
    }

    m_target->EndDraw();
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

    drawScene(static_cast<float>(std::clamp(m_settings.scalePercent, 70, 160)) / 100.0f);

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
    m_contentRect = frame;
    drawBackground(frame, scale);

    float y = top + 8.0f * scale;
    const float lineHeight = 16.5f * scale;

    const auto temperature = [this](MetricId id) {
        const double value = m_store.value(id);
        return value > 0.0 ? std::to_wstring(static_cast<int>(std::lround(value))) + L" °C"
                           : std::wstring(L"—");
    };

    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%d fps", static_cast<int>(std::lround(m_fps.currentFps)));
    drawRow(y, scale, wideText(m_settings.labels[0]), buffer, std::wstring(), true);
    y += lineHeight + 4.0f * scale;

    swprintf_s(buffer, L"%d %%", static_cast<int>(std::lround(m_store.value(MetricId::CpuLoad))));
    drawRow(y, scale, wideText(m_settings.labels[1]), buffer, temperature(MetricId::CpuTemp), false);
    y += lineHeight;

    swprintf_s(buffer, L"%d %%", static_cast<int>(std::lround(m_store.value(MetricId::GpuLoad))));
    drawRow(y, scale, wideText(m_settings.labels[2]), buffer, temperature(MetricId::GpuTemp), false);
    y += lineHeight;

    // Десятичная запятая: интерфейс приложения везде русский.
    swprintf_s(buffer, L"%.1f ГБ", m_store.value(MetricId::RamUsed));
    for (wchar_t* symbol = buffer; *symbol != L'\0'; ++symbol) {
        if (*symbol == L'.') {
            *symbol = L',';
        }
    }
    drawRow(y, scale, wideText(m_settings.labels[3]), buffer, std::wstring(), false);
}

void OverlayWindow::drawLine(float scale)
{
    const float width = 420.0f * scale;
    const float height = 30.0f * scale;
    const float left = alignFor(m_settings.position) == 0 ? kMargin : m_bitmapWidth - width - kMargin;
    const float top = isBottom(m_settings.position) ? m_bitmapHeight - height - kMargin : kMargin;

    m_contentRect = D2D1::RectF(left, top, left + width, top + height);
    drawBackground(m_contentRect, scale);

    const std::wstring text = wideText(m_settings.labels[0]) + L" "
        + std::to_wstring(static_cast<int>(std::lround(m_fps.currentFps))) + L" fps   "
        + wideText(m_settings.labels[1]) + L" " + std::to_wstring(static_cast<int>(std::lround(
                                                    m_store.value(MetricId::CpuLoad))))
        + L"%   " + wideText(m_settings.labels[2]) + L" " + std::to_wstring(static_cast<int>(std::lround(
                                                               m_store.value(MetricId::GpuLoad))))
        + L"%   " + wideText(m_settings.labels[3]) + L" "
        + std::to_wstring(static_cast<int>(std::lround(m_store.value(MetricId::RamUsed) * 10.0) / 10.0)) + L" ГБ";

    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(m_target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f), &brush))) {
        m_target->DrawTextW(text.data(), static_cast<UINT32>(text.size()), m_smallFormat,
            D2D1::RectF(left + 14.0f, top, left + width - 14.0f, top + height), brush);
        brush->Release();
    }
}

bool OverlayWindow::capturePng(const std::wstring& path, std::string& error)
{
    if (m_window == nullptr) {
        error = "окно оверлея не создано";
        return false;
    }

    const UINT screenWidth = static_cast<UINT>(GetSystemMetrics(SM_CXSCREEN));
    const UINT screenHeight = static_cast<UINT>(GetSystemMetrics(SM_CYSCREEN));
    if (m_target == nullptr && !ensureRenderTarget(screenWidth, screenHeight)) {
        error = "не удалось подготовить поверхность оверлея";
        return false;
    }

    drawScene(static_cast<float>(std::clamp(m_settings.scalePercent, 70, 160)) / 100.0f);

    if (m_pixels == nullptr || m_contentRect.right <= m_contentRect.left) {
        error = "оверлей ничего не нарисовал";
        return false;
    }

    // Обрезаем прозрачные поля вокруг содержимого: в PNG попадает только сам оверлей.
    const LONG left = std::max(0L, static_cast<LONG>(m_contentRect.left) - 2L);
    const LONG top = std::max(0L, static_cast<LONG>(m_contentRect.top) - 2L);
    const LONG right = std::min(static_cast<LONG>(m_bitmapWidth),
        static_cast<LONG>(m_contentRect.right) + 2L);
    const LONG bottom = std::min(static_cast<LONG>(m_bitmapHeight),
        static_cast<LONG>(m_contentRect.bottom) + 2L);

    const UINT width = static_cast<UINT>(right - left);
    const UINT height = static_cast<UINT>(bottom - top);
    const auto* source = static_cast<const std::uint8_t*>(m_pixels);
    const std::size_t stride = static_cast<std::size_t>(m_bitmapWidth) * 4U;

    std::vector<std::uint8_t> cropped(static_cast<std::size_t>(width) * height * 4U);
    for (UINT row = 0; row < height; ++row) {
        const std::uint8_t* from = source + (static_cast<std::size_t>(top + static_cast<LONG>(row)) * stride)
            + static_cast<std::size_t>(left) * 4U;
        std::memcpy(cropped.data() + static_cast<std::size_t>(row) * width * 4U, from,
            static_cast<std::size_t>(width) * 4U);
    }

    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory)))) {
        error = "WIC недоступен";
        return false;
    }

    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IWICBitmap* bitmap = nullptr;
    bool ok = false;

    do {
        if (FAILED(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppPBGRA, width * 4U,
                static_cast<UINT>(cropped.size()), cropped.data(), &bitmap))) {
            error = "WIC не принял пиксели оверлея";
            break;
        }
        if (FAILED(factory->CreateStream(&stream))
            || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
            error = "не удалось открыть " + narrow(path);
            break;
        }
        if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))
            || FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) {
            error = "не удалось создать PNG-кодировщик";
            break;
        }
        if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) {
            error = "не удалось создать кадр PNG";
            break;
        }
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
        if (FAILED(frame->SetSize(width, height)) || FAILED(frame->SetPixelFormat(&format))
            || FAILED(frame->WriteSource(bitmap, nullptr)) || FAILED(frame->Commit())
            || FAILED(encoder->Commit())) {
            error = "не удалось записать PNG";
            break;
        }
        ok = true;
    } while (false);

    if (bitmap != nullptr) {
        bitmap->Release();
    }
    if (frame != nullptr) {
        frame->Release();
    }
    if (encoder != nullptr) {
        encoder->Release();
    }
    if (stream != nullptr) {
        stream->Release();
    }
    factory->Release();
    return ok;
}

} // namespace yawa

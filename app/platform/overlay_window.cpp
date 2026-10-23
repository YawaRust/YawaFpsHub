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

    m_monitorRect = monitorRect();
    const int screenWidth = m_monitorRect.right - m_monitorRect.left;
    const int screenHeight = m_monitorRect.bottom - m_monitorRect.top;

    m_window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW
            | WS_EX_NOACTIVATE,
        kClassName, L"YawaFpsHub overlay", WS_POPUP, m_monitorRect.left, m_monitorRect.top, screenWidth,
        screenHeight, nullptr, nullptr, instance, nullptr);

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
    m_monoFormat = createFormat(m_dwrite, 11.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING,
        L"Cascadia Mono");

    // Содержимое появляется только после первого опроса. Чтобы оверлей не оказался
    // пустым при включении, считаем, что рисовать уже есть что: нули честнее пустоты.
    m_hasContent = true;
    return true;
}

RECT OverlayWindow::monitorRect() const
{
    POINT cursor{};
    GetCursorPos(&cursor);
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &info) != FALSE) {
        return info.rcMonitor;
    }

    RECT fallback{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    return fallback;
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
    IDWriteTextFormat** formats[] = { &m_keyFormat, &m_valueFormat, &m_smallFormat, &m_monoFormat };
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

void OverlayWindow::setAccent(Accent accent)
{
    m_theme = makeTheme(accent);
    repaint();
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

bool OverlayWindow::show(std::string* error)
{
    if (m_window == nullptr) {
        if (error != nullptr) {
            *error = "окно оверлея не создано";
        }
        return false;
    }

    // Монитор пересчитываем на каждом показе: пользователь мог перетащить игру на другой.
    m_monitorRect = monitorRect();
    const int width = m_monitorRect.right - m_monitorRect.left;
    const int height = m_monitorRect.bottom - m_monitorRect.top;

    SetWindowPos(m_window, HWND_TOPMOST, m_monitorRect.left, m_monitorRect.top, width, height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    m_visible = true;

    if (!render()) {
        m_visible = false;
        ShowWindow(m_window, SW_HIDE);
        if (error != nullptr) {
            *error = "не удалось нарисовать оверлей";
        }
        return false;
    }

    ShowWindow(m_window, SW_SHOWNOACTIVATE);
    return true;
}

void OverlayWindow::hide()
{
    if (m_window == nullptr) {
        return;
    }
    m_visible = false;
    ShowWindow(m_window, SW_HIDE);
}

void OverlayWindow::toggle(std::string* error)
{
    if (m_visible) {
        hide();
    } else {
        show(error);
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

        HRESULT result = factory->CreateDCRenderTarget(&properties, &m_target);
        if (FAILED(result) || m_target == nullptr) {
            // Запасной путь: программный рендер. Именно он нужен в RDP и на машинах
            // без аппаратного ускорения Direct2D.
            const D2D1_RENDER_TARGET_PROPERTIES software = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            result = factory->CreateDCRenderTarget(&software, &m_target);
        }
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
    if (!m_visible) {
        return;
    }
    render();
}



OverlayValues OverlayWindow::currentValues() const
{
    OverlayValues values;
    values.fps = m_fps.currentFps;
    values.low1 = m_fps.low1Fps;
    values.cpuLoad = m_store.value(MetricId::CpuLoad);
    values.cpuTemp = m_store.value(MetricId::CpuTemp);
    values.hasCpuTemp = values.cpuTemp > 0.0;
    values.gpuLoad = m_store.value(MetricId::GpuLoad);
    values.gpuTemp = m_store.value(MetricId::GpuTemp);
    values.hasGpuTemp = values.gpuTemp > 0.0;
    values.ramUsed = m_store.value(MetricId::RamUsed);
    values.ramTotal = m_store.value(MetricId::RamUsed) + m_store.value(MetricId::RamFree);
    values.vramUsed = m_store.value(MetricId::VramUsed);
    values.vramTotal = m_store.value(MetricId::VramTotal);
    values.hasVram = values.vramTotal > 0.0;
    return values;
}

void OverlayWindow::drawScene(float scale)
{
    OverlayFormats formats{ m_dwrite, m_keyFormat, m_valueFormat, m_smallFormat, m_monoFormat };
    const OverlayText text = makeOverlayText(m_settings, currentValues());
    const Rect bounds{ 0.0f, 0.0f, static_cast<float>(m_bitmapWidth), static_cast<float>(m_bitmapHeight) };
    m_contentRectD2D = D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f);

    m_target->BeginDraw();
    m_target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    const Rect content = paintOverlay(m_target, m_settings, m_theme, text, bounds, scale, formats);
    m_contentRectD2D = D2D1::RectF(content.left, content.top, content.right, content.bottom);
    m_target->EndDraw();
}

bool OverlayWindow::render()
{
    if (m_window == nullptr) {
        return false;
    }

    // Разрешение или монитор могли смениться, пока оверлей висел поверх игры.
    const RECT currentMonitor = monitorRect();
    if (currentMonitor.left != m_monitorRect.left || currentMonitor.top != m_monitorRect.top
        || currentMonitor.right != m_monitorRect.right || currentMonitor.bottom != m_monitorRect.bottom) {
        m_monitorRect = currentMonitor;
        SetWindowPos(m_window, HWND_TOPMOST, m_monitorRect.left, m_monitorRect.top,
            m_monitorRect.right - m_monitorRect.left, m_monitorRect.bottom - m_monitorRect.top,
            SWP_NOACTIVATE);
    }
    const UINT width = static_cast<UINT>(m_monitorRect.right - m_monitorRect.left);
    const UINT height = static_cast<UINT>(m_monitorRect.bottom - m_monitorRect.top);
    // ensureRenderTarget сам решает, нужно ли пересоздавать битмап: при смене разрешения
    // иначе остался бы старый размер и картинка поплыла бы.
    if (!ensureRenderTarget(width, height)) {
        return false;
    }

    drawScene(static_cast<float>(std::clamp(m_settings.scalePercent, 70, 160)) / 100.0f);

    HDC screen = GetDC(nullptr);
    POINT destination{ m_monitorRect.left, m_monitorRect.top };
    POINT source{ 0, 0 };
    SIZE size{ static_cast<LONG>(m_bitmapWidth), static_cast<LONG>(m_bitmapHeight) };
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    const BOOL updated = UpdateLayeredWindow(m_window, screen, &destination, &size, m_memoryDc, &source, 0, &blend,
        ULW_ALPHA);
    ReleaseDC(nullptr, screen);
    return updated != FALSE;
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

    if (m_pixels == nullptr || m_contentRectD2D.right <= m_contentRectD2D.left) {
        error = "оверлей ничего не нарисовал";
        return false;
    }

    // Обрезаем прозрачные поля вокруг содержимого: в PNG попадает только сам оверлей.
    const LONG left = std::max(0L, static_cast<LONG>(m_contentRectD2D.left) - 2L);
    const LONG top = std::max(0L, static_cast<LONG>(m_contentRectD2D.top) - 2L);
    const LONG right = std::min(static_cast<LONG>(m_bitmapWidth),
        static_cast<LONG>(m_contentRectD2D.right) + 2L);
    const LONG bottom = std::min(static_cast<LONG>(m_bitmapHeight),
        static_cast<LONG>(m_contentRectD2D.bottom) + 2L);

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

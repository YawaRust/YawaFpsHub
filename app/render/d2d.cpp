#include "render/d2d.h"

#include <d2d1helper.h>

#include <algorithm>
#include <cmath>

namespace yawa {
namespace {

constexpr wchar_t kFontFamily[] = L"Segoe UI Variable Display";
constexpr wchar_t kFallbackFamily[] = L"Segoe UI";
constexpr wchar_t kMonoFamily[] = L"Cascadia Mono";

D2D1_COLOR_F toD2D(const Color& color)
{
    return D2D1::ColorF(color.r, color.g, color.b, color.a);
}

IDWriteTextFormat* createFormat(IDWriteFactory* factory, const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight)
{
    IDWriteTextFormat* format = nullptr;
    const HRESULT result = factory->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, size, L"ru-RU", &format);
    if (FAILED(result)) {
        return nullptr;
    }

    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return format;
}

} // namespace

Renderer::~Renderer()
{
    shutdown();
}

bool Renderer::initialize(std::string& error)
{
    const HRESULT d2dResult = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_d2d);
    if (FAILED(d2dResult)) {
        error = "D2D1CreateFactory не удался";
        return false;
    }

    const HRESULT dwriteResult = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&m_dwrite));
    if (FAILED(dwriteResult)) {
        error = "DWriteCreateFactory не удался";
        return false;
    }

    createTextFormats();
    return true;
}

void Renderer::createTextFormats()
{
    if (m_dwrite == nullptr) {
        return;
    }

    m_display = createFormat(m_dwrite, kFontFamily, 84.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    m_value = createFormat(m_dwrite, kFontFamily, 26.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    m_heading = createFormat(m_dwrite, kFontFamily, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    m_body = createFormat(m_dwrite, kFontFamily, 12.5f, DWRITE_FONT_WEIGHT_NORMAL);
    m_small = createFormat(m_dwrite, kFontFamily, 11.5f, DWRITE_FONT_WEIGHT_NORMAL);
    m_tiny = createFormat(m_dwrite, kFontFamily, 9.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    m_mono = createFormat(m_dwrite, kMonoFamily, 11.5f, DWRITE_FONT_WEIGHT_NORMAL);

    // Без Segoe UI Variable (Windows 10) DirectWrite вернёт формат с подставленным шрифтом,
    // но интерфейс заметно поедет по метрикам — тогда берём обычный Segoe UI.
    if (m_display == nullptr) {
        m_display = createFormat(m_dwrite, kFallbackFamily, 84.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    }
}

void Renderer::releaseDeviceResources()
{
    for (ID2D1SolidColorBrush* brush : m_brushPool) {
        if (brush != nullptr) {
            brush->Release();
        }
    }
    m_brushPool.clear();

    if (m_brush != nullptr) {
        m_brush->Release();
        m_brush = nullptr;
    }
    if (m_target != nullptr) {
        m_target->Release();
        m_target = nullptr;
    }
}

void Renderer::shutdown()
{
    releaseDeviceResources();

    IDWriteTextFormat** formats[] = { &m_display, &m_value, &m_heading, &m_body, &m_small, &m_tiny, &m_mono };
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
    if (m_d2d != nullptr) {
        m_d2d->Release();
        m_d2d = nullptr;
    }
}

bool Renderer::createDeviceResources(std::string& error)
{
    if (m_window == nullptr || m_d2d == nullptr) {
        error = "окно или фабрика D2D не готовы";
        return false;
    }

    RECT client{};
    GetClientRect(m_window, &client);
    const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT>(std::max<LONG>(client.right, 1)),
        static_cast<LONG>(std::max<LONG>(client.bottom, 1)));

    const HRESULT result = m_d2d->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(m_window, size, D2D1_PRESENT_OPTIONS_IMMEDIATELY), &m_target);
    if (FAILED(result)) {
        error = "CreateHwndRenderTarget не удался";
        return false;
    }

    m_target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    m_target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    m_target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_brush);

    m_width = static_cast<float>(size.width);
    m_height = static_cast<float>(size.height);
    return m_brush != nullptr;
}

bool Renderer::begin(HWND window)
{
    m_window = window;

    if (m_target == nullptr) {
        std::string error;
        if (!createDeviceResources(error)) {
            return false;
        }
    }

    m_target->BeginDraw();
    m_drawing = true;
    return true;
}

bool Renderer::end()
{
    if (!m_drawing) {
        return false;
    }

    m_drawing = false;
    const HRESULT result = m_target->EndDraw();

    // Потеря устройства — обычное дело при смене GPU или пробуждении: пересоздаём ресурсы.
    if (result == D2DERR_RECREATE_TARGET) {
        releaseDeviceResources();
        return false;
    }
    return SUCCEEDED(result);
}

void Renderer::resize(UINT width, UINT height, float dpiScale)
{
    m_dpiScale = dpiScale > 0.0f ? dpiScale : 1.0f;

    if (m_target != nullptr) {
        m_target->Resize(D2D1::SizeU(std::max<UINT>(width, 1), std::max<UINT>(height, 1)));
    }

    m_width = static_cast<float>(std::max<UINT>(width, 1));
    m_height = static_cast<float>(std::max<UINT>(height, 1));
}

ID2D1SolidColorBrush* Renderer::brushFor(const Color& color)
{
    if (m_brush == nullptr) {
        return nullptr;
    }
    m_brush->SetColor(toD2D(color));
    return m_brush;
}

void Renderer::clear(const Color& color)
{
    if (m_target != nullptr) {
        m_target->Clear(toD2D(color));
    }
}

void Renderer::fillRect(const Rect& rect, const Color& color)
{
    ID2D1SolidColorBrush* brush = brushFor(color);
    if (brush == nullptr || rect.width() <= 0.0f || rect.height() <= 0.0f) {
        return;
    }
    m_target->FillRectangle(D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom), brush);
}

void Renderer::fillRoundRect(const Rect& rect, float radius, const Color& color)
{
    ID2D1SolidColorBrush* brush = brushFor(color);
    if (brush == nullptr || rect.width() <= 0.0f || rect.height() <= 0.0f) {
        return;
    }
    m_target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom),
                                     radius, radius),
        brush);
}

void Renderer::strokeRoundRect(const Rect& rect, float radius, const Color& color, float thickness)
{
    ID2D1SolidColorBrush* brush = brushFor(color);
    if (brush == nullptr) {
        return;
    }
    m_target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom),
                                     radius, radius),
        brush, thickness);
}

void Renderer::pushClip(const Rect& rect)
{
    if (m_target == nullptr) {
        return;
    }
    m_target->PushAxisAlignedClip(D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom),
        D2D1_ANTIALIAS_MODE_ALIASED);
}

void Renderer::popClip()
{
    if (m_target != nullptr) {
        m_target->PopAxisAlignedClip();
    }
}

void Renderer::pushTranslate(float dx, float dy)
{
    if (m_target == nullptr) {
        return;
    }
    m_target->SetTransform(D2D1::Matrix3x2F::Translation(dx, dy));
}

void Renderer::popTranslate()
{
    if (m_target != nullptr) {
        m_target->SetTransform(D2D1::Matrix3x2F::Identity());
    }
}

void Renderer::fillCircle(float centerX, float centerY, float radius, const Color& color)
{
    ID2D1SolidColorBrush* brush = brushFor(color);
    if (brush == nullptr || radius <= 0.0f) {
        return;
    }
    m_target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(centerX, centerY), radius, radius), brush);
}

void Renderer::drawLine(float x1, float y1, float x2, float y2, const Color& color, float thickness)
{
    ID2D1SolidColorBrush* brush = brushFor(color);
    if (brush == nullptr) {
        return;
    }
    m_target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush, thickness);
}

void Renderer::drawDashedLine(float x1, float y1, float x2, float y2, const Color& color, float thickness)
{
    ID2D1StrokeStyle* dashed = nullptr;
    const D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT,
        D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10.0f, D2D1_DASH_STYLE_DASH, 0.0f);

    if (FAILED(m_d2d->CreateStrokeStyle(properties, nullptr, 0, &dashed)) || dashed == nullptr) {
        drawLine(x1, y1, x2, y2, color, thickness);
        return;
    }

    ID2D1SolidColorBrush* brush = brushFor(color);
    if (brush != nullptr) {
        m_target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush, thickness, dashed);
    }
    dashed->Release();
}

void Renderer::fillRadialGlow(float centerX, float centerY, float radius, const Color& color)
{
    if (m_target == nullptr || radius <= 0.0f) {
        return;
    }

    const D2D1_GRADIENT_STOP stops[2]{ { 0.0f, toD2D(color) },
        { 1.0f, D2D1::ColorF(color.r, color.g, color.b, 0.0f) } };
    ID2D1GradientStopCollection* collection = nullptr;
    if (FAILED(m_target->CreateGradientStopCollection(stops, 2, &collection)) || collection == nullptr) {
        return;
    }

    ID2D1RadialGradientBrush* brush = nullptr;
    const D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES properties{ D2D1::Point2F(centerX, centerY),
        D2D1::Point2F(0.0f, 0.0f), radius, radius };
    if (SUCCEEDED(m_target->CreateRadialGradientBrush(properties, collection, &brush)) && brush != nullptr) {
        m_target->FillRectangle(
            D2D1::RectF(centerX - radius, centerY - radius, centerX + radius, centerY + radius), brush);
        brush->Release();
    }
    collection->Release();
}

void Renderer::strokeCircle(float centerX, float centerY, float radius, const Color& color, float thickness)
{
    ID2D1SolidColorBrush* brush = brushFor(color);
    if (brush == nullptr) {
        return;
    }
    m_target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(centerX, centerY), radius, radius), brush, thickness);
}

void Renderer::drawArc(float centerX, float centerY, float radius, float thickness, float sweepFraction,
    const Color& color)
{
    ID2D1SolidColorBrush* brush = brushFor(color);
    if (brush == nullptr) {
        return;
    }

    const float clamped = std::clamp(sweepFraction, 0.0f, 1.0f);
    if (clamped <= 0.001f) {
        return;
    }

    const float startAngle = -3.14159265f / 2.0f;
    const float endAngle = startAngle + clamped * 2.0f * 3.14159265f;

    ID2D1PathGeometry* geometry = nullptr;
    if (FAILED(m_d2d->CreatePathGeometry(&geometry)) || geometry == nullptr) {
        return;
    }

    ID2D1GeometrySink* sink = nullptr;
    if (SUCCEEDED(geometry->Open(&sink)) && sink != nullptr) {
        sink->BeginFigure(D2D1::Point2F(centerX + radius * std::cos(startAngle), centerY + radius * std::sin(startAngle)),
            D2D1_FIGURE_BEGIN_HOLLOW);

        const D2D1_ARC_SEGMENT arc = D2D1::ArcSegment(
            D2D1::Point2F(centerX + radius * std::cos(endAngle), centerY + radius * std::sin(endAngle)),
            D2D1::SizeF(radius, radius), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
            clamped > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL);
        sink->AddArc(arc);
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();

        D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties();
        properties.startCap = D2D1_CAP_STYLE_ROUND;
        properties.endCap = D2D1_CAP_STYLE_ROUND;

        ID2D1StrokeStyle* style = nullptr;
        if (SUCCEEDED(m_d2d->CreateStrokeStyle(properties, nullptr, 0, &style)) && style != nullptr) {
            m_target->DrawGeometry(geometry, brush, thickness, style);
            style->Release();
        }
        sink->Release();
    }
    geometry->Release();
}

IDWriteTextFormat* Renderer::formatFor(TextRole role) const
{
    switch (role) {
    case TextRole::Display:
        return m_display;
    case TextRole::Value:
        return m_value;
    case TextRole::Heading:
        return m_heading;
    case TextRole::Body:
        return m_body;
    case TextRole::Small:
        return m_small;
    case TextRole::Tiny:
        return m_tiny;
    case TextRole::Mono:
        return m_mono;
    }
    return m_body;
}

void Renderer::text(std::wstring_view value, const Rect& rect, TextRole role, const Color& color, TextAlign align)
{
    IDWriteTextFormat* format = formatFor(role);
    ID2D1SolidColorBrush* brush = brushFor(color);
    if (format == nullptr || brush == nullptr || value.empty()) {
        return;
    }

    switch (align) {
    case TextAlign::Left:
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        break;
    case TextAlign::Center:
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        break;
    case TextAlign::Right:
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        break;
    }

    m_target->DrawTextW(value.data(), static_cast<UINT32>(value.size()), format,
        D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Renderer::measureText(std::wstring_view value, TextRole role)
{
    IDWriteTextFormat* format = formatFor(role);
    if (format == nullptr || m_dwrite == nullptr || value.empty()) {
        return 0.0f;
    }

    IDWriteTextLayout* layout = nullptr;
    if (FAILED(m_dwrite->CreateTextLayout(value.data(), static_cast<UINT32>(value.size()), format, 4096.0f, 64.0f,
            &layout))
        || layout == nullptr) {
        return 0.0f;
    }

    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    layout->Release();
    return metrics.widthIncludingTrailingWhitespace;
}

void Renderer::drawSeries(const ChartSeries& series, const Rect& rect, float minimum, float maximum, float thickness)
{
    if (series.values == nullptr || series.count < 2 || rect.width() <= 1.0f || maximum <= minimum) {
        return;
    }

    ID2D1PathGeometry* geometry = nullptr;
    if (FAILED(m_d2d->CreatePathGeometry(&geometry)) || geometry == nullptr) {
        return;
    }

    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(geometry->Open(&sink)) || sink == nullptr) {
        geometry->Release();
        return;
    }

    const float step = rect.width() / static_cast<float>(series.count - 1);
    const auto toY = [&](float value) {
        const float normalized = (value - minimum) / (maximum - minimum);
        return rect.bottom - std::clamp(normalized, 0.0f, 1.0f) * rect.height();
    };

    sink->BeginFigure(D2D1::Point2F(rect.left, toY(series.values[0])), D2D1_FIGURE_BEGIN_FILLED);
    for (std::size_t index = 1; index < series.count; ++index) {
        sink->AddLine(D2D1::Point2F(rect.left + step * static_cast<float>(index), toY(series.values[index])));
    }

    if (series.filled) {
        sink->AddLine(D2D1::Point2F(rect.right, rect.bottom));
        sink->AddLine(D2D1::Point2F(rect.left, rect.bottom));
    }

    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();

    ID2D1SolidColorBrush* brush = brushFor(series.color);
    if (brush != nullptr) {
        if (series.filled) {
            m_target->FillGeometry(geometry, brush);
        } else {
            m_target->DrawGeometry(geometry, brush, thickness);
        }
    }

    sink->Release();
    geometry->Release();
}

} // namespace yawa

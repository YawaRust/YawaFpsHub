#include "render/overlay_painter.h"

#include <algorithm>
#include <cstdio>
#include <cwctype>

namespace yawa {
namespace {

constexpr float kMargin = 18.0f;
constexpr float kRowStep = 20.0f;
constexpr float kRowHeight = 16.0f;

D2D1_COLOR_F toD2D(const Color& color, float alphaScale = 1.0f)
{
    return D2D1::ColorF(color.r, color.g, color.b, color.a * alphaScale);
}

std::wstring wideLabel(std::string_view utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), length);
    return result;
}

std::wstring formatNumber(double value, int decimals, std::wstring_view unit)
{
    wchar_t buffer[48]{};
    swprintf_s(buffer, decimals > 0 ? L"%.1f" : L"%.0f", value);
    std::wstring text = buffer;
    if (decimals > 0) {
        // Десятичная запятая: приложение и оверлей полностью русские.
        for (wchar_t& symbol : text) {
            if (symbol == L'.') {
                symbol = L',';
            }
        }
    }
    if (!unit.empty()) {
        text += L" ";
        text += unit;
    }
    return text;
}

std::wstring temperatureText(double value, bool available)
{
    if (!available || value <= 0.0) {
        return L"—";
    }
    return formatNumber(value, 0, L"°C");
}

std::wstring upperCase(std::wstring value)
{
    for (wchar_t& symbol : value) {
        symbol = static_cast<wchar_t>(std::towupper(symbol));
    }
    return value;
}

class Painter {
public:
    Painter(ID2D1RenderTarget* target, const Theme& theme, const OverlayFormats& formats, float scale, float opacity)
        : m_target(target)
        , m_theme(theme)
        , m_formats(formats)
        , m_scale(scale)
        , m_opacity(opacity)
    {
    }

    void fillRound(const Rect& rect, float radius, const Color& color, float alphaScale = 1.0f)
    {
        ID2D1SolidColorBrush* brush = nullptr;
        if (FAILED(m_target->CreateSolidColorBrush(toD2D(color, alphaScale), &brush))) {
            return;
        }
        m_target->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom), radius, radius), brush);
        brush->Release();
    }

    void strokeRound(const Rect& rect, float radius, const Color& color, float thickness, float alphaScale = 1.0f)
    {
        ID2D1SolidColorBrush* brush = nullptr;
        if (FAILED(m_target->CreateSolidColorBrush(toD2D(color, alphaScale), &brush))) {
            return;
        }
        m_target->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom), radius, radius), brush,
            thickness);
        brush->Release();
    }

    void line(float x1, float y1, float x2, float y2, const Color& color, float thickness, float alphaScale = 1.0f)
    {
        ID2D1SolidColorBrush* brush = nullptr;
        if (FAILED(m_target->CreateSolidColorBrush(toD2D(color, alphaScale), &brush))) {
            return;
        }
        m_target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush, thickness);
        brush->Release();
    }

    void text(std::wstring_view value, const Rect& rect, TextRole role, const Color& color,
        TextAlign align = TextAlign::Left, float alphaScale = 1.0f)
    {
        IDWriteTextFormat* format = formatFor(role);
        if (format == nullptr) {
            return;
        }

        ID2D1SolidColorBrush* brush = nullptr;
        if (FAILED(m_target->CreateSolidColorBrush(toD2D(color, alphaScale), &brush))) {
            return;
        }

        const DWRITE_TEXT_ALIGNMENT previous = format->GetTextAlignment();
        format->SetTextAlignment(align == TextAlign::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                : align == TextAlign::Center                 ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                             : DWRITE_TEXT_ALIGNMENT_LEADING);
        m_target->DrawTextW(value.data(), static_cast<UINT32>(value.size()), format,
            D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom), brush);
        format->SetTextAlignment(previous);
        brush->Release();
    }

    float measure(std::wstring_view value, TextRole role) const
    {
        IDWriteTextFormat* format = formatFor(role);
        if (format == nullptr || m_formats.factory == nullptr) {
            return 0.0f;
        }
        IDWriteTextLayout* layout = nullptr;
        if (FAILED(m_formats.factory->CreateTextLayout(value.data(), static_cast<UINT32>(value.size()), format,
                4096.0f, 200.0f, &layout))) {
            return 0.0f;
        }
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        layout->Release();
        return metrics.widthIncludingTrailingWhitespace;
    }

    float scale() const { return m_scale; }
    float opacity() const { return m_opacity; }
    const Theme& theme() const { return m_theme; }

private:
    IDWriteTextFormat* formatFor(TextRole role) const
    {
        switch (role) {
        case TextRole::Display:
        case TextRole::Value:
            return m_formats.value;
        case TextRole::Heading:
        case TextRole::Body:
            return m_formats.key;
        case TextRole::Mono:
            return m_formats.mono;
        default:
            return m_formats.smallText;
        }
    }

    ID2D1RenderTarget* m_target = nullptr;
    const Theme& m_theme;
    OverlayFormats m_formats;
    float m_scale = 1.0f;
    float m_opacity = 1.0f;
};

struct StyleSpec {
    float width = 200.0f;
    float height = 96.0f;
};

// Сколько строк реально рисуем: выключенные в «Составе» не занимают место.
std::size_t visibleRows(const OverlayText& text)
{
    std::size_t rows = 0;
    for (std::size_t index = 0; index < kLabelCount; ++index) {
        if (!text.visible[index]) {
            continue;
        }
        if (index == static_cast<std::size_t>(LabelField::Vram) && text.vram.empty()) {
            continue;
        }
        ++rows;
    }
    return rows;
}

bool rowShown(const OverlayText& text, std::size_t index)
{
    if (!text.visible[index]) {
        return false;
    }
    return !(index == static_cast<std::size_t>(LabelField::Vram) && text.vram.empty());
}

std::wstring inlineText(const OverlayText& text)
{
    std::wstring line = text.labels[0] + L" " + text.fps + L" fps";
    const std::pair<std::wstring, std::wstring> rows[] = {
        { text.labels[1], text.cpu },
        { text.labels[2], text.gpu },
        { text.labels[3], text.ram },
        { text.labels[4], text.vram },
    };
    for (std::size_t index = 0; index < 4; ++index) {
        if (!rowShown(text, index + 1)) {
            continue;
        }
        line += L"   " + rows[index].first + L" " + rows[index].second;
    }
    return line;
}

StyleSpec styleSpec(const OverlaySettings& settings, const OverlayText& text, float lineWidth)
{
    const std::size_t rows = visibleRows(text);
    StyleSpec spec;
    if (settings.layout == OverlayLayout::Line && settings.style != OverlayStyle::Capsule) {
        const float height = settings.style == OverlayStyle::Minimal ? 24.0f : 34.0f;
        return StyleSpec{ std::max(lineWidth + 24.0f, 120.0f), height };
    }
    switch (settings.style) {
    case OverlayStyle::Glass: {
        // Крупное число кадров плюс строки со значениями; без FPS карточка ниже.
        const std::size_t listed = rowShown(text, 0) && rows > 0 ? rows - 1 : rows;
        spec = { 208.0f, (rowShown(text, 0) ? 74.0f : 10.0f) + static_cast<float>(listed) * 18.0f + 6.0f };
        break;
    }
    case OverlayStyle::Neon:
        spec = { 226.0f, 104.0f };
        break;
    case OverlayStyle::Capsule:
        spec = { 156.0f, std::max(static_cast<float>(rows) * 30.0f - 6.0f, 24.0f) };
        break;
    case OverlayStyle::Terminal:
        spec = { 204.0f, 30.0f + static_cast<float>(rows) * 18.0f };
        break;
    case OverlayStyle::Minimal:
        spec = { 168.0f, 4.0f + static_cast<float>(rows) * 18.0f };
        break;
    }
    return spec;
}

void drawGlass(Painter& painter, const OverlayText& text, const Rect& frame)
{
    const Theme& theme = painter.theme();
    const float scale = painter.scale();

    // Стекло: полупрозрачная плитка с крупным числом кадров и списком показателей.
    painter.fillRound(frame, 12.0f * scale, theme.panel, painter.opacity());
    painter.fillRound(Rect{ frame.left + 1.0f, frame.top + 1.0f, frame.right - 1.0f, frame.top + 2.0f * scale },
        1.0f, theme.text, 0.06f);
    painter.strokeRound(frame, 12.0f * scale, theme.line, 1.0f, 0.9f);

    const float left = frame.left + 14.0f * scale;
    const float right = frame.right - 14.0f * scale;

    if (rowShown(text, 0)) {
        painter.text(text.labels[0], Rect{ left, frame.top + 8.0f * scale, left + 120.0f * scale,
                                           frame.top + 22.0f * scale },
            TextRole::Tiny, theme.textDim);
        painter.text(text.fps, Rect{ left, frame.top + 20.0f * scale, left + 130.0f * scale,
                                      frame.top + 58.0f * scale },
            TextRole::Display, theme.text);
        painter.text(L"fps", Rect{ left + 58.0f * scale, frame.top + 44.0f * scale, left + 92.0f * scale,
                                   frame.top + 58.0f * scale },
            TextRole::Tiny, theme.textFaint);

        painter.text(L"1% low", Rect{ right - 96.0f * scale, frame.top + 10.0f * scale, right,
                                      frame.top + 24.0f * scale },
            TextRole::Tiny, theme.textFaint, TextAlign::Right);
        painter.text(text.low1, Rect{ right - 96.0f * scale, frame.top + 24.0f * scale, right,
                                      frame.top + 46.0f * scale },
            TextRole::Body, theme.accent, TextAlign::Right);
        painter.line(left, frame.top + 62.0f * scale, right, frame.top + 62.0f * scale, theme.line, 1.0f, 0.8f);
    }

    const std::pair<std::wstring, std::wstring> rows[] = {
        { text.labels[1], text.cpu },
        { text.labels[2], text.gpu },
        { text.labels[3], text.ram },
        { text.labels[4], text.vram },
    };

    float y = frame.top + (rowShown(text, 0) ? 68.0f : 8.0f) * scale;
    for (std::size_t index = 0; index < 4; ++index) {
        if (!rowShown(text, index + 1)) {
            continue;
        }
        painter.text(rows[index].first, Rect{ left, y, left + 96.0f * scale, y + kRowHeight * scale },
            TextRole::Tiny, theme.textDim);
        painter.text(rows[index].second, Rect{ left + 96.0f * scale, y, right, y + kRowHeight * scale },
            TextRole::Small, theme.text, TextAlign::Right);
        y += 18.0f * scale;
    }
}

void drawNeon(Painter& painter, const OverlayText& text, const Rect& frame)
{
    const Theme& theme = painter.theme();
    const float scale = painter.scale();
    painter.fillRound(frame, 12.0f * scale, theme.background, painter.opacity() * 0.55f);
    painter.strokeRound(Rect{ frame.left - 2.0f, frame.top - 2.0f, frame.right + 2.0f, frame.bottom + 2.0f },
        14.0f * scale, theme.accent, 2.0f, 0.18f);
    painter.strokeRound(frame, 12.0f * scale, theme.accent, 1.6f, 0.9f);

    const float left = frame.left + 14.0f * scale;
    const float right = frame.right - 14.0f * scale;

    painter.text(text.labels[0], Rect{ left, frame.top + 10.0f * scale, left + 90.0f * scale,
                                        frame.top + 24.0f * scale },
        TextRole::Tiny, theme.accent, TextAlign::Left, 0.85f);
    painter.text(text.fps, Rect{ left, frame.top + 20.0f * scale, left + 130.0f * scale,
                                  frame.top + 62.0f * scale },
        TextRole::Display, theme.text);

    painter.text(L"1% LOW", Rect{ right - 92.0f * scale, frame.top + 12.0f * scale, right,
                                  frame.top + 26.0f * scale },
        TextRole::Tiny, theme.textFaint, TextAlign::Right);
    painter.text(text.low1, Rect{ right - 92.0f * scale, frame.top + 24.0f * scale, right,
                                  frame.top + 46.0f * scale },
        TextRole::Body, theme.accent, TextAlign::Right);

    painter.line(left, frame.top + 58.0f * scale, right, frame.top + 58.0f * scale, theme.accent, 1.0f, 0.25f);

    const std::pair<std::wstring, std::wstring> rows[] = {
        { text.labels[1], text.cpu },
        { text.labels[2], text.gpu },
        { text.labels[3], text.ram },
    };

    float x = left;
    const float columnWidth = (right - left) / 3.0f;
    for (std::size_t index = 0; index < 3; ++index) {
        if (!rowShown(text, index + 1)) {
            continue;
        }
        painter.text(rows[index].first, Rect{ x, frame.top + 66.0f * scale, x + columnWidth - 6.0f * scale,
                                              frame.top + 80.0f * scale },
            TextRole::Tiny, theme.textFaint);
        painter.text(rows[index].second, Rect{ x, frame.top + 78.0f * scale, x + columnWidth - 6.0f * scale,
                                                frame.top + 98.0f * scale },
            TextRole::Small, theme.text);
        x += columnWidth;
    }
}

void drawCapsule(Painter& painter, const OverlayText& text, const Rect& frame)
{
    const Theme& theme = painter.theme();
    const float scale = painter.scale();
    const std::wstring values[] = { text.fps, text.cpu, text.gpu, text.ram, text.vram };

    float y = frame.top;
    for (std::size_t index = 0; index < kLabelCount; ++index) {
        if (!rowShown(text, index)) {
            continue;
        }
        const Rect pill{ frame.left, y, frame.right, y + 24.0f * scale };
        const bool isFps = index == 0;
        painter.fillRound(pill, 12.0f * scale, isFps ? theme.accent : theme.panel, isFps ? 1.0f : painter.opacity());
        painter.text(text.labels[index], Rect{ pill.left + 12.0f * scale, pill.top, pill.left + 84.0f * scale,
                                               pill.bottom },
            TextRole::Tiny, isFps ? theme.background : theme.textDim);
        painter.text(values[index], Rect{ pill.left + 84.0f * scale, pill.top, pill.right - 12.0f * scale,
                                          pill.bottom },
            TextRole::Small, isFps ? theme.background : theme.text, TextAlign::Right);
        y += 30.0f * scale;
    }
}

void drawTerminal(Painter& painter, const OverlayText& text, const Rect& frame)
{
    const Theme& theme = painter.theme();
    const float scale = painter.scale();
    painter.fillRound(frame, 2.0f * scale, theme.background, 0.96f);
    painter.strokeRound(frame, 2.0f * scale, theme.line, 1.0f, 0.55f);

    const float left = frame.left + 10.0f * scale;
    const float right = frame.right - 10.0f * scale;

    painter.text(L"YAWAFPSHUB", Rect{ left, frame.top + 5.0f * scale, right, frame.top + 19.0f * scale },
        TextRole::Mono, theme.accent, TextAlign::Left, 0.85f);
    painter.line(left, frame.top + 21.0f * scale, right, frame.top + 21.0f * scale, theme.accent, 1.0f, 0.3f);

    const std::pair<std::wstring, std::wstring> rows[] = {
        { text.labels[0], text.fps },
        { text.labels[1], text.cpu },
        { text.labels[2], text.gpu },
        { text.labels[3], text.ram },
        { text.labels[4], text.vram },
    };

    float y = frame.top + 25.0f * scale;
    for (std::size_t index = 0; index < kLabelCount; ++index) {
        if (!rowShown(text, index)) {
            continue;
        }
        painter.text(L"> " + upperCase(rows[index].first),
            Rect{ left, y, left + 110.0f * scale, y + kRowHeight * scale }, TextRole::Mono, theme.textDim);
        painter.text(upperCase(rows[index].second),
            Rect{ left + 110.0f * scale, y, right, y + kRowHeight * scale }, TextRole::Mono, theme.text,
            TextAlign::Right);
        y += 18.0f * scale;
    }
}

void drawMinimal(Painter& painter, const OverlayText& text, const Rect& frame)
{
    const Theme& theme = painter.theme();
    const float scale = painter.scale();
    const std::wstring values[] = { text.fps, text.cpu, text.gpu, text.ram, text.vram };

    float y = frame.top;
    for (std::size_t index = 0; index < kLabelCount; ++index) {
        if (!rowShown(text, index)) {
            continue;
        }
        painter.text(text.labels[index], Rect{ frame.left, y, frame.left + 60.0f * scale, y + kRowHeight * scale },
            TextRole::Tiny, theme.textFaint, TextAlign::Left, 0.9f);
        painter.text(values[index], Rect{ frame.left + 60.0f * scale, y, frame.right, y + kRowHeight * scale },
            TextRole::Small, index == 0 ? theme.accent : theme.text, TextAlign::Right);
        y += 18.0f * scale;
    }
}

void drawInline(Painter& painter, const OverlaySettings& settings, const OverlayText& text, const Rect& frame)
{
    const Theme& theme = painter.theme();
    const float scale = painter.scale();
    const std::wstring line = inlineText(text);
    const Rect inner = Rect{ frame.left + 12.0f * scale, frame.top, frame.right - 12.0f * scale, frame.bottom };
    const TextRole role = settings.style == OverlayStyle::Terminal ? TextRole::Mono : TextRole::Small;

    switch (settings.style) {
    case OverlayStyle::Glass:
        painter.fillRound(frame, 10.0f * scale, theme.panel, painter.opacity());
        painter.strokeRound(frame, 10.0f * scale, theme.line, 1.0f, 0.9f);
        break;
    case OverlayStyle::Neon:
        painter.fillRound(frame, 10.0f * scale, theme.background, painter.opacity() * 0.55f);
        painter.strokeRound(frame, 10.0f * scale, theme.accent, 1.4f, 0.9f);
        break;
    case OverlayStyle::Terminal:
        painter.fillRound(frame, 2.0f * scale, theme.background, 0.96f);
        painter.strokeRound(frame, 2.0f * scale, theme.line, 1.0f, 0.55f);
        break;
    case OverlayStyle::Capsule:
    case OverlayStyle::Minimal:
        break;
    }

    painter.text(line, inner, role, settings.style == OverlayStyle::Neon ? theme.accent : theme.text);
}

} // namespace

OverlayText makeOverlayText(const OverlaySettings& settings, const OverlayValues& values)
{
    OverlayText text;
    text.fps = formatNumber(values.fps, 0, std::wstring_view());
    text.low1 = formatNumber(values.low1, 0, std::wstring_view());
    text.cpu = formatNumber(values.cpuLoad, 0, L"%");
    text.cpuTemp = temperatureText(values.cpuTemp, values.hasCpuTemp);
    text.gpu = formatNumber(values.gpuLoad, 0, L"%");
    text.gpuTemp = temperatureText(values.gpuTemp, values.hasGpuTemp);
    text.ram = formatNumber(values.ramUsed, 1, L"ГБ");
    text.vram = values.hasVram ? formatNumber(values.vramUsed, 1, L"ГБ") : std::wstring();

    text.visible = settings.visible;
    for (std::size_t index = 0; index < kLabelCount; ++index) {
        text.labels[index] = wideLabel(settings.labels[index]);
        // Пустая подпись означает «не показывать»: пользователь стёр название целиком.
        if (text.labels[index].empty()) {
            text.visible[index] = false;
        }
    }
    return text;
}

Rect paintOverlay(ID2D1RenderTarget* target, const OverlaySettings& settings, const Theme& theme,
    const OverlayText& text, const Rect& bounds, float scale, const OverlayFormats& formats)
{
    if (target == nullptr) {
        return Rect{};
    }

    const float opacity = static_cast<float>(std::clamp(settings.opacityPercent, 0, 100)) / 100.0f;
    Painter measurer(target, theme, formats, scale, opacity);
    const float lineWidth = measurer.measure(inlineText(text),
        settings.style == OverlayStyle::Terminal ? TextRole::Mono : TextRole::Small);
    const StyleSpec spec = styleSpec(settings, text, lineWidth);
    const float width = spec.width * scale;
    const float height = spec.height * scale;

    const float margin = kMargin * scale;
    const bool isLeft = settings.position == OverlayPosition::TopLeft
        || settings.position == OverlayPosition::BottomLeft;
    const bool isTop = settings.position == OverlayPosition::TopLeft
        || settings.position == OverlayPosition::TopRight;

    float left = isLeft ? bounds.left + margin + static_cast<float>(settings.offsetX) * scale
                        : bounds.right - width - margin + static_cast<float>(settings.offsetX) * scale;
    float top = isTop ? bounds.top + margin + static_cast<float>(settings.offsetY) * scale
                      : bounds.bottom - height - margin + static_cast<float>(settings.offsetY) * scale;

    left = std::clamp(left, bounds.left, std::max(bounds.left, bounds.right - width));
    top = std::clamp(top, bounds.top, std::max(bounds.top, bounds.bottom - height));

    const Rect frame{ left, top, left + width, top + height };
    Painter painter(target, theme, formats, scale, opacity);

    if (settings.layout == OverlayLayout::Line && settings.style != OverlayStyle::Capsule) {
        drawInline(painter, settings, text, frame);
        return frame;
    }

    switch (settings.style) {
    case OverlayStyle::Glass:
        drawGlass(painter, text, frame);
        break;
    case OverlayStyle::Neon:
        drawNeon(painter, text, frame);
        break;
    case OverlayStyle::Capsule:
        drawCapsule(painter, text, frame);
        break;
    case OverlayStyle::Terminal:
        drawTerminal(painter, text, frame);
        break;
    case OverlayStyle::Minimal:
        drawMinimal(painter, text, frame);
        break;
    }

    return frame;
}

} // namespace yawa

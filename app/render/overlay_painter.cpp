#include "render/overlay_painter.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cwctype>

namespace yawa {
namespace {

constexpr float kMargin = 18.0f;
constexpr float kRowHeight = 16.0f;

D2D1_COLOR_F toD2D(const Color& color, float alphaScale = 1.0f)
{
    return D2D1::ColorF(color.r, color.g, color.b, color.a * alphaScale);
}

// Цвета пресетов берутся из палитры репозитория Fpsmon2.0: один набор на все стили,
// чтобы оверлей в приложении и в демо выглядел одинаково.
Color styleColor(std::uint32_t rgb, float alpha = 1.0f)
{
    return fromRgba(0xFF000000u | rgb).withAlpha(alpha);
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

// Межбуквенный интервал DirectWrite для DrawTextW не выставляет, а пресеты Terminal,
// Cyber и Retro различаются именно трекингом. Расставляем тонкие пробелы между знаками.
std::wstring tracked(std::wstring_view value, float letterSpacing)
{
    if (letterSpacing < 0.10f) {
        return std::wstring(value);
    }
    std::wstring result;
    result.reserve(value.size() * 2);
    for (wchar_t symbol : value) {
        result.push_back(symbol);
        if (symbol != L' ') {
            result.push_back(L'\u2009');
        }
    }
    return result;
}

// Копия формата с другим размером шрифта. Раньше масштаб двигал только рамку:
// текст оставался прежним, и оверлей выглядел сломанным.
IDWriteTextFormat* scaledFormat(IDWriteFactory* factory, IDWriteTextFormat* base, float factor)
{
    if (factory == nullptr || base == nullptr || factor <= 0.0f) {
        return nullptr;
    }

    const UINT32 length = base->GetFontFamilyNameLength();
    std::wstring family(length + 1, L'\0');
    if (FAILED(base->GetFontFamilyName(family.data(), length + 1))) {
        return nullptr;
    }
    family.resize(length);

    IDWriteTextFormat* format = nullptr;
    if (FAILED(factory->CreateTextFormat(family.c_str(), nullptr, base->GetFontWeight(), base->GetFontStyle(),
            base->GetFontStretch(), base->GetFontSize() * factor, L"ru-ru", &format))) {
        return nullptr;
    }

    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return format;
}

struct OverlayChrome {
    bool background = true;
    bool border = true;
    bool shadow = true;
    // -1 — радиус из пресета.
    float radius = -1.0f;
};

class Painter {
public:
    Painter(ID2D1RenderTarget* target, const Theme& theme, const OverlayFormats& formats, float scale, float opacity,
        const OverlayChrome& chrome)
        : m_target(target)
        , m_theme(theme)
        , m_formats(formats)
        , m_scale(scale)
        , m_opacity(opacity)
        , m_chrome(chrome)
    {
    }

    // Подложка оверлея: уважает переключатель «Фон» и пользовательский радиус углов.
    void plate(const Rect& rect, float radius, const Color& color, float alphaScale = 1.0f)
    {
        if (!m_chrome.background) {
            return;
        }
        fillRound(rect, chromeRadius(radius), color, alphaScale);
    }

    void outline(const Rect& rect, float radius, const Color& color, float thickness, float alphaScale = 1.0f)
    {
        if (!m_chrome.border) {
            return;
        }
        strokeRound(rect, chromeRadius(radius), color, thickness, alphaScale);
    }

    void dropShadow(const Rect& rect, float radius, const Color& color, float alphaScale)
    {
        if (!m_chrome.shadow) {
            return;
        }
        fillRound(rect, chromeRadius(radius), color, alphaScale);
    }

    float textSize() const { return m_scale; }

    void fillRect(const Rect& rect, const Color& color, float alphaScale = 1.0f)
    {
        ID2D1SolidColorBrush* brush = nullptr;
        if (FAILED(m_target->CreateSolidColorBrush(toD2D(color, alphaScale), &brush))) {
            return;
        }
        m_target->FillRectangle(D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom), brush);
        brush->Release();
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

    void fillCircle(float centerX, float centerY, float radius, const Color& color, float alphaScale = 1.0f)
    {
        fillRound(Rect{ centerX - radius, centerY - radius, centerX + radius, centerY + radius }, radius, color,
            alphaScale);
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
    float chromeRadius(float presetRadius) const
    {
        return m_chrome.radius >= 0.0f ? m_chrome.radius * m_scale : presetRadius;
    }

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
    OverlayChrome m_chrome;
};

// Полный визуальный набор стиля: палитра, форма, типографика. Раньше стили отличались
// только подложкой, теперь меняется всё сразу — как в пресетах Fpsmon2.0.
struct StyleLook {
    Color background;
    Color border;
    Color label;
    Color accent;
    Color value;
    std::array<Color, kLabelCount> palette{};
    float radius = 8.0f;
    float padding = 10.0f;
    float rowGap = 2.0f;
    float letterSpacing = 0.0f;
    bool borderVisible = true;
    bool uppercase = false;
    bool separators = false;
    bool mono = false;
    bool shadow = false;
};

StyleLook lookFor(OverlayStyle style)
{
    StyleLook look;
    switch (style) {
    case OverlayStyle::Neon:
        look.background = styleColor(0x05070Bu);
        look.border = styleColor(0x94A3B8u);
        look.label = styleColor(0xCBD5E1u);
        look.accent = styleColor(0x00FF88u);
        look.value = styleColor(0xF1FFF7u);
        look.palette = { styleColor(0x22FF66u), styleColor(0x4DB3FFu), styleColor(0xB9A7FFu),
            styleColor(0xFFCC4Du), styleColor(0x6EF3D6u) };
        look.radius = 10.0f;
        look.padding = 10.0f;
        look.rowGap = 2.0f;
        look.letterSpacing = 0.06f;
        look.uppercase = true;
        look.separators = true;
        look.mono = true;
        look.shadow = true;
        break;
    case OverlayStyle::Rtss:
        // Как RivaTuner: без подложки и рамки, крупный моноширинный, только цифры и подписи.
        look.background = styleColor(0x000000u, 0.0f);
        look.border = styleColor(0x000000u, 0.0f);
        look.label = styleColor(0x9FE8B4u);
        look.accent = styleColor(0x44FF77u);
        look.value = styleColor(0x44FF77u);
        look.palette = { styleColor(0x44FF77u), styleColor(0x44FF77u), styleColor(0x44FF77u),
            styleColor(0x44FF77u), styleColor(0x44FF77u) };
        look.radius = 0.0f;
        look.padding = 4.0f;
        look.rowGap = 0.0f;
        look.letterSpacing = 0.02f;
        look.borderVisible = false;
        look.uppercase = true;
        look.mono = true;
        break;
    case OverlayStyle::Frosted:
        // Светлое стекло: подложка почти непрозрачная, поэтому и текст тёмный.
        look.background = styleColor(0xF3F6FBu);
        look.border = styleColor(0xFFFFFFu);
        look.label = styleColor(0x33415Cu);
        look.accent = styleColor(0x0B1220u);
        look.value = styleColor(0x0B1220u);
        look.palette = { styleColor(0x0B1220u), styleColor(0x123E63u), styleColor(0x3B2A5Cu),
            styleColor(0x5A3E12u), styleColor(0x0F3B36u) };
        look.radius = 14.0f;
        look.padding = 12.0f;
        look.rowGap = 4.0f;
        look.letterSpacing = 0.01f;
        look.shadow = true;
        break;
    case OverlayStyle::Terminal:
        look.background = styleColor(0x000000u);
        look.border = styleColor(0xFFB03Au);
        look.label = styleColor(0x9A6B22u);
        look.accent = styleColor(0xFFB03Au);
        look.value = styleColor(0xFFC76Bu);
        look.palette = { styleColor(0xFFB03Au), styleColor(0xFFC76Bu), styleColor(0xFFA021u),
            styleColor(0xE09020u), styleColor(0xD98020u) };
        look.radius = 0.0f;
        look.padding = 8.0f;
        look.rowGap = 1.0f;
        look.letterSpacing = 0.12f;
        look.uppercase = true;
        look.separators = true;
        look.mono = true;
        break;
    case OverlayStyle::Cyber:
        look.background = styleColor(0x120418u);
        look.border = styleColor(0xFF2BD1u);
        look.label = styleColor(0xC58AE0u);
        look.accent = styleColor(0xFF2BD1u);
        look.value = styleColor(0xFFE8FBu);
        look.palette = { styleColor(0xFF2BD1u), styleColor(0x00E5FFu), styleColor(0xB47AFFu),
            styleColor(0xFFD23Fu), styleColor(0x6EF3D6u) };
        look.radius = 4.0f;
        look.padding = 11.0f;
        look.rowGap = 3.0f;
        look.letterSpacing = 0.18f;
        look.uppercase = true;
        look.separators = true;
        look.shadow = true;
        break;
    case OverlayStyle::Mono:
        look.background = styleColor(0x000000u, 0.28f);
        look.border = styleColor(0xFFFFFFu, 0.0f);
        look.label = styleColor(0x8F98A6u);
        look.accent = styleColor(0xFFFFFFu);
        look.value = styleColor(0xFFFFFFu);
        look.palette = { styleColor(0xFFFFFFu), styleColor(0xE2E8F0u), styleColor(0xE2E8F0u),
            styleColor(0xE2E8F0u), styleColor(0xE2E8F0u) };
        look.radius = 8.0f;
        look.padding = 8.0f;
        look.rowGap = 2.0f;
        look.borderVisible = false;
        break;
    case OverlayStyle::Esport:
        look.background = styleColor(0x070B12u);
        look.border = styleColor(0x1F2B3Du);
        look.label = styleColor(0x64748Bu);
        look.accent = styleColor(0x38BDF8u);
        look.value = styleColor(0xE2E8F0u);
        look.palette = { styleColor(0x38BDF8u), styleColor(0xF472B6u), styleColor(0xA78BFAu),
            styleColor(0xFACC15u), styleColor(0x4ADE80u) };
        look.radius = 6.0f;
        look.padding = 7.0f;
        look.rowGap = 0.0f;
        look.letterSpacing = 0.08f;
        look.uppercase = true;
        look.separators = true;
        look.mono = true;
        look.shadow = true;
        break;
    case OverlayStyle::Retro:
        look.background = styleColor(0x021206u);
        look.border = styleColor(0x2DFF6Au);
        look.label = styleColor(0x1F8F3Fu);
        look.accent = styleColor(0x2DFF6Au);
        look.value = styleColor(0x88FFAAu);
        look.palette = { styleColor(0x2DFF6Au), styleColor(0x55FF88u), styleColor(0x88FFAAu),
            styleColor(0xAAFFBBu), styleColor(0x2DFF6Au) };
        look.radius = 3.0f;
        look.padding = 10.0f;
        look.rowGap = 2.0f;
        look.letterSpacing = 0.14f;
        look.uppercase = true;
        look.separators = true;
        look.mono = true;
        break;
    }
    return look;
}

struct StyleSpec {
    float width = 200.0f;
    float height = 96.0f;
};

bool rowShown(const OverlayText& text, std::size_t index)
{
    if (!text.visible[index]) {
        return false;
    }
    return !(index == static_cast<std::size_t>(LabelField::Vram) && text.vram.empty());
}

// Сколько строк реально рисуем: выключенные в «Составе» не занимают место.
std::size_t visibleRows(const OverlayText& text)
{
    std::size_t rows = 0;
    for (std::size_t index = 0; index < kLabelCount; ++index) {
        if (rowShown(text, index)) {
            ++rows;
        }
    }
    return rows;
}

std::wstring inlineText(const OverlayText& text)
{
    const std::wstring values[] = { text.fps, text.cpu, text.gpu, text.ram, text.vram };
    std::wstring line;
    for (std::size_t index = 0; index < kLabelCount; ++index) {
        if (!rowShown(text, index)) {
            continue;
        }
        if (!line.empty()) {
            line += L"   ";
        }
        line += text.labels[index] + L" " + values[index];
        if (index == 0) {
            line += L" fps";
        }
    }
    return line;
}

std::array<std::wstring, kLabelCount> rowValues(const OverlayText& text)
{
    std::array<std::wstring, kLabelCount> values{ text.fps, text.cpu, text.gpu, text.ram, text.vram };
    // Температура идёт тем же значением через разделитель: в оверлее важно видеть
    // нагрев рядом с загрузкой, не раздувая число строк.
    if (!text.cpuTemp.empty() && text.cpuTemp != L"—") {
        values[1] += L" · " + text.cpuTemp;
    }
    if (!text.gpuTemp.empty() && text.gpuTemp != L"—") {
        values[2] += L" · " + text.gpuTemp;
    }
    return values;
}

std::array<std::wstring, kLabelCount> rowLabels(const OverlayText& text, const StyleLook& look)
{
    std::array<std::wstring, kLabelCount> labels = text.labels;
    if (look.uppercase) {
        for (std::wstring& label : labels) {
            label = upperCase(label);
        }
    }
    for (std::wstring& label : labels) {
        label = tracked(label, look.letterSpacing);
    }
    return labels;
}

StyleSpec styleSpec(const OverlaySettings& settings, const OverlayText& text, float lineWidth)
{
    const std::size_t rows = std::max<std::size_t>(visibleRows(text), 1);
    StyleSpec spec;
    if (settings.layout == OverlayLayout::Line) {
        const float height = settings.style == OverlayStyle::Rtss ? 24.0f
            : settings.style == OverlayStyle::Esport                    ? 32.0f
                                                                       : 30.0f;
        return StyleSpec{ std::max(lineWidth + 28.0f, 140.0f), height };
    }

    switch (settings.style) {
    case OverlayStyle::Neon:
        spec = { 244.0f, 74.0f + static_cast<float>(rows > 1 ? rows - 1 : 0) * 19.0f };
        break;
    case OverlayStyle::Rtss:
        spec = { 196.0f, 6.0f + static_cast<float>(rows) * 19.0f };
        break;
    case OverlayStyle::Frosted:
        spec = { 224.0f, 82.0f + static_cast<float>(rows > 1 ? rows - 1 : 0) * 20.0f };
        break;
    case OverlayStyle::Terminal:
        spec = { 214.0f, 34.0f + static_cast<float>(rows) * 18.0f };
        break;
    case OverlayStyle::Cyber:
        spec = { 232.0f, 78.0f + static_cast<float>(rows > 1 ? rows - 1 : 0) * 20.0f };
        break;
    case OverlayStyle::Mono:
        spec = { 188.0f, 8.0f + static_cast<float>(rows) * 18.0f };
        break;
    case OverlayStyle::Esport:
        // Одна плотная полоса: строка FPS крупно и остальные показатели следом.
        spec = { 320.0f, 44.0f };
        break;
    case OverlayStyle::Retro:
        spec = { 226.0f, 34.0f + static_cast<float>(rows) * 19.0f };
        break;
    }
    return spec;
}

void drawShadow(Painter& painter, const Rect& frame, float radius)
{
    const float scale = painter.scale();
    painter.dropShadow(Rect{ frame.left + 2.0f * scale, frame.top + 4.0f * scale, frame.right + 3.0f * scale,
                               frame.bottom + 5.0f * scale },
        radius, styleColor(0x000000u), 0.28f);
}

// Плитки «плат»: крупный FPS и строки показателей. Frosted Glass.
void drawFrosted(Painter& painter, const OverlayText& text, const Rect& frame, const StyleLook& look)
{
    const float scale = painter.scale();
    const std::array<std::wstring, kLabelCount> labels = rowLabels(text, look);
    const std::array<std::wstring, kLabelCount> values = rowValues(text);

    drawShadow(painter, frame, look.radius * scale);
    painter.plate(frame, look.radius * scale, look.background, 0.35f + 0.6f * painter.opacity());
    painter.outline(frame, look.radius * scale, look.label, 1.0f, 0.25f);

    const float left = frame.left + look.padding * scale;
    const float right = frame.right - look.padding * scale;

    if (rowShown(text, 0)) {
        const Rect chip{ left, frame.top + 8.0f * scale, left + 66.0f * scale, frame.top + 26.0f * scale };
        painter.fillRound(chip, 9.0f * scale, look.label, 0.12f);
        painter.text(labels[0], chip, TextRole::Tiny, look.label, TextAlign::Center);
        painter.text(values[0], Rect{ left - 2.0f * scale, frame.top + 24.0f * scale, left + 120.0f * scale,
                                      frame.top + 62.0f * scale },
            TextRole::Display, look.value);
        painter.text(L"fps", Rect{ left + 62.0f * scale, frame.top + 46.0f * scale, left + 96.0f * scale,
                                   frame.top + 60.0f * scale },
            TextRole::Tiny, look.label);
        painter.text(L"1% LOW", Rect{ right - 96.0f * scale, frame.top + 12.0f * scale, right,
                                      frame.top + 26.0f * scale },
            TextRole::Tiny, look.label, TextAlign::Right);
        painter.text(text.low1, Rect{ right - 96.0f * scale, frame.top + 24.0f * scale, right,
                                      frame.top + 52.0f * scale },
            TextRole::Body, look.palette[0], TextAlign::Right);
        painter.line(left, frame.top + 64.0f * scale, right, frame.top + 64.0f * scale, look.border, 1.0f, 0.22f);
    }

    float y = frame.top + (rowShown(text, 0) ? 70.0f : 8.0f) * scale;
    for (std::size_t index = 1; index < kLabelCount; ++index) {
        if (!rowShown(text, index)) {
            continue;
        }
        painter.text(labels[index], Rect{ left, y, left + 96.0f * scale, y + kRowHeight * scale }, TextRole::Tiny,
            look.label);
        painter.text(values[index], Rect{ right - 120.0f * scale, y, right, y + kRowHeight * scale }, TextRole::Small,
            look.palette[index], TextAlign::Right);
        y += (18.0f + look.rowGap) * scale;
    }
}

// Neon Grid: неоновый акцент, сетка показателей с разделителями и цветом на устройство.
void drawNeon(Painter& painter, const OverlayText& text, const Rect& frame, const StyleLook& look)
{
    const float scale = painter.scale();
    const std::array<std::wstring, kLabelCount> labels = rowLabels(text, look);
    const std::array<std::wstring, kLabelCount> values = rowValues(text);

    drawShadow(painter, frame, look.radius * scale);
    painter.plate(frame, look.radius * scale, look.background, 0.72f * painter.opacity() + 0.18f);
    painter.outline(Rect{ frame.left - 2.0f * scale, frame.top - 2.0f * scale, frame.right + 2.0f * scale,
                            frame.bottom + 2.0f * scale },
        (look.radius + 2.0f) * scale, look.accent, 2.0f, 0.18f);
    painter.outline(frame, look.radius * scale, look.border, 1.0f, 0.35f);

    const float left = frame.left + look.padding * scale;
    const float right = frame.right - look.padding * scale;

    if (rowShown(text, 0)) {
        painter.text(labels[0], Rect{ left, frame.top + 8.0f * scale, left + 120.0f * scale,
                                      frame.top + 22.0f * scale },
            TextRole::Tiny, look.accent);
        painter.text(values[0], Rect{ left, frame.top + 20.0f * scale, left + 118.0f * scale,
                                      frame.top + 60.0f * scale },
            TextRole::Display, look.value);
        painter.text(L"FPS", Rect{ left + 84.0f * scale, frame.top + 32.0f * scale, left + 118.0f * scale,
                                   frame.top + 46.0f * scale },
            TextRole::Tiny, look.accent);

        painter.text(L"1% LOW", Rect{ right - 92.0f * scale, frame.top + 10.0f * scale, right,
                                      frame.top + 24.0f * scale },
            TextRole::Tiny, look.label, TextAlign::Right);
        painter.text(text.low1, Rect{ right - 92.0f * scale, frame.top + 22.0f * scale, right,
                                      frame.top + 48.0f * scale },
            TextRole::Body, look.accent, TextAlign::Right);
        painter.line(left, frame.top + 60.0f * scale, right, frame.top + 60.0f * scale, look.accent, 1.0f, 0.35f);
    }

    float y = frame.top + (rowShown(text, 0) ? 64.0f : 8.0f) * scale;
    for (std::size_t index = 1; index < kLabelCount; ++index) {
        if (!rowShown(text, index)) {
            continue;
        }
        painter.fillCircle(left + 2.0f * scale, y + 8.0f * scale, 2.0f * scale, look.palette[index]);
        painter.text(labels[index], Rect{ left + 10.0f * scale, y, left + 110.0f * scale, y + kRowHeight * scale },
            TextRole::Tiny, look.label);
        painter.text(values[index], Rect{ right - 130.0f * scale, y, right, y + kRowHeight * scale },
            TextRole::Small, look.palette[index], TextAlign::Right);
        y += (18.0f + look.rowGap) * scale;
    }
}

// RTSS Classic: без фона, каждая строка «подпись значение», моноширинный.
void drawRtss(Painter& painter, const OverlayText& text, const Rect& frame, const StyleLook& look)
{
    const float scale = painter.scale();
    const std::array<std::wstring, kLabelCount> labels = rowLabels(text, look);
    const std::array<std::wstring, kLabelCount> values = rowValues(text);

    painter.plate(frame, look.radius * scale, look.background, 0.75f);
    painter.outline(frame, look.radius * scale, look.border, 1.0f, 0.6f);

    float y = frame.top + 2.0f * scale;
    for (std::size_t index = 0; index < kLabelCount; ++index) {
        if (!rowShown(text, index)) {
            continue;
        }
        painter.text(labels[index], Rect{ frame.left, y, frame.left + 84.0f * scale, y + kRowHeight * scale },
            TextRole::Mono, look.label);
        painter.text(values[index] + (index == 0 ? L" fps" : L""),
            Rect{ frame.left + 86.0f * scale, y, frame.right, y + kRowHeight * scale }, TextRole::Mono, look.value);
        y += 19.0f * scale;
    }
}

// Terminal: чёрный бокс, янтарная рамка, строки с приглашением «>».
void drawTerminal(Painter& painter, const OverlayText& text, const Rect& frame, const StyleLook& look)
{
    const float scale = painter.scale();
    const std::array<std::wstring, kLabelCount> labels = rowLabels(text, look);
    const std::array<std::wstring, kLabelCount> values = rowValues(text);

    painter.plate(frame, look.radius * scale, look.background, 0.86f * painter.opacity() + 0.12f);
    painter.outline(frame, look.radius * scale, look.border, 1.0f, 0.9f);

    const float left = frame.left + look.padding * scale;
    const float right = frame.right - look.padding * scale;

    painter.text(tracked(L"YAWAFPSHUB", look.letterSpacing),
        Rect{ left, frame.top + 6.0f * scale, right, frame.top + 20.0f * scale }, TextRole::Mono, look.accent);
    painter.line(left, frame.top + 24.0f * scale, right, frame.top + 24.0f * scale, look.accent, 1.0f, 0.35f);

    float y = frame.top + 28.0f * scale;
    for (std::size_t index = 0; index < kLabelCount; ++index) {
        if (!rowShown(text, index)) {
            continue;
        }
        painter.text(L"> " + labels[index], Rect{ left, y, left + 118.0f * scale, y + kRowHeight * scale },
            TextRole::Mono, look.label);
        painter.text(tracked(values[index], look.letterSpacing),
            Rect{ left + 120.0f * scale, y, right, y + kRowHeight * scale }, TextRole::Mono, look.palette[index],
            TextAlign::Right);
        y += (18.0f + look.rowGap) * scale;
    }
}

// Cyber Magenta: угловые скобки, широкий трекинг, пурпурная подсветка.
void drawCyber(Painter& painter, const OverlayText& text, const Rect& frame, const StyleLook& look)
{
    const float scale = painter.scale();
    const std::array<std::wstring, kLabelCount> labels = rowLabels(text, look);
    const std::array<std::wstring, kLabelCount> values = rowValues(text);

    drawShadow(painter, frame, look.radius * scale);
    painter.plate(frame, look.radius * scale, look.background, 0.78f * painter.opacity() + 0.14f);
    painter.outline(frame, look.radius * scale, look.border, 1.2f, 0.85f);

    const float corner = 12.0f * scale;
    const float inset = 3.0f * scale;
    const Rect brackets{ frame.left + inset, frame.top + inset, frame.right - inset, frame.bottom - inset };
    painter.line(brackets.left, brackets.top, brackets.left + corner, brackets.top, look.accent, 2.0f);
    painter.line(brackets.left, brackets.top, brackets.left, brackets.top + corner, look.accent, 2.0f);
    painter.line(brackets.right - corner, brackets.bottom, brackets.right, brackets.bottom, look.accent, 2.0f);
    painter.line(brackets.right, brackets.bottom - corner, brackets.right, brackets.bottom, look.accent, 2.0f);

    const float left = frame.left + look.padding * scale;
    const float right = frame.right - look.padding * scale;

    if (rowShown(text, 0)) {
        painter.text(labels[0], Rect{ left, frame.top + 8.0f * scale, left + 140.0f * scale,
                                      frame.top + 22.0f * scale },
            TextRole::Tiny, look.accent);
        painter.text(values[0], Rect{ left, frame.top + 20.0f * scale, left + 124.0f * scale,
                                      frame.top + 60.0f * scale },
            TextRole::Display, look.value);
        painter.text(tracked(L"1% LOW", look.letterSpacing),
            Rect{ right - 100.0f * scale, frame.top + 12.0f * scale, right, frame.top + 26.0f * scale },
            TextRole::Tiny, look.label, TextAlign::Right);
        painter.text(text.low1, Rect{ right - 100.0f * scale, frame.top + 24.0f * scale, right,
                                      frame.top + 50.0f * scale },
            TextRole::Body, look.palette[0], TextAlign::Right);
        painter.line(left, frame.top + 62.0f * scale, right, frame.top + 62.0f * scale, look.border, 1.0f, 0.35f);
    }

    float y = frame.top + (rowShown(text, 0) ? 68.0f : 10.0f) * scale;
    for (std::size_t index = 1; index < kLabelCount; ++index) {
        if (!rowShown(text, index)) {
            continue;
        }
        painter.text(labels[index], Rect{ left, y, left + 110.0f * scale, y + kRowHeight * scale }, TextRole::Tiny,
            look.label);
        painter.text(values[index], Rect{ right - 150.0f * scale, y, right, y + kRowHeight * scale },
            TextRole::Small, look.palette[index], TextAlign::Right);
        y += (18.0f + look.rowGap) * scale;
    }
}

// Minimal Mono: только текст, прозрачная плашка, выравненные колонки.
void drawMono(Painter& painter, const OverlayText& text, const Rect& frame, const StyleLook& look)
{
    const float scale = painter.scale();
    const std::array<std::wstring, kLabelCount> labels = rowLabels(text, look);
    const std::array<std::wstring, kLabelCount> values = rowValues(text);

    painter.plate(frame, look.radius * scale, look.background, 0.28f * painter.opacity() + 0.10f);

    float y = frame.top + 6.0f * scale;
    for (std::size_t index = 0; index < kLabelCount; ++index) {
        if (!rowShown(text, index)) {
            continue;
        }
        painter.text(labels[index], Rect{ frame.left + look.padding * scale, y, frame.left + 86.0f * scale,
                                          y + kRowHeight * scale },
            TextRole::Tiny, look.label);
        painter.text(values[index], Rect{ frame.right - look.padding * scale - 140.0f * scale, y,
                                          frame.right - look.padding * scale, y + kRowHeight * scale },
            TextRole::Small, look.palette[index], TextAlign::Right);
        y += (18.0f + look.rowGap) * scale;
    }
}

// Esport Bar: одна плотная полоса с цветными значениями и разделителями.
void drawEsport(Painter& painter, const OverlayText& text, const Rect& frame, const StyleLook& look)
{
    const float scale = painter.scale();
    const std::array<std::wstring, kLabelCount> labels = rowLabels(text, look);
    const std::array<std::wstring, kLabelCount> values = rowValues(text);

    drawShadow(painter, frame, look.radius * scale);
    painter.plate(frame, look.radius * scale, look.background, 0.9f * painter.opacity() + 0.10f);
    painter.outline(frame, look.radius * scale, look.border, 1.0f, 0.9f);

    float x = frame.left + look.padding * scale;
    const float top = frame.top + look.padding * scale;
    const float bottom = frame.bottom - look.padding * scale;

    for (std::size_t index = 0; index < kLabelCount; ++index) {
        if (!rowShown(text, index)) {
            continue;
        }

        const bool big = index == 0;
        const float labelWidth = painter.measure(labels[index], TextRole::Tiny);
        const float valueWidth = big ? painter.measure(values[index], TextRole::Value)
                                     : painter.measure(values[index], TextRole::Small);
        const float cellWidth = std::max(labelWidth, valueWidth) + 10.0f * scale;

        painter.text(labels[index], Rect{ x, top, x + cellWidth, top + 12.0f * scale }, TextRole::Tiny,
            look.label);
        painter.text(values[index], Rect{ x, top + (big ? 8.0f : 12.0f) * scale, x + cellWidth, bottom },
            big ? TextRole::Value : TextRole::Small, look.palette[index]);
        x += cellWidth;

        if (look.separators) {
            painter.line(x, top + 2.0f * scale, x, bottom - 2.0f * scale, look.border, 1.0f, 0.8f);
            x += 9.0f * scale;
        } else {
            x += 6.0f * scale;
        }
    }
}

// Retro CRT: фосфорный зелёный, развёртка и лёгкое свечение строк.
void drawRetro(Painter& painter, const OverlayText& text, const Rect& frame, const StyleLook& look)
{
    const float scale = painter.scale();
    const std::array<std::wstring, kLabelCount> labels = rowLabels(text, look);
    const std::array<std::wstring, kLabelCount> values = rowValues(text);

    painter.plate(frame, look.radius * scale, look.background, 0.82f * painter.opacity() + 0.18f);
    painter.outline(frame, look.radius * scale, look.border, 1.0f, 0.8f);
    painter.outline(Rect{ frame.left + 2.0f * scale, frame.top + 2.0f * scale, frame.right - 2.0f * scale,
                            frame.bottom - 2.0f * scale },
        std::max(look.radius - 1.0f, 0.0f) * scale, look.accent, 1.0f, 0.18f);

    // Строки развёртки: тонкие линии через каждые 4 пикселя.
    for (float y = frame.top + 3.0f * scale; y < frame.bottom - 2.0f * scale; y += 4.0f * scale) {
        painter.line(frame.left + 2.0f * scale, y, frame.right - 2.0f * scale, y, look.accent, 1.0f, 0.06f);
    }

    const float left = frame.left + look.padding * scale;
    const float right = frame.right - look.padding * scale;

    painter.text(tracked(L"YAWAFPSHUB · CRT", look.letterSpacing),
        Rect{ left, frame.top + 6.0f * scale, right, frame.top + 20.0f * scale }, TextRole::Mono, look.label);
    painter.line(left, frame.top + 24.0f * scale, right, frame.top + 24.0f * scale, look.accent, 1.0f, 0.25f);

    float y = frame.top + 28.0f * scale;
    for (std::size_t index = 0; index < kLabelCount; ++index) {
        if (!rowShown(text, index)) {
            continue;
        }
        painter.text(labels[index], Rect{ left, y, left + 110.0f * scale, y + kRowHeight * scale },
            TextRole::Mono, look.label);
        painter.text(tracked(values[index], look.letterSpacing),
            Rect{ left + 112.0f * scale, y, right, y + kRowHeight * scale }, TextRole::Mono, look.palette[index],
            TextAlign::Right);
        y += (18.0f + look.rowGap) * scale;
    }
}

void drawInline(Painter& painter, const OverlaySettings& settings, const OverlayText& text, const Rect& frame,
    const StyleLook& look)
{
    const float scale = painter.scale();
    const std::wstring line = tracked(inlineText(text), look.letterSpacing);
    const Rect inner = Rect{ frame.left + look.padding * scale, frame.top, frame.right - look.padding * scale,
        frame.bottom };
    const TextRole role = look.mono ? TextRole::Mono : TextRole::Small;

    if (settings.style == OverlayStyle::Rtss) {
        painter.text(line, inner, role, look.accent);
        return;
    }

    painter.plate(frame, look.radius * scale, look.background, look.shadow ? 0.72f : 0.35f);
    painter.outline(frame, look.radius * scale, look.border, settings.style == OverlayStyle::Neon ? 1.4f : 1.0f,
        0.85f);
    painter.text(line, inner, role, look.palette[0]);
}

} // namespace

OverlaySwatch overlaySwatch(OverlayStyle style)
{
    const StyleLook look = lookFor(style);
    OverlaySwatch swatch;
    swatch.background = look.background;
    swatch.border = look.border;
    swatch.accent = look.accent;
    swatch.value = look.value;
    swatch.radius = look.radius;
    swatch.borderVisible = look.borderVisible;
    swatch.mono = look.mono;

    switch (style) {
    case OverlayStyle::Rtss:
        swatch.alpha = 0.0f;
        break;
    case OverlayStyle::Frosted:
        // Тёмный текст на светлой подложке читается только при заметной непрозрачности.
        swatch.alpha = 0.75f;
        break;
    case OverlayStyle::Mono:
        swatch.alpha = 0.35f;
        break;
    case OverlayStyle::Cyber:
        swatch.alpha = 0.9f;
        break;
    case OverlayStyle::Terminal:
        swatch.alpha = 0.96f;
        break;
    default:
        swatch.alpha = 0.95f;
        break;
    }
    return swatch;
}

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
    const StyleLook look = lookFor(settings.style);

    // Шрифты масштабируются вместе с рамкой: иначе «Масштаб» растягивал только подложку.
    std::array<IDWriteTextFormat*, 4> owned{};
    OverlayFormats scaled = formats;
    if (scale > 0.0f && (scale < 0.999f || scale > 1.001f) && formats.factory != nullptr) {
        owned = { scaledFormat(formats.factory, formats.key, scale), scaledFormat(formats.factory, formats.value, scale),
            scaledFormat(formats.factory, formats.smallText, scale), scaledFormat(formats.factory, formats.mono, scale) };
        if (owned[0] != nullptr && owned[1] != nullptr && owned[2] != nullptr && owned[3] != nullptr) {
            scaled = OverlayFormats{ formats.factory, owned[0], owned[1], owned[2], owned[3] };
        } else {
            for (IDWriteTextFormat* format : owned) {
                if (format != nullptr) {
                    format->Release();
                }
            }
            owned = {};
            scaled = formats;
        }
    }

    OverlayChrome chrome;
    chrome.background = settings.showBackground;
    chrome.border = settings.showBorder;
    chrome.shadow = settings.showShadow;
    chrome.radius = settings.cornerRadius >= 0 ? static_cast<float>(settings.cornerRadius) : -1.0f;

    Painter measurer(target, theme, scaled, scale, opacity, chrome);
    const float lineWidth = measurer.measure(inlineText(text), look.mono ? TextRole::Mono : TextRole::Small);
    const StyleSpec spec = styleSpec(settings, text, lineWidth);
    const float width = spec.width * scale;
    const float height = spec.height * scale;

    const float margin = kMargin * scale;
    const bool isLeft = settings.position == OverlayPosition::TopLeft
        || settings.position == OverlayPosition::BottomLeft;
    const bool isTop = settings.position == OverlayPosition::TopLeft
        || settings.position == OverlayPosition::TopRight;

    // Смещение — в экранных пикселях и не зависит от масштаба: раньше смена масштаба
    // сдвигала оверлей из-за умножения сдвига на коэффициент.
    float left = isLeft ? bounds.left + margin + static_cast<float>(settings.offsetX)
                        : bounds.right - width - margin + static_cast<float>(settings.offsetX);
    float top = isTop ? bounds.top + margin + static_cast<float>(settings.offsetY)
                      : bounds.bottom - height - margin + static_cast<float>(settings.offsetY);

    left = std::clamp(left, bounds.left, std::max(bounds.left, bounds.right - width));
    top = std::clamp(top, bounds.top, std::max(bounds.top, bounds.bottom - height));

    const Rect frame{ left, top, left + width, top + height };
    Painter painter(target, theme, scaled, scale, opacity, chrome);

    if (settings.layout == OverlayLayout::Line) {
        drawInline(painter, settings, text, frame, look);
    } else {
        switch (settings.style) {
        case OverlayStyle::Neon:
            drawNeon(painter, text, frame, look);
            break;
        case OverlayStyle::Rtss:
            drawRtss(painter, text, frame, look);
            break;
        case OverlayStyle::Frosted:
            drawFrosted(painter, text, frame, look);
            break;
        case OverlayStyle::Terminal:
            drawTerminal(painter, text, frame, look);
            break;
        case OverlayStyle::Cyber:
            drawCyber(painter, text, frame, look);
            break;
        case OverlayStyle::Mono:
            drawMono(painter, text, frame, look);
            break;
        case OverlayStyle::Esport:
            drawEsport(painter, text, frame, look);
            break;
        case OverlayStyle::Retro:
            drawRetro(painter, text, frame, look);
            break;
        }
    }

    for (IDWriteTextFormat* format : owned) {
        if (format != nullptr) {
            format->Release();
        }
    }

    return frame;
}

} // namespace yawa

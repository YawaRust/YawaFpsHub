#include "render/overlay_painter.h"
#include "metrics/overlay_rows.h"

#include <wincodec.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
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
            return { };
        }
        const int length
            = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (length <= 0) {
            return { };
        }
        std::wstring result(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), length);
        return result;
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

    // Семейство шрифта по номеру настройки. Нулевое значение оставляет шрифт пресета.
    const wchar_t* fontFamilyName(int index)
    {
        switch (index) {
        case 1:
            return L"Consolas";
        case 2:
            return L"Bahnschrift SemiCondensed";
        case 3:
            return L"Impact";
        default:
            return nullptr;
        }
    }

    // Копия формата с другим размером, начертанием и семейством. Раньше масштаб двигал только
    // рамку: текст оставался прежним, и оверлей выглядел сломанным.
    IDWriteTextFormat* styledFormat(
        IDWriteFactory* factory, IDWriteTextFormat* base, float factor, const OverlaySettings& settings)
    {
        if (factory == nullptr || base == nullptr || factor <= 0.0f) {
            return nullptr;
        }

        const UINT32 length = base->GetFontFamilyNameLength();
        std::wstring family(static_cast<std::size_t>(length) + 1, L'\0');
        if (FAILED(base->GetFontFamilyName(family.data(), length + 1))) {
            return nullptr;
        }
        family.resize(length);

        if (const wchar_t* chosen = fontFamilyName(settings.fontFamily)) {
            family = chosen;
        }
        const DWRITE_FONT_WEIGHT weight = settings.fontBold ? DWRITE_FONT_WEIGHT_BOLD : base->GetFontWeight();
        const DWRITE_FONT_STYLE style = settings.fontItalic ? DWRITE_FONT_STYLE_ITALIC : base->GetFontStyle();

        IDWriteTextFormat* format = nullptr;
        if (FAILED(factory->CreateTextFormat(family.c_str(), nullptr, weight, style, base->GetFontStretch(),
                base->GetFontSize() * factor, L"ru-ru", &format))) {
            return nullptr;
        }

        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetParagraphAlignment(base->GetParagraphAlignment());
        format->SetTextAlignment(base->GetTextAlignment());
        return format;
    }

    struct OverlayChrome
    {
        bool background = true;
        bool border = true;
        bool shadow = true;
        // -1 — радиус из пресета.
        float radius = -1.0f;
        // Множитель толщины рамки и силы тени: 1 — как в пресете.
        float borderWidth = 1.0f;
        float shadowStrength = 1.0f;
    };

    class Painter
    {
    public:
        Painter(ID2D1RenderTarget* target, const Theme& theme, const OverlayFormats& formats, float scale,
            float opacity, const OverlayChrome& chrome, const OverlaySettings& settings, int outline = 0)
            : m_target(target)
            , m_theme(theme)
            , m_formats(formats)
            , m_scale(scale)
            , m_opacity(opacity)
            , m_chrome(chrome)
            , m_settings(settings)
            , m_outline(static_cast<float>(std::max(outline, 0)))
        {
        }

        // Подложка оверлея: уважает переключатель «Фон» и пользовательский радиус углов.
        void plate(const Rect& rect, float radius, const Color& color, float alphaScale = 1.0f)
        {
            if (!m_chrome.background || m_backgroundOpacity <= 0.0f) {
                return;
            }
            fillRound(rect, chromeRadius(radius), color, alphaScale * m_backgroundOpacity);
        }

        // Прозрачность самой подложки: общая прозрачность окна и прозрачность плашки
        // настраиваются отдельно, иначе «Фон» нельзя сделать почти незаметным.
        void setBackgroundOpacity(float value) { m_backgroundOpacity = std::clamp(value, 0.0f, 1.0f); }

        void outline(
            const Rect& rect, float radius, const Color& color, float thickness, float alphaScale = 1.0f)
        {
            if (!m_chrome.border) {
                return;
            }
            strokeRound(rect, chromeRadius(radius), color, thickness * m_chrome.borderWidth, alphaScale);
        }

        void dropShadow(const Rect& rect, float radius, const Color& color, float alphaScale)
        {
            if (!m_chrome.shadow) {
                return;
            }
            fillRound(rect, chromeRadius(radius), color, alphaScale * m_chrome.shadowStrength);
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
                D2D1::RoundedRect(D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom), radius, radius),
                brush);
            brush->Release();
        }

        void fillCircle(
            float centerX, float centerY, float radius, const Color& color, float alphaScale = 1.0f)
        {
            fillRound(Rect { centerX - radius, centerY - radius, centerX + radius, centerY + radius }, radius,
                color, alphaScale);
        }

        void strokeRound(
            const Rect& rect, float radius, const Color& color, float thickness, float alphaScale = 1.0f)
        {
            ID2D1SolidColorBrush* brush = nullptr;
            if (FAILED(m_target->CreateSolidColorBrush(toD2D(color, alphaScale), &brush))) {
                return;
            }
            m_target->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom), radius, radius),
                brush, thickness);
            brush->Release();
        }

        void line(float x1, float y1, float x2, float y2, const Color& color, float thickness,
            float alphaScale = 1.0f)
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

            const float textBottom
                = rect.bottom + std::max(0.0f, (m_rowTextFactor - 1.0f) * format->GetFontSize());
            const DWRITE_TEXT_ALIGNMENT previous = format->GetTextAlignment();
            format->SetTextAlignment(align == TextAlign::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                    : align == TextAlign::Center               ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                               : DWRITE_TEXT_ALIGNMENT_LEADING);

            // Обводка: контрастный контур, чтобы текст читался на любом фоне игры.
            if (m_outline > 0.0f) {
                const float luminance = color.r * 0.299f + color.g * 0.587f + color.b * 0.114f;
                const Color edge = luminance > 0.55f ? Color { 0.02f, 0.03f, 0.05f, 0.9f }
                                                     : Color { 0.97f, 0.98f, 1.0f, 0.9f };
                ID2D1SolidColorBrush* edgeBrush = nullptr;
                if (SUCCEEDED(m_target->CreateSolidColorBrush(toD2D(edge, alphaScale), &edgeBrush))) {
                    const float step = m_outline * m_scale;
                    const std::array<std::pair<float, float>, 8> shifts {
                        std::pair<float, float> { -step, 0.0f }, { step, 0.0f }, { 0.0f, -step },
                        { 0.0f, step }, { -step, -step }, { step, -step }, { -step, step }, { step, step }
                    };
                    for (const auto& [dx, dy] : shifts) {
                        m_target->DrawTextW(value.data(), static_cast<UINT32>(value.size()), format,
                            D2D1::RectF(rect.left + dx, rect.top + dy, rect.right + dx, textBottom + dy),
                            edgeBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                    edgeBrush->Release();
                }
            }

            ID2D1SolidColorBrush* brush = nullptr;
            if (FAILED(m_target->CreateSolidColorBrush(toD2D(color, alphaScale), &brush))) {
                format->SetTextAlignment(previous);
                return;
            }

            m_target->DrawTextW(value.data(), static_cast<UINT32>(value.size()), format,
                D2D1::RectF(rect.left, rect.top, rect.right, textBottom), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
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
            if (FAILED(m_formats.factory->CreateTextLayout(
                    value.data(), static_cast<UINT32>(value.size()), format, 4096.0f, 200.0f, &layout))) {
                return 0.0f;
            }
            DWRITE_TEXT_METRICS metrics { };
            layout->GetMetrics(&metrics);
            layout->Release();
            return metrics.widthIncludingTrailingWhitespace;
        }

        void setRowBounds(const OverlaySettings& settings, std::vector<OverlayRowBounds>* bounds,
            const Rect& frame, float fit)
        {
            m_rowOffsets = &settings.styleRowOffsets;
            m_rowBounds = bounds;
            m_frame = frame;
            m_fit = fit;
        }

        void beginTextSize(MetricId id)
        {
            const int percent = m_settings.metricTextScalePercent[static_cast<std::size_t>(id)];
            if (percent == 100 || m_formats.factory == nullptr) {
                return;
            }
            m_baseFormats = m_formats;
            const float factor = static_cast<float>(std::clamp(percent, 70, 200)) / 100.0f;
            m_rowFormats = { styledFormat(m_formats.factory, m_formats.key, factor, m_settings),
                styledFormat(m_formats.factory, m_formats.value, factor, m_settings),
                styledFormat(m_formats.factory, m_formats.smallText, factor, m_settings),
                styledFormat(m_formats.factory, m_formats.mono, factor, m_settings) };
            if (std::all_of(m_rowFormats.begin(), m_rowFormats.end(),
                    [](IDWriteTextFormat* format) { return format != nullptr; })) {
                m_formats = OverlayFormats { m_formats.factory, m_rowFormats[0], m_rowFormats[1],
                    m_rowFormats[2], m_rowFormats[3] };
                m_rowTextFactor = factor;
            } else {
                endTextSize();
            }
        }

        void endTextSize()
        {
            if (m_rowFormats[0] == nullptr && m_rowFormats[1] == nullptr && m_rowFormats[2] == nullptr
                && m_rowFormats[3] == nullptr) {
                return;
            }
            m_formats = m_baseFormats;
            for (auto*& format : m_rowFormats) {
                if (format)
                    format->Release();
                format = nullptr;
            }
            m_rowTextFactor = 1.0f;
        }

        void beginRow(MetricId id, const Rect& row)
        {
            beginTextSize(id);
            const auto& offset = (*m_rowOffsets)[static_cast<std::size_t>(id)];
            const float dx = static_cast<float>(offset.x) * m_scale;
            const float dy = static_cast<float>(offset.y) * m_scale;
            if (m_rowBounds != nullptr) {
                m_rowBounds->push_back({ id,
                    { m_frame.left + (row.left + dx - m_frame.left) * m_fit,
                        m_frame.top + (row.top + dy - m_frame.top) * m_fit,
                        m_frame.left + (row.right + dx - m_frame.left) * m_fit,
                        m_frame.top + (row.bottom + dy - m_frame.top) * m_fit } });
            }
            if (offset.x != 0 || offset.y != 0) {
                m_target->GetTransform(&m_rowTransform);
                m_target->SetTransform(D2D1::Matrix3x2F::Translation(dx, dy) * m_rowTransform);
                m_rowTransformed = true;
            }
        }

        void endRow()
        {
            if (m_rowTransformed) {
                m_target->SetTransform(m_rowTransform);
                m_rowTransformed = false;
            }
            endTextSize();
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
        const OverlaySettings& m_settings;
        OverlayFormats m_baseFormats { };
        std::array<IDWriteTextFormat*, 4> m_rowFormats { };
        float m_rowTextFactor = 1.0f;
        float m_outline = 0.0f;
        float m_backgroundOpacity = 1.0f;
        std::vector<OverlayRowBounds>* m_rowBounds = nullptr;
        Rect m_frame { };
        float m_fit = 1.0f;
        const std::array<OverlayRowOffset, kMetricCount>* m_rowOffsets = nullptr;
        D2D1::Matrix3x2F m_rowTransform { };
        bool m_rowTransformed = false;
    };

    // Полный визуальный набор стиля: палитра, форма, типографика. Раньше стили отличались
    // только подложкой, теперь меняется всё сразу — как в пресетах Fpsmon2.0.
    struct StyleLook
    {
        Color background;
        Color border;
        Color label;
        Color accent;
        Color value;
        std::array<Color, kLabelCount> palette { };
        std::array<Color, kMetricCount> rowPalette { };
        float radius = 8.0f;
        float padding = 10.0f;
        float rowGap = 2.0f;
        float labelWidth = 110.0f;
        float valueWidth = 140.0f;
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

    struct StyleSpec
    {
        float width = 200.0f;
        float height = 96.0f;
    };

    bool rowShown(const OverlayText& text, std::size_t index)
    {
        if (!text.visible[index]) {
            return false;
        }
        return true;
    }

    // Сколько строк реально рисуем: выключенные в «Составе» не занимают место.
    std::size_t visibleRows(const OverlayText& text)
    {
        std::size_t rows = 0;
        for (std::size_t index = 0; index < text.labels.size(); ++index) {
            if (rowShown(text, index)) {
                ++rows;
            }
        }
        return rows;
    }

    const std::vector<std::wstring>& rowValues(const OverlayText& text)
    {
        return text.values;
    }

    std::vector<std::wstring> rowLabels(const OverlayText& text, const StyleLook& look)
    {
        std::vector<std::wstring> labels = text.labels;
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
                : settings.style == OverlayStyle::Esport              ? 32.0f
                                                                      : 30.0f;
            return StyleSpec { std::max(lineWidth + 28.0f, 140.0f), height };
        }
        if (settings.layout == OverlayLayout::Gauge) {
            return StyleSpec { 242.0f, 14.0f + static_cast<float>(rows) * 32.0f };
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
            spec = { std::max(320.0f, lineWidth + 40.0f), 44.0f };
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
        painter.dropShadow(Rect { frame.left + 2.0f * scale, frame.top + 4.0f * scale,
                               frame.right + 3.0f * scale, frame.bottom + 5.0f * scale },
            radius, styleColor(0x000000u), 0.28f);
    }

    // Плитки «плат»: крупный FPS и строки показателей. Frosted Glass.
    void drawFrosted(Painter& painter, const OverlayText& text, const Rect& frame, const StyleLook& look)
    {
        const float scale = painter.scale();
        const std::vector<std::wstring> labels = rowLabels(text, look);
        const std::vector<std::wstring> values = rowValues(text);

        drawShadow(painter, frame, look.radius * scale);
        painter.plate(frame, look.radius * scale, look.background, 0.35f + 0.6f * painter.opacity());
        painter.outline(frame, look.radius * scale, look.label, 1.0f, 0.25f);

        const float left = frame.left + look.padding * scale;
        const float right = frame.right - look.padding * scale;

        if (rowShown(text, 0)) {
            painter.beginRow(text.ids[0],
                Rect { frame.left, frame.top + 8.0f * scale, frame.right, frame.top + 62.0f * scale });
            const Rect chip { left, frame.top + 8.0f * scale, left + 66.0f * scale,
                frame.top + 26.0f * scale };
            painter.fillRound(chip, 9.0f * scale, look.label, 0.12f);
            painter.text(labels[0], chip, TextRole::Tiny, look.label, TextAlign::Center);
            painter.text(values[0],
                Rect { left - 2.0f * scale, frame.top + 24.0f * scale, left + 120.0f * scale,
                    frame.top + 62.0f * scale },
                TextRole::Display, look.value);
            painter.text(L"fps",
                Rect { left + 62.0f * scale, frame.top + 46.0f * scale, left + 96.0f * scale,
                    frame.top + 60.0f * scale },
                TextRole::Tiny, look.label);
            painter.line(
                left, frame.top + 64.0f * scale, right, frame.top + 64.0f * scale, look.border, 1.0f, 0.22f);
            painter.endRow();
        }

        float y = frame.top + (rowShown(text, 0) ? 70.0f : 8.0f) * scale;
        for (std::size_t index = 1; index < text.labels.size(); ++index) {
            if (!rowShown(text, index)) {
                continue;
            }
            painter.beginRow(
                text.ids[index], Rect { frame.left, y, frame.right, y + (18.0f + look.rowGap) * scale });
            painter.text(labels[index],
                Rect { left, y, left + look.labelWidth * scale, y + kRowHeight * scale }, TextRole::Tiny,
                look.label);
            painter.text(values[index],
                Rect { right - look.valueWidth * scale, y, right, y + kRowHeight * scale }, TextRole::Small,
                look.rowPalette[index], TextAlign::Right);
            painter.endRow();
            y += (18.0f + look.rowGap) * scale;
        }
    }

    // Neon Grid: неоновый акцент, сетка показателей с разделителями и цветом на устройство.
    void drawNeon(Painter& painter, const OverlayText& text, const Rect& frame, const StyleLook& look)
    {
        const float scale = painter.scale();
        const std::vector<std::wstring> labels = rowLabels(text, look);
        const std::vector<std::wstring> values = rowValues(text);

        drawShadow(painter, frame, look.radius * scale);
        painter.plate(frame, look.radius * scale, look.background, 0.72f * painter.opacity() + 0.18f);
        painter.outline(Rect { frame.left - 2.0f * scale, frame.top - 2.0f * scale,
                            frame.right + 2.0f * scale, frame.bottom + 2.0f * scale },
            (look.radius + 2.0f) * scale, look.accent, 2.0f, 0.18f);
        painter.outline(frame, look.radius * scale, look.border, 1.0f, 0.35f);

        const float left = frame.left + look.padding * scale;
        const float right = frame.right - look.padding * scale;

        if (rowShown(text, 0)) {
            painter.beginRow(text.ids[0],
                Rect { frame.left, frame.top + 8.0f * scale, frame.right, frame.top + 60.0f * scale });
            painter.text(labels[0],
                Rect { left, frame.top + 8.0f * scale, left + 120.0f * scale, frame.top + 22.0f * scale },
                TextRole::Tiny, look.accent);
            painter.text(values[0],
                Rect { left, frame.top + 20.0f * scale, left + 118.0f * scale, frame.top + 60.0f * scale },
                TextRole::Display, look.value);
            painter.text(L"FPS",
                Rect { left + 84.0f * scale, frame.top + 32.0f * scale, left + 118.0f * scale,
                    frame.top + 46.0f * scale },
                TextRole::Tiny, look.accent);

            painter.line(
                left, frame.top + 60.0f * scale, right, frame.top + 60.0f * scale, look.accent, 1.0f, 0.35f);
            painter.endRow();
        }

        float y = frame.top + (rowShown(text, 0) ? 64.0f : 8.0f) * scale;
        for (std::size_t index = 1; index < text.labels.size(); ++index) {
            if (!rowShown(text, index)) {
                continue;
            }
            painter.beginRow(
                text.ids[index], Rect { frame.left, y, frame.right, y + (18.0f + look.rowGap) * scale });
            painter.fillCircle(left + 2.0f * scale, y + 8.0f * scale, 2.0f * scale, look.rowPalette[index]);
            painter.text(labels[index],
                Rect { left + 10.0f * scale, y, left + look.labelWidth * scale, y + kRowHeight * scale },
                TextRole::Tiny, look.label);
            painter.text(values[index],
                Rect { right - look.valueWidth * scale, y, right, y + kRowHeight * scale }, TextRole::Small,
                look.rowPalette[index], TextAlign::Right);
            painter.endRow();
            y += (18.0f + look.rowGap) * scale;
        }
    }

    // RTSS Classic: без фона, каждая строка «подпись значение», моноширинный.
    void drawRtss(Painter& painter, const OverlayText& text, const Rect& frame, const StyleLook& look)
    {
        const float scale = painter.scale();
        const std::vector<std::wstring> labels = rowLabels(text, look);
        const std::vector<std::wstring> values = rowValues(text);

        painter.plate(frame, look.radius * scale, look.background, 0.75f);
        painter.outline(frame, look.radius * scale, look.border, 1.0f, 0.6f);

        float y = frame.top + 2.0f * scale;
        for (std::size_t index = 0; index < text.labels.size(); ++index) {
            if (!rowShown(text, index)) {
                continue;
            }
            painter.beginRow(text.ids[index], Rect { frame.left, y, frame.right, y + 19.0f * scale });
            painter.text(labels[index],
                Rect { frame.left, y, frame.left + look.padding * scale + look.labelWidth * scale,
                    y + kRowHeight * scale },
                TextRole::Mono, look.label);
            painter.text(values[index],
                Rect { frame.left + look.padding * scale + look.labelWidth * scale, y, frame.right,
                    y + kRowHeight * scale },
                TextRole::Mono, look.value);
            painter.endRow();
            y += 19.0f * scale;
        }
    }

    // Terminal: чёрный бокс, янтарная рамка, строки с приглашением «>».
    void drawTerminal(Painter& painter, const OverlayText& text, const Rect& frame, const StyleLook& look)
    {
        const float scale = painter.scale();
        const std::vector<std::wstring> labels = rowLabels(text, look);
        const std::vector<std::wstring> values = rowValues(text);

        painter.plate(frame, look.radius * scale, look.background, 0.86f * painter.opacity() + 0.12f);
        painter.outline(frame, look.radius * scale, look.border, 1.0f, 0.9f);

        const float left = frame.left + look.padding * scale;
        const float right = frame.right - look.padding * scale;

        painter.text(tracked(L"YAWAFPSHUB", look.letterSpacing),
            Rect { left, frame.top + 6.0f * scale, right, frame.top + 20.0f * scale }, TextRole::Mono,
            look.accent);
        painter.line(
            left, frame.top + 24.0f * scale, right, frame.top + 24.0f * scale, look.accent, 1.0f, 0.35f);

        float y = frame.top + 28.0f * scale;
        for (std::size_t index = 0; index < text.labels.size(); ++index) {
            if (!rowShown(text, index)) {
                continue;
            }
            painter.beginRow(text.ids[index],
                Rect { frame.left, y, frame.right, y + std::min(kRowHeight, 18.0f + look.rowGap) * scale });
            painter.text(L"> " + labels[index],
                Rect { left, y, left + look.labelWidth * scale, y + kRowHeight * scale }, TextRole::Mono,
                look.label);
            painter.text(tracked(values[index], look.letterSpacing),
                Rect { right - look.valueWidth * scale, y, right, y + kRowHeight * scale }, TextRole::Mono,
                look.rowPalette[index], TextAlign::Right);
            painter.endRow();
            y += (18.0f + look.rowGap) * scale;
        }
    }

    // Cyber Magenta: угловые скобки, широкий трекинг, пурпурная подсветка.
    void drawCyber(Painter& painter, const OverlayText& text, const Rect& frame, const StyleLook& look)
    {
        const float scale = painter.scale();
        const std::vector<std::wstring> labels = rowLabels(text, look);
        const std::vector<std::wstring> values = rowValues(text);

        drawShadow(painter, frame, look.radius * scale);
        painter.plate(frame, look.radius * scale, look.background, 0.78f * painter.opacity() + 0.14f);
        painter.outline(frame, look.radius * scale, look.border, 1.2f, 0.85f);

        const float corner = 12.0f * scale;
        const float inset = 3.0f * scale;
        const Rect brackets { frame.left + inset, frame.top + inset, frame.right - inset,
            frame.bottom - inset };
        painter.line(brackets.left, brackets.top, brackets.left + corner, brackets.top, look.accent, 2.0f);
        painter.line(brackets.left, brackets.top, brackets.left, brackets.top + corner, look.accent, 2.0f);
        painter.line(
            brackets.right - corner, brackets.bottom, brackets.right, brackets.bottom, look.accent, 2.0f);
        painter.line(
            brackets.right, brackets.bottom - corner, brackets.right, brackets.bottom, look.accent, 2.0f);

        const float left = frame.left + look.padding * scale;
        const float right = frame.right - look.padding * scale;

        if (rowShown(text, 0)) {
            painter.beginRow(text.ids[0],
                Rect { frame.left, frame.top + 8.0f * scale, frame.right, frame.top + 62.0f * scale });
            painter.text(labels[0],
                Rect { left, frame.top + 8.0f * scale, left + 140.0f * scale, frame.top + 22.0f * scale },
                TextRole::Tiny, look.accent);
            painter.text(values[0],
                Rect { left, frame.top + 20.0f * scale, left + 124.0f * scale, frame.top + 60.0f * scale },
                TextRole::Display, look.value);
            painter.line(
                left, frame.top + 62.0f * scale, right, frame.top + 62.0f * scale, look.border, 1.0f, 0.35f);
            painter.endRow();
        }

        float y = frame.top + (rowShown(text, 0) ? 68.0f : 10.0f) * scale;
        for (std::size_t index = 1; index < text.labels.size(); ++index) {
            if (!rowShown(text, index)) {
                continue;
            }
            painter.beginRow(
                text.ids[index], Rect { frame.left, y, frame.right, y + (18.0f + look.rowGap) * scale });
            painter.text(labels[index],
                Rect { left, y, left + look.labelWidth * scale, y + kRowHeight * scale }, TextRole::Tiny,
                look.label);
            painter.text(values[index],
                Rect { right - look.valueWidth * scale, y, right, y + kRowHeight * scale }, TextRole::Small,
                look.rowPalette[index], TextAlign::Right);
            painter.endRow();
            y += (18.0f + look.rowGap) * scale;
        }
    }

    // Minimal Mono: только текст, прозрачная плашка, выравненные колонки.
    void drawMono(Painter& painter, const OverlayText& text, const Rect& frame, const StyleLook& look)
    {
        const float scale = painter.scale();
        const std::vector<std::wstring> labels = rowLabels(text, look);
        const std::vector<std::wstring> values = rowValues(text);

        painter.plate(frame, look.radius * scale, look.background, 0.28f * painter.opacity() + 0.10f);

        float y = frame.top + 6.0f * scale;
        for (std::size_t index = 0; index < text.labels.size(); ++index) {
            if (!rowShown(text, index)) {
                continue;
            }
            painter.beginRow(text.ids[index],
                Rect { frame.left, y, frame.right, y + std::min(kRowHeight, 18.0f + look.rowGap) * scale });
            painter.text(labels[index],
                Rect { frame.left + look.padding * scale, y, frame.left + 86.0f * scale,
                    y + kRowHeight * scale },
                TextRole::Tiny, look.label);
            painter.text(values[index],
                Rect { frame.right - look.padding * scale - look.valueWidth * scale, y,
                    frame.right - look.padding * scale, y + kRowHeight * scale },
                TextRole::Small, look.rowPalette[index], TextAlign::Right);
            painter.endRow();
            y += (18.0f + look.rowGap) * scale;
        }
    }

    // Esport Bar: одна плотная полоса с цветными значениями и разделителями.
    void drawEsport(Painter& painter, const OverlayText& text, const Rect& frame, const StyleLook& look)
    {
        const float scale = painter.scale();
        const std::vector<std::wstring> labels = rowLabels(text, look);
        const std::vector<std::wstring> values = rowValues(text);

        drawShadow(painter, frame, look.radius * scale);
        painter.plate(frame, look.radius * scale, look.background, 0.9f * painter.opacity() + 0.10f);
        painter.outline(frame, look.radius * scale, look.border, 1.0f, 0.9f);

        float x = frame.left + look.padding * scale;
        const float top = frame.top + look.padding * scale;
        const float bottom = frame.bottom - look.padding * scale;

        for (std::size_t index = 0; index < text.labels.size(); ++index) {
            if (!rowShown(text, index)) {
                continue;
            }

            const bool big = index == 0;
            const float labelWidth = painter.measure(labels[index], TextRole::Tiny);
            const float valueWidth = big ? painter.measure(values[index], TextRole::Value)
                                         : painter.measure(values[index], TextRole::Small);
            const float cellWidth = std::max(labelWidth, valueWidth) + 10.0f * scale;
            painter.beginRow(text.ids[index], Rect { x, top, x + cellWidth, bottom });

            painter.text(labels[index], Rect { x, top, x + cellWidth, top + 12.0f * scale }, TextRole::Tiny,
                look.label);
            painter.text(values[index], Rect { x, top + (big ? 8.0f : 12.0f) * scale, x + cellWidth, bottom },
                big ? TextRole::Value : TextRole::Small, look.rowPalette[index]);
            painter.endRow();
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
        const std::vector<std::wstring> labels = rowLabels(text, look);
        const std::vector<std::wstring> values = rowValues(text);

        painter.plate(frame, look.radius * scale, look.background, 0.82f * painter.opacity() + 0.18f);
        painter.outline(frame, look.radius * scale, look.border, 1.0f, 0.8f);
        painter.outline(Rect { frame.left + 2.0f * scale, frame.top + 2.0f * scale,
                            frame.right - 2.0f * scale, frame.bottom - 2.0f * scale },
            std::max(look.radius - 1.0f, 0.0f) * scale, look.accent, 1.0f, 0.18f);

        // Строки развёртки: тонкие линии через каждые 4 пикселя.
        for (float y = frame.top + 3.0f * scale; y < frame.bottom - 2.0f * scale; y += 4.0f * scale) {
            painter.line(
                frame.left + 2.0f * scale, y, frame.right - 2.0f * scale, y, look.accent, 1.0f, 0.06f);
        }

        const float left = frame.left + look.padding * scale;
        const float right = frame.right - look.padding * scale;

        painter.text(tracked(L"YAWAFPSHUB · CRT", look.letterSpacing),
            Rect { left, frame.top + 6.0f * scale, right, frame.top + 20.0f * scale }, TextRole::Mono,
            look.label);
        painter.line(
            left, frame.top + 24.0f * scale, right, frame.top + 24.0f * scale, look.accent, 1.0f, 0.25f);

        float y = frame.top + 28.0f * scale;
        for (std::size_t index = 0; index < text.labels.size(); ++index) {
            if (!rowShown(text, index)) {
                continue;
            }
            painter.beginRow(text.ids[index],
                Rect { frame.left, y, frame.right, y + std::min(kRowHeight, 18.0f + look.rowGap) * scale });
            painter.text(labels[index],
                Rect { left, y, left + look.labelWidth * scale, y + kRowHeight * scale }, TextRole::Mono,
                look.label);
            painter.text(tracked(values[index], look.letterSpacing),
                Rect { right - look.valueWidth * scale, y, right, y + kRowHeight * scale }, TextRole::Mono,
                look.rowPalette[index], TextAlign::Right);
            painter.endRow();
            y += (18.0f + look.rowGap) * scale;
        }
    }

    // Шкала заполняется только для реально прочитанной нагрузки CPU/GPU. Для FPS,
    // температур и неизвестных значений остаётся нейтральная дорожка без выдуманного процента.
    void drawGauge(Painter& painter, const OverlayText& text, const Rect& frame, const StyleLook& look)
    {
        const float scale = painter.scale();
        const float left = frame.left + look.padding * scale;
        const float right = frame.right - look.padding * scale;
        const auto labels = rowLabels(text, look);
        painter.plate(frame, look.radius * scale, look.background, 0.95f);
        painter.outline(frame, look.radius * scale, look.border, 1.0f);
        float y = frame.top + look.padding * scale;
        for (std::size_t index = 0; index < text.labels.size(); ++index) {
            if (!rowShown(text, index)) {
                continue;
            }
            painter.beginRow(text.ids[index], Rect { frame.left, y, frame.right, y + 24.0f * scale });
            painter.text(labels[index], Rect { left, y, right - 90.0f * scale, y + 17.0f * scale },
                TextRole::Small, look.label);
            painter.text(text.values[index], Rect { right - 90.0f * scale, y, right, y + 17.0f * scale },
                TextRole::Small, look.rowPalette[index], TextAlign::Right);
            const float barTop = y + 20.0f * scale;
            const Rect track { left, barTop, right, barTop + 3.0f * scale };
            painter.fillRound(track, 1.5f * scale, look.border, 0.35f);
            if (index < text.gaugeFill.size() && text.gaugeFill[index] >= 0.0f) {
                const float end = left + (right - left) * text.gaugeFill[index];
                if (end > left) {
                    painter.fillRound(
                        Rect { left, barTop, end, track.bottom }, 1.5f * scale, look.rowPalette[index], 0.9f);
                }
            }
            painter.endRow();
            y += (30.0f + look.rowGap) * scale;
        }
    }

    std::wstring rtssWide(std::string_view encoded)
    {
        if (encoded.empty()) {
            return { };
        }
        const int length
            = MultiByteToWideChar(1251, 0, encoded.data(), static_cast<int>(encoded.size()), nullptr, 0);
        if (length <= 0) {
            return { };
        }
        std::wstring value(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(1251, 0, encoded.data(), static_cast<int>(encoded.size()), value.data(), length);
        return value;
    }

    std::wstring rtssText(
        const RtssVisualLayout& layout, std::string_view source, const OverlayText& values, bool utf8 = false)
    {
        std::wstring result;
        for (std::size_t at = 0; at < source.size() && result.size() < 8192;) {
            const char start = source[at];
            if (start == '<' || start == '%') {
                const auto end = source.find(start == '<' ? '>' : '%', at + 1);
                if (end != std::string_view::npos) {
                    const std::string_view token = source.substr(at, end - at + 1);
                    auto known = std::find_if(layout.availableTags.begin(), layout.availableTags.end(),
                        [token](const auto& entry) { return entry.first == token; });
                    if (known != layout.availableTags.end()) {
                        const auto id = std::find(values.ids.begin(), values.ids.end(), known->second);
                        if (id != values.ids.end()) {
                            std::wstring metric
                                = values.values[static_cast<std::size_t>(id - values.ids.begin())];
                            if (metric != L"нет данных") {
                                const auto unit = metric.find_first_of(L" %°/");
                                if (unit != std::wstring::npos) {
                                    metric.resize(unit);
                                }
                            }
                            result += metric;
                        } else {
                            result += L"нет данных";
                        }
                    } else {
                        result += L"нет данных";
                    }
                    at = end + 1;
                    continue;
                }
            }
            if (start == '\\' && at + 1 < source.size()) {
                result += L' ';
                at += 2;
                continue;
            }
            if (start == '\n' || start == '\r') {
                result += L'\n';
                at += start == '\r' && at + 1 < source.size() && source[at + 1] == '\n' ? 2 : 1;
                continue;
            }
            if (static_cast<unsigned char>(start) < 32) {
                ++at;
                continue;
            }
            std::size_t end = at + 1;
            while (end < source.size() && source[end] != '<' && source[end] != '%' && source[end] != '\\'
                && static_cast<unsigned char>(source[end]) >= 32) {
                ++end;
            }
            result += utf8 ? wideLabel(source.substr(at, end - at)) : rtssWide(source.substr(at, end - at));
            at = end;
        }
        return result;
    }

    struct RtssRaster
    {
        std::weak_ptr<const std::string> source;
        std::vector<std::uint8_t> pixels;
        UINT width = 0;
        UINT height = 0;
    };

    const RtssRaster* rtssRaster(const RtssVisualLayout& layout)
    {
        if (!layout.imageBytes || layout.imageBytes->size() < 24
            || layout.imageBytes->size() > 4 * 1024 * 1024
            || !layout.imageBytes->starts_with("\x89PNG\r\n\x1A\n")) {
            return nullptr;
        }
        static std::vector<RtssRaster> cache;
        cache.erase(std::remove_if(cache.begin(), cache.end(),
                        [](const RtssRaster& raster) { return raster.source.expired(); }),
            cache.end());
        for (const RtssRaster& raster : cache) {
            if (raster.source.lock() == layout.imageBytes) {
                return raster.pixels.empty() ? nullptr : &raster;
            }
        }
        if (cache.size() == 3) {
            cache.erase(cache.begin());
        }
        cache.emplace_back();
        RtssRaster& raster = cache.back();
        raster.source = layout.imageBytes;
        std::vector<BYTE> input(layout.imageBytes->begin(), layout.imageBytes->end());
        IWICImagingFactory* factory = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;
        do {
            if (FAILED(CoCreateInstance(
                    CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
                || FAILED(factory->CreateStream(&stream))
                || FAILED(stream->InitializeFromMemory(input.data(), static_cast<DWORD>(input.size())))
                || FAILED(
                    factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder))
                || FAILED(decoder->GetFrame(0, &frame))
                || FAILED(frame->GetSize(&raster.width, &raster.height)) || raster.width == 0
                || raster.height == 0 || raster.width > 2048 || raster.height > 2048
                || FAILED(factory->CreateFormatConverter(&converter))
                || FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                    nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                break;
            }
            raster.pixels.resize(static_cast<std::size_t>(raster.width) * raster.height * 4);
            if (FAILED(converter->CopyPixels(nullptr, raster.width * 4,
                    static_cast<UINT>(raster.pixels.size()), raster.pixels.data()))) {
                raster.pixels.clear();
            }
        } while (false);
        if (converter)
            converter->Release();
        if (frame)
            frame->Release();
        if (decoder)
            decoder->Release();
        if (stream)
            stream->Release();
        if (factory)
            factory->Release();
        return raster.pixels.empty() ? nullptr : &raster;
    }

    bool rtssImageRect(std::string_view source, const RtssRaster& raster, D2D1_RECT_F& crop)
    {
        if (!source.starts_with("<I=") || !source.ends_with('>')) {
            return false;
        }
        source.remove_prefix(3);
        source.remove_suffix(1);
        std::array<unsigned int, 6> fields { };
        for (std::size_t index = 0; index < fields.size(); ++index) {
            const std::size_t comma = source.find(',');
            const std::string_view entry = source.substr(0, comma);
            const auto result = std::from_chars(entry.data(), entry.data() + entry.size(), fields[index]);
            if (result.ec != std::errc { } || result.ptr != entry.data() + entry.size()
                || (index < 5 && comma == std::string_view::npos)
                || (index == 5 && comma != std::string_view::npos)) {
                return false;
            }
            if (index < 5) {
                source.remove_prefix(comma + 1);
            }
        }
        if (fields[0] || fields[1] || fields[4] == 0 || fields[5] == 0 || fields[2] >= raster.width
            || fields[3] >= raster.height || fields[4] > raster.width - fields[2]
            || fields[5] > raster.height - fields[3]) {
            return false;
        }
        crop = D2D1::RectF(static_cast<float>(fields[2]), static_cast<float>(fields[3]),
            static_cast<float>(fields[2] + fields[4]), static_cast<float>(fields[3] + fields[5]));
        return true;
    }

    struct RtssPlacement
    {
        Rect rect;
        std::wstring text;
        D2D1_RECT_F imageCrop { };
        int graph = -1;
        bool fillGraph = false;
        bool image = false;
    };

    Rect drawRtssVisual(ID2D1RenderTarget* target, const RtssVisualLayout& layout, const OverlayText& values,
        const OverlaySettings& settings, const Rect& bounds, float scale, const OverlayFormats& formats,
        std::vector<Rect>* layerBounds, float* layerScale)
    {
        if (layerBounds != nullptr) {
            layerBounds->assign(layout.layers.size(), {});
        }
        const RtssRaster* image = rtssRaster(layout);
        const float zoom = static_cast<float>(layout.zoom) * scale;
        const float cellHeight = static_cast<float>(layout.fontHeight) * zoom;
        const float cellWidth = cellHeight * 0.61f;
        const auto units = [zoom, cellWidth, cellHeight](int value, bool vertical) {
            return value < 0 ? static_cast<float>(-value) * (vertical ? cellHeight : cellWidth)
                             : static_cast<float>(value) * zoom;
        };
        const std::wstring family = layout.fontFace.empty() ? L"Consolas" : rtssWide(layout.fontFace);
        std::vector<RtssPlacement> placed;
        placed.reserve(layout.layers.size());
        float leftEdge = 0.0f;
        float topEdge = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        for (std::size_t index = 0; index < layout.layers.size(); ++index) {
            const RtssVisualLayer& layer = layout.layers[index];
            const RtssLayerEdit& edit = settings.rtssLayerEdits[index];
            const std::string_view source
                = edit.text ? std::string_view(*edit.text) : std::string_view(layer.text);
            D2D1_RECT_F imageCrop { };
            const bool imageTag = source.starts_with("<I=");
            const bool imageReady = imageTag && image && rtssImageRect(source, *image, imageCrop);
            const std::string visible = rtssVisibleTemplate(source);
            std::wstring text = imageTag ? (imageReady ? L"" : L"нет данных")
                                         : rtssText(layout, visible, values, edit.text.has_value());
            if (text.empty() && !source.empty() && !imageReady) {
                text = L"н/д"; // Слой с неподдерживаемым тегом остаётся видимым и выбираемым.
            }
            int graph = -1;
            const bool fillGraph = source.ends_with(",81>");
            if (source.ends_with(",80>") || source.ends_with(",10>") || fillGraph) {
                if (source.starts_with("<G=Frametime,")) {
                    graph = 0;
                } else if (source.starts_with("<G=Framerate 1% Low,")) {
                    graph = 1;
                } else if (source.starts_with("<G=CPU usage,")
                    && std::any_of(layout.availableTags.begin(), layout.availableTags.end(),
                        [](const auto& tag) { return tag.first == "%CPU usage%"; })) {
                    graph = 2;
                } else if (source.starts_with("<G=GPU1 usage,")
                    && std::any_of(layout.availableTags.begin(), layout.availableTags.end(),
                        [](const auto& tag) { return tag.first == "%GPU1 usage%"; })) {
                    graph = 3;
                }
            }
            if (graph >= 0) {
                const auto& history = values.rtssGraphHistory[static_cast<std::size_t>(graph)];
                if (history.size() >= 2 && std::all_of(history.begin(), history.end(), [graph](float value) {
                        return std::isfinite(value) && (graph < 2 ? value > 0.0f : value >= 0.0f);
                    })) {
                    text.clear();
                } else {
                    graph = -1; // Нет реальных кадров: не рисуем искусственную кривую.
                    text = L"нет данных";
                }
            }
            const std::uint32_t background = edit.backgroundColor.value_or(layer.backgroundColor);
            const bool hasBackground
                = (edit.backgroundColor.has_value() || layer.hasBackground) && (background >> 24) != 0;
            if (!edit.visible || (text.empty() && graph < 0 && !hasBackground && !imageReady)) {
                placed.push_back({ });
                continue;
            }
            const int size = edit.size == 0 ? layer.size : edit.size;
            std::size_t longestLine = 0;
            std::size_t lines = 1;
            std::size_t lineLength = 0;
            for (wchar_t symbol : text) {
                if (symbol == L'\n') {
                    longestLine = std::max(longestLine, lineLength);
                    lineLength = 0;
                    ++lines;
                } else {
                    ++lineLength;
                }
            }
            longestLine = std::max(longestLine, lineLength);
            float textWidth = static_cast<float>(longestLine) * cellWidth * size / 100.0f;
            float textHeight = cellHeight * size / 100.0f * lines;
            if (lines > 1 && !text.empty() && formats.factory != nullptr
                && ((edit.width == 0 && layer.width == 0) || (edit.height == 0 && layer.height == 0))) {
                IDWriteTextFormat* format = nullptr;
                if (SUCCEEDED(formats.factory->CreateTextFormat(family.c_str(), nullptr,
                        static_cast<DWRITE_FONT_WEIGHT>(layout.fontWeight), DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL, cellHeight * size / 100.0f, L"ru-RU", &format))) {
                    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    IDWriteTextLayout* measured = nullptr;
                    if (SUCCEEDED(
                            formats.factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
                                format, 1024.0f * zoom, 8192.0f * zoom, &measured))) {
                        DWRITE_TEXT_METRICS metrics { };
                        if (SUCCEEDED(measured->GetMetrics(&metrics))) {
                            textWidth = metrics.widthIncludingTrailingWhitespace;
                            textHeight = metrics.height;
                        }
                        measured->Release();
                    }
                    format->Release();
                }
            }
            // Явный Extent задаёт границу слоя: текст и отсутствующие провайдеры не
            // могут увеличивать весь макет и превращать остальные датчики в пиксельную пыль.
            const float width = edit.width > 0 ? static_cast<float>(edit.width) * zoom
                : layer.width == 0 ? (imageReady ? (imageCrop.right - imageCrop.left) * zoom
                                                 : std::clamp(textWidth, cellWidth, 1024.0f * zoom))
                                   : units(layer.width, false);
            const float height = edit.height > 0 ? static_cast<float>(edit.height) * zoom
                : layer.height == 0 ? (imageReady ? (imageCrop.bottom - imageCrop.top) * zoom : textHeight)
                                    : units(layer.height, true);
            float x = units(layer.x, false) + static_cast<float>(edit.x) * zoom;
            float y = units(layer.y, true) + static_cast<float>(edit.y) * zoom;
            x -= width * static_cast<float>(layer.origin % 3) / 2.0f;
            y -= height * static_cast<float>(layer.origin / 3) / 2.0f;
            placed.push_back(
                { Rect { x, y, x + width, y + height }, text, imageCrop, graph, fillGraph, imageReady });
            leftEdge = std::min(leftEdge, x);
            topEdge = std::min(topEdge, y);
            right = std::max(right, x + width);
            bottom = std::max(bottom, y + height);
        }
        if (right <= leftEdge || bottom <= topEdge || formats.factory == nullptr) {
            return { };
        }
        const float fit = std::min({ 1.0f, std::max(1.0f, bounds.width() - 4.0f) / (right - leftEdge),
            std::max(1.0f, bounds.height() - 4.0f) / (bottom - topEdge) });
        if (layerScale != nullptr) {
            *layerScale = zoom * fit;
        }
        const float width = (right - leftEdge) * fit;
        const float height = (bottom - topEdge) * fit;
        const bool onRight = settings.position == OverlayPosition::TopRight
            || settings.position == OverlayPosition::BottomRight;
        const bool onBottom = settings.position == OverlayPosition::BottomLeft
            || settings.position == OverlayPosition::BottomRight;
        const float anchorX = settings.anchorMode == OverlayAnchorMode::Free
            ? bounds.left + static_cast<float>(settings.originX)
            : (onRight ? bounds.right - width - 18.0f : bounds.left + 18.0f)
                + static_cast<float>(settings.offsetX);
        const float anchorY = settings.anchorMode == OverlayAnchorMode::Free
            ? bounds.top + static_cast<float>(settings.originY)
            : (onBottom ? bounds.bottom - height - 18.0f : bounds.top + 18.0f)
                + static_cast<float>(settings.offsetY);
        const float left = std::clamp(anchorX, bounds.left, std::max(bounds.left, bounds.right - width));
        const float top = std::clamp(anchorY, bounds.top, std::max(bounds.top, bounds.bottom - height));

        ID2D1Bitmap* bitmap = nullptr;
        if (image) {
            const auto properties = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            target->CreateBitmap(D2D1::SizeU(image->width, image->height), image->pixels.data(),
                image->width * 4, &properties, &bitmap);
        }
        std::array<IDWriteTextFormat*, 401> fonts { };
        D2D1::Matrix3x2F oldTransform;
        target->GetTransform(&oldTransform);
        target->SetTransform(D2D1::Matrix3x2F::Scale(fit, fit, D2D1::Point2F(left, top)) * oldTransform);
        const float opacity = static_cast<float>(std::clamp(settings.opacityPercent, 0, 100)) / 100.0f;
        for (std::size_t index = 0; index < layout.layers.size(); ++index) {
            const RtssVisualLayer& layer = layout.layers[index];
            const RtssLayerEdit& edit = settings.rtssLayerEdits[index];
            const std::uint32_t color = edit.textColor.value_or(layer.textColor);
            const int textSize = edit.size == 0 ? layer.size : edit.size;
            const RtssPlacement& placement = placed[index];
            if (placement.rect.right <= placement.rect.left) {
                continue;
            }
            if (layerBounds != nullptr) {
                (*layerBounds)[index] = Rect { left + fit * (placement.rect.left - leftEdge),
                    top + fit * (placement.rect.top - topEdge),
                    left + fit * (placement.rect.right - leftEdge),
                    top + fit * (placement.rect.bottom - topEdge) };
            }
            const D2D1_RECT_F rect
                = D2D1::RectF(left + placement.rect.left - leftEdge, top + placement.rect.top - topEdge,
                    left + placement.rect.right - leftEdge, top + placement.rect.bottom - topEdge);
            const std::uint32_t background = edit.backgroundColor.value_or(layer.backgroundColor);
            if ((edit.backgroundColor.has_value() || layer.hasBackground) && background >> 24 != 0) {
                ID2D1SolidColorBrush* brush = nullptr;
                if (SUCCEEDED(target->CreateSolidColorBrush(toD2D(fromRgba(background), opacity), &brush))) {
                    target->FillRectangle(rect, brush);
                    brush->Release();
                }
            }
            if (placement.image) {
                if (bitmap) {
                    target->DrawBitmap(
                        bitmap, &rect, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &placement.imageCrop);
                }
                continue;
            }
            if (placement.graph >= 0) {
                const auto& history = values.rtssGraphHistory[static_cast<std::size_t>(placement.graph)];
                const auto highest = std::max_element(history.begin(), history.end());
                const float ceiling = std::max(placement.graph == 0 ? 33.0f
                        : placement.graph == 1                      ? 1.0f
                                                                    : 100.0f,
                    *highest);
                ID2D1SolidColorBrush* brush = nullptr;
                if (SUCCEEDED(target->CreateSolidColorBrush(toD2D(fromRgba(color), opacity), &brush))) {
                    const auto point = [&](std::size_t sample) {
                        const float x = rect.left + (rect.right - rect.left)
                            * static_cast<float>(sample) / static_cast<float>(history.size() - 1);
                        const float fraction = std::clamp(history[sample] / ceiling, 0.0f, 1.0f);
                        return D2D1::Point2F(x, rect.bottom - fraction * (rect.bottom - rect.top));
                    };
                    for (std::size_t sample = 1; sample < history.size(); ++sample) {
                        const D2D1_POINT_2F from = point(sample - 1);
                        const D2D1_POINT_2F to = point(sample);
                        if (placement.fillGraph) {
                            target->FillRectangle(D2D1::RectF(from.x, std::min(from.y, to.y),
                                std::max(from.x + 1.0f, to.x), rect.bottom), brush);
                        } else {
                            target->DrawLine(from, to, brush, std::max(1.0f, zoom));
                        }
                    }
                    brush->Release();
                }
                continue;
            }
            if (placement.text.empty()) {
                continue;
            }
            IDWriteTextFormat*& format = fonts[static_cast<std::size_t>(textSize)];
            if (format == nullptr) {
                const float size = cellHeight * static_cast<float>(textSize) / 100.0f;
                if (FAILED(formats.factory->CreateTextFormat(family.c_str(), nullptr,
                        static_cast<DWRITE_FONT_WEIGHT>(layout.fontWeight), DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL, size, L"ru-RU", &format))) {
                    continue;
                }
                format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
            ID2D1SolidColorBrush* brush = nullptr;
            if (SUCCEEDED(target->CreateSolidColorBrush(toD2D(fromRgba(color), opacity), &brush))) {
                target->DrawTextW(placement.text.data(), static_cast<UINT32>(placement.text.size()), format,
                    rect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                brush->Release();
            }
        }
        target->SetTransform(oldTransform);
        if (bitmap) {
            bitmap->Release();
        }
        for (IDWriteTextFormat* format : fonts) {
            if (format != nullptr) {
                format->Release();
            }
        }
        return Rect { left, top, left + width * fit, top + height * fit };
    }

    void drawInline(Painter& painter, const OverlaySettings& settings, const OverlayText& text,
        const Rect& frame, const StyleLook& look)
    {
        const float scale = painter.scale();
        const Rect inner = Rect { frame.left + look.padding * scale, frame.top,
            frame.right - look.padding * scale, frame.bottom };
        const TextRole role = look.mono ? TextRole::Mono : TextRole::Small;

        if (settings.style != OverlayStyle::Rtss || settings.surfaceActive) {
            painter.plate(frame, look.radius * scale, look.background, look.shadow ? 0.72f : 0.35f);
            painter.outline(frame, look.radius * scale, look.border,
                settings.style == OverlayStyle::Neon ? 1.4f : 1.0f, 0.85f);
        }
        float x = inner.left;
        const float separator = painter.measure(tracked(L"   ", look.letterSpacing), role);
        for (std::size_t index = 0; index < text.labels.size(); ++index) {
            if (!rowShown(text, index)) {
                continue;
            }
            const std::wstring label = look.uppercase ? upperCase(text.labels[index]) : text.labels[index];
            const std::wstring segment = tracked(label + L" " + text.values[index], look.letterSpacing);
            painter.beginTextSize(text.ids[index]);
            const float width = painter.measure(segment, role);
            painter.endTextSize();
            painter.beginRow(text.ids[index], Rect { x, inner.top, x + width + 4.0f, inner.bottom });
            const auto group = overlayColorGroup(text.ids[index]);
            const bool customized = settings.metricColors[static_cast<std::size_t>(text.ids[index])] >= 0
                || settings.metricRgba[static_cast<std::size_t>(text.ids[index])] || settings.valueColor >= 0
                || settings.valueRgba || settings.rowColors[index % kLabelCount] >= 0
                || (group && settings.rowRgba[*group]);
            painter.text(segment, Rect { x, inner.top, x + width + 4.0f, inner.bottom }, role,
                settings.style == OverlayStyle::Rtss && !customized ? look.accent : look.rowPalette[index]);
            painter.endRow();
            x += width + separator;
        }
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

// Палитра вынесена из анонимного namespace: она объявлена в заголовке и нужна редактору.
std::array<Color, kOverlayColorCount> overlayColorPalette()
{
    return { styleColor(0xFFFFFFu), styleColor(0x5EF29Bu), styleColor(0x6AA8FFu), styleColor(0xB47BFFu),
        styleColor(0xFFB347u), styleColor(0xFF6F6Fu) };
}

OverlayText makeOverlayText(const OverlaySettings& settings, const MetricStore& store)
{
    OverlayText text;
    if (settings.recipe == OverlayRecipe::Rtss && settings.rtssTemplate && settings.rtssVisual) {
        const std::array<MetricId, 4> graphMetrics { MetricId::FrameTime, MetricId::Low1Fps,
            MetricId::CpuLoad, MetricId::GpuLoad };
        for (std::size_t index = 0; index < graphMetrics.size(); ++index) {
            if (!store.hasValue(graphMetrics[index])) {
                continue;
            }
            std::array<float, 32> history { };
            const std::size_t count = store.copyHistory(graphMetrics[index], history.data(), history.size());
            text.rtssGraphHistory[index].assign(history.begin(),
                history.begin() + static_cast<std::ptrdiff_t>(count));
        }
    }
    if (settings.showSparkline && store.hasValue(settings.sparklineMetric)) {
        std::array<float, 32> history { };
        const std::size_t count = store.copyHistory(settings.sparklineMetric, history.data(), history.size());
        text.sparklineHistory.assign(history.begin(), history.begin() + static_cast<std::ptrdiff_t>(count));
    }
    for (const OverlayMetricRow& row : overlayMetricRows(settings, store)) {
        text.labels.push_back(wideLabel(row.label));
        text.values.push_back(wideLabel(row.value));
        text.visible.push_back(true);
        text.ids.push_back(row.id);
        const bool percentage = row.id == MetricId::CpuLoad || row.id == MetricId::GpuLoad;
        const double value = percentage && store.hasValue(row.id) ? store.value(row.id) : -1.0;
        text.gaugeFill.push_back(percentage && std::isfinite(value) && value >= 0.0
                ? static_cast<float>(std::clamp(value / 100.0, 0.0, 1.0))
                : -1.0f);
    }
    return text;
}

Rect paintOverlay(ID2D1RenderTarget* target, const OverlaySettings& settings, const Theme& theme,
    const OverlayText& text, const Rect& bounds, float scale, const OverlayFormats& formats,
    std::vector<Rect>* rtssLayerBounds, float* rtssLayerScale, std::vector<OverlayRowBounds>* rowBounds,
    float* rowScale)
{
    if (rowScale != nullptr) {
        *rowScale = 1.0f;
    }
    if (rowBounds != nullptr) {
        rowBounds->clear();
    }
    if (rtssLayerScale != nullptr) {
        *rtssLayerScale = 1.0f;
    }
    if (rtssLayerBounds != nullptr) {
        rtssLayerBounds->clear();
    }
    if (target == nullptr) {
        return Rect { };
    }
    if (settings.recipe == OverlayRecipe::Rtss && settings.rtssVisual && settings.rtssTemplate) {
        return drawRtssVisual(target, *settings.rtssVisual, text, settings, bounds, scale, formats,
            rtssLayerBounds, rtssLayerScale);
    }
    if (visibleRows(text) == 0) {
        return Rect { };
    }

    const float opacity = static_cast<float>(std::clamp(settings.opacityPercent, 0, 100)) / 100.0f;
    const StyleLook preset = lookFor(settings.style);
    StyleLook look = preset;
    if (settings.surfaceActive && settings.style == OverlayStyle::Rtss) {
        // Старый RTSS прозрачен; новая ось поверхности может запросить подложку.
        look.background = styleColor(0x101820u);
        look.border = look.accent;
    }
    if (settings.surfaceActive && settings.surface == OverlaySurface::Plate) {
        look.background.a = 1.0f;
    }
    // Ручные подстройки: отступы, интервал, цвета и типографика. -1 — как в пресете.
    if (settings.paddingPx >= 0) {
        look.padding = static_cast<float>(settings.paddingPx);
    }
    if (settings.rowGapPx >= 0) {
        look.rowGap = static_cast<float>(settings.rowGapPx);
    }
    const std::array<Color, kOverlayColorCount> palette = overlayColorPalette();
    const auto pickColor = [&palette](int index) { return palette[static_cast<std::size_t>(index)]; };
    const auto customColor
        = [](std::uint32_t rgba) { return fromRgba((rgba >> 8) | ((rgba & 0xFFu) << 24)); };
    if (settings.valueColor >= 0 && settings.valueColor < kOverlayColorCount) {
        look.value = pickColor(settings.valueColor);
    }
    if (settings.labelColor >= 0 && settings.labelColor < kOverlayColorCount) {
        look.label = pickColor(settings.labelColor);
    }
    if (settings.accentColor >= 0 && settings.accentColor < kOverlayColorCount) {
        look.accent = pickColor(settings.accentColor);
    }
    if (settings.valueRgba)
        look.value = customColor(*settings.valueRgba);
    if (settings.labelRgba)
        look.label = customColor(*settings.labelRgba);
    if (settings.accentRgba)
        look.accent = customColor(*settings.accentRgba);
    // Цвет каждой строки: в демо и в пресетах строки раскрашены по-разному.
    for (std::size_t index = 0; index < kLabelCount; ++index) {
        const int color = settings.rowColors[index];
        if (color >= 0 && color < kOverlayColorCount) {
            look.palette[index] = pickColor(color);
        }
        if (settings.rowRgba[index])
            look.palette[index] = customColor(*settings.rowRgba[index]);
    }
    for (std::size_t index = 0; index < text.ids.size() && index < kMetricCount; ++index) {
        const int metric = settings.metricColors[static_cast<std::size_t>(text.ids[index])];
        const int row = settings.rowColors[index % kLabelCount];
        const auto& exact = settings.metricRgba[static_cast<std::size_t>(text.ids[index])];
        const auto groupIndex = overlayColorGroup(text.ids[index]);
        const auto group = groupIndex ? settings.rowRgba[*groupIndex] : std::nullopt;
        look.rowPalette[index] = exact                   ? customColor(*exact)
            : metric >= 0 && metric < kOverlayColorCount ? pickColor(metric)
            : group                                      ? customColor(*group)
            : row >= 0 && row < kOverlayColorCount       ? pickColor(row)
            : settings.valueRgba                         ? customColor(*settings.valueRgba)
            : settings.valueColor >= 0 && settings.valueColor < kOverlayColorCount
            ? pickColor(settings.valueColor)
            : look.palette[index % kLabelCount];
    }
    if (!text.ids.empty()) {
        const auto group = overlayColorGroup(text.ids[0]);
        if (settings.metricColors[static_cast<std::size_t>(text.ids[0])] >= 0
            || settings.metricRgba[static_cast<std::size_t>(text.ids[0])]
            || (group && settings.rowRgba[*group])) {
            look.value = look.rowPalette[0];
        }
    }
    // Регистр и разделители: -1 — из стиля, иначе явное значение пользователя.
    if (settings.uppercaseMode >= 0) {
        look.uppercase = settings.uppercaseMode == 1;
    }
    if (settings.separatorsMode >= 0) {
        look.separators = settings.separatorsMode == 1;
    }
    look.letterSpacing *= static_cast<float>(std::clamp(settings.letterSpacing, 0, 200)) / 100.0f;
    // Увеличенный шрифт требует большего шага строк: иначе цифры перекрывают друг друга.
    if (settings.layout == OverlayLayout::Plates && settings.textScalePercent > 100) {
        look.rowGap += static_cast<float>(settings.textScalePercent - 100) * 0.18f;
    }
    // Размер шрифта отдельно от масштаба плашки: 85…130 %.
    const float textFactor = static_cast<float>(std::clamp(settings.textScalePercent, 85, 130)) / 100.0f;
    const float backgroundOpacity = settings.backgroundOpacityPercent >= 0
        ? static_cast<float>(std::clamp(settings.backgroundOpacityPercent, 0, 100)) / 100.0f
        : 1.0f;

    // Шрифты масштабируются вместе с рамкой, а семейство, начертание и обводка берутся
    // из редактора текста: иначе «Масштаб» растягивал только подложку.
    std::array<IDWriteTextFormat*, 4> owned { };
    OverlayFormats scaled = formats;
    const bool customFont = settings.fontFamily != 0 || settings.fontBold || settings.fontItalic;
    const float textScale = (scale > 0.0f ? scale : 1.0f) * textFactor;
    if (formats.factory != nullptr && (customFont || textScale < 0.999f || textScale > 1.001f)) {
        const float factor = textScale;
        owned = { styledFormat(formats.factory, formats.key, factor, settings),
            styledFormat(formats.factory, formats.value, factor, settings),
            styledFormat(formats.factory, formats.smallText, factor, settings),
            styledFormat(formats.factory, formats.mono, factor, settings) };
        if (owned[0] != nullptr && owned[1] != nullptr && owned[2] != nullptr && owned[3] != nullptr) {
            scaled = OverlayFormats { formats.factory, owned[0], owned[1], owned[2], owned[3] };
        } else {
            for (IDWriteTextFormat* format : owned) {
                if (format != nullptr) {
                    format->Release();
                }
            }
            owned = { };
            scaled = formats;
        }
    }

    OverlayChrome chrome;
    chrome.background = settings.showBackground;
    chrome.border = settings.showBorder;
    chrome.shadow = settings.showShadow;
    chrome.radius = settings.cornerRadius >= 0 ? static_cast<float>(settings.cornerRadius) : -1.0f;
    chrome.borderWidth = settings.borderWidth >= 0 ? static_cast<float>(settings.borderWidth) : 1.0f;
    chrome.shadowStrength = static_cast<float>(std::clamp(settings.shadowStrength, 0, 100)) / 100.0f;

    Painter measurer(target, theme, scaled, scale, opacity, chrome, settings, settings.textOutline);
    measurer.setBackgroundOpacity(backgroundOpacity);
    const TextRole inlineRole = look.mono ? TextRole::Mono : TextRole::Small;
    float lineWidth = 0.0f;
    for (std::size_t index = 0; index < text.labels.size(); ++index) {
        if (!rowShown(text, index)) {
            continue;
        }
        if (lineWidth > 0.0f) {
            lineWidth += measurer.measure(tracked(L"   ", look.letterSpacing), inlineRole) / scale;
        }
        const std::wstring label = look.uppercase ? upperCase(text.labels[index]) : text.labels[index];
        measurer.beginTextSize(text.ids[index]);
        lineWidth
            += measurer.measure(tracked(label + L" " + text.values[index], look.letterSpacing), inlineRole)
            / scale;
        measurer.endTextSize();
    }
    StyleSpec spec = styleSpec(settings, text, lineWidth);
    const auto labels = rowLabels(text, look);
    if (settings.layout == OverlayLayout::Plates && settings.style == OverlayStyle::Esport) {
        float cellsWidth = look.padding * 2.0f;
        for (std::size_t index = 0; index < text.labels.size(); ++index) {
            if (rowShown(text, index)) {
                measurer.beginTextSize(text.ids[index]);
                cellsWidth += std::max(measurer.measure(labels[index], TextRole::Tiny),
                                  measurer.measure(
                                      text.values[index], index == 0 ? TextRole::Value : TextRole::Small))
                        / scale
                    + 20.0f;
                measurer.endTextSize();
            }
        }
        spec.width = std::max(spec.width, cellsWidth);
    }
    for (std::size_t index = 0; index < text.labels.size(); ++index) {
        if (!rowShown(text, index)) {
            continue;
        }
        const auto role = look.mono ? TextRole::Mono : TextRole::Small;
        measurer.beginTextSize(text.ids[index]);
        look.labelWidth = std::max(look.labelWidth, measurer.measure(labels[index], role) / scale + 12.0f);
        look.valueWidth
            = std::max(look.valueWidth, measurer.measure(text.values[index], role) / scale + 12.0f);
        measurer.endTextSize();
    }
    if (settings.layout == OverlayLayout::Plates && settings.style != OverlayStyle::Esport) {
        spec.width = std::max(spec.width, look.labelWidth + look.valueWidth + look.padding * 2.0f + 12.0f);
    }

    // Плашка растёт вместе с отступами, интервалом строк и размером шрифта:
    // иначе содержимое вылезало бы за её край.
    if (settings.layout == OverlayLayout::Plates) {
        const float rows = static_cast<float>(std::max<std::size_t>(visibleRows(text), 1) - 1);
        if (settings.paddingPx >= 0) {
            const float delta = look.padding - preset.padding;
            spec.width += delta * 2.0f;
            spec.height += delta * 2.0f;
        }
        if (settings.rowGapPx >= 0) {
            spec.height += (look.rowGap - preset.rowGap) * rows;
        }
        if (textFactor > 1.0f) {
            spec.width *= 0.72f + 0.28f * textFactor;
            spec.height *= 0.82f + 0.18f * textFactor;
        }
    }
    if (settings.layout == OverlayLayout::Plates && settings.style != OverlayStyle::Esport) {
        const bool header = rowShown(text, 0)
            && (settings.style == OverlayStyle::Frosted || settings.style == OverlayStyle::Neon
                || settings.style == OverlayStyle::Cyber);
        const float bodyRows = static_cast<float>(visibleRows(text) - (header ? 1 : 0));
        const float head = header
            ? 70.0f
            : (settings.style == OverlayStyle::Terminal || settings.style == OverlayStyle::Retro ? 28.0f
                                                                                                 : 8.0f);
        const float step = settings.style == OverlayStyle::Rtss ? 19.0f : 18.0f + look.rowGap;
        spec.height = std::max(spec.height, head + bodyRows * step + look.padding + 8.0f);
    }
    if (settings.pixelLayout && settings.layout == OverlayLayout::Plates && !text.ids.empty()) {
        spec.width = 520.0f;
        spec.height = 80.0f;
        for (std::size_t i = 0; i < text.ids.size(); ++i) {
            if (!rowShown(text, i)) {
                continue;
            }
            const auto& row = settings.rowPositions[static_cast<std::size_t>(text.ids[i])];
            spec.height = std::max(
                spec.height, static_cast<float>((row.y >= 0 ? row.y : 12 + static_cast<int>(i) * 28) + 30));
        }
    }
    const bool hasSparkline = settings.showSparkline && text.sparklineHistory.size() >= 2
        && std::all_of(text.sparklineHistory.begin(), text.sparklineHistory.end(),
            [](float value) { return std::isfinite(value) && value >= 0.0f; });
    if (hasSparkline) {
        spec.height += 30.0f;
    }
    if (settings.layout == OverlayLayout::Plates) {
        if (settings.backgroundWidthPx >= 80)
            spec.width = static_cast<float>(settings.backgroundWidthPx);
        if (settings.backgroundHeightPx >= 40)
            spec.height = static_cast<float>(settings.backgroundHeightPx);
    }
    const float width = spec.width * scale;
    const float height = spec.height * scale;
    // Не обрезаем длинный список границами монитора или предпросмотра.
    const float fit = std::min({ 1.0f, std::max(bounds.width() - 4.0f, 1.0f) / width,
        std::max(bounds.height() - 4.0f, 1.0f) / height });
    if (rowScale != nullptr) {
        *rowScale = fit * scale;
    }
    const float fittedWidth = width * fit;
    const float fittedHeight = height * fit;

    const float margin = kMargin * scale;
    const bool isLeft
        = settings.position == OverlayPosition::TopLeft || settings.position == OverlayPosition::BottomLeft;
    const bool isTop
        = settings.position == OverlayPosition::TopLeft || settings.position == OverlayPosition::TopRight;

    // Смещение — в экранных пикселях и не зависит от масштаба: раньше смена масштаба
    // сдвигала оверлей из-за умножения сдвига на коэффициент.
    float left = settings.anchorMode == OverlayAnchorMode::Free
        ? bounds.left + static_cast<float>(settings.originX)
        : isLeft ? bounds.left + margin + static_cast<float>(settings.offsetX)
                 : bounds.right - fittedWidth - margin + static_cast<float>(settings.offsetX);
    float top = settings.anchorMode == OverlayAnchorMode::Free
        ? bounds.top + static_cast<float>(settings.originY)
        : isTop ? bounds.top + margin + static_cast<float>(settings.offsetY)
                : bounds.bottom - fittedHeight - margin + static_cast<float>(settings.offsetY);

    left = std::clamp(left, bounds.left, std::max(bounds.left, bounds.right - fittedWidth));
    top = std::clamp(top, bounds.top, std::max(bounds.top, bounds.bottom - fittedHeight));

    D2D1::Matrix3x2F previousTransform;
    target->GetTransform(&previousTransform);
    target->SetTransform(D2D1::Matrix3x2F::Scale(fit, fit, D2D1::Point2F(left, top)) * previousTransform);
    const Rect frame { left, top, left + width, top + height };
    Painter painter(target, theme, scaled, scale, opacity, chrome, settings, settings.textOutline);
    painter.setBackgroundOpacity(backgroundOpacity);
    painter.setRowBounds(settings, rowBounds, frame, fit);

    if (settings.pixelLayout && settings.layout == OverlayLayout::Plates && !text.ids.empty()) {
        painter.plate(frame, look.radius * scale, look.background);
        painter.outline(frame, look.radius * scale, look.border, 1.0f);
        for (std::size_t i = 0; i < text.ids.size(); ++i) {
            if (!rowShown(text, i)) {
                continue;
            }
            const auto& row = settings.rowPositions[static_cast<std::size_t>(text.ids[i])];
            const float y
                = frame.top + static_cast<float>(row.y >= 0 ? row.y : 12 + static_cast<int>(i) * 28) * scale;
            const int labelEnd = row.labelX == row.valueX ? row.labelX
                : row.labelX < row.valueX                 ? std::min(row.labelX + 230, row.valueX - 8)
                                                          : 505;
            const int valueEnd = row.valueX < row.labelX ? row.labelX - 8 : 505;
            if (rowBounds != nullptr) {
                rowBounds->push_back(OverlayRowBounds { text.ids[i],
                    Rect { left + fit * std::min(row.labelX, row.valueX) * scale, top + fit * (y - top),
                        left + fit * std::max(labelEnd, valueEnd) * scale,
                        top + fit * (y + 26.0f * scale - top) } });
            }
            painter.beginTextSize(text.ids[i]);
            if (labelEnd > row.labelX) {
                painter.text(text.labels[i],
                    Rect { frame.left + row.labelX * scale, y, frame.left + labelEnd * scale,
                        y + 26.0f * scale },
                    TextRole::Small, look.label);
            }
            if (valueEnd > row.valueX) {
                painter.text(text.values[i],
                    Rect { frame.left + row.valueX * scale, y, frame.left + valueEnd * scale,
                        y + 26.0f * scale },
                    TextRole::Small, look.rowPalette[i]);
            }
            painter.endTextSize();
        }
    } else if (settings.layout == OverlayLayout::Line) {
        drawInline(painter, settings, text, frame, look);
    } else if (settings.layout == OverlayLayout::Gauge) {
        drawGauge(painter, text, frame, look);
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

    if (hasSparkline) {
        const auto [low, high]
            = std::minmax_element(text.sparklineHistory.begin(), text.sparklineHistory.end());
        const float range = std::max(*high - *low, 1.0f);
        const float span = frame.width() - 2.0f * look.padding * scale;
        for (std::size_t index = 1; index < text.sparklineHistory.size(); ++index) {
            const float x1 = frame.left + look.padding * scale
                + span * static_cast<float>(index - 1) / static_cast<float>(text.sparklineHistory.size() - 1);
            const float x2 = frame.left + look.padding * scale
                + span * static_cast<float>(index) / static_cast<float>(text.sparklineHistory.size() - 1);
            const float y1
                = frame.bottom - (8.0f + 16.0f * (text.sparklineHistory[index - 1] - *low) / range) * scale;
            const float y2
                = frame.bottom - (8.0f + 16.0f * (text.sparklineHistory[index] - *low) / range) * scale;
            painter.line(x1, y1, x2, y2, look.accent, 1.5f * scale, 0.85f);
        }
    }

    for (IDWriteTextFormat* format : owned) {
        if (format != nullptr) {
            format->Release();
        }
    }

    target->SetTransform(previousTransform);
    return Rect { left, top, left + fittedWidth, top + fittedHeight };
}

} // namespace yawa

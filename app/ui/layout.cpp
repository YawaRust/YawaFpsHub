#include "ui/layout.h"

#include "metrics/units.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace yawa {
namespace {

    float clamp01(float value)
    {
        return std::clamp(value, 0.0f, 1.0f);
    }

} // namespace

std::wstring wideText(std::string_view utf8)
{
    std::wstring result;
    result.reserve(utf8.size());

    std::size_t index = 0;
    while (index < utf8.size()) {
        const auto byte = static_cast<unsigned char>(utf8[index]);
        std::uint32_t code = byte;
        std::size_t extra = 0;

        if ((byte & 0xE0) == 0xC0) {
            code = byte & 0x1F;
            extra = 1;
        } else if ((byte & 0xF0) == 0xE0) {
            code = byte & 0x0F;
            extra = 2;
        } else if ((byte & 0xF8) == 0xF0) {
            code = byte & 0x07;
            extra = 3;
        }

        ++index;
        for (std::size_t part = 0; part < extra && index < utf8.size(); ++part, ++index) {
            code = (code << 6) | (static_cast<unsigned char>(utf8[index]) & 0x3F);
        }

        if (code <= 0xFFFF) {
            result.push_back(static_cast<wchar_t>(code));
        } else {
            code -= 0x10000;
            result.push_back(static_cast<wchar_t>(0xD800 + (code >> 10)));
            result.push_back(static_cast<wchar_t>(0xDC00 + (code & 0x3FF)));
        }
    }
    return result;
}

std::wstring wideNumber(double value, MetricUnit unit, std::uint8_t decimals)
{
    return wideText(formatWithUnit(value, unit, decimals));
}

std::wstring wideMetric(MetricId id, double value)
{
    const MetricInfo& info = metricInfo(id);
    return wideNumber(value, info.unit, info.decimals);
}

std::wstring formatClock(std::uint64_t milliseconds)
{
    const std::uint64_t totalSeconds = milliseconds / 1000;
    wchar_t buffer[32] { };
    std::swprintf(buffer, 32, L"%02llu:%02llu:%02llu", static_cast<unsigned long long>(totalSeconds / 3600),
        static_cast<unsigned long long>((totalSeconds / 60) % 60),
        static_cast<unsigned long long>(totalSeconds % 60));
    return buffer;
}

void drawCard(Renderer& renderer, const Rect& rect, const Theme& theme)
{
    renderer.fillRoundRect(rect, kCardRadius, theme.panel);
    renderer.strokeRoundRect(rect, kCardRadius, theme.line.withAlpha(0.65f), 1.0f);
}

void drawLabelValue(Renderer& renderer, const Rect& rect, std::wstring_view label, std::wstring_view value,
    const Theme& theme)
{
    const float valueWidth
        = std::min(rect.width() * 0.7f, std::max(56.0f, renderer.measureText(value, TextRole::Body) + 4.0f));
    const float split = std::clamp(rect.right - valueWidth, rect.left + 24.0f, rect.right - 24.0f);
    renderer.text(clipText(renderer, label, TextRole::Small, split - rect.left - 6.0f),
        Rect { rect.left, rect.top, split - 4.0f, rect.top + 16.0f }, TextRole::Small, theme.textFaint);
    renderer.text(clipText(renderer, value, TextRole::Body, rect.right - split),
        Rect { split, rect.top, rect.right, rect.top + 16.0f }, TextRole::Body, theme.text, TextAlign::Right);
}

void drawChip(Renderer& renderer, const Rect& rect, std::wstring_view label, bool active, const Theme& theme,
    TextRole role, bool hovered)
{
    renderer.fillRoundRect(rect, 9.0f, active ? theme.accentSoft : theme.panelSoft);
    if (active) {
        renderer.strokeRoundRect(rect, 9.0f, theme.accentGlow, 1.0f);
    } else if (hovered) {
        renderer.strokeRoundRect(rect, 9.0f, theme.lineStrong, 1.0f);
    }
    renderer.text(
        label, rect, role, active ? theme.accent : (hovered ? theme.text : theme.textDim), TextAlign::Center);
}

namespace {

    // Ряды карточек «Баланса» и «Системы»: высота снята с demo-*-tall.png, шаг списка —
    // из той же разметки прототипа.
    constexpr float kBalanceVerdict = 204.0f;
    constexpr float kBalancePair = 363.0f;
    constexpr float kBalanceAdviceRow = 57.0f;
    constexpr float kBalanceAdviceHead = 42.0f;
    constexpr float kBalanceAdviceMin = 249.0f;
    constexpr float kBalanceNote = 144.0f;
    constexpr float kBalanceDiagnosis = 300.0f;
    constexpr float kBalanceDiagnosisNarrow = 410.0f;
    constexpr float kSystemRow = 274.0f;
    constexpr float kSystemMemory = 416.0f;
    constexpr float kSystemMonitor = 190.0f;
    constexpr float kSystemDisk = 184.0f;
    constexpr float kSystemNote = 143.0f;

} // namespace

float balanceHeight(std::size_t adviceCount, bool narrow)
{
    const float advice = std::max(
        kBalanceAdviceMin, kBalanceAdviceHead + kBalanceAdviceRow * static_cast<float>(adviceCount) + 12.0f);
    const float extraRow = narrow ? kBalancePair + kGap : 0.0f;
    return 8.0f + kBalanceVerdict + kGap + kBalancePair + kGap + advice + kGap
        + (narrow ? kBalanceDiagnosisNarrow : kBalanceDiagnosis) + kGap + kBalanceNote + extraRow + 8.0f;
}

float systemHeight(bool narrow)
{
    if (narrow) {
        return 8.0f + 3.0f * (kSystemRow + kGap) + kSystemMemory + kGap + kSystemMonitor + kGap + kSystemDisk
            + kGap + kSystemNote + 8.0f;
    }
    return 8.0f + kSystemRow + kGap + kSystemMemory + kGap + kSystemDisk + kGap + kSystemNote + 8.0f;
}

float drawCardHeader(Renderer& renderer, const Rect& card, std::wstring_view title,
    std::wstring_view subtitle, const Theme& theme)
{
    const Rect inner = card.inset(kCardPadding);
    renderer.text(
        title, Rect { inner.left, inner.top, inner.right, inner.top + 20.0f }, TextRole::Heading, theme.text);
    if (!subtitle.empty()) {
        renderer.text(subtitle,
            Rect { inner.left, inner.top + 18.0f, inner.right - 76.0f, inner.top + 34.0f }, TextRole::Tiny,
            theme.textFaint);
    }
    return inner.top + 36.0f;
}

void drawTag(Renderer& renderer, const Rect& card, std::wstring_view label, bool live, const Theme& theme)
{
    const float width = renderer.measureText(label, TextRole::Tiny) + 30.0f;
    const Rect tag { card.right - kCardPadding - width, card.top + kCardPadding - 2.0f,
        card.right - kCardPadding, card.top + kCardPadding + 20.0f };
    renderer.fillRoundRect(tag, 11.0f, live ? theme.accentSoft : theme.panelSoft);
    renderer.fillCircle(tag.left + 11.0f, tag.centerY(), 3.0f, live ? theme.accent : theme.textFaint);
    renderer.text(label, Rect { tag.left + 20.0f, tag.top, tag.right - 6.0f, tag.bottom }, TextRole::Tiny,
        live ? theme.accent : theme.textFaint);
}

std::wstring clipText(Renderer& renderer, std::wstring_view text, TextRole role, float maxWidth)
{
    if (text.empty() || renderer.measureText(text, role) <= maxWidth) {
        return std::wstring(text);
    }

    std::wstring clipped;
    for (const wchar_t symbol : text) {
        if (renderer.measureText(clipped + symbol + L"…", role) > maxWidth) {
            break;
        }
        clipped.push_back(symbol);
    }
    return clipped.empty() ? std::wstring(L"…") : clipped + L"…";
}

float drawWrappedText(Renderer& renderer, std::wstring_view text, const Rect& rect, TextRole role,
    const Color& color, float lineHeight, std::uint32_t maxLines)
{
    if (text.empty()) {
        return 0.0f;
    }

    // Переносим по пробелам: в DirectWrite формат создан без переноса, поэтому длинная
    // строка иначе просто обрезалась бы по ширине карточки.
    std::wstring line;
    std::wstring word;
    std::uint32_t lines = 0;
    for (const wchar_t symbol : text) {
        if (symbol == L'\n') {
            ++lines;
            if (lines >= maxLines) {
                renderer.text(line + word, rect, role, color);
                return static_cast<float>(lines) * lineHeight;
            }
            renderer.text(line + word,
                Rect { rect.left, rect.top + static_cast<float>(lines - 1) * lineHeight, rect.right,
                    rect.top + static_cast<float>(lines) * lineHeight },
                role, color);
            line.clear();
            word.clear();
            continue;
        }
        if (symbol != L' ') {
            word.push_back(symbol);
            continue;
        }

        const std::wstring candidate = line.empty() ? word : line + L" " + word;
        if (renderer.measureText(candidate, role) > rect.width() && !line.empty()) {
            ++lines;
            if (lines >= maxLines) {
                line += L"…";
                break;
            }
            renderer.text(line,
                Rect { rect.left, rect.top + static_cast<float>(lines - 1) * lineHeight, rect.right,
                    rect.top + static_cast<float>(lines) * lineHeight },
                role, color);
            line = word;
        } else {
            line = candidate;
        }
        word.clear();
    }

    if (!word.empty()) {
        const std::wstring candidate = line.empty() ? word : line + L" " + word;
        if (renderer.measureText(candidate, role) > rect.width() && !line.empty() && lines + 1 < maxLines) {
            ++lines;
            renderer.text(line,
                Rect { rect.left, rect.top + static_cast<float>(lines - 1) * lineHeight, rect.right,
                    rect.top + static_cast<float>(lines) * lineHeight },
                role, color);
            line = word;
        } else {
            line = candidate;
        }
    }

    ++lines;
    renderer.text(line,
        Rect { rect.left, rect.top + static_cast<float>(lines - 1) * lineHeight, rect.right,
            rect.top + static_cast<float>(lines) * lineHeight },
        role, color);
    return static_cast<float>(lines) * lineHeight;
}

void drawBar(Renderer& renderer, const Rect& rect, float fraction, const Color& color, const Theme& theme)
{
    const float radius = rect.height() * 0.5f;
    renderer.fillRoundRect(rect, radius, theme.line);

    const float width = rect.width() * clamp01(fraction);
    if (width > 1.0f) {
        renderer.fillRoundRect(Rect { rect.left, rect.top, rect.left + width, rect.bottom }, radius, color);
    }
}

void drawGauge(Renderer& renderer, float centerX, float centerY, float radius, float fraction,
    const Color& color, const Color& track, std::wstring_view value, const Color& valueColor)
{
    renderer.strokeCircle(centerX, centerY, radius, track, 8.0f);
    renderer.drawArc(centerX, centerY, radius, 8.0f, clamp01(fraction), color);

    const Rect valueRect { centerX - radius, centerY - 16.0f, centerX + radius, centerY + 14.0f };
    renderer.text(value, valueRect, TextRole::Value, valueColor, TextAlign::Center);
}

} // namespace yawa

#include "ui/layout.h"

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
    wchar_t buffer[32]{};
    std::swprintf(buffer, 32, L"%02llu:%02llu:%02llu", static_cast<unsigned long long>(totalSeconds / 3600),
        static_cast<unsigned long long>((totalSeconds / 60) % 60),
        static_cast<unsigned long long>(totalSeconds % 60));
    return buffer;
}

void drawCard(Renderer& renderer, const Rect& rect, const Theme& theme)
{
    renderer.fillRoundRect(rect, kCardRadius, theme.panel);
    renderer.strokeRoundRect(rect, kCardRadius, theme.line.withAlpha(0.85f), 1.0f);
}

void drawLabelValue(Renderer& renderer, const Rect& rect, std::wstring_view label, std::wstring_view value,
    const Theme& theme)
{
    renderer.text(label, Rect{ rect.left, rect.top, rect.centerX(), rect.top + 16.0f }, TextRole::Small,
        theme.textFaint);
    renderer.text(value, Rect{ rect.centerX(), rect.top, rect.right, rect.top + 16.0f }, TextRole::Body,
        theme.text, TextAlign::Right);
}

void drawBar(Renderer& renderer, const Rect& rect, float fraction, const Color& color, const Color& trackColor)
{
    const float radius = rect.height() * 0.5f;
    renderer.fillRoundRect(rect, radius, trackColor);

    const float width = rect.width() * clamp01(fraction);
    if (width > 1.0f) {
        renderer.fillRoundRect(Rect{ rect.left, rect.top, rect.left + width, rect.bottom }, radius, color);
    }
}

void drawGauge(Renderer& renderer, float centerX, float centerY, float radius, float fraction,
    const Color& color, const Color& trackColor, std::wstring_view value, const Color& valueColor)
{
    renderer.drawArc(centerX, centerY, radius, 8.0f, 1.0f, trackColor);
    renderer.drawArc(centerX, centerY, radius, 8.0f, clamp01(fraction), color);

    const Rect valueRect{ centerX - radius, centerY - 16.0f, centerX + radius, centerY + 14.0f };
    renderer.text(value, valueRect, TextRole::Value, valueColor, TextAlign::Center);
}

} // namespace yawa

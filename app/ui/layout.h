#pragma once

#include "metrics/metric_id.h"
#include "metrics/units.h"
#include "render/d2d.h"
#include "render/theme.h"

#include <string>
#include <string_view>

namespace yawa {

constexpr float kCardRadius = 14.0f;
constexpr float kGap = 12.0f;
constexpr float kCardPadding = 16.0f;

// Строка UI-текста: Direct2D принимает wide-строки, а числа приходят из портируемого
// форматтера в UTF-8, поэтому конвертация собрана в одном месте.
std::wstring wideNumber(double value, MetricUnit unit, std::uint8_t decimals);
std::wstring wideMetric(MetricId id, double value);
std::wstring wideText(std::string_view utf8);

void drawCard(Renderer& renderer, const Rect& rect, const Theme& theme);
void drawLabelValue(Renderer& renderer, const Rect& rect, std::wstring_view label, std::wstring_view value,
    const Theme& theme);
void drawBar(Renderer& renderer, const Rect& rect, float fraction, const Color& color, const Color& trackColor);
void drawGauge(Renderer& renderer, float centerX, float centerY, float radius, float fraction,
    const Color& color, const Color& trackColor, std::wstring_view value, const Color& valueColor);
std::wstring formatClock(std::uint64_t milliseconds);

} // namespace yawa

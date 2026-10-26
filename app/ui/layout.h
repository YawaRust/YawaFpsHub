#pragma once

#include "metrics/metric_id.h"
#include "render/d2d.h"
#include "render/theme.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace yawa {

// Высота экрана как в прототипе: содержимое не сжимается под окно, а прокручивается.
// Значения сняты с демо при ширине кадра 1044 px (см. demo-<view>-tall.png в CI).
constexpr float kDashboardHeight = 1156.0f;
// Узкое окно: сетка из трёх колонок не читается, карточки идут по одной.
constexpr float kDashboardNarrowWidth = 900.0f;
constexpr float kDashboardNarrowHeight = 2562.0f;
constexpr float kSettingsHeight = 1882.0f;
constexpr float kOverlayHeight = 1852.0f;


constexpr float kCardRadius = 20.0f;
constexpr float kGap = 14.0f;
constexpr float kCardPadding = 18.0f;

std::wstring wideText(std::string_view utf8);
std::wstring wideNumber(double value, MetricUnit unit, std::uint8_t decimals);
std::wstring wideMetric(MetricId id, double value);
std::wstring formatClock(std::uint64_t milliseconds);

// Карточка: фон, тонкая рамка и мягкая тень отсутствует — интерфейс «плоский», как в прототипе.
void drawCard(Renderer& renderer, const Rect& rect, const Theme& theme);
void drawChip(Renderer& renderer, const Rect& rect, std::wstring_view label, bool active, const Theme& theme,
    TextRole role = TextRole::Small, bool hovered = false);
void drawLabelValue(Renderer& renderer, const Rect& rect, std::wstring_view label, std::wstring_view value,
    const Theme& theme);
void drawBar(Renderer& renderer, const Rect& rect, float fraction, const Color& color, const Theme& theme);
void drawGauge(Renderer& renderer, float centerX, float centerY, float radius, float fraction, const Color& color,
    const Color& track, std::wstring_view value, const Color& valueColor);

} // namespace yawa

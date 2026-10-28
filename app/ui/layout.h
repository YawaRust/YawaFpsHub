#pragma once

#include "metrics/metric_id.h"
#include "render/d2d.h"
#include "render/theme.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace yawa {

// Высота экрана как в прототипе: содержимое не сжимается под окно, а прокручивается.
// Для «Баланса» и «Системы» высота считается функциями ниже — карточки там
// фиксированные, но список рекомендаций умеет расти.
// Значения сняты с демо при ширине кадра 1044 px (см. demo-<view>-tall.png в CI).
constexpr float kDashboardHeight = 1156.0f;
// Узкое окно: сетка из трёх колонок не читается, карточки идут по одной.
constexpr float kDashboardNarrowWidth = 900.0f;
constexpr float kDashboardNarrowHeight = 2562.0f;
constexpr float kSettingsHeight = 1926.0f;
constexpr float kOverlayHeight = 2422.0f;


constexpr float kCardRadius = 20.0f;
constexpr float kGap = 14.0f;
constexpr float kCardPadding = 18.0f;

std::wstring wideText(std::string_view utf8);
std::wstring wideNumber(double value, MetricUnit unit, std::uint8_t decimals);
std::wstring wideMetric(MetricId id, double value);
std::wstring formatClock(std::uint64_t milliseconds);

// Высота экрана «Баланс»: карточки фиксированные, растёт только список рекомендаций —
// иначе на machines с пятью советами текст упирался бы в край карточки.
float balanceHeight(std::size_t adviceCount, bool narrow);
// То же для «Системы»: там всё постоянно, только узкий окно добавляет два ряда.
float systemHeight(bool narrow);

// Карточка: фон, тонкая рамка и мягкая тень отсутствует — интерфейс «плоский», как в прототипе.
void drawCard(Renderer& renderer, const Rect& rect, const Theme& theme);
void drawChip(Renderer& renderer, const Rect& rect, std::wstring_view label, bool active, const Theme& theme,
    TextRole role = TextRole::Small, bool hovered = false);
void drawLabelValue(Renderer& renderer, const Rect& rect, std::wstring_view label, std::wstring_view value,
    const Theme& theme);
void drawBar(Renderer& renderer, const Rect& rect, float fraction, const Color& color, const Theme& theme);

// Заголовок карточки: крупное название и строка-пояснение под ним. Возвращает
// координату, с которой можно начинать содержимое.
float drawCardHeader(Renderer& renderer, const Rect& card, std::wstring_view title, std::wstring_view subtitle,
    const Theme& theme);
// Метка в правом верхнем углу карточки: «LIVE», «ОЦЕНКА», «НЕТ ДАННЫХ».
void drawTag(Renderer& renderer, const Rect& card, std::wstring_view label, bool live, const Theme& theme);

// Формат DirectWrite не переносит строки, поэтому абзацы режем сами по ширине карточки.
// Возвращает занятую высоту, чтобы следующий блок встал под текстом, а не поперёк.
// Обрезает строку по ширине с многоточием: имена путей и модулей бывают длиннее карточки.
std::wstring clipText(Renderer& renderer, std::wstring_view text, TextRole role, float maxWidth);
float drawWrappedText(Renderer& renderer, std::wstring_view text, const Rect& rect, TextRole role,
    const Color& color, float lineHeight = 15.0f, std::uint32_t maxLines = 6);
void drawGauge(Renderer& renderer, float centerX, float centerY, float radius, float fraction, const Color& color,
    const Color& track, std::wstring_view value, const Color& valueColor);

} // namespace yawa

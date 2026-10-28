#pragma once

#include "metrics/metric_store.h"
#include "render/d2d.h"
#include "render/theme.h"
#include "ui/settings.h"

#include <array>
#include <string>
#include <vector>

namespace yawa {

// Общий набор строк для предпросмотра и окна поверх игры.
struct OverlayText {
    std::vector<std::wstring> labels;
    std::vector<std::wstring> values;
    std::vector<bool> visible;
    std::vector<MetricId> ids;
    // Только показатели с определённой шкалой 0–100%; -1 означает «нет шкалы».
    std::vector<float> gaugeFill;
    std::vector<float> sparklineHistory;
};

struct OverlayFormats {
    IDWriteFactory* factory = nullptr;
    IDWriteTextFormat* key = nullptr;
    IDWriteTextFormat* value = nullptr;
    IDWriteTextFormat* smallText = nullptr;
    IDWriteTextFormat* mono = nullptr;
};

OverlayText makeOverlayText(const OverlaySettings& settings, const MetricStore& store);

// Палитра стиля для галереи в редакторе: миниатюру рисуем теми же цветами, что и оверлей.
struct OverlaySwatch {
    Color background;
    Color border;
    Color accent;
    Color value;
    float alpha = 1.0f;
    float radius = 8.0f;
    bool borderVisible = true;
    bool mono = false;
};

OverlaySwatch overlaySwatch(OverlayStyle style);

// Ручные цвета оверлея: одна палитра на редактор и на отрисовку. Индекс -1 означает
// «как в пресете», 0…5 — конкретный цвет.
constexpr int kOverlayColorCount = 6;
std::array<Color, kOverlayColorCount> overlayColorPalette();

// Рисует оверлей выбранного стиля внутри bounds и возвращает прямоугольник содержимого.
// Один и тот же код рисует и настоящее окно поверх игры, и предпросмотр в настройках.
Rect paintOverlay(ID2D1RenderTarget* target, const OverlaySettings& settings, const Theme& theme,
    const OverlayText& text, const Rect& bounds, float scale, const OverlayFormats& formats);

} // namespace yawa

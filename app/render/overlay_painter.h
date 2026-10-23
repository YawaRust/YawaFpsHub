#pragma once

#include "render/d2d.h"
#include "render/theme.h"
#include "ui/settings.h"

#include <array>
#include <string>

namespace yawa {

// Числа, которые показывает оверлей. Форматирование живёт в одном месте, чтобы
// предпросмотр в настройках и настоящее окно поверх игры совпадали до символа.
struct OverlayValues {
    double fps = 0.0;
    double low1 = 0.0;
    double cpuLoad = 0.0;
    double cpuTemp = 0.0;
    bool hasCpuTemp = false;
    double gpuLoad = 0.0;
    double gpuTemp = 0.0;
    bool hasGpuTemp = false;
    double ramUsed = 0.0;
    double ramTotal = 0.0;
    double vramUsed = 0.0;
    double vramTotal = 0.0;
    bool hasVram = false;
};

struct OverlayText {
    std::wstring fps;
    std::wstring low1;
    std::wstring cpu;
    std::wstring cpuTemp;
    std::wstring gpu;
    std::wstring gpuTemp;
    std::wstring ram;
    std::wstring vram;
    std::array<std::wstring, kLabelCount> labels;
    std::array<bool, kLabelCount> visible;
};

struct OverlayFormats {
    IDWriteFactory* factory = nullptr;
    IDWriteTextFormat* key = nullptr;
    IDWriteTextFormat* value = nullptr;
    IDWriteTextFormat* smallText = nullptr;
    IDWriteTextFormat* mono = nullptr;
};

OverlayText makeOverlayText(const OverlaySettings& settings, const OverlayValues& values);

// Рисует оверлей выбранного стиля внутри bounds и возвращает прямоугольник содержимого.
// Один и тот же код рисует и настоящее окно поверх игры, и предпросмотр в настройках.
Rect paintOverlay(ID2D1RenderTarget* target, const OverlaySettings& settings, const Theme& theme,
    const OverlayText& text, const Rect& bounds, float scale, const OverlayFormats& formats);

} // namespace yawa

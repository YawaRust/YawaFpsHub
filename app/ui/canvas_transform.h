#pragma once

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <span>

namespace yawa {

struct AxisSnap
{
    int value = 0;
    int guide = -1;
};

inline int snapGrid(int value, int step = 8)
{
    return step > 0 ? static_cast<int>(std::lround(static_cast<double>(value) / step)) * step : value;
}

// Сопоставляем левый/средний/правый край блока с линиями; равные расстояния
// разрешаются в порядке направляющих, чтобы при движении не было дрожания.
inline AxisSnap snapToGuides(int value, int extent, std::span<const int> guides, int threshold)
{
    AxisSnap result { value, -1 };
    int closest = threshold + 1;
    for (int guide : guides) {
        for (int edge : { 0, extent / 2, extent }) {
            const int distance = std::abs(value + edge - guide);
            if (distance < closest) {
                closest = distance;
                result = AxisSnap { guide - edge, guide };
            }
        }
    }
    return result;
}

// Прямоугольники в абсолютных координатах экрана либо относительно холста.
struct ScreenBox
{
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

// При отключённом/отсоединённом мониторе не меняем границы отрисовки.
inline bool monitorOnCanvas(ScreenBox screen, ScreenBox monitor, ScreenBox& result)
{
    const ScreenBox clipped { std::max(screen.left, monitor.left), std::max(screen.top, monitor.top),
        std::min(screen.right, monitor.right), std::min(screen.bottom, monitor.bottom) };
    if (clipped.left >= clipped.right || clipped.top >= clipped.bottom) {
        return false;
    }
    result = ScreenBox { clipped.left - screen.left, clipped.top - screen.top, clipped.right - screen.left,
        clipped.bottom - screen.top };
    return true;
}

// Восстановление свободной позиции при перестановке экранов без изменения сохранённого offset.
inline int originFromMonitor(int local, int monitorStart, int screenStart)
{
    return std::clamp(local + monitorStart - screenStart, 0, 32768);
}

// origin — координата в виртуальном экране; monitorStart — начало сохранённого монитора.
inline int originOnMonitor(int origin, int monitorStart)
{
    return std::max(0, origin - monitorStart);
}

inline int resizeRtssExtent(int start, int pixels, float scale)
{
    const int delta = static_cast<int>(std::lround(static_cast<float>(pixels) / std::max(0.001f, scale)));
    return delta == 0 ? start : std::clamp(start + delta, 16, 2048);
}

// Сдвиг к левой/центральной/правой или верхней/средней/нижней границе опорной строки.
inline float alignCanvasEdge(
    float referenceStart, float referenceEnd, float itemStart, float itemEnd, int edge)
{
    const float fraction = edge == 0 ? 0.0f : edge == 1 ? 0.5f : 1.0f;
    return referenceStart + (referenceEnd - referenceStart) * fraction - itemStart
        - (itemEnd - itemStart) * fraction;
}

// Не телепортируем строку при нулевом сдвиге, даже если она уже выходит за экран.
inline int clampDraggedRow(int candidate, int start, int minimumDelta, int maximumDelta)
{
    return std::clamp(candidate, start + std::min(0, minimumDelta), start + std::max(0, maximumDelta));
}

struct CanvasFit
{
    float zoom = 1.0f;
    float panX = 0.0f;
    float panY = 0.0f;
};

// Масштабируется только вид холста; экранные координаты и настройки оверлея не меняются.
inline CanvasFit fitCanvasContent(
    ScreenBox content, float viewportWidth, float viewportHeight, float desktopWidth)
{
    if (content.right <= content.left || content.bottom <= content.top || viewportWidth <= 32.0f
        || viewportHeight <= 32.0f || desktopWidth <= 0.0f) {
        return { };
    }
    const float baseScale = viewportWidth / desktopWidth;
    const float fitScale = std::min((viewportWidth - 28.0f) / (content.right - content.left),
        (viewportHeight - 28.0f) / (content.bottom - content.top));
    const float zoom = std::clamp(fitScale / baseScale, 0.15f, 20.0f);
    return { zoom, viewportWidth * 0.5f - (content.left + content.right) * 0.5f * baseScale * zoom,
        viewportHeight * 0.5f - (content.top + content.bottom) * 0.5f * baseScale * zoom };
}

// Ось холста в пикселях окна; screenCoord — пиксели виртуального экрана.
struct CanvasAxis
{
    float origin = 0.0f;
    float pan = 0.0f;
    float scale = 1.0f;

    float toCanvas(float screenCoord) const { return origin + pan + screenCoord * scale; }
    float toScreen(float canvasCoord) const { return (canvasCoord - origin - pan) / scale; }
    float panAtZoom(float focus, float nextScale) const
    {
        return focus - origin - (focus - origin - pan) * nextScale / scale;
    }
};

} // namespace yawa

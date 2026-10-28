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

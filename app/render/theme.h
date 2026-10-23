#pragma once

#include "ui/settings.h"

#include <cstdint>

namespace yawa {

// Цвета в линейном виде 0..1: Direct2D принимает именно такие, а конвертация из 0xAARRGGBB
// делается один раз при создании темы.
struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    Color withAlpha(float alpha) const { return Color{ r, g, b, alpha }; }
    Color mix(const Color& other, float amount) const;
};

Color fromRgba(std::uint32_t rgba);

struct Theme {
    Color background;
    Color panel;
    Color panelSoft;
    Color line;
    Color lineStrong;
    Color text;
    Color textDim;
    Color textFaint;
    Color accent;
    Color accentSoft;
    Color accentGlow;
    Color warn;
    Color danger;
    Color cpu;
    Color gpu;
    Color ram;
    Color disk;
};

Theme makeTheme(Accent accent);

} // namespace yawa

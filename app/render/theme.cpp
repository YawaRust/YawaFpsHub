#include "render/theme.h"

namespace yawa {
namespace {

Color accentColorFor(Accent accent)
{
    switch (accent) {
    case Accent::Mint:
        return fromRgba(0xFF5EF29B);
    case Accent::Ice:
        return fromRgba(0xFF6AA8FF);
    case Accent::Violet:
        return fromRgba(0xFFB47BFF);
    case Accent::Amber:
        return fromRgba(0xFFFFB347);
    case Accent::Coral:
        return fromRgba(0xFFFF6F6F);
    }
    return fromRgba(0xFF5EF29B);
}

} // namespace

Color Color::mix(const Color& other, float amount) const
{
    const float weight = amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount);
    return Color{
        r + (other.r - r) * weight,
        g + (other.g - g) * weight,
        b + (other.b - b) * weight,
        a + (other.a - a) * weight,
    };
}

Color fromRgba(std::uint32_t rgba)
{
    return Color{
        static_cast<float>((rgba >> 16) & 0xFF) / 255.0f,
        static_cast<float>((rgba >> 8) & 0xFF) / 255.0f,
        static_cast<float>(rgba & 0xFF) / 255.0f,
        static_cast<float>((rgba >> 24) & 0xFF) / 255.0f,
    };
}

Theme makeTheme(Accent accent)
{
    Theme theme;
    theme.background = fromRgba(0xFF070A0F);
    theme.panel = fromRgba(0xFF10151D);
    theme.panelSoft = fromRgba(0xFF151B25);
    theme.line = fromRgba(0xFF212934);
    theme.lineStrong = fromRgba(0xFF2E3844);
    theme.text = fromRgba(0xFFE9EEF4);
    theme.textDim = fromRgba(0xFF98A4B3);
    theme.textFaint = fromRgba(0xFF65717F);
    theme.warn = fromRgba(0xFFFFB347);
    theme.danger = fromRgba(0xFFFF6F6F);
    theme.cpu = fromRgba(0xFF5EF29B);
    theme.gpu = fromRgba(0xFF6AA8FF);
    theme.ram = fromRgba(0xFFB47BFF);
    theme.disk = fromRgba(0xFF4DD6D6);

    theme.accent = accentColorFor(accent);
    theme.accentSoft = theme.accent.withAlpha(0.16f);
    theme.accentGlow = theme.accent.withAlpha(0.34f);
    return theme;
}

} // namespace yawa

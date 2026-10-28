#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace yawa {

// The app loses foreground ownership when its preview opens; only a recent
// confirmed game can be offered for an explicit, window-checked capture.
inline bool recentGameSelection(std::uint64_t nowMs, std::uint64_t observedMs, std::uint32_t processId)
{
    return processId != 0 && nowMs >= observedMs && nowMs - observedMs <= 30000;
}

struct ReportMask
{
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

// Coordinates on the 1920x1080 diagnostic card, not on the source image.
// Restrict masks to the game pane so a drag cannot hide the provenance text.
inline std::optional<ReportMask> reportMaskFromDrag(int x0, int y0, int x1, int y1)
{
    const auto clampX = [](int x) { return std::clamp(x, 48, 1108); };
    const auto clampY = [](int y) { return std::clamp(y, 155, 840); };
    const int left = std::min(clampX(x0), clampX(x1));
    const int right = std::max(clampX(x0), clampX(x1));
    const int top = std::min(clampY(y0), clampY(y1));
    const int bottom = std::max(clampY(y0), clampY(y1));
    if (right - left < 5 || bottom - top < 5)
        return std::nullopt;
    return ReportMask { left, top, right, bottom };
}

} // namespace yawa

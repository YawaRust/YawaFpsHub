#pragma once

#include "render/d2d.h"
#include "render/theme.h"
#include "ui/app_model.h"

#include <vector>

namespace yawa {

float overlayOrderCardHeight(const OverlaySettings& settings);
float overlayEditorControlsHeight(const OverlaySettings& settings);
void drawOverlayEditorPreview(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card,
    int screenWidth, int screenHeight);
void drawOverlayEditorControls(Renderer& renderer, const AppModel& model, const Theme& theme,
    const Rect& area, std::vector<UiHit>& hits);

void drawOverlayView(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits);

} // namespace yawa

#pragma once

#include "render/d2d.h"
#include "render/theme.h"
#include "ui/app_model.h"

#include <vector>

namespace yawa {

void drawSensors(Renderer& renderer, AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits);

} // namespace yawa

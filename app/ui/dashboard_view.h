#pragma once

#include "render/d2d.h"
#include "render/theme.h"
#include "ui/app_model.h"

#include <vector>

namespace yawa {

void drawDashboard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits);

} // namespace yawa

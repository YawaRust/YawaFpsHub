#pragma once

#include "render/d2d.h"
#include "render/theme.h"
#include "ui/app_model.h"

#include <vector>

namespace yawa {

// Экран «Баланс»: калькулятор узкого места. Собирается из двух источников —
// живых загрузок и паспорта железа, — поэтому карточка осмысленна и без запущенной игры.
void drawBottleneckView(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits);

} // namespace yawa

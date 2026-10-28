#pragma once

#include "render/d2d.h"
#include "render/theme.h"
#include "ui/app_model.h"

#include <vector>

namespace yawa {

// Экран «Сведения о системе»: паспорт из Windows/BIOS и отдельные действующие тайминги UMC.
// Если аппаратный источник недоступен, вместо таймингов отображается «недоступны».
void drawSystemView(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits);

} // namespace yawa

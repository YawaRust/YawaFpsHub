#pragma once

#include "render/d2d.h"
#include "render/theme.h"
#include "ui/app_model.h"

#include <vector>

namespace yawa {

// Экран «Сведения о системе»: железо, Windows как в winver и раскладка планок памяти.
// Всё собрано без драйверов, поэтому там, где BIOS молчит, честно написано «нет данных».
void drawSystemView(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits);

} // namespace yawa

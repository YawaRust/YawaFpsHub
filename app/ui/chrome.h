#pragma once

#include "render/d2d.h"
#include "render/theme.h"
#include "ui/app_model.h"

#include <string_view>
#include <vector>

namespace yawa {

constexpr float kTitleBarHeight = 46.0f;
constexpr float kTabBarHeight = 40.0f;
constexpr float kStatusBarHeight = 28.0f;

void drawTitleBar(Renderer& renderer, const AppModel& model, const Theme& theme);
void drawTabs(Renderer& renderer, const AppModel& model, const Theme& theme, std::vector<UiHit>& hits);
void drawStatusBar(Renderer& renderer, const AppModel& model, const Theme& theme);
std::string_view viewTitle(View view);

} // namespace yawa

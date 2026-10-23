#pragma once

#include "render/d2d.h"
#include "render/theme.h"
#include "ui/app_model.h"

#include <string_view>
#include <vector>

namespace yawa {

// Каркас окна повторяет прототип: узкая колонка с иконками разделов слева,
// широкая шапка с надзаголовком и названием экрана, действия справа.
constexpr float kRailWidth = 68.0f;
constexpr float kTopBarHeight = 84.0f;
constexpr float kStatusBarHeight = 28.0f;

void drawRail(Renderer& renderer, const AppModel& model, const Theme& theme, std::vector<UiHit>& hits);
void drawTopBar(Renderer& renderer, const AppModel& model, const Theme& theme, std::vector<UiHit>& hits);
void drawStatusBar(Renderer& renderer, const AppModel& model, const Theme& theme);
std::string_view viewTitle(View view);
std::string_view viewEyebrow(View view);

} // namespace yawa

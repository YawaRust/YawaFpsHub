#pragma once

#include "render/d2d.h"
#include "render/theme.h"
#include "ui/app_model.h"

#include <string_view>
#include <vector>

namespace yawa {

// Каркас окна повторяет прототип: узкая колонка с иконками разделов слева,
// широкая шапка с надзаголовком и названием экрана, действия справа.
constexpr float kTitleBarHeight = 40.0f;
constexpr float kRailWidth = 68.0f;
constexpr float kTopBarHeight = 84.0f;
constexpr float kStatusBarHeight = 28.0f;

// Свой заголовок окна: системная рамка отключена, кнопки рисуются в стиле приложения.
constexpr float kWindowButtonWidth = 46.0f;
constexpr float kWindowButtonHeight = 34.0f;

void drawTitleBar(Renderer& renderer, const AppModel& model, const Theme& theme, std::vector<UiHit>& hits);
// Click по кнопкам заголовка должен уходить в WM_LBUTTONDOWN, а не в перетаскивание.
bool titleBarButtonAt(float x, float y, float windowWidth);
void drawRail(Renderer& renderer, const AppModel& model, const Theme& theme, std::vector<UiHit>& hits);
void drawTopBar(Renderer& renderer, const AppModel& model, const Theme& theme, std::vector<UiHit>& hits);
void drawStatusBar(Renderer& renderer, const AppModel& model, const Theme& theme);
std::string_view viewTitle(View view);
std::string_view viewEyebrow(View view);

} // namespace yawa

#include "ui/chrome.h"
#include "ui/layout.h"

#include <array>

namespace yawa {
namespace {

constexpr std::array<std::wstring_view, 3> kTabNames{ L"Обзор", L"Датчики", L"Настройки" };
constexpr std::array<View, 3> kTabViews{ View::Dashboard, View::Sensors, View::Settings };

void drawBrand(Renderer& renderer, const Theme& theme)
{
    const Rect badge{ 18.0f, 10.0f, 44.0f, 36.0f };
    renderer.fillRoundRect(badge, 9.0f, theme.accentSoft);
    renderer.strokeRoundRect(badge, 9.0f, theme.accentGlow, 1.0f);
    renderer.text(L"Y", badge, TextRole::Heading, theme.accent, TextAlign::Center);

    renderer.text(L"YawaFpsHub", Rect{ 54.0f, 8.0f, 260.0f, 26.0f }, TextRole::Heading, theme.text);
    renderer.text(L"мониторинг кадров и системы", Rect{ 54.0f, 24.0f, 300.0f, 40.0f }, TextRole::Tiny,
        theme.textFaint);
}

} // namespace

std::string_view viewTitle(View view)
{
    switch (view) {
    case View::Dashboard:
        return "Обзор";
    case View::Sensors:
        return "Все датчики";
    case View::Settings:
        return "Настройки";
    }
    return "Обзор";
}

void drawTitleBar(Renderer& renderer, const AppModel& model, const Theme& theme)
{
    renderer.fillRect(Rect{ 0.0f, 0.0f, renderer.width(), kTitleBarHeight }, theme.panel.withAlpha(0.55f));
    renderer.drawLine(0.0f, kTitleBarHeight - 1.0f, renderer.width(), kTitleBarHeight - 1.0f, theme.line, 1.0f);

    drawBrand(renderer, theme);

    const float right = renderer.width();
    const Rect live{ right - 300.0f, 13.0f, right - 190.0f, 33.0f };
    renderer.fillRoundRect(live, 10.0f, model.etwRunning ? theme.accentSoft : theme.panelSoft);
    renderer.fillCircle(live.left + 12.0f, live.centerY(), 3.0f,
        model.etwRunning ? theme.accent : theme.textFaint);
    renderer.text(model.etwRunning ? L"ETW · кадры" : L"ETW выключен",
        Rect{ live.left + 22.0f, live.top, live.right - 6.0f, live.bottom }, TextRole::Tiny,
        model.etwRunning ? theme.accent : theme.textFaint);

    const Rect interval{ right - 182.0f, 13.0f, right - 96.0f, 33.0f };
    renderer.fillRoundRect(interval, 10.0f, theme.panelSoft);
    renderer.text(wideText("опрос " + std::to_string(model.settings.capture.intervalMs) + " мс"), interval,
        TextRole::Tiny, theme.textDim, TextAlign::Center);

    const Rect pause{ right - 88.0f, 13.0f, right - 14.0f, 33.0f };
    renderer.fillRoundRect(pause, 10.0f, theme.panelSoft);
    renderer.text(model.paused ? L"Продолжить" : L"Пауза", pause, TextRole::Tiny, theme.textDim, TextAlign::Center);
}

void drawTabs(Renderer& renderer, const AppModel& model, const Theme& theme, std::vector<UiHit>& hits)
{
    renderer.fillRect(Rect{ 0.0f, kTitleBarHeight, renderer.width(), kTitleBarHeight + kTabBarHeight },
        theme.background);

    float left = 18.0f;
    for (std::size_t index = 0; index < kTabNames.size(); ++index) {
        const float width = renderer.measureText(kTabNames[index], TextRole::Body) + 28.0f;
        const Rect tab{ left, kTitleBarHeight + 8.0f, left + width, kTitleBarHeight + kTabBarHeight - 6.0f };
        const bool isActive = kTabViews[index] == model.view;

        renderer.fillRoundRect(tab, 9.0f, isActive ? theme.accentSoft : theme.panelSoft.withAlpha(0.55f));
        if (isActive) {
            renderer.strokeRoundRect(tab, 9.0f, theme.accentGlow, 1.0f);
        }
        renderer.text(kTabNames[index], tab, TextRole::Body, isActive ? theme.accent : theme.textDim,
            TextAlign::Center);

        hits.push_back(UiHit{ tab, ActionKind::SwitchView, static_cast<int>(kTabViews[index]) });
        left += width + 8.0f;
    }
}

void drawStatusBar(Renderer& renderer, const AppModel& model, const Theme& theme)
{
    const float top = renderer.height() - kStatusBarHeight;
    renderer.fillRect(Rect{ 0.0f, top, renderer.width(), renderer.height() }, theme.panel.withAlpha(0.5f));
    renderer.drawLine(0.0f, top, renderer.width(), top, theme.line, 1.0f);

    renderer.fillCircle(24.0f, top + 14.0f, 3.0f, model.etwRunning ? theme.accent : theme.textFaint);

    std::wstring left = model.etwRunning ? L"ETW-сессия активна · кадры DXGI" : L"ETW выключен · FPS недоступен";
    if (model.paused) {
        left += L" · сбор на паузе";
    }
    renderer.text(left, Rect{ 34.0f, top, 460.0f, renderer.height() }, TextRole::Tiny, theme.textFaint);

    const std::wstring right = L"приложение " + wideMetric(MetricId::SelfCpu, model.store.value(MetricId::SelfCpu))
        + L" · " + wideMetric(MetricId::SelfRam, model.store.value(MetricId::SelfRam))
        + L" · сессия " + formatClock(model.elapsedMs);
    renderer.text(right, Rect{ renderer.width() - 520.0f, top, renderer.width() - 18.0f, renderer.height() },
        TextRole::Tiny, theme.textFaint, TextAlign::Right);
}

} // namespace yawa

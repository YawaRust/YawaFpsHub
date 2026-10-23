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

void drawTitleBar(Renderer& renderer, const AppModel& model, const Theme& theme, std::vector<UiHit>& hits)
{
    renderer.fillRect(Rect{ 0.0f, 0.0f, renderer.width(), kTitleBarHeight }, theme.panel.withAlpha(0.55f));
    renderer.drawLine(0.0f, kTitleBarHeight - 1.0f, renderer.width(), kTitleBarHeight - 1.0f, theme.line, 1.0f);

    drawBrand(renderer, theme);

    // Правая часть шапки собирается справа налево: так кнопки не разъезжаются
    // при длинных подписях и любом масштабе.
    const float top = 13.0f;
    const float bottom = 33.0f;
    float right = renderer.width() - 14.0f;

    const Rect pause{ right - 82.0f, top, right, bottom };
    renderer.fillRoundRect(pause, 10.0f, theme.panelSoft);
    renderer.text(model.paused ? L"Продолжить" : L"Пауза", pause, TextRole::Tiny,
        model.paused ? theme.warn : theme.textDim, TextAlign::Center);
    hits.push_back(UiHit{ pause, ActionKind::TogglePause, 0 });
    right = pause.left - 8.0f;

    const Rect overlayButton{ right - 104.0f, top, right, bottom };
    renderer.fillRoundRect(overlayButton, 10.0f, model.overlayVisible ? theme.accentSoft : theme.panelSoft);
    renderer.fillCircle(overlayButton.left + 12.0f, overlayButton.centerY(), 3.0f,
        model.overlayVisible ? theme.accent : theme.textFaint);
    renderer.text(L"Оверлей", Rect{ overlayButton.left + 22.0f, overlayButton.top, overlayButton.right - 6.0f,
                         overlayButton.bottom },
        TextRole::Tiny, model.overlayVisible ? theme.accent : theme.textDim);
    hits.push_back(UiHit{ overlayButton, ActionKind::ToggleOverlay, 0 });
    right = overlayButton.left - 12.0f;

    constexpr std::array<std::uint32_t, 5> kAccentRgba{ 0xFF5EF29B, 0xFF6AA8FF, 0xFFB47BFF, 0xFFFFB347, 0xFFFF6F6F };
    for (std::size_t index = kAccentRgba.size(); index-- > 0;) {
        const float centerX = right - 8.0f;
        const Color color = fromRgba(kAccentRgba[index]);
        const bool isActive = static_cast<std::size_t>(model.settings.ui.accent) == index;
        renderer.fillCircle(centerX, (top + bottom) * 0.5f, 8.0f, isActive ? color : color.withAlpha(0.35f));
        if (isActive) {
            renderer.fillCircle(centerX, (top + bottom) * 0.5f, 3.0f, theme.background);
        }
        hits.push_back(UiHit{ Rect{ centerX - 9.0f, top, centerX + 9.0f, bottom }, ActionKind::SetAccent,
            static_cast<int>(index) });
        right = centerX - 8.0f - 6.0f;
    }
    right -= 4.0f;

    const Rect interval{ right - 92.0f, top, right, bottom };
    renderer.fillRoundRect(interval, 10.0f, theme.panelSoft);
    renderer.text(wideText("опрос " + std::to_string(model.settings.capture.intervalMs) + " мс"), interval,
        TextRole::Tiny, theme.textDim, TextAlign::Center);
    right = interval.left - 8.0f;

    const Rect live{ right - 168.0f, top, right, bottom };
    renderer.fillRoundRect(live, 10.0f, model.etwRunning ? theme.accentSoft : theme.panelSoft);
    renderer.fillCircle(live.left + 12.0f, live.centerY(), 3.0f,
        model.etwRunning ? theme.accent : theme.textFaint);
    renderer.text(model.etwRunning ? L"ETW · кадры" : L"ETW выключен",
        Rect{ live.left + 22.0f, live.top, live.right - 6.0f, live.bottom }, TextRole::Tiny,
        model.etwRunning ? theme.accent : theme.textFaint);
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

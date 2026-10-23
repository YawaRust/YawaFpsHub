#include "ui/chrome.h"
#include "ui/layout.h"

#include <array>
#include <cstdint>
#include <iterator>

namespace yawa {
namespace {

constexpr std::array<std::wstring_view, 4> kTabNames{ L"Обзор", L"Датчики", L"Оверлей", L"Настройки" };
constexpr std::array<View, 4> kTabViews{ View::Dashboard, View::Sensors, View::Overlay, View::Settings };

void drawGridIcon(Renderer& renderer, const Rect& box, const Color& color)
{
    const float size = 7.0f;
    const float gap = 3.0f;
    const float left = box.centerX() - size - gap * 0.5f;
    const float top = box.centerY() - size - gap * 0.5f;
    for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 2; ++column) {
            const float x = left + static_cast<float>(column) * (size + gap);
            const float y = top + static_cast<float>(row) * (size + gap);
            renderer.fillRoundRect(Rect{ x, y, x + size, y + size }, 2.0f, color);
        }
    }
}

void drawPulseIcon(Renderer& renderer, const Rect& box, const Color& color)
{
    const float y = box.centerY();
    const float left = box.centerX() - 10.0f;
    const float points[] = { 0.0f, 0.0f, 4.0f, 0.0f, 6.5f, -6.0f, 10.0f, 6.0f, 13.0f, -2.0f, 16.0f, 0.0f, 20.0f, 0.0f };
    for (std::size_t index = 2; index + 1 < std::size(points); index += 2) {
        renderer.drawLine(left + points[index - 2], y + points[index - 1], left + points[index],
            y + points[index + 1], color, 1.6f);
    }
}

void drawLayersIcon(Renderer& renderer, const Rect& box, const Color& color)
{
    const float width = 16.0f;
    const float height = 6.0f;
    const float left = box.centerX() - width * 0.5f;
    renderer.strokeRoundRect(Rect{ left, box.centerY() - 9.0f, left + width, box.centerY() - 3.0f }, 2.0f, color, 1.4f);
    renderer.strokeRoundRect(Rect{ left, box.centerY() - 3.0f, left + width, box.centerY() + 3.0f }, 2.0f, color, 1.4f);
    renderer.strokeRoundRect(Rect{ left, box.centerY() + 3.0f, left + width, box.centerY() + 9.0f }, 2.0f, color, 1.4f);
    (void)height;
}

void drawSlidersIcon(Renderer& renderer, const Rect& box, const Color& color)
{
    const float left = box.centerX() - 9.0f;
    const float right = box.centerX() + 9.0f;
    for (int row = 0; row < 3; ++row) {
        const float y = box.centerY() - 6.0f + static_cast<float>(row) * 6.0f;
        renderer.drawLine(left, y, right, y, color, 1.3f);
        const float knobX = left + 4.0f + static_cast<float>(row) * 4.0f;
        renderer.fillCircle(knobX, y, 2.2f, color);
    }
}

void drawBrand(Renderer& renderer, const Theme& theme)
{
    const Rect badge{ 16.0f, 16.0f, 52.0f, 52.0f };
    renderer.fillRoundRect(badge, 12.0f, theme.accentSoft);
    renderer.strokeRoundRect(badge, 12.0f, theme.accentGlow, 1.0f);
    renderer.text(L"Y", badge, TextRole::Value, theme.accent, TextAlign::Center);
}

} // namespace

namespace {

bool hovered(const AppModel& model, const Rect& rect)
{
    return model.cursorInside && rect.contains(model.cursorX, model.cursorY);
}

void drawWindowButtons(Renderer& renderer, const AppModel& model, const Theme& theme, std::vector<UiHit>& hits)
{
    const float top = (kTitleBarHeight - kWindowButtonHeight) * 0.5f;
    float right = renderer.width() - 8.0f;

    // Закрытие: обычная кнопка, при наведении — красная, как принято в Windows.
    const Rect close{ right - kWindowButtonWidth, top, right, top + kWindowButtonHeight };
    if (hovered(model, close)) {
        renderer.fillRoundRect(close, 8.0f, theme.danger.withAlpha(0.85f));
    }
    const Color closeColor = hovered(model, close) ? theme.background : theme.textDim;
    const float closeCenterX = close.centerX();
    const float closeCenterY = close.centerY();
    renderer.drawLine(closeCenterX - 5.0f, closeCenterY - 5.0f, closeCenterX + 5.0f, closeCenterY + 5.0f, closeColor,
        1.2f);
    renderer.drawLine(closeCenterX + 5.0f, closeCenterY - 5.0f, closeCenterX - 5.0f, closeCenterY + 5.0f, closeColor,
        1.2f);
    hits.push_back(UiHit{ close, ActionKind::WindowClose, 0 });
    right = close.left;

    const Rect maximize{ right - kWindowButtonWidth, top, right, top + kWindowButtonHeight };
    if (hovered(model, maximize)) {
        renderer.fillRoundRect(maximize, 8.0f, theme.panelSoft);
    }
    const Color maximizeColor = hovered(model, maximize) ? theme.text : theme.textDim;
    if (model.maximized) {
        const Rect back{ maximize.centerX() - 3.0f, maximize.centerY() - 6.0f, maximize.centerX() + 6.0f,
            maximize.centerY() + 3.0f };
        renderer.strokeRoundRect(back, 1.5f, maximizeColor, 1.1f);
        const Rect front{ maximize.centerX() - 6.0f, maximize.centerY() - 3.0f, maximize.centerX() + 3.0f,
            maximize.centerY() + 6.0f };
        renderer.fillRoundRect(front, 1.5f, theme.background);
        renderer.strokeRoundRect(front, 1.5f, maximizeColor, 1.1f);
    } else {
        renderer.strokeRoundRect(Rect{ maximize.centerX() - 5.0f, maximize.centerY() - 5.0f,
                                   maximize.centerX() + 5.0f, maximize.centerY() + 5.0f },
            2.0f, maximizeColor, 1.1f);
    }
    hits.push_back(UiHit{ maximize, ActionKind::WindowMaximize, 0 });
    right = maximize.left;

    const Rect minimize{ right - kWindowButtonWidth, top, right, top + kWindowButtonHeight };
    if (hovered(model, minimize)) {
        renderer.fillRoundRect(minimize, 8.0f, theme.panelSoft);
    }
    renderer.drawLine(minimize.centerX() - 5.0f, minimize.centerY() + 1.0f, minimize.centerX() + 5.0f,
        minimize.centerY() + 1.0f, hovered(model, minimize) ? theme.text : theme.textDim, 1.2f);
    hits.push_back(UiHit{ minimize, ActionKind::WindowMinimize, 0 });
}

} // namespace

std::string_view viewTitle(View view)
{
    switch (view) {
    case View::Dashboard:
        return "Обзор";
    case View::Sensors:
        return "Все датчики";
    case View::Overlay:
        return "Оверлей";
    case View::Settings:
        return "Настройки";
    }
    return "Обзор";
}

std::string_view viewEyebrow(View view)
{
    switch (view) {
    case View::Dashboard:
        return "МОНИТОРИНГ";
    case View::Sensors:
        return "ДАТЧИКИ";
    case View::Overlay:
        return "НАЛОЖЕНИЕ ПОВЕРХ ИГРЫ";
    case View::Settings:
        return "ПРИЛОЖЕНИЕ";
    }
    return "МОНИТОРИНГ";
}

bool titleBarButtonAt(float x, float y, float windowWidth)
{
    if (y < 0.0f || y > kTitleBarHeight) {
        return false;
    }
    const float buttonsLeft = windowWidth - 8.0f - kWindowButtonWidth * 3.0f - 8.0f;
    return x >= buttonsLeft;
}

void drawTitleBar(Renderer& renderer, const AppModel& model, const Theme& theme, std::vector<UiHit>& hits)
{
    const Rect bar{ kRailWidth, 0.0f, renderer.width(), kTitleBarHeight };
    renderer.fillRect(bar, theme.background);
    renderer.drawLine(kRailWidth, kTitleBarHeight - 0.5f, renderer.width(), kTitleBarHeight - 0.5f, theme.line, 1.0f);

    renderer.text(L"YawaFpsHub", Rect{ kRailWidth + 20.0f, 8.0f, kRailWidth + 160.0f, 24.0f }, TextRole::Small,
        theme.textDim);
    renderer.fillCircle(kRailWidth + 22.0f, 30.0f, 2.0f, model.etwRunning ? theme.accent : theme.textFaint);
    renderer.text(model.etwRunning ? L"мониторинг активен" : L"мониторинг остановлен",
        Rect{ kRailWidth + 30.0f, 23.0f, kRailWidth + 220.0f, 38.0f }, TextRole::Tiny, theme.textFaint);

    drawWindowButtons(renderer, model, theme, hits);
}

void drawRail(Renderer& renderer, const AppModel& model, const Theme& theme, std::vector<UiHit>& hits)
{
    renderer.fillRect(Rect{ 0.0f, 0.0f, kRailWidth, renderer.height() }, theme.panel.withAlpha(0.85f));
    renderer.drawLine(kRailWidth - 1.0f, kTitleBarHeight, kRailWidth - 1.0f, renderer.height(), theme.line, 1.0f);

    drawBrand(renderer, theme);

    float top = 96.0f;
    for (std::size_t index = 0; index < kTabNames.size(); ++index) {
        const Rect button{ 12.0f, top, kRailWidth - 12.0f, top + 44.0f };
        const bool isActive = kTabViews[index] == model.view;
        const bool isHovered = !isActive && hovered(model, button);
        renderer.fillRoundRect(button, 12.0f,
            isActive ? theme.accentSoft : theme.panelSoft.withAlpha(isHovered ? 0.95f : 0.6f));
        if (isActive) {
            renderer.strokeRoundRect(button, 12.0f, theme.accentGlow, 1.0f);
        }

        const Color icon = isActive ? theme.accent : theme.textFaint;
        switch (kTabViews[index]) {
        case View::Dashboard:
            drawGridIcon(renderer, button, icon);
            break;
        case View::Sensors:
            drawPulseIcon(renderer, button, icon);
            break;
        case View::Overlay:
            drawLayersIcon(renderer, button, icon);
            break;
        case View::Settings:
            drawSlidersIcon(renderer, button, icon);
            break;
        }

        hits.push_back(UiHit{ button, ActionKind::SwitchView, static_cast<int>(kTabViews[index]) });
        top += 54.0f;
    }

    renderer.fillCircle(24.0f, renderer.height() - 54.0f, 3.0f,
        model.etwRunning ? theme.accent : theme.textFaint);
    renderer.text(L"v0.4", Rect{ 14.0f, renderer.height() - 44.0f, kRailWidth - 6.0f,
                                 renderer.height() - 26.0f },
        TextRole::Tiny, theme.textFaint);
}

void drawTopBar(Renderer& renderer, const AppModel& model, const Theme& theme, std::vector<UiHit>& hits)
{
    renderer.fillRect(Rect{ kRailWidth, kTitleBarHeight, renderer.width(), kTitleBarHeight + kTopBarHeight },
        theme.background);

    renderer.text(wideText(viewEyebrow(model.view)),
        Rect{ kRailWidth + 24.0f, kTitleBarHeight + 22.0f, kRailWidth + 360.0f, kTitleBarHeight + 38.0f },
        TextRole::Tiny, theme.accent.withAlpha(0.8f));
    renderer.text(wideText(viewTitle(model.view)),
        Rect{ kRailWidth + 24.0f, kTitleBarHeight + 38.0f, kRailWidth + 460.0f, kTitleBarHeight + 76.0f },
        TextRole::Value, theme.text);

    const float top = kTitleBarHeight + 28.0f;
    const float bottom = kTitleBarHeight + 62.0f;
    float right = renderer.width() - 20.0f;

    const Rect pause{ right - 88.0f, top, right, bottom };
    if (!model.paused && hovered(model, pause)) {
        renderer.fillRoundRect(pause, 9.0f, theme.panelSoft);
    }
    drawChip(renderer, pause, model.paused ? L"Продолжить" : L"Пауза", model.paused, theme);
    hits.push_back(UiHit{ pause, ActionKind::TogglePause, 0 });
    right = pause.left - 8.0f;

    const Rect overlayButton{ right - 104.0f, top, right, bottom };
    if (!model.overlayVisible && hovered(model, overlayButton)) {
        renderer.strokeRoundRect(overlayButton, 10.0f, theme.lineStrong, 1.0f);
    }
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
        right = centerX - 14.0f;
    }
    right -= 4.0f;

    const Rect interval{ right - 112.0f, top, right, bottom };
    drawChip(renderer, interval, wideText("опрос " + std::to_string(model.settings.capture.intervalMs) + " мс"),
        false, theme);
    right = interval.left - 8.0f;

    const Rect live{ right - 168.0f, top, right, bottom };
    renderer.fillRoundRect(live, 10.0f, model.etwRunning ? theme.accentSoft : theme.panelSoft);
    renderer.fillCircle(live.left + 12.0f, live.centerY(), 3.0f, model.etwRunning ? theme.accent : theme.textFaint);
    renderer.text(model.etwRunning ? L"ETW · кадры" : L"ETW выключен",
        Rect{ live.left + 22.0f, live.top, live.right - 6.0f, live.bottom }, TextRole::Tiny,
        model.etwRunning ? theme.accent : theme.textFaint);
}

void drawStatusBar(Renderer& renderer, const AppModel& model, const Theme& theme)
{
    const float top = renderer.height() - kStatusBarHeight;
    renderer.fillRect(Rect{ 0.0f, top, renderer.width(), renderer.height() }, theme.panel.withAlpha(0.85f));
    renderer.drawLine(0.0f, top, renderer.width(), top, theme.line, 1.0f);

    renderer.fillCircle(kRailWidth + 16.0f, top + 14.0f, 3.0f, model.etwRunning ? theme.accent : theme.textFaint);

    std::wstring left = model.etwRunning ? L"ETW-сессия активна · кадры DXGI" : L"ETW выключен · FPS недоступен";
    if (model.paused) {
        left += L" · сбор на паузе";
    }
    renderer.text(left, Rect{ kRailWidth + 26.0f, top, kRailWidth + 460.0f, renderer.height() }, TextRole::Tiny,
        theme.textFaint);

    const std::wstring right = L"приложение " + wideMetric(MetricId::SelfCpu, model.store.value(MetricId::SelfCpu))
        + L" · " + wideMetric(MetricId::SelfRam, model.store.value(MetricId::SelfRam))
        + L" · сессия " + formatClock(model.elapsedMs);
    renderer.text(right, Rect{ renderer.width() - 520.0f, top, renderer.width() - 18.0f, renderer.height() },
        TextRole::Tiny, theme.textFaint, TextAlign::Right);

    const wchar_t* hints = L"F10 — оверлей · F11 — положение · F12 — окно";
    const float hintsWidth = renderer.measureText(hints, TextRole::Tiny);
    renderer.text(hints, Rect{ renderer.width() * 0.5f - hintsWidth * 0.5f, top,
                             renderer.width() * 0.5f + hintsWidth * 0.5f, renderer.height() },
        TextRole::Tiny, theme.textFaint, TextAlign::Center);
}

} // namespace yawa

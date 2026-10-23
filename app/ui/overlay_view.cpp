#include "ui/overlay_view.h"
#include "render/overlay_painter.h"
#include "ui/layout.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <string>

namespace yawa {
namespace {

constexpr std::array<std::wstring_view, 3> kStyleNames{ L"Стекло", L"Неон", L"Капсулы" };
constexpr std::array<std::wstring_view, 2> kStyleNamesTail{ L"Терминал", L"Минимал" };
constexpr std::array<std::wstring_view, 4> kPositionNames{ L"↖", L"↗", L"↙", L"↘" };
constexpr std::array<std::wstring_view, 5> kMetricNames{ L"FPS", L"Процессор", L"Видеокарта", L"Память",
    L"Видеопамять" };

void drawChip(Renderer& renderer, const Rect& rect, std::wstring_view label, bool isActive, const Theme& theme,
    TextRole role = TextRole::Small)
{
    renderer.fillRoundRect(rect, 9.0f, isActive ? theme.accentSoft : theme.panelSoft);
    if (isActive) {
        renderer.strokeRoundRect(rect, 9.0f, theme.accentGlow, 1.0f);
    }
    renderer.text(label, rect, role, isActive ? theme.accent : theme.textDim, TextAlign::Center);
}

// Ползунок без отдельного контрола: клик по дорожке сразу ставит значение,
// а перетаскивание обрабатывает окно (SetScale/SetOpacity в UiHit).
void drawSlider(Renderer& renderer, const Rect& row, int value, int minimum, int maximum, std::wstring_view caption,
    const Theme& theme, std::vector<UiHit>& hits, ActionKind action)
{
    renderer.text(caption, Rect{ row.left, row.top, row.left + 130.0f, row.top + 16.0f }, TextRole::Small,
        theme.textDim);
    renderer.text(wideText(std::to_string(value) + " %"),
        Rect{ row.right - 60.0f, row.top, row.right, row.top + 16.0f }, TextRole::Small, theme.text,
        TextAlign::Right);

    const Rect track{ row.left, row.top + 24.0f, row.right, row.top + 36.0f };
    renderer.fillRoundRect(Rect{ track.left, track.centerY() - 2.0f, track.right, track.centerY() + 2.0f }, 2.0f,
        theme.line);
    const float fraction = static_cast<float>(value - minimum) / static_cast<float>(maximum - minimum);
    const float knob = track.left + std::clamp(fraction, 0.0f, 1.0f) * track.width();
    renderer.fillRoundRect(Rect{ track.left, track.centerY() - 2.0f, knob, track.centerY() + 2.0f }, 2.0f,
        theme.accent);
    renderer.fillCircle(knob, track.centerY(), 6.0f, theme.accent);
    renderer.fillCircle(knob, track.centerY(), 2.5f, theme.background);

    hits.push_back(UiHit{ track, action, 0 });
}

OverlayValues valuesFromModel(const AppModel& model)
{
    OverlayValues values;
    values.fps = model.fps.currentFps;
    values.low1 = model.fps.low1Fps;
    values.cpuLoad = model.store.value(MetricId::CpuLoad);
    values.cpuTemp = model.store.value(MetricId::CpuTemp);
    values.hasCpuTemp = model.availability.cpuTemperature && values.cpuTemp > 0.0;
    values.gpuLoad = model.store.value(MetricId::GpuLoad);
    values.gpuTemp = model.store.value(MetricId::GpuTemp);
    values.hasGpuTemp = model.availability.gpuTemperature && values.gpuTemp > 0.0;
    values.ramUsed = model.store.value(MetricId::RamUsed);
    values.ramTotal = model.store.value(MetricId::RamUsed) + model.store.value(MetricId::RamFree);
    values.vramUsed = model.store.value(MetricId::VramUsed);
    values.vramTotal = model.store.value(MetricId::VramTotal);
    values.hasVram = values.vramTotal > 0.0;
    return values;
}

// Имитация игры под предпросмотром: показывает, как оверлей читается на живой картинке.
void drawPreviewScene(Renderer& renderer, const Rect& scene, const Theme& theme)
{
    renderer.fillRoundRect(scene, 12.0f, fromRgba(0xFF0B1220));
    renderer.fillRoundRect(Rect{ scene.left, scene.top, scene.right, scene.top + scene.height() * 0.55f }, 12.0f,
        fromRgba(0xFF16283C));
    renderer.fillCircle(scene.right - 90.0f, scene.top + 54.0f, 26.0f, theme.warn.withAlpha(0.22f));

    const float ground = scene.bottom - scene.height() * 0.22f;
    renderer.fillRect(Rect{ scene.left, ground, scene.right, scene.bottom }, fromRgba(0xFF0E1A26));

    float left = scene.left + 24.0f;
    const float heights[] = { 44.0f, 72.0f, 34.0f, 96.0f, 58.0f, 80.0f, 40.0f, 64.0f };
    for (std::size_t index = 0; index < std::size(heights); ++index) {
        const float width = 26.0f + static_cast<float>(index % 3) * 8.0f;
        renderer.fillRoundRect(Rect{ left, ground - heights[index], left + width, ground }, 3.0f,
            fromRgba(0xFF1B2A3A));
        left += width + 12.0f;
        if (left > scene.right - 60.0f) {
            break;
        }
    }

    // Прицел и полоска здоровья из «игры».
    renderer.drawLine(scene.centerX() - 10.0f, scene.centerY() - 40.0f, scene.centerX() + 10.0f,
        scene.centerY() - 40.0f, theme.text.withAlpha(0.5f), 1.2f);
    renderer.drawLine(scene.centerX(), scene.centerY() - 50.0f, scene.centerX(), scene.centerY() - 30.0f,
        theme.text.withAlpha(0.5f), 1.2f);
    renderer.fillRoundRect(Rect{ scene.left + 24.0f, scene.bottom - 34.0f, scene.left + 150.0f, scene.bottom - 26.0f },
        4.0f, theme.text.withAlpha(0.12f));
    renderer.fillRoundRect(Rect{ scene.left + 24.0f, scene.bottom - 34.0f, scene.left + 118.0f, scene.bottom - 26.0f },
        4.0f, theme.warn.withAlpha(0.65f));
}

void drawPreviewCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);

    renderer.text(L"Оверлей поверх игры", Rect{ inner.left, inner.top, inner.left + 260.0f, inner.top + 22.0f },
        TextRole::Heading, theme.text);
    renderer.text(L"предпросмотр · в игре это отдельное topmost-окно, F10 — показать или скрыть",
        Rect{ inner.left, inner.top + 20.0f, inner.right - 70.0f, inner.top + 38.0f }, TextRole::Tiny,
        theme.textFaint);

    const Rect badge{ inner.right - 62.0f, inner.top - 2.0f, inner.right, inner.top + 20.0f };
    renderer.fillRoundRect(badge, 9.0f, model.overlayVisible ? theme.accentSoft : theme.panelSoft);
    renderer.text(model.overlayVisible ? L"ВКЛ" : L"F10", badge, TextRole::Tiny,
        model.overlayVisible ? theme.accent : theme.textFaint, TextAlign::Center);

    const Rect scene{ inner.left, inner.top + 48.0f, inner.right, inner.bottom };
    drawPreviewScene(renderer, scene, theme);

    const OverlayFormats formats{ renderer.dwrite(), renderer.textFormat(TextRole::Heading),
        renderer.textFormat(TextRole::Value), renderer.textFormat(TextRole::Small),
        renderer.textFormat(TextRole::Mono) };

    // Предпросмотр масштабируется вместе с реальным оверлеем, но чуть меньше,
    // чтобы при 160 % содержимое не упиралось в рамку карточки.
    const float scale = static_cast<float>(model.settings.overlay.scalePercent) / 100.0f * 0.92f;
    const OverlayText text = makeOverlayText(model.settings.overlay, valuesFromModel(model));
    paintOverlay(renderer.target(), model.settings.overlay, theme, text, scene.inset(2.0f), scale, formats);

    renderer.strokeRoundRect(scene, 12.0f, theme.line, 1.0f);
}

void drawControlsCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card,
    std::vector<UiHit>& hits)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    const OverlaySettings& overlay = model.settings.overlay;

    renderer.text(L"Настройка оверлея", Rect{ inner.left, inner.top, inner.right, inner.top + 22.0f },
        TextRole::Heading, theme.text);
    renderer.text(L"изменения сразу видны в предпросмотре",
        Rect{ inner.left, inner.top + 20.0f, inner.right, inner.top + 38.0f }, TextRole::Tiny, theme.textFaint);

    float top = inner.top + 48.0f;

    renderer.text(L"Стиль", Rect{ inner.left, top, inner.left + 90.0f, top + 16.0f }, TextRole::Small, theme.textDim);
    top += 20.0f;
    const float styleWidth = (inner.width() - 2.0f * 8.0f) / 3.0f;
    for (std::size_t index = 0; index < kStyleNames.size(); ++index) {
        const Rect chip{ inner.left + static_cast<float>(index) * (styleWidth + 8.0f), top,
            inner.left + static_cast<float>(index) * (styleWidth + 8.0f) + styleWidth, top + 28.0f };
        drawChip(renderer, chip, kStyleNames[index], static_cast<std::size_t>(overlay.style) == index, theme);
        hits.push_back(UiHit{ chip, ActionKind::SetStyle, static_cast<int>(index) });
    }

    const float tailWidth = (inner.width() - 8.0f) / 2.0f;
    for (std::size_t index = 0; index < kStyleNamesTail.size(); ++index) {
        const Rect chip{ inner.left + static_cast<float>(index) * (tailWidth + 8.0f), top + 34.0f,
            inner.left + static_cast<float>(index) * (tailWidth + 8.0f) + tailWidth, top + 62.0f };
        drawChip(renderer, chip, kStyleNamesTail[index], static_cast<std::size_t>(overlay.style) == index + 3, theme);
        hits.push_back(UiHit{ chip, ActionKind::SetStyle, static_cast<int>(index + 3) });
    }
    top += 74.0f;

    renderer.text(L"Положение", Rect{ inner.left, top, inner.left + 90.0f, top + 16.0f }, TextRole::Small,
        theme.textDim);
    for (std::size_t index = 0; index < kPositionNames.size(); ++index) {
        const Rect chip{ inner.left + 96.0f + static_cast<float>(index) * 40.0f, top - 4.0f,
            inner.left + 128.0f + static_cast<float>(index) * 40.0f, top + 22.0f };
        drawChip(renderer, chip, kPositionNames[index],
            static_cast<std::size_t>(overlay.position) == index, theme);
        hits.push_back(UiHit{ chip, ActionKind::SetPosition, static_cast<int>(index) });
    }

    renderer.text(L"Вид", Rect{ inner.left + 268.0f, top, inner.left + 298.0f, top + 16.0f }, TextRole::Small,
        theme.textDim);
    const Rect plates{ inner.left + 304.0f, top - 4.0f, inner.left + 372.0f, top + 22.0f };
    const Rect line{ inner.left + 378.0f, top - 4.0f, inner.left + 434.0f, top + 22.0f };
    drawChip(renderer, plates, L"Плитки", overlay.layout == OverlayLayout::Plates, theme);
    drawChip(renderer, line, L"Строка", overlay.layout == OverlayLayout::Line, theme);
    hits.push_back(UiHit{ plates, ActionKind::SetLayout, static_cast<int>(OverlayLayout::Plates) });
    hits.push_back(UiHit{ line, ActionKind::SetLayout, static_cast<int>(OverlayLayout::Line) });
    top += 40.0f;

    drawSlider(renderer, Rect{ inner.left, top, inner.right, top + 36.0f }, overlay.scalePercent, 70, 160,
        L"Масштаб", theme, hits, ActionKind::SetScale);
    top += 50.0f;
    drawSlider(renderer, Rect{ inner.left, top, inner.right, top + 36.0f }, overlay.opacityPercent, 0, 100,
        L"Прозрачность фона", theme, hits, ActionKind::SetOpacity);
    top += 54.0f;

    renderer.text(L"Смещение", Rect{ inner.left, top, inner.left + 90.0f, top + 18.0f }, TextRole::Small,
        theme.textDim);
    const auto drawOffset = [&](float left, ActionKind action, int value, std::wstring_view axis) {
        const Rect minus{ left, top - 4.0f, left + 28.0f, top + 22.0f };
        const Rect plus{ left + 96.0f, top - 4.0f, left + 124.0f, top + 22.0f };
        drawChip(renderer, minus, L"−", false, theme);
        drawChip(renderer, plus, L"+", false, theme);
        renderer.text(axis, Rect{ minus.right + 2.0f, top, plus.left - 2.0f, top + 18.0f }, TextRole::Small,
            theme.textFaint, TextAlign::Center);
        renderer.text(wideText(std::to_string(value) + " px"),
            Rect{ minus.left, top + 22.0f, plus.right, top + 38.0f }, TextRole::Tiny, theme.textDim,
            TextAlign::Center);
        hits.push_back(UiHit{ minus, action, -10 });
        hits.push_back(UiHit{ plus, action, 10 });
    };
    drawOffset(inner.left + 96.0f, ActionKind::OffsetX, overlay.offsetX, L"X");
    drawOffset(inner.left + 236.0f, ActionKind::OffsetY, overlay.offsetY, L"Y");
    top += 46.0f;

    renderer.text(L"Состав", Rect{ inner.left, top, inner.right, top + 16.0f }, TextRole::Small, theme.textDim);
    top += 22.0f;
    const float metricWidth = (inner.width() - 8.0f) / 2.0f;
    for (std::size_t index = 0; index < kMetricNames.size(); ++index) {
        const float column = static_cast<float>(index % 2);
        const float row = static_cast<float>(index / 2);
        const Rect chip{ inner.left + column * (metricWidth + 8.0f), top + row * 32.0f,
            inner.left + column * (metricWidth + 8.0f) + metricWidth, top + row * 32.0f + 26.0f };
        drawChip(renderer, chip, kMetricNames[index], overlay.visible[index], theme, TextRole::Tiny);
        hits.push_back(UiHit{ chip, ActionKind::ToggleMetric, static_cast<int>(index) });
    }
}

} // namespace

void drawOverlayView(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits)
{
    const float leftWidth = std::max(area.width() * 0.56f, 460.0f);
    const Rect preview{ area.left, area.top, area.left + leftWidth - kGap, area.bottom };
    const Rect controls{ preview.right + kGap, area.top, area.right, area.bottom };

    drawPreviewCard(renderer, model, theme, preview);
    drawControlsCard(renderer, model, theme, controls, hits);
}

} // namespace yawa

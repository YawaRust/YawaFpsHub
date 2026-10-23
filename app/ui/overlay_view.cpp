#include "ui/overlay_view.h"
#include "render/overlay_painter.h"
#include "ui/hotkey_text.h"
#include "ui/layout.h"

#include <algorithm>
#include <array>
#include <string>

namespace yawa {
namespace {

// Восемь пресетов из Fpsmon2.0: имя и подсказка, чем стиль отличается.
constexpr std::array<std::wstring_view, 8> kStyleNames{ L"Неон", L"RTSS", L"Стекло", L"Терминал", L"Кибер",
    L"Минимал", L"Esport", L"Retro" };
constexpr std::array<std::wstring_view, 4> kPositionNames{ L"Слева сверху", L"Справа сверху", L"Слева снизу",
    L"Справа снизу" };
constexpr std::array<std::wstring_view, 5> kFieldNames{ L"Кадры в секунду", L"Загрузка процессора",
    L"Загрузка видеокарты", L"Занятая память", L"Видеопамять" };

OverlayValues currentValues(const AppModel& model)
{
    OverlayValues values;
    values.fps = model.fps.currentFps;
    values.low1 = model.fps.low1Fps;
    values.cpuLoad = model.store.value(MetricId::CpuLoad);
    values.cpuTemp = model.store.value(MetricId::CpuTemp);
    values.hasCpuTemp = model.availability.cpuTemperature;
    values.gpuLoad = model.store.value(MetricId::GpuLoad);
    values.gpuTemp = model.store.value(MetricId::GpuTemp);
    values.hasGpuTemp = model.availability.gpuTemperature;
    values.ramUsed = model.store.value(MetricId::RamUsed);
    values.vramUsed = model.store.value(MetricId::VramUsed);
    values.hasVram = model.store.value(MetricId::VramTotal) > 0.0;
    return values;
}

void drawSlider(Renderer& renderer, const Rect& row, int value, int minimum, int maximum, std::wstring_view caption,
    const Theme& theme, std::vector<UiHit>& hits, ActionKind action, std::wstring_view suffix)
{
    renderer.text(caption, Rect{ row.left, row.top, row.left + 160.0f, row.top + 16.0f }, TextRole::Small,
        theme.textDim);
    std::wstring valueText = std::to_wstring(value);
    valueText += suffix;
    renderer.text(valueText, Rect{ row.right - 70.0f, row.top, row.right, row.top + 16.0f }, TextRole::Small,
        theme.text, TextAlign::Right);

    const Rect track{ row.left, row.top + 24.0f, row.right, row.top + 34.0f };
    drawBar(renderer, track, static_cast<float>(value - minimum) / static_cast<float>(maximum - minimum),
        theme.accent, theme);
    const float knobX = track.left + track.width() * static_cast<float>(value - minimum)
        / static_cast<float>(maximum - minimum);
    renderer.fillCircle(knobX, track.centerY(), 6.0f, theme.accent);
    hits.push_back(UiHit{ Rect{ track.left, track.top - 8.0f, track.right, track.bottom + 8.0f }, action, 0 });
}

void drawToggleRow(Renderer& renderer, const Rect& row, std::wstring_view label, bool on, const Theme& theme,
    std::vector<UiHit>& hits, ActionKind action, int value)
{
    const Rect track{ row.right - 36.0f, row.centerY() - 9.0f, row.right, row.centerY() + 9.0f };
    renderer.fillRoundRect(track, 9.0f, on ? theme.accentSoft : theme.panelSoft);
    if (on) {
        renderer.strokeRoundRect(track, 9.0f, theme.accentGlow, 1.0f);
    }
    renderer.fillCircle(on ? track.right - 9.0f : track.left + 9.0f, track.centerY(), 6.0f,
        on ? theme.accent : theme.textFaint);
    renderer.text(label, Rect{ row.left, row.top, track.left - 12.0f, row.bottom }, TextRole::Small,
        on ? theme.text : theme.textFaint);
    hits.push_back(UiHit{ row, action, value });
}

void drawStyleGallery(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits);

void drawPreviewCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card,
    std::vector<UiHit>& hits)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);

    renderer.text(L"Оверлей поверх игры", Rect{ inner.left, inner.top, inner.right, inner.top + 22.0f },
        TextRole::Heading, theme.text);
    renderer.text(L"Ctrl + мышь по оверлею — перетащить; здесь — предпросмотр на макете сцены",
        Rect{ inner.left, inner.top + 20.0f, inner.right - 60.0f, inner.top + 38.0f }, TextRole::Tiny, theme.textFaint);

    const Rect tag{ inner.right - 72.0f, inner.top, inner.right, inner.top + 20.0f };
    renderer.fillRoundRect(tag, 10.0f, theme.panelSoft);
    renderer.text(hotkeyName(model.settings.hotkeys.overlay), tag, TextRole::Tiny, theme.textFaint,
        TextAlign::Center);

    const float galleryTop = inner.bottom - 104.0f;
    const Rect scene{ inner.left, inner.top + 46.0f, inner.right, std::max(inner.top + 76.0f, galleryTop - 10.0f) };

    // Макет игровой сцены: небо, земля и пара силуэтов — на нём видно, как читается оверлей.
    renderer.fillRoundRect(scene, kCardRadius, theme.panelSoft);
    renderer.fillRect(Rect{ scene.left + 1.0f, scene.centerY() + 14.0f, scene.right - 1.0f, scene.bottom - 1.0f },
        theme.background.withAlpha(0.55f));
    renderer.fillCircle(scene.right - 74.0f, scene.top + 56.0f, 22.0f, theme.warn.withAlpha(0.16f));
    for (int index = 0; index < 5; ++index) {
        const float width = 26.0f + static_cast<float>(index % 3) * 12.0f;
        const float left = scene.left + 26.0f + static_cast<float>(index) * 58.0f;
        const float height = 40.0f + static_cast<float>(index % 4) * 22.0f;
        renderer.fillRoundRect(
            Rect{ left, scene.bottom - height, left + width, scene.bottom }, 3.0f, theme.line.withAlpha(0.8f));
    }

    const OverlayFormats formats{ renderer.dwrite(), renderer.textFormat(TextRole::Heading),
        renderer.textFormat(TextRole::Value), renderer.textFormat(TextRole::Small),
        renderer.textFormat(TextRole::Mono) };
    const OverlayText text = makeOverlayText(model.settings.overlay, currentValues(model));
    const float scale = static_cast<float>(model.settings.overlay.scalePercent) / 100.0f * 0.9f;
    paintOverlay(renderer.target(), model.settings.overlay, theme, text, scene, scale, formats);

    renderer.strokeRoundRect(scene, kCardRadius, theme.line, 1.0f);
    hits.push_back(UiHit{ scene, ActionKind::DragOverlay, 0 });

    drawStyleGallery(renderer, model, theme, Rect{ inner.left, galleryTop, inner.right, inner.bottom }, hits);
}

void drawStyleGallery(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits)
{
    renderer.text(L"Стиль оверлея", Rect{ area.left, area.top, area.left + 120.0f, area.top + 14.0f }, TextRole::Tiny,
        theme.textDim);

    const float gap = 6.0f;
    const float rowGap = 6.0f;
    const float columnWidth = (area.width() - gap * 3.0f) / 4.0f;
    const float rowHeight = (area.height() - 16.0f - rowGap) / 2.0f;

    for (std::size_t index = 0; index < kStyleNames.size(); ++index) {
        const std::size_t column = index % 4;
        const std::size_t row = index / 4;
        const float left = area.left + static_cast<float>(column) * (columnWidth + gap);
        const float top = area.top + 16.0f + static_cast<float>(row) * (rowHeight + rowGap);
        const Rect tile{ left, top, left + columnWidth, top + rowHeight };
        const bool active = static_cast<std::size_t>(model.settings.overlay.style) == index;
        const bool hover = isHovered(model, tile);
        const OverlaySwatch swatch = overlaySwatch(static_cast<OverlayStyle>(index));

        renderer.fillRoundRect(
            tile, 8.0f, active ? theme.accentSoft : theme.panelSoft.withAlpha(hover ? 0.95f : 0.6f));
        if (active) {
            renderer.strokeRoundRect(tile, 8.0f, theme.accentGlow, 1.0f);
        }

        // Миниатюра показывает палитру и форму пресета: фон, рамку, цвет значения.
        const Rect preview{ tile.left + 5.0f, tile.top + 4.0f, tile.right - 5.0f, tile.top + 26.0f };
        if (swatch.alpha > 0.0f) {
            renderer.fillRoundRect(preview, swatch.radius * 0.6f, swatch.background.withAlpha(swatch.alpha));
        }
        if (swatch.borderVisible) {
            renderer.strokeRoundRect(preview, swatch.radius * 0.6f, swatch.border.withAlpha(0.8f), 1.0f);
        }
        // «142» и «FPS» не должны наезжать друг на друга: подпись ставим по замеру числа.
        const TextRole valueRole = swatch.mono ? TextRole::Mono : TextRole::Small;
        const float valueWidth = renderer.measureText(L"142", valueRole);
        renderer.text(L"142", Rect{ preview.left + 7.0f, preview.top, preview.left + 7.0f + valueWidth + 2.0f,
                                    preview.bottom },
            valueRole, swatch.value, TextAlign::Left);
        renderer.text(L"FPS",
            Rect{ preview.left + 9.0f + valueWidth, preview.top, preview.left + 9.0f + valueWidth + 34.0f,
                preview.bottom },
            TextRole::Tiny, swatch.accent, TextAlign::Left);
        renderer.text(L"46%", Rect{ preview.right - 38.0f, preview.top, preview.right - 6.0f, preview.bottom },
            TextRole::Tiny, swatch.value, TextAlign::Right);

        renderer.text(kStyleNames[index], Rect{ tile.left + 4.0f, tile.bottom - 15.0f, tile.right - 4.0f,
                                                 tile.bottom - 1.0f },
            TextRole::Tiny, active ? theme.accent : (hover ? theme.text : theme.textDim), TextAlign::Center);
        hits.push_back(UiHit{ tile, ActionKind::SetStyle, static_cast<int>(index) });
    }
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
        Rect{ inner.left, inner.top + 20.0f, inner.right, inner.top + 36.0f }, TextRole::Tiny, theme.textFaint);

    float top = inner.top + 44.0f;

    renderer.text(L"Положение", Rect{ inner.left, top, inner.right, top + 16.0f }, TextRole::Small, theme.textDim);
    top += 18.0f;
    const float quarterWidth = (inner.width() - 3.0f * 6.0f) / 4.0f;
    for (std::size_t index = 0; index < 4; ++index) {
        const Rect chip{ inner.left + static_cast<float>(index) * (quarterWidth + 6.0f), top,
            inner.left + static_cast<float>(index) * (quarterWidth + 6.0f) + quarterWidth, top + 26.0f };
        drawChip(renderer, chip, kPositionNames[index], static_cast<std::size_t>(overlay.position) == index, theme,
            TextRole::Tiny);
        hits.push_back(UiHit{ chip, ActionKind::SetPosition, static_cast<int>(index) });
    }
    top += 34.0f;

    renderer.text(L"Вид", Rect{ inner.left, top, inner.left + 40.0f, top + 16.0f }, TextRole::Small, theme.textDim);
    const Rect plates{ inner.left + 44.0f, top - 4.0f, inner.left + 120.0f, top + 20.0f };
    const Rect line{ inner.left + 126.0f, top - 4.0f, inner.left + 196.0f, top + 20.0f };
    drawChip(renderer, plates, L"Плитки", overlay.layout == OverlayLayout::Plates, theme, TextRole::Tiny);
    drawChip(renderer, line, L"Строка", overlay.layout == OverlayLayout::Line, theme, TextRole::Tiny);
    hits.push_back(UiHit{ plates, ActionKind::SetLayout, static_cast<int>(OverlayLayout::Plates) });
    hits.push_back(UiHit{ line, ActionKind::SetLayout, static_cast<int>(OverlayLayout::Line) });
    top += 34.0f;

    drawSlider(renderer, Rect{ inner.left, top, inner.right, top + 34.0f }, overlay.scalePercent, 70, 160,
        L"Масштаб", theme, hits, ActionKind::SetScale, L" %");
    top += 48.0f;
    drawSlider(renderer, Rect{ inner.left, top, inner.right, top + 34.0f }, overlay.opacityPercent, 0, 100,
        L"Прозрачность", theme, hits, ActionKind::SetOpacity, L" %");
    top += 42.0f;

    // Оформление: то, что нельзя выразить пресетом, но нужно для своей картинки.
    renderer.text(L"Оформление", Rect{ inner.left, top, inner.right, top + 16.0f }, TextRole::Small, theme.textDim);
    top += 18.0f;
    const std::array<std::pair<std::wstring_view, bool>, 3> switches{
        std::pair<std::wstring_view, bool>{ L"Фон", overlay.showBackground },
        std::pair<std::wstring_view, bool>{ L"Рамка", overlay.showBorder },
        std::pair<std::wstring_view, bool>{ L"Тень", overlay.showShadow }
    };
    const std::array<ActionKind, 3> switchActions{ ActionKind::ToggleOverlayBackground,
        ActionKind::ToggleOverlayBorder, ActionKind::ToggleOverlayShadow };
    const float switchWidth = (inner.width() - 2.0f * 6.0f) / 3.0f;
    for (std::size_t index = 0; index < switches.size(); ++index) {
        const Rect chip{ inner.left + static_cast<float>(index) * (switchWidth + 6.0f), top,
            inner.left + static_cast<float>(index) * (switchWidth + 6.0f) + switchWidth, top + 24.0f };
        drawChip(renderer, chip, switches[index].first, switches[index].second, theme, TextRole::Tiny,
            isHovered(model, chip));
        hits.push_back(UiHit{ chip, switchActions[index], 0 });
    }
    top += 32.0f;

    const int radiusValue = overlay.cornerRadius >= 0 ? overlay.cornerRadius : 8;
    drawSlider(renderer, Rect{ inner.left, top, inner.right, top + 34.0f }, radiusValue, 0, 24, L"Углы", theme, hits,
        ActionKind::SetOverlayRadius, L" px");
    top += 36.0f;

    // Редактор текста: шрифт, начертание и обводка — как в RivaTuner и FPS Monitor.
    renderer.text(L"Шрифт", Rect{ inner.left, top, inner.right, top + 16.0f }, TextRole::Small, theme.textDim);
    top += 18.0f;
    const std::array<std::wstring_view, 4> fontNames{ L"Сего", L"Моно", L"Узкий", L"Плотный" };
    const float fontWidth = (inner.width() - 3.0f * 6.0f) / 4.0f;
    for (std::size_t index = 0; index < fontNames.size(); ++index) {
        const Rect chip{ inner.left + static_cast<float>(index) * (fontWidth + 6.0f), top,
            inner.left + static_cast<float>(index) * (fontWidth + 6.0f) + fontWidth, top + 24.0f };
        drawChip(renderer, chip, fontNames[index], overlay.fontFamily == static_cast<int>(index), theme,
            TextRole::Tiny, isHovered(model, chip));
        hits.push_back(UiHit{ chip, ActionKind::SetOverlayFont, static_cast<int>(index) });
    }
    top += 30.0f;

    const Rect bold{ inner.left, top, inner.left + 30.0f, top + 24.0f };
    const Rect italic{ bold.right + 6.0f, top, bold.right + 36.0f, top + 24.0f };
    drawChip(renderer, bold, L"Ж", overlay.fontBold, theme, TextRole::Small, isHovered(model, bold));
    drawChip(renderer, italic, L"К", overlay.fontItalic, theme, TextRole::Small, isHovered(model, italic));
    hits.push_back(UiHit{ bold, ActionKind::ToggleOverlayBold, 0 });
    hits.push_back(UiHit{ italic, ActionKind::ToggleOverlayItalic, 0 });

    const Rect outlineMinus{ inner.left + 60.0f, top, inner.left + 86.0f, top + 24.0f };
    const Rect outlinePlus{ inner.left + 168.0f, top, inner.left + 194.0f, top + 24.0f };
    drawChip(renderer, outlineMinus, L"−", false, theme, TextRole::Small, isHovered(model, outlineMinus));
    drawChip(renderer, outlinePlus, L"+", false, theme, TextRole::Small, isHovered(model, outlinePlus));
    renderer.text(L"Обводка " + std::to_wstring(overlay.textOutline) + L" px",
        Rect{ outlineMinus.right, top, outlinePlus.left, top + 24.0f }, TextRole::Tiny, theme.textDim,
        TextAlign::Center);
    hits.push_back(UiHit{ outlineMinus, ActionKind::ChangeOverlayOutline, -1 });
    hits.push_back(UiHit{ outlinePlus, ActionKind::ChangeOverlayOutline, 1 });
    top += 30.0f;

    renderer.text(L"Смещение", Rect{ inner.left, top, inner.right, top + 16.0f }, TextRole::Small, theme.textDim);
    top += 20.0f;
    const auto drawOffset = [&](float left, ActionKind action, int value, std::wstring_view axis) {
        const Rect minus{ left, top, left + 26.0f, top + 24.0f };
        const Rect plus{ left + 92.0f, top, left + 118.0f, top + 24.0f };
        drawChip(renderer, minus, L"−", false, theme, TextRole::Small);
        drawChip(renderer, plus, L"+", false, theme, TextRole::Small);
        renderer.text(axis, Rect{ minus.right, top, plus.left, top + 24.0f }, TextRole::Tiny, theme.textFaint,
            TextAlign::Center);
        renderer.text(wideText(std::to_string(value) + " px"),
            Rect{ left - 6.0f, top + 24.0f, plus.right + 6.0f, top + 38.0f }, TextRole::Tiny, theme.textDim,
            TextAlign::Center);
        hits.push_back(UiHit{ minus, action, -10 });
        hits.push_back(UiHit{ plus, action, 10 });
    };
    drawOffset(inner.left, ActionKind::OffsetX, overlay.offsetX, L"X");
    drawOffset(inner.left + 148.0f, ActionKind::OffsetY, overlay.offsetY, L"Y");
    top += 40.0f;

    renderer.text(L"Состав", Rect{ inner.left, top, inner.right, top + 16.0f }, TextRole::Small, theme.textDim);
    top += 20.0f;
    for (std::size_t index = 0; index < kFieldNames.size(); ++index) {
        if (top + 24.0f > inner.bottom) {
            break;
        }
        drawToggleRow(renderer, Rect{ inner.left, top, inner.right, top + 24.0f }, kFieldNames[index],
            overlay.visible[index], theme, hits, ActionKind::ToggleMetric, static_cast<int>(index));
        top += 26.0f;
    }
}


void drawLabelsCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card,
    std::vector<UiHit>& hits)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);

    renderer.text(L"Подписи полей", Rect{ inner.left, inner.top, inner.right, inner.top + 22.0f }, TextRole::Heading,
        theme.text);
    renderer.text(L"клик по строке очищает подпись и начинает ввод; Esc вернёт прежнюю, Delete сотрёт целиком",
        Rect{ inner.left, inner.top + 20.0f, inner.right, inner.top + 36.0f }, TextRole::Tiny, theme.textFaint);

    const std::array<std::wstring_view, 3> presets{ L"Стандарт", L"Русские", L"Только FPS" };
    float top = inner.top + 42.0f;
    const float presetWidth = (inner.width() - 2.0f * 6.0f) / 3.0f;
    for (std::size_t index = 0; index < presets.size(); ++index) {
        const Rect chip{ inner.left + static_cast<float>(index) * (presetWidth + 6.0f), top,
            inner.left + static_cast<float>(index) * (presetWidth + 6.0f) + presetWidth, top + 24.0f };
        drawChip(renderer, chip, presets[index], false, theme, TextRole::Tiny);
        hits.push_back(UiHit{ chip, ActionKind::ApplyLabelPreset, static_cast<int>(index) });
    }
    top += 32.0f;

    for (std::size_t index = 0; index < kFieldNames.size(); ++index) {
        if (top + 24.0f > inner.bottom) {
            break;
        }
        const Rect row{ inner.left, top, inner.right, top + 24.0f };
        const bool editing = model.editingLabel == static_cast<int>(index);
        renderer.fillRoundRect(row, 9.0f, editing ? theme.accentSoft : theme.panelSoft);
        if (editing) {
            renderer.strokeRoundRect(row, 9.0f, theme.accentGlow, 1.0f);
        }
        renderer.text(kFieldNames[index], Rect{ row.left + 10.0f, row.top, row.left + 150.0f, row.bottom },
            TextRole::Tiny, theme.textDim);

        const std::string& value = labelValue(model.settings.overlay, static_cast<LabelField>(index));
        const std::wstring shown = wideText(value);
        renderer.text(editing ? shown + L"|" : (shown.empty() ? L"скрыто" : shown),
            Rect{ row.left + 156.0f, row.top, row.right - 34.0f, row.bottom }, TextRole::Small,
            editing ? theme.accent : (shown.empty() ? theme.textFaint : theme.text), TextAlign::Right);

        // Крестик стирает подпись одним нажатием: пустая подпись — валидное состояние.
        const Rect clear{ row.right - 28.0f, row.centerY() - 9.0f, row.right - 10.0f, row.centerY() + 9.0f };
        renderer.text(L"×", clear, TextRole::Small, theme.textFaint, TextAlign::Center);
        hits.push_back(UiHit{ row, ActionKind::SelectLabel, static_cast<int>(index) });
        hits.push_back(UiHit{ clear, ActionKind::ClearLabel, static_cast<int>(index) });
        top += 28.0f;
    }
}

} // namespace

void drawOverlayView(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits)
{
    const float previewWidth = area.width() * 0.58f;
    const float previewHeight = std::max(220.0f, (area.height() - kGap) * 0.58f);

    const Rect preview{ area.left, area.top, area.left + previewWidth, area.top + previewHeight };
    const Rect labels{ area.left, preview.bottom + kGap, preview.right, area.bottom };
    const Rect controls{ preview.right + kGap, area.top, area.right, area.bottom };

    drawPreviewCard(renderer, model, theme, preview, hits);
    drawLabelsCard(renderer, model, theme, labels, hits);
    drawControlsCard(renderer, model, theme, controls, hits);
}

} // namespace yawa

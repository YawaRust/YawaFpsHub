#include "ui/overlay_view.h"
#include "render/overlay_painter.h"
#include "ui/hotkey_text.h"
#include "ui/layout.h"

#include <algorithm>
#include <array>
#include <cmath>
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

// Ползунок с готовой подписью значения: нужен там, где положение бегунка и число
// не совпадают («из стиля» рисуется на месте значения пресета).
void drawSliderAt(Renderer& renderer, const Rect& row, float fraction, std::wstring_view caption,
    std::wstring_view valueText, const Theme& theme, std::vector<UiHit>& hits, ActionKind action)
{
    renderer.text(caption, Rect{ row.left, row.top, row.left + 160.0f, row.top + 16.0f }, TextRole::Small,
        theme.textDim);
    renderer.text(valueText, Rect{ row.right - 90.0f, row.top, row.right, row.top + 16.0f }, TextRole::Small,
        theme.text, TextAlign::Right);

    const Rect track{ row.left, row.top + 24.0f, row.right, row.top + 34.0f };
    drawBar(renderer, track, std::clamp(fraction, 0.0f, 1.0f), theme.accent, theme);
    renderer.fillCircle(track.left + track.width() * std::clamp(fraction, 0.0f, 1.0f), track.centerY(), 6.0f,
        theme.accent);
    hits.push_back(UiHit{ Rect{ track.left, track.top - 8.0f, track.right, track.bottom + 8.0f }, action, 0 });
}

// Палитра цветов оверлея: «авто» (из пресета) и шесть цветов.
void drawColorRow(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& row,
    std::wstring_view caption, int selected, ActionKind action, std::vector<UiHit>& hits)
{
    renderer.text(caption, Rect{ row.left, row.top, row.left + 150.0f, row.bottom }, TextRole::Small,
        theme.textDim);

    constexpr int kCount = kOverlayColorCount + 1;
    const float chipWidth = 26.0f;
    const float gap = 4.0f;
    const float total = static_cast<float>(kCount) * chipWidth + static_cast<float>(kCount - 1) * gap;
    float left = row.right - total;
    const auto palette = overlayColorPalette();
    for (int index = -1; index < kOverlayColorCount; ++index) {
        const Rect chip{ left, row.top, left + chipWidth, row.top + 22.0f };
        const bool active = index == selected;
        const bool hover = isHovered(model, chip);
        renderer.fillRoundRect(chip, 7.0f, active ? theme.accentSoft : theme.panelSoft.withAlpha(hover ? 0.95f : 0.6f));
        if (active) {
            renderer.strokeRoundRect(chip, 7.0f, theme.accentGlow, 1.0f);
        }
        if (index < 0) {
            renderer.text(L"авто", chip, TextRole::Tiny, active ? theme.accent : theme.textFaint,
                TextAlign::Center);
        } else {
            renderer.fillCircle(chip.centerX(), chip.centerY(), 6.0f,
                palette[static_cast<std::size_t>(index)].withAlpha(active ? 1.0f : 0.85f));
        }
        hits.push_back(UiHit{ chip, action, index });
        left += chipWidth + gap;
    }
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

// Скруглённые углы для сцены: Renderer рисует клипом по прямоугольнику, поэтому
// «съедаем» уголки фоном карточки — по строкам дуги, как маска.
void maskCorners(Renderer& renderer, const Rect& rect, float radius, const Color& color)
{
    const int steps = static_cast<int>(radius);
    for (int row = 0; row < steps; ++row) {
        const float y = static_cast<float>(row);
        const float dy = radius - y;
        const float bite = radius - std::sqrt(std::max(radius * radius - dy * dy, 0.0f));
        if (bite <= 0.05f) {
            continue;
        }
        const float top = rect.top + y;
        renderer.fillRect(Rect{ rect.left, top, rect.left + bite, top + 1.0f }, color);
        renderer.fillRect(Rect{ rect.right - bite, top, rect.right, top + 1.0f }, color);
        const float bottom = rect.bottom - y - 1.0f;
        renderer.fillRect(Rect{ rect.left, bottom, rect.left + bite, bottom + 1.0f }, color);
        renderer.fillRect(Rect{ rect.right - bite, bottom, rect.right, bottom + 1.0f }, color);
    }
}

Color mixColor(const Color& from, const Color& to, float t)
{
    const float k = std::clamp(t, 0.0f, 1.0f);
    return Color{ from.r + (to.r - from.r) * k, from.g + (to.g - from.g) * k, from.b + (to.b - from.b) * k,
        from.a + (to.a - from.a) * k };
}

// Макет игровой сцены как в прототипе: вечернее небо, солнце за хребтом, сетка пола
// и полоса задания. На нём сразу видно, читается ли оверлей на живом фоне.
void drawSceneBackdrop(Renderer& renderer, const Rect& scene, const Theme& theme)
{
    const Color skyTop = fromRgba(0xFF0B1020);
    const Color skyMid = fromRgba(0xFF3A2B4A);
    const Color skyLow = fromRgba(0xFF8A4A32);
    const Color horizon = fromRgba(0xFFFF8A3D);
    const Color ground = fromRgba(0xFF05070B);

    renderer.fillRoundRect(scene, 14.0f, skyTop);
    const float skyBottom = scene.top + scene.height() * 0.62f;
    const int bands = 48;
    const float bandHeight = (skyBottom - scene.top) / static_cast<float>(bands);
    for (int band = 0; band < bands; ++band) {
        const float t = static_cast<float>(band) / static_cast<float>(bands - 1);
        const Color color = t < 0.55f ? mixColor(skyTop, skyMid, t / 0.55f)
                                      : mixColor(skyMid, t < 0.88f ? skyLow : horizon,
                                            (t - 0.55f) / 0.45f);
        const float top = scene.top + static_cast<float>(band) * bandHeight;
        renderer.fillRect(Rect{ scene.left, top, scene.right, top + bandHeight + 0.6f }, color);
    }

    // Солнце: мягкое свечение и сам диск у линии хребта.
    const float sunX = scene.left + scene.width() * 0.66f;
    const float sunY = skyBottom - 22.0f;
    renderer.fillRadialGlow(sunX, sunY, scene.height() * 0.36f, fromRgba(0xFFFF9A4D).withAlpha(0.26f));
    renderer.fillCircle(sunX, sunY, 21.0f, fromRgba(0xFFFFC46B).withAlpha(0.62f));
    renderer.fillCircle(sunX, sunY, 13.0f, fromRgba(0xFFFFE7BC).withAlpha(0.45f));

    // Земля и перспективная сетка: горизонтали сгущаются к горизонту.
    renderer.fillRect(Rect{ scene.left, skyBottom, scene.right, scene.bottom }, ground.withAlpha(0.92f));
    const int rows = 9;
    for (int row = 1; row <= rows; ++row) {
        const float t = static_cast<float>(row) / static_cast<float>(rows);
        const float y = skyBottom + (scene.bottom - skyBottom) * t * t;
        renderer.drawLine(scene.left, y, scene.right, y, fromRgba(0xFFFFB27A).withAlpha(0.10f), 1.0f);
    }
    const float centerX = scene.centerX();
    for (int column = -5; column <= 5; ++column) {
        if (column == 0) {
            continue;
        }
        const float bottomX = centerX + static_cast<float>(column) * (scene.width() / 6.0f);
        renderer.drawLine(centerX + static_cast<float>(column) * 12.0f, skyBottom, bottomX, scene.bottom,
            fromRgba(0xFFFFB27A).withAlpha(0.08f), 1.0f);
    }

    // Хребет: плосковерхие скалы со скосами, как в сцене прототипа. Скосы — два
    // более низких столбца по краям: силуэт получается ступенчатым, а не «кирпичным».
    constexpr int kPeaks = 13;
    const Color rock = fromRgba(0xFF0A0C12).withAlpha(0.95f);
    for (int peak = 0; peak < kPeaks; ++peak) {
        const float step = scene.width() / static_cast<float>(kPeaks);
        const float left = scene.left + step * static_cast<float>(peak);
        const float slim = step * 0.52f;
        const float height = 14.0f + static_cast<float>((peak * 53) % 34);
        const float center = left + step * 0.44f;
        renderer.fillRoundRect(Rect{ center, skyBottom - height - 4.0f, center + slim, skyBottom + 2.0f }, 2.0f,
            rock);
        renderer.fillRoundRect(Rect{ center - step * 0.10f, skyBottom - height * 0.62f,
                                   center + step * 0.05f, skyBottom + 2.0f },
            2.0f, rock);
        renderer.fillRoundRect(Rect{ center + slim - step * 0.05f, skyBottom - height * 0.66f,
                                   center + slim + step * 0.10f, skyBottom + 2.0f },
            2.0f, rock);
    }
    renderer.fillRect(Rect{ scene.left, skyBottom - 1.0f, scene.right, skyBottom + 2.0f },
        fromRgba(0xFFFFA45C).withAlpha(0.16f));

    // Прицел в центре и карточка задания снизу.
    const float crossY = scene.top + scene.height() * 0.46f;
    renderer.drawLine(scene.centerX() - 7.0f, crossY, scene.centerX() + 7.0f, crossY,
        fromRgba(0xFFFFFFFF).withAlpha(0.55f), 1.2f);
    renderer.drawLine(scene.centerX(), crossY - 7.0f, scene.centerX(), crossY + 7.0f,
        fromRgba(0xFFFFFFFF).withAlpha(0.55f), 1.2f);

    const float taskTop = scene.bottom - 44.0f;
    renderer.text(L"Задание · Погоня в Найт-Сити",
        Rect{ scene.left + 20.0f, taskTop, scene.left + 260.0f, taskTop + 16.0f }, TextRole::Tiny,
        fromRgba(0xFFFFFFFF).withAlpha(0.72f));
    const Rect progress{ scene.left + 20.0f, taskTop + 22.0f, scene.left + scene.width() * 0.36f,
        taskTop + 26.0f };
    renderer.fillRoundRect(progress, 2.0f, fromRgba(0xFFFFFFFF).withAlpha(0.14f));
    renderer.fillRoundRect(Rect{ progress.left, progress.top, progress.left + progress.width() * 0.25f,
                               progress.bottom },
        2.0f, fromRgba(0xFFFFFFFF).withAlpha(0.8f));
    renderer.text(L"24", Rect{ scene.right - 92.0f, taskTop + 6.0f, scene.right - 46.0f, taskTop + 34.0f },
        TextRole::Value, fromRgba(0xFFFFFFFF).withAlpha(0.92f), TextAlign::Right);
    renderer.text(L"/ 96", Rect{ scene.right - 46.0f, taskTop + 12.0f, scene.right - 18.0f, taskTop + 32.0f },
        TextRole::Small, fromRgba(0xFFFFFFFF).withAlpha(0.55f));

    maskCorners(renderer, scene, 14.0f, theme.panel);
}

void drawPreviewCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card,
    std::vector<UiHit>& hits)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);

    renderer.text(L"Оверлей поверх игры", Rect{ inner.left, inner.top, inner.right, inner.top + 22.0f },
        TextRole::Heading, theme.text);
    renderer.text(L"перетаскивание — прямо по оверлею; правый клик по нему возвращает это окно",
        Rect{ inner.left, inner.top + 20.0f, inner.right - 60.0f, inner.top + 38.0f }, TextRole::Tiny, theme.textFaint);

    const Rect tag{ inner.right - 72.0f, inner.top, inner.right, inner.top + 20.0f };
    renderer.fillRoundRect(tag, 10.0f, theme.panelSoft);
    renderer.text(hotkeyName(model.settings.hotkeys.overlay), tag, TextRole::Tiny, theme.textFaint,
        TextAlign::Center);

    const Rect scene{ inner.left, inner.top + 46.0f, inner.right, inner.bottom };

    // Сцена: на ней сразу видно, читается ли оверлей поверх живого фона.
    renderer.pushClip(scene);
    drawSceneBackdrop(renderer, scene, theme);
    renderer.popClip();

    const OverlayFormats formats{ renderer.dwrite(), renderer.textFormat(TextRole::Heading),
        renderer.textFormat(TextRole::Value), renderer.textFormat(TextRole::Small),
        renderer.textFormat(TextRole::Mono) };
    const OverlayText text = makeOverlayText(model.settings.overlay, currentValues(model));
    const float scale = static_cast<float>(model.settings.overlay.scalePercent) / 100.0f * 0.9f;
    paintOverlay(renderer.target(), model.settings.overlay, theme, text, scene, scale, formats);

    renderer.strokeRoundRect(scene, 14.0f, theme.line, 1.0f);
    hits.push_back(UiHit{ scene, ActionKind::DragOverlay, 0 });
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

    float top = inner.top + 40.0f;

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
    top += 30.0f;

    renderer.text(L"Вид", Rect{ inner.left, top, inner.left + 40.0f, top + 16.0f }, TextRole::Small, theme.textDim);
    const Rect plates{ inner.left + 44.0f, top - 4.0f, inner.left + 120.0f, top + 20.0f };
    const Rect line{ inner.left + 126.0f, top - 4.0f, inner.left + 196.0f, top + 20.0f };
    drawChip(renderer, plates, L"Плитки", overlay.layout == OverlayLayout::Plates, theme, TextRole::Tiny);
    drawChip(renderer, line, L"Строка", overlay.layout == OverlayLayout::Line, theme, TextRole::Tiny);
    hits.push_back(UiHit{ plates, ActionKind::SetLayout, static_cast<int>(OverlayLayout::Plates) });
    hits.push_back(UiHit{ line, ActionKind::SetLayout, static_cast<int>(OverlayLayout::Line) });
    top += 30.0f;

    drawSlider(renderer, Rect{ inner.left, top, inner.right, top + 34.0f }, overlay.scalePercent, 70, 160,
        L"Масштаб плашки", theme, hits, ActionKind::SetScale, L" %");
    top += 44.0f;
    // Размер текста — отдельно от масштаба: цифры можно укрупнить, не раздувая плашку.
    drawSlider(renderer, Rect{ inner.left, top, inner.right, top + 34.0f }, overlay.textScalePercent, 85, 130,
        L"Размер текста", theme, hits, ActionKind::SetOverlayTextScale, L" %");
    top += 44.0f;
    drawSlider(renderer, Rect{ inner.left, top, inner.right, top + 34.0f }, overlay.opacityPercent, 0, 100,
        L"Прозрачность окна", theme, hits, ActionKind::SetOpacity, L" %");
    top += 44.0f;

    // Прозрачность самой подложки: «Фон» и прозрачность окна — разные вещи.
    const int backgroundOpacity = overlay.backgroundOpacityPercent;
    drawSliderAt(renderer, Rect{ inner.left, top, inner.right, top + 34.0f },
        backgroundOpacity < 0 ? 1.0f : static_cast<float>(backgroundOpacity) / 100.0f, L"Фон подложки",
        backgroundOpacity < 0 ? L"из стиля" : std::to_wstring(backgroundOpacity) + L" %", theme, hits,
        ActionKind::SetOverlayBgOpacity);
    top += 44.0f;

    // Оформление: то, что нельзя выразить пресетом, но нужно для своей картинки.
    renderer.text(L"Оформление", Rect{ inner.left, top, inner.right, top + 16.0f }, TextRole::Small, theme.textDim);
    top += 18.0f;
    const std::array<std::pair<std::wstring_view, bool>, 4> switches{
        std::pair<std::wstring_view, bool>{ L"Фон", overlay.showBackground },
        std::pair<std::wstring_view, bool>{ L"Рамка", overlay.showBorder },
        std::pair<std::wstring_view, bool>{ L"Тень", overlay.showShadow },
        std::pair<std::wstring_view, bool>{ L"Сквозной клик", overlay.clickThrough }
    };
    const std::array<ActionKind, 4> switchActions{ ActionKind::ToggleOverlayBackground,
        ActionKind::ToggleOverlayBorder, ActionKind::ToggleOverlayShadow,
        ActionKind::ToggleOverlayClickThrough };
    const float switchWidth = (inner.width() - 3.0f * 6.0f) / 4.0f;
    for (std::size_t index = 0; index < switches.size(); ++index) {
        const Rect chip{ inner.left + static_cast<float>(index) * (switchWidth + 6.0f), top,
            inner.left + static_cast<float>(index) * (switchWidth + 6.0f) + switchWidth, top + 24.0f };
        drawChip(renderer, chip, switches[index].first, switches[index].second, theme, TextRole::Tiny,
            isHovered(model, chip));
        hits.push_back(UiHit{ chip, switchActions[index], 0 });
    }
    top += 30.0f;

    const int radiusValue = overlay.cornerRadius >= 0 ? overlay.cornerRadius : 8;
    drawSlider(renderer, Rect{ inner.left, top, inner.right, top + 34.0f }, radiusValue, 0, 24, L"Углы", theme, hits,
        ActionKind::SetOverlayRadius, L" px");
    top += 40.0f;

    // Отступы и межстрочный интервал: плотный RTSS и воздушный «Неон» настраиваются вручную.
    const int padding = overlay.paddingPx;
    drawSliderAt(renderer, Rect{ inner.left, top, inner.right, top + 34.0f },
        padding < 0 ? 0.36f : static_cast<float>(padding) / 28.0f, L"Отступы внутри",
        padding < 0 ? L"из стиля" : std::to_wstring(padding) + L" px", theme, hits, ActionKind::SetOverlayPadding);
    top += 44.0f;

    const int rowGap = overlay.rowGapPx;
    drawSliderAt(renderer, Rect{ inner.left, top, inner.right, top + 34.0f },
        rowGap < 0 ? 0.18f : static_cast<float>(rowGap) / 14.0f, L"Интервал строк",
        rowGap < 0 ? L"из стиля" : std::to_wstring(rowGap) + L" px", theme, hits, ActionKind::SetOverlayRowGap);
    top += 44.0f;

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
    top += 28.0f;

    const Rect bold{ inner.left, top, inner.left + 30.0f, top + 24.0f };
    const Rect italic{ bold.right + 6.0f, top, bold.right + 36.0f, top + 24.0f };
    drawChip(renderer, bold, L"Ж", overlay.fontBold, theme, TextRole::Small, isHovered(model, bold));
    drawChip(renderer, italic, L"К", overlay.fontItalic, theme, TextRole::Small, isHovered(model, italic));
    hits.push_back(UiHit{ bold, ActionKind::ToggleOverlayBold, 0 });
    hits.push_back(UiHit{ italic, ActionKind::ToggleOverlayItalic, 0 });

    // Между «К» и «−» нужен зазор: раньше чипы обводки наезжали на начертание.
    const Rect outlineMinus{ inner.left + 76.0f, top, inner.left + 102.0f, top + 24.0f };
    const Rect outlinePlus{ inner.left + 184.0f, top, inner.left + 210.0f, top + 24.0f };
    drawChip(renderer, outlineMinus, L"−", false, theme, TextRole::Small, isHovered(model, outlineMinus));
    drawChip(renderer, outlinePlus, L"+", false, theme, TextRole::Small, isHovered(model, outlinePlus));
    renderer.text(L"Обводка " + std::to_wstring(overlay.textOutline) + L" px",
        Rect{ outlineMinus.right, top, outlinePlus.left, top + 24.0f }, TextRole::Tiny, theme.textDim,
        TextAlign::Center);
    hits.push_back(UiHit{ outlineMinus, ActionKind::ChangeOverlayOutline, -1 });
    hits.push_back(UiHit{ outlinePlus, ActionKind::ChangeOverlayOutline, 1 });
    top += 32.0f;

    // Цвета по элементам: значения и подписи настраиваются отдельно.
    drawColorRow(renderer, model, theme, Rect{ inner.left, top, inner.right, top + 22.0f }, L"Цвет значений",
        overlay.valueColor, ActionKind::SetValueColor, hits);
    top += 26.0f;
    drawColorRow(renderer, model, theme, Rect{ inner.left, top, inner.right, top + 22.0f }, L"Цвет подписей",
        overlay.labelColor, ActionKind::SetLabelColor, hits);
    top += 28.0f;

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
    top += 38.0f;

    renderer.text(L"Состав", Rect{ inner.left, top, inner.right, top + 16.0f }, TextRole::Small, theme.textDim);
    top += 18.0f;
    for (std::size_t index = 0; index < kFieldNames.size(); ++index) {
        if (top + 24.0f > inner.bottom) {
            break;
        }
        drawToggleRow(renderer, Rect{ inner.left, top, inner.right, top + 24.0f }, kFieldNames[index],
            overlay.visible[index], theme, hits, ActionKind::ToggleMetric, static_cast<int>(index));
        top += 25.0f;
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
    // Как в демо: одна колонка карточек фиксированной высоты. Экран получает
    // демо-высоту целиком, поэтому при сжатии окна карточки не наезжают друг на друга,
    // а прокручиваются.
    const auto drawClipped = [&](const Rect& card, const auto& draw) {
        renderer.pushClip(card);
        draw(card);
        renderer.popClip();
    };

    float top = area.top;
    const auto place = [&](float height) {
        const Rect card{ area.left, top, area.right, top + height };
        top += height + kGap;
        return card;
    };

    drawClipped(place(430.0f),
        [&](const Rect& card) { drawPreviewCard(renderer, model, theme, card, hits); });
    drawClipped(place(880.0f),
        [&](const Rect& card) { drawControlsCard(renderer, model, theme, card, hits); });
    drawClipped(place(232.0f),
        [&](const Rect& card) { drawLabelsCard(renderer, model, theme, card, hits); });
    drawClipped(place(268.0f), [&](const Rect& card) {
        drawCard(renderer, card, theme);
        drawStyleGallery(renderer, model, theme, card.inset(kCardPadding), hits);
    });
}

} // namespace yawa

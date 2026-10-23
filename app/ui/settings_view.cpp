#include "ui/settings_view.h"
#include "ui/layout.h"

#include <algorithm>
#include <array>
#include <string>

namespace yawa {
namespace {

constexpr std::array<std::wstring_view, 5> kStyleNames{ L"Стекло", L"Неон", L"Капсулы", L"Терминал", L"Минимал" };
constexpr std::array<std::wstring_view, 4> kPositionNames{ L"↖", L"↗", L"↙", L"↘" };
constexpr std::array<int, 3> kIntervals{ 500, 1000, 2000 };
constexpr std::array<int, 4> kTargets{ 60, 120, 144, 165 };
constexpr std::array<LabelField, 5> kLabelFields{ LabelField::Fps, LabelField::Cpu, LabelField::Gpu,
    LabelField::Ram, LabelField::Vram };
constexpr std::array<std::wstring_view, 5> kLabelTitles{ L"Кадры", L"Процессор", L"Видеокарта", L"Память",
    L"Видеопамять" };

void drawSectionHeader(Renderer& renderer, const Rect& rect, std::wstring_view title, std::wstring_view hint,
    const Theme& theme)
{
    renderer.text(title, Rect{ rect.left, rect.top, rect.right, rect.top + 20.0f }, TextRole::Heading, theme.text);
    if (!hint.empty()) {
        renderer.text(hint, Rect{ rect.left, rect.top + 18.0f, rect.right, rect.top + 36.0f }, TextRole::Tiny,
            theme.textFaint);
    }
}

void drawChip(Renderer& renderer, const Rect& rect, std::wstring_view label, bool isActive, const Theme& theme)
{
    renderer.fillRoundRect(rect, 9.0f, isActive ? theme.accentSoft : theme.panelSoft);
    if (isActive) {
        renderer.strokeRoundRect(rect, 9.0f, theme.accentGlow, 1.0f);
    }
    renderer.text(label, rect, TextRole::Small, isActive ? theme.accent : theme.textDim, TextAlign::Center);
}

// Длинный путь не влезает в карточку: оставляем хвост, начало заменяем многоточием,
// чтобы пользователь всё равно видел, где лежит файл.
std::wstring clipPathLeft(Renderer& renderer, std::wstring value, float maxWidth)
{
    // Запас в 8 %: DirectWrite и фактическая отрисовка расходятся на пару пикселей,
    // без запаса хвост пути обрезался бы посреди буквы.
    const float limit = maxWidth * 0.92f;
    if (renderer.measureText(value, TextRole::Mono) <= limit) {
        return value;
    }

    const std::wstring prefix = L"…";
    while (!value.empty() && renderer.measureText(prefix + value, TextRole::Mono) > limit) {
        value.erase(0, 1);
    }
    return prefix + value;
}

Rect chipRow(float left, float top, float width, float height)
{
    return Rect{ left, top, left + width, top + height };
}

void drawOverlayCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card,
    std::vector<UiHit>& hits)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    drawSectionHeader(renderer, inner, L"Оверлей", L"стиль виден сразу в игре, F10 — показать или скрыть", theme);

    float top = inner.top + 46.0f;
    const float tileWidth = (inner.width() - 4.0f * 8.0f) / 5.0f;
    for (std::size_t index = 0; index < kStyleNames.size(); ++index) {
        const Rect tile = chipRow(inner.left + static_cast<float>(index) * (tileWidth + 8.0f), top, tileWidth,
            30.0f);
        drawChip(renderer, tile, kStyleNames[index],
            static_cast<std::size_t>(model.settings.overlay.style) == index, theme);
        hits.push_back(UiHit{ tile, ActionKind::SetStyle, static_cast<int>(index) });
    }

    top += 42.0f;
    renderer.text(L"Положение", Rect{ inner.left, top, inner.left + 90.0f, top + 20.0f }, TextRole::Small,
        theme.textFaint);
    for (std::size_t index = 0; index < kPositionNames.size(); ++index) {
        const Rect button{ inner.left + 96.0f + static_cast<float>(index) * 38.0f, top, inner.left + 128.0f
                + static_cast<float>(index) * 38.0f, top + 24.0f };
        drawChip(renderer, button, kPositionNames[index],
            static_cast<std::size_t>(model.settings.overlay.position) == index, theme);
        hits.push_back(UiHit{ button, ActionKind::SetPosition, static_cast<int>(index) });
    }

    renderer.text(L"Вид", Rect{ inner.left + 276.0f, top, inner.left + 306.0f, top + 20.0f }, TextRole::Small,
        theme.textFaint);
    const Rect plates{ inner.left + 312.0f, top, inner.left + 386.0f, top + 24.0f };
    const Rect line{ inner.left + 392.0f, top, inner.left + 452.0f, top + 24.0f };
    drawChip(renderer, plates, L"Плитки", model.settings.overlay.layout == OverlayLayout::Plates, theme);
    drawChip(renderer, line, L"Строка", model.settings.overlay.layout == OverlayLayout::Line, theme);
    hits.push_back(UiHit{ plates, ActionKind::SetLayout, static_cast<int>(OverlayLayout::Plates) });
    hits.push_back(UiHit{ line, ActionKind::SetLayout, static_cast<int>(OverlayLayout::Line) });

    top += 40.0f;
    renderer.text(L"Интервал опроса", Rect{ inner.left, top, inner.left + 100.0f, top + 20.0f }, TextRole::Small,
        theme.textFaint);
    for (std::size_t index = 0; index < kIntervals.size(); ++index) {
        const Rect button{ inner.left + 104.0f + static_cast<float>(index) * 82.0f, top, inner.left + 180.0f
                + static_cast<float>(index) * 82.0f, top + 24.0f };
        drawChip(renderer, button, wideText(std::to_string(kIntervals[index]) + " мс"),
            model.settings.capture.intervalMs == kIntervals[index], theme);
        hits.push_back(UiHit{ button, ActionKind::SetInterval, kIntervals[index] });
    }

    top += 36.0f;
    renderer.text(L"Цель кадра", Rect{ inner.left, top, inner.left + 100.0f, top + 20.0f }, TextRole::Small,
        theme.textFaint);
    for (std::size_t index = 0; index < kTargets.size(); ++index) {
        const Rect button{ inner.left + 104.0f + static_cast<float>(index) * 60.0f, top, inner.left + 158.0f
                + static_cast<float>(index) * 60.0f, top + 24.0f };
        drawChip(renderer, button, wideText(std::to_string(kTargets[index]) + " Гц"),
            model.settings.capture.targetHz == kTargets[index], theme);
        hits.push_back(UiHit{ button, ActionKind::SetInterval, kTargets[index] });
    }
}

void drawLabelsCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card,
    std::vector<UiHit>& hits)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    drawSectionHeader(renderer, inner, L"Подписи полей", L"нажмите, чтобы изменить, Enter — сохранить", theme);

    float top = inner.top + 46.0f;
    for (std::size_t index = 0; index < kLabelFields.size(); ++index) {
        const Rect row{ inner.left, top, inner.right, top + 28.0f };
        const bool isEditing = model.editingLabel == static_cast<int>(index);

        renderer.text(kLabelTitles[index], Rect{ row.left, row.top, row.left + 130.0f, row.bottom }, TextRole::Small,
            theme.textDim);

        const Rect field{ row.right - 132.0f, row.top, row.right, row.bottom };
        renderer.fillRoundRect(field, 8.0f, isEditing ? theme.accentSoft : theme.panelSoft);
        renderer.strokeRoundRect(field, 8.0f, isEditing ? theme.accentGlow : theme.line, 1.0f);

        std::wstring value = wideText(labelValue(model.settings.overlay, kLabelFields[index]));
        if (isEditing) {
            value += L"|";
        }
        renderer.text(value, field.inset(8.0f), TextRole::Small, theme.text, TextAlign::Left);

        hits.push_back(UiHit{ field, ActionKind::SelectLabel, static_cast<int>(index) });
        top += 34.0f;
    }

    const Rect reset{ inner.left, inner.bottom - 30.0f, inner.left + 128.0f, inner.bottom };
    drawChip(renderer, reset, L"Сбросить", false, theme);
    hits.push_back(UiHit{ reset, ActionKind::ResetLabels, 0 });
}

void drawGpuCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card,
    std::vector<UiHit>& hits)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    drawSectionHeader(renderer, inner, L"Видеокарта",
        model.gpuNames.size() > 1 ? L"несколько адаптеров — выбор по клику" : L"адаптер выбран автоматически",
        theme);

    float top = inner.top + 46.0f;
    for (std::size_t index = 0; index < model.gpuNames.size() && index < 4; ++index) {
        const Rect row{ inner.left, top, inner.right, top + 30.0f };
        const bool isActive = index == model.activeGpu;
        renderer.fillRoundRect(row, 9.0f, isActive ? theme.accentSoft : theme.panelSoft);
        if (isActive) {
            renderer.strokeRoundRect(row, 9.0f, theme.accentGlow, 1.0f);
        }
        renderer.text(model.gpuNames[index], row.inset(10.0f), TextRole::Small,
            isActive ? theme.accent : theme.textDim);
        hits.push_back(UiHit{ row, ActionKind::CycleGpu, static_cast<int>(index) });
        top += 36.0f;
    }
}

void drawPortableCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card,
    std::vector<UiHit>& hits)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    drawSectionHeader(renderer, inner, L"Портативность", L"реестр и сеть не используются", theme);

    const std::pair<std::wstring, std::wstring> rows[] = {
        { L"Настройки", wideText(model.settingsPath) },
        { L"Журнал", wideText(model.logPath) },
        { L"Сессии", wideText(model.sessionsPath) },
    };

    float top = inner.top + 46.0f;
    for (const auto& [label, value] : rows) {
        renderer.text(label, Rect{ inner.left, top, inner.left + 92.0f, top + 18.0f }, TextRole::Small,
            theme.textDim);
        renderer.text(clipPathLeft(renderer, value, inner.right - inner.left - 96.0f),
            Rect{ inner.left + 96.0f, top, inner.right, top + 18.0f }, TextRole::Mono, theme.textFaint);
        top += 26.0f;
    }

    renderer.text(L"ini пишется атомарно рядом с exe, реестр и сеть не используются",
        Rect{ inner.left, top + 6.0f, inner.right, top + 24.0f }, TextRole::Tiny, theme.textFaint);

    const Rect compact{ inner.left, inner.bottom - 32.0f, inner.left + 150.0f, inner.bottom - 4.0f };
    drawChip(renderer, compact, L"Компактный вид", model.settings.ui.compactDensity, theme);
    hits.push_back(UiHit{ compact, ActionKind::ToggleCompact, 0 });

    const Rect folder{ inner.left + 158.0f, inner.bottom - 32.0f, inner.left + 290.0f, inner.bottom - 4.0f };
    drawChip(renderer, folder, L"Открыть папку", false, theme);
    hits.push_back(UiHit{ folder, ActionKind::OpenSettingsFolder, 0 });

    const Rect reset{ inner.right - 130.0f, inner.bottom - 32.0f, inner.right, inner.bottom - 4.0f };
    drawChip(renderer, reset, L"Сбросить всё", false, theme);
    hits.push_back(UiHit{ reset, ActionKind::ResetSettings, 0 });
}

} // namespace

void drawSettings(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits)
{
    const float leftWidth = std::max(area.width() * 0.58f, 460.0f);
    const Rect left{ area.left, area.top, area.left + leftWidth - kGap, area.bottom };
    const Rect right{ left.right + kGap, area.top, area.right, area.bottom };

    const float overlayHeight = 232.0f;
    drawOverlayCard(renderer, model, theme, Rect{ left.left, left.top, left.right, left.top + overlayHeight }, hits);
    drawPortableCard(renderer, model, theme,
        Rect{ left.left, left.top + overlayHeight + kGap, left.right, left.bottom }, hits);

    const float labelsHeight = std::clamp(area.height() * 0.55f, 230.0f, 280.0f);
    drawLabelsCard(renderer, model, theme, Rect{ right.left, right.top, right.right, right.top + labelsHeight }, hits);
    drawGpuCard(renderer, model, theme,
        Rect{ right.left, right.top + labelsHeight + kGap, right.right, right.bottom }, hits);
}

} // namespace yawa

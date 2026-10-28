#include "ui/settings_view.h"
#include "ui/hotkey_text.h"
#include "ui/layout.h"

#include "metrics/units.h"

#include <algorithm>
#include <array>
#include <string>

namespace yawa {
namespace {

constexpr std::array<int, 3> kIntervals{ 500, 1000, 2000 };
constexpr std::array<int, 4> kTargets{ 60, 120, 144, 165 };
constexpr std::array<std::wstring_view, 5> kAccentNames{ L"Мята", L"Лёд", L"Фиалка", L"Янтарь", L"Коралл" };
constexpr std::array<std::uint32_t, 5> kAccentRgba{ 0xFF5EF29B, 0xFF6AA8FF, 0xFFB47BFF, 0xFFFFB347, 0xFFFF6F6F };

// Свой заголовок карточки: в настройках подпись идёт на всю ширину и занимает 34 px,
// тогда как на обзоре и «Балансе» она короче на 2 px — поэтому helper не переиспользуем.
float drawSettingsCardHeader(Renderer& renderer, const Rect& card, std::wstring_view title,
    std::wstring_view subtitle, const Theme& theme)
{
    const Rect inner = card.inset(kCardPadding);
    renderer.text(title, Rect{ inner.left, inner.top, inner.right, inner.top + 20.0f }, TextRole::Heading, theme.text);
    if (!subtitle.empty()) {
        renderer.text(subtitle, Rect{ inner.left, inner.top + 18.0f, inner.right, inner.top + 34.0f }, TextRole::Tiny,
            theme.textFaint);
    }
    return inner.top + 34.0f;
}

void drawSwitchRow(Renderer& renderer, const Rect& row, std::wstring_view label, std::wstring_view note, bool on,
    const Theme& theme)
{
    const Rect track{ row.right - 38.0f, row.centerY() - 9.0f, row.right, row.centerY() + 9.0f };
    renderer.fillRoundRect(track, 9.0f, on ? theme.accentSoft : theme.panelSoft);
    if (on) {
        renderer.strokeRoundRect(track, 9.0f, theme.accentGlow, 1.0f);
        renderer.fillCircle(track.right - 9.0f, track.centerY(), 6.0f, theme.accent);
    } else {
        renderer.fillCircle(track.left + 9.0f, track.centerY(), 6.0f, theme.textFaint);
    }

    renderer.text(label, Rect{ row.left, row.top, track.left - 12.0f, row.top + 16.0f }, TextRole::Small, theme.text);
    if (!note.empty()) {
        renderer.text(note, Rect{ row.left, row.top + 16.0f, track.left - 12.0f, row.top + 32.0f }, TextRole::Tiny,
            theme.textFaint);
    }
}

std::wstring clipRight(Renderer& renderer, std::wstring text, TextRole role, float maxWidth)
{
    if (renderer.measureText(text, role) <= maxWidth) {
        return text;
    }
    while (!text.empty() && renderer.measureText(text + L"…", role) > maxWidth) {
        text.pop_back();
    }
    return text + L"…";
}

void drawFieldLabel(Renderer& renderer, const Rect& rect, std::wstring_view label, std::wstring_view value,
    const Theme& theme)
{
    renderer.text(label, Rect{ rect.left, rect.top, rect.centerX(), rect.top + 16.0f }, TextRole::Small, theme.textDim);
    if (!value.empty()) {
        renderer.text(value, Rect{ rect.centerX(), rect.top, rect.right, rect.top + 16.0f }, TextRole::Small,
            theme.text, TextAlign::Right);
    }
}

void drawCaptureCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card,
    std::vector<UiHit>& hits)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    float top = drawSettingsCardHeader(renderer, card, L"Сбор данных", L"как часто и что именно измеряем", theme);

    renderer.text(L"Интервал опроса", Rect{ inner.left, top, inner.right, top + 16.0f }, TextRole::Small,
        theme.textDim);
    top += 18.0f;
    const float intervalWidth = (inner.width() - 2.0f * 8.0f) / 3.0f;
    for (std::size_t index = 0; index < kIntervals.size(); ++index) {
        const Rect chip{ inner.left + static_cast<float>(index) * (intervalWidth + 8.0f), top,
            inner.left + static_cast<float>(index) * (intervalWidth + 8.0f) + intervalWidth, top + 26.0f };
        drawChip(renderer, chip, wideText(std::to_string(kIntervals[index]) + " мс"),
            model.settings.capture.intervalMs == kIntervals[index], theme);
        hits.push_back(UiHit{ chip, ActionKind::SetInterval, kIntervals[index] });
    }

    top += 36.0f;
    renderer.text(L"Цель кадра", Rect{ inner.left, top, inner.left + 90.0f, top + 16.0f }, TextRole::Small,
        theme.textDim);
    const std::wstring monitor = model.displayWidth > 0
        ? L"монитор " + std::to_wstring(model.displayWidth) + L"×" + std::to_wstring(model.displayHeight)
            + (model.displayRefreshHz > 0 ? L" · " + std::to_wstring(model.displayRefreshHz) + L" Гц" : L"")
        : L"определяем при запуске";
    renderer.text(monitor, Rect{ inner.left + 96.0f, top, inner.right, top + 16.0f }, TextRole::Tiny,
        theme.textFaint);
    top += 18.0f;
    // Пять чипов: «Авто» (герцовка монитора) и четыре частых значения.
    const float targetWidth = (inner.width() - 4.0f * 8.0f) / 5.0f;
    const std::wstring autoLabel = model.displayRefreshHz > 0
        ? L"Авто " + std::to_wstring(model.displayRefreshHz)
        : L"Авто";
    const Rect autoChip{ inner.left, top, inner.left + targetWidth, top + 26.0f };
    drawChip(renderer, autoChip, autoLabel, model.settings.capture.targetAuto, theme, TextRole::Tiny);
    hits.push_back(UiHit{ autoChip, ActionKind::SetTargetHz, 0 });
    for (std::size_t index = 0; index < kTargets.size(); ++index) {
        const std::size_t slot = index + 1;
        const Rect chip{ inner.left + static_cast<float>(slot) * (targetWidth + 8.0f), top,
            inner.left + static_cast<float>(slot) * (targetWidth + 8.0f) + targetWidth, top + 26.0f };
        drawChip(renderer, chip, wideText(std::to_string(kTargets[index]) + " Гц"),
            !model.settings.capture.targetAuto && model.settings.capture.targetHz == kTargets[index], theme,
            TextRole::Tiny);
        hits.push_back(UiHit{ chip, ActionKind::SetTargetHz, kTargets[index] });
    }

    top += 36.0f;
    const std::array<std::array<std::wstring_view, 2>, 3> toggles {
        std::array<std::wstring_view, 2> {
            L"Собирать кадры игры", L"системные события Windows, без внедрения в игру" },
        { L"Останавливать опрос вне игры", L"экономит ресурсы в простое" },
        { L"Показывать диски и сеть", L"скрывает карточку и строки в датчиках" }
    };
    const std::array<bool, 3> states{ model.settings.capture.etwEnabled, model.settings.capture.pauseWhenIdle,
        model.settings.capture.includeDiskNet };
    const std::array<ActionKind, 3> actions{ ActionKind::ToggleEtw, ActionKind::ToggleIdlePause,
        ActionKind::ToggleDiskNet };

    for (std::size_t index = 0; index < toggles.size(); ++index) {
        if (top + 30.0f > inner.bottom) {
            break;
        }
        const Rect row{ inner.left, top, inner.right, top + 30.0f };
        drawSwitchRow(renderer, row, toggles[index][0], toggles[index][1], states[index], theme);
        hits.push_back(UiHit{ row, actions[index], 0 });
        top += 32.0f;
    }
}

void drawAppearanceCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card,
    std::vector<UiHit>& hits)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    float top = drawSettingsCardHeader(renderer, card, L"Внешний вид", L"акцент меняет весь интерфейс и оверлей", theme);

    renderer.text(L"Акцент", Rect{ inner.left, top, inner.right, top + 16.0f }, TextRole::Small, theme.textDim);
    top += 20.0f;

    const float chipWidth = (inner.width() - 4.0f * 8.0f) / 5.0f;
    for (std::size_t index = 0; index < kAccentNames.size(); ++index) {
        const Rect chip{ inner.left + static_cast<float>(index) * (chipWidth + 8.0f), top,
            inner.left + static_cast<float>(index) * (chipWidth + 8.0f) + chipWidth, top + 28.0f };
        const bool active = static_cast<std::size_t>(model.settings.ui.accent) == index;
        renderer.fillRoundRect(chip, 9.0f, active ? theme.accentSoft : theme.panelSoft);
        if (active) {
            renderer.strokeRoundRect(chip, 9.0f, theme.accentGlow, 1.0f);
        }
        renderer.fillCircle(chip.left + 13.0f, chip.centerY(), 4.0f, fromRgba(kAccentRgba[index]));
        renderer.text(kAccentNames[index], Rect{ chip.left + 22.0f, chip.top, chip.right - 4.0f, chip.bottom },
            TextRole::Tiny, active ? theme.accent : theme.textDim);
        hits.push_back(UiHit{ chip, ActionKind::SetAccent, static_cast<int>(index) });
    }

    top += 40.0f;
    renderer.text(L"Плотность интерфейса", Rect{ inner.left, top, inner.right, top + 16.0f }, TextRole::Small,
        theme.textDim);
    renderer.text(L"компактная умещает больше карточек", Rect{ inner.left, top + 16.0f, inner.right, top + 30.0f },
        TextRole::Tiny, theme.textFaint);
    top += 34.0f;
    const Rect normal{ inner.left, top, inner.left + inner.width() * 0.5f - 4.0f, top + 26.0f };
    const Rect compact{ inner.left + inner.width() * 0.5f + 4.0f, top, inner.right, top + 26.0f };
    drawChip(renderer, normal, L"Обычная", !model.settings.ui.compactDensity, theme);
    drawChip(renderer, compact, L"Компактная", model.settings.ui.compactDensity, theme);
    hits.push_back(UiHit{ normal, ActionKind::SetDensity, 0 });
    hits.push_back(UiHit{ compact, ActionKind::SetDensity, 1 });

    top += 38.0f;
    if (top + 30.0f > inner.bottom) {
        return;
    }
    const Rect tray{ inner.left, top, inner.right, top + 30.0f };
    drawSwitchRow(renderer, tray, L"Сворачивать в трей при закрытии", L"опрос продолжается в фоне",
        model.settings.ui.closeToTray, theme);
    hits.push_back(UiHit{ tray, ActionKind::ToggleTray, 0 });
}

void drawPortableCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card,
    std::vector<UiHit>& hits)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    float top = drawSettingsCardHeader(renderer, card, L"Портативность и данные",
        L"один exe, настройки в каталоге пользователя, реестр и сеть не используются", theme);

    const std::array<std::array<std::wstring_view, 2>, 3> rows{ std::array<std::wstring_view, 2>{ L"Каталог данных",
                                                                                               L"%LOCALAPPDATA%\\YawaFpsHub" },
        { L"Файл настроек", L"YawaFpsHub.ini" }, { L"Сетевые запросы", L"нет" } };

    for (const auto& row : rows) {
        if (top + 16.0f > inner.bottom) {
            break;
        }
        drawFieldLabel(renderer, Rect{ inner.left, top, inner.right, top + 16.0f }, row[0], row[1], theme);
        top += 18.0f;
    }

    // Кнопки прижимаем к низу карточки: раньше при низком окне путь и «Сбросить»
    // уезжали за край, и кнопки просто исчезали. Путь показываем только если между
    // строками и кнопками для него осталось место — иначе он печатался поверх строк.
    const float actionsTop = inner.bottom - 28.0f;
    const float pathTop = actionsTop - 18.0f;
    if (pathTop >= top + 2.0f) {
        renderer.text(clipRight(renderer, wideText(model.settingsPath), TextRole::Mono, inner.width() * 0.96f),
            Rect{ inner.left, pathTop, inner.right, pathTop + 16.0f }, TextRole::Mono, theme.textFaint);
    }

    const Rect open{ inner.left, actionsTop, inner.left + 150.0f, actionsTop + 28.0f };
    const Rect reset{ open.right + 8.0f, actionsTop, open.right + 158.0f, actionsTop + 28.0f };
    drawChip(renderer, open, L"Папка настроек", false, theme);
    drawChip(renderer, reset, L"Сбросить", false, theme);
    hits.push_back(UiHit{ open, ActionKind::OpenSettingsFolder, 0 });
    hits.push_back(UiHit{ reset, ActionKind::ResetSettings, 0 });
}

void drawAboutCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card,
    std::vector<UiHit>& hits)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    const std::wstring build
        = wideText(std::string("YawaFpsHub ") + YAWA_VERSION_STRING + " · Windows 10/11 x64");
    float top = drawSettingsCardHeader(renderer, card, L"О программе", build, theme);

    const std::array<std::array<std::wstring, 2>, 4> rows {
        std::array<std::wstring, 2> { L"Собственное потребление",
            wideMetric(MetricId::SelfCpu, model.store.value(MetricId::SelfCpu)) + L" CPU · "
                + wideMetric(MetricId::SelfRam, model.store.value(MetricId::SelfRam)) },
        { L"Потоков", wideNumber(model.store.value(MetricId::SelfThreads), MetricUnit::Count, 0) },
        { L"Активный адаптер",
            model.activeGpu < model.gpuNames.size() ? model.gpuNames[model.activeGpu] : L"не найден" },
        { L"Сбор кадров", model.etwRunning ? L"работает" : L"выключен" }
    };

    for (const auto& row : rows) {
        if (top + 16.0f > inner.bottom) {
            break;
        }
        const float valueLeft = inner.left + inner.width() * 0.46f;
        renderer.text(row[0], Rect{ inner.left, top, valueLeft, top + 16.0f }, TextRole::Small, theme.textDim);
        renderer.text(row[1], Rect{ valueLeft, top, inner.right, top + 16.0f }, TextRole::Small, theme.text,
            TextAlign::Right);
        top += 24.0f;
    }

    if (model.gpuNames.size() > 1 && top + 30.0f <= inner.bottom - 36.0f) {
        const Rect picker{ inner.left, top + 4.0f, inner.left + 190.0f, top + 34.0f };
        drawChip(renderer, picker, L"Следующий адаптер", false, theme);
        hits.push_back(UiHit{ picker, ActionKind::CycleGpu, 0 });
    }
    const Rect changelog{ inner.left, inner.bottom - 30.0f, inner.left + 190.0f, inner.bottom - 2.0f };
    drawChip(renderer, changelog, L"Что нового", false, theme);
    hits.push_back(UiHit{ changelog, ActionKind::ShowChangelog, 0 });
}

void drawHotkeyCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card,
    std::vector<UiHit>& hits)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    float top = drawSettingsCardHeader(renderer, card, L"Горячие клавиши",
        L"нажмите строку и введите клавишу · Esc — отмена · Delete — выключить", theme);

    const std::array<std::wstring_view, 4> names{ L"Оверлей", L"Положение оверлея", L"Показать или скрыть окно",
        L"Сквозной клик по оверлею" };
    const std::array<unsigned int, 4> keys{ model.settings.hotkeys.overlay, model.settings.hotkeys.position,
        model.settings.hotkeys.window, model.settings.hotkeys.clickThrough };

    for (std::size_t index = 0; index < names.size(); ++index) {
        if (top + 28.0f > inner.bottom) {
            break;
        }
        const Rect row{ inner.left, top, inner.right, top + 26.0f };
        const bool editing = model.hotkeyEditing == static_cast<int>(index);
        renderer.fillRoundRect(row, 9.0f, editing ? theme.accentSoft : theme.panelSoft);
        if (editing) {
            renderer.strokeRoundRect(row, 9.0f, theme.accentGlow, 1.0f);
        }

        renderer.text(names[index], Rect{ row.left + 10.0f, row.top, row.right - 120.0f, row.bottom },
            TextRole::Small, editing ? theme.text : theme.textDim);

        const Rect keyChip{ row.right - 108.0f, row.top + 3.0f, row.right - 8.0f, row.bottom - 3.0f };
        renderer.fillRoundRect(keyChip, 7.0f, editing ? theme.accent : theme.background.withAlpha(0.6f));
        renderer.text(editing ? L"нажмите клавишу" : hotkeyName(keys[index]), keyChip, TextRole::Tiny,
            editing ? theme.background : theme.text, TextAlign::Center);

        hits.push_back(UiHit{ row, ActionKind::CaptureHotkey, static_cast<int>(index) });
        top += 30.0f;
    }
}

void drawSourcesCard(
    Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card, std::vector<UiHit>& hits)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    const float top = drawSettingsCardHeader(renderer, card, L"Источники данных",
        L"доступность проверяется при запуске, недоступное честно помечается", theme) + 10.0f;

    struct Source {
        std::wstring_view name;
        std::wstring_view detail;
        bool available;
    };

    const SensorAvailability& availability = model.availability;
    const std::array<Source, 6> sources { Source { L"Кадры игры", L"События Windows, без внедрения в игру",
                                              model.etwRunning },
        { L"Процессор, диски и сеть", L"Системные счётчики Windows", availability.cpu },
        { L"Загрузка видеокарты", L"Выбранная видеокарта", availability.gpuLoad },
        { L"Видеопамять", L"Объём и занятость выбранной видеокарты", availability.gpuMemory },
        { L"Температура видеокарты", L"Если доступна в установленном драйвере", availability.gpuTemperature },
        { L"Температура процессора", L"Может требовать доступ к датчикам", availability.cpuTemperature } };

    // Две колонки по три источника: шесть строк в одну не помещаются на 1040x790.
    const float columnWidth = (inner.width() - 24.0f) / 2.0f;
    for (std::size_t index = 0; index < sources.size(); ++index) {
        const Source& source = sources[index];
        const float left = inner.left + static_cast<float>(index / 3) * (columnWidth + 24.0f);
        const float rowTop = top + static_cast<float>(index % 3) * 28.0f;
        if (rowTop + 30.0f > inner.bottom) {
            break;
        }

        renderer.text(source.name, Rect{ left, rowTop, left + columnWidth, rowTop + 15.0f }, TextRole::Small,
            theme.text);
        renderer.text(source.detail, Rect{ left, rowTop + 14.0f, left + columnWidth - 74.0f, rowTop + 29.0f },
            TextRole::Tiny, theme.textFaint);
        const bool disabled = index == 0 && !model.settings.capture.etwEnabled;
        renderer.text(disabled     ? L"выключено"
                : source.available ? L"работает"
                                   : L"нет данных",
            Rect { left + columnWidth - 70.0f, rowTop, left + columnWidth, rowTop + 15.0f }, TextRole::Tiny,
            source.available ? theme.accent : theme.textFaint, TextAlign::Right);
    }

    const Rect logFolder { inner.left, inner.bottom - 28.0f, inner.left + 150.0f, inner.bottom - 2.0f };
    renderer.fillRoundRect(logFolder, 8.0f, isHovered(model, logFolder) ? theme.accentSoft : theme.panelSoft);
    renderer.text(L"Папка с журналом", logFolder, TextRole::Tiny, theme.text, TextAlign::Center);
    hits.push_back(UiHit { logFolder, ActionKind::OpenSettingsFolder, 0 });
}

} // namespace

void drawSettings(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits)
{
    // Как в прототипе: карточки идут одной колонкой, высоты сняты с демо.
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

    drawClipped(place(385.0f), [&](const Rect& card) { drawCaptureCard(renderer, model, theme, card, hits); });
    drawClipped(place(389.0f), [&](const Rect& card) { drawAppearanceCard(renderer, model, theme, card, hits); });
    drawClipped(
        place(260.0f), [&](const Rect& card) { drawSourcesCard(renderer, model, theme, card, hits); });
    drawClipped(place(262.0f), [&](const Rect& card) { drawHotkeyCard(renderer, model, theme, card, hits); });
    drawClipped(place(316.0f), [&](const Rect& card) { drawPortableCard(renderer, model, theme, card, hits); });
    drawClipped(place(244.0f), [&](const Rect& card) { drawAboutCard(renderer, model, theme, card, hits); });
}

} // namespace yawa

#include "ui/sensors_view.h"
#include "ui/layout.h"

#include "metrics/units.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <string>
#include <vector>

namespace yawa {
namespace {

struct Group {
    std::wstring_view title;
    std::vector<MetricId> ids;
};

constexpr std::array<std::wstring_view, 7> kFilterNames{ L"Все", L"Кадры", L"Процессор", L"Видеокарта", L"Память",
    L"Диски и сеть", L"Система" };

constexpr float kToolbarHeight = 36.0f;
constexpr float kRowHeight = 26.0f;
constexpr float kGroupHeight = 24.0f;

std::vector<Group> makeGroups()
{
    return {
        { L"Кадры", { MetricId::Fps, MetricId::FrameTime, MetricId::Low1Fps, MetricId::Low01Fps, MetricId::AvgFps,
                        MetricId::DroppedFrames } },
        { L"Процессор", { MetricId::CpuLoad, MetricId::CpuTemp, MetricId::CpuClock, MetricId::CpuPower,
                            MetricId::GameThreadLoad } },
        { L"Видеокарта", { MetricId::GpuLoad, MetricId::GpuTemp, MetricId::GpuHotspot, MetricId::GpuClock,
                             MetricId::GpuMemClock, MetricId::GpuPower, MetricId::GpuFan, MetricId::VramUsed,
                             MetricId::VramTotal } },
        { L"Память", { MetricId::RamUsed, MetricId::RamCache, MetricId::RamFree, MetricId::PageFile } },
        { L"Диски и сеть", { MetricId::DiskRead, MetricId::DiskWrite, MetricId::DiskTemp, MetricId::NetDown,
                               MetricId::NetUp } },
        { L"Система", { MetricId::ChipsetTemp, MetricId::SelfCpu, MetricId::SelfRam, MetricId::SelfThreads } },
    };
}

// «Глаз» в строке привязывает датчик к строке оверлея: у каждой строки есть своё поле подписи.
int overlayField(MetricId id)
{
    switch (id) {
    case MetricId::Fps:
        return static_cast<int>(LabelField::Fps);
    case MetricId::CpuLoad:
        return static_cast<int>(LabelField::Cpu);
    case MetricId::GpuLoad:
        return static_cast<int>(LabelField::Gpu);
    case MetricId::RamUsed:
        return static_cast<int>(LabelField::Ram);
    case MetricId::VramUsed:
        return static_cast<int>(LabelField::Vram);
    default:
        return -1;
    }
}

bool valueKnown(MetricId id, const AppModel& model)
{
    if (!model.store.hasValue(id)) {
        return false;
    }

    // Ноль у этих датчиков означает «источника нет», а не реальный ноль.
    switch (id) {
    case MetricId::CpuTemp:
        return model.availability.cpuTemperature && model.store.value(id) > 0.0;
    case MetricId::CpuClock:
        return model.availability.cpuClock && model.store.value(id) > 0.0;
    case MetricId::CpuPower:
    case MetricId::GameThreadLoad:
    case MetricId::GpuHotspot:
    case MetricId::GpuPower:
    case MetricId::GpuFan:
    case MetricId::VramTotal:
    case MetricId::DiskTemp:
    case MetricId::ChipsetTemp:
        return model.store.value(id) > 0.0;
    case MetricId::GpuTemp:
        return model.availability.gpuTemperature && model.store.value(id) > 0.0;
    case MetricId::GpuLoad:
        return model.availability.gpuLoad;
    default:
        return true;
    }
}

std::wstring lowerCopy(std::wstring_view text)
{
    std::wstring result(text);
    for (wchar_t& symbol : result) {
        symbol = static_cast<wchar_t>(std::towlower(symbol));
    }
    return result;
}

bool matchesQuery(MetricId id, std::wstring_view group, const std::wstring& query)
{
    if (query.empty()) {
        return true;
    }
    const std::wstring label = lowerCopy(wideText(metricInfo(id).label));
    return label.find(query) != std::wstring::npos || lowerCopy(group).find(query) != std::wstring::npos;
}

// Цвет пипа как в демо: у каждой группы свой оттенок.
Color pipColor(std::size_t groupIndex, const Theme& theme)
{
    switch (groupIndex) {
    case 0:
        return theme.accent;
    case 1:
        return theme.cpu;
    case 2:
        return theme.gpu;
    case 3:
        return theme.ram;
    case 4:
        return theme.disk;
    case 5:
        return theme.warn;
    default:
        return theme.textDim;
    }
}

void drawSearchBox(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& rect,
    std::vector<UiHit>& hits)
{
    const bool active = model.sensorSearchActive;
    const bool hover = isHovered(model, rect);
    renderer.fillRoundRect(rect, 10.0f, active || hover ? theme.panelSoft : theme.panel);
    renderer.strokeRoundRect(rect, 10.0f, active ? theme.accentGlow : theme.line, 1.0f);

    const float iconX = rect.left + 16.0f;
    const float iconY = rect.centerY();
    renderer.strokeCircle(iconX, iconY - 1.0f, 5.0f, active ? theme.accent : theme.textFaint, 1.4f);
    renderer.drawLine(iconX + 4.0f, iconY + 3.0f, iconX + 8.0f, iconY + 7.0f,
        active ? theme.accent : theme.textFaint, 1.4f);

    const std::wstring query = wideText(model.sensorQuery);
    const Rect textRect{ rect.left + 32.0f, rect.top, rect.right - 12.0f, rect.bottom };
    if (query.empty() && !active) {
        renderer.text(L"Найти датчик: температура, частота, VRAM…", textRect, TextRole::Small, theme.textFaint);
    } else {
        renderer.text(active ? query + L"|" : query, textRect, TextRole::Small, active ? theme.text : theme.textDim);
    }

    hits.push_back(UiHit{ rect, ActionKind::FocusSensorSearch, 0 });
}

} // namespace

void drawSensors(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits)
{
    // --- Панель инструментов: поиск, фильтры, «только в оверлее».
    const float searchWidth = 288.0f;
    const Rect search{ area.left, area.top, area.left + searchWidth, area.top + kToolbarHeight };
    drawSearchBox(renderer, model, theme, search, hits);

    const Rect overlaySwitch{ area.right - 170.0f, area.top, area.right, area.top + kToolbarHeight };
    const bool overlayOn = model.overlayOnly;
    const bool switchHover = isHovered(model, overlaySwitch);
    renderer.text(L"Только в оверлее", Rect{ overlaySwitch.left, overlaySwitch.top, overlaySwitch.right - 44.0f,
                                         overlaySwitch.bottom },
        TextRole::Tiny, overlayOn || switchHover ? theme.text : theme.textDim, TextAlign::Right);
    const Rect track{ overlaySwitch.right - 38.0f, overlaySwitch.centerY() - 9.0f, overlaySwitch.right,
        overlaySwitch.centerY() + 9.0f };
    renderer.fillRoundRect(track, 9.0f, overlayOn ? theme.accentSoft : theme.panelSoft);
    if (overlayOn) {
        renderer.strokeRoundRect(track, 9.0f, theme.accentGlow, 1.0f);
    }
    renderer.fillCircle(overlayOn ? track.right - 9.0f : track.left + 9.0f, track.centerY(), 6.0f,
        overlayOn ? theme.accent : theme.textFaint);
    hits.push_back(UiHit{ overlaySwitch, ActionKind::ToggleOverlayOnly, 0 });

    float filterLeft = search.right + 10.0f;
    for (std::size_t index = 0; index < kFilterNames.size(); ++index) {
        const float width = renderer.measureText(kFilterNames[index], TextRole::Small) + 24.0f;
        if (filterLeft + width > overlaySwitch.left - 8.0f) {
            break;
        }
        const Rect chip{ filterLeft, area.top, filterLeft + width, area.top + kToolbarHeight - 2.0f };
        const bool active = index == 0 ? model.sensorFilter < 0 : model.sensorFilter == static_cast<int>(index) - 1;
        drawChip(renderer, chip, kFilterNames[index], active, theme, TextRole::Small, isHovered(model, chip));
        hits.push_back(UiHit{ chip, ActionKind::SetSensorFilter, index == 0 ? -1 : static_cast<int>(index) - 1 });
        filterLeft = chip.right + 6.0f;
    }

    // --- Таблица: одна колонка на весь экран, как в демо.
    const Rect card{ area.left, area.top + kToolbarHeight + 10.0f, area.right, area.bottom };
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);

    // Сетка колонок как в демо: имя, значение, мин, среднее, макс, график, «глаз».
    const float width = inner.width();
    const float nameX = inner.left + 14.0f;
    const float valueRight = inner.left + width * 0.50f;
    const float minRight = inner.left + width * 0.62f;
    const float avgRight = inner.left + width * 0.73f;
    const float maxRight = inner.left + width * 0.84f;
    const float sparkLeft = inner.left + width * 0.85f;
    // Справа оставляем жёлоб под полосу прокрутки, иначе она наезжает на «глаз».
    const float sparkRight = inner.right - 46.0f;

    renderer.text(L"Датчик", Rect{ nameX, inner.top, inner.left + width * 0.3f, inner.top + 14.0f }, TextRole::Tiny,
        theme.textFaint);
    const std::array<std::pair<std::wstring_view, float>, 4> headers{
        std::pair<std::wstring_view, float>{ L"Значение", valueRight }, { L"Мин", minRight },
        { L"Среднее", avgRight }, { L"Макс", maxRight } };
    for (const auto& [title, right] : headers) {
        renderer.text(title, Rect{ right - 100.0f, inner.top, right, inner.top + 14.0f }, TextRole::Tiny,
            theme.textFaint, TextAlign::Right);
    }
    renderer.text(L"60 с", Rect{ sparkLeft, inner.top, sparkRight, inner.top + 14.0f }, TextRole::Tiny,
        theme.textFaint, TextAlign::Right);
    renderer.drawLine(inner.left, inner.top + 20.0f, inner.right, inner.top + 20.0f, theme.line, 1.0f);

    const std::vector<Group> groups = makeGroups();
    const std::wstring query = lowerCopy(wideText(model.sensorQuery));
    const int historySamples = std::max(60000 / std::max(model.settings.capture.intervalMs, 100), 2);

    struct Entry {
        std::size_t group;
        MetricId id;
    };
    std::vector<Entry> entries;
    std::vector<std::size_t> groupStarts;
    std::vector<std::size_t> groupIndexes;

    for (std::size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        const Group& group = groups[groupIndex];
        if (model.sensorFilter >= 0 && static_cast<std::size_t>(model.sensorFilter) != groupIndex) {
            continue;
        }
        bool started = false;
        for (MetricId id : group.ids) {
            if (!matchesQuery(id, group.title, query)) {
                continue;
            }
            if (model.overlayOnly && overlayField(id) < 0) {
                continue;
            }
            if (!started) {
                started = true;
                groupStarts.push_back(entries.size());
                groupIndexes.push_back(groupIndex);
            }
            entries.push_back(Entry{ groupIndex, id });
        }
    }

    // Высота содержимого: заголовок группы на каждую непустую группу.
    const float contentHeight = static_cast<float>(entries.size()) * kRowHeight
        + static_cast<float>(groupStarts.size()) * kGroupHeight;

    // Низ таблицы освобождает строку под сноску: раньше она висела под карточкой.
    const Rect viewport{ inner.left, inner.top + 26.0f, inner.right, inner.bottom - 24.0f };
    const float maxScroll = std::max(0.0f, contentHeight - viewport.height());
    const float offset = std::clamp(static_cast<float>(model.sensorScroll), 0.0f, maxScroll);

    renderer.pushClip(viewport);
    float top = viewport.top - offset;

    if (entries.empty()) {
        renderer.text(query.empty() ? L"Датчиков нет: проверьте фильтр" : L"Ничего не найдено. Измените запрос.",
            viewport, TextRole::Small, theme.textFaint, TextAlign::Center);
    }

    std::size_t nextGroupStart = 0;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (nextGroupStart < groupStarts.size() && groupStarts[nextGroupStart] == index) {
            if (top + kGroupHeight > viewport.top) {
                renderer.text(groups[groupIndexes[nextGroupStart]].title,
                    Rect{ inner.left + 14.0f, top, inner.right, top + kGroupHeight - 4.0f }, TextRole::Small,
                    theme.accent);
            }
            top += kGroupHeight;
            ++nextGroupStart;
        }

        const MetricId id = entries[index].id;
        const float rowTop = top;

        if (rowTop + kRowHeight > viewport.top && rowTop < viewport.bottom) {
            const Rect row{ inner.left, rowTop, inner.right, rowTop + kRowHeight };
            const bool hover = isHovered(model, row);
            if (hover) {
                // Подсветка строки была почти не виднее фона карточки — теперь это заметная плашка.
                renderer.fillRoundRect(row, 6.0f, theme.accentSoft);
                renderer.fillRoundRect(Rect{ inner.left + 2.0f, rowTop + 4.0f, inner.left + 4.0f,
                                               rowTop + kRowHeight - 4.0f },
                    1.0f, theme.accent);
            }

            const MetricInfo& info = metricInfo(id);
            const MetricSlot& slot = model.store.slot(id);
            const bool known = valueKnown(id, model);

            // Пип группы и название.
            renderer.fillCircle(nameX + 3.0f, rowTop + kRowHeight * 0.5f, 3.0f, pipColor(entries[index].group, theme));
            renderer.text(wideText(info.label), Rect{ nameX + 14.0f, rowTop, valueRight - 120.0f,
                                                     rowTop + kRowHeight },
                TextRole::Small, known ? theme.textDim : theme.textFaint);

            // Значение крупнее остальных, единица — мелким шрифтом через пробел.
            const std::wstring value = known ? wideText(formatValue(slot.stats.current, info.decimals)) : L"—";
            const std::wstring unit = known ? wideText(unitSuffix(info.unit)) : std::wstring();
            const float unitWidth = unit.empty() ? 0.0f : renderer.measureText(unit, TextRole::Tiny) + 3.0f;
            renderer.text(value, Rect{ valueRight - 100.0f - unitWidth, rowTop, valueRight - unitWidth,
                                   rowTop + kRowHeight },
                TextRole::Small, known ? theme.text : theme.textFaint, TextAlign::Right);
            if (!unit.empty()) {
                renderer.text(unit, Rect{ valueRight - unitWidth, rowTop + kRowHeight * 0.5f, valueRight,
                                       rowTop + kRowHeight },
                    TextRole::Tiny, theme.textFaint, TextAlign::Right);
            }

            const std::array<double, 3> values{ slot.stats.minimum, slot.stats.average, slot.stats.maximum };
            const std::array<float, 3> rights{ minRight, avgRight, maxRight };
            for (std::size_t stat = 0; stat < values.size(); ++stat) {
                renderer.text(known ? wideText(formatValue(values[stat], info.decimals)) : L"—",
                    Rect{ rights[stat] - 80.0f, rowTop, rights[stat], rowTop + kRowHeight }, TextRole::Tiny,
                    theme.textFaint, TextAlign::Right);
            }

            const Rect spark{ sparkLeft, rowTop + 6.0f, sparkRight, rowTop + kRowHeight - 6.0f };
            std::array<float, kHistoryCapacity> history{};
            const std::size_t count = model.store.copyHistory(id, history.data(), history.size());
            if (known && count >= 2 && spark.width() > 16.0f) {
                const std::size_t visible = std::min(count, static_cast<std::size_t>(historySamples));
                float maximum = 0.01f;
                for (std::size_t sample = count - visible; sample < count; ++sample) {
                    maximum = std::max(maximum, history[sample]);
                }
                renderer.drawSeries(ChartSeries{ history.data() + (count - visible), visible,
                                        pipColor(entries[index].group, theme), false },
                    spark, 0.0f, maximum, 1.3f);
            }

            // «Глаз»: датчик показан в оверлее. Только у строк, которые туда попадают.
            const int field = overlayField(id);
            if (field >= 0) {
                const Rect eye{ inner.right - 40.0f, rowTop + 3.0f, inner.right - 14.0f, rowTop + kRowHeight - 3.0f };
                const bool pinned = model.settings.overlay.visible[static_cast<std::size_t>(field)];
                const bool eyeHover = isHovered(model, eye);
                if (pinned || eyeHover) {
                    renderer.fillRoundRect(eye, 7.0f, pinned ? theme.accentSoft : theme.panelSoft);
                }
                // Иконка глаза: овал и зрачок; у выключенного — перечёркнут.
                const float centerX = eye.centerX();
                const float centerY = eye.centerY();
                renderer.strokeCircle(centerX, centerY, 4.5f, pinned ? theme.accent : theme.textFaint, 1.2f);
                renderer.fillCircle(centerX, centerY, 1.8f, pinned ? theme.accent : theme.textFaint);
                if (!pinned) {
                    renderer.drawLine(centerX - 5.0f, centerY + 5.0f, centerX + 5.0f, centerY - 5.0f, theme.textFaint,
                        1.1f);
                }
                hits.push_back(UiHit{ eye, ActionKind::ToggleMetric, field });
            }

            hits.push_back(UiHit{ row, ActionKind::None, static_cast<int>(index) });
        }

        top += kRowHeight;
    }
    renderer.popClip();

    // Полоса прокрутки: тонкая, только когда содержимое не помещается.
    if (contentHeight > viewport.height() && viewport.height() > 0.0f && maxScroll > 0.0f) {
        const float thumbHeight = std::max(24.0f, viewport.height() * (viewport.height() / contentHeight));
        const float thumbTop = viewport.top + (viewport.height() - thumbHeight) * (offset / maxScroll);
        renderer.fillRoundRect(Rect{ inner.right - 9.0f, viewport.top, inner.right - 5.0f, viewport.bottom }, 2.0f,
            theme.line.withAlpha(0.5f));
        renderer.fillRoundRect(Rect{ inner.right - 9.0f, thumbTop, inner.right - 5.0f, thumbTop + thumbHeight }, 2.0f,
            theme.textFaint.withAlpha(0.7f));
    }

    renderer.text(L"ETW для кадров, PDH для счётчиков диспетчера задач, DXGI для видеопамяти, NVML для температур"
                  L" NVIDIA, ACPI-термозона для процессора. «—» — источник не отвечает на этом железе.",
        Rect{ inner.left, inner.bottom - 18.0f, inner.right - 14.0f, inner.bottom }, TextRole::Tiny,
        theme.textFaint);
}

} // namespace yawa

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

constexpr std::array<MetricId, 5> kOverlayIds{ MetricId::Fps, MetricId::CpuLoad, MetricId::GpuLoad,
    MetricId::RamUsed, MetricId::VramUsed };

constexpr float kSearchWidth = 288.0f;
constexpr float kToolbarHeight = 38.0f;
constexpr float kRowHeight = 24.0f;
constexpr float kGroupHeight = 22.0f;

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

bool inOverlay(MetricId id)
{
    return std::find(kOverlayIds.begin(), kOverlayIds.end(), id) != kOverlayIds.end();
}

void drawSearchBox(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& rect,
    std::vector<UiHit>& hits)
{
    const bool active = model.sensorSearchActive;
    renderer.fillRoundRect(rect, 10.0f, active ? theme.panelSoft : theme.panel);
    renderer.strokeRoundRect(rect, 10.0f, active ? theme.accentGlow : theme.line, 1.0f);

    // Лупа: круг и ручка.
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
        renderer.text(active ? query + L"|" : query, textRect, TextRole::Small,
            active ? theme.text : theme.textDim);
    }

    hits.push_back(UiHit{ rect, ActionKind::FocusSensorSearch, 0 });
}

void drawColumnHeader(Renderer& renderer, const Rect& inner, const Theme& theme)
{
    renderer.text(L"Датчик", Rect{ inner.left + 8.0f, inner.top, inner.left + 200.0f, inner.top + 16.0f },
        TextRole::Tiny, theme.textFaint);

    const struct {
        const wchar_t* title;
        float left;
        float right;
    } columns[]{ { L"Значение", 0.40f, 0.58f }, { L"Мин", 0.58f, 0.68f }, { L"Среднее", 0.68f, 0.80f },
        { L"Макс", 0.80f, 0.90f } };

    for (const auto& column : columns) {
        const float left = inner.left + inner.width() * column.left;
        const float right = inner.left + inner.width() * column.right;
        renderer.text(column.title, Rect{ left, inner.top, right - 8.0f, inner.top + 16.0f }, TextRole::Tiny,
            theme.textFaint, TextAlign::Right);
    }
    renderer.text(L"60 с", Rect{ inner.left + inner.width() * 0.90f, inner.top, inner.right - 8.0f,
                              inner.top + 16.0f },
        TextRole::Tiny, theme.textFaint, TextAlign::Right);
}

void drawRow(Renderer& renderer, MetricId id, const AppModel& model, const Theme& theme, const Rect& inner,
    float top, int historySamples)
{
    const MetricInfo& info = metricInfo(id);
    const MetricSlot& slot = model.store.slot(id);
    const bool known = valueKnown(id, model);
    const float width = inner.width();

    renderer.text(wideText(info.label), Rect{ inner.left + 8.0f, top, inner.left + width * 0.38f, top + kRowHeight },
        TextRole::Small, known ? theme.textDim : theme.textFaint);

    const auto column = [&](float from, float to) {
        return Rect{ inner.left + width * from, top, inner.left + width * to - 8.0f, top + kRowHeight };
    };

    renderer.text(known ? wideMetric(id, slot.stats.current) : L"—", column(0.40f, 0.58f), TextRole::Small,
        known ? theme.text : theme.textFaint, TextAlign::Right);

    const std::array<double, 3> values{ slot.stats.minimum, slot.stats.average, slot.stats.maximum };
    const std::array<std::array<float, 2>, 3> ranges{ std::array<float, 2>{ 0.58f, 0.68f },
        { 0.68f, 0.80f }, { 0.80f, 0.90f } };
    for (std::size_t index = 0; index < values.size(); ++index) {
        renderer.text(known ? wideText(formatValue(values[index], info.decimals)) : L"—",
            column(ranges[index][0], ranges[index][1]), TextRole::Tiny,
            known ? theme.textDim : theme.textFaint, TextAlign::Right);
    }

    const Rect spark{ inner.left + width * 0.90f, top + 6.0f, inner.right - 10.0f, top + kRowHeight - 6.0f };
    std::array<float, kHistoryCapacity> history{};
    const std::size_t count = model.store.copyHistory(id, history.data(), history.size());
    if (known && count >= 2 && spark.width() > 16.0f) {
        // Рисуем только последние historySamples: это и есть «60 с» в заголовке колонки.
        const std::size_t visible = std::min(count, static_cast<std::size_t>(std::max(historySamples, 1)));
        float maximum = 0.01f;
        for (std::size_t index = count - visible; index < count; ++index) {
            maximum = std::max(maximum, history[index]);
        }
        renderer.drawSeries(ChartSeries{ history.data() + (count - visible), visible, theme.accent.withAlpha(0.8f),
                                false },
            spark, 0.0f, maximum, 1.3f);
    }
}

} // namespace

void drawSensors(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits)
{
    // --- Панель инструментов: поиск, фильтры, «только в оверлее».
    const Rect search{ area.left, area.top, area.left + kSearchWidth, area.top + kToolbarHeight };
    drawSearchBox(renderer, model, theme, search, hits);

    const Rect overlaySwitch{ area.right - 176.0f, area.top, area.right, area.top + kToolbarHeight };
    const bool overlayOn = model.overlayOnly;
    renderer.text(L"Только в оверлее", Rect{ overlaySwitch.left, overlaySwitch.top, overlaySwitch.right - 44.0f,
                                         overlaySwitch.bottom },
        TextRole::Tiny, overlayOn ? theme.text : theme.textDim, TextAlign::Right);
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
        const Rect chip{ filterLeft, area.top, filterLeft + width, area.top + kToolbarHeight - 4.0f };
        const bool active = index == 0 ? model.sensorFilter < 0 : model.sensorFilter == static_cast<int>(index) - 1;
        drawChip(renderer, chip, kFilterNames[index], active, theme);
        hits.push_back(UiHit{ chip, ActionKind::SetSensorFilter, index == 0 ? -1 : static_cast<int>(index) - 1 });
        filterLeft = chip.right + 6.0f;
    }

    // --- Таблица: один свиток, как в прототипе, с прокруткой колесом.
    const Rect card{ area.left, area.top + kToolbarHeight + 10.0f, area.right, area.bottom - 30.0f };
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);

    drawColumnHeader(renderer, inner, theme);
    renderer.drawLine(inner.left, inner.top + 20.0f, inner.right, inner.top + 20.0f, theme.line, 1.0f);

    const std::vector<Group> groups = makeGroups();
    const std::wstring query = lowerCopy(wideText(model.sensorQuery));
    const int historySamples = std::max(60000 / std::max(model.settings.capture.intervalMs, 100), 2);

    const Rect viewport{ inner.left, inner.top + 26.0f, inner.right, inner.bottom };
    const float contentOffset = static_cast<float>(model.sensorScroll);

    float contentHeight = 0.0f;
    std::size_t visibleRows = 0;
    for (std::size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        const Group& group = groups[groupIndex];
        if (model.sensorFilter >= 0 && static_cast<std::size_t>(model.sensorFilter) != groupIndex) {
            continue;
        }
        std::size_t count = 0;
        for (MetricId id : group.ids) {
            if (!matchesQuery(id, group.title, query) || (model.overlayOnly && !inOverlay(id))) {
                continue;
            }
            ++count;
        }
        if (count == 0) {
            continue;
        }
        contentHeight += kGroupHeight + static_cast<float>(count) * kRowHeight;
        visibleRows += count;
    }

    const float maxScroll = std::max(0.0f, contentHeight - viewport.height());
    const float offset = std::clamp(contentOffset, 0.0f, maxScroll);

    renderer.pushClip(viewport);
    float top = viewport.top - offset;

    if (visibleRows == 0) {
        renderer.text(query.empty() ? L"Датчиков нет: проверьте фильтр" : L"Ничего не найдено. Измените запрос.",
            viewport, TextRole::Small, theme.textFaint, TextAlign::Center);
    } else {
        for (std::size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
            const Group& group = groups[groupIndex];
            if (model.sensorFilter >= 0 && static_cast<std::size_t>(model.sensorFilter) != groupIndex) {
                continue;
            }

            std::vector<MetricId> shown;
            for (MetricId id : group.ids) {
                if (!matchesQuery(id, group.title, query) || (model.overlayOnly && !inOverlay(id))) {
                    continue;
                }
                shown.push_back(id);
            }
            if (shown.empty()) {
                continue;
            }

            if (top + kGroupHeight >= viewport.top) {
                renderer.text(group.title, Rect{ inner.left + 8.0f, top, inner.right, top + kGroupHeight - 4.0f },
                    TextRole::Small, theme.accent);
            }
            top += kGroupHeight;

            for (MetricId id : shown) {
                if (top + kRowHeight > viewport.top && top < viewport.bottom) {
                    drawRow(renderer, id, model, theme, inner, top, historySamples);
                }
                top += kRowHeight;
            }
        }
    }
    renderer.popClip();

    // Полоса прокрутки: тонкая, только когда содержимое не помещается.
    const float viewportHeight = viewport.height();
    if (contentHeight > viewportHeight && viewportHeight > 0.0f) {
        const float ratio = viewportHeight / contentHeight;
        const float thumbHeight = std::max(24.0f, viewportHeight * ratio);
        const float thumbTop = viewport.top + (viewportHeight - thumbHeight) * (maxScroll > 0.0f ? offset / maxScroll : 0.0f);
        const Rect thumb{ inner.right - 6.0f, thumbTop, inner.right - 2.0f, thumbTop + thumbHeight };
        renderer.fillRoundRect(Rect{ inner.right - 6.0f, viewport.top, inner.right - 2.0f, viewport.bottom }, 2.0f,
            theme.line.withAlpha(0.5f));
        renderer.fillRoundRect(thumb, 2.0f, theme.textFaint.withAlpha(0.7f));
    }

    renderer.text(L"ETW для кадров, PDH для счётчиков диспетчера задач, DXGI для видеопамяти, NVML для температур"
                  L" NVIDIA. «—» — датчик недоступен без драйвера.",
        Rect{ area.left, area.bottom - 24.0f, area.right, area.bottom }, TextRole::Tiny, theme.textFaint);
}

} // namespace yawa

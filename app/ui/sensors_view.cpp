#include "ui/sensors_view.h"
#include "ui/layout.h"

#include "metrics/units.h"

#include <algorithm>
#include <array>
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

// Три группы в левой колонке и три в правой: так все датчики помещаются на экран
// без прокрутки, которую прототип делает средствами браузера.
std::array<std::vector<Group>, 2> makeColumns()
{
    return { std::vector<Group>{
                 { L"Кадры", { MetricId::Fps, MetricId::FrameTime, MetricId::Low1Fps, MetricId::Low01Fps,
                                 MetricId::AvgFps, MetricId::DroppedFrames } },
                 { L"Процессор", { MetricId::CpuLoad, MetricId::CpuTemp, MetricId::CpuClock, MetricId::CpuPower,
                                     MetricId::GameThreadLoad } },
                 { L"Видеокарта", { MetricId::GpuLoad, MetricId::GpuTemp, MetricId::GpuHotspot, MetricId::GpuClock,
                                      MetricId::GpuMemClock, MetricId::GpuPower, MetricId::GpuFan, MetricId::VramUsed,
                                      MetricId::VramTotal } },
             },
        std::vector<Group>{
            { L"Память", { MetricId::RamUsed, MetricId::RamCache, MetricId::RamFree, MetricId::PageFile } },
            { L"Диски и сеть", { MetricId::DiskRead, MetricId::DiskWrite, MetricId::DiskTemp, MetricId::NetDown,
                                   MetricId::NetUp } },
            { L"Система", { MetricId::ChipsetTemp, MetricId::SelfCpu, MetricId::SelfRam, MetricId::SelfThreads } },
        } };
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

void drawColumnHeader(Renderer& renderer, const Rect& inner, const Theme& theme)
{
    renderer.text(L"Датчик", Rect{ inner.left, inner.top, inner.right, inner.top + 14.0f }, TextRole::Tiny,
        theme.textFaint);

    // Значения выровнены по правому краю колонок, заголовки — тоже.
    const struct {
        const wchar_t* title;
        float right;
    } columns[]{ { L"Значение", 0.58f }, { L"Мин", 0.70f }, { L"Среднее", 0.83f }, { L"Макс", 0.95f } };

    for (const auto& column : columns) {
        const float right = inner.left + inner.width() * column.right;
        renderer.text(column.title, Rect{ right - 80.0f, inner.top, right, inner.top + 14.0f }, TextRole::Tiny,
            theme.textFaint, TextAlign::Right);
    }
}

float drawRow(Renderer& renderer, MetricId id, const AppModel& model, const Theme& theme, const Rect& inner,
    float top)
{
    const MetricInfo& info = metricInfo(id);
    const MetricSlot& slot = model.store.slot(id);
    const bool known = valueKnown(id, model);
    const float rowHeight = 22.0f;
    const float width = inner.width();

    renderer.text(wideText(info.label), Rect{ inner.left, top, inner.left + width * 0.40f, top + rowHeight },
        TextRole::Small, theme.textDim);
    renderer.text(known ? wideMetric(id, slot.stats.current) : L"—",
        Rect{ inner.left + width * 0.44f, top, inner.left + width * 0.58f, top + rowHeight }, TextRole::Small,
        known ? theme.text : theme.textFaint, TextAlign::Right);

    const std::array<double, 3> values{ slot.stats.minimum, slot.stats.average, slot.stats.maximum };
    const std::array<float, 3> rights{ 0.70f, 0.83f, 0.95f };
    for (std::size_t index = 0; index < values.size(); ++index) {
        const float right = inner.left + width * rights[index];
        renderer.text(known ? wideNumber(values[index], info.unit, info.decimals) : L"—",
            Rect{ right - 70.0f, top, right, top + rowHeight }, TextRole::Tiny,
            known ? theme.textDim : theme.textFaint, TextAlign::Right);
    }

    const Rect spark{ inner.left + width * 0.57f, top + 6.0f, inner.right, top + rowHeight - 6.0f };
    std::array<float, kHistoryCapacity> history{};
    const std::size_t count = model.store.copyHistory(id, history.data(), history.size());
    if (known && count >= 2 && spark.width() > 20.0f) {
        float maximum = 0.01f;
        for (std::size_t index = 0; index < count; ++index) {
            maximum = std::max(maximum, history[index]);
        }
        renderer.drawSeries(ChartSeries{ history.data(), count, theme.accent.withAlpha(0.75f), false }, spark, 0.0f,
            maximum, 1.2f);
    }

    return top + rowHeight;
}

void drawColumn(Renderer& renderer, const std::vector<Group>& groups, const AppModel& model, const Theme& theme,
    const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);

    drawColumnHeader(renderer, inner, theme);
    renderer.drawLine(inner.left, inner.top + 18.0f, inner.right, inner.top + 18.0f, theme.line, 1.0f);

    float top = inner.top + 26.0f;
    for (std::size_t index = 0; index < groups.size(); ++index) {
        if (top + 18.0f > inner.bottom) {
            return;
        }
        renderer.text(groups[index].title, Rect{ inner.left, top, inner.right, top + 16.0f }, TextRole::Small,
            theme.accent);
        top += 20.0f;

        for (MetricId id : groups[index].ids) {
            if (top + 22.0f > inner.bottom) {
                return;
            }
            top = drawRow(renderer, id, model, theme, inner, top);
        }
        top += 6.0f;
    }
}

} // namespace

void drawSensors(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits)
{
    float filterLeft = area.left;
    for (std::size_t index = 0; index < kFilterNames.size(); ++index) {
        const float width = renderer.measureText(kFilterNames[index], TextRole::Small) + 26.0f;
        const Rect chip{ filterLeft, area.top, filterLeft + width, area.top + 28.0f };
        const bool active = index == 0 ? model.sensorFilter < 0 : model.sensorFilter == static_cast<int>(index) - 1;
        drawChip(renderer, chip, kFilterNames[index], active, theme);
        hits.push_back(UiHit{ chip, ActionKind::SetSensorFilter, index == 0 ? -1 : static_cast<int>(index) - 1 });
        filterLeft = chip.right + 8.0f;
    }

    renderer.text(L"фильтр по группам датчиков", Rect{ area.left, area.top + 34.0f, area.right, area.top + 50.0f },
        TextRole::Tiny, theme.textFaint);

    const float cardTop = area.top + 56.0f;
    const float cardBottom = area.bottom - 30.0f;
    const std::array<std::vector<Group>, 2> columns = makeColumns();

    if (model.sensorFilter < 0) {
        const float columnWidth = (area.width() - kGap) / 2.0f;
        drawColumn(renderer, columns[0], model, theme, Rect{ area.left, cardTop, area.left + columnWidth, cardBottom });
        drawColumn(renderer, columns[1], model, theme,
            Rect{ area.left + columnWidth + kGap, cardTop, area.right, cardBottom });
    } else {
        const std::size_t wanted = static_cast<std::size_t>(model.sensorFilter);
        const std::vector<Group>& side = wanted < 3 ? columns[0] : columns[1];
        const std::size_t index = wanted < 3 ? wanted : wanted - 3;
        if (index < side.size()) {
            drawColumn(renderer, std::vector<Group>{ side[index] }, model, theme,
                Rect{ area.left, cardTop, area.right, cardBottom });
        }
    }

    renderer.text(L"Источники: PDH для счётчиков GPU, NtQuerySystemInformation для CPU и памяти, DXGI для видеопамяти,"
                  L" NVML для температур NVIDIA. «—» — датчик недоступен без драйвера.",
        Rect{ area.left, area.bottom - 24.0f, area.right, area.bottom }, TextRole::Tiny, theme.textFaint);
}

} // namespace yawa

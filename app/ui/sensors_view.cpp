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
    case MetricId::CpuClock:
    case MetricId::CpuPower:
    case MetricId::GpuHotspot:
    case MetricId::GpuPower:
    case MetricId::GpuFan:
    case MetricId::VramTotal:
    case MetricId::DiskTemp:
    case MetricId::ChipsetTemp:
    case MetricId::GameThreadLoad:
        return model.store.value(id) > 0.0;
    default:
        return true;
    }
}

void drawColumnHeader(Renderer& renderer, const Rect& row, const Rect& card, const Theme& theme)
{
    const std::array<std::wstring_view, 5> titles{ L"Датчик", L"Значение", L"Мин", L"Среднее", L"Макс" };
    const float available = row.width();
    const std::array<float, 5> starts{ 0.0f, 0.42f, 0.60f, 0.72f, 0.84f };

    for (std::size_t index = 0; index < titles.size(); ++index) {
        const float left = card.left + available * starts[index];
        renderer.text(titles[index], Rect{ left, row.top, left + available * 0.16f + 40.0f, row.bottom },
            TextRole::Tiny, theme.textFaint);
    }
    renderer.text(L"История", Rect{ card.right - 120.0f, row.top, card.right, row.bottom }, TextRole::Tiny,
        theme.textFaint);
}

float drawRow(Renderer& renderer, MetricId id, const AppModel& model, const Theme& theme, const Rect& card,
    float top)
{
    const MetricInfo& info = metricInfo(id);
    const MetricSlot& slot = model.store.slot(id);
    const bool known = valueKnown(id, model);
    const float rowHeight = 26.0f;
    const float width = card.width();

    renderer.text(wideText(info.label), Rect{ card.left, top, card.left + width * 0.40f, top + rowHeight },
        TextRole::Small, theme.textDim);
    renderer.text(known ? wideMetric(id, slot.stats.current) : L"—",
        Rect{ card.left + width * 0.42f, top, card.left + width * 0.58f, top + rowHeight }, TextRole::Body,
        known ? theme.text : theme.textFaint, TextAlign::Right);

    const std::array<double, 3> values{ slot.stats.minimum, slot.stats.average, slot.stats.maximum };
    const std::array<float, 3> starts{ 0.60f, 0.72f, 0.84f };
    for (std::size_t index = 0; index < values.size(); ++index) {
        const float left = card.left + width * starts[index];
        renderer.text(known ? wideNumber(values[index], info.unit, info.decimals) : L"—",
            Rect{ left, top, left + width * 0.11f, top + rowHeight }, TextRole::Tiny,
            known ? theme.textDim : theme.textFaint, TextAlign::Right);
    }

    const Rect spark{ card.right - 112.0f, top + 5.0f, card.right, top + rowHeight - 5.0f };
    std::array<float, kHistoryCapacity> history{};
    const std::size_t count = model.store.copyHistory(id, history.data(), history.size());
    if (known && count >= 2) {
        float maximum = 0.01f;
        for (std::size_t index = 0; index < count; ++index) {
            maximum = std::max(maximum, history[index]);
        }
        renderer.drawSeries(ChartSeries{ history.data(), count, theme.accent.withAlpha(0.85f), false }, spark, 0.0f,
            maximum, 1.2f);
    }

    return top + rowHeight;
}

} // namespace

void drawSensors(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits)
{
    const std::vector<Group> groups = makeGroups();

    // Панель фильтров: те же группы, что в прототипе, плюс «Все».
    float filterLeft = area.left;
    for (std::size_t index = 0; index < kFilterNames.size(); ++index) {
        const float width = renderer.measureText(kFilterNames[index], TextRole::Small) + 26.0f;
        const Rect chip{ filterLeft, area.top, filterLeft + width, area.top + 30.0f };
        const bool active = index == 0 ? model.sensorFilter < 0
                                       : model.sensorFilter == static_cast<int>(index) - 1;
        drawChip(renderer, chip, kFilterNames[index], active, theme);
        hits.push_back(UiHit{ chip, ActionKind::SetSensorFilter, index == 0 ? -1 : static_cast<int>(index) - 1 });
        filterLeft = chip.right + 8.0f;
    }

    renderer.text(L"фильтр по группам датчиков", Rect{ area.left, area.top + 38.0f, area.right, area.top + 54.0f },
        TextRole::Tiny, theme.textFaint);

    const Rect card{ area.left, area.top + 60.0f, area.right, area.bottom - 34.0f };
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);

    drawColumnHeader(renderer, Rect{ inner.left, inner.top, inner.right, inner.top + 16.0f }, inner, theme);
    renderer.drawLine(inner.left, inner.top + 22.0f, inner.right, inner.top + 22.0f, theme.line, 1.0f);

    float top = inner.top + 30.0f;
    for (std::size_t index = 0; index < groups.size(); ++index) {
        if (model.sensorFilter >= 0 && model.sensorFilter != static_cast<int>(index)) {
            continue;
        }
        if (index == 4 && !model.settings.capture.includeDiskNet) {
            continue;
        }
        if (top + 22.0f > inner.bottom) {
            break;
        }

        renderer.text(groups[index].title, Rect{ inner.left, top, inner.right, top + 18.0f }, TextRole::Small,
            theme.accent);
        top += 22.0f;

        for (MetricId id : groups[index].ids) {
            if (top + 26.0f > inner.bottom) {
                break;
            }
            top = drawRow(renderer, id, model, theme, inner, top);
        }
        top += 8.0f;
    }

    renderer.text(L"Источники: PDH для счётчиков GPU, NtQuerySystemInformation для CPU и памяти, DXGI для видеопамяти,"
                  L" NVML для температур NVIDIA. «—» — датчик недоступен без драйвера.",
        Rect{ area.left, area.bottom - 28.0f, area.right, area.bottom }, TextRole::Tiny, theme.textFaint);
}

} // namespace yawa

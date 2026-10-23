#include "ui/sensors_view.h"
#include "ui/layout.h"

#include <algorithm>
#include <array>
#include <string>

namespace yawa {
namespace {

struct SensorGroup {
    std::wstring_view title;
    MetricId first;
    std::size_t count;
};

constexpr std::array<SensorGroup, 6> kGroups{ {
    { L"Кадры", MetricId::Fps, 6 },
    { L"Процессор", MetricId::CpuLoad, 5 },
    { L"Видеокарта", MetricId::GpuLoad, 9 },
    { L"Память", MetricId::RamUsed, 4 },
    { L"Диски и сеть", MetricId::DiskRead, 5 },
    { L"Система", MetricId::ChipsetTemp, 4 },
} };

// Датчики без источника в приложении (нет MSR-драйвера, нет NVML, поток игры не отслеживается)
// не должны выглядеть как настоящие нули.
bool isMeasured(MetricId id, const MetricSlot& slot)
{
    if (!slot.stats.initialized) {
        return false;
    }
    switch (id) {
    case MetricId::CpuTemp:
    case MetricId::CpuClock:
    case MetricId::CpuPower:
    case MetricId::GameThreadLoad:
    case MetricId::GpuTemp:
    case MetricId::GpuHotspot:
    case MetricId::GpuClock:
    case MetricId::GpuMemClock:
    case MetricId::GpuPower:
    case MetricId::GpuFan:
    case MetricId::VramTotal:
    case MetricId::DiskTemp:
    case MetricId::ChipsetTemp:
        return slot.stats.current > 0.0;
    default:
        return true;
    }
}

void drawRow(Renderer& renderer, const AppModel& model, const Theme& theme, MetricId id, const Rect& row)
{
    const MetricInfo& info = metricInfo(id);
    const MetricSlot& slot = model.store.slot(id);
    const bool measured = isMeasured(id, slot);

    renderer.text(wideText(info.label), Rect{ row.left, row.top, row.left + row.width() * 0.42f, row.bottom },
        TextRole::Small, theme.textDim);
    renderer.text(measured ? wideMetric(id, slot.stats.current) : std::wstring(L"нет данных"),
        Rect{ row.left + row.width() * 0.42f, row.top, row.left + row.width() * 0.66f, row.bottom },
        measured ? TextRole::Body : TextRole::Small, measured ? theme.text : theme.textFaint, TextAlign::Right);

    const std::wstring stats = measured ? wideNumber(slot.stats.minimum, info.unit, info.decimals)
            + L" · " + wideNumber(slot.stats.average, info.unit, info.decimals) + L" · "
            + wideNumber(slot.stats.maximum, info.unit, info.decimals)
                                                  : L"—";
    renderer.text(stats, Rect{ row.left + row.width() * 0.68f, row.top, row.right, row.bottom }, TextRole::Tiny,
        theme.textFaint, TextAlign::Right);
}

} // namespace

void drawSensors(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits)
{
    (void)hits;

    const float columnGap = 18.0f;
    const float columnWidth = (area.width() - columnGap) * 0.5f;

    float leftTop = area.top;
    float rightTop = area.top;

    for (std::size_t groupIndex = 0; groupIndex < kGroups.size(); ++groupIndex) {
        const SensorGroup& group = kGroups[groupIndex];
        const bool leftSide = groupIndex < 3;

        float& top = leftSide ? leftTop : rightTop;
        const float left = leftSide ? area.left : area.left + columnWidth + columnGap;

        renderer.text(group.title, Rect{ left, top, left + columnWidth, top + 20.0f }, TextRole::Tiny, theme.accent);
        top += 22.0f;

        for (std::size_t index = 0; index < group.count; ++index) {
            const auto id = static_cast<MetricId>(static_cast<std::size_t>(group.first) + index);
            const Rect row{ left, top, left + columnWidth, top + 26.0f };
            renderer.drawLine(row.left, row.bottom, row.right, row.bottom, theme.line.withAlpha(0.5f), 1.0f);
            drawRow(renderer, model, theme, id, row);
            top += 26.0f;
        }

        top += 14.0f;
    }

    const float noteTop = std::max(leftTop, rightTop) + 6.0f;
    if (noteTop < area.bottom - 20.0f) {
        renderer.text(
            L"Температура процессора и чипсета недоступна без драйвера доступа к датчикам — "
            L"показываем «нет данных», а не ноль.",
            Rect{ area.left, noteTop, area.right, noteTop + 20.0f }, TextRole::Tiny, theme.textFaint);
    }
}

} // namespace yawa

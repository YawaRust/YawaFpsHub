#pragma once

#include "metrics/metric_id.h"
#include "metrics/metric_store.h"
#include "metrics/units.h"

#include <array>
#include <string>

namespace yawa {

inline constexpr std::array<MetricId, 3> kPairedMetricDetails { MetricId::VramTotal, MetricId::RamFree,
    MetricId::PageFileTotal };

constexpr MetricId displayMetric(MetricId id)
{
    switch (id) {
    case MetricId::VramTotal:
        return MetricId::VramUsed;
    case MetricId::RamFree:
        return MetricId::RamUsed;
    case MetricId::PageFileTotal:
        return MetricId::PageFile;
    case MetricId::CpuPeakLoad:
        return MetricId::CpuLoad;
    default:
        return id;
    }
}

// Одна строка вместо «занято» и «всего»; недоступную ёмкость не подставляем нулём.
inline std::string displayMetricValue(MetricId id, const MetricStore& store)
{
    if (!store.hasValue(id)) {
        return "нет данных";
    }
    const MetricInfo& info = metricInfo(id);
    const std::string value = formatWithUnit(store.value(id), info.unit, info.decimals);
    MetricId detail = id;
    double total = 0.0;
    switch (id) {
    case MetricId::VramUsed:
        detail = MetricId::VramTotal;
        total = store.value(detail);
        break;
    case MetricId::RamUsed:
        detail = MetricId::RamFree;
        total = store.value(id) + store.value(detail);
        break;
    case MetricId::PageFile:
        detail = MetricId::PageFileTotal;
        total = store.value(detail);
        break;
    default:
        return value;
    }
    if (!store.hasValue(detail) || total <= 0.0) {
        return value;
    }
    return formatValue(store.value(id), info.decimals) + " / "
        + formatWithUnit(total, info.unit, info.decimals);
}

} // namespace yawa

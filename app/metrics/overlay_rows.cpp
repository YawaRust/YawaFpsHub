#include "metrics/overlay_rows.h"
#include "metrics/overlay_metric.h"
#include "metrics/units.h"

#include <algorithm>

namespace yawa {
namespace {

    bool metricKnown(const MetricStore& store, const MetricInfo& metric)
    {
        if (!store.hasValue(metric.id)) {
            return false;
        }
        switch (metric.unit) {
        case MetricUnit::Celsius:
        case MetricUnit::Megahertz:
        case MetricUnit::Watts:
        case MetricUnit::Rpm:
            return store.value(metric.id) > 0.0;
        default:
            return true;
        }
    }

} // namespace

std::vector<OverlayMetricRow> overlayMetricRows(const OverlaySettings& settings, const MetricStore& store)
{
    std::vector<OverlayMetricRow> rows;
    for (MetricId id : settings.order) {
        if (static_cast<std::size_t>(id) >= kMetricCount || displayMetric(id) != id) {
            continue;
        }
        const MetricInfo& metric = metricInfo(id);
        bool selected = settings.metrics[static_cast<std::size_t>(id)];
        for (MetricId detail : kPairedMetricDetails) {
            if (displayMetric(detail) == id) {
                selected |= settings.metrics[static_cast<std::size_t>(detail)];
            }
        }
        if (!selected) {
            continue;
        }
        std::string label(metric.label);
        const auto primary
            = std::find(kOverlayPrimaryMetrics.begin(), kOverlayPrimaryMetrics.end(), metric.id);
        if (primary != kOverlayPrimaryMetrics.end()) {
            label = settings.labels[static_cast<std::size_t>(primary - kOverlayPrimaryMetrics.begin())];
        }
        auto decimals = metric.decimals;
        if (metric.id == MetricId::Fps || metric.id == MetricId::Low1Fps || metric.id == MetricId::Low01Fps
            || metric.id == MetricId::AvgFps) {
            decimals = static_cast<std::uint8_t>(std::clamp(settings.fpsDecimals, 0, 2));
        }
        const bool paired = std::any_of(kPairedMetricDetails.begin(), kPairedMetricDetails.end(),
            [id](MetricId detail) { return displayMetric(detail) == id; });
        rows.push_back({ metric.id, label,
            metricKnown(store, metric)
                ? (paired ? displayMetricValue(id, store)
                          : formatWithUnit(store.value(metric.id), metric.unit, decimals))
                                       : "нет данных" });
    }
    return rows;
}

} // namespace yawa

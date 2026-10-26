#pragma once

#include "metrics/metric_store.h"
#include "ui/settings.h"

#include <string>
#include <vector>

namespace yawa {

struct OverlayMetricRow
{
    MetricId id;
    std::string label;
    std::string value;
};

std::vector<OverlayMetricRow> overlayMetricRows(const OverlaySettings& settings, const MetricStore& store);

} // namespace yawa

#pragma once

#include "metrics/metric_id.h"

#include <string>
#include <string_view>

namespace yawa {

std::string_view unitSuffix(MetricUnit unit);

// Разделитель дробной части — запятая, разряды — узкий пробел: так значения читаются
// в русском интерфейсе и совпадают с диспетчером задач.
std::string formatValue(double value, std::uint8_t decimals);

std::string formatWithUnit(double value, MetricUnit unit, std::uint8_t decimals);

std::string formatMetric(MetricId id, double value);

} // namespace yawa

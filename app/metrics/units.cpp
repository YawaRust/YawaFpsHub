#include "metrics/units.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace yawa {
namespace {

constexpr std::string_view kThinSpace = "\xE2\x80\x89";

void groupThousands(std::string& digits)
{
    if (digits.size() <= 3) {
        return;
    }

    std::string grouped;
    grouped.reserve(digits.size() + digits.size() / 3);

    std::size_t remaining = digits.size();
    for (std::size_t index = 0; index < digits.size(); ++index) {
        grouped.push_back(digits[index]);
        --remaining;
        if (remaining > 0 && remaining % 3 == 0) {
            grouped.append(kThinSpace);
        }
    }
    digits.swap(grouped);
}

} // namespace

std::string_view unitSuffix(MetricUnit unit)
{
    switch (unit) {
    case MetricUnit::Percent:
        return "%";
    case MetricUnit::Milliseconds:
        return "мс";
    case MetricUnit::Celsius:
        return "°C";
    case MetricUnit::Megahertz:
        return "МГц";
    case MetricUnit::Watts:
        return "Вт";
    case MetricUnit::Rpm:
        return "об/мин";
    case MetricUnit::Gigabytes:
        return "ГБ";
    case MetricUnit::MegabytesPerSecond:
        return "МБ/с";
    case MetricUnit::MegabitsPerSecond:
        return "Мбит/с";
    case MetricUnit::Count:
        break;
    }
    return {};
}

std::string formatValue(double value, std::uint8_t decimals)
{
    if (!std::isfinite(value)) {
        return "—";
    }

    const double rounded = std::round(value * std::pow(10.0, decimals)) / std::pow(10.0, decimals);
    if (std::abs(rounded) < std::pow(10.0, -static_cast<int>(decimals)) * 0.5) {
        value = 0.0;
    } else {
        value = rounded;
    }

    std::array<char, 64> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%.*f", static_cast<int>(decimals), value);
    if (written <= 0) {
        return "—";
    }

    std::string text(buffer.data(), static_cast<std::size_t>(written));
    const std::size_t dot = text.find('.');
    const std::size_t integerEnd = dot == std::string::npos ? text.size() : dot;

    const bool negative = text[0] == '-';
    std::string digits = text.substr(negative ? 1 : 0, integerEnd - (negative ? 1 : 0));
    groupThousands(digits);

    std::string result;
    if (negative) {
        result.push_back('-');
    }
    result.append(digits);
    if (dot != std::string::npos) {
        result.push_back(',');
        result.append(text.substr(dot + 1));
    }
    return result;
}

std::string formatWithUnit(double value, MetricUnit unit, std::uint8_t decimals)
{
    std::string text = formatValue(value, decimals);
    const std::string_view suffix = unitSuffix(unit);
    if (!suffix.empty()) {
        text.push_back(' ');
        text.append(suffix);
    }
    return text;
}

std::string formatMetric(MetricId id, double value)
{
    const MetricInfo& info = metricInfo(id);
    return formatWithUnit(value, info.unit, info.decimals);
}

} // namespace yawa

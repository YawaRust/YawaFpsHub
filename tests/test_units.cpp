#include "metrics/units.h"
#include "test_support.h"

#include <string>

namespace yawa::test {

namespace {

void testDecimalComma()
{
    CHECK(formatValue(7.04, 2) == "7,04");
    CHECK(formatValue(0.3, 1) == "0,3");
    CHECK(formatValue(-1.5, 1) == "-1,5");
}

void testRounding()
{
    CHECK(formatValue(7.999, 2) == "8,00");
    CHECK(formatValue(0.04, 1) == "0,0");
    CHECK(formatValue(2.5, 0) == "3");
}

void testThousandsGrouping()
{
    CHECK(formatValue(1204.0, 0) == "1" "\xE2\x80\x89" "204");
    CHECK(formatValue(712428.0, 0) == "712" "\xE2\x80\x89" "428");
    CHECK(formatValue(1000.5, 1) == "1" "\xE2\x80\x89" "000,5");
}

void testUnits()
{
    CHECK(formatMetric(MetricId::CpuTemp, 64.4) == "64 °C");
    CHECK(formatMetric(MetricId::CpuClock, 4925.0) == "4" "\xE2\x80\x89" "925 МГц");
    CHECK(formatMetric(MetricId::NetDown, 2.44) == "2,4 Мбит/с");
    CHECK(formatWithUnit(12.0, MetricUnit::Count, 0) == "12");
    CHECK(unitSuffix(MetricUnit::Rpm) == "об/мин");
}

void testNotFinite()
{
    CHECK(formatValue(0.0 / 0.0, 0) == "—");
}

void testMetricLookup()
{
    const MetricInfo* info = findMetricByKey("gpu_load");
    CHECK(info != nullptr);
    CHECK(info != nullptr && info->id == MetricId::GpuLoad);
    CHECK(findMetricByKey("no_such_metric") == nullptr);
}

} // namespace

void runUnitsTests()
{
    testDecimalComma();
    testRounding();
    testThousandsGrouping();
    testUnits();
    testNotFinite();
    testMetricLookup();
}

} // namespace yawa::test

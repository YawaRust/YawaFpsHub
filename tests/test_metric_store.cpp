#include "metrics/metric_store.h"
#include "test_support.h"

namespace yawa::test {

namespace {

void testStoresValues()
{
    MetricStore store;
    CHECK(!store.hasValue(MetricId::GpuLoad));

    store.set(MetricId::GpuLoad, 93.0);
    CHECK(store.hasValue(MetricId::GpuLoad));
    CHECK_NEAR(store.value(MetricId::GpuLoad), 93.0, 1e-9);
}

void testSmoothing()
{
    MetricStore store;
    store.set(MetricId::CpuLoad, 20.0, 50);
    store.set(MetricId::CpuLoad, 40.0, 50);

    CHECK_NEAR(store.value(MetricId::CpuLoad), 40.0, 1e-9);
    CHECK_NEAR(store.slot(MetricId::CpuLoad).stats.smoothed, 30.0, 1e-9);
}

void testHistoryOrder()
{
    MetricStore store;
    for (int index = 1; index <= 5; ++index) {
        store.set(MetricId::Fps, static_cast<double>(index));
    }

    const MetricSlot& slot = store.slot(MetricId::Fps);
    CHECK(slot.historyCount == 5);
    CHECK_NEAR(slot.history[slot.orderedIndex(0)], 1.0, 1e-6);
    CHECK_NEAR(slot.history[slot.orderedIndex(4)], 5.0, 1e-6);
}

void testHistoryKeepsLastSamples()
{
    MetricStore store;
    const std::size_t total = kHistoryCapacity + 40;
    for (std::size_t index = 0; index < total; ++index) {
        store.set(MetricId::Fps, static_cast<double>(index));
    }

    const MetricSlot& slot = store.slot(MetricId::Fps);
    CHECK(slot.historyCount == kHistoryCapacity);
    CHECK_NEAR(slot.history[slot.orderedIndex(slot.historyCount - 1)], static_cast<double>(total - 1), 1e-3);
    CHECK_NEAR(slot.history[slot.orderedIndex(0)], static_cast<double>(total - kHistoryCapacity), 1e-3);
}

void testMetricsDoNotMix()
{
    MetricStore store;
    store.set(MetricId::CpuLoad, 44.0);
    store.set(MetricId::GpuLoad, 93.0);

    CHECK_NEAR(store.value(MetricId::CpuLoad), 44.0, 1e-9);
    CHECK_NEAR(store.value(MetricId::GpuLoad), 93.0, 1e-9);
}

void testReset()
{
    MetricStore store;
    store.set(MetricId::RamUsed, 21.4);
    store.reset();

    CHECK(!store.hasValue(MetricId::RamUsed));
    CHECK(store.slot(MetricId::RamUsed).historyCount == 0);
}

} // namespace

void runMetricStoreTests()
{
    testStoresValues();
    testSmoothing();
    testHistoryOrder();
    testHistoryKeepsLastSamples();
    testMetricsDoNotMix();
    testReset();
}

} // namespace yawa::test

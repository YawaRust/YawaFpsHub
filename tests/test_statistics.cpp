#include "metrics/statistics.h"
#include "test_support.h"

#include <vector>

namespace yawa::test {

namespace {

void testRunningStats()
{
    MetricStats stats;
    stats.add(10.0);
    CHECK_NEAR(stats.minimum, 10.0, 1e-9);
    CHECK_NEAR(stats.maximum, 10.0, 1e-9);

    stats.add(30.0);
    stats.add(20.0);
    CHECK_NEAR(stats.minimum, 10.0, 1e-9);
    CHECK_NEAR(stats.maximum, 30.0, 1e-9);
    CHECK_NEAR(stats.average, 20.0, 1e-9);
    CHECK(stats.samples == 3);
}

void testSmoothing()
{
    CHECK_NEAR(smoothStep(10.0, 20.0, 0), 20.0, 1e-9);
    CHECK_NEAR(smoothStep(10.0, 20.0, 50), 15.0, 1e-9);

    // Ограничение сверху не даёт графику застыть навсегда.
    CHECK(smoothStep(10.0, 20.0, 100) < 20.0);
}

void testPercentile()
{
    // percentile работает с уже отсортированным массивом.
    const std::vector<double> values{ 1.0, 2.0, 3.0, 4.0 };
    CHECK_NEAR(percentile(values, 0.5), 2.0, 1e-9);
    CHECK_NEAR(percentile(values, 1.0), 4.0, 1e-9);
    CHECK_NEAR(percentile(values, 0.0), 1.0, 1e-9);
}

void testLowFpsUsesSlowestFrames()
{
    FrameTimeWindow window;
    for (int index = 0; index < 99; ++index) {
        window.push(1000.0 / 120.0);
    }
    window.push(40.0);

    CHECK(window.size() == 100);
    CHECK_NEAR(window.currentFps(), 25.0, 1e-6);
    CHECK_NEAR(window.averageFps(), 100.0 * 1000.0 / (99.0 * (1000.0 / 120.0) + 40.0), 1e-6);

    // Один медленный кадр из ста не должен утянуть 1% low: он попадает только в 0.1% low.
    const double low1 = window.lowFraction(0.01);
    const double low01 = window.lowFraction(0.001);
    CHECK(low1 > low01);
    CHECK_NEAR(low01, 25.0, 1e-6);
}

void testDroppedShare()
{
    FrameTimeWindow window;
    for (int index = 0; index < 50; ++index) {
        window.push(1000.0 / 120.0);
    }
    for (int index = 0; index < 50; ++index) {
        window.push(1000.0 / 30.0);
    }

    CHECK_NEAR(window.droppedShare(1000.0 / 60.0), 0.5, 1e-9);
    CHECK_NEAR(window.droppedShare(1000.0 / 20.0), 0.0, 1e-9);
}

void testWindowWrapsAround()
{
    FrameTimeWindow window;
    for (std::size_t index = 0; index < window.capacity() + 120; ++index) {
        window.push(8.0);
    }
    CHECK(window.size() == window.capacity());
    CHECK_NEAR(window.averageFps(), 125.0, 1e-6);
}

void testSummary()
{
    FrameTimeWindow window;
    for (int index = 0; index < 240; ++index) {
        window.push(1000.0 / 120.0);
    }

    const FpsSummary summary = FpsSummary::fromWindow(window, 60.0);
    CHECK_NEAR(summary.currentFps, 120.0, 1e-6);
    CHECK_NEAR(summary.frameTimeMs, 1000.0 / 120.0, 1e-6);
    CHECK_NEAR(summary.low1Fps, 120.0, 1e-6);

    // Все кадры уложились в бюджет 60 fps, поэтому пропусков нет.
    CHECK_NEAR(summary.droppedShare, 0.0, 1e-9);

    FrameTimeWindow slow;
    for (int index = 0; index < 60; ++index) {
        slow.push(1000.0 / 30.0);
    }
    CHECK_NEAR(FpsSummary::fromWindow(slow, 60.0).droppedShare, 1.0, 1e-9);
}

} // namespace

void runStatisticsTests()
{
    testRunningStats();
    testSmoothing();
    testPercentile();
    testLowFpsUsesSlowestFrames();
    testDroppedShare();
    testWindowWrapsAround();
    testSummary();
}

} // namespace yawa::test

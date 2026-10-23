#include "metrics/system_sampler.h"
#include "test_support.h"

namespace yawa::test {

namespace {

SystemSnapshot baseSnapshot()
{
    SystemSnapshot snapshot;
    snapshot.cpuIdleTicks = 8'000;
    snapshot.cpuTotalTicks = 10'000;
    snapshot.memoryTotalBytes = 64ull * 1024 * 1024 * 1024;
    snapshot.memoryFreeBytes = 31ull * 1024 * 1024 * 1024;
    snapshot.memoryCacheBytes = 11ull * 1024 * 1024 * 1024;
    snapshot.pageFileUsedBytes = 900ull * 1024 * 1024;
    snapshot.selfWorkingSetBytes = 24ull * 1024 * 1024;
    snapshot.selfThreads = 3;
    snapshot.gpuLoadPercent = 93.0;
    return snapshot;
}

void testFirstSampleHasNoBaseline()
{
    SystemSampler sampler;
    const SystemSampler::Deltas deltas = sampler.compute(baseSnapshot(), 1.0);

    CHECK(!deltas.hasBaseline);
    CHECK_NEAR(deltas.cpuLoadPercent, 0.0, 1e-9);
}

void testCpuLoadFromTicks()
{
    SystemSampler sampler;
    sampler.compute(baseSnapshot(), 1.0);

    SystemSnapshot next = baseSnapshot();
    next.cpuIdleTicks += 500;
    next.cpuTotalTicks += 1'000;

    const SystemSampler::Deltas deltas = sampler.compute(next, 1.0);
    CHECK(deltas.hasBaseline);
    CHECK_NEAR(deltas.cpuLoadPercent, 50.0, 1e-6);
}

void testIdleTicksLargerThanTotalDoNotOverflow()
{
    SystemSampler sampler;
    sampler.compute(baseSnapshot(), 1.0);

    SystemSnapshot next = baseSnapshot();
    next.cpuIdleTicks += 2'000;
    next.cpuTotalTicks += 1'000;

    const SystemSampler::Deltas deltas = sampler.compute(next, 1.0);
    CHECK(deltas.cpuLoadPercent >= 0.0);
    CHECK(deltas.cpuLoadPercent <= 100.0);
}

void testDiskAndNetworkRates()
{
    SystemSampler sampler;
    sampler.compute(baseSnapshot(), 1.0);

    SystemSnapshot next = baseSnapshot();
    next.diskReadBytes = 500ull * 1024 * 1024;
    next.diskWriteBytes = 100ull * 1024 * 1024;
    next.netReceivedBytes = 2'500'000;
    next.netSentBytes = 1'250'000;

    const SystemSampler::Deltas deltas = sampler.compute(next, 1.0);
    CHECK_NEAR(deltas.diskReadMBps, 500.0, 1e-6);
    CHECK_NEAR(deltas.diskWriteMBps, 100.0, 1e-6);
    CHECK_NEAR(deltas.netDownMbps, 20.0, 1e-6);
    CHECK_NEAR(deltas.netUpMbps, 10.0, 1e-6);
}

void testRatesScaleWithInterval()
{
    SystemSampler sampler;
    sampler.compute(baseSnapshot(), 1.0);

    SystemSnapshot next = baseSnapshot();
    next.diskReadBytes = 250ull * 1024 * 1024;

    const SystemSampler::Deltas deltas = sampler.compute(next, 0.5);
    CHECK_NEAR(deltas.diskReadMBps, 500.0, 1e-6);
}

void testSelfLoad()
{
    SystemSampler sampler;
    sampler.compute(baseSnapshot(), 1.0);

    SystemSnapshot next = baseSnapshot();
    next.selfCpuTicks += 3;
    next.cpuTotalTicks += 1'000;
    next.cpuIdleTicks += 900;

    const SystemSampler::Deltas deltas = sampler.compute(next, 1.0);
    CHECK_NEAR(deltas.selfCpuPercent, 0.3, 1e-6);
}

void testDerivedMemory()
{
    const SystemSnapshot snapshot = baseSnapshot();
    const DerivedValues values = deriveValues(snapshot, SystemSampler::Deltas{});

    CHECK_NEAR(values.ramUsedGb, 33.0, 1e-6);
    CHECK_NEAR(values.ramFreeGb, 31.0, 1e-6);
    CHECK_NEAR(values.ramCacheGb, 11.0, 1e-6);
    CHECK_NEAR(values.pageFileGb, 900.0 / 1024.0, 1e-6);
}

void testDerivedVramUsesBudget()
{
    SystemSnapshot snapshot = baseSnapshot();
    snapshot.vramUsedBytes = 9ull * 1024 * 1024 * 1024;
    snapshot.vramBudgetBytes = 16ull * 1024 * 1024 * 1024;

    const DerivedValues values = deriveValues(snapshot, SystemSampler::Deltas{});
    CHECK_NEAR(values.vramUsedGb, 9.0, 1e-6);
    CHECK_NEAR(values.vramTotalGb, 16.0, 1e-6);
}

void testApplySnapshotFillsStore()
{
    MetricStore store;
    SystemSnapshot snapshot = baseSnapshot();
    snapshot.cpuTemperature = 64.0;
    snapshot.gpuTemperature = 71.0;
    snapshot.gpuFanRpm = 1940;

    SystemSampler::Deltas deltas;
    deltas.cpuLoadPercent = 44.0;
    deltas.diskReadMBps = 184.0;
    deltas.selfCpuPercent = 0.3;

    FpsSummary fps;
    fps.currentFps = 142.0;
    fps.frameTimeMs = 1000.0 / 142.0;
    fps.low1Fps = 118.0;

    applySnapshot(store, snapshot, deriveValues(snapshot, deltas), fps, 0);

    CHECK_NEAR(store.value(MetricId::Fps), 142.0, 1e-6);
    CHECK_NEAR(store.value(MetricId::CpuLoad), 44.0, 1e-6);
    CHECK_NEAR(store.value(MetricId::CpuTemp), 64.0, 1e-6);
    CHECK_NEAR(store.value(MetricId::GpuTemp), 71.0, 1e-6);
    CHECK_NEAR(store.value(MetricId::GpuFan), 1940.0, 1e-6);
    CHECK_NEAR(store.value(MetricId::RamUsed), 33.0, 1e-6);
    CHECK_NEAR(store.value(MetricId::DiskRead), 184.0, 1e-6);
    CHECK_NEAR(store.value(MetricId::SelfCpu), 0.3, 1e-6);
    CHECK_NEAR(store.value(MetricId::SelfRam), 24.0 / 1024.0, 1e-6);
}

} // namespace

void runSystemSamplerTests()
{
    testFirstSampleHasNoBaseline();
    testCpuLoadFromTicks();
    testIdleTicksLargerThanTotalDoNotOverflow();
    testDiskAndNetworkRates();
    testRatesScaleWithInterval();
    testSelfLoad();
    testDerivedMemory();
    testDerivedVramUsesBudget();
    testApplySnapshotFillsStore();
}

} // namespace yawa::test

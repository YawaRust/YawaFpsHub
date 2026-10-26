#include "metrics/cpu_telemetry.h"
#include "metrics/system_sampler.h"
#include "test_support.h"

#include <limits>

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

void testDiskAndNetworkRatesPassThrough()
{
    SystemSnapshot snapshot = baseSnapshot();
    snapshot.diskReadBytesPerSecond = 500.0 * 1024 * 1024;
    snapshot.diskWriteBytesPerSecond = 100.0 * 1024 * 1024;
    snapshot.netReceivedBytesPerSecond = 2'500'000.0;
    snapshot.netSentBytesPerSecond = 1'250'000.0;

    const DerivedValues values = deriveValues(snapshot, SystemSampler::Deltas{});
    CHECK_NEAR(values.diskRead, 500.0, 1e-6);
    CHECK_NEAR(values.diskWrite, 100.0, 1e-6);
    CHECK_NEAR(values.netDown, 20.0, 1e-6);
    CHECK_NEAR(values.netUp, 10.0, 1e-6);
}

void testRatesIgnoreNegativeValues()
{
    SystemSnapshot snapshot = baseSnapshot();
    snapshot.diskReadBytesPerSecond = -5.0;
    snapshot.netSentBytesPerSecond = 0.0;

    const DerivedValues values = deriveValues(snapshot, SystemSampler::Deltas{});
    CHECK_NEAR(values.diskRead, 0.0, 1e-9);
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
    snapshot.cpuPowerWatts = 85.0;
    snapshot.gpuTemperature = 71.0;
    snapshot.gpuFanRpm = 1940;
    snapshot.diskReadBytesPerSecond = 184.0 * 1024 * 1024;

    SystemSampler::Deltas deltas;
    deltas.cpuLoadPercent = 44.0;
    deltas.selfCpuPercent = 0.3;

    FpsSummary fps;
    fps.currentFps = 142.0;
    fps.frameTimeMs = 1000.0 / 142.0;
    fps.low1Fps = 118.0;

    applySnapshot(store, snapshot, deriveValues(snapshot, deltas), fps, 0);

    CHECK_NEAR(store.value(MetricId::Fps), 142.0, 1e-6);
    CHECK_NEAR(store.value(MetricId::CpuLoad), 44.0, 1e-6);
    CHECK_NEAR(store.value(MetricId::CpuTemp), 64.0, 1e-6);
    CHECK_NEAR(store.value(MetricId::CpuPower), 85.0, 1e-6);
    CHECK_NEAR(store.value(MetricId::GpuTemp), 71.0, 1e-6);
    CHECK_NEAR(store.value(MetricId::GpuFan), 1940.0, 1e-6);
    CHECK_NEAR(store.value(MetricId::RamUsed), 33.0, 1e-6);
    CHECK_NEAR(store.value(MetricId::DiskRead), 184.0, 1e-6);
    CHECK_NEAR(store.value(MetricId::SelfCpu), 0.3, 1e-6);
    CHECK_NEAR(store.value(MetricId::SelfRam), 24.0 / 1024.0, 1e-6);
}

void testCpuTemperatureRegisters()
{
    CHECK(!intelPackageTemperature(0, 100ull << 16));
    CHECK_NEAR(
        intelPackageTemperature((1ull << 31) | (30ull << 16), 100ull << 16).value_or(-999), 70.0, 1e-9);
    CHECK(!intelPackageTemperature(1ull << 31, 0));
    CHECK(!intelPackageTemperature((1ull << 31) | (127ull << 16), 100ull << 16));
    CHECK(!amdControlTemperature(0));
    CHECK(!amdControlTemperature(0xffffffffu));
    CHECK_NEAR(amdControlTemperature(640u << 21).value_or(-999), 80.0, 1e-9);
    CHECK_NEAR(amdControlTemperature((1000u << 21) | (1u << 19)).value_or(-999), 76.0, 1e-9);
    CHECK_NEAR(amdControlTemperature((1000u << 21) | (3u << 16)).value_or(-999), 76.0, 1e-9);
}

void testPackageEnergy()
{
    CpuEnergyCounter energy;
    CHECK(!energy.sample(100, 14, 1.0));
    const auto power = energy.sample(8292, 14, 1.5);
    CHECK_NEAR(power.value_or(-999), 1.0, 1e-9);
    CHECK(!energy.sample(8293, 14, 1.5));
    CHECK(!energy.sample(8294, 14, 20.0));
    energy.reset();
    CHECK(!energy.sample(0xfffffff0u, 10, 21.0));
    const auto wrapped = energy.sample(0x10u, 10, 21.5);
    CHECK_NEAR(wrapped.value_or(-999), 0.0625, 1e-9);
    CHECK(!energy.sample(0x20u, 11, 22.0));
    CHECK(!energy.sample(0x30u, 32, 22.5));
    CHECK(!energy.sample(0x40u, 10, 23.0));
    CHECK(!energy.sample(0xffffffffu, 10, 23.5));
    energy.reset();
    CHECK(!energy.sample(1, 31, 30.0));
    CHECK(!energy.sample(2, 31, 31.0));
    CHECK(!energy.sample(3, 10, std::numeric_limits<double>::quiet_NaN()));
    CHECK(!energy.sample(4, 10, 32.0));
    CHECK(!energy.sample(5, 10, 31.0));
    CHECK(!energy.sample(6, 10, -1.0));
}

} // namespace

void runSystemSamplerTests()
{
    testCpuTemperatureRegisters();
    testPackageEnergy();
    testFirstSampleHasNoBaseline();
    testCpuLoadFromTicks();
    testIdleTicksLargerThanTotalDoNotOverflow();
    testDiskAndNetworkRatesPassThrough();
    testRatesIgnoreNegativeValues();
    testSelfLoad();
    testDerivedMemory();
    testDerivedVramUsesBudget();
    testApplySnapshotFillsStore();
}

} // namespace yawa::test

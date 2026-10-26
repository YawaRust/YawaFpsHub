#include "metrics/bottleneck.h"
#include "metrics/hardware_info.h"
#include "test_support.h"

#include <string>

namespace yawa::test {
namespace {

HardwareIdentity demoMachine()
{
    HardwareIdentity info;
    info.detected = true;
    info.cpu.name = "AMD Ryzen 7 5800X3D";
    info.cpu.physicalCores = 8;
    info.cpu.logicalProcessors = 16;
    info.cpu.baseMhz = 3400.0;

    GpuIdentity gpu;
    gpu.name = "NVIDIA GeForce RTX 4090";
    gpu.vendor = "NVIDIA";
    gpu.dedicatedMb = 24576.0;
    gpu.discrete = true;
    info.gpus.push_back(gpu);

    info.memory.totalGb = 32.0;
    info.memory.type = "DDR4";
    info.memory.ratedMt = 3600.0;
    info.memory.configuredMt = 3600.0;
    info.memory.channels = 2;
    for (int index = 0; index < 2; ++index) {
        MemoryModule module;
        module.populated = true;
        module.sizeGb = 16.0;
        module.ratedMt = 3600.0;
        module.configuredMt = 3600.0;
        module.type = "DDR4";
        module.slot = index == 0 ? "DIMM A1" : "DIMM B1";
        module.vendor = "G Skill";
        info.memory.modules.push_back(module);
    }

    DisplayIdentity display;
    display.name = "LG 27GP850";
    display.width = 2560;
    display.height = 1440;
    display.refreshHz = 165;
    display.primary = true;
    info.displays.push_back(display);
    return info;
}

SystemLoad liveLoads(double cpu, double core, double gpu, double fps)
{
    SystemLoad load;
    load.cpuLoad = cpu;
    load.cpuPeakCore = core;
    load.gpuLoad = gpu;
    load.fps = fps;
    load.vramUsedGb = 6.0;
    load.vramTotalGb = 24.0;
    load.ramLoad = 45.0;
    load.pageFileLoad = 5.0;
    load.targetFps = 165.0;
    return load;
}

bool contains(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
}

bool hasAdvice(const BottleneckResult& result, const char* needle)
{
    for (const BottleneckAdvice& advice : result.advice) {
        if (contains(advice.text, needle) || contains(advice.title, needle)) {
            return true;
        }
    }
    return false;
}

void testIndexesAreMonotonic()
{
    CHECK(cpuPerfIndex("AMD Ryzen 7 5800X3D", 8, 16) > cpuPerfIndex("Intel Core i5-9400F", 6, 6));
    CHECK(cpuPerfIndex("AMD Ryzen 9 7950X", 16, 32) > cpuPerfIndex("AMD Ryzen 9 7900X", 12, 24));
    CHECK(gpuPerfIndex("NVIDIA GeForce RTX 4090", 24576.0) > gpuPerfIndex("NVIDIA GeForce RTX 4060", 8192.0));
    CHECK(gpuPerfIndex("NVIDIA GeForce RTX 4070 Ti", 12288.0) > gpuPerfIndex("NVIDIA GeForce RTX 4070", 12288.0));
    CHECK(gpuPerfIndex("AMD Radeon RX 7900 XTX", 24576.0) > gpuPerfIndex("AMD Radeon RX 7600", 8192.0));
    // Встроенное видео не должно получать индекс быстрее дискретной карты.
    CHECK(gpuPerfIndex("Intel UHD Graphics 770", 128.0) < gpuPerfIndex("NVIDIA GeForce RTX 3060", 12288.0));
    CHECK(cpuPerfIndex("", 0, 0) == 0.0);
    CHECK(gpuPerfIndex("Microsoft Basic Render Driver", 0.0) == 0.0);
}

void testMeasuredCpuBottleneck()
{
    const HardwareIdentity machine = demoMachine();
    const BottleneckResult result = analyzeBottleneck(machine, liveLoads(96.0, 99.0, 52.0, 118.0));

    CHECK(result.kind == BottleneckKind::LiveCpu);
    CHECK(result.measured);
    CHECK(contains(result.headline, "процессор"));
    CHECK(result.percent > 25);
    // Главное при упоре в CPU — перенести нагрузку на видеокарту.
    CHECK(hasAdvice(result, "разрешени"));
}

void testMeasuredGpuBottleneck()
{
    const HardwareIdentity machine = demoMachine();
    const BottleneckResult result = analyzeBottleneck(machine, liveLoads(41.0, 60.0, 99.0, 74.0));

    CHECK(result.kind == BottleneckKind::LiveGpu);
    CHECK(contains(result.headline, "видеокарту"));
    CHECK(hasAdvice(result, "DLSS"));
}

void testBalancedPairHasNoBottleneck()
{
    const HardwareIdentity machine = demoMachine();
    const BottleneckResult result = analyzeBottleneck(machine, liveLoads(93.0, 97.0, 98.0, 164.0));

    CHECK(result.kind == BottleneckKind::LiveBalanced);
    CHECK(result.percent <= 12);
    CHECK(hasAdvice(result, "Апгрейд"));
}

void testVideoMemoryShortageWins()
{
    HardwareIdentity machine = demoMachine();
    SystemLoad load = liveLoads(70.0, 80.0, 99.0, 60.0);
    load.vramUsedGb = 23.9;
    load.vramTotalGb = 24.0;

    const BottleneckResult result = analyzeBottleneck(machine, load);
    CHECK(result.kind == BottleneckKind::VramLimited);
    CHECK(contains(result.detail, "Видеопамять заполнена"));
}

void testFrameLimitedByRefresh()
{
    const HardwareIdentity machine = demoMachine();
    const BottleneckResult result = analyzeBottleneck(machine, liveLoads(30.0, 40.0, 38.0, 164.0));

    CHECK(result.kind == BottleneckKind::RefreshLimited);
    CHECK(result.percent == 0);
    CHECK(hasAdvice(result, "G-SYNC"));
}

void testIdleIsNotABottleneck()
{
    const HardwareIdentity machine = demoMachine();
    const BottleneckResult result = analyzeBottleneck(machine, liveLoads(3.0, 6.0, 2.0, 0.0));

    CHECK(result.kind == BottleneckKind::Idle);
    CHECK(result.advice.empty());
}

void testStaticEstimateFollowsResolution()
{
    HardwareIdentity machine = demoMachine();
    machine.gpus[0].name = "NVIDIA GeForce RTX 4090";
    machine.cpu.name = "Intel Core i5-9600K";
    machine.cpu.physicalCores = 6;
    machine.cpu.logicalProcessors = 6;
    machine.displays[0].width = 1920;
    machine.displays[0].height = 1080;

    // Слабый CPU и флагманская карта в 1080p — прогноз должен показать упор в процессор.
    SystemLoad idle;
    const BottleneckResult at1080 = analyzeBottleneck(machine, idle);
    CHECK(at1080.measured == false);
    CHECK(at1080.kind == BottleneckKind::EstimateCpu);

    // То же железо в 4K упирается в видеокарту: нагрузка на GPU растёт с числом пикселей.
    machine.displays[0].width = 3840;
    machine.displays[0].height = 2160;
    const BottleneckResult at4k = analyzeBottleneck(machine, idle);
    CHECK(at4k.kind == BottleneckKind::EstimateGpu);
    CHECK(at4k.expectedFps < at1080.expectedFps);
}

void testUnknownHardwareSaysSo()
{
    HardwareIdentity machine;
    machine.detected = true;
    machine.cpu.name = "";

    const BottleneckResult result = analyzeBottleneck(machine, SystemLoad{});
    CHECK(result.kind == BottleneckKind::NoData);
    CHECK(result.advice.empty());
    CHECK(contains(result.detail, "не смогло определить"));
}

void testMemoryDescription()
{
    const HardwareIdentity machine = demoMachine();
    const std::string text = describeMemory(machine.memory);
    CHECK(contains(text, "2×16"));
    CHECK(contains(text, "DDR4-3600"));
    CHECK(contains(text, "2 канала"));

    HardwareIdentity single = demoMachine();
    single.memory.modules.resize(1);
    single.memory.totalGb = 16.0;
    single.memory.channels = 1;
    const std::string singleText = describeMemory(single.memory);
    CHECK(contains(singleText, "1×16"));
    CHECK(contains(singleText, "один канал"));

    MemoryIdentity unknown;
    CHECK(describeMemory(unknown) == "нет данных");
}

void testCycleTime()
{
    MemoryIdentity memory;
    memory.configuredMt = 6000.0;
    CHECK_NEAR(memory.cycleTimeNs(), 0.3333, 0.001);

    memory.configuredMt = 0.0;
    CHECK(memory.cycleTimeNs() == 0.0);
}

} // namespace

void runBottleneckTests()
{
    testIndexesAreMonotonic();
    testMeasuredCpuBottleneck();
    testMeasuredGpuBottleneck();
    testBalancedPairHasNoBottleneck();
    testVideoMemoryShortageWins();
    testFrameLimitedByRefresh();
    testIdleIsNotABottleneck();
    testStaticEstimateFollowsResolution();
    testUnknownHardwareSaysSo();
    testMemoryDescription();
    testCycleTime();
}

} // namespace yawa::test

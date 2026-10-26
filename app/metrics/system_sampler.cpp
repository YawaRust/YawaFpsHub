#include "metrics/system_sampler.h"

#include <algorithm>
#include <cmath>

namespace yawa {
namespace {

constexpr double kBytesInGigabyte = 1024.0 * 1024.0 * 1024.0;
constexpr double kBytesInMegabyte = 1024.0 * 1024.0;
constexpr double kBitsInMegabit = 1'000'000.0;

double ratioPercent(std::uint64_t part, std::uint64_t total)
{
    if (total == 0) {
        return 0.0;
    }
    return std::clamp(static_cast<double>(part) * 100.0 / static_cast<double>(total), 0.0, 100.0);
}

double toGigabytes(std::uint64_t bytes)
{
    return static_cast<double>(bytes) / kBytesInGigabyte;
}

} // namespace

SystemSampler::Deltas SystemSampler::compute(const SystemSnapshot& snapshot, double elapsedSeconds)
{
    Deltas deltas;
    if (!m_hasPrevious || elapsedSeconds <= 0.0) {
        m_previous = snapshot;
        m_hasPrevious = true;
        return deltas;
    }

    const std::uint64_t totalTicks = snapshot.cpuTotalTicks - m_previous.cpuTotalTicks;
    const std::uint64_t idleTicks = snapshot.cpuIdleTicks - m_previous.cpuIdleTicks;

    // Счётчик GetSystemTimes может отдать нулевой дельта-интервал, если опрос попал
    // в тот же квант: тогда загрузку не обновляем, иначе получим деление на ноль.
    if (totalTicks > 0) {
        deltas.cpuLoadPercent = ratioPercent(totalTicks - std::min(idleTicks, totalTicks), totalTicks);
    }

    if (totalTicks > 0) {
        const std::uint64_t selfTicks = snapshot.selfCpuTicks - m_previous.selfCpuTicks;
        deltas.selfCpuPercent = ratioPercent(selfTicks, totalTicks);
    }

    deltas.hasBaseline = true;

    m_previous = snapshot;
    return deltas;
}

double processCpuPercent(std::uint64_t busyTicks, double seconds, std::uint32_t logicalProcessors)
{
    if (seconds <= 0.0 || logicalProcessors == 0) { return -1.0; }
    return std::clamp(static_cast<double>(busyTicks) / 10000000.0 / seconds
            / static_cast<double>(logicalProcessors) * 100.0, 0.0, 100.0);
}

DerivedValues deriveValues(const SystemSnapshot& snapshot, const SystemSampler::Deltas& deltas)
{
    DerivedValues values;
    values.cpuLoad = deltas.cpuLoadPercent;
    values.diskRead = std::max(0.0, snapshot.diskReadBytesPerSecond) / kBytesInMegabyte;
    values.diskWrite = std::max(0.0, snapshot.diskWriteBytesPerSecond) / kBytesInMegabyte;
    values.netDown = std::max(0.0, snapshot.netReceivedBytesPerSecond) * 8.0 / kBitsInMegabit;
    values.netUp = std::max(0.0, snapshot.netSentBytesPerSecond) * 8.0 / kBitsInMegabit;
    values.selfCpu = deltas.selfCpuPercent;

    const std::uint64_t usedBytes = snapshot.memoryFreeBytes < snapshot.memoryTotalBytes
        ? snapshot.memoryTotalBytes - snapshot.memoryFreeBytes
        : 0;
    values.ramUsedGb = toGigabytes(usedBytes);
    values.ramCacheGb = toGigabytes(snapshot.memoryCacheBytes);
    values.ramFreeGb = toGigabytes(snapshot.memoryFreeBytes);
    values.pageFileGb = toGigabytes(snapshot.pageFileUsedBytes);
    // Загрузка ОЗУ и файла подкачки — проценты от их полного объёма.
    values.ramLoad = snapshot.memoryTotalBytes > 0
        ? 100.0 * static_cast<double>(usedBytes) / static_cast<double>(snapshot.memoryTotalBytes)
        : 0.0;
    values.pageFileLoad = snapshot.pageFileTotalBytes > 0
        ? 100.0 * static_cast<double>(snapshot.pageFileUsedBytes) / static_cast<double>(snapshot.pageFileTotalBytes)
        : 0.0;
    values.pageFileTotalGb = toGigabytes(snapshot.pageFileTotalBytes);

    values.ramLoad = std::clamp(values.ramLoad, 0.0, 100.0);
    values.pageFileLoad = std::clamp(values.pageFileLoad, 0.0, 100.0);

    values.vramUsedGb = toGigabytes(snapshot.vramUsedBytes);
    values.vramTotalGb = toGigabytes(snapshot.vramBudgetBytes);
    return values;
}

void applySnapshot(MetricStore& store, const SystemSnapshot& snapshot, const DerivedValues& values,
    const FpsSummary& fps, std::uint8_t smoothingPercent)
{
    store.set(MetricId::Fps, fps.currentFps, smoothingPercent);
    store.set(MetricId::FrameTime, fps.frameTimeMs);
    store.set(MetricId::Low1Fps, fps.low1Fps);
    store.set(MetricId::Low01Fps, fps.low01Fps);
    store.set(MetricId::AvgFps, fps.averageFps);
    // В хранилище доля хранится в процентах: иначе в интерфейсе «1» читалось бы как 1 %.
    store.set(MetricId::DroppedFrames, fps.droppedShare * 100.0);

    store.set(MetricId::CpuLoad, values.cpuLoad, smoothingPercent);
    store.set(MetricId::CpuTemp, snapshot.cpuTemperature);
    store.set(MetricId::CpuClock, snapshot.cpuClockMhz);
    store.set(MetricId::CpuPower, snapshot.cpuPowerWatts);
    store.set(MetricId::GameThreadLoad, snapshot.gameThreadLoad, smoothingPercent);

    const std::size_t cores = std::min<std::size_t>(snapshot.coreCount, snapshot.coreLoadPercent.size());
    double peak = 0.0;
    for (std::size_t index = 0; index < cores; ++index) {
        peak = std::max(peak, snapshot.coreLoadPercent[index]);
        store.set(static_cast<MetricId>(static_cast<std::size_t>(MetricId::Core0Load) + index),
            snapshot.coreLoadPercent[index], smoothingPercent);
    }
    // Пик берём без сглаживания: смысл строки — показать, что ядро действительно
    // упирается в 100 %, а усреднение это скрывает.
    store.set(MetricId::CpuPeakLoad, cores > 0 ? peak : 0.0);

    store.set(MetricId::GpuLoad, snapshot.gpuLoadPercent, smoothingPercent);
    store.set(MetricId::GpuTemp, snapshot.gpuTemperature);
    store.set(MetricId::GpuHotspot, snapshot.gpuHotspot);
    store.set(MetricId::GpuClock, snapshot.gpuClockMhz);
    store.set(MetricId::GpuMemClock, snapshot.gpuMemClockMhz);
    store.set(MetricId::GpuPower, snapshot.gpuPowerWatts);
    store.set(MetricId::GpuFan, static_cast<double>(snapshot.gpuFanRpm));
    store.set(MetricId::GpuEncode, snapshot.gpuEncodePercent, smoothingPercent);
    store.set(MetricId::GpuDecode, snapshot.gpuDecodePercent, smoothingPercent);
    store.set(MetricId::PcieLoad, snapshot.pcieLoadPercent, smoothingPercent);
    if (snapshot.vramUsageKnown) {
        store.set(MetricId::VramUsed, values.vramUsedGb);
    } else {
        store.slot(MetricId::VramUsed) = MetricSlot { };
    }
    if (snapshot.vramBudgetBytes != 0) {
        store.set(MetricId::VramTotal, values.vramTotalGb);
    } else {
        store.slot(MetricId::VramTotal) = MetricSlot { };
    }

    store.set(MetricId::RamUsed, values.ramUsedGb);
    store.set(MetricId::RamCache, values.ramCacheGb);
    store.set(MetricId::RamFree, values.ramFreeGb);
    store.set(MetricId::PageFile, values.pageFileGb);
    store.set(MetricId::PageFileTotal, values.pageFileTotalGb);
    store.set(MetricId::RamLoad, values.ramLoad, smoothingPercent);
    store.set(MetricId::PageFileLoad, values.pageFileLoad, smoothingPercent);

    store.set(MetricId::DiskRead, values.diskRead);
    store.set(MetricId::DiskWrite, values.diskWrite);
    store.set(MetricId::DiskTemp, snapshot.diskTemperature);
    store.set(MetricId::NetDown, values.netDown);
    store.set(MetricId::NetUp, values.netUp);

    store.set(MetricId::ChipsetTemp, snapshot.chipsetTemperature);
    store.set(MetricId::SelfCpu, values.selfCpu);
    store.set(MetricId::SelfRam, toGigabytes(snapshot.selfWorkingSetBytes));
    store.set(MetricId::SelfThreads, static_cast<double>(snapshot.selfThreads));
}

} // namespace yawa

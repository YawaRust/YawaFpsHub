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

    const double bytesPerSecondToMegabytes = 1.0 / (kBytesInMegabyte * elapsedSeconds);
    deltas.diskReadMBps = static_cast<double>(snapshot.diskReadBytes - m_previous.diskReadBytes)
        * bytesPerSecondToMegabytes;
    deltas.diskWriteMBps = static_cast<double>(snapshot.diskWriteBytes - m_previous.diskWriteBytes)
        * bytesPerSecondToMegabytes;
    deltas.netDownMbps = static_cast<double>(snapshot.netReceivedBytes - m_previous.netReceivedBytes) * 8.0
        / (kBitsInMegabit * elapsedSeconds);
    deltas.netUpMbps = static_cast<double>(snapshot.netSentBytes - m_previous.netSentBytes) * 8.0
        / (kBitsInMegabit * elapsedSeconds);

    if (totalTicks > 0) {
        const std::uint64_t selfTicks = snapshot.selfCpuTicks - m_previous.selfCpuTicks;
        deltas.selfCpuPercent = ratioPercent(selfTicks, totalTicks);
    }

    deltas.diskReadMBps = std::max(0.0, deltas.diskReadMBps);
    deltas.diskWriteMBps = std::max(0.0, deltas.diskWriteMBps);
    deltas.netDownMbps = std::max(0.0, deltas.netDownMbps);
    deltas.netUpMbps = std::max(0.0, deltas.netUpMbps);
    deltas.hasBaseline = true;

    m_previous = snapshot;
    return deltas;
}

DerivedValues deriveValues(const SystemSnapshot& snapshot, const SystemSampler::Deltas& deltas)
{
    DerivedValues values;
    values.cpuLoad = deltas.cpuLoadPercent;
    values.diskRead = deltas.diskReadMBps;
    values.diskWrite = deltas.diskWriteMBps;
    values.netDown = deltas.netDownMbps;
    values.netUp = deltas.netUpMbps;
    values.selfCpu = deltas.selfCpuPercent;

    const std::uint64_t usedBytes = snapshot.memoryFreeBytes < snapshot.memoryTotalBytes
        ? snapshot.memoryTotalBytes - snapshot.memoryFreeBytes
        : 0;
    values.ramUsedGb = toGigabytes(usedBytes);
    values.ramCacheGb = toGigabytes(snapshot.memoryCacheBytes);
    values.ramFreeGb = toGigabytes(snapshot.memoryFreeBytes);
    values.pageFileGb = toGigabytes(snapshot.pageFileUsedBytes);

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
    store.set(MetricId::DroppedFrames, fps.droppedShare);

    store.set(MetricId::CpuLoad, values.cpuLoad, smoothingPercent);
    store.set(MetricId::CpuTemp, snapshot.cpuTemperature);
    store.set(MetricId::CpuClock, snapshot.cpuClockMhz);
    store.set(MetricId::CpuPower, snapshot.cpuPowerWatts);
    store.set(MetricId::GameThreadLoad, snapshot.gameThreadLoad, smoothingPercent);

    store.set(MetricId::GpuLoad, snapshot.gpuLoadPercent, smoothingPercent);
    store.set(MetricId::GpuTemp, snapshot.gpuTemperature);
    store.set(MetricId::GpuHotspot, snapshot.gpuHotspot);
    store.set(MetricId::GpuClock, snapshot.gpuClockMhz);
    store.set(MetricId::GpuMemClock, snapshot.gpuMemClockMhz);
    store.set(MetricId::GpuPower, snapshot.gpuPowerWatts);
    store.set(MetricId::GpuFan, static_cast<double>(snapshot.gpuFanRpm));
    store.set(MetricId::VramUsed, values.vramUsedGb);
    store.set(MetricId::VramTotal, values.vramTotalGb);

    store.set(MetricId::RamUsed, values.ramUsedGb);
    store.set(MetricId::RamCache, values.ramCacheGb);
    store.set(MetricId::RamFree, values.ramFreeGb);
    store.set(MetricId::PageFile, values.pageFileGb);

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

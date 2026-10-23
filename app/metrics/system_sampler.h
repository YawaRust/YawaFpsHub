#pragma once

#include "metrics/metric_id.h"
#include "metrics/metric_store.h"
#include "metrics/statistics.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace yawa {

// Значения, которые отдают платформенные счётчики за один интервал опроса.
// Ядро метрик работает только с этими числами и не знает, откуда они пришли.
struct SystemSnapshot {
    std::uint64_t cpuIdleTicks = 0;
    std::uint64_t cpuTotalTicks = 0;
    std::uint64_t memoryTotalBytes = 0;
    std::uint64_t memoryFreeBytes = 0;
    std::uint64_t memoryCacheBytes = 0;
    std::uint64_t pageFileUsedBytes = 0;

    std::uint64_t diskReadBytes = 0;
    std::uint64_t diskWriteBytes = 0;
    std::uint64_t netReceivedBytes = 0;
    std::uint64_t netSentBytes = 0;

    std::uint64_t selfCpuTicks = 0;
    std::uint64_t selfWorkingSetBytes = 0;
    std::uint32_t selfThreads = 0;

    double cpuTemperature = 0.0;
    double cpuClockMhz = 0.0;
    double cpuPowerWatts = 0.0;
    double gameThreadLoad = 0.0;

    double gpuLoadPercent = 0.0;
    double gpuTemperature = 0.0;
    double gpuHotspot = 0.0;
    double gpuClockMhz = 0.0;
    double gpuMemClockMhz = 0.0;
    double gpuPowerWatts = 0.0;
    std::uint32_t gpuFanRpm = 0;
    std::uint64_t vramUsedBytes = 0;
    std::uint64_t vramBudgetBytes = 0;

    double diskTemperature = 0.0;
    double chipsetTemperature = 0.0;
};

// Считает производные значения между двумя снимками: проценты, скорость в байтах в секунду.
// Чистая логика, без Win32 — поэтому проверяется тестами на любой платформе.
class SystemSampler {
public:
    struct Deltas {
        double cpuLoadPercent = 0.0;
        double diskReadMBps = 0.0;
        double diskWriteMBps = 0.0;
        double netDownMbps = 0.0;
        double netUpMbps = 0.0;
        double selfCpuPercent = 0.0;
        bool hasBaseline = false;
    };

    Deltas compute(const SystemSnapshot& snapshot, double elapsedSeconds);

private:
    SystemSnapshot m_previous{};
    bool m_hasPrevious = false;
};

struct DerivedValues {
    double cpuLoad = 0.0;
    double diskRead = 0.0;
    double diskWrite = 0.0;
    double netDown = 0.0;
    double netUp = 0.0;
    double selfCpu = 0.0;
    double ramUsedGb = 0.0;
    double ramCacheGb = 0.0;
    double ramFreeGb = 0.0;
    double pageFileGb = 0.0;
    double vramUsedGb = 0.0;
    double vramTotalGb = 0.0;
};

DerivedValues deriveValues(const SystemSnapshot& snapshot, const SystemSampler::Deltas& deltas);

// Переносит готовый кадр в хранилище: единая точка, чтобы UI и запись сессии видели
// одни и те же числа.
void applySnapshot(MetricStore& store, const SystemSnapshot& snapshot, const DerivedValues& values,
    const FpsSummary& fps, std::uint8_t smoothingPercent);

} // namespace yawa

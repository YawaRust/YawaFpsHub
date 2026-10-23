#pragma once

#include "metrics/gpu_registry.h"
#include "metrics/system_sampler.h"
#include "platform/dxgi_adapters.h"
#include "platform/nvml_sensors.h"
#include "platform/pdh_query.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

struct IWbemServices;

namespace yawa {

class Log;

// Что реально удалось поднять на этой машине. Остальное приложение честно показывает
// как «нет данных», а не нулём.
struct SensorAvailability {
    bool cpu = false;
    bool cpuClock = false;
    bool cpuTemperature = false;
    bool gameThread = false;
    bool memory = false;
    bool disk = false;
    bool network = false;
    bool gpuLoad = false;
    bool gpuMemory = false;
    bool gpuTemperature = false;
    bool gpuPower = false;
};

class SystemSensors {
public:
    SystemSensors() = default;

    SystemSensors(const SystemSensors&) = delete;
    SystemSensors& operator=(const SystemSensors&) = delete;

    bool initialize(Log& log, std::string& error);
    void shutdown();

    const GpuRegistry& gpus() const { return m_adapters.registry(); }
    std::size_t activeGpu() const { return m_activeGpu; }
    void selectGpu(std::size_t index);

    bool sample(SystemSnapshot& snapshot);
    const SensorAvailability& availability() const { return m_availability; }

    std::wstring activeGpuName() const;
    std::uint32_t coreCount() const { return m_coreCount; }

private:
    void initializeCpu(Log& log);
    bool initializeGpu(Log& log, std::string& error);
    void sampleCpu(SystemSnapshot& snapshot);
    void sampleGameThread(SystemSnapshot& snapshot);
    // ACPI-термозона через WMI: работает там, где PDH-счётчик тепловых зон молчит.
    bool queryWmiTemperature(double& celsius);
    void sampleMemory(SystemSnapshot& snapshot);
    void sampleDiskAndNetwork(SystemSnapshot& snapshot);
    void sampleGpu(SystemSnapshot& snapshot);
    void sampleSelf(SystemSnapshot& snapshot);

    DxgiAdapterList m_adapters;
    NvmlSensors m_nvml;
    PdhCounter m_cpuPerformance;
    PdhCounter m_diskRead;
    PdhCounter m_diskWrite;
    PdhWildcardCounter m_netReceived;
    PdhWildcardCounter m_netSent;
    PdhWildcardCounter m_gpuEngine;
    // ACPI-термозона: отдаёт температуру без прав администратора и без MSR-драйвера.
    PdhWildcardCounter m_thermalZone;
    // Счётчик загрузки на каждое физическое ядро; nullptr — ядра с таким номером нет.
    std::array<std::unique_ptr<PdhCounter>, 16> m_coreLoad;

    SensorAvailability m_availability;
    std::size_t m_activeGpu = 0;
    std::uint32_t m_nominalCpuMhz = 0;
    std::uint32_t m_coreCount = 0;
    // Кэш WMI-температуры: запрос дорогой, поэтому не чаще раза в 15 секунд.
    double m_wmiTemperature = 0.0;
    std::uint64_t m_wmiQueriedAt = 0;
    bool m_wmiAvailable = false;
    // WMI недоступен на этой машине: не повторяем дорогой запрос каждые 15 секунд.
    bool m_wmiFailed = false;
    double m_gpuLoad = 0.0;

    // Поток игры: главный поток активного окна. Считаем по разнице времён, как диспетчер задач.
    std::uint32_t m_gameThreadId = 0;
    std::uint64_t m_gameThreadTicks = 0;
    std::uint64_t m_gameQueryTime = 0;
};

} // namespace yawa

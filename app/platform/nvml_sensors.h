#pragma once

#include <cstdint>
#include <string>

namespace yawa {

struct GpuSensorReading {
    bool available = false;
    double temperature = 0.0;
    double hotspot = 0.0;
    double graphicsClockMhz = 0.0;
    double memoryClockMhz = 0.0;
    double powerWatts = 0.0;
    std::uint32_t fanRpm = 0;
};

// Температуры, частоты и питание есть только у драйвера вендора: NVML ставится вместе
// с драйвером NVIDIA и не требует ни установки, ни службы. Библиотека грузится динамически,
// поэтому на машине без NVIDIA приложение работает без неё и без ошибок загрузки.
class NvmlSensors {
public:
    NvmlSensors() = default;
    ~NvmlSensors();

    NvmlSensors(const NvmlSensors&) = delete;
    NvmlSensors& operator=(const NvmlSensors&) = delete;

    bool initialize(std::string& error);
    void shutdown();

    bool isAvailable() const { return m_initialized; }

    // nameHint — имя адаптера из DXGI: у ноутбуков с Optimus NVML может отдавать
    // устройства в другом порядке, поэтому сверяем по имени.
    void selectDevice(const std::wstring& nameHint);
    GpuSensorReading read() const;

private:
    struct Impl;
    Impl* m_impl = nullptr;
    bool m_initialized = false;
};

} // namespace yawa

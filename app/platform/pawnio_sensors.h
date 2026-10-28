#pragma once

#include "metrics/cpu_telemetry.h"
#include "metrics/memory_timings.h"
#include "metrics/sensor_status.h"

#include <windows.h>

#include <cstdint>
#include <optional>

namespace yawa {

class Log;

struct CpuHardwareReading
{
    std::optional<double> temperature;
    std::optional<double> powerWatts;
};

class PawnioSensors
{
public:
    PawnioSensors() = default;
    ~PawnioSensors();
    PawnioSensors(const PawnioSensors&) = delete;
    PawnioSensors& operator=(const PawnioSensors&) = delete;

    static bool supportsCpu();
    bool initialize(Log& log);
    void shutdown();
    CpuHardwareReading read();
    std::optional<AmdDdr4MemoryDump> readMemoryDump();
    std::optional<AmdDdr4Timings> readMemoryTimings();
    SensorFailure failure() const { return m_failure; }

private:
    bool readRegister(bool smn, std::uint32_t address, std::uint64_t& value);

    HANDLE m_device = INVALID_HANDLE_VALUE;
    SensorFailure m_failure = SensorFailure::Unknown;
    HANDLE m_pciMutex = nullptr;
    bool m_amd = false;
    bool m_packageThermal = false;
    CpuEnergyCounter m_energy;
};

} // namespace yawa

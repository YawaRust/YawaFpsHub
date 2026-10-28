#pragma once

#include "metrics/hardware_telemetry.h"

#include <windows.h>

#include <mutex>
#include <optional>
#include <vector>

namespace yawa {
class Log;

class StorageSensors
{
public:
    ~StorageSensors();
    bool initialize(Log& log);
    void shutdown();
    std::optional<double> temperature() const;
    std::vector<DiskSmartReading> health() const;

private:
    static DWORD WINAPI worker(void* context);
    void poll();
    bool query(HANDLE disk, DWORD controlCode, void* buffer, DWORD size, DWORD& written);
    std::optional<double> readDisk(HANDLE disk);
    std::optional<NvmeSmartHealth> readNvme(HANDLE disk, DWORD property);

    HANDLE m_stop = nullptr;
    HANDLE m_thread = nullptr;
    mutable std::mutex m_mutex;
    std::optional<double> m_temperature;
    std::vector<DiskSmartReading> m_health;
    ULONGLONG m_sampledAt = 0;
};
} // namespace yawa

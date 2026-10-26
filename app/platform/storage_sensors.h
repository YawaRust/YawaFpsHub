#pragma once

#include <windows.h>

#include <mutex>
#include <optional>

namespace yawa {
class Log;

class StorageSensors
{
public:
    ~StorageSensors();
    bool initialize(Log& log);
    void shutdown();
    std::optional<double> temperature() const;

private:
    static DWORD WINAPI worker(void* context);
    void poll();
    bool query(HANDLE disk, void* buffer, DWORD size, DWORD& written);
    std::optional<double> readDisk(HANDLE disk);

    HANDLE m_stop = nullptr;
    HANDLE m_thread = nullptr;
    mutable std::mutex m_mutex;
    std::optional<double> m_temperature;
    ULONGLONG m_sampledAt = 0;
};
} // namespace yawa

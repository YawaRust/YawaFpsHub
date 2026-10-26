#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace yawa {

enum class MetricUnit : std::uint8_t {
    Count,
    Percent,
    Milliseconds,
    Celsius,
    Megahertz,
    Watts,
    Rpm,
    Gigabytes,
    MegabytesPerSecond,
    MegabitsPerSecond,
};

enum class MetricId : std::uint8_t {
    Fps,
    FrameTime,
    Low1Fps,
    Low01Fps,
    AvgFps,
    DroppedFrames,

    CpuLoad,
    CpuTemp,
    CpuClock,
    CpuPower,
    GameThreadLoad,
    // Самый загруженный поток: считается по тем же данным, что и загрузка ядер,
    // и работает всегда, в отличие от питания без драйвера.
    CpuPeakLoad,

    GpuLoad,
    GpuTemp,
    GpuHotspot,
    GpuClock,
    GpuMemClock,
    GpuPower,
    GpuFan,
    // Медиа-движки GPU и шина: кодирование/декодирование видео считаются по тем же
    // счётчикам GPU Engine, что и 3D, поэтому работают у NVIDIA, AMD и Intel.
    GpuEncode,
    GpuDecode,
    PcieLoad,
    VramUsed,
    VramTotal,

    RamUsed,
    RamCache,
    RamFree,
    PageFile,
    RamLoad,
    PageFileLoad,

    DiskRead,
    DiskWrite,
    DiskTemp,
    NetDown,
    NetUp,

    ChipsetTemp,
    SelfCpu,
    SelfRam,
    SelfThreads,

    // Загрузка физических ядер: тот же счётчик, что показывает диспетчер задач.
    Core0Load,
    Core1Load,
    Core2Load,
    Core3Load,
    Core4Load,
    Core5Load,
    Core6Load,
    Core7Load,
    Core8Load,
    Core9Load,
    Core10Load,
    Core11Load,
    Core12Load,
    Core13Load,
    Core14Load,
    Core15Load,
};

struct MetricInfo {
    MetricId id;
    std::string_view key;
    std::string_view label;
    MetricUnit unit;
    std::uint8_t decimals;
};

constexpr std::size_t kMetricCount = 55;

inline constexpr std::array<MetricInfo, kMetricCount> kMetricTable{{
    { MetricId::Fps, "fps", "FPS", MetricUnit::Count, 0 },
    { MetricId::FrameTime, "frame_time", "Frame time", MetricUnit::Milliseconds, 2 },
    { MetricId::Low1Fps, "low_1", "1% low", MetricUnit::Count, 0 },
    { MetricId::Low01Fps, "low_0_1", "0.1% low", MetricUnit::Count, 0 },
    { MetricId::AvgFps, "fps_avg", "Средний FPS", MetricUnit::Count, 1 },
    { MetricId::DroppedFrames, "dropped", "Пропущено за секунду", MetricUnit::Percent, 1 },

    { MetricId::CpuLoad, "cpu_load", "Загрузка процессора", MetricUnit::Percent, 0 },
    { MetricId::CpuTemp, "cpu_temp", "Температура CPU", MetricUnit::Celsius, 0 },
    { MetricId::CpuClock, "cpu_clock", "Частота CPU", MetricUnit::Megahertz, 0 },
    { MetricId::CpuPower, "cpu_power", "Питание CPU", MetricUnit::Watts, 0 },
    { MetricId::GameThreadLoad, "game_thread", "Поток игры", MetricUnit::Percent, 0 },
    { MetricId::CpuPeakLoad, "cpu_peak", "Пик по ядрам", MetricUnit::Percent, 0 },

    { MetricId::GpuLoad, "gpu_load", "Загрузка видеокарты", MetricUnit::Percent, 0 },
    { MetricId::GpuTemp, "gpu_temp", "Температура GPU", MetricUnit::Celsius, 0 },
    { MetricId::GpuHotspot, "gpu_hotspot", "Hotspot GPU", MetricUnit::Celsius, 0 },
    { MetricId::GpuClock, "gpu_clock", "Частота ядра", MetricUnit::Megahertz, 0 },
    { MetricId::GpuMemClock, "gpu_mem_clock", "Частота памяти", MetricUnit::Megahertz, 0 },
    { MetricId::GpuPower, "gpu_power", "Питание GPU", MetricUnit::Watts, 0 },
    { MetricId::GpuFan, "gpu_fan", "Вентилятор GPU", MetricUnit::Rpm, 0 },
    { MetricId::GpuEncode, "gpu_encode", "Кодирование видео", MetricUnit::Percent, 0 },
    { MetricId::GpuDecode, "gpu_decode", "Декодирование видео", MetricUnit::Percent, 0 },
    { MetricId::PcieLoad, "pcie_load", "Нагрузка PCI-e", MetricUnit::Percent, 0 },
    { MetricId::VramUsed, "vram_used", "Видеопамять занято", MetricUnit::Gigabytes, 1 },
    { MetricId::VramTotal, "vram_total", "Видеопамять всего", MetricUnit::Gigabytes, 1 },

    { MetricId::RamUsed, "ram_used", "Память занято", MetricUnit::Gigabytes, 1 },
    { MetricId::RamCache, "ram_cache", "Память в кэше", MetricUnit::Gigabytes, 1 },
    { MetricId::RamFree, "ram_free", "Память свободно", MetricUnit::Gigabytes, 1 },
    { MetricId::PageFile, "page_file", "Своп-файл", MetricUnit::Gigabytes, 1 },
    { MetricId::RamLoad, "ram_load", "Загрузка ОЗУ", MetricUnit::Percent, 0 },
    { MetricId::PageFileLoad, "page_file_load", "Загрузка файла подкачки", MetricUnit::Percent, 0 },

    { MetricId::DiskRead, "disk_read", "Диск: чтение", MetricUnit::MegabytesPerSecond, 0 },
    { MetricId::DiskWrite, "disk_write", "Диск: запись", MetricUnit::MegabytesPerSecond, 0 },
    { MetricId::DiskTemp, "disk_temp", "Температура NVMe", MetricUnit::Celsius, 0 },
    { MetricId::NetDown, "net_down", "Сеть: входящий", MetricUnit::MegabitsPerSecond, 1 },
    { MetricId::NetUp, "net_up", "Сеть: исходящий", MetricUnit::MegabitsPerSecond, 1 },

    { MetricId::ChipsetTemp, "chipset_temp", "Температура чипсета", MetricUnit::Celsius, 0 },
    { MetricId::SelfCpu, "self_cpu", "Собственное потребление CPU", MetricUnit::Percent, 1 },
    { MetricId::SelfRam, "self_ram", "Собственное потребление ОЗУ", MetricUnit::Gigabytes, 2 },
    { MetricId::SelfThreads, "self_threads", "Потоков в процессе", MetricUnit::Count, 0 },

    { MetricId::Core0Load, "core0_load", "Ядро 1", MetricUnit::Percent, 0 },
    { MetricId::Core1Load, "core1_load", "Ядро 2", MetricUnit::Percent, 0 },
    { MetricId::Core2Load, "core2_load", "Ядро 3", MetricUnit::Percent, 0 },
    { MetricId::Core3Load, "core3_load", "Ядро 4", MetricUnit::Percent, 0 },
    { MetricId::Core4Load, "core4_load", "Ядро 5", MetricUnit::Percent, 0 },
    { MetricId::Core5Load, "core5_load", "Ядро 6", MetricUnit::Percent, 0 },
    { MetricId::Core6Load, "core6_load", "Ядро 7", MetricUnit::Percent, 0 },
    { MetricId::Core7Load, "core7_load", "Ядро 8", MetricUnit::Percent, 0 },
    { MetricId::Core8Load, "core8_load", "Ядро 9", MetricUnit::Percent, 0 },
    { MetricId::Core9Load, "core9_load", "Ядро 10", MetricUnit::Percent, 0 },
    { MetricId::Core10Load, "core10_load", "Ядро 11", MetricUnit::Percent, 0 },
    { MetricId::Core11Load, "core11_load", "Ядро 12", MetricUnit::Percent, 0 },
    { MetricId::Core12Load, "core12_load", "Ядро 13", MetricUnit::Percent, 0 },
    { MetricId::Core13Load, "core13_load", "Ядро 14", MetricUnit::Percent, 0 },
    { MetricId::Core14Load, "core14_load", "Ядро 15", MetricUnit::Percent, 0 },
    { MetricId::Core15Load, "core15_load", "Ядро 16", MetricUnit::Percent, 0 },
}};

constexpr std::size_t metricIndex(MetricId id)
{
    return static_cast<std::size_t>(id);
}

inline const MetricInfo& metricInfo(MetricId id)
{
    return kMetricTable[metricIndex(id)];
}

inline const MetricInfo* findMetricByKey(std::string_view key)
{
    for (const MetricInfo& info : kMetricTable) {
        if (info.key == key) {
            return &info;
        }
    }
    return nullptr;
}

} // namespace yawa

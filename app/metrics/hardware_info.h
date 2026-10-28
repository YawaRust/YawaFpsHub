#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace yawa {

// Паспорт процессора. Имя и частоты берутся из CPUID и реестра, ядра — из
// GetLogicalProcessorInformationEx: ни драйверов, ни WMI для этого не нужно.
struct CpuIdentity {
    std::string name;
    std::string vendor;
    std::uint32_t physicalCores = 0;
    std::uint32_t logicalProcessors = 0;
    double baseMhz = 0.0;
    std::uint32_t l3CacheMb = 0;
    bool smt = false;
};

struct GpuIdentity {
    std::string name;
    std::string vendor;
    double dedicatedMb = 0.0;
    std::string driverVersion;
    bool discrete = false;
};

// Одна планка памяти. Поле sizeGb нулевое — слот пустой, такие строки тоже нужны:
// по ним видно «2×16 ГБ» против «1×32 ГБ».
struct MemoryModule {
    std::string slot;
    std::string vendor;
    std::string part;
    double sizeGb = 0.0;
    double ratedMt = 0.0;
    double configuredMt = 0.0;
    std::string type;
    double voltage = 0.0;
    bool populated = false;
};

// Тайминги без SMBus-драйвера Windows не отдаёт: есть только частоты из SMBIOS.
// tCK выводим из конфигурационной частоты — это честная величина, её и показываем.
struct MemoryIdentity {
    double totalGb = 0.0;
    double ratedMt = 0.0;
    double configuredMt = 0.0;
    std::string type;
    std::uint32_t slots = 0;
    std::vector<MemoryModule> modules;

    double cycleTimeNs() const;
};

struct DisplayIdentity {
    std::string name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t refreshHz = 0;
    double scale = 1.0;
    bool primary = false;
};

struct DiskIdentity {
    // WMI Index is the PhysicalDrive number, not the position in this vector.
    int diskNumber = -1;
    std::string name;
    std::string interfaceType;
    std::string mediaType;
    double totalGb = 0.0;
    double freeGb = 0.0;
    std::string fileSystem;
    std::string driveLetter;
};

struct OsIdentity {
    std::string product;
    std::string edition;
    std::string featureVersion;
    std::string build;
    std::string architecture;
    bool isServer = false;
};

// Всё, что приложение знает о машине. Собирается один раз при старте; если источник
// недоступен, поле остаётся пустым, и интерфейс пишет «нет данных», а не ноль.
struct HardwareIdentity {
    OsIdentity os;
    CpuIdentity cpu;
    std::vector<GpuIdentity> gpus;
    MemoryIdentity memory;
    std::vector<DisplayIdentity> displays;
    std::vector<DiskIdentity> disks;
    bool detected = false;

    const GpuIdentity* primaryGpu() const;
    const DisplayIdentity* primaryDisplay() const;
};

// «2×16 ГБ DDR5-6000» — одна строка для карточки и для лога.
std::string describeMemory(const MemoryIdentity& memory);
std::string describeDisplay(const DisplayIdentity& display);

} // namespace yawa

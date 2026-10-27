#include "platform/pawnio_sensors.h"
#include "platform/log.h"
#include "platform/win_util.h"
#include "resources/pawnio_ids.h"

#include <winioctl.h>

#include <array>
#include <cstring>

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif

namespace yawa {
namespace {

    // Публичный протокол PawnIO 2.2.0; PawnIOLib не линкуется.
    constexpr DWORD kLoadModule = CTL_CODE(41394u, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD kExecute = CTL_CODE(41394u, 0x841, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD kVersion = CTL_CODE(41394u, 0x861, METHOD_BUFFERED, FILE_ANY_ACCESS);

    std::array<unsigned, 4> cpuid(unsigned leaf)
    {
#ifdef _MSC_VER
        int values[4] { };
        __cpuidex(values, static_cast<int>(leaf), 0);
        return { static_cast<unsigned>(values[0]), static_cast<unsigned>(values[1]),
            static_cast<unsigned>(values[2]), static_cast<unsigned>(values[3]) };
#else
        std::array<unsigned, 4> values { };
        __cpuid_count(leaf, 0, values[0], values[1], values[2], values[3]);
        return values;
#endif
    }

    class PackageAffinity
    {
    public:
        PackageAffinity()
        {
            GROUP_AFFINITY first { };
            first.Mask = 1;
            m_ready = SetThreadGroupAffinity(GetCurrentThread(), &first, &m_previous) != FALSE;
        }
        ~PackageAffinity()
        {
            if (m_ready) {
                SetThreadGroupAffinity(GetCurrentThread(), &m_previous, nullptr);
            }
        }
        bool ready() const { return m_ready; }

    private:
        GROUP_AFFINITY m_previous { };
        bool m_ready = false;
    };

    bool loadCpuModule(HANDLE device, int id)
    {
        const HMODULE instance = GetModuleHandleW(nullptr);
        const HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
        if (resource == nullptr) {
            SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
            return false;
        }
        const DWORD size = SizeofResource(instance, resource);
        const void* bytes = LockResource(LoadResource(instance, resource));
        if (bytes == nullptr || size == 0) {
            SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
            return false;
        }
        DWORD written = 0;
        return DeviceIoControl(
                   device, kLoadModule, const_cast<void*>(bytes), size, nullptr, 0, &written, nullptr)
            != FALSE;
    }

} // namespace

PawnioSensors::~PawnioSensors()
{
    shutdown();
}

bool PawnioSensors::supportsCpu()
{
    const auto vendor = cpuid(0);
    char name[13] { };
    std::memcpy(name, &vendor[1], 4);
    std::memcpy(name + 4, &vendor[3], 4);
    std::memcpy(name + 8, &vendor[2], 4);
    const unsigned signature = cpuid(1)[0];
    const unsigned baseFamily = (signature >> 8) & 0xf;
    const unsigned family = baseFamily == 0xf ? baseFamily + ((signature >> 20) & 0xff) : baseFamily;
    return (std::strcmp(name, "GenuineIntel") == 0 && family == 6)
        || (std::strcmp(name, "AuthenticAMD") == 0 && (family == 0x17 || family == 0x19 || family == 0x1a));
}

bool PawnioSensors::initialize(Log& log)
{
    shutdown();
    m_failure = SensorFailure::Unknown;
    const auto vendor = cpuid(0);
    char name[13] { };
    std::memcpy(name, &vendor[1], 4);
    std::memcpy(name + 4, &vendor[3], 4);
    std::memcpy(name + 8, &vendor[2], 4);
    const unsigned signature = cpuid(1)[0];
    const unsigned baseFamily = (signature >> 8) & 0xf;
    const unsigned family = baseFamily == 0xf ? baseFamily + ((signature >> 20) & 0xff) : baseFamily;
    m_amd = std::strcmp(name, "AuthenticAMD") == 0;
    const bool intel = std::strcmp(name, "GenuineIntel") == 0 && family == 6;
    if (!supportsCpu()) {
        m_failure = cpuSensorFailure(false, CpuDeviceFailure::Other);
        log.info("PawnIO: семейство CPU пока не поддержано сборщиком");
        return false;
    }
    m_packageThermal = intel && vendor[0] >= 6 && (cpuid(6)[0] & (1u << 6)) != 0;
    m_device = CreateFileW(L"\\\\?\\GLOBALROOT\\Device\\PawnIO", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (m_device == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        const CpuDeviceFailure reason = error == ERROR_ACCESS_DENIED         ? CpuDeviceFailure::AccessDenied
            : error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? CpuDeviceFailure::Missing
                                                                             : CpuDeviceFailure::Other;
        m_failure = cpuSensorFailure(true, reason);
        log.info("PawnIO: " + win32ErrorText(error)
            + "; если устройство отсутствует или нет доступа, используйте «Включить» на экране датчиков");
        return false;
    }
    DWORD version = 0;
    DWORD written = 0;
    if (!DeviceIoControl(m_device, kVersion, nullptr, 0, &version, sizeof(version), &written, nullptr)
        || written != sizeof(version) || version < 0x00020200 || (version >> 16) != 2) {
        log.warning("PawnIO: требуется совместимый драйвер версии 2.2 или новее (ABI 2)");
        shutdown();
        return false;
    }
    if (!loadCpuModule(m_device, m_amd ? IDR_PAWNIO_AMD : IDR_PAWNIO_INTEL)) {
        const DWORD error = GetLastError();
        shutdown();
        log.warning("PawnIO: модуль CPU не загружен: " + win32ErrorText(error));
        return false;
    }
    if (m_amd) {
        // Этот именованный mutex используется мониторами для адресно-данных операций PCI.
        m_pciMutex = CreateMutexW(nullptr, FALSE, L"Global\\Access_PCI");
    }
    log.info(m_amd ? "PawnIO: AMD MSR/SMN подключён (температура Tctl)"
                   : "PawnIO: Intel MSR подключён (package DTS/RAPL)");
    return true;
}

void PawnioSensors::shutdown()
{
    if (m_device != INVALID_HANDLE_VALUE) {
        CloseHandle(m_device);
        m_device = INVALID_HANDLE_VALUE;
    }
    if (m_pciMutex != nullptr) {
        CloseHandle(m_pciMutex);
        m_pciMutex = nullptr;
    }
    m_energy.reset();
}

bool PawnioSensors::readRegister(bool smn, std::uint32_t address, std::uint64_t& value)
{
    struct Request
    {
        char function[32] { };
        std::uint64_t address = 0;
    } request;
    static_assert(sizeof(Request) == 40);
    const char* function = smn ? "ioctl_read_smn" : "ioctl_read_msr";
    std::memcpy(request.function, function, std::strlen(function) + 1);
    request.address = address;
    DWORD written = 0;
    value = 0;
    return DeviceIoControl(
               m_device, kExecute, &request, sizeof(request), &value, sizeof(value), &written, nullptr)
        && written == sizeof(value);
}

std::optional<AmdDdr4MemoryDump> PawnioSensors::readMemoryDump()
{
    if (!m_amd || m_device == INVALID_HANDLE_VALUE || m_pciMutex == nullptr) {
        return std::nullopt;
    }
    const unsigned signature = cpuid(1)[0];
    const unsigned family = ((signature >> 8) & 0xf) == 0xf
        ? ((signature >> 8) & 0xf) + ((signature >> 20) & 0xff)
        : (signature >> 8) & 0xf;
    const unsigned model = ((signature >> 4) & 0xf) | ((signature >> 12) & 0xf0);
    if (family != 0x17 || model != 0x71 || WaitForSingleObject(m_pciMutex, 20) != WAIT_OBJECT_0) {
        return std::nullopt;
    }
    const auto readDword = [this](std::uint32_t address, std::uint32_t& result) {
        std::uint64_t raw = 0;
        if (!readRegister(true, address, raw) || raw > 0xffffffffu) {
            return false;
        }
        result = static_cast<std::uint32_t>(raw);
        return true;
    };
    AmdDdr4MemoryDump dump {};
    bool valid = true;
    for (unsigned channel = 0; channel < dump.size(); ++channel) {
        const std::uint32_t offset = channel << 20;
        auto& data = dump[channel];
        if (!readDword(offset | 0x50100, data.config) || !readDword(offset | 0x50df0, data.maxRate)
            || !readDword(offset | 0x50000, data.dimm0) || !readDword(offset | 0x50008, data.dimm1)) {
            valid = false;
            break;
        }
        if (!isMatisseDdr4Channel(family, model, data.config, data.maxRate, data.dimm0, data.dimm1)) {
            continue;
        }
        if (!readDword(offset | 0x50204, data.timing0) || !readDword(offset | 0x50208, data.timing1)) {
            valid = false;
            break;
        }
        data.hasTimings = true;
    }
    ReleaseMutex(m_pciMutex);
    return valid ? std::optional<AmdDdr4MemoryDump>(dump) : std::nullopt;
}

std::optional<AmdDdr4Timings> PawnioSensors::readMemoryTimings()
{
    const auto dump = readMemoryDump();
    return dump ? decodeAmdDdr4MemoryDump(*dump) : std::nullopt;
}

CpuHardwareReading PawnioSensors::read()
{
    CpuHardwareReading reading;
    if (m_device == INVALID_HANDLE_VALUE) {
        return reading;
    }
    const PackageAffinity affinity;
    if (!affinity.ready()) {
        m_energy.reset();
        return reading;
    }
    std::uint64_t status = 0;
    std::uint64_t target = 0;
    if (!m_amd && m_packageThermal && readRegister(false, 0x1b1, status)
        && readRegister(false, 0x1a2, target)) {
        reading.temperature = intelPackageTemperature(status, target);
    }
    if (m_amd && m_pciMutex != nullptr) {
        const DWORD lock = WaitForSingleObject(m_pciMutex, 10);
        if (lock == WAIT_OBJECT_0 || lock == WAIT_ABANDONED) {
            if (lock == WAIT_OBJECT_0 && readRegister(true, 0x59800, status)) {
                reading.temperature = amdControlTemperature(static_cast<std::uint32_t>(status));
            }
            ReleaseMutex(m_pciMutex);
        }
    }
    std::uint64_t units = 0;
    std::uint64_t energy = 0;
    if (readRegister(false, m_amd ? 0xc0010299u : 0x606u, units)
        && readRegister(false, m_amd ? 0xc001029bu : 0x611u, energy)) {
        reading.powerWatts = m_energy.sample(static_cast<std::uint32_t>(energy),
            static_cast<unsigned>((units >> 8) & 0x1f), static_cast<double>(GetTickCount64()) / 1000.0);
    } else {
        m_energy.reset();
    }
    return reading;
}

} // namespace yawa

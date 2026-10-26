#include "platform/nvml_sensors.h"
#include "metrics/gpu_registry.h"
#include "platform/win_util.h"

#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace yawa {
namespace {

using NvmlDevice = void*;
using NvmlReturn = int;

constexpr NvmlReturn kNvmlSuccess = 0;
constexpr int kNvmlTemperatureGpu = 0;
constexpr int kNvmlClockGraphics = 0;
constexpr int kNvmlClockMemory = 2;
constexpr unsigned kBoundedBufferSize = 96;
// nvmlPcieUtilCounter_t: 0 — переданные байты, 1 — принятые.
constexpr int kNvmlPcieTxBytes = 0;
constexpr int kNvmlPcieRxBytes = 1;

struct NvmlFanSpeedInfo
{
    unsigned version = (1u << 24) | 12u;
    unsigned fan = 0;
    unsigned speed = 0;
};
static_assert(sizeof(NvmlFanSpeedInfo) == 12);

struct NvmlApi {
    HMODULE module = nullptr;

    NvmlReturn (*init)(void) = nullptr;
    NvmlReturn (*shutdown)(void) = nullptr;
    NvmlReturn (*deviceGetCount)(unsigned*) = nullptr;
    NvmlReturn (*deviceGetHandleByIndex)(unsigned, NvmlDevice*) = nullptr;
    NvmlReturn (*deviceGetName)(NvmlDevice, char*, unsigned) = nullptr;
    NvmlReturn (*deviceGetTemperature)(NvmlDevice, int, unsigned*) = nullptr;
    NvmlReturn (*deviceGetClockInfo)(NvmlDevice, int, unsigned*) = nullptr;
    NvmlReturn (*deviceGetPowerUsage)(NvmlDevice, unsigned*) = nullptr;
    NvmlReturn (*deviceGetFanSpeed)(NvmlDevice, unsigned*) = nullptr;
    NvmlReturn (*deviceGetFanSpeedRpm)(NvmlDevice, NvmlFanSpeedInfo*) = nullptr;
    NvmlReturn (*deviceGetEncoderUtilization)(NvmlDevice, unsigned*, unsigned*) = nullptr;
    NvmlReturn (*deviceGetDecoderUtilization)(NvmlDevice, unsigned*, unsigned*) = nullptr;
    NvmlReturn (*deviceGetPcieThroughput)(NvmlDevice, int, unsigned*) = nullptr;
    NvmlReturn (*deviceGetCurrPcieLinkWidth)(NvmlDevice, unsigned*) = nullptr;
    NvmlReturn (*deviceGetCurrPcieLinkGeneration)(NvmlDevice, unsigned*) = nullptr;
};

// nvml.dll лежит в System32 только у новых драйверов; у большинства установок
// он остаётся в папке NVSMI, поэтому ищем по известным местам, а не только в PATH.
HMODULE loadNvmlLibrary()
{
    if (HMODULE module = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        module != nullptr) {
        return module;
    }
    PWSTR root = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &root))) {
        const std::wstring path = std::wstring(root) + L"\\NVIDIA Corporation\\NVSMI\\nvml.dll";
        CoTaskMemFree(root);
        return LoadLibraryExW(
            path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    return nullptr;
}

template <typename T>
bool loadSymbol(HMODULE module, const char* name, T& target)
{
    target = reinterpret_cast<T>(GetProcAddress(module, name));
    return target != nullptr;
}

} // namespace

struct NvmlSensors::Impl {
    NvmlApi api;
    NvmlDevice device = nullptr;
    unsigned deviceCount = 0;
    bool started = false;
};

NvmlSensors::~NvmlSensors()
{
    shutdown();
}

bool NvmlSensors::initialize(std::string& error)
{
    shutdown();

    HMODULE module = loadNvmlLibrary();
    if (module == nullptr) {
        error = "nvml.dll не найден: драйвер NVIDIA не установлен";
        return false;
    }

    m_impl = new Impl();
    m_impl->api.module = module;

    const bool ok = loadSymbol(module, "nvmlInit_v2", m_impl->api.init)
        && loadSymbol(module, "nvmlShutdown", m_impl->api.shutdown)
        && loadSymbol(module, "nvmlDeviceGetCount_v2", m_impl->api.deviceGetCount)
        && loadSymbol(module, "nvmlDeviceGetHandleByIndex_v2", m_impl->api.deviceGetHandleByIndex)
        && loadSymbol(module, "nvmlDeviceGetName", m_impl->api.deviceGetName);
    loadSymbol(module, "nvmlDeviceGetTemperature", m_impl->api.deviceGetTemperature);
    loadSymbol(module, "nvmlDeviceGetClockInfo", m_impl->api.deviceGetClockInfo);
    loadSymbol(module, "nvmlDeviceGetPowerUsage", m_impl->api.deviceGetPowerUsage);
    loadSymbol(module, "nvmlDeviceGetFanSpeed", m_impl->api.deviceGetFanSpeed);

    // Версия RPM-вентилятора появилась в NVML 12: на старых драйверах её просто нет.
    loadSymbol(module, "nvmlDeviceGetFanSpeedRPM", m_impl->api.deviceGetFanSpeedRpm);
    // Всё ниже есть только у относительно новых драйверов: символы необязательные.
    loadSymbol(module, "nvmlDeviceGetEncoderUtilization", m_impl->api.deviceGetEncoderUtilization);
    loadSymbol(module, "nvmlDeviceGetDecoderUtilization", m_impl->api.deviceGetDecoderUtilization);
    loadSymbol(module, "nvmlDeviceGetPcieThroughput", m_impl->api.deviceGetPcieThroughput);
    loadSymbol(module, "nvmlDeviceGetCurrPcieLinkWidth", m_impl->api.deviceGetCurrPcieLinkWidth);
    loadSymbol(module, "nvmlDeviceGetCurrPcieLinkGeneration", m_impl->api.deviceGetCurrPcieLinkGeneration);

    if (!ok) {
        error = "nvml.dll загружен, но не совпадает по версии API";
        shutdown();
        return false;
    }

    if (m_impl->api.init() != kNvmlSuccess) {
        error = "nvmlInit не удался: нет доступа к драйверу NVIDIA";
        shutdown();
        return false;
    }

    m_impl->started = true;
    if (m_impl->api.deviceGetCount(&m_impl->deviceCount) != kNvmlSuccess || m_impl->deviceCount == 0
        || m_impl->deviceCount > 128) {
        error = "NVML не нашёл ни одного устройства NVIDIA";
        shutdown();
        return false;
    }

    m_impl->device = nullptr;
    m_initialized = true;
    return true;
}

void NvmlSensors::shutdown()
{
    if (m_impl != nullptr) {
        if (m_impl->started && m_impl->api.shutdown != nullptr) {
            m_impl->api.shutdown();
        }
        if (m_impl->api.module != nullptr) {
            FreeLibrary(m_impl->api.module);
        }
        delete m_impl;
        m_impl = nullptr;
    }
    m_initialized = false;
}

void NvmlSensors::selectDevice(const std::wstring& nameHint)
{
    if (!m_initialized || m_impl == nullptr) {
        return;
    }

    m_impl->device = nullptr;
    if (nameHint.empty()) {
        return;
    }
    std::vector<std::wstring> names(m_impl->deviceCount);
    std::vector<NvmlDevice> devices(m_impl->deviceCount, nullptr);
    for (unsigned index = 0; index < m_impl->deviceCount; ++index) {
        if (m_impl->api.deviceGetHandleByIndex(index, &devices[index]) != kNvmlSuccess || !devices[index]) {
            continue;
        }
        char name[kBoundedBufferSize]{};
        if (m_impl->api.deviceGetName(devices[index], name, kBoundedBufferSize) == kNvmlSuccess) {
            name[kBoundedBufferSize - 1] = 0;
            names[index] = widen(name);
        }
    }
    const auto selected = uniqueGpuNameMatch(nameHint, names);
    if (selected) {
        m_impl->device = devices[*selected];
    }
}

GpuSensorReading NvmlSensors::read() const
{
    GpuSensorReading reading;
    if (!m_initialized || m_impl == nullptr || m_impl->device == nullptr) {
        return reading;
    }

    const NvmlApi& api = m_impl->api;
    unsigned value = 0;

    if (api.deviceGetTemperature != nullptr
        && api.deviceGetTemperature(m_impl->device, kNvmlTemperatureGpu, &value) == kNvmlSuccess) {
        reading.temperature = static_cast<double>(value);
        reading.available = true;
    }
    if (api.deviceGetClockInfo != nullptr
        && api.deviceGetClockInfo(m_impl->device, kNvmlClockGraphics, &value) == kNvmlSuccess) {
        reading.graphicsClockMhz = static_cast<double>(value);
    }
    if (api.deviceGetClockInfo != nullptr
        && api.deviceGetClockInfo(m_impl->device, kNvmlClockMemory, &value) == kNvmlSuccess) {
        reading.memoryClockMhz = static_cast<double>(value);
    }
    if (api.deviceGetPowerUsage != nullptr
        && api.deviceGetPowerUsage(m_impl->device, &value) == kNvmlSuccess) {
        reading.powerWatts = static_cast<double>(value) / 1000.0;
    }
    // Занятость медиа-движков и пропускная способность PCI-e.
    unsigned samplingPeriod = 0;
    if (api.deviceGetEncoderUtilization != nullptr
        && api.deviceGetEncoderUtilization(m_impl->device, &value, &samplingPeriod) == kNvmlSuccess) {
        reading.encoderPercent = static_cast<double>(value);
    }
    if (api.deviceGetDecoderUtilization != nullptr
        && api.deviceGetDecoderUtilization(m_impl->device, &value, &samplingPeriod) == kNvmlSuccess) {
        reading.decoderPercent = static_cast<double>(value);
    }
    if (api.deviceGetPcieThroughput != nullptr) {
        unsigned throughput = 0;
        if (api.deviceGetPcieThroughput(m_impl->device, kNvmlPcieTxBytes, &throughput) == kNvmlSuccess) {
            reading.pcieTxKilobytesPerSecond = static_cast<double>(throughput);
            reading.hasPcie = true;
        }
        if (api.deviceGetPcieThroughput(m_impl->device, kNvmlPcieRxBytes, &throughput) == kNvmlSuccess) {
            reading.pcieRxKilobytesPerSecond = static_cast<double>(throughput);
            reading.hasPcie = true;
        }
    }
    unsigned link = 0;
    if (api.deviceGetCurrPcieLinkWidth != nullptr
        && api.deviceGetCurrPcieLinkWidth(m_impl->device, &link) == kNvmlSuccess) {
        reading.pcieLinkWidth = link;
    }
    if (api.deviceGetCurrPcieLinkGeneration != nullptr
        && api.deviceGetCurrPcieLinkGeneration(m_impl->device, &link) == kNvmlSuccess) {
        reading.pcieLinkGeneration = link;
    }

    NvmlFanSpeedInfo fan;
    if (api.deviceGetFanSpeedRpm != nullptr
        && api.deviceGetFanSpeedRpm(m_impl->device, &fan) == kNvmlSuccess) {
        reading.fanRpm = fan.speed;
    } else if (api.deviceGetFanSpeed != nullptr
        && api.deviceGetFanSpeed(m_impl->device, &value) == kNvmlSuccess) {
        // Старый API умеет только проценты: показываем их как 0 об/мин, чтобы не врать числом.
        reading.fanRpm = 0;
    }

    reading.available = reading.available || reading.graphicsClockMhz > 0 || reading.powerWatts > 0;
    return reading;
}

} // namespace yawa

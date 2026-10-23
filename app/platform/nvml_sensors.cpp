#include "platform/nvml_sensors.h"
#include "platform/win_util.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace yawa {
namespace {

using NvmlDevice = void*;
using NvmlReturn = int;

constexpr NvmlReturn kNvmlSuccess = 0;
constexpr int kNvmlTemperatureGpu = 0;
constexpr int kNvmlClockGraphics = 0;
constexpr int kNvmlClockMemory = 2;
constexpr unsigned kBoundedBufferSize = 96;

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
    NvmlReturn (*deviceGetFanSpeedRpm)(NvmlDevice, unsigned*) = nullptr;
};

std::wstring lowercase(std::string_view text)
{
    std::wstring result;
    result.reserve(text.size());
    for (const char symbol : text) {
        result.push_back(static_cast<wchar_t>(std::tolower(static_cast<unsigned char>(symbol))));
    }
    return result;
}

// nvml.dll лежит в System32 только у новых драйверов; у большинства установок
// он остаётся в папке NVSMI, поэтому ищем по известным местам, а не только в PATH.
HMODULE loadNvmlLibrary()
{
    if (HMODULE module = LoadLibraryW(L"nvml.dll"); module != nullptr) {
        return module;
    }

    wchar_t buffer[MAX_PATH]{};
    if (GetSystemDirectoryW(buffer, MAX_PATH) != 0) {
        const std::wstring path = std::wstring(buffer) + L"\\nvml.dll";
        if (HMODULE module = LoadLibraryW(path.c_str()); module != nullptr) {
            return module;
        }
    }

    const wchar_t* roots[] = { L"ProgramW6432", L"ProgramFiles" };
    for (const wchar_t* root : roots) {
        if (GetEnvironmentVariableW(root, buffer, MAX_PATH) == 0) {
            continue;
        }
        const std::wstring path = std::wstring(buffer) + L"\\NVIDIA Corporation\\NVSMI\\nvml.dll";
        if (HMODULE module = LoadLibraryW(path.c_str()); module != nullptr) {
            return module;
        }
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

    bool ok = loadSymbol(module, "nvmlInit_v2", m_impl->api.init)
        && loadSymbol(module, "nvmlShutdown", m_impl->api.shutdown)
        && loadSymbol(module, "nvmlDeviceGetCount_v2", m_impl->api.deviceGetCount)
        && loadSymbol(module, "nvmlDeviceGetHandleByIndex_v2", m_impl->api.deviceGetHandleByIndex)
        && loadSymbol(module, "nvmlDeviceGetName", m_impl->api.deviceGetName)
        && loadSymbol(module, "nvmlDeviceGetTemperature", m_impl->api.deviceGetTemperature)
        && loadSymbol(module, "nvmlDeviceGetClockInfo", m_impl->api.deviceGetClockInfo)
        && loadSymbol(module, "nvmlDeviceGetPowerUsage", m_impl->api.deviceGetPowerUsage)
        && loadSymbol(module, "nvmlDeviceGetFanSpeed", m_impl->api.deviceGetFanSpeed);

    // Версия RPM-вентилятора появилась в NVML 12: на старых драйверах её просто нет.
    loadSymbol(module, "nvmlDeviceGetFanSpeedRPM", m_impl->api.deviceGetFanSpeedRpm);

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

    if (m_impl->api.deviceGetCount(&m_impl->deviceCount) != kNvmlSuccess || m_impl->deviceCount == 0) {
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
        if (m_initialized && m_impl->api.shutdown != nullptr) {
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

    const std::wstring hint = lowercase(narrow(nameHint));
    for (unsigned index = 0; index < m_impl->deviceCount; ++index) {
        NvmlDevice device = nullptr;
        if (m_impl->api.deviceGetHandleByIndex(index, &device) != kNvmlSuccess || device == nullptr) {
            continue;
        }

        char name[kBoundedBufferSize]{};
        if (m_impl->api.deviceGetName(device, name, kBoundedBufferSize) != kNvmlSuccess) {
            continue;
        }

        const std::wstring deviceName = lowercase(name);
        if (!hint.empty() && (deviceName.find(hint) != std::wstring::npos
                                 || hint.find(deviceName) != std::wstring::npos)) {
            m_impl->device = device;
            return;
        }
    }

    if (m_impl->device == nullptr) {
        m_impl->api.deviceGetHandleByIndex(0, &m_impl->device);
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

    if (api.deviceGetTemperature(m_impl->device, kNvmlTemperatureGpu, &value) == kNvmlSuccess) {
        reading.temperature = static_cast<double>(value);
        reading.available = true;
    }
    if (api.deviceGetClockInfo(m_impl->device, kNvmlClockGraphics, &value) == kNvmlSuccess) {
        reading.graphicsClockMhz = static_cast<double>(value);
    }
    if (api.deviceGetClockInfo(m_impl->device, kNvmlClockMemory, &value) == kNvmlSuccess) {
        reading.memoryClockMhz = static_cast<double>(value);
    }
    if (api.deviceGetPowerUsage(m_impl->device, &value) == kNvmlSuccess) {
        reading.powerWatts = static_cast<double>(value) / 1000.0;
    }
    if (api.deviceGetFanSpeedRpm != nullptr
        && api.deviceGetFanSpeedRpm(m_impl->device, &value) == kNvmlSuccess) {
        reading.fanRpm = value;
    } else if (api.deviceGetFanSpeed != nullptr
        && api.deviceGetFanSpeed(m_impl->device, &value) == kNvmlSuccess) {
        // Старый API умеет только проценты: показываем их как 0 об/мин, чтобы не врать числом.
        reading.fanRpm = 0;
    }

    return reading;
}

} // namespace yawa

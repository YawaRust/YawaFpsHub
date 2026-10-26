#include "platform/vendor_gpu_sensors.h"
#include "metrics/hardware_telemetry.h"
#include "platform/log.h"
#include "platform/win_util.h"

#include <windows.h>

#include <adl_sdk.h>
#include <igcl_api.h>

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <vector>

namespace yawa {
namespace {

    template <typename T> T symbol(HMODULE module, const char* name)
    {
        return reinterpret_cast<T>(GetProcAddress(module, name));
    }

    void* __stdcall allocateAdl(int size)
    {
        return size > 0 ? std::malloc(static_cast<std::size_t>(size)) : nullptr;
    }

    bool displayMatches(const GpuAdapter& adapter, const char* display)
    {
        UINT32 paths = 0;
        UINT32 modes = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS || paths > 512
            || modes > 4096) {
            return false;
        }
        std::vector<DISPLAYCONFIG_PATH_INFO> pathInfo(paths);
        std::vector<DISPLAYCONFIG_MODE_INFO> modeInfo(modes);
        if (QueryDisplayConfig(
                QDC_ONLY_ACTIVE_PATHS, &paths, pathInfo.data(), &modes, modeInfo.data(), nullptr)
            != ERROR_SUCCESS) {
            return false;
        }
        const std::wstring name = widen(display);
        for (UINT32 i = 0; i < paths; ++i) {
            const auto& source = pathInfo[i].sourceInfo;
            if (source.adapterId.LowPart != adapter.luidLow
                || source.adapterId.HighPart != adapter.luidHigh) {
                continue;
            }
            DISPLAYCONFIG_SOURCE_DEVICE_NAME info { };
            info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            info.header.size = sizeof(info);
            info.header.adapterId = source.adapterId;
            info.header.id = source.id;
            if (DisplayConfigGetDeviceInfo(&info.header) == ERROR_SUCCESS
                && _wcsicmp(info.viewGdiDeviceName, name.c_str()) == 0) {
                return true;
            }
        }
        return false;
    }

    std::optional<double> telemetry(const ctl_oc_telemetry_item_t& item, ctl_units_t units)
    {
        if (!item.bSupported || item.units != units) {
            return std::nullopt;
        }
        double value = 0;
        switch (item.type) {
        case CTL_DATA_TYPE_INT8:
            value = item.value.data8;
            break;
        case CTL_DATA_TYPE_UINT8:
            value = item.value.datau8;
            break;
        case CTL_DATA_TYPE_INT16:
            value = item.value.data16;
            break;
        case CTL_DATA_TYPE_UINT16:
            value = item.value.datau16;
            break;
        case CTL_DATA_TYPE_DOUBLE:
            value = item.value.datadouble;
            break;
        case CTL_DATA_TYPE_FLOAT:
            value = item.value.datafloat;
            break;
        case CTL_DATA_TYPE_INT32:
            value = item.value.data32;
            break;
        case CTL_DATA_TYPE_UINT32:
            value = item.value.datau32;
            break;
        case CTL_DATA_TYPE_INT64:
            value = static_cast<double>(item.value.data64);
            break;
        case CTL_DATA_TYPE_UINT64:
            value = static_cast<double>(item.value.datau64);
            break;
        default:
            return std::nullopt;
        }
        return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
    }

    double bounded(double value, double maximum)
    {
        return std::isfinite(value) && value >= 0 && value <= maximum ? value : 0;
    }

} // namespace

struct VendorGpuSensors::Impl
{
    HMODULE adl = nullptr;
    ADL_CONTEXT_HANDLE context = nullptr;
    int (*destroyAdl)(ADL_CONTEXT_HANDLE) = nullptr;
    int (*countAdl)(ADL_CONTEXT_HANDLE, int*) = nullptr;
    int (*infoAdl)(ADL_CONTEXT_HANDLE, AdapterInfo*, int) = nullptr;
    int (*pmLog)(ADL_CONTEXT_HANDLE, int, ADLPMLogDataOutput*) = nullptr;
    int (*temperatureAdl)(ADL_CONTEXT_HANDLE, int, int, ADLTemperature*) = nullptr;
    int amdIndex = -1;
    HMODULE igcl = nullptr;
    ctl_api_handle_t api = nullptr;
    ctl_device_adapter_handle_t intelDevice = nullptr;
    ctl_pfnClose_t closeIntel = nullptr;
    ctl_pfnEnumerateDevices_t enumerateIntel = nullptr;
    ctl_pfnGetDeviceProperties_t infoIntel = nullptr;
    ctl_pfnPowerTelemetryGet_t readIntel = nullptr;
    ContinuousEnergyCounter energy;

    GpuSensorReading readAmd() const;
    GpuSensorReading readIgcl();
};

VendorGpuSensors::~VendorGpuSensors()
{
    shutdown();
}

void VendorGpuSensors::initialize(const GpuRegistry& registry, Log& log)
{
    shutdown();
    m_impl = new Impl();
    bool amd = false;
    bool intel = false;
    for (const auto& adapter : registry.adapters()) {
        amd |= adapter.vendor == GpuVendor::Amd;
        intel |= adapter.vendor == GpuVendor::Intel;
    }
    if (amd) {
        m_impl->adl = LoadLibraryExW(L"atiadlxx.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (m_impl->adl != nullptr) {
            using Create = int (*)(ADL_MAIN_MALLOC_CALLBACK, int, ADL_CONTEXT_HANDLE*);
            const auto create = symbol<Create>(m_impl->adl, "ADL2_Main_Control_Create");
            m_impl->destroyAdl
                = symbol<decltype(m_impl->destroyAdl)>(m_impl->adl, "ADL2_Main_Control_Destroy");
            m_impl->countAdl
                = symbol<decltype(m_impl->countAdl)>(m_impl->adl, "ADL2_Adapter_NumberOfAdapters_Get");
            m_impl->infoAdl = symbol<decltype(m_impl->infoAdl)>(m_impl->adl, "ADL2_Adapter_AdapterInfo_Get");
            m_impl->pmLog = symbol<decltype(m_impl->pmLog)>(m_impl->adl, "ADL2_New_QueryPMLogData_Get");
            m_impl->temperatureAdl
                = symbol<decltype(m_impl->temperatureAdl)>(m_impl->adl, "ADL2_Overdrive5_Temperature_Get");
            ADL_CONTEXT_HANDLE context = nullptr;
            if (create && m_impl->destroyAdl && m_impl->countAdl && m_impl->infoAdl
                && (m_impl->pmLog || m_impl->temperatureAdl) && create(allocateAdl, 0, &context) == ADL_OK
                && context) {
                m_impl->context = context;
                log.info("GPU: AMD ADL подключён (PMLog / temperature fallback)");
            }
        }
    }
    if (amd && !m_impl->context) {
        log.info("GPU: AMD ADL недоступна; остаются DXGI/PDH");
    }
    if (intel) {
        m_impl->igcl = LoadLibraryExW(L"ControlLib.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (m_impl->igcl != nullptr) {
            const auto init = symbol<ctl_pfnInit_t>(m_impl->igcl, "ctlInit");
            m_impl->closeIntel = symbol<ctl_pfnClose_t>(m_impl->igcl, "ctlClose");
            m_impl->enumerateIntel = symbol<ctl_pfnEnumerateDevices_t>(m_impl->igcl, "ctlEnumerateDevices");
            m_impl->infoIntel = symbol<ctl_pfnGetDeviceProperties_t>(m_impl->igcl, "ctlGetDeviceProperties");
            m_impl->readIntel = symbol<ctl_pfnPowerTelemetryGet_t>(m_impl->igcl, "ctlPowerTelemetryGet");
            ctl_init_args_t args { };
            args.Size = sizeof(args);
            args.AppVersion = CTL_MAKE_VERSION(1, 0);
            args.flags = CTL_INIT_FLAG_USE_LEVEL_ZERO;
            ctl_api_handle_t api = nullptr;
            if (init && m_impl->closeIntel && m_impl->enumerateIntel && m_impl->infoIntel && m_impl->readIntel
                && init(&args, &api) == CTL_RESULT_SUCCESS && api) {
                m_impl->api = api;
                log.info("GPU: Intel Control Library подключена (telemetry v0)");
            }
        }
    }
    if (intel && !m_impl->api) {
        log.info("GPU: Intel Control Library недоступна; остаются DXGI/PDH");
    }
}

void VendorGpuSensors::shutdown()
{
    if (m_impl == nullptr) {
        return;
    }
    if (m_impl->context && m_impl->destroyAdl) {
        m_impl->destroyAdl(m_impl->context);
    }
    if (m_impl->api && m_impl->closeIntel) {
        m_impl->closeIntel(m_impl->api);
    }
    if (m_impl->adl) {
        FreeLibrary(m_impl->adl);
    }
    if (m_impl->igcl) {
        FreeLibrary(m_impl->igcl);
    }
    delete m_impl;
    m_impl = nullptr;
}

void VendorGpuSensors::selectDevice(const GpuAdapter* adapter, bool singleVendor)
{
    if (m_impl == nullptr) {
        return;
    }
    m_impl->amdIndex = -1;
    m_impl->intelDevice = nullptr;
    m_impl->energy.reset();
    if (adapter == nullptr) {
        return;
    }
    if (adapter->vendor == GpuVendor::Amd && m_impl->context) {
        int count = 0;
        if (m_impl->countAdl(m_impl->context, &count) != ADL_OK || count <= 0 || count > 128) {
            return;
        }
        std::vector<AdapterInfo> adapters(static_cast<std::size_t>(count));
        for (auto& item : adapters) {
            item.iSize = sizeof(AdapterInfo);
        }
        if (m_impl->infoAdl(
                m_impl->context, adapters.data(), static_cast<int>(adapters.size() * sizeof(AdapterInfo)))
            != ADL_OK) {
            return;
        }
        const AdapterInfo* selected = nullptr;
        for (auto& item : adapters) {
            item.strDisplayName[ADL_MAX_PATH - 1] = 0;
            item.strAdapterName[ADL_MAX_PATH - 1] = 0;
            if (!item.iExist) {
                continue;
            }
            const bool matches = displayMatches(*adapter, item.strDisplayName)
                || (singleVendor && _wcsicmp(adapter->name.c_str(), widen(item.strAdapterName).c_str()) == 0);
            if (!matches) {
                continue;
            }
            if (selected
                && (selected->iBusNumber != item.iBusNumber || selected->iDeviceNumber != item.iDeviceNumber
                    || selected->iFunctionNumber != item.iFunctionNumber)) {
                return;
            }
            selected = &item;
        }
        if (selected) {
            m_impl->amdIndex = selected->iAdapterIndex;
        }
    }
    if (adapter->vendor == GpuVendor::Intel && m_impl->api) {
        uint32_t count = 0;
        if (m_impl->enumerateIntel(m_impl->api, &count, nullptr) != CTL_RESULT_SUCCESS || count == 0
            || count > 128) {
            return;
        }
        std::vector<ctl_device_adapter_handle_t> devices(count);
        if (m_impl->enumerateIntel(m_impl->api, &count, devices.data()) != CTL_RESULT_SUCCESS
            || count > devices.size()) {
            return;
        }
        for (uint32_t i = 0; i < count; ++i) {
            LUID luid { };
            ctl_device_adapter_properties_t properties { };
            properties.Size = sizeof(properties);
            properties.pDeviceID = &luid;
            properties.device_id_size = sizeof(luid);
            if (m_impl->infoIntel(devices[i], &properties) == CTL_RESULT_SUCCESS
                && properties.pci_vendor_id == 0x8086 && luid.LowPart == adapter->luidLow
                && luid.HighPart == adapter->luidHigh) {
                m_impl->intelDevice = devices[i];
                return;
            }
        }
    }
}

GpuSensorReading VendorGpuSensors::Impl::readAmd() const
{
    GpuSensorReading result;
    if (amdIndex < 0) {
        return result;
    }
    ADLPMLogDataOutput data { };
    data.size = sizeof(data);
    if (pmLog && pmLog(context, amdIndex, &data) == ADL_OK) {
        const auto value = [&data](unsigned id, double limit) {
            return data.sensors[id].supported ? bounded(data.sensors[id].value, limit) : 0.0;
        };
        result.temperature = value(ADL_PMLOG_TEMPERATURE_EDGE, 150);
        result.hotspot = value(ADL_PMLOG_TEMPERATURE_HOTSPOT, 150);
        result.graphicsClockMhz = value(ADL_PMLOG_CLK_GFXCLK, 10000);
        result.memoryClockMhz = value(ADL_PMLOG_CLK_MEMCLK, 20000);
        result.powerWatts = value(ADL_PMLOG_ASIC_POWER, 2000);
        result.fanRpm = static_cast<std::uint32_t>(value(ADL_PMLOG_FAN_RPM, 30000));
    }
    if (result.temperature <= 0 && temperatureAdl) {
        ADLTemperature temperature { };
        temperature.iSize = sizeof(temperature);
        if (temperatureAdl(context, amdIndex, 0, &temperature) == ADL_OK) {
            result.temperature = bounded(temperature.iTemperature / 1000.0, 150);
        }
    }
    result.available = result.temperature > 0 || result.graphicsClockMhz > 0 || result.powerWatts > 0;
    return result;
}

GpuSensorReading VendorGpuSensors::Impl::readIgcl()
{
    GpuSensorReading result;
    if (!intelDevice) {
        return result;
    }
    ctl_power_telemetry_t data { };
    data.Size = sizeof(data);
    if (readIntel(intelDevice, &data) != CTL_RESULT_SUCCESS) {
        energy.reset();
        return result;
    }
    result.temperature
        = bounded(telemetry(data.gpuCurrentTemperature, CTL_UNITS_TEMPERATURE_CELSIUS).value_or(0), 150);
    result.graphicsClockMhz
        = bounded(telemetry(data.gpuCurrentClockFrequency, CTL_UNITS_FREQUENCY_MHZ).value_or(0), 10000);
    result.memoryClockMhz
        = bounded(telemetry(data.vramCurrentClockFrequency, CTL_UNITS_FREQUENCY_MHZ).value_or(0), 20000);
    for (const auto& fan : data.fanSpeed) {
        result.fanRpm = std::max(result.fanRpm,
            static_cast<std::uint32_t>(
                bounded(telemetry(fan, CTL_UNITS_ANGULAR_SPEED_RPM).value_or(0), 30000)));
    }
    const auto joules = telemetry(data.gpuEnergyCounter, CTL_UNITS_ENERGY_JOULES);
    const auto seconds = telemetry(data.timeStamp, CTL_UNITS_TIME_SECONDS);
    if (joules && seconds) {
        result.powerWatts = energy.sample(*joules, *seconds).value_or(0);
    } else {
        energy.reset();
    }
    result.available = result.temperature > 0 || result.graphicsClockMhz > 0 || result.powerWatts > 0;
    return result;
}

GpuSensorReading VendorGpuSensors::read()
{
    if (!m_impl) {
        return { };
    }
    return m_impl->intelDevice ? m_impl->readIgcl() : m_impl->readAmd();
}

} // namespace yawa

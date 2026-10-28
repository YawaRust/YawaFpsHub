// Сбор сведений о машине: модель CPU, адаптеры DXGI, модули памяти из SMBIOS,
// монитор и версия Windows. Источники — только встроенные в Windows: реестр, CPUID,
// GetSystemFirmwareTable, DXGI и EnumDisplaySettings. Драйверов, WMI-провайдеров
// производителей и сети приложение не требует.
#include "platform/hardware_info_win.h"
#include "metrics/gpu_registry.h"
#include "metrics/smbios_memory.h"
#include "platform/win_util.h"

#include <windows.h>
#include <wbemidl.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace yawa {
namespace {

std::wstring registryString(HKEY root, const wchar_t* key, const wchar_t* value)
{
    HKEY opened = nullptr;
    if (RegOpenKeyExW(root, key, 0, KEY_READ | KEY_WOW64_64KEY, &opened) != ERROR_SUCCESS) {
        return {};
    }

    std::wstring result;
    DWORD type = 0;
    std::array<wchar_t, 256> buffer{};
    DWORD size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    if (RegQueryValueExW(opened, value, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &size) == ERROR_SUCCESS
        && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        const std::size_t characters = size / sizeof(wchar_t);
        result.assign(buffer.data(),
            characters > 0 && buffer[characters - 1] == L'\0' ? characters - 1 : characters);
    }
    RegCloseKey(opened);
    return result;
}

DWORD registryDword(HKEY root, const wchar_t* key, const wchar_t* value, DWORD fallback = 0)
{
    HKEY opened = nullptr;
    if (RegOpenKeyExW(root, key, 0, KEY_READ | KEY_WOW64_64KEY, &opened) != ERROR_SUCCESS) {
        return fallback;
    }

    DWORD result = fallback;
    DWORD type = 0;
    DWORD size = sizeof(result);
    if (RegQueryValueExW(opened, value, nullptr, &type, reinterpret_cast<BYTE*>(&result), &size) != ERROR_SUCCESS
        || (type != REG_DWORD && type != REG_QWORD)) {
        result = fallback;
    }
    RegCloseKey(opened);
    return result;
}

void collectOperatingSystem(HardwareIdentity& info)
{
    constexpr const wchar_t* kKey = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::wstring product = registryString(HKEY_LOCAL_MACHINE, kKey, L"ProductName");
    const std::wstring build = registryString(HKEY_LOCAL_MACHINE, kKey, L"CurrentBuildNumber");
    const DWORD buildNumber = build.empty() ? 0 : static_cast<DWORD>(std::wcstol(build.c_str(), nullptr, 10));

    // Реестр на Windows 11 по-прежнему пишет «Windows 10»: сам winver подменяет строку
    // по номеру сборки. Делаем так же, чтобы карточка совпадала с окном «О системе».
    if (buildNumber >= 22000 && product.rfind(L"Windows 10", 0) == 0) {
        product.replace(0, 10, L"Windows 11");
    }
    if (product.empty()) {
        product = L"Windows";
    }

    info.os.product = toUtf8(product);
    info.os.edition = toUtf8(registryString(HKEY_LOCAL_MACHINE, kKey, L"EditionID"));
    info.os.featureVersion = toUtf8(registryString(HKEY_LOCAL_MACHINE, kKey, L"DisplayVersion"));
    info.os.isServer = registryString(HKEY_LOCAL_MACHINE, kKey, L"InstallationType") == L"Server";

    const DWORD ubr = registryDword(HKEY_LOCAL_MACHINE, kKey, L"UBR", 0);
    wchar_t buffer[64]{};
    if (ubr > 0) {
        swprintf_s(buffer, L"%s.%lu", build.c_str(), static_cast<unsigned long>(ubr));
    } else {
        swprintf_s(buffer, L"%s", build.c_str());
    }
    info.os.build = toUtf8(buffer);

    SYSTEM_INFO systemInfo{};
    GetNativeSystemInfo(&systemInfo);
    info.os.architecture = systemInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64
        ? "x64"
        : (systemInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "ARM64" : "x86");
}

std::string cpuBrand()
{
    std::array<int, 4> limits{};
    __cpuid(limits.data(), 0x80000000);
    if (static_cast<unsigned int>(limits[0]) < 0x80000004u) {
        return {};
    }

    char brand[49]{};
    for (int leaf = 0; leaf < 3; ++leaf) {
        std::array<int, 4> registers{};
        __cpuid(registers.data(), 0x80000002 + leaf);
        std::memcpy(brand + leaf * 16, registers.data(), 16);
    }

    std::string name(brand);
    while (!name.empty() && name.back() == ' ') {
        name.pop_back();
    }
    return name;
}

std::string cpuVendor()
{
    std::array<int, 4> registers{};
    __cpuid(registers.data(), 0);

    char vendor[13]{};
    std::memcpy(vendor, &registers[1], 4);
    std::memcpy(vendor + 4, &registers[3], 4);
    std::memcpy(vendor + 8, &registers[2], 4);
    return std::string(vendor);
}

void collectCpu(HardwareIdentity& info)
{
    info.cpu.name = cpuBrand();
    if (info.cpu.name.empty()) {
        info.cpu.name = toUtf8(registryString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            L"ProcessorNameString"));
    }
    info.cpu.vendor = cpuVendor();
    info.cpu.baseMhz = static_cast<double>(registryDword(HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz", 0));

    // GetLogicalProcessorInformationEx даёт и физическое число ядер, и кэши: ни реестр,
    // ни WMI для этого не нужны.
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationAll, nullptr, &length);
    std::vector<BYTE> storage(length == 0 ? 1 : length);
    if (length == 0
        || !GetLogicalProcessorInformationEx(RelationAll,
               reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(storage.data()), &length)) {
        info.cpu.logicalProcessors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        info.cpu.smt = info.cpu.logicalProcessors > 1;
        return;
    }

    std::uint32_t physical = 0;
    std::uint32_t logical = 0;
    std::uint32_t l3Mb = 0;
    const BYTE* cursor = storage.data();
    const BYTE* const end = storage.data() + length;
    while (cursor + sizeof(DWORD) <= end) {
        const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(cursor);
        if (entry->Relationship == RelationProcessorCore) {
            ++physical;
            for (USHORT group = 0; group < entry->Processor.GroupCount; ++group) {
                const KAFFINITY mask = entry->Processor.GroupMask[group].Mask;
                for (USHORT bit = 0; bit < 64; ++bit) {
                    if ((mask & (KAFFINITY(1) << bit)) != 0) {
                        ++logical;
                    }
                }
            }
        } else if (entry->Relationship == RelationCache && entry->Cache.Level == 3) {
            // Кэш L3 общий для группы ядер: суммировать нельзя, берём максимальный.
            const std::uint32_t size = static_cast<std::uint32_t>(entry->Cache.CacheSize / (1024ull * 1024ull));
            l3Mb = std::max(l3Mb, size);
        }

        // Каждая запись буфера несёт собственный размер в поле Size.
        cursor += entry->Size == 0 ? sizeof(*entry) : entry->Size;
    }

    info.cpu.physicalCores = physical;
    info.cpu.logicalProcessors = logical > 0 ? logical : GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    info.cpu.smt = physical > 0 && info.cpu.logicalProcessors > physical;
    info.cpu.l3CacheMb = l3Mb;
}

void collectSmbiosMemory(HardwareIdentity& info)
{
    constexpr DWORD kSmbios = 0x52534D42;
    const DWORD needed = GetSystemFirmwareTable(kSmbios, 0, nullptr, 0);
    if (needed <= 8 || needed > 16u * 1024u * 1024u) {
        return;
    }
    std::vector<BYTE> table(needed);
    if (GetSystemFirmwareTable(kSmbios, 0, table.data(), needed) != needed) {
        return;
    }
    info.memory = parseSmbiosMemory(table, info.memory.totalGb);
}

void collectWmiMemory(HardwareIdentity& info)
{
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        return;
    }
    IWbemLocator* locator = nullptr;
    IWbemServices* services = nullptr;
    IEnumWbemClassObject* enumerator = nullptr;
    IEnumWbemClassObject* slotsEnumerator = nullptr;
    std::uint32_t wmiSlots = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator)))
        && locator != nullptr) {
        BSTR ns = SysAllocString(L"ROOT\\CIMV2");
        if (ns != nullptr) {
            locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
            SysFreeString(ns);
        }
        locator->Release();
    }
    if (services != nullptr) {
        CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
            RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        BSTR lang = SysAllocString(L"WQL");
        BSTR query = SysAllocString(L"SELECT DeviceLocator, Capacity, Speed, ConfiguredClockSpeed, "
                                    L"Manufacturer, PartNumber, SMBIOSMemoryType FROM Win32_PhysicalMemory");
        if (lang != nullptr && query != nullptr) {
            services->ExecQuery(lang, query, WBEM_FLAG_FORWARD_ONLY, nullptr, &enumerator);
        }
        if (lang) {
            SysFreeString(lang);
        }
        if (query) {
            SysFreeString(query);
        }
        lang = SysAllocString(L"WQL");
        query = SysAllocString(L"SELECT MemoryDevices, Use FROM Win32_PhysicalMemoryArray");
        if (lang != nullptr && query != nullptr) {
            services->ExecQuery(lang, query, WBEM_FLAG_FORWARD_ONLY, nullptr, &slotsEnumerator);
        }
        if (lang) {
            SysFreeString(lang);
        }
        if (query) {
            SysFreeString(query);
        }
        services->Release();
        if (slotsEnumerator != nullptr) {
            for (int index = 0; index < 8; ++index) {
                IWbemClassObject* array = nullptr;
                ULONG count = 0;
                if (FAILED(slotsEnumerator->Next(200, 1, &array, &count)) || count != 1 || array == nullptr) {
                    break;
                }
                VARIANT value;
                VariantInit(&value);
                const bool systemMemory = SUCCEEDED(array->Get(L"Use", 0, &value, nullptr, nullptr))
                    && ((value.vt == VT_I4 && value.lVal == 3) || (value.vt == VT_UI4 && value.ulVal == 3)
                        || (value.vt == VT_I2 && value.iVal == 3)
                        || (value.vt == VT_UI2 && value.uiVal == 3));
                VariantClear(&value);
                if (systemMemory && SUCCEEDED(array->Get(L"MemoryDevices", 0, &value, nullptr, nullptr))) {
                    const long devices = value.vt == VT_I4 ? value.lVal
                        : value.vt == VT_UI4               ? static_cast<long>(value.ulVal)
                                                           : 0;
                    if (devices > 0 && devices <= 64
                        && wmiSlots + static_cast<std::uint32_t>(devices) <= 64) {
                        wmiSlots += static_cast<std::uint32_t>(devices);
                    }
                }
                VariantClear(&value);
                array->Release();
            }
            slotsEnumerator->Release();
        }
    }
    std::vector<MemoryModule> modules;
    if (enumerator != nullptr) {
        for (int index = 0; index < 32; ++index) {
            IWbemClassObject* item = nullptr;
            ULONG count = 0;
            if (FAILED(enumerator->Next(200, 1, &item, &count)) || count != 1 || item == nullptr) {
                break;
            }
            const auto property = [item](const wchar_t* name) {
                VARIANT result;
                VariantInit(&result);
                item->Get(name, 0, &result, nullptr, nullptr);
                return result;
            };
            const auto number = [](const VARIANT& value) -> std::uint64_t {
                if (value.vt == VT_BSTR && value.bstrVal != nullptr) {
                    return static_cast<std::uint64_t>(_wcstoui64(value.bstrVal, nullptr, 10));
                }
                if (value.vt == VT_UI8) {
                    return value.ullVal;
                }
                if (value.vt == VT_UI4) {
                    return value.ulVal;
                }
                if (value.vt == VT_I4 && value.lVal > 0) {
                    return static_cast<std::uint64_t>(value.lVal);
                }
                return 0;
            };
            const auto string = [](const VARIANT& value) {
                return value.vt == VT_BSTR && value.bstrVal != nullptr ? toUtf8(value.bstrVal)
                                                                       : std::string { };
            };
            MemoryModule module;
            VARIANT value = property(L"Capacity");
            const std::uint64_t capacity = number(value);
            VariantClear(&value);
            module.sizeGb = static_cast<double>(capacity) / (1024.0 * 1024.0 * 1024.0);
            module.populated = module.sizeGb >= 0.25 && module.sizeGb <= 8192.0;
            value = property(L"DeviceLocator");
            module.slot = string(value);
            VariantClear(&value);
            value = property(L"Manufacturer");
            module.vendor = string(value);
            VariantClear(&value);
            value = property(L"PartNumber");
            module.part = string(value);
            VariantClear(&value);
            value = property(L"Speed");
            module.ratedMt = validatedMemorySpeed(static_cast<double>(number(value)));
            VariantClear(&value);
            value = property(L"ConfiguredClockSpeed");
            module.configuredMt = validatedMemorySpeed(static_cast<double>(number(value)));
            VariantClear(&value);
            value = property(L"SMBIOSMemoryType");
            module.type = smbiosMemoryTypeName(static_cast<BYTE>(number(value)));
            VariantClear(&value);
            if (module.populated) {
                modules.push_back(std::move(module));
            }
            item->Release();
        }
        enumerator->Release();
    }
    if (SUCCEEDED(initialized)) {
        CoUninitialize();
    }
    double reported = 0.0;
    for (const MemoryModule& module : modules) {
        reported += module.sizeGb;
    }
    if (modules.empty() || !smbiosSizesPlausible(reported, info.memory.totalGb)) {
        return;
    }
    if (info.memory.slots == 0 && wmiSlots >= modules.size()) {
        info.memory.slots = wmiSlots;
    }
    const bool hasSmbiosSize = std::any_of(info.memory.modules.begin(), info.memory.modules.end(),
        [](const MemoryModule& module) { return module.sizeGb > 0.0; });
    if (!hasSmbiosSize) {
        info.memory.modules = std::move(modules);
        return;
    }
    for (MemoryModule& current : info.memory.modules) {
        const auto match
            = std::find_if(modules.begin(), modules.end(), [&current](const MemoryModule& other) {
                  return !current.slot.empty() && current.slot == other.slot;
              });
        if (match == modules.end()) {
            continue;
        }
        if (current.configuredMt <= 0.0) {
            current.configuredMt = match->configuredMt;
        }
        if (current.ratedMt <= 0.0) {
            current.ratedMt = match->ratedMt;
        }
    }
}

void collectMemory(HardwareIdentity& info, bool includeWmi)
{
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        info.memory.totalGb = static_cast<double>(status.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
    }

    // SMBIOS даёт раскладку по слотам; если он молчит (виртуалка, старый BIOS),
    // остаются только гигабайты из GlobalMemoryStatusEx.
    collectSmbiosMemory(info);
    // Не тормозим каждый запуск дополнительным WMI-запросом, когда Type 17 полон.
    const bool smbiosReady = info.memory.slots > 0 && !info.memory.modules.empty()
        && std::all_of(
            info.memory.modules.begin(), info.memory.modules.end(), [](const MemoryModule& module) {
                return !module.populated || (module.sizeGb > 0.0 && module.configuredMt > 0.0);
            });
    if (includeWmi && !smbiosReady) {
        collectWmiMemory(info);
    }
    double rated = 0.0;
    for (const MemoryModule& module : info.memory.modules) {
        if (!module.populated) {
            continue;
        }
        rated = std::max(rated, module.ratedMt);
        if (info.memory.type.empty()) {
            info.memory.type = module.type;
        }
    }

    info.memory.ratedMt = rated;
    info.memory.configuredMt = configuredMemorySpeed(info.memory.modules);
}

void collectDisplays(HardwareIdentity& info)
{
    const UINT dpi = GetDpiForSystem();
    DISPLAY_DEVICEW adapter{};
    adapter.cb = sizeof(adapter);
    for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &adapter, 0) != FALSE; ++index) {
        if ((adapter.StateFlags & DISPLAY_DEVICE_ACTIVE) == 0) {
            continue;
        }

        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsExW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0) == FALSE) {
            continue;
        }

        DisplayIdentity display;
        display.width = mode.dmPelsWidth;
        display.height = mode.dmPelsHeight;
        display.refreshHz = mode.dmDisplayFrequency;
        display.primary = (adapter.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
        display.scale = dpi > 0 ? static_cast<double>(dpi) / 96.0 : 1.0;

        // Имя монитора берём у драйвера монитора: это EDID-модель, её же показывает
        // «Параметры экрана», а не имя видеоадаптера.
        DISPLAY_DEVICEW monitor{};
        monitor.cb = sizeof(monitor);
        if (EnumDisplayDevicesW(adapter.DeviceName, 0, &monitor, 0) != FALSE) {
            display.name = toUtf8(monitor.DeviceString);
        }
        if (display.name.empty()) {
            display.name = toUtf8(adapter.DeviceString);
        }
        info.displays.push_back(display);
    }
}

void collectDisks(HardwareIdentity& info)
{
    // Диски — из WMI без драйвера: Win32_DiskDrive отдаёт
    // модель, тип интерфейса и размер любого накопителя, будь то NVMe, SATA или USB.
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comReady = SUCCEEDED(comResult) || comResult == RPC_E_CHANGED_MODE;
    bool wmiOk = false;
    if (comReady) {
        IWbemLocator* locator = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator)))
            && locator != nullptr) {
            IWbemServices* services = nullptr;
            BSTR ns = SysAllocString(L"ROOT\\CIMV2");
            HRESULT connect = E_FAIL;
            if (ns != nullptr) {
                connect = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
                SysFreeString(ns);
            }
            locator->Release();
            if (SUCCEEDED(connect) && services != nullptr) {
                CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                    RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
                IEnumWbemClassObject* enumerator = nullptr;
                BSTR lang = SysAllocString(L"WQL");
                BSTR query = SysAllocString(L"SELECT Model, Size, Index, InterfaceType, MediaType FROM Win32_DiskDrive");
                if (lang != nullptr && query != nullptr) {
                    if (SUCCEEDED(services->ExecQuery(lang, query, WBEM_FLAG_FORWARD_ONLY, nullptr, &enumerator))
                        && enumerator != nullptr) {
                        for (int index = 0; index < 32; ++index) {
                            IWbemClassObject* object = nullptr;
                            ULONG returned = 0;
                            if (FAILED(enumerator->Next(200, 1, &object, &returned)) || returned == 0
                                || object == nullptr) {
                                break;
                            }
                            DiskIdentity disk;
                            VARIANT value;
                            VariantInit(&value);
                            if (SUCCEEDED(object->Get(L"Index", 0, &value, nullptr, nullptr))) {
                                if (value.vt == VT_I4 && value.lVal >= 0 && value.lVal < 1024)
                                    disk.diskNumber = static_cast<int>(value.lVal);
                                else if (value.vt == VT_UI4 && value.ulVal < 1024)
                                    disk.diskNumber = static_cast<int>(value.ulVal);
                            }
                            VariantClear(&value);
                            if (SUCCEEDED(object->Get(L"Model", 0, &value, nullptr, nullptr)) && value.vt == VT_BSTR
                                && value.bstrVal != nullptr) {
                                disk.name = toUtf8(value.bstrVal);
                            }
                            VariantClear(&value);
                            if (SUCCEEDED(object->Get(L"InterfaceType", 0, &value, nullptr, nullptr))
                                && value.vt == VT_BSTR && value.bstrVal != nullptr) {
                                disk.interfaceType = toUtf8(value.bstrVal);
                            }
                            VariantClear(&value);
                            if (SUCCEEDED(object->Get(L"MediaType", 0, &value, nullptr, nullptr))
                                && value.vt == VT_BSTR && value.bstrVal != nullptr) {
                                disk.mediaType = toUtf8(value.bstrVal);
                            }
                            VariantClear(&value);
                            if (SUCCEEDED(object->Get(L"Size", 0, &value, nullptr, nullptr))) {
                                std::uint64_t bytes = 0;
                                if (value.vt == VT_BSTR && value.bstrVal != nullptr) {
                                    bytes = static_cast<std::uint64_t>(_wtoi64(value.bstrVal));
                                } else if (value.vt == VT_UI8) {
                                    bytes = value.ullVal;
                                }
                                if (bytes > 0) {
                                    disk.totalGb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
                                }
                            }
                            VariantClear(&value);
                            if (!disk.name.empty()) {
                                info.disks.push_back(disk);
                                wmiOk = true;
                            }
                            object->Release();
                        }
                        enumerator->Release();
                    }
                }
                if (lang) SysFreeString(lang);
                if (query) SysFreeString(query);
                services->Release();
            }
        }
    }

    if (SUCCEEDED(comResult)) {
        CoUninitialize();
    }
    if (wmiOk) {
        return;
    }

    // Резерв: логические диски через GetLogicalDrives — работает даже в виртуалке без WMI.
    const DWORD drives = GetLogicalDrives();
    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
        if ((drives & (1u << (letter - L'A'))) == 0) {
            continue;
        }
        wchar_t root[4]{ letter, L':', L'\\', L'\0' };
        if (GetDriveTypeW(root) != DRIVE_FIXED) {
            continue;
        }
        ULARGE_INTEGER freeBytes{}, totalBytes{};
        if (!GetDiskFreeSpaceExW(root, &freeBytes, &totalBytes, nullptr)) {
            continue;
        }
        wchar_t fs[32]{};
        GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, fs, 32);
        DiskIdentity disk;
        disk.driveLetter = std::string(1, static_cast<char>(letter)) + ":";
        disk.name = disk.driveLetter + " — том";
        disk.totalGb = static_cast<double>(totalBytes.QuadPart) / (1024.0 * 1024.0 * 1024.0);
        disk.freeGb = static_cast<double>(freeBytes.QuadPart) / (1024.0 * 1024.0 * 1024.0);
        disk.fileSystem = toUtf8(fs);
        disk.interfaceType = "Fixed";
        info.disks.push_back(disk);
    }
}

// Версия драйвера видеокарты — из реестра класса дисплейных адаптеров: ищем подпись
// «VEN_10DE&DEV_2704», чтобы не перепутать встроенную и дискретную карты.
void collectGpuDrivers(HardwareIdentity& info, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& pciIds)
{
    constexpr const wchar_t* kClassKey =
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
    HKEY service = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kClassKey, 0, KEY_READ | KEY_WOW64_64KEY, &service) != ERROR_SUCCESS) {
        return;
    }

    std::vector<std::pair<std::string, std::string>> entries;
    for (DWORD index = 0; index < 32; ++index) {
        wchar_t subName[8]{};
        swprintf_s(subName, L"%04u", static_cast<unsigned>(index));
        HKEY subkey = nullptr;
        if (RegOpenKeyExW(service, subName, 0, KEY_READ | KEY_WOW64_64KEY, &subkey) != ERROR_SUCCESS) {
            continue;
        }

        const std::wstring matching = registryString(HKEY_LOCAL_MACHINE,
            (std::wstring(kClassKey) + L"\\" + subName).c_str(), L"MatchingDeviceId");
        const std::wstring version = registryString(HKEY_LOCAL_MACHINE,
            (std::wstring(kClassKey) + L"\\" + subName).c_str(), L"DriverVersion");
        RegCloseKey(subkey);

        if (!version.empty()) {
            entries.emplace_back(toUtf8(matching), toUtf8(version));
        }
    }
    RegCloseKey(service);

    for (std::size_t index = 0; index < info.gpus.size() && index < pciIds.size(); ++index) {
        char needle[40]{};
        std::snprintf(needle, sizeof(needle), "VEN_%04X&DEV_%04X", pciIds[index].first, pciIds[index].second);
        for (const auto& [id, version] : entries) {
            if (id.find(needle) != std::string::npos) {
                info.gpus[index].driverVersion = version;
                break;
            }
        }
    }
}

} // namespace

bool collectHardwareInfo(
    HardwareIdentity& info, const GpuRegistry& registry, std::string& error, bool includeWmi)
{
    collectOperatingSystem(info);
    collectCpu(info);
    collectMemory(info, includeWmi);
    collectDisplays(info);
    if (includeWmi) {
        collectDisks(info);
    }

    std::vector<std::pair<std::uint32_t, std::uint32_t>> pciIds;
    for (const GpuAdapter& adapter : registry.adapters()) {
        if (adapter.isSoftware) {
            continue;
        }

        GpuIdentity gpu;
        gpu.name = toUtf8(adapter.name);
        gpu.dedicatedMb = static_cast<double>(adapter.dedicatedVideoMemory) / (1024.0 * 1024.0);
        gpu.discrete = adapter.dedicatedVideoMemory > 512ull * 1024 * 1024;
        switch (adapter.vendor) {
        case GpuVendor::Nvidia:
            gpu.vendor = "NVIDIA";
            break;
        case GpuVendor::Amd:
            gpu.vendor = "AMD";
            break;
        case GpuVendor::Intel:
            gpu.vendor = "Intel";
            break;
        default:
            gpu.vendor = "Unknown";
            break;
        }
        info.gpus.push_back(gpu);
        pciIds.emplace_back(adapter.vendorId, adapter.deviceId);
    }
    collectGpuDrivers(info, pciIds);

    info.detected = !info.cpu.name.empty() || !info.gpus.empty();
    if (!info.detected) {
        error = "не удалось определить ни процессор, ни видеокарту";
    }
    return info.detected;
}

} // namespace yawa

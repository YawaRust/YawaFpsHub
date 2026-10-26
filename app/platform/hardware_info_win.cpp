// Сбор сведений о машине: модель CPU, адаптеры DXGI, модули памяти из SMBIOS,
// монитор и версия Windows. Источники — только встроенные в Windows: реестр, CPUID,
// GetSystemFirmwareTable, DXGI и EnumDisplaySettings. Драйверов, WMI-провайдеров
// производителей и сети приложение не требует.
#include "platform/hardware_info_win.h"
#include "metrics/gpu_registry.h"
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
    if (limits[0] < 0x80000004) {
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

std::string memoryTypeName(BYTE type)
{
    switch (type) {
    case 0x12:
        return "DDR";
    case 0x13:
        return "DDR2";
    case 0x18:
        return "DDR3";
    case 0x1A:
        return "DDR4";
    case 0x22:
        return "DDR5";
    case 0x1E:
        return "LPDDR4";
    case 0x21:
        return "LPDDR5";
    default:
        return {};
    }
}

// SMBIOS — единственный источник, который честно говорит «сколько планок и каких».
// Поля читаем с проверкой длины структуры: на старых BIOS часть полей отсутствует.
void collectSmbiosMemory(HardwareIdentity& info)
{
    // GetSystemFirmwareTable принимает сигнатуру провайдера: четыре символа 'RSMB'.
    constexpr DWORD kSmbios = 0x52534D42;
    const DWORD needed = GetSystemFirmwareTable(kSmbios, 0, nullptr, 0);
    if (needed <= 8 || needed > 16u * 1024u * 1024u) {
        return;
    }

    std::vector<BYTE> table(needed);
    if (GetSystemFirmwareTable(kSmbios, 0, table.data(), needed) != needed) {
        return;
    }

    // RAW_SMBIOS_DATA: байты метода и версий, затем DWORD длины и сами структуры.
    const std::size_t dataStart = 8;
    DWORD dataLength = 0;
    std::memcpy(&dataLength, table.data() + 4, sizeof(dataLength));
    if (dataStart + dataLength > table.size()) {
        return;
    }

    const BYTE* const begin = table.data() + dataStart;
    const BYTE* const limit = begin + dataLength;
    const BYTE* cursor = begin;

    std::vector<MemoryModule> modules;
    std::uint32_t slots = 0;
    // SMBIOS 2.7 перешёл с мегагерц на мегаперехваты в секунду; до него удваиваем.
    const bool speedInMhz = table[1] < 2 || (table[1] == 2 && table[2] < 7);
    const double speedScale = speedInMhz ? 2.0 : 1.0;
    std::string channelLetters;
    while (cursor + 4 <= limit) {
        const BYTE type = cursor[0];
        const BYTE length = cursor[1];
        if (length < 4 || cursor + length > limit) {
            break;
        }

        const BYTE* const strings = cursor + length;
        auto stringAt = [&](BYTE index) -> std::string {
            if (index == 0 || strings >= limit) {
                return {};
            }
            const BYTE* scan = strings;
            for (BYTE current = 1; current < index && scan < limit; ++current) {
                while (scan < limit && *scan != 0) {
                    ++scan;
                }
                ++scan;
            }
            const BYTE* start = scan;
            while (scan < limit && *scan != 0) {
                ++scan;
            }
            if (start > limit) {
                return {};
            }
            return std::string(reinterpret_cast<const char*>(start), static_cast<std::size_t>(scan - start));
        };

        auto at = [&](std::size_t offset) -> WORD {
            WORD value = 0;
            if (length >= offset + 2 && cursor + offset + 2 <= limit) {
                std::memcpy(&value, cursor + offset, sizeof(value));
            }
            return value;
        };
        // Байт только внутри форматной части: короткие структуры старых BIOS
        // не доходят до нужного поля, и читать за ней нельзя.
        auto byteAt = [&](std::size_t offset) -> BYTE {
            return offset < length && cursor + offset < limit ? cursor[offset] : BYTE{ 0 };
        };

        if (type == 16 && length >= 0x12) {
            // Physical Memory Array, 10h — число слотов под память.
            const WORD devices = at(0x10);
            slots = std::max<std::uint32_t>(slots, devices);
        } else if (type == 17 && length >= 0x13) {
            // Память по DMTF: размер — 10h, тип — 16h, скорость SPD — 19h, строки
            // слота/вендора/партномера — 14h/1Bh/1Eh, конфигурационная частота — 24h.
            const WORD rawSize = at(0x10);
            MemoryModule module;
            if (rawSize != 0 && rawSize != 0xFFFF) {
                const double megabytes = (rawSize & 0x8000) != 0
                    ? static_cast<double>(rawSize & 0x7FFF) / 1024.0
                    : static_cast<double>(rawSize);
                double sizeGb = megabytes / 1024.0;
                if (sizeGb <= 0.0 && length >= 0x24) {
                    // 20h — расширенный размер в МБ для планок больше 32 ГБ.
                    DWORD extended = 0;
                    if (cursor + 0x24 <= limit) {
                        std::memcpy(&extended, cursor + 0x20, sizeof(extended));
                    }
                    sizeGb = static_cast<double>(extended) / 1024.0;
                }
                if (sizeGb <= 0.0) {
                    module.populated = false;
                }
                module.sizeGb = sizeGb;
                module.populated = sizeGb > 0.0;
            }

            if (module.populated) {
                const WORD rated = at(0x19);
                const WORD configured = length >= 0x26 ? at(0x24) : 0;
                const WORD millivolts = length >= 0x2C ? at(0x2A) : 0;
                module.ratedMt = rated == 0xFFFF ? 0.0 : static_cast<double>(rated) * speedScale;
                module.configuredMt = configured == 0xFFFF ? 0.0 : static_cast<double>(configured) * speedScale;
                module.voltage = millivolts > 0 ? static_cast<double>(millivolts) / 1000.0 : 0.0;
                module.type = memoryTypeName(byteAt(0x16));
                module.slot = stringAt(byteAt(0x14));
                module.vendor = stringAt(byteAt(0x1B));
                module.part = stringAt(byteAt(0x1E));
                for (const char symbol : module.slot) {
                    const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(symbol)));
                    if (upper >= 'A' && upper <= 'H' && channelLetters.find(upper) == std::string::npos) {
                        channelLetters.push_back(upper);
                    }
                }
                modules.push_back(module);
            }
        }

        // Следующая структура идёт за блоком строк, который заканчивается пустой строкой.
        const BYTE* scan = strings;
        while (scan + 1 < limit && !(*scan == 0 && *(scan + 1) == 0)) {
            ++scan;
        }
        cursor = scan + 2;
    }

    if (!modules.empty()) {
        info.memory.modules = modules;
        info.memory.slots = std::max<std::uint32_t>(slots, static_cast<std::uint32_t>(modules.size()));
        info.memory.channels = static_cast<std::uint32_t>(channelLetters.size());
    }
}

void collectMemory(HardwareIdentity& info)
{
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        info.memory.totalGb = static_cast<double>(status.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
    }

    // SMBIOS даёт раскладку по слотам; если он молчит (виртуалка, старый BIOS),
    // остаются только гигабайты из GlobalMemoryStatusEx.
    collectSmbiosMemory(info);

    std::uint32_t populated = 0;
    double rated = 0.0;
    double configured = 0.0;
    for (const MemoryModule& module : info.memory.modules) {
        if (!module.populated) {
            continue;
        }
        ++populated;
        rated = std::max(rated, module.ratedMt);
        configured = std::max(configured, module.configuredMt);
        if (info.memory.type.empty()) {
            info.memory.type = module.type;
        }
    }

    info.memory.ratedMt = rated;
    info.memory.configuredMt = configured > 0.0 ? configured : rated;
    // Разница паспортной и рабочей частот — и есть признак невыключенного XMP/EXPO.
    info.memory.profileEnabled = rated <= 0.0 || configured <= 0.0 || configured + 1.0 >= rated;
    // Один заселённый слот из двух и больше — это одноканальный режим, его можно
    // утверждать. Сколько именно каналов при двух планках — сказать без SMBus нельзя.
    info.memory.singleChannel = populated == 1 && (info.memory.slots == 0 || info.memory.slots >= 2);
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
    // Диски — из WMI, как и температуры, но без драйвера: Win32_DiskDrive отдаёт
    // модель, тип интерфейса и размер любого накопителя, будь то NVMe, SATA или USB.
    const bool comReady = [] {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
    }();
    bool wmiOk = false;
    if (comReady) {
        IWbemLocator* locator = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator)))
            && locator != nullptr) {
            IWbemServices* services = nullptr;
            BSTR ns = SysAllocString(L"ROOT\\\\CIMV2");
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
                BSTR query = SysAllocString(L"SELECT Model, Size, InterfaceType, MediaType FROM Win32_DiskDrive");
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
                                } else if (value.vt == VT_BSVALT) {
                                    // редко
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

    if (wmiOk) {
        // Обогащаем свободным местом по логическим дискам: каждому физическому сопоставить
        // суммарный свободный объём нельзя без Partition-драйвера, поэтому показываем
        // логические тома отдельной строкой только если WMI не дал ни одного диска.
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
        disk.driveLetter = std::string(1, static_cast<char>(letter)) + \":\";
        disk.name = disk.driveLetter + \" — том\";
        disk.totalGb = static_cast<double>(totalBytes.QuadPart) / (1024.0 * 1024.0 * 1024.0);
        disk.freeGb = static_cast<double>(freeBytes.QuadPart) / (1024.0 * 1024.0 * 1024.0);
        disk.fileSystem = toUtf8(fs);
        disk.interfaceType = \"Fixed\";
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

bool collectHardwareInfo(HardwareIdentity& info, const GpuRegistry& registry, std::string& error)
{
    collectOperatingSystem(info);
    collectCpu(info);
    collectMemory(info);
    collectDisplays(info);
    collectDisks(info);

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

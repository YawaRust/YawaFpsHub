#include "platform/pawnio_package.h"
#include "platform/pawnio_sensors.h"
#include "platform/win_util.h"
#include "resources/pawnio_ids.h"

#include <objbase.h>
#include <windows.h>

#include <setupapi.h>
#include <wintrust.h>

#include <bcrypt.h>
#include <newdev.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <softpub.h>

#include <array>
#include <string>
#include <string_view>
#include <thread>

namespace yawa {
namespace {

    struct PackageFile
    {
        int id;
        const wchar_t* name;
        const char* sha256;
    };

    constexpr PackageFile kPackage[] = {
        { IDR_PAWNIO_SYS, L"PawnIO.sys", "fca6e7d58b0cf38dbb913a2b9e532f48629145d395f454b16a9f58e97b8d3940" },
        { IDR_PAWNIO_INF, L"PawnIO.inf", "7c1c203e13693531243fbee3cb87d7b79170eae89f5729b3f41387fe68a54f0b" },
        { IDR_PAWNIO_CAT, L"pawnio.cat", "a37d46840280efec92063d3a21014c803939e599b4c0da4a4d063b79eeca9446" },
    };
    constexpr GUID kSoftwareDeviceClass
        = { 0x62f9c741, 0xb25a, 0x46ce, { 0xb5, 0x4c, 0x9b, 0xcc, 0xce, 0x08, 0xb6, 0xf2 } };
    constexpr wchar_t kHardwareId[] = L"Root\\PawnIO\0";

    bool matchesHash(const void* bytes, DWORD size, std::string_view expected)
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
            return false;
        }
        std::array<unsigned char, 32> digest { };
        const LONG status = BCryptHash(algorithm, nullptr, 0, static_cast<PUCHAR>(const_cast<void*>(bytes)),
            size, digest.data(), static_cast<ULONG>(digest.size()));
        BCryptCloseAlgorithmProvider(algorithm, 0);
        std::string actual;
        for (const unsigned char byte : digest) {
            actual += "0123456789abcdef"[byte >> 4];
            actual += "0123456789abcdef"[byte & 15];
        }
        return status >= 0 && actual == expected;
    }

    bool writeResource(int id, const std::wstring& path, std::string_view hash, std::wstring& error)
    {
        const HMODULE instance = GetModuleHandleW(nullptr);
        const HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
        const DWORD size = resource != nullptr ? SizeofResource(instance, resource) : 0;
        const void* bytes = resource != nullptr ? LockResource(LoadResource(instance, resource)) : nullptr;
        if (bytes == nullptr || size == 0 || (!hash.empty() && !matchesHash(bytes, size, hash))) {
            error = L"Ресурс датчиков отсутствует или его SHA-256 не совпадает: " + path;
            return false;
        }
        const HANDLE file = CreateFileW(
            path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            error = L"Не удалось создать " + path + L": " + widen(win32ErrorText(GetLastError()));
            return false;
        }
        DWORD written = 0;
        const BOOL result = WriteFile(file, bytes, size, &written, nullptr);
        const bool success = result && written == size;
        const DWORD status = result ? ERROR_WRITE_FAULT : GetLastError();
        CloseHandle(file);
        if (!success) {
            DeleteFileW(path.c_str());
            error = L"Не удалось записать " + path + L": " + widen(win32ErrorText(status));
        }
        return success;
    }

    class PackageDirectory
    {
    public:
        ~PackageDirectory()
        {
            if (!m_path.empty()) {
                for (const auto& file : kPackage) {
                    DeleteFileW((m_path + L"\\" + file.name).c_str());
                }
                RemoveDirectoryW(m_path.c_str());
            }
        }

        bool create(std::wstring& error)
        {
            PWSTR root = nullptr;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &root))) {
                error = L"Каталог Program Files недоступен";
                return false;
            }
            GUID guid { };
            const HRESULT result = CoCreateGuid(&guid);
            wchar_t suffix[40] { };
            StringFromGUID2(guid, suffix, 40);
            const std::wstring path = std::wstring(root) + L"\\YawaFpsHub-PawnIO-" + suffix;
            CoTaskMemFree(root);
            if (FAILED(result)) {
                error = L"Не удалось создать идентификатор пакета";
                return false;
            }
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr)) {
                error = L"Не удалось подготовить права каталога драйвера";
                return false;
            }
            SECURITY_ATTRIBUTES attributes { sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE };
            const bool created = CreateDirectoryW(path.c_str(), &attributes) != FALSE;
            const DWORD status = GetLastError();
            LocalFree(descriptor);
            if (!created) {
                error = L"Не удалось создать каталог пакета: " + widen(win32ErrorText(status));
                return false;
            }
            m_path = path;
            return true;
        }

        const std::wstring& path() const { return m_path; }

    private:
        std::wstring m_path;
    };

    bool verifyDriver(const std::wstring& path, std::wstring& error)
    {
        WINTRUST_FILE_INFO file { };
        file.cbStruct = sizeof(file);
        file.pcwszFilePath = path.c_str();
        WINTRUST_DATA trust { };
        trust.cbStruct = sizeof(trust);
        trust.dwUIChoice = WTD_UI_NONE;
        trust.dwUnionChoice = WTD_CHOICE_FILE;
        trust.pFile = &file;
        // У зафиксированного пакета дополнительная подпись Microsoft имеет индекс 1.
        WINTRUST_SIGNATURE_SETTINGS signatures { };
        signatures.cbStruct = sizeof(signatures);
        signatures.dwFlags = WSS_VERIFY_SPECIFIC;
        signatures.dwIndex = 1;
        trust.pSignatureSettings = &signatures;
        trust.dwStateAction = WTD_STATEACTION_VERIFY;
        // Приложение не обращается к сети. Политика допуска kernel-пакета остаётся за SetupAPI.
        trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
        GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        const LONG status = WinVerifyTrust(nullptr, &action, &trust);
        trust.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(nullptr, &action, &trust);
        if (status != ERROR_SUCCESS) {
            error = L"Windows не подтвердила подпись драйвера: " + widen(hresultText(status));
            return false;
        }
        return true;
    }

    bool findOrCreateDevice(HDEVINFO devices, SP_DEVINFO_DATA& device, bool& created, std::wstring& error)
    {
        for (DWORD index = 0;; ++index) {
            device = { };
            device.cbSize = sizeof(device);
            if (!SetupDiEnumDeviceInfo(devices, index, &device)) {
                if (GetLastError() != ERROR_NO_MORE_ITEMS) {
                    error = L"Не удалось перечислить устройства PawnIO";
                    return false;
                }
                break;
            }
            std::array<wchar_t, 513> ids { };
            DWORD type = 0;
            if (SetupDiGetDeviceRegistryPropertyW(devices, &device, SPDRP_HARDWAREID, &type,
                    reinterpret_cast<PBYTE>(ids.data()), 512 * sizeof(wchar_t), nullptr)
                && type == REG_MULTI_SZ && _wcsicmp(ids.data(), L"Root\\PawnIO") == 0) {
                return true;
            }
        }
        device = { };
        device.cbSize = sizeof(device);
        if (!SetupDiCreateDeviceInfoW(
                devices, L"PawnIO", &kSoftwareDeviceClass, L"PawnIO", nullptr, DICD_GENERATE_ID, &device)) {
            error = L"Не удалось создать устройство PawnIO: " + widen(win32ErrorText(GetLastError()));
            return false;
        }
        if (!SetupDiSetDeviceRegistryPropertyW(devices, &device, SPDRP_HARDWAREID,
                reinterpret_cast<const BYTE*>(kHardwareId), sizeof(kHardwareId))
            || !SetupDiCallClassInstaller(DIF_REGISTERDEVICE, devices, &device)) {
            error = L"Не удалось зарегистрировать Root\\PawnIO: " + widen(win32ErrorText(GetLastError()));
            return false;
        }
        created = true;
        return true;
    }

    bool installDriver(bool& reboot, std::wstring& error)
    {
        PackageDirectory directory;
        if (!directory.create(error)) {
            return false;
        }
        for (const auto& file : kPackage) {
            if (!writeResource(file.id, directory.path() + L"\\" + file.name, file.sha256, error)) {
                return false;
            }
        }
        if (!verifyDriver(directory.path() + L"\\PawnIO.sys", error)) {
            return false;
        }
        const HDEVINFO devices = SetupDiGetClassDevsW(&kSoftwareDeviceClass, nullptr, nullptr, 0);
        if (devices == INVALID_HANDLE_VALUE) {
            error = L"Не удалось открыть список устройств: " + widen(win32ErrorText(GetLastError()));
            return false;
        }
        SP_DEVINFO_DATA device { };
        bool created = false;
        bool success = findOrCreateDevice(devices, device, created, error);
        if (success) {
            BOOL required = FALSE;
            success = UpdateDriverForPlugAndPlayDevicesW(nullptr, L"Root\\PawnIO",
                          (directory.path() + L"\\PawnIO.inf").c_str(), 0, &required)
                != FALSE;
            reboot = required != FALSE;
            if (!success) {
                error = L"Windows не установила пакет PawnIO: " + widen(win32ErrorText(GetLastError()));
            }
        }
        if (!success && created && !SetupDiCallClassInstaller(DIF_REMOVE, devices, &device)) {
            error += L". Созданное устройство также не удалось удалить; проверьте диспетчер устройств";
        }
        SetupDiDestroyDeviceInfoList(devices);
        return success;
    }

    bool isAdministrator()
    {
        SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
        PSID administrators = nullptr;
        if (!AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0,
                0, 0, 0, 0, 0, &administrators)) {
            return false;
        }
        BOOL member = FALSE;
        const BOOL queried = CheckTokenMembership(nullptr, administrators, &member);
        FreeSid(administrators);
        return queried && member;
    }

} // namespace

namespace {

    std::wstring sensorReadyEvent(DWORD parentPid)
    {
        return L"Local\\YawaFpsHubSensorReady-" + std::to_wstring(parentPid);
    }

    bool deviceAccessible()
    {
        const HANDLE device = CreateFileW(L"\\\\?\\GLOBALROOT\\Device\\PawnIO", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (device == INVALID_HANDLE_VALUE) {
            return false;
        }
        CloseHandle(device);
        return true;
    }

} // namespace

void requestSensorElevation(HWND window, UINT completionMessage)
{
    if (!PawnioSensors::supportsCpu() || deviceAccessible()) {
        return;
    }
    const DWORD status = GetLastError();
    SYSTEM_INFO system { };
    GetNativeSystemInfo(&system);
    if (system.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64) {
        return;
    }
    if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND
        && !(status == ERROR_ACCESS_DENIED && !isAdministrator())) {
        return; // Не переустанавливаем устройство, которое существует, но сломано или занято.
    }
    DWORD parentPid = 0;
    GetWindowThreadProcessId(window, &parentPid);
    if (parentPid == 0) {
        return;
    }
    const std::wstring eventName = sensorReadyEvent(parentPid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;BA)(A;;GA;;;SY)", SDDL_REVISION_1, &descriptor, nullptr)) {
        PostMessageW(window, completionMessage, 2, GetLastError());
        return;
    }
    SECURITY_ATTRIBUTES attributes { sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE };
    HANDLE ready = CreateEventW(&attributes, TRUE, FALSE, eventName.c_str());
    const DWORD eventStatus = GetLastError();
    LocalFree(descriptor);
    if (ready == nullptr || eventStatus == ERROR_ALREADY_EXISTS) {
        if (ready != nullptr) {
            CloseHandle(ready);
        }
        PostMessageW(window, completionMessage, 2, eventStatus);
        return;
    }
    std::thread([ready, window, completionMessage, parentPid]() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        wchar_t executable[32768] { };
        DWORD error = ERROR_SUCCESS;
        bool accepted = false;
        if (GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))) == 0) {
            error = GetLastError();
        } else {
            const std::wstring parameters = L"--sensor-ready=" + std::to_wstring(parentPid);
            SHELLEXECUTEINFOW launch { };
            launch.cbSize = sizeof(launch);
            launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
            launch.lpVerb = isAdministrator() ? L"open" : L"runas";
            launch.lpFile = executable;
            launch.lpParameters = parameters.c_str();
            launch.nShow = SW_SHOWNORMAL;
            if (!ShellExecuteExW(&launch)) {
                error = GetLastError();
            } else if (launch.hProcess != nullptr) {
                HANDLE handles[] { ready, launch.hProcess };
                const DWORD result = WaitForMultipleObjects(2, handles, FALSE, 120000);
                accepted = result == WAIT_OBJECT_0;
                if (!accepted) {
                    error = result == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_GEN_FAILURE;
                }
                CloseHandle(launch.hProcess);
            } else {
                error = ERROR_INVALID_HANDLE;
            }
        }
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
        CloseHandle(ready);
        DWORD owner = 0;
        GetWindowThreadProcessId(window, &owner);
        if (owner == parentPid) {
            PostMessageW(window, completionMessage, accepted ? 1 : 2, error);
        }
    }).detach();
}

bool prepareSensorsForLaunch(std::wstring_view arguments)
{
    constexpr std::wstring_view flag = L"--sensor-ready=";
    if (!arguments.starts_with(flag)) {
        return false;
    }
    const std::wstring value(arguments.substr(flag.size()));
    wchar_t* end = nullptr;
    const unsigned long parsed = std::wcstoul(value.c_str(), &end, 10);
    if (parsed == 0 || *end != L'\0' || !isAdministrator()) {
        return true;
    }
    const DWORD parentPid = static_cast<DWORD>(parsed);
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, sensorReadyEvent(parentPid).c_str());
    if (parent == nullptr || ready == nullptr) {
        if (parent) {
            CloseHandle(parent);
        }
        if (ready) {
            CloseHandle(ready);
        }
        return true;
    }
    if (!deviceAccessible()) {
        const DWORD status = GetLastError();
        if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND) {
            CloseHandle(ready);
            CloseHandle(parent);
            return true;
        }
        bool reboot = false;
        std::wstring error;
        if (!installDriver(reboot, error)) {
            MessageBoxW(nullptr, (L"Аппаратные датчики CPU недоступны: " + error).c_str(), L"YawaFpsHub",
                MB_OK | MB_ICONWARNING);
            CloseHandle(ready);
            CloseHandle(parent);
            return true;
        }
        if (reboot) {
            MessageBoxW(nullptr, L"Для аппаратных датчиков CPU потребуется перезагрузка Windows.",
                L"YawaFpsHub", MB_OK | MB_ICONINFORMATION);
        }
    }
    if (!SetEvent(ready)) {
        CloseHandle(ready);
        CloseHandle(parent);
        return true;
    }
    CloseHandle(ready);
    const DWORD result = WaitForSingleObject(parent, 45000);
    CloseHandle(parent);
    return result != WAIT_OBJECT_0;
}

int runSensorCommandLine()
{
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr) {
        return -1;
    }
    const bool install = count == 2 && std::wstring_view(arguments[1]) == L"--install-sensors";
    const bool sources = count == 2 && std::wstring_view(arguments[1]) == L"--sensor-sources";
    LocalFree(arguments);
    if (!install && !sources) {
        return -1;
    }
    std::wstring error;
    if (sources) {
        if (!writeResource(IDR_PAWNIO_SOURCES, L"YawaFpsHub-PawnIO-sources.zip",
                "46cb979754d6f2a6161c1aebc6b383f7ce1a1d611c8158cda4b61a9da795b47c", error)) {
            MessageBoxW(nullptr, error.c_str(), L"Исходники датчиков", MB_OK | MB_ICONERROR);
            return 1;
        }
        MessageBoxW(nullptr,
            L"Исходники и лицензии сохранены в YawaFpsHub-PawnIO-sources.zip в текущем каталоге.",
            L"Исходники датчиков", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    if (!isAdministrator()) {
        MessageBoxW(nullptr,
            L"Запустите эту команду из PowerShell от имени администратора:\n"
            L".\\YawaFpsHub.exe --install-sensors\n\n"
            L"Для чтения kernel-датчиков приложение также нужно запускать от администратора.",
            L"Установка датчиков", MB_OK | MB_ICONINFORMATION);
        return ERROR_ELEVATION_REQUIRED;
    }
    SYSTEM_INFO system { };
    GetNativeSystemInfo(&system);
    if (system.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64) {
        MessageBoxW(nullptr, L"В этот exe включён только AMD64-пакет PawnIO. ARM64 пока не поддержан.",
            L"Установка датчиков", MB_OK | MB_ICONERROR);
        return ERROR_NOT_SUPPORTED;
    }
    if (MessageBoxW(nullptr,
            L"Установить подписанный kernel-драйвер PawnIO 2.2.0?\n"
            L"Windows зарегистрирует устройство и службу драйвера. Может потребоваться перезагрузка.\n"
            L"Другие приложения мониторинга на время установки рекомендуется закрыть.\n\n"
            L"Драйвер: GPL-2.0-or-later с исключением; модули: LGPL-2.1-or-later.\n"
            L"Исходники и лицензии: YawaFpsHub.exe --sensor-sources.",
            L"Установка датчиков", MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING)
        != IDYES) {
        return ERROR_CANCELLED;
    }
    bool reboot = false;
    if (!installDriver(reboot, error)) {
        MessageBoxW(nullptr, error.c_str(), L"Установка датчиков", MB_OK | MB_ICONERROR);
        return 1;
    }
    MessageBoxW(nullptr,
        reboot ? L"Пакет установлен. Перезагрузите Windows перед запуском kernel-датчиков."
               : L"Пакет установлен. Перезапустите YawaFpsHub от имени администратора.",
        L"Установка датчиков", MB_OK | MB_ICONINFORMATION);
    return reboot ? ERROR_SUCCESS_REBOOT_REQUIRED : 0;
}

} // namespace yawa

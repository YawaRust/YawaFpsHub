#include "platform/sensors_win.h"
#include "platform/log.h"
#include "platform/win_util.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <wbemidl.h>

#include <algorithm>
#include <cstdio>
#include <vector>
#include <string_view>

namespace yawa {
namespace {

std::uint64_t fileTimeToTicks(const FILETIME& time)
{
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

std::uint32_t readNominalCpuMhz()
{
    DWORD mhz = 0;
    DWORD size = sizeof(mhz);
    const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        L"~MHz", RRF_RT_REG_DWORD, nullptr, &mhz, &size);
    return status == ERROR_SUCCESS ? mhz : 0;
}

// «AMD Ryzen 7 7800X3D 8-Core Processor» -> «Ryzen 7 7800X3D»: в карточке нужна
// короткая подпись, как в демо, а не полное имя из реестра на две строки.
std::wstring shortenCpuName(std::wstring raw)
{
    for (const std::wstring_view noise : { std::wstring_view(L"(R)"), std::wstring_view(L"(TM)"),
             std::wstring_view(L"(tm)"), std::wstring_view(L" CPU") }) {
        for (std::size_t at = raw.find(noise); at != std::wstring::npos; at = raw.find(noise)) {
            raw.erase(at, noise.size());
        }
    }

    std::wstring result;
    std::size_t start = 0;
    while (start < raw.size()) {
        std::size_t end = raw.find(L' ', start);
        if (end == std::wstring::npos) {
            end = raw.size();
        }
        const std::wstring token = raw.substr(start, end - start);
        start = end + 1;

        const bool vendor = token == L"AMD" || token == L"Intel";
        const bool suffix = token == L"Processor" || token == L"CPU" || token == L"@"
            || token.find(L"-Core") != std::wstring::npos || token.find(L"GHz") != std::wstring::npos
            || token.find(L"Core(TM)") != std::wstring::npos;
        if (token.empty() || vendor || suffix) {
            continue;
        }
        if (!result.empty()) {
            result += L' ';
        }
        result += token;
    }
    return result.empty() ? raw : result;
}

bool isLoopbackName(std::wstring_view name)
{
    return name.find(L"Loopback") != std::wstring_view::npos
        || name.find(L"isatap") != std::wstring_view::npos
        || name.find(L"Pseudo-Interface") != std::wstring_view::npos;
}

std::uint32_t countProcessThreads()
{
    const DWORD processId = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    std::uint32_t count = 0;

    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == processId) {
                ++count;
            }
        } while (Thread32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return count;
}

} // namespace

namespace {

// Те же данные, что у диспетчера задач: ядро отдаёт время простоя и полное время
// по каждому логическому процессору. Доступно без прав администратора и без драйверов.
struct ProcessorPerformance {
    long long idleTime;
    long long kernelTime;
    long long userTime;
    long long dpcTime;
    long long interruptTime;
    unsigned long interruptCount;
};

using NtQuerySystemInformationFn = long(__stdcall*)(int, void*, unsigned long, unsigned long*);

constexpr int kSystemProcessorPerformanceInformation = 8;
// NTSTATUS 0xC0000004: буфер меньше, чем нужно системе.
constexpr long kStatusInfoLengthMismatch = static_cast<long>(0xC0000004u);

// Частота процессора без PDH: powrprof.dll отдаёт текущие МГц по каждому потоку.
// Это тот же источник, что был у диспетчера задач, и он работает без драйверов.
struct ProcessorPowerInformation {
    unsigned long number;
    unsigned long maxMhz;
    unsigned long currentMhz;
    unsigned long mhzLimit;
    unsigned long maxIdleState;
    unsigned long currentIdleState;
};

using CallNtPowerInformationFn = long(__stdcall*)(int, void*, unsigned long, void*, unsigned long);

constexpr int kProcessorInformation = 11;

CallNtPowerInformationFn resolveCallNtPowerInformation()
{
    static CallNtPowerInformationFn function = [] {
        HMODULE module = LoadLibraryW(L"powrprof.dll");
        if (module == nullptr) {
            return static_cast<CallNtPowerInformationFn>(nullptr);
        }
        return reinterpret_cast<CallNtPowerInformationFn>(
            GetProcAddress(module, "CallNtPowerInformation"));
    }();
    return function;
}

double currentClockFromPowerInformation()
{
    const CallNtPowerInformationFn query = resolveCallNtPowerInformation();
    if (query == nullptr) {
        return 0.0;
    }

    std::array<ProcessorPowerInformation, 64> buffer{};
    const long status = query(kProcessorInformation, nullptr, 0, buffer.data(),
        static_cast<unsigned long>(buffer.size() * sizeof(ProcessorPowerInformation)));
    if (status < 0) {
        return 0.0;
    }

    // Максимум по потокам: у спящих ядер частоты нулевые, среднее занижало бы значение.
    unsigned long clock = 0;
    for (const ProcessorPowerInformation& info : buffer) {
        clock = std::max(clock, info.currentMhz);
    }
    return static_cast<double>(clock);
}

NtQuerySystemInformationFn resolveNtQuerySystemInformation()
{
    static NtQuerySystemInformationFn function = [] {
        HMODULE module = GetModuleHandleW(L"ntdll.dll");
        if (module == nullptr) {
            module = LoadLibraryW(L"ntdll.dll");
        }
        if (module == nullptr) {
            return static_cast<NtQuerySystemInformationFn>(nullptr);
        }
        return reinterpret_cast<NtQuerySystemInformationFn>(
            GetProcAddress(module, "NtQuerySystemInformation"));
    }();
    return function;
}

} // namespace

bool SystemSensors::sampleCoreTimes()
{
    const NtQuerySystemInformationFn query = resolveNtQuerySystemInformation();
    if (query == nullptr) {
        return false;
    }

    // Буфер с запасом и одним повтором: на машине с сотней потоков первый вызов вернёт
    // STATUS_INFO_LENGTH_MISMATCH и требуемый размер в written.
    std::vector<ProcessorPerformance> buffer(64);
    unsigned long written = 0;
    long status = query(kSystemProcessorPerformanceInformation, buffer.data(),
        static_cast<unsigned long>(buffer.size() * sizeof(ProcessorPerformance)), &written);
    if (status == kStatusInfoLengthMismatch && written > buffer.size() * sizeof(ProcessorPerformance)) {
        buffer.resize(written / sizeof(ProcessorPerformance) + 1);
        written = 0;
        status = query(kSystemProcessorPerformanceInformation, buffer.data(),
            static_cast<unsigned long>(buffer.size() * sizeof(ProcessorPerformance)), &written);
    }
    m_coreQueryStatus = status;
    m_coreQueryWritten = written;
    if (status < 0 || written == 0) {
        return false;
    }

    // Дальше живут массивы на 16 ядер: лишние потоки отбрасываем, а не пишем мимо.
    const std::size_t count = std::min<std::size_t>({ written / sizeof(ProcessorPerformance),
        buffer.size(), m_coreIdleTicks.size() });
    m_coreTimeCount = static_cast<std::uint32_t>(count);
    for (std::size_t index = 0; index < count; ++index) {
        m_coreIdleTicks[index] = static_cast<std::uint64_t>(buffer[index].idleTime);
        m_coreTotalTicks[index] = static_cast<std::uint64_t>(buffer[index].kernelTime)
            + static_cast<std::uint64_t>(buffer[index].userTime);
    }
    return count > 0;
}

bool SystemSensors::initialize(Log& log, std::string& error)
{
    error.clear();
    initializeCpu(log);

    m_availability.memory = true;
    m_availability.disk = m_diskRead.open(L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec")
        && m_diskWrite.open(L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec");
    m_availability.network = m_netReceived.open(L"\\Network Interface(*)\\Bytes Received/sec")
        && m_netSent.open(L"\\Network Interface(*)\\Bytes Sent/sec");

    if (!m_availability.disk) {
        log.warning("PDH: счётчики диска недоступны");
    }
    if (!m_availability.network) {
        log.warning("PDH: счётчики сети недоступны");
    }

    std::string gpuError;
    if (!initializeGpu(log, gpuError)) {
        log.warning("GPU: " + gpuError);
    }

    return true;
}

void SystemSensors::initializeCpu(Log& log)
{
    m_nominalCpuMhz = readNominalCpuMhz();
    // Частота в PDH отдаётся в процентах от номинала; без номинала показывать нечего.
    m_availability.cpuClock = m_nominalCpuMhz != 0
        && m_cpuPerformance.open(L"\\Processor Information(_Total)\\% Processor Performance");
    m_availability.cpu = true;

    // Даже без PDH частоту отдаст powrprof или номинал из реестра — строка не останется пустой.
    log.info(m_availability.cpuClock ? "CPU: частота через PDH % Processor Performance"
                                     : "CPU: PDH-частоты нет, используем powrprof/номинал");

    // Загрузка по ядрам не требует PDH: берём её из системного вызова, который
    // отдаёт время простоя и полное время каждого логического процессора.
    if (sampleCoreTimes()) {
        m_coreCount = m_coreTimeCount;
        m_hasCoreTimes = true;
        m_previousCoreIdle = m_coreIdleTicks;
        m_previousCoreTotal = m_coreTotalTicks;
        log.info("CPU: загрузка ядер через NtQuerySystemInformation, потоков " + std::to_string(m_coreCount));
    } else {
        // Код возврата в логе: без него причину «датчиков нет» на чужой машине не видно.
        char message[96]{};
        std::snprintf(message, sizeof(message), "CPU: время по ядрам недоступно (status 0x%08lX, %lu байт)",
            static_cast<unsigned long>(m_coreQueryStatus) & 0xFFFFFFFFul, m_coreQueryWritten);
        log.warning(message);
    }

    // Температура: ACPI-термозона читается через PDH, драйвер не нужен.
    // Счётчик есть не на каждой машине — тогда пробуем WMI (см. queryWmiTemperature).
    if (m_thermalZone.open(L"\\Thermal Zone Information(*)\\Temperature")) {
        log.info("CPU: термозона ACPI подключена");
    } else {
        log.info("CPU: PDH-термозона молчит, включаем WMI MSAcpi_ThermalZoneTemperature");
        double celsius = 0.0;
        if (queryWmiTemperature(celsius)) {
            m_wmiTemperature = celsius;
            m_wmiAvailable = true;
            m_availability.cpuTemperature = true;
            log.info("CPU: температура через WMI доступна");
        } else {
            log.info("CPU: температура недоступна без драйвера MSR");
        }
    }
}

bool SystemSensors::initializeGpu(Log& log, std::string& error)
{
    if (!m_adapters.refresh(error)) {
        return false;
    }

    m_availability.gpuLoad = m_gpuEngine.open(L"\\GPU Engine(*)\\Utilization Percentage");
    if (!m_availability.gpuLoad) {
        log.warning("GPU: счётчик GPU Engine недоступен, загрузка будет пустой");
    }

    m_activeGpu = m_adapters.registry().preferredIndex();

    AdapterMemoryInfo memory;
    m_availability.gpuMemory = m_adapters.queryMemory(m_activeGpu, memory);

    std::string nvmlError;
    if (m_nvml.initialize(nvmlError)) {
        m_nvml.selectDevice(activeGpuName());
        const GpuSensorReading reading = m_nvml.read();
        m_availability.gpuTemperature = reading.available;
        m_availability.gpuPower = reading.powerWatts > 0.0;
        log.info("GPU: NVML подключён, температура доступна");
    } else {
        log.info("GPU: NVML недоступен — " + nvmlError);
    }

    return true;
}

void SystemSensors::shutdown()
{
    m_nvml.shutdown();
}

void SystemSensors::selectGpu(std::size_t index)
{
    if (index >= m_adapters.registry().count()) {
        return;
    }

    m_activeGpu = index;
    m_gpuLoad = 0.0;

    AdapterMemoryInfo memory;
    m_availability.gpuMemory = m_adapters.queryMemory(m_activeGpu, memory);

    if (m_nvml.isAvailable()) {
        m_nvml.selectDevice(activeGpuName());
        m_availability.gpuTemperature = m_nvml.read().available;
    }
}

std::wstring SystemSensors::activeGpuName() const
{
    const GpuAdapter* adapter = m_adapters.registry().byIndex(m_activeGpu);
    return adapter != nullptr ? adapter->name : std::wstring();
}

bool SystemSensors::sample(SystemSnapshot& snapshot)
{
    sampleCpu(snapshot);
    sampleMemory(snapshot);
    sampleDiskAndNetwork(snapshot);
    sampleGpu(snapshot);
    sampleSelf(snapshot);
    return true;
}

void SystemSensors::sampleCpu(SystemSnapshot& snapshot)
{
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) {
        return;
    }

    // В kernel уже входит idle, поэтому суммарное время — kernel + user.
    snapshot.cpuIdleTicks = fileTimeToTicks(idle);
    snapshot.cpuTotalTicks = fileTimeToTicks(kernel) + fileTimeToTicks(user);

    if (m_cpuPerformance.collect()) {
        const double percent = std::clamp(m_cpuPerformance.value(), 0.0, 300.0);
        snapshot.cpuClockMhz = static_cast<double>(m_nominalCpuMhz) * percent / 100.0;
        if (snapshot.cpuClockMhz > 0.0) {
            m_availability.cpuClock = true;
        }
    }
    // PDH-счётчик частоты есть не на каждой машине: тогда берём powrprof.
    if (snapshot.cpuClockMhz <= 0.0) {
        const double mhz = currentClockFromPowerInformation();
        if (mhz > 0.0) {
            snapshot.cpuClockMhz = mhz;
            m_availability.cpuClock = true;
        } else if (m_nominalCpuMhz != 0) {
            // Последний резерв — номинал из реестра: лучше номинал, чем пустая строка.
            snapshot.cpuClockMhz = static_cast<double>(m_nominalCpuMhz);
            m_availability.cpuClock = true;
        }
    }

    sampleGameThread(snapshot);

    // Загрузка ядер: разница времён между двумя опросами, как считает диспетчер задач.
    if (m_hasCoreTimes && sampleCoreTimes()) {
        const std::size_t cores = std::min<std::size_t>(m_coreTimeCount, snapshot.coreLoadPercent.size());
        for (std::size_t index = 0; index < cores; ++index) {
            const std::uint64_t idleTicks = m_coreIdleTicks[index] >= m_previousCoreIdle[index]
                ? m_coreIdleTicks[index] - m_previousCoreIdle[index]
                : 0;
            const std::uint64_t total = m_coreTotalTicks[index] >= m_previousCoreTotal[index]
                ? m_coreTotalTicks[index] - m_previousCoreTotal[index]
                : 0;
            snapshot.coreLoadPercent[index]
                = total > 0 ? std::clamp(100.0 * (1.0 - static_cast<double>(idleTicks) / static_cast<double>(total)),
                    0.0, 100.0)
                            : 0.0;
        }
        snapshot.coreCount = static_cast<std::uint32_t>(cores);
        m_previousCoreIdle = m_coreIdleTicks;
        m_previousCoreTotal = m_coreTotalTicks;
        // Суммарные тики остаются накопительными (GetSystemTimes): сборщик считает
        // загрузку по их разнице, а не по готовым процентам.
    }

    if (m_thermalZone.collect()) {
        double hottest = -1000.0;
        for (const auto& [name, value] : m_thermalZone.values()) {
            (void)name;
            // Значение приходит в десятых долях Кельвина; на части систем — в Кельвинах.
            const double celsius = value > 1000.0 ? value / 10.0 - 273.15 : value - 273.15;
            if (celsius > -20.0 && celsius < 130.0) {
                hottest = std::max(hottest, celsius);
            }
        }
        if (hottest > -20.0) {
            snapshot.cpuTemperature = hottest;
            m_availability.cpuTemperature = true;
        }
    }

    // Запасной источник — WMI. Опрашиваем нечасто: запрос идёт через COM и стоит
    // десятки миллисекунд, а температура меняется медленно.
    if (snapshot.cpuTemperature <= 0.0 && !m_wmiFailed) {
        const std::uint64_t now = GetTickCount64();
        if (!m_wmiAvailable || now - m_wmiQueriedAt > 15000) {
            m_wmiQueriedAt = now;
            double celsius = 0.0;
            if (queryWmiTemperature(celsius)) {
                m_wmiTemperature = celsius;
                m_wmiAvailable = true;
                m_availability.cpuTemperature = true;
            } else if (!m_wmiAvailable) {
                // Один раз не получилось — больше не дёргаем COM каждую секунду.
                m_wmiFailed = true;
            }
        }
        if (m_wmiAvailable) {
            snapshot.cpuTemperature = m_wmiTemperature;
            m_availability.cpuTemperature = true;
        }
    }
}

std::wstring SystemSensors::cpuName() const
{
    static const std::wstring name = [] {
        wchar_t buffer[256]{};
        DWORD size = sizeof(buffer);
        const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE,
            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString", RRF_RT_REG_SZ,
            nullptr, buffer, &size);
        return status == ERROR_SUCCESS ? shortenCpuName(buffer) : std::wstring{};
    }();
    return name;
}

std::uint32_t SystemSensors::cpuThreads() const
{
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors;
}

bool SystemSensors::queryWmiTemperature(double& celsius)
{
    // Драйвер не нужен и не ставится: температура берётся из ACPI-термозон, которые
    // прошивка отдаёт через WMI. Классов несколько — от нового к старому, потому что
    // на разных машинах отвечает только один из них.
    struct WmiSource {
        const wchar_t* query;
        const wchar_t* property;
        // В одних классах значение в десятых долях Кельвина, в других — в Кельвинах.
        bool tenthsOfKelvin;
    };

    static constexpr WmiSource kSources[] = {
        { L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature", L"CurrentTemperature", true },
        { L"SELECT HighPrecisionTemperature FROM Win32_PerfFormattedData_Counters_ThermalZoneInformation",
            L"HighPrecisionTemperature", true },
        { L"SELECT Temperature FROM Win32_PerfFormattedData_Counters_ThermalZoneInformation", L"Temperature",
            false },
    };

    for (const WmiSource& source : kSources) {
        if (queryWmiValue(source.query, source.property, source.tenthsOfKelvin, celsius)) {
            return true;
        }
    }
    return false;
}

bool SystemSensors::queryWmiValue(const wchar_t* query, const wchar_t* property, bool tenthsOfKelvin,
    double& celsius)
{
    // COM инициализируется на потоке один раз за жизнь приложения.
    thread_local const bool comReady = [] {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
    }();
    if (!comReady) {
        return false;
    }

    IWbemLocator* locator = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator)))
        || locator == nullptr) {
        return false;
    }

    IWbemServices* services = nullptr;
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    const HRESULT connect = ns != nullptr
        ? locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services)
        : E_OUTOFMEMORY;
    SysFreeString(ns);
    locator->Release();
    if (FAILED(connect) || services == nullptr) {
        return false;
    }

    CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
        RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

    IEnumWbemClassObject* enumerator = nullptr;
    BSTR language = SysAllocString(L"WQL");
    BSTR wql = SysAllocString(query);
    HRESULT execResult = E_OUTOFMEMORY;
    if (language != nullptr && wql != nullptr) {
        execResult = services->ExecQuery(language, wql, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr, &enumerator);
    }
    SysFreeString(language);
    SysFreeString(wql);
    services->Release();
    if (FAILED(execResult) || enumerator == nullptr) {
        return false;
    }

    double hottest = 0.0;
    for (int index = 0; index < 16; ++index) {
        IWbemClassObject* object = nullptr;
        ULONG returned = 0;
        if (FAILED(enumerator->Next(200, 1, &object, &returned)) || returned == 0 || object == nullptr) {
            break;
        }

        VARIANT value;
        VariantInit(&value);
        if (SUCCEEDED(object->Get(property, 0, &value, nullptr, nullptr))
            && (value.vt == VT_I4 || value.vt == VT_UI4 || value.vt == VT_R8)) {
            const double raw = value.vt == VT_I4
                ? static_cast<double>(value.lVal)
                : (value.vt == VT_UI4 ? static_cast<double>(value.ulVal) : value.dblVal);
            const double parsed = (tenthsOfKelvin ? raw / 10.0 : raw) - 273.15;
            if (parsed > -20.0 && parsed < 130.0) {
                hottest = std::max(hottest, parsed);
            }
        }
        VariantClear(&value);
        object->Release();
    }
    enumerator->Release();

    if (hottest <= 0.0) {
        return false;
    }
    celsius = hottest;
    return true;
}

void SystemSensors::sampleGameThread(SystemSnapshot& snapshot)
{
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return;
    }

    DWORD processId = 0;
    const DWORD threadId = GetWindowThreadProcessId(foreground, &processId);
    if (threadId == 0 || processId == GetCurrentProcessId()) {
        return;
    }

    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, threadId);
    if (thread == nullptr) {
        return;
    }

    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);

    if (GetThreadTimes(thread, &creation, &exit, &kernel, &user)) {
        const std::uint64_t ticks = fileTimeToTicks(kernel) + fileTimeToTicks(user);

        // Первый замер только запоминает состояние: разницу считать ещё не от чего.
        if (threadId == m_gameThreadId && ticks > m_gameThreadTicks && m_gameQueryTime != 0 && frequency.QuadPart > 0) {
            const double seconds = static_cast<double>(counter.QuadPart - m_gameQueryTime)
                / static_cast<double>(frequency.QuadPart);
            if (seconds > 0.0) {
                // 10 000 000 тиков FILETIME — секунда времени потока.
                const double busy = static_cast<double>(ticks - m_gameThreadTicks) / 10000000.0;
                snapshot.gameThreadLoad = std::clamp(busy / seconds * 100.0, 0.0, 100.0);
                m_availability.gameThread = true;
            }
        }

        if (threadId != m_gameThreadId) {
            m_availability.gameThread = false;
        }
        m_gameThreadId = threadId;
        m_gameThreadTicks = ticks;
        m_gameQueryTime = static_cast<std::uint64_t>(counter.QuadPart);
    }

    CloseHandle(thread);
}

void SystemSensors::sampleMemory(SystemSnapshot& snapshot)
{
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return;
    }

    snapshot.memoryTotalBytes = status.ullTotalPhys;
    snapshot.memoryFreeBytes = status.ullAvailPhys;

    PERFORMANCE_INFORMATION performance{};
    performance.cb = sizeof(performance);
    if (GetPerformanceInfo(&performance, sizeof(performance))) {
        snapshot.memoryCacheBytes = static_cast<std::uint64_t>(performance.SystemCache) * performance.PageSize;

        // Своп-файл занят на столько, на сколько выданный объём больше физического.
        const std::uint64_t committed = static_cast<std::uint64_t>(performance.CommitTotal) * performance.PageSize;
        const std::uint64_t limit = static_cast<std::uint64_t>(performance.CommitLimit) * performance.PageSize;
        snapshot.pageFileUsedBytes = committed > status.ullTotalPhys ? committed - status.ullTotalPhys : 0;
        if (snapshot.pageFileUsedBytes > limit) {
            snapshot.pageFileUsedBytes = limit;
        }
        // Полный файл подкачки — разница между лимитом фиксации и физической памятью.
        snapshot.pageFileTotalBytes = limit > status.ullTotalPhys ? limit - status.ullTotalPhys : 0;
        m_availability.pageFile = snapshot.pageFileTotalBytes > 0;
    }
}

void SystemSensors::sampleDiskAndNetwork(SystemSnapshot& snapshot)
{
    if (m_availability.disk) {
        const bool readOk = m_diskRead.collect();
        const bool writeOk = m_diskWrite.collect();
        if (readOk) {
            snapshot.diskReadBytesPerSecond = std::max(0.0, m_diskRead.value());
        }
        if (writeOk) {
            snapshot.diskWriteBytesPerSecond = std::max(0.0, m_diskWrite.value());
        }
    }

    if (!m_availability.network) {
        return;
    }

    if (m_netReceived.collect()) {
        double total = 0.0;
        for (const auto& [name, value] : m_netReceived.values()) {
            if (!isLoopbackName(name)) {
                total += std::max(0.0, value);
            }
        }
        snapshot.netReceivedBytesPerSecond = total;
    }

    if (m_netSent.collect()) {
        double total = 0.0;
        for (const auto& [name, value] : m_netSent.values()) {
            if (!isLoopbackName(name)) {
                total += std::max(0.0, value);
            }
        }
        snapshot.netSentBytesPerSecond = total;
    }
}

void SystemSensors::sampleGpu(SystemSnapshot& snapshot)
{
    const GpuAdapter* adapter = m_adapters.registry().byIndex(m_activeGpu);
    if (adapter == nullptr) {
        return;
    }

    if (m_availability.gpuMemory) {
        AdapterMemoryInfo memory;
        if (m_adapters.queryMemory(m_activeGpu, memory)) {
            snapshot.vramUsedBytes = memory.usageBytes;
            snapshot.vramBudgetBytes = memory.budgetBytes != 0 ? memory.budgetBytes : adapter->dedicatedVideoMemory;
        }
    }

    if (m_availability.gpuLoad && m_gpuEngine.collect()) {
        const std::wstring tag = luidTag(adapter->luidLow, adapter->luidHigh);
        double allEngines = 0.0;
        double threeD = 0.0;
        double encode = 0.0;
        double decode = 0.0;
        bool hasThreeD = false;
        bool hasEncode = false;
        bool hasDecode = false;
        bool anyEngine = false;

        for (const auto& [name, value] : m_gpuEngine.values()) {
            if (name.find(tag) == std::wstring::npos) {
                continue;
            }
            const double utilization = std::clamp(value, 0.0, 100.0);
            allEngines += utilization;
            anyEngine = true;
            if (name.find(L"engtype_3D") != std::wstring::npos) {
                threeD += utilization;
                hasThreeD = true;
            } else if (name.find(L"engtype_VideoEncode") != std::wstring::npos) {
                encode += utilization;
                hasEncode = true;
            } else if (name.find(L"engtype_VideoDecode") != std::wstring::npos) {
                decode += utilization;
                hasDecode = true;
            }
        }

        m_gpuLoad = std::clamp(hasThreeD ? threeD : allEngines, 0.0, 100.0);
        snapshot.gpuLoadPercent = m_gpuLoad;
        // Медиа-движки считаются теми же счётчиками GPU Engine: работает у NVIDIA, AMD и Intel.
        snapshot.gpuEncodePercent = std::clamp(encode, 0.0, 100.0);
        snapshot.gpuDecodePercent = std::clamp(decode, 0.0, 100.0);
        // Счётчик GPU Engine перечисляет движки, которые уже работали: если движки
        // адаптера видны, кодер и декодер существуют, даже когда оба простаивают.
        m_availability.gpuMedia = anyEngine || hasEncode || hasDecode;
    }

    if (m_nvml.isAvailable()) {
        const GpuSensorReading reading = m_nvml.read();
        // Доступность уточняется на каждом опросе: драйвер может ответить позже,
        // чем прошла инициализация, и тогда температура появляется сама.
        m_availability.gpuTemperature = reading.temperature > 0.0;
        m_availability.gpuPower = reading.powerWatts > 0.0;
        snapshot.gpuTemperature = reading.temperature;
        snapshot.gpuClockMhz = reading.graphicsClockMhz;
        snapshot.gpuMemClockMhz = reading.memoryClockMhz;
        snapshot.gpuPowerWatts = reading.powerWatts;
        snapshot.gpuFanRpm = reading.fanRpm;
        snapshot.gpuHotspot = 0.0;

        // PCI-e: только NVML отдаёт скорость обмена и ширину линии. На AMD/Intel
        // честно оставляем «нет данных», а не выдумываем число.
        if (reading.hasPcie && reading.pcieLinkWidth > 0) {
            double laneMegabytes = 0.0;
            switch (std::min(reading.pcieLinkGeneration, 5u)) {
            case 1:
                laneMegabytes = 250.0;
                break;
            case 2:
                laneMegabytes = 500.0;
                break;
            case 3:
                laneMegabytes = 985.0;
                break;
            case 4:
                laneMegabytes = 1969.0;
                break;
            case 5:
                laneMegabytes = 3938.0;
                break;
            default:
                laneMegabytes = 0.0;
                break;
            }
            const double capacity = laneMegabytes * static_cast<double>(reading.pcieLinkWidth);
            const double used = (reading.pcieTxKilobytesPerSecond + reading.pcieRxKilobytesPerSecond) / 1024.0;
            if (capacity > 0.0) {
                snapshot.pcieLoadPercent = std::clamp(used / capacity * 100.0, 0.0, 100.0);
                m_availability.pcie = true;
            }
        }
        // Если PCI-e не читается, но кодеки NVML отвечают — берём их как более точные.
        if (m_availability.gpuMedia) {
            snapshot.gpuEncodePercent = std::max(snapshot.gpuEncodePercent,
                std::clamp(reading.encoderPercent, 0.0, 100.0));
            snapshot.gpuDecodePercent = std::max(snapshot.gpuDecodePercent,
                std::clamp(reading.decoderPercent, 0.0, 100.0));
        } else if (reading.encoderPercent > 0.0 || reading.decoderPercent > 0.0) {
            snapshot.gpuEncodePercent = std::clamp(reading.encoderPercent, 0.0, 100.0);
            snapshot.gpuDecodePercent = std::clamp(reading.decoderPercent, 0.0, 100.0);
            m_availability.gpuMedia = true;
        }
    }
}

void SystemSensors::sampleSelf(SystemSnapshot& snapshot)
{
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        snapshot.selfCpuTicks = fileTimeToTicks(kernel) + fileTimeToTicks(user);
    }

    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        snapshot.selfWorkingSetBytes = counters.WorkingSetSize;
    }

    snapshot.selfThreads = countProcessThreads();
}

} // namespace yawa

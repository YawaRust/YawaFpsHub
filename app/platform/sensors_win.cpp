#include "platform/sensors_win.h"
#include "platform/log.h"
#include "platform/win_util.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <wbemidl.h>

#include <algorithm>
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

    if (!m_availability.cpuClock) {
        log.warning("CPU: частота недоступна, нет счётчика % Processor Performance");
    }

    // Загрузка по ядрам: \Processor Information(0,N)\% Processor Time. Диспетчер задач
    // берёт те же счётчики, поэтому цифры совпадают.
    const DWORD coreCount = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    for (std::size_t index = 0; index < m_coreLoad.size(); ++index) {
        if (index >= coreCount) {
            break;
        }
        const std::wstring path
            = L"\\Processor Information(0," + std::to_wstring(index) + L")\\% Processor Time";
        auto counter = std::make_unique<PdhCounter>();
        if (counter->open(path.c_str())) {
            m_coreLoad[index] = std::move(counter);
            ++m_coreCount;
        }
    }
    if (!m_coreLoad[0]) {
        log.warning("CPU: счётчики по ядрам недоступны");
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

    if (m_availability.cpuClock && m_cpuPerformance.collect()) {
        const double percent = std::clamp(m_cpuPerformance.value(), 0.0, 300.0);
        snapshot.cpuClockMhz = static_cast<double>(m_nominalCpuMhz) * percent / 100.0;
    }

    sampleGameThread(snapshot);

    std::uint32_t cores = 0;
    for (std::size_t index = 0; index < m_coreLoad.size(); ++index) {
        if (m_coreLoad[index] == nullptr || !m_coreLoad[index]->collect()) {
            continue;
        }
        snapshot.coreLoadPercent[index] = std::clamp(m_coreLoad[index]->value(), 0.0, 100.0);
        ++cores;
    }
    snapshot.coreCount = cores;

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

bool SystemSensors::queryWmiTemperature(double& celsius)
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
    BSTR wql = SysAllocString(L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature");
    HRESULT query = E_OUTOFMEMORY;
    if (language != nullptr && wql != nullptr) {
        query = services->ExecQuery(language, wql, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr,
            &enumerator);
    }
    SysFreeString(language);
    SysFreeString(wql);
    services->Release();
    if (FAILED(query) || enumerator == nullptr) {
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
        if (SUCCEEDED(object->Get(L"CurrentTemperature", 0, &value, nullptr, nullptr))
            && (value.vt == VT_I4 || value.vt == VT_UI4)) {
            // Значение приходит в десятых долях Кельвина.
            const double kelvinTenths = value.vt == VT_I4 ? static_cast<double>(value.lVal)
                                                          : static_cast<double>(value.ulVal);
            const double parsed = kelvinTenths / 10.0 - 273.15;
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
        bool hasThreeD = false;

        for (const auto& [name, value] : m_gpuEngine.values()) {
            if (name.find(tag) == std::wstring::npos) {
                continue;
            }
            const double utilization = std::clamp(value, 0.0, 100.0);
            allEngines += utilization;
            if (name.find(L"engtype_3D") != std::wstring::npos) {
                threeD += utilization;
                hasThreeD = true;
            }
        }

        m_gpuLoad = std::clamp(hasThreeD ? threeD : allEngines, 0.0, 100.0);
        snapshot.gpuLoadPercent = m_gpuLoad;
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

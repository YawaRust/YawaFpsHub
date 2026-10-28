#include "platform/sensors_win.h"
#include "platform/log.h"
#include "platform/win_util.h"

#include <psapi.h>
#include <tlhelp32.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <string_view>
#include <vector>

namespace yawa {
namespace {

std::uint64_t fileTimeToTicks(const FILETIME& time)
{
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

bool containsCounterPart(std::wstring_view name, std::wstring_view part)
{
    return std::search(name.begin(), name.end(), part.begin(), part.end(), [](wchar_t left, wchar_t right) {
        return std::towlower(left) == std::towlower(right);
    }) != name.end();
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
    m_pawnio.initialize(log);
    m_availability.cpuHardware = m_pawnio.failure();
    m_memoryTimings = m_pawnio.readMemoryTimings();
    if (m_memoryTimings) {
        log.info("UMC: AMD DDR4 (Matisse/Cezanne), два канала согласованы или заселён один; "
                 "аппаратная проверка требуется");
    }
    m_storage.initialize(log);
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
    m_availability.cpuTemperature = m_pawnio.read().temperature.has_value();
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

    if (!m_availability.cpuTemperature) {
        log.info("CPU: аппаратная температура недоступна (нужен PawnIO и права администратора); "
                 "ACPI-термозона не является температурой кристалла");
    }
}

bool SystemSensors::initializeGpu(Log& log, std::string& error)
{
    if (!m_adapters.refresh(error)) {
        return false;
    }

    m_availability.gpuLoad = m_gpuEngine.open(L"\\GPU Engine(*)\\Utilization Percentage");
    if (!m_gpuAdapterMemory.open(L"\\GPU Adapter Memory(*)\\Dedicated Usage")) {
        log.warning("GPU: счётчик занятой видеопамяти недоступен");
    }
    if (!m_availability.gpuLoad) {
        log.warning("GPU: счётчик GPU Engine недоступен, загрузка будет пустой");
    }

    m_activeGpu = m_adapters.registry().preferredIndex();

    const GpuAdapter* active = m_adapters.registry().byIndex(m_activeGpu);
    m_availability.gpuMemory
        = active != nullptr && (active->dedicatedVideoMemory != 0 || m_gpuAdapterMemory.isOpen());

    std::string nvmlError;
    if (m_nvml.initialize(nvmlError)) {
        log.info("GPU: NVML подключён");
    } else {
        log.info("GPU: NVML недоступен — " + nvmlError);
    }
    m_vendorGpu.initialize(m_adapters.registry(), log);
    selectGpuSensors();

    return true;
}

void SystemSensors::shutdown()
{
    m_storage.shutdown();
    m_vendorGpu.shutdown();
    m_pawnio.shutdown();
    m_nvml.shutdown();
}

void SystemSensors::selectGpu(std::size_t index)
{
    if (index >= m_adapters.registry().count()) {
        return;
    }

    m_activeGpu = index;
    m_gpuLoad = 0.0;

    const GpuAdapter* active = m_adapters.registry().byIndex(m_activeGpu);
    m_availability.gpuMemory
        = active != nullptr && (active->dedicatedVideoMemory != 0 || m_gpuAdapterMemory.isOpen());

    selectGpuSensors();
}

void SystemSensors::selectGpuSensors()
{
    const GpuAdapter* adapter = m_adapters.registry().byIndex(m_activeGpu);
    m_nvml.selectDevice(adapter && adapter->vendor == GpuVendor::Nvidia ? adapter->name : L"");
    const auto& adapters = m_adapters.registry().adapters();
    const auto count = adapter ? std::count_if(adapters.begin(), adapters.end(),
                                     [adapter](const auto& item) { return item.vendor == adapter->vendor; })
                               : 0;
    m_vendorGpu.selectDevice(adapter, count == 1);
    const GpuSensorReading reading
        = adapter && adapter->vendor == GpuVendor::Nvidia ? m_nvml.read() : m_vendorGpu.read();
    m_availability.gpuTemperature = reading.temperature > 0;
    m_availability.gpuPower = reading.powerWatts > 0;
    m_availability.gpuMedia = false;
    m_availability.pcie = false;
}

std::wstring SystemSensors::activeGpuName() const
{
    const GpuAdapter* adapter = m_adapters.registry().byIndex(m_activeGpu);
    return adapter != nullptr ? adapter->name : std::wstring();
}

bool SystemSensors::sample(SystemSnapshot& snapshot, std::uint32_t gamePid)
{
    sampleCpu(snapshot);
    sampleMemory(snapshot);
    sampleDiskAndNetwork(snapshot);
    sampleGpu(snapshot);
    sampleGameProcess(snapshot, gamePid);
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

    m_availability.cpuTemperature = false;
    const CpuHardwareReading hardware = m_pawnio.read();
    snapshot.cpuTemperature = hardware.temperature.value_or(0.0);
    snapshot.cpuPowerWatts = hardware.powerWatts.value_or(0.0);
    if (hardware.temperature) {
        m_availability.cpuTemperature = true;
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
    return GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
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
    snapshot.diskTemperature = m_storage.temperature().value_or(0.0);
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

    // DedicatedVideoMemory описывает выделенную память адаптера. DXGI Budget — лимит
    // процесса, на iGPU он часто относится к общей RAM и не является объёмом VRAM.
    snapshot.vramBudgetBytes = adapter->dedicatedVideoMemory;
    snapshot.vramUsageKnown = false;
    if (m_gpuAdapterMemory.collect()) {
        const std::wstring tag = luidTag(adapter->luidLow, adapter->luidHigh);
        double bytes = 0.0;
        bool found = false;
        for (const auto& [name, usage] : m_gpuAdapterMemory.values()) {
            if (containsCounterPart(name, tag) && usage >= 0.0) {
                bytes += usage;
                found = true;
            }
        }
        if (found) {
            snapshot.vramUsedBytes = static_cast<std::uint64_t>(bytes);
            snapshot.vramUsageKnown = true;
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
            if (!containsCounterPart(name, tag)) {
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

    {
        const GpuSensorReading reading
            = adapter->vendor == GpuVendor::Nvidia ? m_nvml.read() : m_vendorGpu.read();
        m_availability.pcie = false;
        if (adapter->vendor == GpuVendor::Nvidia && reading.hasMemory) {
            snapshot.vramUsedBytes = reading.memoryUsedBytes;
            snapshot.vramUsageKnown = true;
            snapshot.vramBudgetBytes = reading.memoryTotalBytes;
        }
        // Доступность уточняется на каждом опросе: драйвер может ответить позже,
        // чем прошла инициализация, и тогда температура появляется сама.
        m_availability.gpuMemory = snapshot.vramUsageKnown || snapshot.vramBudgetBytes > 0;
        m_availability.gpuTemperature = reading.temperature > 0.0;
        m_availability.gpuPower = reading.powerWatts > 0.0;
        snapshot.gpuTemperature = reading.temperature;
        snapshot.gpuClockMhz = reading.graphicsClockMhz;
        snapshot.gpuMemClockMhz = reading.memoryClockMhz;
        snapshot.gpuPowerWatts = reading.powerWatts;
        snapshot.gpuFanRpm = reading.fanRpm;
        snapshot.gpuHotspot = reading.hotspot;

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

void SystemSensors::sampleGameProcess(SystemSnapshot& snapshot, std::uint32_t gamePid)
{
    if (gamePid == 0) {
        m_gameProcessId = 0;
        m_gameThreadTimes.clear();
        return;
    }
    const GpuAdapter* adapter = m_adapters.registry().byIndex(m_activeGpu);
    if (adapter != nullptr && m_availability.gpuLoad) {
        const std::wstring luid = luidTag(adapter->luidLow, adapter->luidHigh);
        const std::wstring process = L"pid_" + std::to_wstring(gamePid) + L"_";
        double gpuLoad = 0.0;
        bool found = false;
        for (const auto& [name, value] : m_gpuEngine.values()) {
            if (containsCounterPart(name, luid) && containsCounterPart(name, process)
                && containsCounterPart(name, L"engtype_3D")) {
                gpuLoad += std::clamp(value, 0.0, 100.0);
                found = true;
            }
        }
        if (found) {
            snapshot.gameGpuLoad = std::clamp(gpuLoad, 0.0, 100.0);
        }
    }

    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, gamePid);
    if (process == nullptr) {
        m_gameProcessId = 0;
        m_gameThreadTimes.clear();
        return;
    }
    FILETIME creation { };
    FILETIME exit { };
    FILETIME kernel { };
    FILETIME user { };
    const bool read = GetProcessTimes(process, &creation, &exit, &kernel, &user) != FALSE;
    CloseHandle(process);
    if (!read) {
        m_gameProcessId = 0;
        m_gameThreadTimes.clear();
        return;
    }
    const std::uint64_t ticks = fileTimeToTicks(kernel) + fileTimeToTicks(user);
    const std::uint64_t created = fileTimeToTicks(creation);
    const std::uint64_t now = GetTickCount64();
    if (m_gameProcessId == gamePid && m_gameProcessCreated == created && now > m_gameProcessTime
        && ticks >= m_gameProcessTicks) {
        const double seconds = static_cast<double>(now - m_gameProcessTime) / 1000.0;
        snapshot.gameCpuLoad = processCpuPercent(ticks - m_gameProcessTicks, seconds, cpuThreads());
    }
    sampleGameThreads(snapshot, gamePid, now);
    m_gameProcessId = gamePid;
    m_gameProcessCreated = created;
    m_gameProcessTicks = ticks;
    m_gameProcessTime = now;
}

void SystemSensors::sampleGameThreads(SystemSnapshot& snapshot, std::uint32_t gamePid, std::uint64_t now)
{
    const HANDLE threads = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (threads == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 entry { };
    entry.dwSize = sizeof(entry);
    std::vector<GameThreadSample> current;
    double peak = -1.0;
    const double seconds = static_cast<double>(now - m_gameThreadsTime) / 1000.0;
    if (Thread32First(threads, &entry)) {
        do {
            if (entry.th32OwnerProcessID != gamePid || current.size() >= 512) {
                continue;
            }
            const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
            if (thread == nullptr) {
                continue;
            }
            FILETIME creation { };
            FILETIME exit { };
            FILETIME kernel { };
            FILETIME user { };
            const bool read = GetThreadTimes(thread, &creation, &exit, &kernel, &user) != FALSE;
            CloseHandle(thread);
            if (!read) {
                continue;
            }
            const std::uint64_t ticks = fileTimeToTicks(kernel) + fileTimeToTicks(user);
            const std::uint64_t created = fileTimeToTicks(creation);
            current.push_back(GameThreadSample { entry.th32ThreadID, created, ticks });
            if (m_gameProcessId != gamePid || m_gameThreadsTime == 0 || seconds <= 0.0) {
                continue;
            }
            const auto old = std::find_if(m_gameThreadTimes.begin(), m_gameThreadTimes.end(),
                [&](const GameThreadSample& item) { return item.id == entry.th32ThreadID; });
            if (old != m_gameThreadTimes.end() && created == old->created && ticks >= old->busy) {
                peak = std::max(peak, processCpuPercent(ticks - old->busy, seconds, 1));
            }
        } while (Thread32Next(threads, &entry));
    }
    CloseHandle(threads);
    if (peak >= 0.0) {
        snapshot.gamePeakThreadLoad = peak;
    }
    m_gameThreadTimes = std::move(current);
    m_gameThreadsTime = now;
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

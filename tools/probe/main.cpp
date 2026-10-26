// yawa-probe: диагностика датчиков на конкретной машине.
// Показывает, какие источники поднялись, и печатает те же значения, что пойдут в интерфейс.

#include "metrics/bottleneck.h"
#include "metrics/frame_tracker.h"
#include "metrics/system_sampler.h"
#include "metrics/units.h"
#include "platform/etw_frames.h"
#include "platform/hardware_info_win.h"
#include "platform/log.h"
#include "platform/sensors_win.h"
#include "platform/win_util.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

constexpr double kDefaultTargetFps = 120.0;

struct Options {
    int seconds = 10;
    int intervalMs = 1000;
    bool useEtw = true;
    long gpuIndex = -1;
    std::string logPath;
};

Options parseArgs(int argc, wchar_t** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring arg = argv[index];
        const bool hasValue = index + 1 < argc;

        if (arg == L"--seconds" && hasValue) {
            options.seconds = _wtoi(argv[++index]);
        } else if (arg == L"--interval" && hasValue) {
            options.intervalMs = _wtoi(argv[++index]);
        } else if (arg == L"--gpu" && hasValue) {
            options.gpuIndex = _wtoi(argv[++index]);
        } else if (arg == L"--log" && hasValue) {
            options.logPath = yawa::narrow(argv[++index]);
        } else if (arg == L"--no-etw") {
            options.useEtw = false;
        }
    }

    if (options.intervalMs < 100) {
        options.intervalMs = 100;
    }
    if (options.seconds < 1) {
        options.seconds = 1;
    }
    return options;
}

std::string executableDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);

    std::wstring full(path);
    const std::size_t slash = full.find_last_of(L"\\/");
    return yawa::narrow(slash == std::wstring::npos ? full : full.substr(0, slash));
}

// Паспорт машины и вердикт по узкому месту: на разных GPU и CPU нужно видеть, что
// определилось и что приложение собралось показывать.
void printMachine(const yawa::SystemSensors& sensors, const yawa::MetricStore& store, double targetFps)
{
    yawa::HardwareIdentity info;
    std::string error;
    if (!yawa::collectHardwareInfo(info, sensors.gpus(), error)) {
        std::printf("паспорт железа: %s\n\n", error.c_str());
        return;
    }

    std::printf("железо\n");
    std::printf("  ОС: %s, сборка %s (%s), %s\n", info.os.product.c_str(), info.os.build.c_str(),
        info.os.featureVersion.empty() ? "версия не указана" : info.os.featureVersion.c_str(),
        info.os.architecture.c_str());
    std::printf("  CPU: %s — ядер %u, потоков %u, %.0f МГц, L3 %u МБ\n", info.cpu.name.c_str(),
        info.cpu.physicalCores, info.cpu.logicalProcessors, info.cpu.baseMhz, info.cpu.l3CacheMb);
    for (const yawa::GpuIdentity& gpu : info.gpus) {
        std::printf("  GPU: %s%s — %.0f МБ, драйвер %s, индекс %.0f\n", gpu.name.c_str(),
            gpu.discrete ? "" : " (встроенная)", gpu.dedicatedMb,
            gpu.driverVersion.empty() ? "нет данных" : gpu.driverVersion.c_str(),
            yawa::gpuPerfIndex(gpu.name, gpu.dedicatedMb));
    }
    std::printf("  RAM: %s\n", yawa::describeMemory(info.memory).c_str());
    const double tck = info.memory.cycleTimeNs();
    std::printf("       частота %.0f МТ/с (паспорт %.0f), tCK %.3f нс, XMP/EXPO: не определяется из SMBIOS\n",
        info.memory.configuredMt, info.memory.ratedMt, tck);
    std::printf("       тайминги SPD (tCL-tRCD-tRP-tRAS) без драйвера недоступны — показываем частоты\n");
    for (const yawa::DisplayIdentity& display : info.displays) {
        std::printf("  экран: %s%s\n", yawa::describeDisplay(display).c_str(), display.primary ? " (основной)" : "");
    }

    const yawa::BottleneckResult balance
        = yawa::analyzeBottleneck(info, yawa::loadFromStore(store, targetFps));
    std::printf("  баланс: %s — %u%% (%s)\n", balance.headline.c_str(), balance.percent,
        balance.measured ? "по живым замерам" : "прогноз по индексу");
    for (const yawa::BottleneckAdvice& advice : balance.advice) {
        std::printf("    · %s — %s\n", advice.title.c_str(), advice.text.c_str());
    }
    std::printf("\n");
}

void printHeader(const yawa::SystemSensors& sensors)
{
    const yawa::SensorAvailability& state = sensors.availability();
    const auto mark = [](bool value) { return value ? "да" : "нет"; };

    std::printf("YawaFpsHub probe\n");
    std::printf("  CPU: нагрузка %s, частота %s, температура %s\n", mark(state.cpu), mark(state.cpuClock),
        mark(state.cpuTemperature));
    std::printf("  Память %s, диск %s, сеть %s\n", mark(state.memory), mark(state.disk), mark(state.network));
    std::printf("  GPU: нагрузка %s, видеопамять %s, температура %s, питание %s\n", mark(state.gpuLoad),
        mark(state.gpuMemory), mark(state.gpuTemperature), mark(state.gpuPower));
    std::printf("  Медиа-движки %s, PCI-e %s, файл подкачки %s\n", mark(state.gpuMedia), mark(state.pcie),
        mark(state.pageFile));

    const yawa::GpuRegistry& gpus = sensors.gpus();
    for (std::size_t index = 0; index < gpus.count(); ++index) {
        const yawa::GpuAdapter* adapter = gpus.byIndex(index);
        if (adapter == nullptr) {
            continue;
        }
        std::printf("  адаптер %zu%s: %s (%llu МБ)%s\n", index,
            index == sensors.activeGpu() ? " *" : "", yawa::narrow(adapter->name).c_str(),
            static_cast<unsigned long long>(adapter->dedicatedVideoMemory / (1024 * 1024)),
            adapter->isSoftware ? " — программный" : "");
    }

    if (!state.cpuTemperature) {
        std::printf("  температура CPU: нужен драйвер доступа к датчикам, показываем «нет данных»\n");
    }
    std::printf("\n");
}

std::string formatLine(const yawa::MetricStore& store)
{
    const auto value = [&store](yawa::MetricId id) {
        return yawa::formatMetric(id, store.value(id));
    };

    std::string line;
    line += "CPU " + value(yawa::MetricId::CpuLoad) + " " + value(yawa::MetricId::CpuClock);
    line += " | GPU " + value(yawa::MetricId::GpuLoad);
    if (store.value(yawa::MetricId::GpuTemp) > 0.0) {
        line += " " + value(yawa::MetricId::GpuTemp);
    }
    if (store.value(yawa::MetricId::GpuPower) > 0.0) {
        line += " " + value(yawa::MetricId::GpuPower);
    }
    line += " | RAM " + value(yawa::MetricId::RamUsed) + " / " + value(yawa::MetricId::RamFree) + " своб.";
    line += " | FPS " + value(yawa::MetricId::Fps) + " (1% " + value(yawa::MetricId::Low1Fps) + ")";
    line += " | Диск " + value(yawa::MetricId::DiskRead) + " / " + value(yawa::MetricId::DiskWrite);
    line += " | Сеть " + value(yawa::MetricId::NetDown) + " / " + value(yawa::MetricId::NetUp);
    if (store.hasValue(yawa::MetricId::GpuEncode) || store.hasValue(yawa::MetricId::GpuDecode)) {
        line += " | Кодек " + value(yawa::MetricId::GpuEncode) + " / " + value(yawa::MetricId::GpuDecode);
    }
    if (store.hasValue(yawa::MetricId::PcieLoad)) {
        line += " | PCI-e " + value(yawa::MetricId::PcieLoad);
    }
    line += " | ОЗУ " + value(yawa::MetricId::RamLoad) + ", подкачка " + value(yawa::MetricId::PageFile) + " / "
        + value(yawa::MetricId::PageFileTotal) + " (" + value(yawa::MetricId::PageFileLoad) + ")";
    return line;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    const Options options = parseArgs(argc, argv);

    yawa::Log log;
    const std::string logPath = options.logPath.empty() ? executableDirectory() + "\\yawa-probe.log" : options.logPath;
    if (!log.open(logPath)) {
        std::printf("не удалось открыть лог %s\n", logPath.c_str());
    }

    yawa::SystemSensors sensors;
    std::string error;
    if (!sensors.initialize(log, error)) {
        std::printf("датчики не поднялись: %s\n", error.c_str());
        log.error("sensors: " + error);
        return 1;
    }

    if (options.gpuIndex >= 0) {
        sensors.selectGpu(static_cast<std::size_t>(options.gpuIndex));
    }
    printHeader(sensors);

    yawa::EtwFrameSession etw;
    if (options.useEtw && !etw.start(error)) {
        std::printf("ETW не запущена: %s — FPS будет недоступен\n\n", error.c_str());
        log.warning("etw: " + error);
    }

    yawa::FrameTracker tracker(yawa::EtwFrameSession::qpcFrequency());
    yawa::SystemSampler sampler;
    yawa::MetricStore store;

    const auto start = std::chrono::steady_clock::now();
    auto previous = start;
    int sampleIndex = 0;

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(options.intervalMs));

        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - previous).count();
        previous = now;

        etw.drain(tracker);

        yawa::SystemSnapshot snapshot;
        sensors.sample(snapshot);

        const yawa::SystemSampler::Deltas deltas = sampler.compute(snapshot, elapsed);
        const yawa::DerivedValues values = yawa::deriveValues(snapshot, deltas);
        yawa::applySnapshot(store, snapshot, values, tracker.summary(kDefaultTargetFps), 40);

        ++sampleIndex;
        std::printf("[%3d] %s\n", sampleIndex, formatLine(store).c_str());

        const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (total >= static_cast<double>(options.seconds)) {
            break;
        }
    }

    printMachine(sensors, store, kDefaultTargetFps);

    std::printf("\nкадров из ETW: %llu, отклонено событий: %llu, потеряно сэмплов: %llu, потоков: %zu\n",
        static_cast<unsigned long long>(etw.eventsReceived()),
        static_cast<unsigned long long>(etw.eventsRejected()),
        static_cast<unsigned long long>(etw.samplesDropped()), tracker.streamCount());

    etw.stop();
    sensors.shutdown();
    return 0;
}

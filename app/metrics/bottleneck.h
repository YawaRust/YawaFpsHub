#pragma once

#include "metrics/hardware_info.h"
#include "metrics/metric_store.h"

#include <cstdint>
#include <string>
#include <vector>

namespace yawa {

// Индекс производительности относительно условного флагмана (RTX 4090 и CPU
// уровня Ryzen 9 7900X = 100). Значения — обобщение публичных обзоров, они нужны
// только для прогноза, когда игра не запущена: живые замеры точнее любого индекса.
double cpuPerfIndex(std::string_view name, std::uint32_t cores, std::uint32_t threads);
double gpuPerfIndex(std::string_view name, double dedicatedMb);

// Чем именно узкое место и насколько сильно.
enum class BottleneckKind : std::uint8_t {
    Idle,           // нагрузки нет — судить не о чем
    LiveCpu,        // упор в процессор по живым замерам
    LiveGpu,        // упор в видеокарту по живым замерам
    LiveBalanced,   // оба компонента загружены ровно
    VramLimited,    // закончилась видеопамять
    RefreshLimited, // кадры ограничены герцовкой или вертикальной синхронизацией
    EstimateCpu,    // прогноз по индексу: упирается в CPU
    EstimateGpu,    // прогноз по индексу: упирается в GPU
    EstimateBalanced,
    NoData,
};

struct BottleneckAdvice {
    std::string title;
    std::string text;
};

struct BottleneckResult {
    BottleneckKind kind = BottleneckKind::NoData;
    // 0…100 — насколько сильно узкое место: 0 — связка ровная, 100 — второй компонент простаивает.
    std::uint32_t percent = 0;
    std::string headline;
    std::string detail;
    // Прогноз кадров, к которому стремится узкий компонент.
    double cpuCapacityFps = 0.0;
    double gpuCapacityFps = 0.0;
    double expectedFps = 0.0;
    bool measured = false;
    std::vector<BottleneckAdvice> advice;
};

// Живые показатели за последний интервал опроса. Значения < 0 означают «датчик молчит»:
// тогда расчёт уходит в прогноз по индексу, а не в ноль.
struct SystemLoad {
    double cpuLoad = -1.0;
    double cpuPeakCore = -1.0;
    double gpuLoad = -1.0;
    double fps = -1.0;
    double vramUsedGb = -1.0;
    double vramTotalGb = -1.0;
    double ramLoad = -1.0;
    double pageFileLoad = -1.0;
    double cpuPower = -1.0;
    double gpuPower = -1.0;
    double targetFps = 60.0;
    bool vsyncLikeLimit = false;
};

BottleneckResult analyzeBottleneck(const HardwareIdentity& hardware, const SystemLoad& load);

// Собирает нагрузку из хранилища метрик. Датчик, который ни разу не приходил,
// остаётся со значением < 0: тогда расчёт идёт по индексу железа, а не по нулям.
SystemLoad loadFromStore(const MetricStore& store, double targetFps);

} // namespace yawa

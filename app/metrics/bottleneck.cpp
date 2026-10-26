#include "metrics/bottleneck.h"

#include "metrics/units.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace yawa {
namespace {

std::string upper(std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    for (char symbol : text) {
        result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(symbol))));
    }
    return result;
}

// Первое число длиной 3–5 знаков: 12400 из «Core i5-12400F», 7800 из «Ryzen 7 7800X3D».
// Младшие цифры — модель, старшая — поколение: именно так у Intel и AMD устроены имена.
long firstModelNumber(const std::string& text)
{
    std::size_t index = 0;
    while (index < text.size()) {
        if (!std::isdigit(static_cast<unsigned char>(text[index]))) {
            ++index;
            continue;
        }
        std::size_t end = index;
        while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) {
            ++end;
        }
        const std::size_t length = end - index;
        if (length >= 3 && length <= 5) {
            long value = 0;
            for (std::size_t digit = index; digit < end; ++digit) {
                value = value * 10 + (text[digit] - '0');
            }
            return value;
        }
        index = end;
    }
    return 0;
}

int firstDigit(const std::string& text, const char* after)
{
    const std::size_t anchor = after != nullptr ? text.find(after) : 0;
    if (anchor == std::string::npos) {
        return 0;
    }
    for (std::size_t index = anchor; index < text.size(); ++index) {
        if (std::isdigit(static_cast<unsigned char>(text[index]))) {
            return text[index] - '0';
        }
    }
    return 0;
}

double clampPercent(double value)
{
    return std::clamp(value, 0.0, 100.0);
}

// Проценты везде печатаем так же, как датчики: «44 %», а не «44%».
std::string percentText(double value)
{
    return formatWithUnit(value, MetricUnit::Percent, 0);
}

// Поколение Intel по номеру модели: 12400 → 12, 9600 → 9, 7400 → 7.
double intelGenerationIndex(long model)
{
    const int generation = static_cast<int>(model / 1000);
    switch (generation) {
    case 6:
        return 46.0;
    case 7:
        return 50.0;
    case 8:
        return 55.0;
    case 9:
        return 60.0;
    case 10:
        return 66.0;
    case 11:
        return 74.0;
    case 12:
        return 88.0;
    case 13:
        return 94.0;
    case 14:
        return 99.0;
    default:
        break;
    }
    if (generation > 14) {
        return 96.0; // Core Ultra 200S: IPC выше, но игровая производительность — на уровне 14-го
    }
    return 40.0;
}

double amdGenerationIndex(long model)
{
    const int generation = static_cast<int>(model / 1000);
    switch (generation) {
    case 1:
        return 54.0;
    case 2:
        return 60.0;
    case 3:
        return 70.0;
    case 5:
        return 85.0;
    case 7:
        return 95.0;
    case 9:
        return 100.0;
    default:
        break;
    }
    return generation > 9 ? 100.0 : 48.0;
}

double tierFactor(int tier)
{
    switch (tier) {
    case 3:
        return 0.86;
    case 5:
        return 0.95;
    case 7:
        return 1.0;
    case 9:
        return 1.03;
    default:
        return 0.95;
    }
}

struct GpuFamily {
    const char* needle;
    double base;
};

// База — индекс флагмана семейства. Дальше он масштабируется младшими цифрами модели,
// поэтому «RTX 4060» и «RTX 4090» попадают в один ряд, но с разным весом.
constexpr GpuFamily kGpuFamilies[] = {
    { "GTX 10", 40.0 },   { "RTX 20", 62.0 },  { "RTX 30", 88.0 },   { "RTX 40", 122.0 }, { "RTX 50", 152.0 },
    { "RX 5", 50.0 },     { "RX 6", 92.0 },     { "RX 7", 124.0 },    { "RX 9", 150.0 },   { "ARC A", 55.0 },
    { "ARC B", 82.0 },    { "INTEL UHD", 12.0 }, { "IRIS", 18.0 },     { "RADEON GRAPHICS", 20.0 },
};

double gpuTierFactor(long model)
{
    const int tier = static_cast<int>(model % 1000 / 10) * 10; // 60, 70, 80, 90
    switch (tier) {
    case 50:
        return 0.38;
    case 60:
        return 0.46;
    case 70:
        return 0.62;
    case 80:
        return 0.8;
    case 90:
        return 1.0;
    default:
        break;
    }
    if (tier >= 950) {
        return 1.0;
    }
    return 0.55;
}

void pushAdvice(BottleneckResult& result, const char* title, const std::string& text)
{
    if (result.advice.size() >= 5) {
        return;
    }
    result.advice.push_back(BottleneckAdvice{ title, text });
}

void cpuAdvice(const HardwareIdentity& hardware, BottleneckResult& result)
{
    const std::uint32_t threads = hardware.cpu.logicalProcessors;
    pushAdvice(result, "Повысить разрешение или включить DSR/VSR",
        "Пока узкое место — процессор, кадры ограничивает не видеокарта. Перенос нагрузки "
        "на GPU (разрешение выше на ступень) почти не меняет средний FPS, но выравнивает frame time.");
    pushAdvice(result, "Снизить настройки, которые считает процессор",
        "Дальность прорисовки, плотность растительности и NPC, физика и тени — нагрузки CPU. "
        "Текстуры и фильтрация при упоре в процессор FPS не добавляют.");
    pushAdvice(result, "Ограничить FPS на 5–10 % ниже максимума",
        "Процессор перестанет работать на пределе, пропадут провалы: 1 % low поднимется ближе к среднему.");
    if (threads > 0 && threads <= 8) {
        pushAdvice(result, "Игра упирается в малое число потоков",
            "На " + std::to_string(threads) + " потоках движку не хватает однопоточной скорости. "
            "В киберспортивных играх больше всего даёт CPU с большим кэшем (X3D), а не лишние ядра.");
    }
}

void gpuAdvice(const HardwareIdentity& hardware, const SystemLoad& load, BottleneckResult& result)
{
    pushAdvice(result, "Включить DLSS, FSR или XeSS в режиме Quality",
        "Рендер в более низком разрешении снимает нагрузку с видеокарты: +20…50 % кадров "
        "с минимальной потерей качества.");
    pushAdvice(result, "Снизить тяжёлые для GPU эффекты",
        "Трассировка лучей, глобальное освещение, качество теней и разрешение экранов "
        "водятся видеокартой — их снижение даёт больше всего кадров.");
    if (load.vramTotalGb > 0.0 && load.vramUsedGb > 0.0 && load.vramUsedGb > load.vramTotalGb * 0.85) {
        const double free = std::max(0.0, load.vramTotalGb - load.vramUsedGb);
        pushAdvice(result, "Освободить видеопамять",
            "Занято " + formatValue(load.vramUsedGb, 1) + " из " + formatValue(load.vramTotalGb, 1)
                + " ГБ (свободно " + formatValue(free, 1) + " ГБ): текстуры уже уходят в файл подкачки. "
                      "Снизьте качество текстур, иначе появятся периодические провалы.");
    }
    if (load.gpuPower > 0.0 && !hardware.gpus.empty()) {
        pushAdvice(result, "Проверить лимит питания и температуры",
            "Видеокарта держит " + formatValue(load.gpuPower, 0)
                + " Вт. Если частота ядра ниже паспортной, а температура выше 83 °C — карта троттлит, "
                  "и поднятие power limit вернёт кадры дешевле, чем апгрейд.");
    }
}

} // namespace

double cpuPerfIndex(std::string_view name, std::uint32_t cores, std::uint32_t threads)
{
    const std::string text = upper(name);
    if (text.empty()) {
        return 0.0;
    }

    const long model = firstModelNumber(text);
    double single = 0.0;

    if (text.find("RYZEN") != std::string::npos || text.find("THREADRIPPER") != std::string::npos) {
        single = amdGenerationIndex(model);
        single *= tierFactor(firstDigit(text, "RYZEN"));
        // Кэш 3D V-Cache — главный источник игрового преимущества AMD: он ускоряет
        // именно однопоточные сценарии, поэтому считается коэффициентом к индексу.
        if (text.find("X3D") != std::string::npos) {
            single *= 1.14;
        } else if (text.find("HX") != std::string::npos || text.find("3D") != std::string::npos) {
            single *= 1.06;
        }
    } else if (text.find("CORE ULTRA") != std::string::npos) {
        // Core Ultra 200S: IPC выше, чем у 14-го поколения, а в играх результат — на его уровне.
        single = 96.0 * tierFactor(firstDigit(text, "ULTRA"));
        const int tier = 0;
        single *= 1.0;
        (void)tier;
    } else if (text.find("CORE I") != std::string::npos || text.find("XEON") != std::string::npos) {
        single = intelGenerationIndex(model);
        const int tier = firstDigit(text, "CORE I");
        single *= tierFactor(tier);
        if (text.find("KF") != std::string::npos || text.find("K ") != std::string::npos || text.back() == 'K') {
            single *= 1.04; // разгон и более высокие буст-частоты
        }
    } else if (text.find("PENTIUM") != std::string::npos || text.find("CELERON") != std::string::npos) {
        single = 42.0;
    } else {
        // Неизвестное имя: честнее взять среднее по ядрам, чем выдумывать индекс.
        single = 60.0 + std::min(threads, 32u) * 0.8;
    }

    // Многопоточный потолок: 16 потоков считаются «полными», дальше прирост мал.
    const double threadFactor = threads == 0 ? 1.0 : std::clamp(std::sqrt(threads / 16.0), 0.45, 1.12);
    const double coresPenalty = cores > 0 && cores < 6 ? 0.9 : 1.0;
    // Лишние ядра в играх почти не помогают, но сравнение «16 ядер против 12» того же
    // поколения обязано оставаться строгим — иначе карточка выглядела бы сломанной.
    const double extraCores = cores > 8 ? std::min((cores - 8) * 0.012, 0.10) : 0.0;
    return std::max(0.0, single * threadFactor * coresPenalty * (1.0 + extraCores));
}

double gpuPerfIndex(std::string_view name, double dedicatedMb)
{
    const std::string text = upper(name);
    if (text.empty()) {
        return 0.0;
    }

    for (const GpuFamily& family : kGpuFamilies) {
        if (text.find(family.needle) == std::string::npos) {
            continue;
        }
        const long model = firstModelNumber(text);
        double index = family.base * (model > 0 ? gpuTierFactor(model) : 0.7);
        if (text.find("SUPER") != std::string::npos) {
            index *= 1.12;
        }
        if (text.find("TI") != std::string::npos) {
            index *= 1.16;
        }
        if (text.find("XTX") != std::string::npos) {
            index *= 1.2;
        } else if (text.find(" XT") != std::string::npos || text.find("XT") != std::string::npos) {
            index *= 1.08;
        }
        if (text.find("GRE") != std::string::npos) {
            index *= 0.94;
        }
        if (text.find("M ") != std::string::npos || text.find("MOBILE") != std::string::npos
            || text.find("LAPTOP") != std::string::npos) {
            index *= 0.72; // мобильные чипы того же имени медленнее на два шага
        }
        return std::max(0.0, index);
    }

    // «Microsoft Basic Render» и неизвестные карты: индекс по объёму памяти,
    // чтобы прогноз не выглядел выдуманным.
    if (text.find("BASIC") != std::string::npos) {
        return 0.0;
    }
    if (dedicatedMb > 0.0) {
        return std::clamp(dedicatedMb / 96.0, 6.0, 60.0);
    }
    return 0.0;
}

SystemLoad loadFromStore(const MetricStore& store, double targetFps)
{
    auto read = [&store](MetricId id, double fallback) {
        return store.hasValue(id) ? store.value(id) : fallback;
    };

    SystemLoad load;
    load.cpuLoad = read(MetricId::CpuLoad, -1.0);
    load.cpuPeakCore = read(MetricId::CpuPeakLoad, -1.0);
    load.gpuLoad = read(MetricId::GpuLoad, -1.0);
    load.fps = read(MetricId::Fps, -1.0);
    load.vramUsedGb = read(MetricId::VramUsed, -1.0);
    load.vramTotalGb = read(MetricId::VramTotal, -1.0);
    load.ramLoad = read(MetricId::RamLoad, -1.0);
    load.pageFileLoad = read(MetricId::PageFileLoad, -1.0);
    load.cpuPower = read(MetricId::CpuPower, -1.0);
    load.gpuPower = read(MetricId::GpuPower, -1.0);
    load.targetFps = targetFps;
    return load;
}

BottleneckResult analyzeBottleneck(const HardwareIdentity& hardware, const SystemLoad& load)
{
    BottleneckResult result;

    const double cpuIndex = cpuPerfIndex(hardware.cpu.name, hardware.cpu.physicalCores, hardware.cpu.logicalProcessors);
    const GpuIdentity* gpu = hardware.primaryGpu();
    const double gpuIndex = gpu != nullptr ? gpuPerfIndex(gpu->name, gpu->dedicatedMb) : 0.0;

    const DisplayIdentity* display = hardware.primaryDisplay();
    double pixels = 1920.0 * 1080.0;
    if (display != nullptr && display->width > 0 && display->height > 0) {
        pixels = static_cast<double>(display->width) * static_cast<double>(display->height);
    }
    // Пересчёт в кадры: 100 индекса ≈ 320 FPS у CPU (разрешение почти не влияет)
    // и ≈ 260 FPS у GPU при 1080p. Зависимость от разрешения — степенью 0,85:
    // часть работы (подача кадров, постобработка) от пикселей не зависит.
    result.cpuCapacityFps = cpuIndex * 3.2;
    result.gpuCapacityFps = gpuIndex * 2.6 * std::pow(1920.0 * 1080.0 / std::max(pixels, 1.0), 0.85);

    const bool haveLiveLoad = (load.cpuLoad >= 0.0 || load.cpuPeakCore >= 0.0) && load.gpuLoad >= 0.0;
    const double cpuPressure = std::max(load.cpuLoad, load.cpuPeakCore * 0.9);
    const double gpuPressure = load.gpuLoad;

    if (haveLiveLoad && std::max(cpuPressure, gpuPressure) < 22.0 && !(load.fps > 0.0)) {
        result.kind = BottleneckKind::Idle;
        result.headline = "Нагрузки нет";
        result.detail = "Пока ни процессор, ни видеокарта не загружены, судить об узком месте не по чему. "
                        "Запустите игру — расчёт пересчитается по живым данным.";
        return result;
    }

    if (haveLiveLoad && std::max(cpuPressure, gpuPressure) >= 22.0) {
        result.measured = true;
        result.expectedFps = load.fps > 0.0 ? load.fps : 0.0;

        if (load.vramTotalGb > 0.0 && load.vramUsedGb >= load.vramTotalGb - 0.2) {
            result.kind = BottleneckKind::VramLimited;
            result.percent = 100;
            result.headline = "Не хватает видеопамяти";
            result.detail = "Видеопамять заполнена: " + formatValue(load.vramUsedGb, 1) + " из "
                + formatValue(load.vramTotalGb, 1)
                + " ГБ. Кадры упираются не в чип, а в обмен с ОЗУ — отсюда периодические провалы.";
            gpuAdvice(hardware, load, result);
            return result;
        }

        if (load.fps > 0.0 && load.targetFps > 0.0 && load.fps >= load.targetFps * 0.96
            && std::max(cpuPressure, gpuPressure) < 85.0) {
            result.kind = BottleneckKind::RefreshLimited;
            result.percent = 0;
            result.headline = "Кадры ограничены герцовкой или вертикальной синхронизацией";
            result.detail = "FPS держится на цели " + std::to_string(static_cast<int>(load.targetFps))
                + " Гц при незагруженной видеокарте: узкого места в железе нет, лимит стоит сверху.";
            pushAdvice(result, "Снять ограничение и включить G-SYNC/FreeSync",
                "Переменная герцовка уберёт разрывы без жёсткого лимита, а целевой кадр имеет смысл "
                "поставить на 3 ниже максимума монитора — так оверлей не добавляет задержку.");
            pushAdvice(result, "Поднять качество вместо борьбы за FPS",
                "Пока кадр упирается в лимит, запас видеокарты можно потратить на настройки: "
                "качество картинки вырастет без просадок.");
            return result;
        }

        const double high = std::max(cpuPressure, gpuPressure);
        const double low = std::min(cpuPressure, gpuPressure);
        result.percent = static_cast<std::uint32_t>(std::lround(clampPercent(100.0 * (1.0 - low / std::max(high, 1.0)))));

        if (high - low < 12.0) {
            result.kind = BottleneckKind::LiveBalanced;
            result.headline = "Связка работает ровно";
            result.detail = "Процессор загружен на " + percentText(cpuPressure) + ", видеокарта — на "
                + percentText(gpuPressure) + ": оба узла близко к пределу, простой отсутствует.";
            pushAdvice(result, "Апгрейд одного узла кадров не добавит",
                "Пока оба компонента загружены ровно, замена видеокарты упрётся в процессор, и наоборот. "
                "Смысл есть только в паре или в переходе на более высокое разрешение.");
            if (load.ramLoad > 90.0) {
                pushAdvice(result, "Проверить запас ОЗУ",
                    "Память занята на " + percentText(load.ramLoad) + ", файл подкачки — на "
                        + percentText(load.pageFileLoad) + ". Закройте браузер и фоновые приложения: "
                          "своп во время игры даёт ровно те провалы, которые сложно объяснить железом.");
            }
            return result;
        }

        if (cpuPressure > gpuPressure) {
            result.kind = BottleneckKind::LiveCpu;
            result.headline = "Упор в процессор";
            result.detail = "Процессор держит " + percentText(cpuPressure) + " (самое занятое ядро — "
                + percentText(load.cpuPeakCore) + "), видеокарта — только " + percentText(gpuPressure) + ". "
                + percentText(static_cast<double>(result.percent)) + " мощности GPU простаивает.";
            cpuAdvice(hardware, result);
        } else {
            result.kind = BottleneckKind::LiveGpu;
            result.headline = "Упор в видеокарту";
            result.detail = "Видеокарта загружена на " + percentText(gpuPressure) + ", процессор — на "
                + percentText(cpuPressure) + ". " + percentText(static_cast<double>(result.percent))
                + " мощности CPU не используется: это нормальный режим для тяжёлой графики.";
            gpuAdvice(hardware, load, result);
        }
        return result;
    }

    if (cpuIndex <= 0.0 || gpuIndex <= 0.0) {
        result.kind = BottleneckKind::NoData;
        result.headline = "Недостаточно данных";
        result.detail = "Приложение не смогло определить модель процессора или видеокарты, "
            "а живых замеров нет. Запустите игру: оценка по загрузке не требует таблиц производительности.";
        return result;
    }

    // Прогноз: считаем, какой узел первым упирается в потолок при текущем разрешении.
    const double high = std::max(result.cpuCapacityFps, result.gpuCapacityFps);
    const double low = std::min(result.cpuCapacityFps, result.gpuCapacityFps);
    result.expectedFps = low;
    result.percent = static_cast<std::uint32_t>(std::lround(clampPercent(100.0 * (1.0 - low / high) * 1.15)));

    const std::string resolution = display != nullptr && display->width > 0
        ? std::to_string(display->width) + "×" + std::to_string(display->height)
        : "1920×1080";

    if (result.percent < 12) {
        result.kind = BottleneckKind::EstimateBalanced;
        result.headline = "Связка сбалансирована";
        result.detail = "По индексу производительности при разрешении " + resolution
            + " оба узла приходят к потолку вместе. Оценка ±15 %: живые замеры точнее.";
        pushAdvice(result, "Держать цель кадра по герцовке монитора",
            "Лишние кадры при незагруженной видеокарте только добавляют задержку и нагрев.");
        return result;
    }

    if (result.cpuCapacityFps < result.gpuCapacityFps) {
        result.kind = BottleneckKind::EstimateCpu;
        result.headline = "Упор в процессор";
        result.detail = "По индексу производительности в разрешении " + resolution
            + " связка даёт ~" + formatValue(result.cpuCapacityFps, 0) + " кадров: дальше "
              "видеокарта не разгонится, "
            + std::to_string(result.percent) + "% её мощности будет простаивать.";
        cpuAdvice(hardware, result);
    } else {
        result.kind = BottleneckKind::EstimateGpu;
        result.headline = "Упор в видеокарту";
        result.detail = "По индексу производительности в разрешении " + resolution + " потолок — видеокарта: ~"
            + formatValue(result.gpuCapacityFps, 0) + " кадров, процессор загружен не будет.";
        gpuAdvice(hardware, load, result);
    }

    if (display != nullptr && display->refreshHz > 0 && result.expectedFps > display->refreshHz * 1.5)
    {
        pushAdvice(result, "Экран не использует запас кадров",
            "Монитор " + std::to_string(display->refreshHz) + " Гц: прогноз выше герцовки вдвое. "
                "Либо поднимите настройки до упора в видеокарту, либо ограничьте FPS — лишние кадры не видны.");
    }
    return result;
}

} // namespace yawa

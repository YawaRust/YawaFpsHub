#include "ui/system_view.h"
#include "metrics/units.h"
#include "ui/layout.h"

#include <algorithm>
#include <string>

namespace yawa {
namespace {

struct Columns {
    Rect area{};

    float width() const { return (area.width() - kGap * 11.0f) / 12.0f; }

    Rect span(float first, float count, float top, float height) const
    {
        const float column = width();
        const float left = area.left + first * (column + kGap);
        const float right = area.left + (first + count) * column + (first + count - 1.0f) * kGap;
        return Rect{ left, top, right, top + height };
    }
};

std::wstring field(std::string_view text)
{
    return text.empty() ? std::wstring(L"нет данных") : wideText(text);
}

// Строка «подпись — значение»: как в карточках обзора, но значение может быть длинным,
// поэтому подпись занимает ровно треть ширины.
void drawRow(Renderer& renderer, const Rect& row, std::wstring_view label, std::wstring_view value,
    const Theme& theme, TextRole valueRole = TextRole::Body)
{
    renderer.text(label, Rect{ row.left, row.top, row.left + row.width() * 0.34f, row.bottom }, TextRole::Small,
        theme.textDim);
    renderer.text(value, Rect{ row.left + row.width() * 0.34f, row.top, row.right, row.bottom }, valueRole,
        theme.text, TextAlign::Right);
}

void drawOsCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    const OsIdentity& os = model.hardware.os;
    drawCardHeader(renderer, card, L"Windows", L"как в winver: выпуск, версия функционала и сборка", theme);

    std::wstring build = field(os.build);
    if (!os.featureVersion.empty()) {
        build = wideText(os.featureVersion) + L" · " + build;
    }

    float top = inner.top + 42.0f;
    drawRow(renderer, Rect{ inner.left, top, inner.right, top + 18.0f }, L"Система", field(os.product), theme,
        TextRole::Value);
    top += 34.0f;
    drawRow(renderer, Rect{ inner.left, top, inner.right, top + 18.0f }, L"Сборка", build, theme);
    top += 26.0f;
    drawRow(renderer, Rect{ inner.left, top, inner.right, top + 18.0f }, L"Издание", field(os.edition), theme);
    top += 26.0f;
    drawRow(renderer, Rect{ inner.left, top, inner.right, top + 18.0f }, L"Архитектура",
        os.architecture.empty() ? std::wstring(L"нет данных") : wideText(os.architecture), theme);
}

void drawCpuCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    const CpuIdentity& cpu = model.hardware.cpu;
    drawCardHeader(renderer, card, L"Процессор", L"CPUID и GetLogicalProcessorInformationEx", theme);

    float top = inner.top + 42.0f;
    renderer.text(field(cpu.name), Rect{ inner.left, top, inner.right, top + 36.0f }, TextRole::Body, theme.text);
    top += 42.0f;

    std::wstring cores = L"—";
    if (cpu.physicalCores > 0) {
        cores = wideText(std::to_string(cpu.physicalCores) + " ядер / " + std::to_string(cpu.logicalProcessors)
            + " потоков");
    } else if (cpu.logicalProcessors > 0) {
        cores = wideText(std::to_string(cpu.logicalProcessors) + " логических");
    }
    drawRow(renderer, Rect{ inner.left, top, inner.right, top + 18.0f }, L"Ядра", cores, theme);
    top += 26.0f;
    drawRow(renderer, Rect{ inner.left, top, inner.right, top + 18.0f }, L"Базовая частота",
        cpu.baseMhz > 0.0 ? wideText(formatWithUnit(cpu.baseMhz, MetricUnit::Megahertz, 0)) : std::wstring(L"—"),
        theme);
    top += 26.0f;
    drawRow(renderer, Rect{ inner.left, top, inner.right, top + 18.0f }, L"Кэш L3",
        cpu.l3CacheMb > 0 ? wideText(std::to_string(cpu.l3CacheMb) + " МБ") : std::wstring(L"—"), theme);
    top += 26.0f;
    drawRow(renderer, Rect{ inner.left, top, inner.right, top + 18.0f }, L"Вендор", field(cpu.vendor), theme);
    top += 26.0f;
    drawRow(renderer, Rect{ inner.left, top, inner.right, top + 18.0f }, L"Индекс",
        cpuPerfIndex(cpu.name, cpu.physicalCores, cpu.logicalProcessors) > 0.0
            ? wideText(formatValue(cpuPerfIndex(cpu.name, cpu.physicalCores, cpu.logicalProcessors), 0))
            : std::wstring(L"—"),
        theme);
}

void drawGpuCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    drawCardHeader(renderer, card, L"Видеокарты", L"DXGI и реестр класса Display: список адаптеров и драйвер", theme);

    if (model.hardware.gpus.empty()) {
        renderer.text(L"нет данных", Rect{ inner.left, inner.top + 44.0f, inner.right, inner.top + 62.0f },
            TextRole::Body, theme.textFaint);
        return;
    }

    float top = inner.top + 40.0f;
    for (const GpuIdentity& gpu : model.hardware.gpus) {
        if (top + 44.0f > inner.bottom) {
            break;
        }
        const bool active = &gpu == model.hardware.primaryGpu();
        std::wstring title = wideText(gpu.name);
        if (active) {
            title += L" · основная";
        }
        renderer.text(title, Rect{ inner.left, top, inner.right, top + 18.0f }, TextRole::Body,
            active ? theme.accent : theme.text);
        std::wstring note = wideText(gpu.vendor);
        if (gpu.dedicatedMb > 0.0) {
            note += L" · " + wideText(formatWithUnit(gpu.dedicatedMb / 1024.0, MetricUnit::Gigabytes, 1)) + L" выделено";
        }
        if (!gpu.driverVersion.empty()) {
            note += L" · драйвер " + wideText(gpu.driverVersion);
        }
        renderer.text(note, Rect{ inner.left, top + 19.0f, inner.right, top + 33.0f }, TextRole::Tiny,
            theme.textFaint);
        const double index = gpuPerfIndex(gpu.name, gpu.dedicatedMb);
        if (index > 0.0) {
            drawBar(renderer, Rect{ inner.left, top + 36.0f, inner.right, top + 39.0f },
                std::clamp(static_cast<float>(index / 150.0), 0.0f, 1.0f), theme.gpu, theme);
        }
        top += 48.0f;
    }
}

void drawMemoryCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    const MemoryIdentity& memory = model.hardware.memory;
    drawCardHeader(renderer, card, L"Память",
        L"SMBIOS Type 17: раскладка по слотам, частоты и напряжение модулей", theme);

    float top = inner.top + 42.0f;
    drawRow(renderer, Rect{ inner.left, top, inner.right, top + 18.0f }, L"Конфигурация",
        field(describeMemory(memory)), theme, TextRole::Value);
    top += 32.0f;

    std::wstring speed = L"—";
    if (memory.configuredMt > 0.0) {
        speed = wideText(formatWithUnit(memory.configuredMt, MetricUnit::Megahertz, 0) + " МТ/с");
        if (memory.ratedMt > 0.0 && memory.ratedMt > memory.configuredMt + 1.0) {
            speed += wideText(" из " + formatWithUnit(memory.ratedMt, MetricUnit::Megahertz, 0));
        }
    }
    drawRow(renderer, Rect{ inner.left, top, inner.right, top + 18.0f }, L"Частота", speed, theme);
    top += 26.0f;

    const double tck = memory.cycleTimeNs();
    drawRow(renderer, Rect{ inner.left, top, inner.right, top + 18.0f }, L"Такт tCK",
        tck > 0.0 ? wideText(formatValue(tck, 3) + " нс") : std::wstring(L"—"), theme);
    top += 26.0f;

    std::wstring profile = memory.profileEnabled ? L"включён" : L"не включён";
    if (memory.ratedMt <= 0.0) {
        profile = L"нет данных";
    }
    drawRow(renderer, Rect{ inner.left, top, inner.right, top + 18.0f }, L"XMP/EXPO", profile, theme);
    top += 26.0f;

    std::wstring slots = L"—";
    if (memory.slots > 0) {
        std::uint32_t populated = 0;
        for (const MemoryModule& module : memory.modules) {
            if (module.populated) {
                ++populated;
            }
        }
        slots = wideText(std::to_string(populated) + " из " + std::to_string(memory.slots));
    }
    drawRow(renderer, Rect{ inner.left, top, inner.right, top + 18.0f }, L"Слоты", slots, theme);
    top += 30.0f;

    // Таблица планок: слот, объём, модуль, напряжение. Именно это и спрашивают
    // «2×16 или 1×32».
    for (const MemoryModule& module : memory.modules) {
        if (!module.populated) {
            continue;
        }
        if (top + 16.0f > inner.bottom) {
            break;
        }
        std::wstring line = module.slot.empty() ? L"слот" : wideText(module.slot);
        line += L" · " + wideText(formatValue(module.sizeGb, 0) + " ГБ");
        if (!module.type.empty()) {
            line += wideText(" " + module.type);
        }
        if (module.configuredMt > 0.0) {
            line += wideText("-" + std::to_string(static_cast<int>(module.configuredMt)));
        }
        if (!module.vendor.empty()) {
            line += wideText(" · " + module.vendor);
        }
        if (!module.part.empty()) {
            line += wideText(" · " + module.part);
        }
        if (module.voltage > 0.0) {
            line += wideText(" · " + formatValue(module.voltage, 2) + " В");
        }
        renderer.text(clipText(renderer, line, TextRole::Small, inner.width()),
            Rect{ inner.left, top, inner.right, top + 16.0f }, TextRole::Small, theme.textDim);
        top += 18.0f;
    }

    if (top + 30.0f <= inner.bottom) {
        drawWrappedText(renderer,
            L"Точные тайминги (tCL-tRCD-tRP-tRAS) лежат в SPD на шине SMBus: их нельзя прочитать "
            L"без драйвера, а драйверы приложение не ставит и не грузит.",
            Rect{ inner.left, inner.bottom - 28.0f, inner.right, inner.bottom }, TextRole::Tiny, theme.textFaint,
            12.0f, 2);
    }
}

void drawDisplayCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    drawCardHeader(renderer, card, L"Мониторы", L"EnumDisplaySettings и EDID-имя драйвера монитора", theme);

    if (model.hardware.displays.empty()) {
        renderer.text(model.displayWidth > 0
                ? wideText(std::to_string(model.displayWidth) + "×" + std::to_string(model.displayHeight))
                : std::wstring(L"нет данных"),
            Rect{ inner.left, inner.top + 44.0f, inner.right, inner.top + 62.0f }, TextRole::Body, theme.textDim);
        return;
    }

    float top = inner.top + 42.0f;
    for (const DisplayIdentity& display : model.hardware.displays) {
        if (top + 34.0f > inner.bottom) {
            break;
        }
        std::wstring title = display.name.empty() ? L"Монитор" : wideText(display.name);
        if (display.primary) {
            title += L" · основной";
        }
        renderer.text(title, Rect{ inner.left, top, inner.right, top + 18.0f }, TextRole::Body, theme.text);
        std::wstring note = wideText(std::to_string(display.width) + "×" + std::to_string(display.height));
        if (display.refreshHz > 0) {
            note += wideText(", " + std::to_string(display.refreshHz) + " Гц");
        }
        if (display.scale > 1.001) {
            note += wideText(", масштаб " + formatValue(display.scale * 100.0, 0) + " %");
        }
        renderer.text(note, Rect{ inner.left, top + 18.0f, inner.right, top + 32.0f }, TextRole::Tiny,
            theme.textFaint);
        top += 40.0f;
    }
}

} // namespace

void drawSystemView(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits)
{
    (void)hits; // экран только показывает собранное: действий на нём нет

    const Columns columns{ area };
    const float top = area.top;
    drawOsCard(renderer, model, theme, columns.span(0, 4, top, 200.0f));
    drawCpuCard(renderer, model, theme, columns.span(4, 4, top, 200.0f));
    drawGpuCard(renderer, model, theme, columns.span(8, 4, top, 200.0f));

    drawMemoryCard(renderer, model, theme, columns.span(0, 8, top + 214.0f, 250.0f));
    drawDisplayCard(renderer, model, theme, columns.span(8, 4, top + 214.0f, 250.0f));

    const Rect note = columns.span(0, 12, top + 478.0f, kSystemHeight - 486.0f);
    drawCard(renderer, note, theme);
    const Rect inner = note.inset(kCardPadding);
    drawCardHeader(renderer, note, L"Откуда эти числа",
        L"реестр, CPUID, SMBIOS, DXGI и EnumDisplaySettings — без драйверов и без сети", theme);
    drawWrappedText(renderer,
        L"Версию Windows берём из CurrentVersion — оттуда же её берёт winver, поэтому карточка и окно «О системе» "
        L"совпадают. Модель процессора читается прямо из CPUID, ядра и кэш — из GetLogicalProcessorInformationEx. "
        L"Видеокарты перечисляет DXGI, а версию драйвера добавляет реестр класса Display. Модули памяти и их частоты "
        L"приложение берёт из таблиц SMBIOS, которые BIOS собирает из SPD: именно так видно «2×16 ГБ», а не только "
        L"«32 ГБ» суммарно.",
        Rect{ inner.left, inner.top + 40.0f, inner.right, inner.bottom }, TextRole::Small, theme.textDim, 15.0f, 4);
}

} // namespace yawa

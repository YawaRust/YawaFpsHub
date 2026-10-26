#include "ui/bottleneck_view.h"
#include "metrics/bottleneck.h"
#include "metrics/units.h"
#include "ui/layout.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace yawa {
namespace {

// Сетка 12 колонок — та же, что на обзоре.
struct Columns {
    Rect area{};

    float width() const { return (area.width() - kGap * 11.0f) / 12.0f; }

    // Колонки целые, координаты — вещественные: иначе на каждом вызове MSVC ругается
    // на неявное int→float в аргументах.
    Rect span(int first, int count, float top, float height) const
    {
        const float column = width();
        const float left = area.left + static_cast<float>(first) * (column + kGap);
        const float right = area.left + static_cast<float>(first + count) * column
            + static_cast<float>(first + count - 1) * kGap;
        return Rect{ left, top, right, top + height };
    }
};

std::wstring field(std::string_view text)
{
    return text.empty() ? std::wstring(L"нет данных") : wideText(text);
}

std::string joinParts(const std::vector<std::string>& parts)
{
    std::string text;
    for (const std::string& part : parts) {
        if (!text.empty()) {
            text += " · ";
        }
        text += part;
    }
    return text;
}

// Цвет verdict-блока по силе упора: до 12 % — норма, до 35 % — предупреждение, дальше красным.
Color severityColor(std::uint32_t percent, const Theme& theme)
{
    if (percent >= 35) {
        return theme.danger;
    }
    if (percent >= 12) {
        return theme.warn;
    }
    return theme.accent;
}

// Размеры карточек сняты с эталонного снимка демо (demo-bottleneck-tall.png):
// 204 + 14 + 363 + 14 + «Что делать» + 14 + 144 — так экран совпадает с прототипом.
constexpr float kVerdictHeight = 204.0f;
constexpr float kPairHeight = 363.0f;
constexpr float kNoteHeight = 144.0f;
constexpr float kAdviceMinHeight = 249.0f;

float adviceHeight(const std::size_t count)
{
    return std::max(kAdviceMinHeight, 42.0f + 57.0f * static_cast<float>(count) + 12.0f);
}

void drawVerdictCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    const BottleneckResult& balance = model.balance;

    std::vector<std::string> parts;
    if (!model.hardware.cpu.name.empty()) {
        parts.push_back(model.hardware.cpu.name);
    }
    if (const GpuIdentity* gpu = model.hardware.primaryGpu(); gpu != nullptr) {
        parts.push_back(gpu->name);
    }
    if (const DisplayIdentity* display = model.hardware.primaryDisplay(); display != nullptr) {
        parts.push_back(describeDisplay(*display));
    }
    const std::wstring tag = balance.measured ? L"ЗАМЕРЫ ИГРЫ" : L"НЕТ ЗАМЕРОВ";
    const std::wstring subtitle = model.gameName.empty()
        ? L"игра не обнаружена"
        : model.gameName + L" · " + wideText(joinParts(parts));
    drawCardHeader(renderer, card, L"Узкое место игры", { }, theme);
    const float subtitleWidth
        = std::max(0.0f, inner.width() - renderer.measureText(tag, TextRole::Tiny) - 42.0f);
    renderer.text(clipText(renderer, subtitle, TextRole::Tiny, subtitleWidth),
        Rect { inner.left, inner.top + 18.0f, inner.left + subtitleWidth, inner.top + 34.0f }, TextRole::Tiny,
        theme.textFaint);
    drawTag(renderer, card, tag, balance.measured, theme);

    const Color color = severityColor(balance.percent, theme);
    const float radius = 46.0f;
    const float centerX = inner.left + radius + 6.0f;
    const float centerY = inner.top + 78.0f;
    const bool quiet = balance.kind == BottleneckKind::Idle || balance.kind == BottleneckKind::NoData;
    drawGauge(renderer, centerX, centerY, radius, static_cast<float>(balance.percent) / 100.0f, color, theme.line,
        quiet ? std::wstring(L"—") : wideText(std::to_string(balance.percent) + "%"), theme.text);
    renderer.text(L"помехи", Rect{ centerX - radius, centerY + radius - 10.0f, centerX + radius,
                                centerY + radius + 4.0f },
        TextRole::Tiny, theme.textFaint, TextAlign::Center);

    const float textLeft = centerX + radius + 22.0f;
    renderer.text(clipText(renderer, field(balance.headline), TextRole::Value, inner.right - textLeft),
        Rect{ textLeft, inner.top + 44.0f, inner.right, inner.top + 70.0f }, TextRole::Value, color);
    drawWrappedText(renderer, wideText(balance.detail),
        Rect{ textLeft, inner.top + 76.0f, inner.right, inner.bottom }, TextRole::Body, theme.textDim, 18.0f, 3);

    const float numbersTop = inner.bottom - 22.0f;
    if (balance.expectedFps > 0.0) {
        renderer.text(L"FPS игры: " + wideText(formatWithUnit(balance.expectedFps, MetricUnit::Count, 0)),
            Rect { inner.left + 300.0f, numbersTop, inner.right, numbersTop + 16.0f }, TextRole::Small,
            theme.textFaint);
    }
}

void drawPairCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    drawCardHeader(renderer, card, L"Связка", L"индексы нормированы: флагман = 100", theme);

    struct Row {
        std::wstring label;
        std::wstring value;
        std::wstring note;
        float fraction = 0.0f;
        Color color;
    };

    std::vector<Row> rows;
    {
        const double index = cpuPerfIndex(model.hardware.cpu.name, model.hardware.cpu.physicalCores,
            model.hardware.cpu.logicalProcessors);
        rows.push_back(Row{ L"Процессор",
            index > 0.0 ? wideText(formatValue(index, 0)) : std::wstring(L"—"),
            field(model.hardware.cpu.name), static_cast<float>(std::clamp(index / 120.0, 0.0, 1.0)), theme.cpu });
    }
    if (const GpuIdentity* gpu = model.hardware.primaryGpu(); gpu != nullptr) {
        const double index = gpuPerfIndex(gpu->name, gpu->dedicatedMb);
        rows.push_back(Row{ L"Видеокарта", index > 0.0 ? wideText(formatValue(index, 0)) : std::wstring(L"—"),
            field(gpu->name), static_cast<float>(std::clamp(index / 150.0, 0.0, 1.0)), theme.gpu });
    }
    {
        const MemoryIdentity& memory = model.hardware.memory;
        rows.push_back(Row{ L"Память", wideText(describeMemory(memory)), L"XMP/EXPO из SMBIOS не определяется",
            static_cast<float>(std::clamp(memory.totalGb / 64.0, 0.0, 1.0)), theme.ram });
    }
    if (const DisplayIdentity* display = model.hardware.primaryDisplay(); display != nullptr) {
        rows.push_back(Row{ L"Экран", std::to_wstring(display->width) + L"×" + std::to_wstring(display->height),
            display->refreshHz > 0 ? wideText(std::to_string(display->refreshHz) + " Гц") : L"нет герцовки", 1.0f,
            theme.disk });
    }

    // Шаг 68 px — как в демо: подпись, полоса и пояснение каждой строки.
    float top = inner.top + 44.0f;
    for (const Row& row : rows) {
        if (top + 40.0f > inner.bottom) {
            break;
        }
        renderer.text(row.label, Rect{ inner.left, top, inner.left + 110.0f, top + 16.0f }, TextRole::Small,
            theme.textDim);
        const float labelWidth = renderer.measureText(row.label, TextRole::Small) + 12.0f;
        renderer.text(clipText(renderer, row.value, TextRole::Body, inner.right - inner.left - labelWidth - 8.0f),
            Rect{ inner.left + labelWidth, top - 4.0f, inner.right, top + 16.0f }, TextRole::Body, theme.text,
            TextAlign::Right);
        drawBar(renderer, Rect{ inner.left, top + 20.0f, inner.right, top + 26.0f }, row.fraction, row.color, theme);
        if (!row.note.empty()) {
            renderer.text(clipText(renderer, row.note, TextRole::Tiny, inner.width()),
                Rect{ inner.left, top + 29.0f, inner.right, top + 41.0f }, TextRole::Tiny, theme.textFaint);
        }
        top += 68.0f;
    }
}

void drawLoadCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    drawCardHeader(renderer, card, L"Нагрузка игры и системы", L"последний интервал опроса", theme);

    struct Row {
        std::wstring label;
        double value;
        double total;
        bool known;
        Color color;
    };

    const auto reading
        = [&model](MetricId id) { return model.store.hasValue(id) ? model.store.value(id) : -1.0; };
    const std::array<Row, 6> rows { Row { L"CPU игры (все)", model.gameCpuLoad, 100.0, model.gameCpuLoad >= 0.0,
                                        theme.cpu },
        { L"Поток игры", model.gamePeakThreadLoad, 100.0, model.gamePeakThreadLoad >= 0.0, theme.cpu },
        { L"GPU игры (3D)", model.gameGpuLoad, 100.0, model.gameGpuLoad >= 0.0, theme.gpu },
        { L"CPU системы", reading(MetricId::CpuLoad), 100.0, model.store.hasValue(MetricId::CpuLoad),
            theme.cpu },
        { L"GPU системы", reading(MetricId::GpuLoad), 100.0, model.store.hasValue(MetricId::GpuLoad),
            theme.gpu },
        { L"VRAM системы", reading(MetricId::VramUsed), reading(MetricId::VramTotal),
            model.store.hasValue(MetricId::VramUsed), theme.ram } };

    float top = inner.top + 44.0f;
    for (const Row& row : rows) {
        if (top + 26.0f > inner.bottom) {
            break;
        }
        const float fraction
            = row.total > 0.0 ? std::clamp(static_cast<float>(row.value / row.total), 0.0f, 1.0f) : 0.0f;
        renderer.text(row.label, Rect { inner.left, top, inner.left + 118.0f, top + 16.0f }, TextRole::Small,
            theme.textDim);
        const MetricUnit unit = row.label == L"VRAM системы" ? MetricUnit::Gigabytes : MetricUnit::Percent;
        renderer.text(row.known ? wideText(formatWithUnit(row.value, unit, 1)) : L"нет данных",
            Rect { inner.left + 118.0f, top - 2.0f, inner.right, top + 16.0f }, TextRole::Body,
            row.known ? theme.text : theme.textFaint, TextAlign::Right);
        drawBar(renderer, Rect { inner.left, top + 19.0f, inner.right, top + 22.0f },
            row.known ? fraction : 0.0f, row.color, theme);
        top += 42.0f;
    }
}

void drawAdviceCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    const std::size_t count = model.balance.advice.size();
    const std::wstring subtitle = count == 0
        ? std::wstring(L"рекомендаций нет — нагрузка или данные отсутствуют")
        : wideText(count == 1 ? "1 рекомендация" : (count < 5 ? std::to_string(count) + " рекомендации"
                                                                : std::to_string(count) + " рекомендаций"));
    drawCardHeader(renderer, card, L"Что делать", subtitle, theme);

    float top = inner.top + 42.0f;
    for (const BottleneckAdvice& advice : model.balance.advice) {
        if (top + 34.0f > inner.bottom) {
            break;
        }
        renderer.fillCircle(inner.left + 3.0f, top + 5.0f, 3.0f, severityColor(model.balance.percent, theme));
        renderer.text(clipText(renderer, wideText(advice.title), TextRole::Body, inner.width() - 14.0f),
            Rect{ inner.left + 14.0f, top, inner.right, top + 16.0f }, TextRole::Body, theme.text);
        const float used = drawWrappedText(renderer, wideText(advice.text),
            Rect{ inner.left + 14.0f, top + 17.0f, inner.right, inner.bottom }, TextRole::Small, theme.textDim, 14.0f,
            2);
        top += 17.0f + used + 12.0f;
    }
}

} // namespace

void drawBottleneckView(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits)
{
    (void)hits; // экран только показывает расчёт: действий на нём нет

    const Columns columns{ area };
    // Узкое окно — как в прототипе: ряд 7+5 распадается на две карточки друг под другом.
    const bool narrow = area.width() < kDashboardNarrowWidth;

    float top = area.top;
    drawVerdictCard(renderer, model, theme, columns.span(0, 12, top, kVerdictHeight));
    top += kVerdictHeight + kGap;

    drawPairCard(renderer, model, theme, columns.span(0, narrow ? 12 : 7, top, kPairHeight));
    drawLoadCard(renderer, model, theme, columns.span(narrow ? 0 : 7, narrow ? 12 : 5, top, kPairHeight));
    top += kPairHeight + kGap;

    const float adviceHeightLocal = adviceHeight(model.balance.advice.size());
    drawAdviceCard(renderer, model, theme, columns.span(0, 12, top, adviceHeightLocal));
    top += adviceHeightLocal + kGap;

    // Методика — внизу: вывод важнее пояснения, но проверить его должно быть можно.
    const Rect note = columns.span(0, 12, top, kNoteHeight);
    drawCard(renderer, note, theme);
    const Rect inner = note.inset(kCardPadding);
    drawCardHeader(renderer, note, L"Как считаем",
        L"оба источника показаны отдельно: живые замеры точнее любого справочника", theme);
    drawWrappedText(renderer,
        L"Игра — активный процесс с кадрами ETW. CPU измеряется временем её процесса, GPU — счётчиком 3D "
        L"для её PID и выбранного адаптера. Нагрузки всей системы показаны отдельно и не входят в вердикт. "
        L"Без игры или счётчика вывод не подменяется прогнозом либо частотой монитора. "
        L"Сравнение загрузок не доказывает точный прирост FPS от замены железа.",
        Rect { inner.left, inner.top + 40.0f, inner.right, inner.bottom }, TextRole::Small, theme.textDim,
        15.0f, 3);
}

} // namespace yawa

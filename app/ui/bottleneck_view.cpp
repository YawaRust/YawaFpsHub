#include "ui/bottleneck_view.h"
#include "metrics/bottleneck.h"
#include "metrics/units.h"
#include "ui/layout.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace yawa {
namespace {

// Сетка 12 колонок — та же, что на обзоре: карточки перестраиваются с окном,
// а не сжимаются в кашу.
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

std::wstring orNoData(const std::string& text)
{
    return text.empty() ? L"нет данных" : wideText(text);
}

// Цвет зависит от того, насколько сильно узкое место: 0…12 % — норма, дальше
// предупреждение, а за 35 % — уже красный.
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

void drawVerdictCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    const BottleneckResult& balance = model.balance;

    std::wstring subtitle;
    if (!model.hardware.cpu.name.empty()) {
        subtitle += wideText(model.hardware.cpu.name);
    }
    if (const GpuIdentity* gpu = model.hardware.primaryGpu(); gpu != nullptr) {
        subtitle += subtitle.empty() ? L"" : L" · ";
        subtitle += wideText(gpu->name);
    }
    if (const DisplayIdentity* display = model.hardware.primaryDisplay(); display != nullptr) {
        subtitle += subtitle.empty() ? L"" : L" · ";
        subtitle += wideText(describeDisplay(*display));
    }
    if (subtitle.empty()) {
        subtitle = L"оборудование не определено";
    }

    drawCardHeader(renderer, card, L"Узкое место", subtitle, theme);
    drawTag(renderer, card, balance.measured ? L"ЖИВЫЕ ЗАМЕРЫ" : L"ОЦЕНКА ±15 %", balance.measured, theme);

    const Color color = severityColor(balance.percent, theme);
    const float radius = 46.0f;
    const float centerX = inner.left + radius + 6.0f;
    const float centerY = inner.top + 78.0f;
    const std::wstring percentText = balance.kind == BottleneckKind::Idle || balance.kind == BottleneckKind::NoData
        ? std::wstring(L"—")
        : wideText(std::to_string(balance.percent) + "%");
    drawGauge(renderer, centerX, centerY, radius, static_cast<float>(balance.percent) / 100.0f, color, theme.line,
        percentText, theme.text);
    renderer.text(L"ПОМЕХИ", Rect{ centerX - radius, centerY + radius - 10.0f, centerX + radius,
                                centerY + radius + 4.0f },
        TextRole::Tiny, theme.textFaint, TextAlign::Center);

    const float textLeft = centerX + radius + 22.0f;
    renderer.text(orNoData(balance.headline),
        Rect{ textLeft, inner.top + 44.0f, inner.right, inner.top + 70.0f }, TextRole::Value, color);
    drawWrappedText(renderer, wideText(balance.detail), Rect{ textLeft, inner.top + 76.0f, inner.right,
                                                                   inner.bottom },
        TextRole::Small, theme.textDim, 15.0f, 3);

    // Пропускная способность узлов: два числа рядом, чтобы было видно, из чего вывод.
    const float numbersTop = inner.bottom - 26.0f;
    const std::wstring cpuLine = L"CPU: ~" + wideText(formatValue(balance.cpuCapacityFps, 0)) + L" к/с";
    const std::wstring gpuLine = L"GPU: ~" + wideText(formatValue(balance.gpuCapacityFps, 0)) + L" к/с";
    renderer.text(cpuLine, Rect{ inner.left, numbersTop, inner.left + 150.0f, numbersTop + 16.0f }, TextRole::Small,
        theme.cpu.withAlpha(0.9f));
    renderer.text(gpuLine, Rect{ inner.left + 150.0f, numbersTop, inner.left + 300.0f, numbersTop + 16.0f },
        TextRole::Small, theme.gpu.withAlpha(0.9f));
    if (balance.expectedFps > 0.0) {
        const std::wstring expected = L"ожидание: " + wideText(formatWithUnit(balance.expectedFps, MetricUnit::Count, 0));
        renderer.text(expected,
            Rect{ inner.left + 300.0f, numbersTop, inner.right, numbersTop + 16.0f }, TextRole::Small, theme.textFaint);
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
        Row row;
        row.label = L"Процессор";
        const double index = cpuPerfIndex(model.hardware.cpu.name, model.hardware.cpu.physicalCores,
            model.hardware.cpu.logicalProcessors);
        row.value = index > 0.0 ? wideText(formatValue(index, 0)) : L"—";
        row.note = model.hardware.cpu.name.empty()
            ? L""
            : L"индекс " + wideText(formatValue(index, 0)) + L" из 100";
        row.fraction = static_cast<float>(std::clamp(index / 120.0, 0.0, 1.0));
        row.color = theme.cpu;
        rows.push_back(row);
    }
    if (const GpuIdentity* gpu = model.hardware.primaryGpu(); gpu != nullptr) {
        Row row;
        const double index = gpuPerfIndex(gpu->name, gpu->dedicatedMb);
        row.label = L"Видеокарта";
        row.value = index > 0.0 ? wideText(formatValue(index, 0)) : L"—";
        row.note = L"индекс " + wideText(formatValue(index, 0)) + L" из 100";
        row.fraction = static_cast<float>(std::clamp(index / 150.0, 0.0, 1.0));
        row.color = theme.gpu;
        rows.push_back(row);
    }
    {
        Row row;
        row.label = L"Память";
        row.value = wideText(describeMemory(model.hardware.memory));
        row.note = model.hardware.memory.profileEnabled ? L"профиль памяти активен"
                                                        : L"XMP/EXPO не включён";
        row.fraction = static_cast<float>(std::clamp(model.hardware.memory.totalGb / 64.0, 0.0, 1.0));
        row.color = theme.ram;
        rows.push_back(row);
    }
    if (const DisplayIdentity* display = model.hardware.primaryDisplay(); display != nullptr) {
        Row row;
        row.label = L"Экран";
        row.value = wideText(describeDisplay(*display));
        row.fraction = 1.0f;
        row.color = theme.disk;
        rows.push_back(row);
    }

    float top = inner.top + 42.0f;
    for (const Row& row : rows) {
        if (top + 44.0f > inner.bottom) {
            break;
        }
        renderer.text(row.label, Rect{ inner.left, top, inner.left + 110.0f, top + 16.0f }, TextRole::Small,
            theme.textDim);
        renderer.text(row.value, Rect{ inner.left + 110.0f, top - 4.0f, inner.right, top + 16.0f }, TextRole::Body,
            theme.text, TextAlign::Right);
        const Rect bar{ inner.left, top + 20.0f, inner.right, top + 26.0f };
        drawBar(renderer, bar, row.fraction, row.color, theme);
        if (!row.note.empty()) {
            renderer.text(row.note, Rect{ inner.left, top + 28.0f, inner.right, top + 40.0f }, TextRole::Tiny,
                theme.textFaint);
        }
        top += 46.0f;
    }
}

void drawLoadCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    drawCardHeader(renderer, card, L"Нагрузка сейчас", L"последний интервал опроса", theme);

    struct Row {
        std::wstring label;
        double value = 0.0;
        MetricId id = MetricId::CpuLoad;
        Color color;
    };

    const std::array<Row, 4> rows{ Row{ L"Процессор", model.store.value(MetricId::CpuLoad), MetricId::CpuLoad,
                                   theme.cpu },
        { L"Пик по ядрам", model.store.value(MetricId::CpuPeakLoad), MetricId::CpuPeakLoad, theme.cpu },
        { L"Видеокарта", model.store.value(MetricId::GpuLoad), MetricId::GpuLoad, theme.gpu },
        { L"Видеопамять", model.store.value(MetricId::VramUsed), MetricId::VramUsed, theme.gpu } };

    float top = inner.top + 42.0f;
    for (const Row& row : rows) {
        if (top + 26.0f > inner.bottom) {
            break;
        }
        const bool known = model.store.hasValue(row.id);
        const double total = row.id == MetricId::VramUsed ? model.store.value(MetricId::VramTotal) : 100.0;
        const float fraction = total > 0.0 ? std::clamp(static_cast<float>(row.value / total), 0.0f, 1.0f) : 0.0f;

        renderer.text(row.label, Rect{ inner.left, top, inner.left + 100.0f, top + 16.0f }, TextRole::Small,
            theme.textDim);
        const std::wstring value
            = known ? wideMetric(row.id, row.value) : std::wstring(L"нет данных");
        renderer.text(value, Rect{ inner.left + 100.0f, top - 2.0f, inner.right, top + 16.0f }, TextRole::Body,
            known ? theme.text : theme.textFaint, TextAlign::Right);
        drawBar(renderer, Rect{ inner.left, top + 18.0f, inner.right, top + 22.0f }, known ? fraction : 0.0f,
            row.color, theme);
        top += 32.0f;
    }
}

void drawAdviceCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    const std::size_t count = model.balance.advice.size();
    drawCardHeader(renderer, card, L"Что делать",
        count == 0 ? L"рекомендаций нет — нагрузка или данные отсутствуют"
                   : wideText(std::to_string(count) + (count == 1 ? " рекомендация" : (count < 5 ? " рекомендации" : " рекомендаций"))),
        theme);

    float top = inner.top + 42.0f;
    for (const BottleneckAdvice& advice : model.balance.advice) {
        if (top + 40.0f > inner.bottom) {
            break;
        }
        renderer.fillCircle(inner.left + 3.0f, top + 5.0f, 3.0f, severityColor(model.balance.percent, theme));
        renderer.text(wideText(advice.title), Rect{ inner.left + 14.0f, top, inner.right, top + 16.0f },
            TextRole::Body, theme.text);
        const float used = drawWrappedText(renderer, wideText(advice.text),
            Rect{ inner.left + 14.0f, top + 17.0f, inner.right, inner.bottom }, TextRole::Small, theme.textDim, 14.0f,
            2);
        top += 17.0f + used + 14.0f;
    }
}

} // namespace

void drawBottleneckView(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits)
{
    (void)hits; // экран пока только показывает расчёт: действий на нём нет

    const Columns columns{ area };
    const float top = area.top;
    drawVerdictCard(renderer, model, theme, columns.span(0, 12, top, 214.0f));
    drawPairCard(renderer, model, theme, columns.span(0, 7, top + 228.0f, 250.0f));
    drawLoadCard(renderer, model, theme, columns.span(7, 5, top + 228.0f, 250.0f));

    const float adviceTop = top + 492.0f;
    drawAdviceCard(renderer, model, theme, columns.span(0, 12, adviceTop, 260.0f));

    // Методика — внизу: вывод важнее пояснения, но проверить его всё равно должно быть можно.
    const Rect note = columns.span(0, 12, top + 766.0f, 118.0f);
    drawCard(renderer, note, theme);
    const Rect inner = note.inset(kCardPadding);
    drawCardHeader(renderer, note, L"Как считаем",
        L"оба источника показаны отдельно: живые замеры точнее любого справочника", theme);
    drawWrappedText(renderer,
        L"Во время игры берём загрузку процессора (с учётом самого занятого ядра) и видеокарты: узкое место — тот узел, "
        L"что ближе к 100 %, а процент показывает, насколько простаивает второй. Если игры нет, считаем прогноз по "
        L"индексам производительности и разрешения экрана: потолок CPU от пикселей почти не зависит, потолок GPU падает "
        L"при росте разрешения. Прогноз — оценка ±15 % и живые замеры он не заменяет.",
        Rect{ inner.left, inner.top + 40.0f, inner.right, inner.bottom }, TextRole::Small, theme.textDim, 15.0f, 3);
}

} // namespace yawa

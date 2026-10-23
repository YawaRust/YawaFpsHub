#include "ui/dashboard_view.h"
#include "ui/layout.h"

#include "metrics/units.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace yawa {
namespace {

// Сетка 12 колонок как в прототипе: карточки занимают фиксированное число колонок,
// поэтому экран перестраивается вместе с окном, а не «плывёт».
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

std::wstring signedPercent(double percent)
{
    const std::string text = formatWithUnit(std::abs(percent), MetricUnit::Percent, 1);
    return wideText((percent >= 0.0 ? "+" : "-") + text);
}

std::wstring historyWindowLabel(int intervalMs)
{
    const int seconds = static_cast<int>(kHistoryCapacity) * intervalMs / 1000;
    if (seconds >= 120) {
        return L"Последние " + wideText(std::to_string(seconds / 60) + " минут");
    }
    return L"Последние " + wideText(std::to_string(seconds) + " секунд");
}

// Заголовок карточки: крупное название и строка-пояснение под ним.
float drawCardHeader(Renderer& renderer, const Rect& card, std::wstring_view title, std::wstring_view subtitle,
    const Theme& theme)
{
    const Rect inner = card.inset(kCardPadding);
    renderer.text(title, Rect{ inner.left, inner.top, inner.right, inner.top + 20.0f }, TextRole::Heading, theme.text);
    if (!subtitle.empty()) {
        renderer.text(subtitle, Rect{ inner.left, inner.top + 18.0f, inner.right - 76.0f, inner.top + 34.0f },
            TextRole::Tiny, theme.textFaint);
    }
    return inner.top + 36.0f;
}

void drawTag(Renderer& renderer, const Rect& card, std::wstring_view label, bool live, const Theme& theme)
{
    const float width = renderer.measureText(label, TextRole::Tiny) + 30.0f;
    const Rect tag{ card.right - kCardPadding - width, card.top + kCardPadding - 2.0f, card.right - kCardPadding,
        card.top + kCardPadding + 20.0f };
    renderer.fillRoundRect(tag, 11.0f, live ? theme.accentSoft : theme.panelSoft);
    renderer.fillCircle(tag.left + 11.0f, tag.centerY(), 3.0f, live ? theme.accent : theme.textFaint);
    renderer.text(label, Rect{ tag.left + 20.0f, tag.top, tag.right - 6.0f, tag.bottom }, TextRole::Tiny,
        live ? theme.accent : theme.textFaint);
}

void drawStatBlock(Renderer& renderer, const Rect& rect, std::wstring_view label, std::wstring_view value,
    const Theme& theme)
{
    renderer.text(label, Rect{ rect.left, rect.top, rect.right, rect.top + 14.0f }, TextRole::Tiny, theme.textFaint);
    renderer.text(value, Rect{ rect.left, rect.top + 14.0f, rect.right, rect.top + 38.0f }, TextRole::Value,
        theme.text);
}

void drawFpsCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);

    const std::wstring game = model.foregroundApp.empty() ? L"игр не запущено" : model.foregroundApp;
    const std::wstring subtitle
        = game + L" · цель " + wideText(std::to_string(model.settings.capture.targetHz) + " Гц");
    drawCardHeader(renderer, card, L"Кадры в секунду", subtitle, theme);
    drawTag(renderer, card, model.etwRunning ? L"LIVE" : L"НЕТ ДАННЫХ", model.etwRunning, theme);

    // Заголовок карточки занимает 36 px: крупное число начинается ниже, иначе
    // подзаголовок «игра · цель 60 Гц» проступал сквозь цифры.
    const float numberTop = inner.top + 38.0f;
    const std::wstring value = wideNumber(model.fps.currentFps, MetricUnit::Count, 0);
    const float valueWidth = renderer.measureText(value, TextRole::Display);
    renderer.text(value, Rect{ inner.left - 2.0f, numberTop, inner.left + valueWidth + 8.0f, numberTop + 58.0f },
        TextRole::Display, model.etwRunning ? theme.accent : theme.textFaint);
    renderer.text(L"fps", Rect{ inner.left + valueWidth + 2.0f, numberTop + 40.0f, inner.left + valueWidth + 44.0f,
                            numberTop + 58.0f },
        TextRole::Small, theme.textFaint);

    if (model.fps.averageFps > 0.1) {
        const double delta = (model.fps.currentFps - model.fps.averageFps) / model.fps.averageFps * 100.0;
        const std::wstring text = signedPercent(delta);
        const float width = renderer.measureText(text, TextRole::Tiny) + 16.0f;
        const Rect chip{ inner.left, numberTop + 54.0f, inner.left + width, numberTop + 72.0f };
        const Color color = delta >= 0.0 ? theme.accent : theme.warn;
        renderer.fillRoundRect(chip, 9.0f, color.withAlpha(0.16f));
        renderer.text(text, chip, TextRole::Tiny, color, TextAlign::Center);
        renderer.text(L"к среднему за сессию", Rect{ chip.right + 8.0f, chip.top, chip.right + 150.0f, chip.bottom },
            TextRole::Tiny, theme.textFaint);
    }

    // Статистика кадров — сетка 2x2 справа, как в прототипе.
    const float statsLeft = inner.left + std::max(inner.width() * 0.66f, 220.0f);
    const float columnWidth = (inner.right - statsLeft) / 2.0f;
    const std::array<std::array<std::wstring, 2>, 4> stats{ std::array<std::wstring, 2>{ L"1% LOW",
                                                                                       wideNumber(model.fps.low1Fps,
                                                                                                  MetricUnit::Count, 0) },
        { L"0.1% LOW", wideNumber(model.fps.low01Fps, MetricUnit::Count, 0) },
        { L"FRAME TIME", wideNumber(model.fps.frameTimeMs, MetricUnit::Milliseconds, 2) },
        { L"ПРОПУЩЕНО", wideNumber(model.fps.droppedShare * 100.0, MetricUnit::Percent, 1) } };

    const float statsTop = inner.top + 26.0f;
    for (std::size_t index = 0; index < stats.size(); ++index) {
        const float left = statsLeft + static_cast<float>(index % 2) * columnWidth;
        const float top = statsTop + (index < 2 ? 0.0f : 44.0f);
        drawStatBlock(renderer, Rect{ left, top, left + columnWidth - 16.0f, top + 36.0f }, stats[index][0],
            stats[index][1], theme);
    }

    // График прижимается к низу карточки и не залезает под чип разницы:
    // в коротком окне он просто становится ниже.
    const float chartTop = std::max(numberTop + 78.0f, inner.bottom - 74.0f);
    const Rect chart{ inner.left, chartTop, inner.right, inner.bottom };
    if (chart.height() < 22.0f) {
        return;
    }
    renderer.drawLine(chart.left, chart.bottom, chart.right, chart.bottom, theme.line, 1.0f);
    renderer.drawLine(chart.left, chart.top + chart.height() * 0.5f, chart.right, chart.top + chart.height() * 0.5f,
        theme.line.withAlpha(0.5f), 1.0f);

    const double budget = 1000.0 / static_cast<double>(std::max(model.settings.capture.targetHz, 30));
    float maximum = static_cast<float>(budget) * 1.8f;
    std::array<float, kHistoryCapacity> history{};
    const std::size_t count = model.store.copyHistory(MetricId::FrameTime, history.data(), history.size());
    for (std::size_t index = 0; index < count; ++index) {
        maximum = std::max(maximum, history[index]);
    }

    const float budgetY = chart.bottom - std::clamp(static_cast<float>(budget) / maximum, 0.0f, 1.0f) * chart.height();
    renderer.drawDashedLine(chart.left, budgetY, chart.right, budgetY, theme.lineStrong, 1.0f);
    renderer.text(L"бюджет " + wideNumber(budget, MetricUnit::Milliseconds, 1),
        Rect{ chart.left, chart.top - 14.0f, chart.right, chart.top }, TextRole::Tiny, theme.textFaint,
        TextAlign::Right);

    if (count < 2) {
        renderer.text(L"данные появятся после первых кадров", chart, TextRole::Small, theme.textFaint,
            TextAlign::Center);
        return;
    }

    renderer.drawSeries(ChartSeries{ history.data(), count, theme.accent, true }, chart, 0.0f, maximum, 1.6f);
}

void drawSessionCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    float rowTop = drawCardHeader(renderer, card, L"Сессия", L"длительность и темп кадров", theme) + 6.0f;

    const std::array<std::pair<std::wstring, std::wstring>, 4> rows{
        std::pair<std::wstring, std::wstring>{ L"Длительность", formatClock(model.elapsedMs) },
        { L"Средний FPS", wideNumber(model.fps.averageFps, MetricUnit::Count, 1) },
        { L"Медианный frame time", wideNumber(model.fps.frameTimeMs, MetricUnit::Milliseconds, 2) },
        { L"Пропущено кадров", wideNumber(model.fps.droppedShare * 100.0, MetricUnit::Percent, 1) },
    };

    for (const auto& [label, value] : rows) {
        if (rowTop + 16.0f > inner.bottom - 38.0f) {
            return;
        }
        drawLabelValue(renderer, Rect{ inner.left, rowTop, inner.right, rowTop + 16.0f }, label, value, theme);
        rowTop += 21.0f;
    }

    const double budgetMs = 1000.0 / static_cast<double>(std::max(model.settings.capture.targetHz, 30));
    const double used = model.fps.frameTimeMs / budgetMs;
    const float barTop = inner.bottom - 34.0f;
    if (barTop < rowTop) {
        return;
    }

    renderer.text(L"Бюджет кадра " + wideText(std::to_string(model.settings.capture.targetHz) + " Гц"),
        Rect{ inner.left, barTop, inner.centerX(), barTop + 16.0f }, TextRole::Small, theme.textDim);
    renderer.text(wideNumber(std::clamp(used, 0.0, 1.999) * 100.0, MetricUnit::Percent, 0),
        Rect{ inner.centerX(), barTop, inner.right, barTop + 16.0f }, TextRole::Body,
        used > 1.0 ? theme.danger : theme.text, TextAlign::Right);
    drawBar(renderer, Rect{ inner.left, barTop + 20.0f, inner.right, barTop + 26.0f },
        static_cast<float>(std::clamp(used, 0.0, 1.0)), used > 1.0 ? theme.danger : theme.accent, theme);
    renderer.text(wideNumber(std::max(budgetMs - model.fps.frameTimeMs, 0.0), MetricUnit::Milliseconds, 2)
            + L" свободно",
        Rect{ inner.left, barTop + 28.0f, inner.right, barTop + 42.0f }, TextRole::Tiny, theme.textFaint);
}

// Радиус кольца считается от места под карточку: два радиуса плюс подпись снизу.
float gaugeRadius(const Rect& area)
{
    return std::clamp((area.height() - 22.0f) * 0.5f, 16.0f, 40.0f);
}

void drawGaugeCard(Renderer& renderer, const Rect& inner, double load, const Color& color, const Theme& theme)
{
    const float radius = gaugeRadius(inner);
    const float centerX = inner.left + radius + 4.0f;
    const float centerY = inner.top + 12.0f + radius;
    drawGauge(renderer, centerX, centerY, radius, static_cast<float>(load / 100.0), color, theme.line,
        wideNumber(load, MetricUnit::Percent, 0), theme.text);
    renderer.text(L"ЗАГРУЗКА", Rect{ centerX - radius, centerY + radius + 2.0f, centerX + radius,
                                  centerY + radius + 14.0f },
        TextRole::Tiny, theme.textFaint, TextAlign::Center);
}

void drawCpuCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    // Подзаголовок как в демо: модель процессора и число ядер с потоками.
    std::wstring subtitle = L"загрузка как в диспетчере задач";
    if (!model.cpuName.empty()) {
        subtitle = model.cpuName;
        if (model.cpuCores > 0) {
            // Склоняем: «1 ядро», «4 ядра», «8 ядер».
            const std::uint32_t cores = model.cpuCores;
            subtitle += L" · " + std::to_wstring(cores)
                + (cores % 10 == 1 && cores % 100 != 11 ? L" ядро"
                        : (cores % 10 >= 2 && cores % 10 <= 4 && (cores % 100 < 12 || cores % 100 > 14) ? L" ядра"
                                                                                                        : L" ядер"));
        }
        if (model.cpuThreads > 0) {
            const std::uint32_t threads = model.cpuThreads;
            subtitle += L" / " + std::to_wstring(threads)
                + (threads % 10 == 1 && threads % 100 != 11 ? L" поток"
                        : (threads % 10 >= 2 && threads % 10 <= 4 && (threads % 100 < 12 || threads % 100 > 14)
                                ? L" потока"
                                : L" потоков"));
        }
    }
    const float top = drawCardHeader(renderer, card, L"Процессор", std::wstring_view{}, theme);
    renderer.text(subtitle, Rect{ inner.left, inner.top + 18.0f, inner.right - 4.0f, inner.top + 34.0f },
        TextRole::Tiny, theme.textFaint);

    const double load = model.store.value(MetricId::CpuLoad);
    drawGaugeCard(renderer, Rect{ inner.left, top, inner.right, inner.bottom }, load, theme.cpu, theme);

    const Rect gaugeArea{ inner.left, top, inner.right, inner.bottom };
    const float radius = gaugeRadius(gaugeArea);
    const float rowsLeft = inner.left + radius * 2.0f + 20.0f;
    float rowTop = top + 2.0f;
    // Четыре строки, как в демо: частота, температура, питание и поток игры.
    const std::array<std::pair<std::wstring, std::wstring>, 4> rows{
        std::pair<std::wstring, std::wstring>{ L"Частота",
            model.availability.cpuClock ? wideMetric(MetricId::CpuClock, model.store.value(MetricId::CpuClock))
                                        : L"нет данных" },
        { L"Температура",
            model.availability.cpuTemperature ? wideMetric(MetricId::CpuTemp, model.store.value(MetricId::CpuTemp))
                                              : L"нет данных" },
        { L"Питание",
            model.store.value(MetricId::CpuPower) > 0.0
                ? wideMetric(MetricId::CpuPower, model.store.value(MetricId::CpuPower))
                : L"нет данных" },
        { L"Поток игры",
            model.store.value(MetricId::GameThreadLoad) > 0.0
                ? wideMetric(MetricId::GameThreadLoad, model.store.value(MetricId::GameThreadLoad))
                : L"нет данных" },
    };

    // Шаг строк подстраивается под высоту карточки: при 790 px все четыре строки
    // обязаны быть видны, а раньше «Поток игры» молча обрезался.
    const float rowsHeight = std::max(inner.bottom - rowTop, 1.0f);
    const float rowStep = std::clamp(rowsHeight / static_cast<float>(rows.size()), 15.0f, 22.0f);
    for (const auto& [label, value] : rows) {
        if (rowTop + 14.0f > inner.bottom) {
            break;
        }
        drawLabelValue(renderer, Rect{ rowsLeft, rowTop, inner.right, rowTop + 14.0f }, label, value, theme);
        rowTop += rowStep;
    }

    // Полоски по ядрам, как в демо: видно, что процессор действительно опрашивается.
    // Полоски по ядрам — только справа от датчика и только когда под них есть место:
    // в короткой карточке они наезжали на подпись «загрузка».
    const float coreLeft = rowsLeft;
    const float coreWidth = inner.right - coreLeft;
    if (model.cpuCores > 0 && coreWidth > 60.0f && inner.bottom - rowTop >= 18.0f) {
        const std::size_t count = std::min<std::size_t>(model.cpuCores, 16);
        const float gap = 3.0f;
        const float barWidth = (coreWidth - gap * static_cast<float>(count - 1)) / static_cast<float>(count);
        const float base = inner.bottom;
        const float height = std::min(16.0f, inner.bottom - rowTop);

        for (std::size_t index = 0; index < count; ++index) {
            const MetricId id = static_cast<MetricId>(static_cast<std::size_t>(MetricId::Core0Load) + index);
            const double ratio = std::clamp(model.store.value(id) / 100.0, 0.0, 1.0);
            const float left = coreLeft + static_cast<float>(index) * (barWidth + gap);
            renderer.fillRoundRect(Rect{ left, base - height, left + barWidth, base }, 2.0f,
                theme.panelSoft);
            const float filled = std::max(2.0f, height * static_cast<float>(ratio));
            renderer.fillRoundRect(Rect{ left, base - filled, left + barWidth, base }, 2.0f, theme.cpu);
        }
    }
}

void drawGpuCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card,
    std::vector<UiHit>& hits)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    const std::wstring name = model.activeGpu < model.gpuNames.size() ? model.gpuNames[model.activeGpu]
                                                                     : L"адаптер не найден";
    const float top = drawCardHeader(renderer, card, L"Видеокарта", name, theme);

    const double load = model.store.value(MetricId::GpuLoad);
    drawGaugeCard(renderer, Rect{ inner.left, top, inner.right, inner.bottom }, load, theme.gpu, theme);

    const float radius = gaugeRadius(Rect{ inner.left, top, inner.right, inner.bottom });
    const float rowsLeft = inner.left + radius * 2.0f + 20.0f;
    float rowTop = top + 8.0f;
    const double vramTotal = model.store.value(MetricId::VramTotal);
    const std::array<std::pair<std::wstring, std::wstring>, 2> rows{
        std::pair<std::wstring, std::wstring>{ L"Температура",
            model.availability.gpuTemperature ? wideMetric(MetricId::GpuTemp, model.store.value(MetricId::GpuTemp))
                                              : L"нет данных" },
        { L"Питание",
            model.availability.gpuPower ? wideMetric(MetricId::GpuPower, model.store.value(MetricId::GpuPower))
                                        : L"нет данных" },
    };

    for (const auto& [label, value] : rows) {
        if (rowTop + 14.0f > inner.bottom) {
            break;
        }
        drawLabelValue(renderer, Rect{ rowsLeft, rowTop, inner.right, rowTop + 14.0f }, label, value, theme);
        rowTop += 22.0f;
    }

    if (vramTotal > 0.0 && rowTop + 16.0f <= inner.bottom) {
        drawLabelValue(renderer, Rect{ rowsLeft, rowTop, inner.right, rowTop + 16.0f }, L"Видеопамять",
            wideNumber(model.store.value(MetricId::VramUsed), MetricUnit::Gigabytes, 1) + L" / "
                + wideNumber(vramTotal, MetricUnit::Gigabytes, 1),
            theme);
        rowTop += 22.0f;
    }

    if (model.gpuNames.size() > 1 && inner.bottom - rowTop >= 24.0f) {
        // Выпадающий список адаптеров: кнопка-заголовок и раскрытый список с галочкой.
        const Rect picker{ rowsLeft, inner.bottom - 26.0f, inner.right, inner.bottom };
        drawChip(renderer, picker, model.gpuMenuOpen ? L"скрыть список" : L"выбрать адаптер",
            model.gpuMenuOpen, theme, TextRole::Tiny, isHovered(model, picker));
        hits.push_back(UiHit{ picker, ActionKind::ToggleGpuMenu, model.gpuMenuOpen ? 1 : 0 });

        if (model.gpuMenuOpen) {
            const float listTop = inner.top + 34.0f;
            for (std::size_t index = 0; index < model.gpuNames.size(); ++index) {
                const float optionTop = listTop + static_cast<float>(index) * 26.0f;
                if (optionTop + 24.0f > inner.bottom - 30.0f) {
                    break;
                }
                const Rect row{ inner.left, optionTop, inner.right, optionTop + 24.0f };
                const bool active = index == model.activeGpu;
                renderer.fillRoundRect(row, 7.0f, active ? theme.accentSoft : theme.panelSoft);
                renderer.text(std::to_wstring(index + 1) + L". " + model.gpuNames[index],
                    Rect{ row.left + 8.0f, row.top, row.right - 24.0f, row.bottom }, TextRole::Tiny,
                    active ? theme.accent : theme.textDim);
                if (active) {
                    renderer.fillCircle(row.right - 12.0f, row.centerY(), 3.0f, theme.accent);
                }
                hits.push_back(UiHit{ row, ActionKind::SelectGpu, static_cast<int>(index) });
            }
        }
    }
}

void drawMemoryCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    const float top = drawCardHeader(renderer, card, L"Память", L"занято, кэш и свободно", theme);

    const double used = model.store.value(MetricId::RamUsed);
    const double cache = model.store.value(MetricId::RamCache);
    const double free = model.store.value(MetricId::RamFree);
    const double total = used + free;

    renderer.text(wideNumber(used, MetricUnit::Gigabytes, 1),
        Rect{ inner.left, top - 6.0f, inner.left + 160.0f, top + 26.0f }, TextRole::Value, theme.text);
    const float valueWidth = renderer.measureText(wideNumber(used, MetricUnit::Gigabytes, 1), TextRole::Value);
    renderer.text(L"из " + wideNumber(total, MetricUnit::Gigabytes, 1) + L" ГБ",
        Rect{ inner.left + valueWidth + 8.0f, top + 4.0f, inner.right, top + 20.0f }, TextRole::Tiny,
        theme.textFaint);

    const float barTop = top + 32.0f;
    if (barTop + 8.0f < inner.bottom) {
        drawBar(renderer, Rect{ inner.left, barTop, inner.right, barTop + 6.0f },
            total > 0.0 ? static_cast<float>(used / total) : 0.0f, theme.ram, theme);
    }

    const Rect legend{ inner.left, barTop + 10.0f, inner.right, barTop + 24.0f };
    if (legend.bottom > inner.bottom) {
        return;
    }

    // Легенда собирается из частей и обрезается по ширине, а не пропадает целиком.
    const std::array<std::pair<std::wstring, Color>, 3> parts{
        std::pair<std::wstring, Color>{ L"Приложения " + wideNumber(used, MetricUnit::Gigabytes, 1), theme.ram },
        { L"Кэш " + wideNumber(cache, MetricUnit::Gigabytes, 1), theme.disk },
        { L"Свободно " + wideNumber(free, MetricUnit::Gigabytes, 1), theme.lineStrong } };

    float x = legend.left;
    for (const auto& [text, color] : parts) {
        const float width = renderer.measureText(text, TextRole::Tiny) + 16.0f;
        if (x + width > legend.right) {
            break;
        }
        renderer.fillCircle(x + 3.0f, legend.centerY(), 3.0f, color);
        renderer.text(text, Rect{ x + 11.0f, legend.top, x + width, legend.bottom }, TextRole::Tiny, theme.textDim);
        x += width + 6.0f;
    }

    float rowTop = legend.bottom + 6.0f;
    const std::array<std::pair<std::wstring, std::wstring>, 2> rows{
        std::pair<std::wstring, std::wstring>{ L"Своп-файл",
            wideNumber(model.store.value(MetricId::PageFile), MetricUnit::Gigabytes, 1) },
        { L"Всего", wideNumber(total, MetricUnit::Gigabytes, 1) },
    };

    for (const auto& [label, value] : rows) {
        if (rowTop + 16.0f > inner.bottom) {
            break;
        }
        drawLabelValue(renderer, Rect{ inner.left, rowTop, inner.right, rowTop + 16.0f }, label, value, theme);
        rowTop += 20.0f;
    }
}

void drawPerformanceCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    const float top = drawCardHeader(renderer, card, L"Производительность",
        historyWindowLabel(model.settings.capture.intervalMs), theme);

    const Rect legend{ inner.left + 260.0f, card.top + kCardPadding - 2.0f, inner.right,
        card.top + kCardPadding + 18.0f };
    renderer.fillCircle(legend.left + 4.0f, legend.centerY(), 3.0f, theme.cpu);
    renderer.text(L"Процессор", Rect{ legend.left + 12.0f, legend.top, legend.left + 90.0f, legend.bottom },
        TextRole::Tiny, theme.textFaint);
    renderer.fillCircle(legend.left + 104.0f, legend.centerY(), 3.0f, theme.gpu);
    renderer.text(L"Видеокарта", Rect{ legend.left + 112.0f, legend.top, legend.right, legend.bottom }, TextRole::Tiny,
        theme.textFaint);

    const Rect chart{ inner.left, top + 4.0f, inner.right, inner.bottom - 2.0f };
    if (chart.height() < 24.0f) {
        return;
    }

    for (int step = 1; step < 4; ++step) {
        const float y = chart.top + chart.height() * static_cast<float>(step) / 4.0f;
        renderer.drawLine(chart.left, y, chart.right, y, theme.line.withAlpha(0.6f), 1.0f);
    }

    std::array<float, kHistoryCapacity> cpuHistory{};
    const std::size_t cpuCount = model.store.copyHistory(MetricId::CpuLoad, cpuHistory.data(), cpuHistory.size());
    std::array<float, kHistoryCapacity> gpuHistory{};
    const std::size_t gpuCount = model.store.copyHistory(MetricId::GpuLoad, gpuHistory.data(), gpuHistory.size());
    if (gpuCount >= 2) {
        renderer.drawSeries(ChartSeries{ gpuHistory.data(), gpuCount, theme.gpu, true }, chart, 0.0f, 100.0f, 1.2f);
    }
    if (cpuCount >= 2) {
        renderer.drawSeries(ChartSeries{ cpuHistory.data(), cpuCount, theme.cpu, false }, chart, 0.0f, 100.0f, 1.4f);
    }

    renderer.text(L"100 %", Rect{ chart.left, chart.top - 12.0f, chart.right, chart.top - 1.0f }, TextRole::Tiny,
        theme.textFaint, TextAlign::Right);
    renderer.text(L"0", Rect{ chart.left, chart.bottom - 13.0f, chart.right, chart.bottom }, TextRole::Tiny,
        theme.textFaint, TextAlign::Right);
}

void drawDiskNetCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    renderer.text(L"Диски и сеть", Rect{ inner.left, inner.top, inner.right, inner.top + 18.0f }, TextRole::Heading,
        theme.text);
    const float top = inner.top + 26.0f;

    if (!model.settings.capture.includeDiskNet) {
        renderer.text(L"раздел отключён в настройках", Rect{ inner.left, top, inner.right, top + 18.0f },
            TextRole::Small, theme.textFaint);
        return;
    }

    struct Cell {
        std::wstring_view label;
        MetricId id;
    };

    const std::array<Cell, 4> cells{ Cell{ L"Диск: чтение", MetricId::DiskRead },
        { L"Диск: запись", MetricId::DiskWrite }, { L"Сеть: входящий", MetricId::NetDown },
        { L"Сеть: исходящий", MetricId::NetUp } };

    // Карточка узкая: одна колонка, иначе подпись и значение наезжают друг на друга.
    const float rowHeight = std::clamp((inner.bottom - top) / 4.0f, 14.0f, 22.0f);
    float rowTop = top + 2.0f;

    for (std::size_t index = 0; index < cells.size(); ++index) {
        if (rowTop + rowHeight > inner.bottom) {
            break;
        }
        const bool known = model.store.hasValue(cells[index].id);
        renderer.text(cells[index].label, Rect{ inner.left, rowTop, inner.centerX(), rowTop + rowHeight },
            TextRole::Tiny, theme.textDim);
        renderer.text(known ? wideMetric(cells[index].id, model.store.value(cells[index].id)) : L"—",
            Rect{ inner.centerX(), rowTop, inner.right, rowTop + rowHeight }, TextRole::Small,
            known ? theme.text : theme.textFaint, TextAlign::Right);
        rowTop += rowHeight;
    }
}

void drawTemperaturesCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    renderer.text(L"Температуры", Rect{ inner.left, inner.top, inner.right, inner.top + 18.0f }, TextRole::Heading,
        theme.text);
    const float top = inner.top + 30.0f;

    struct Row {
        std::wstring_view label;
        MetricId id;
        bool available;
    };

    const std::array<Row, 3> rows{ Row{ L"Процессор", MetricId::CpuTemp, model.availability.cpuTemperature },
        { L"Видеокарта", MetricId::GpuTemp, model.availability.gpuTemperature },
        { L"NVMe", MetricId::DiskTemp, model.availability.disk } };

    float rowTop = top + 2.0f;
    for (const Row& row : rows) {
        if (rowTop + 15.0f > inner.bottom) {
            break;
        }
        const bool known = row.available && model.store.hasValue(row.id) && model.store.value(row.id) > 0.0;
        renderer.text(row.label, Rect{ inner.left, rowTop, inner.centerX(), rowTop + 15.0f }, TextRole::Small,
            theme.textDim);
        renderer.text(known ? wideMetric(row.id, model.store.value(row.id)) : L"нет данных",
            Rect{ inner.centerX(), rowTop, inner.right, rowTop + 15.0f }, TextRole::Body,
            known ? theme.text : theme.textFaint, TextAlign::Right);
        rowTop += 19.0f;
    }
}

void drawRuleRow(Renderer& renderer, const Rect& row, std::wstring_view label, std::wstring_view state, int severity,
    const Theme& theme)
{
    const Color dot = severity < 0 ? theme.textFaint : (severity == 0 ? theme.accent : theme.danger);
    renderer.fillCircle(row.left + 4.0f, row.centerY(), 3.0f, dot);
    renderer.text(label, Rect{ row.left + 16.0f, row.top, row.right - 90.0f, row.bottom }, TextRole::Small,
        severity < 0 ? theme.textFaint : theme.textDim);
    renderer.text(state, Rect{ row.right - 90.0f, row.top, row.right, row.bottom }, TextRole::Tiny,
        severity < 0 ? theme.textFaint : dot, TextAlign::Right);
}

void drawAlertsCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    renderer.text(L"Уведомления", Rect{ inner.left, inner.top, inner.right, inner.top + 18.0f }, TextRole::Heading,
        theme.text);
    const float top = inner.top + 30.0f;

    const double gpuTemp = model.store.value(MetricId::GpuTemp);
    const bool hasGpuTemp = model.availability.gpuTemperature && model.store.hasValue(MetricId::GpuTemp);
    const bool hasLow = model.fps.low1Fps > 0.0;

    const double ramUsed = model.store.value(MetricId::RamUsed);
    const double ramFree = model.store.value(MetricId::RamFree);
    const double ramShare = ramUsed + ramFree > 0.0 ? ramUsed / (ramUsed + ramFree) : 0.0;
    const double budgetMs = 1000.0 / static_cast<double>(std::max(model.settings.capture.targetHz, 30));

    struct Rule {
        std::wstring_view label;
        std::wstring_view state;
        int severity;
    };

    const std::array<Rule, 4> rules{
        Rule{ L"Перегрев GPU выше 83 °C", hasGpuTemp ? (gpuTemp > 83.0 ? L"сработало" : L"норма") : L"нет данных",
            hasGpuTemp ? (gpuTemp > 83.0 ? 1 : 0) : -1 },
        { L"1% low ниже 60 fps", hasLow ? (model.fps.low1Fps < 60.0 ? L"сработало" : L"норма") : L"нет данных",
            hasLow ? (model.fps.low1Fps < 60.0 ? 1 : 0) : -1 },
        { L"Память занята больше 90 %", ramShare > 0.0 ? (ramShare > 0.9 ? L"сработало" : L"норма") : L"нет данных",
            ramShare > 0.0 ? (ramShare > 0.9 ? 1 : 0) : -1 },
        { L"Кадр дольше бюджета", hasLow ? (model.fps.frameTimeMs > budgetMs ? L"сработало" : L"норма") : L"нет данных",
            hasLow ? (model.fps.frameTimeMs > budgetMs ? 1 : 0) : -1 },
    };

    float rowTop = top + 2.0f;
    for (const Rule& rule : rules) {
        if (rowTop + 15.0f > inner.bottom) {
            break;
        }
        drawRuleRow(renderer, Rect{ inner.left, rowTop, inner.right, rowTop + 15.0f }, rule.label, rule.state,
            rule.severity, theme);
        rowTop += 19.0f;
    }
}

} // namespace

void drawDashboard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits)
{
    // Раскладка как в прототипе: карточки идут одной колонкой, высоты сняты с демо.
    // Экран получает демо-высоту целиком, а окно показывает его часть и прокручивает.
    const Columns columns{ area };

    const auto drawClipped = [&](const Rect& card, const auto& draw) {
        renderer.pushClip(card);
        draw(card);
        renderer.popClip();
    };

    float top = area.top;
    const auto place = [&](float height) {
        const Rect card{ area.left, top, area.right, top + height };
        top += height + kGap;
        return card;
    };

    drawClipped(place(344.0f), [&](const Rect& card) { drawFpsCard(renderer, model, theme, card); });
    drawClipped(place(350.0f), [&](const Rect& card) { drawSessionCard(renderer, model, theme, card); });
    drawClipped(place(280.0f), [&](const Rect& card) { drawCpuCard(renderer, model, theme, card); });
    drawClipped(place(246.0f), [&](const Rect& card) { drawGpuCard(renderer, model, theme, card, hits); });
    drawClipped(place(286.0f), [&](const Rect& card) { drawMemoryCard(renderer, model, theme, card); });
    drawClipped(place(288.0f), [&](const Rect& card) { drawPerformanceCard(renderer, model, theme, card); });
    drawClipped(place(347.0f), [&](const Rect& card) { drawDiskNetCard(renderer, model, theme, card); });
    drawClipped(columns.span(0.0f, 6.0f, top, 286.0f),
        [&](const Rect& card) { drawTemperaturesCard(renderer, model, theme, card); });
    drawClipped(columns.span(6.0f, 6.0f, top, 286.0f),
        [&](const Rect& card) { drawAlertsCard(renderer, model, theme, card); });
}

} // namespace yawa

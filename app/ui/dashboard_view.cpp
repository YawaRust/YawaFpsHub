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

float drawCardHeader(Renderer& renderer, const Rect& card, std::wstring_view title, std::wstring_view subtitle,
    const Theme& theme)
{
    const Rect inner = card.inset(kCardPadding);
    renderer.text(title, Rect{ inner.left, inner.top, inner.right, inner.top + 20.0f }, TextRole::Heading, theme.text);
    if (!subtitle.empty()) {
        renderer.text(subtitle, Rect{ inner.left, inner.top + 18.0f, inner.right, inner.top + 34.0f }, TextRole::Tiny,
            theme.textFaint);
    }
    return inner.top + 34.0f;
}

void drawTag(Renderer& renderer, const Rect& card, std::wstring_view label, bool live, const Theme& theme)
{
    const float width = renderer.measureText(label, TextRole::Tiny) + 30.0f;
    const Rect tag{ card.right - kCardPadding - width, card.top + kCardPadding, card.right - kCardPadding,
        card.top + kCardPadding + 22.0f };
    renderer.fillRoundRect(tag, 11.0f, live ? theme.accentSoft : theme.panelSoft);
    renderer.fillCircle(tag.left + 11.0f, tag.centerY(), 3.0f, live ? theme.accent : theme.textFaint);
    renderer.text(label, Rect{ tag.left + 20.0f, tag.top, tag.right - 6.0f, tag.bottom }, TextRole::Tiny,
        live ? theme.accent : theme.textFaint);
}

void drawFpsCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);

    const std::wstring game = model.foregroundApp.empty() ? L"игр не запущено" : model.foregroundApp;
    const std::wstring subtitle = game + L" · цель " + wideText(std::to_string(model.settings.capture.targetHz) + " Гц");
    drawCardHeader(renderer, card, L"Кадры в секунду", subtitle, theme);
    drawTag(renderer, card, model.etwRunning ? L"LIVE" : L"НЕТ ДАННЫХ", model.etwRunning, theme);

    const float numberTop = inner.top + 40.0f;
    const std::wstring value = wideNumber(model.fps.currentFps, MetricUnit::Count, 0);
    renderer.text(value, Rect{ inner.left - 2.0f, numberTop, inner.left + 220.0f, numberTop + 66.0f },
        TextRole::Display, model.etwRunning ? theme.accent : theme.textFaint);
    const float valueWidth = renderer.measureText(value, TextRole::Display);
    renderer.text(L"fps", Rect{ inner.left + valueWidth + 4.0f, numberTop + 46.0f, inner.left + valueWidth + 48.0f,
                            numberTop + 66.0f },
        TextRole::Small, theme.textFaint);

    if (model.fps.averageFps > 0.1) {
        const double delta = (model.fps.currentFps - model.fps.averageFps) / model.fps.averageFps * 100.0;
        renderer.text(signedPercent(delta), Rect{ inner.left, numberTop + 66.0f, inner.left + 80.0f,
                                               numberTop + 84.0f },
            TextRole::Small, delta >= 0.0 ? theme.accent : theme.warn);
        renderer.text(L"к среднему за сессию", Rect{ inner.left + 84.0f, numberTop + 66.0f, inner.left + 240.0f,
                                                 numberTop + 84.0f },
            TextRole::Tiny, theme.textFaint);
    }

    const std::array<std::pair<std::wstring, std::wstring>, 4> stats{
        std::pair<std::wstring, std::wstring>{ L"1% LOW", wideNumber(model.fps.low1Fps, MetricUnit::Count, 0) },
        { L"0.1% LOW", wideNumber(model.fps.low01Fps, MetricUnit::Count, 0) },
        { L"FRAME TIME", wideNumber(model.fps.frameTimeMs, MetricUnit::Milliseconds, 2) },
        { L"ПРОПУЩЕНО", wideNumber(model.fps.droppedShare * 100.0, MetricUnit::Percent, 1) },
    };

    const float statsLeft = inner.left + std::max(inner.width() * 0.40f, 250.0f);
    const float columnWidth = (inner.right - statsLeft) / 4.0f;
    for (std::size_t index = 0; index < stats.size(); ++index) {
        const float left = statsLeft + static_cast<float>(index) * columnWidth;
        renderer.text(stats[index].first, Rect{ left, numberTop + 14.0f, left + columnWidth - 10.0f,
                                                  numberTop + 30.0f },
            TextRole::Tiny, theme.textFaint);
        renderer.text(stats[index].second, Rect{ left, numberTop + 30.0f, left + columnWidth - 10.0f,
                                                   numberTop + 56.0f },
            TextRole::Value, theme.text);
    }

    const float chartHeight = std::max(36.0f, inner.height() * 0.32f);
    const Rect chart{ inner.left, inner.bottom - chartHeight, inner.right, inner.bottom };
    renderer.drawLine(chart.left, chart.bottom, chart.right, chart.bottom, theme.line, 1.0f);

    const double budget = 1000.0 / static_cast<double>(std::max(model.settings.capture.targetHz, 30));
    float maximum = static_cast<float>(budget) * 1.8f;
    std::array<float, kHistoryCapacity> history{};
    const std::size_t count = model.store.copyHistory(MetricId::FrameTime, history.data(), history.size());
    for (std::size_t index = 0; index < count; ++index) {
        maximum = std::max(maximum, history[index]);
    }

    const float budgetY = chart.bottom - std::clamp(static_cast<float>(budget) / maximum, 0.0f, 1.0f) * chart.height();
    renderer.drawDashedLine(chart.left, budgetY, chart.right, budgetY, theme.lineStrong, 1.0f);

    if (count < 2) {
        renderer.text(L"данные появятся после первых кадров", chart, TextRole::Small, theme.textFaint,
            TextAlign::Center);
        return;
    }

    renderer.drawSeries(ChartSeries{ history.data(), count, theme.accent, false }, chart, 0.0f, maximum, 1.6f);
    renderer.text(wideNumber(budget, MetricUnit::Milliseconds, 1) + L" бюджет",
        Rect{ chart.left, budgetY - 14.0f, chart.right, budgetY - 2.0f }, TextRole::Tiny, theme.textFaint,
        TextAlign::Right);
}

void drawSessionCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    const float top = drawCardHeader(renderer, card, L"Сессия", L"профиль включается по запуску игры", theme);

    const std::array<std::pair<std::wstring, std::wstring>, 3> rows{
        std::pair<std::wstring, std::wstring>{ L"Длительность", formatClock(model.elapsedMs) },
        { L"Средний FPS", wideNumber(model.fps.averageFps, MetricUnit::Count, 1) },
        { L"Отброшено кадров", wideNumber(model.fps.droppedShare * 100.0, MetricUnit::Percent, 1) },
    };

    float rowTop = top + 14.0f;
    for (const auto& [label, value] : rows) {
        if (rowTop + 18.0f > inner.bottom) {
            return;
        }
        drawLabelValue(renderer, Rect{ inner.left, rowTop, inner.right, rowTop + 16.0f }, label, value, theme);
        rowTop += 24.0f;
    }

    const double budgetMs = 1000.0 / static_cast<double>(std::max(model.settings.capture.targetHz, 30));
    const double used = model.fps.frameTimeMs / budgetMs;
    rowTop += 8.0f;
    if (rowTop + 30.0f > inner.bottom) {
        return;
    }
    renderer.text(L"Бюджет кадра " + wideText(std::to_string(model.settings.capture.targetHz) + " Гц"),
        Rect{ inner.left, rowTop, inner.centerX(), rowTop + 16.0f }, TextRole::Small, theme.textDim);
    renderer.text(wideNumber(std::clamp(used, 0.0, 1.999) * 100.0, MetricUnit::Percent, 0),
        Rect{ inner.centerX(), rowTop, inner.right, rowTop + 16.0f }, TextRole::Body,
        used > 1.0 ? theme.danger : theme.text, TextAlign::Right);
    drawBar(renderer, Rect{ inner.left, rowTop + 22.0f, inner.right, rowTop + 28.0f },
        static_cast<float>(std::clamp(used, 0.0, 1.0)), used > 1.0 ? theme.danger : theme.accent, theme);
}

void drawCpuCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    const float top = drawCardHeader(renderer, card, L"Процессор", L"загрузка по счётчику диспетчера задач", theme);

    const double load = model.store.value(MetricId::CpuLoad);
    const float radius = std::min(46.0f, (inner.height() - 50.0f) * 0.5f);
    drawGauge(renderer, inner.left + radius + 4.0f, top + radius + 8.0f, radius,
        static_cast<float>(load / 100.0), theme.cpu, theme.line, wideNumber(load, MetricUnit::Percent, 0), theme.text);

    const float rowsLeft = inner.left + radius * 2.0f + 18.0f;
    float rowTop = top + 6.0f;
    const std::array<std::pair<std::wstring, std::wstring>, 3> rows{
        std::pair<std::wstring, std::wstring>{ L"Частота",
            model.availability.cpuClock ? wideMetric(MetricId::CpuClock, model.store.value(MetricId::CpuClock))
                                        : L"нет данных" },
        { L"Температура",
            model.availability.cpuTemperature ? wideMetric(MetricId::CpuTemp, model.store.value(MetricId::CpuTemp))
                                              : L"нет данных" },
        { L"Поток игры",
            model.store.value(MetricId::GameThreadLoad) > 0.0
                ? wideMetric(MetricId::GameThreadLoad, model.store.value(MetricId::GameThreadLoad))
                : L"нет данных" },
    };

    for (const auto& [label, value] : rows) {
        if (rowTop + 16.0f > inner.bottom) {
            break;
        }
        drawLabelValue(renderer, Rect{ rowsLeft, rowTop, inner.right, rowTop + 16.0f }, label, value, theme);
        rowTop += 22.0f;
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
    const float radius = std::min(46.0f, (inner.height() - 50.0f) * 0.5f);
    drawGauge(renderer, inner.left + radius + 4.0f, top + radius + 8.0f, radius,
        static_cast<float>(load / 100.0), theme.gpu, theme.line, wideNumber(load, MetricUnit::Percent, 0), theme.text);

    const float rowsLeft = inner.left + radius * 2.0f + 18.0f;
    float rowTop = top + 6.0f;
    const double vramTotal = model.store.value(MetricId::VramTotal);
    const std::array<std::pair<std::wstring, std::wstring>, 3> rows{
        std::pair<std::wstring, std::wstring>{ L"Температура",
            model.availability.gpuTemperature ? wideMetric(MetricId::GpuTemp, model.store.value(MetricId::GpuTemp))
                                              : L"нет данных" },
        { L"Питание",
            model.availability.gpuPower ? wideMetric(MetricId::GpuPower, model.store.value(MetricId::GpuPower))
                                        : L"нет данных" },
        { L"Видеопамять",
            vramTotal > 0.0 ? wideNumber(model.store.value(MetricId::VramUsed), MetricUnit::Gigabytes, 1) + L" / "
                    + wideNumber(vramTotal, MetricUnit::Gigabytes, 1)
                            : L"нет данных" },
    };

    for (const auto& [label, value] : rows) {
        if (rowTop + 16.0f > inner.bottom) {
            break;
        }
        drawLabelValue(renderer, Rect{ rowsLeft, rowTop, inner.right, rowTop + 16.0f }, label, value, theme);
        rowTop += 22.0f;
    }

    if (model.gpuNames.size() > 1 && inner.bottom - rowTop > 24.0f) {
        const Rect picker{ rowsLeft, rowTop + 2.0f, inner.right, rowTop + 26.0f };
        drawChip(renderer, picker, L"сменить адаптер", false, theme, TextRole::Tiny);
        hits.push_back(UiHit{ picker, ActionKind::CycleGpu, 0 });
    }
}

void drawMemoryCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    const float top = drawCardHeader(renderer, card, L"Память", L"занято и свободно, как в диспетчере задач", theme);

    const double used = model.store.value(MetricId::RamUsed);
    const double free = model.store.value(MetricId::RamFree);
    const double total = used + free;

    renderer.text(wideNumber(used, MetricUnit::Gigabytes, 1),
        Rect{ inner.left, top + 4.0f, inner.left + 150.0f, top + 40.0f }, TextRole::Value, theme.text);
    const float valueWidth = renderer.measureText(wideNumber(used, MetricUnit::Gigabytes, 1), TextRole::Value);
    renderer.text(L"из " + wideNumber(total, MetricUnit::Gigabytes, 1),
        Rect{ inner.left + valueWidth + 8.0f, top + 22.0f, inner.right, top + 40.0f }, TextRole::Tiny,
        theme.textFaint);

    const float barTop = top + 50.0f;
    if (barTop + 8.0f < inner.bottom) {
        drawBar(renderer, Rect{ inner.left, barTop, inner.right, barTop + 6.0f },
            total > 0.0 ? static_cast<float>(used / total) : 0.0f, theme.ram, theme);
    }

    float rowTop = barTop + 22.0f;
    const std::array<std::pair<std::wstring, std::wstring>, 3> rows{
        std::pair<std::wstring, std::wstring>{ L"Кэш", wideNumber(model.store.value(MetricId::RamCache),
                                                   MetricUnit::Gigabytes, 1) },
        { L"Свободно", wideNumber(free, MetricUnit::Gigabytes, 1) },
        { L"Своп-файл", wideNumber(model.store.value(MetricId::PageFile), MetricUnit::Gigabytes, 1) },
    };

    for (const auto& [label, value] : rows) {
        if (rowTop + 16.0f > inner.bottom) {
            break;
        }
        drawLabelValue(renderer, Rect{ inner.left, rowTop, inner.right, rowTop + 16.0f }, label, value, theme);
        rowTop += 22.0f;
    }
}

void drawPerformanceCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    const float top = drawCardHeader(renderer, card, L"Производительность",
        historyWindowLabel(model.settings.capture.intervalMs), theme);

    const Rect legend{ inner.centerX() - 120.0f, card.top + kCardPadding, inner.right, card.top + kCardPadding + 18.0f };
    renderer.fillCircle(legend.left + 4.0f, legend.centerY(), 3.0f, theme.cpu);
    renderer.text(L"Процессор", Rect{ legend.left + 12.0f, legend.top, legend.left + 90.0f, legend.bottom },
        TextRole::Tiny, theme.textFaint);
    renderer.fillCircle(legend.left + 104.0f, legend.centerY(), 3.0f, theme.gpu);
    renderer.text(L"Видеокарта", Rect{ legend.left + 112.0f, legend.top, legend.right, legend.bottom }, TextRole::Tiny,
        theme.textFaint);

    const Rect chart{ inner.left, top + 6.0f, inner.right, inner.bottom - 2.0f };
    if (chart.height() < 24.0f) {
        return;
    }

    for (int step = 1; step < 4; ++step) {
        const float y = chart.top + chart.height() * static_cast<float>(step) / 4.0f;
        renderer.drawLine(chart.left, y, chart.right, y, theme.line.withAlpha(0.6f), 1.0f);
    }

    std::array<float, kHistoryCapacity> history{};
    const std::size_t cpuCount = model.store.copyHistory(MetricId::CpuLoad, history.data(), history.size());
    if (cpuCount >= 2) {
        renderer.drawSeries(ChartSeries{ history.data(), cpuCount, theme.cpu, true }, chart, 0.0f, 100.0f, 1.4f);
    }
    const std::size_t gpuCount = model.store.copyHistory(MetricId::GpuLoad, history.data(), history.size());
    if (gpuCount >= 2) {
        renderer.drawSeries(ChartSeries{ history.data(), gpuCount, theme.gpu, false }, chart, 0.0f, 100.0f, 1.4f);
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
    const float top = drawCardHeader(renderer, card, L"Диски и сеть", L"скорости за последнюю секунду", theme);

    if (!model.settings.capture.includeDiskNet) {
        renderer.text(L"раздел отключён в настройках", Rect{ inner.left, top + 6.0f, inner.right, top + 24.0f },
            TextRole::Small, theme.textFaint);
        return;
    }

    const std::array<MetricId, 4> ids{ MetricId::DiskRead, MetricId::DiskWrite, MetricId::NetDown, MetricId::NetUp };
    float rowTop = top + 6.0f;
    const float step = std::max(20.0f, (inner.bottom - rowTop) / 4.0f);

    for (MetricId id : ids) {
        if (rowTop + 16.0f > inner.bottom) {
            break;
        }
        const MetricInfo& info = metricInfo(id);
        renderer.text(wideText(info.label), Rect{ inner.left, rowTop, inner.centerX(), rowTop + 16.0f },
            TextRole::Small, theme.textDim);
        renderer.text(model.store.hasValue(id) ? wideMetric(id, model.store.value(id)) : L"нет данных",
            Rect{ inner.centerX(), rowTop, inner.right, rowTop + 16.0f }, TextRole::Body,
            model.store.hasValue(id) ? theme.text : theme.textFaint, TextAlign::Right);
        rowTop += step;
    }
}

void drawTemperatureRow(Renderer& renderer, const Rect& row, std::wstring_view label, MetricId id,
    bool available, const AppModel& model, const Theme& theme)
{
    renderer.text(label, Rect{ row.left, row.top, row.centerX(), row.bottom }, TextRole::Small, theme.textDim);
    const bool known = available && model.store.hasValue(id) && model.store.value(id) > 0.0;
    renderer.text(known ? wideMetric(id, model.store.value(id)) : L"нет данных",
        Rect{ row.centerX(), row.top, row.right, row.bottom }, TextRole::Body,
        known ? theme.text : theme.textFaint, TextAlign::Right);
}

void drawTemperaturesCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);
    const float top = drawCardHeader(renderer, card, L"Температуры", L"что отдают датчики без драйвера", theme);

    float rowTop = top + 6.0f;
    const std::array<std::pair<std::wstring_view, MetricId>, 4> rows{
        std::pair<std::wstring_view, MetricId>{ L"Процессор", MetricId::CpuTemp },
        { L"Видеокарта", MetricId::GpuTemp },
        { L"Hotspot GPU", MetricId::GpuHotspot },
        { L"NVMe", MetricId::DiskTemp },
    };

    for (const auto& [label, id] : rows) {
        if (rowTop + 16.0f > inner.bottom) {
            break;
        }
        bool available = true;
        if (id == MetricId::CpuTemp) {
            available = model.availability.cpuTemperature;
        } else if (id == MetricId::GpuTemp || id == MetricId::GpuHotspot) {
            available = model.availability.gpuTemperature;
        } else if (id == MetricId::DiskTemp) {
            available = model.availability.disk;
        }
        drawTemperatureRow(renderer, Rect{ inner.left, rowTop, inner.right, rowTop + 16.0f }, label, id, available,
            model, theme);
        rowTop += 22.0f;
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
    const float top = drawCardHeader(renderer, card, L"Уведомления", L"правила считаются по живым значениям", theme);

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

    float rowTop = top + 8.0f;
    for (const Rule& rule : rules) {
        if (rowTop + 16.0f > inner.bottom) {
            break;
        }
        drawRuleRow(renderer, Rect{ inner.left, rowTop, inner.right, rowTop + 16.0f }, rule.label, rule.state,
            rule.severity, theme);
        rowTop += 22.0f;
    }
}

} // namespace

void drawDashboard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits)
{
    const Columns columns{ area };

    const float usable = area.height() - kGap * 3.0f;
    const float first = std::max(150.0f, usable * 0.30f);
    const float second = std::max(140.0f, usable * 0.27f);
    const float third = std::max(120.0f, usable * 0.24f);
    const float fourth = std::max(96.0f, usable - first - second - third);

    const float row1 = area.top;
    const float row2 = row1 + first + kGap;
    const float row3 = row2 + second + kGap;
    const float row4 = row3 + third + kGap;

    drawFpsCard(renderer, model, theme, columns.span(0.0f, 8.0f, row1, first));
    drawSessionCard(renderer, model, theme, columns.span(8.0f, 4.0f, row1, first));
    drawCpuCard(renderer, model, theme, columns.span(0.0f, 4.0f, row2, second));
    drawGpuCard(renderer, model, theme, columns.span(4.0f, 4.0f, row2, second), hits);
    drawMemoryCard(renderer, model, theme, columns.span(8.0f, 4.0f, row2, second));
    drawPerformanceCard(renderer, model, theme, columns.span(0.0f, 8.0f, row3, third));
    drawDiskNetCard(renderer, model, theme, columns.span(8.0f, 4.0f, row3, third));
    drawTemperaturesCard(renderer, model, theme, columns.span(0.0f, 6.0f, row4, fourth));
    drawAlertsCard(renderer, model, theme, columns.span(6.0f, 6.0f, row4, fourth));
}

} // namespace yawa

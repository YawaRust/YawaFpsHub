#include "ui/dashboard_view.h"
#include "ui/layout.h"

#include <algorithm>
#include <string>

namespace yawa {
namespace {

void drawFpsCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);

    renderer.text(L"Кадры в секунду", Rect{ inner.left, inner.top, inner.left + 220.0f, inner.top + 22.0f },
        TextRole::Heading, theme.text);

    const std::wstring subtitle = wideText("цель " + std::to_string(model.settings.capture.targetHz) + " Гц");
    renderer.text(subtitle, Rect{ inner.left, inner.top + 20.0f, inner.left + 220.0f, inner.top + 38.0f },
        TextRole::Tiny, theme.textFaint);

    const std::wstring fps = wideNumber(model.fps.currentFps, MetricUnit::Count, 0);
    renderer.text(fps, Rect{ inner.left - 2.0f, inner.top + 34.0f, inner.left + 240.0f, inner.top + 110.0f },
        TextRole::Display, model.paused ? theme.textDim : theme.accent);
    const float fpsWidth = renderer.measureText(fps, TextRole::Display);
    renderer.text(L"fps", Rect{ inner.left + fpsWidth + 8.0f, inner.top + 92.0f, inner.left + fpsWidth + 60.0f,
                            inner.top + 112.0f },
        TextRole::Small, theme.textFaint);

    const float statsTop = inner.top + 42.0f;
    const float statsLeft = inner.left + 250.0f;
    const float columnWidth = 96.0f;
    const std::pair<std::wstring, std::wstring> stats[] = {
        { L"1% LOW", wideNumber(model.fps.low1Fps, MetricUnit::Count, 0) },
        { L"0.1% LOW", wideNumber(model.fps.low01Fps, MetricUnit::Count, 0) },
        { L"FRAME TIME", wideNumber(model.fps.frameTimeMs, MetricUnit::Milliseconds, 2) },
        { L"ПРОПУЩЕНО", wideNumber(model.fps.droppedShare * 100.0, MetricUnit::Percent, 1) },
    };

    for (std::size_t index = 0; index < 4; ++index) {
        const float left = statsLeft + static_cast<float>(index) * columnWidth;
        renderer.text(stats[index].first, Rect{ left, statsTop, left + columnWidth - 8.0f, statsTop + 16.0f },
            TextRole::Tiny, theme.textFaint);
        renderer.text(stats[index].second, Rect{ left, statsTop + 16.0f, left + columnWidth - 8.0f, statsTop + 42.0f },
            TextRole::Body, theme.text);
    }

    const Rect chart{ inner.left, inner.bottom - 92.0f, inner.right, inner.bottom };
    const std::size_t count = model.store.copyHistory(MetricId::FrameTime, model.historyScratch.data(),
        kHistoryCapacity);

    float maximum = 1.0f;
    for (std::size_t index = 0; index < count; ++index) {
        maximum = std::max(maximum, model.historyScratch[index]);
    }
    maximum *= 1.15f;

    const float targetMs = 1000.0f / static_cast<float>(std::max(model.settings.capture.targetHz, 30));
    if (targetMs < maximum) {
        const float y = chart.bottom - (targetMs / maximum) * chart.height();
        renderer.drawDashedLine(chart.left, y, chart.right, y, theme.lineStrong, 1.0f);
    }

    renderer.drawSeries(ChartSeries{ model.historyScratch.data(), count, theme.accent.withAlpha(0.16f), true }, chart, 0.0f,
        maximum, 1.0f);
    renderer.drawSeries(ChartSeries{ model.historyScratch.data(), count, theme.accent, false }, chart, 0.0f, maximum, 1.6f);
}

void drawSessionCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);

    renderer.text(L"Сессия", Rect{ inner.left, inner.top, inner.right, inner.top + 22.0f }, TextRole::Heading,
        theme.text);
    renderer.text(L"профиль включается по запуску игры",
        Rect{ inner.left, inner.top + 20.0f, inner.right, inner.top + 38.0f }, TextRole::Tiny, theme.textFaint);

    const std::pair<std::wstring, std::wstring> rows[] = {
        { L"Длительность", formatClock(model.elapsedMs) },
        { L"Средний FPS", wideNumber(model.fps.averageFps, MetricUnit::Count, 1) },
        { L"Frame time", wideNumber(model.fps.frameTimeMs, MetricUnit::Milliseconds, 2) },
        { L"Пропущено за секунду", wideNumber(model.fps.droppedShare * 100.0, MetricUnit::Percent, 1) },
    };

    float top = inner.top + 50.0f;
    for (const auto& [label, value] : rows) {
        drawLabelValue(renderer, Rect{ inner.left, top, inner.right, top + 18.0f }, label, value, theme);
        top += 26.0f;
    }

    const float budgetTop = top + 6.0f;
    const float budgetMs = 1000.0f / static_cast<float>(std::max(model.settings.capture.targetHz, 30));
    const float used = std::clamp(static_cast<float>(model.fps.frameTimeMs) / budgetMs, 0.0f, 1.0f);

    renderer.text(L"Бюджет кадра", Rect{ inner.left, budgetTop, inner.centerX(), budgetTop + 16.0f }, TextRole::Small,
        theme.textFaint);
    renderer.text(wideNumber(used * 100.0, MetricUnit::Percent, 0),
        Rect{ inner.centerX(), budgetTop, inner.right, budgetTop + 16.0f }, TextRole::Body, theme.text,
        TextAlign::Right);
    drawBar(renderer, Rect{ inner.left, budgetTop + 20.0f, inner.right, budgetTop + 26.0f }, used, theme.accent,
        theme.line);
}

void drawProcessorCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);

    renderer.text(L"Процессор", Rect{ inner.left, inner.top, inner.right, inner.top + 22.0f }, TextRole::Heading,
        theme.text);
    renderer.text(wideText(std::to_string(model.settings.capture.intervalMs) + " мс · "
                     + (model.availability.cpuClock ? "частота есть" : "частота недоступна")),
        Rect{ inner.left, inner.top + 20.0f, inner.right, inner.top + 38.0f }, TextRole::Tiny, theme.textFaint);

    const float load = static_cast<float>(model.store.value(MetricId::CpuLoad));
    const float radius = 30.0f;
    const float centerX = inner.left + radius + 8.0f;
    const float centerY = inner.top + 84.0f;

    drawGauge(renderer, centerX, centerY, radius, load / 100.0f, theme.cpu, theme.line,
        wideNumber(load, MetricUnit::Percent, 0), theme.text);

    const float rowsLeft = centerX + radius + 26.0f;
    const std::pair<std::wstring, std::wstring> rows[] = {
        { L"Частота", wideMetric(MetricId::CpuClock, model.store.value(MetricId::CpuClock)) },
        { L"Температура", model.availability.cpuTemperature
                ? wideMetric(MetricId::CpuTemp, model.store.value(MetricId::CpuTemp))
                : L"нет данных" },
        { L"Поток игры", wideMetric(MetricId::GameThreadLoad, model.store.value(MetricId::GameThreadLoad)) },
    };

    float top = inner.top + 56.0f;
    for (const auto& [label, value] : rows) {
        drawLabelValue(renderer, Rect{ rowsLeft, top, inner.right, top + 18.0f }, label, value, theme);
        top += 26.0f;
    }
}

void drawGraphicsCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card,
    std::vector<UiHit>& hits)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);

    renderer.text(L"Видеокарта", Rect{ inner.left, inner.top, inner.left + 140.0f, inner.top + 22.0f },
        TextRole::Heading, theme.text);

    const std::wstring name = model.activeGpu < model.gpuNames.size() ? model.gpuNames[model.activeGpu]
                                                                      : L"адаптер не найден";
    renderer.text(name, Rect{ inner.left, inner.top + 20.0f, inner.right, inner.top + 38.0f }, TextRole::Tiny,
        theme.textFaint);

    if (model.gpuNames.size() > 1) {
        const Rect picker{ inner.right - 132.0f, inner.top - 2.0f, inner.right, inner.top + 20.0f };
        renderer.fillRoundRect(picker, 8.0f, theme.panelSoft);
        renderer.strokeRoundRect(picker, 8.0f, theme.lineStrong, 1.0f);
        renderer.text(L"сменить GPU", picker, TextRole::Tiny, theme.textDim, TextAlign::Center);
        hits.push_back(UiHit{ picker, ActionKind::CycleGpu, 0 });
    }

    const float load = static_cast<float>(model.store.value(MetricId::GpuLoad));
    const float radius = 30.0f;
    const float centerX = inner.left + radius + 8.0f;
    const float centerY = inner.top + 84.0f;
    drawGauge(renderer, centerX, centerY, radius, load / 100.0f, theme.gpu, theme.line,
        wideNumber(load, MetricUnit::Percent, 0), theme.text);

    const float rowsLeft = centerX + radius + 26.0f;
    const std::pair<std::wstring, std::wstring> rows[] = {
        { L"Температура", model.availability.gpuTemperature
                ? wideMetric(MetricId::GpuTemp, model.store.value(MetricId::GpuTemp))
                : L"нет данных" },
        { L"Питание", model.availability.gpuPower ? wideMetric(MetricId::GpuPower, model.store.value(MetricId::GpuPower))
                                                  : L"нет данных" },
        { L"Частота", wideMetric(MetricId::GpuClock, model.store.value(MetricId::GpuClock)) },
    };

    float top = inner.top + 56.0f;
    for (const auto& [label, value] : rows) {
        drawLabelValue(renderer, Rect{ rowsLeft, top, inner.right, top + 18.0f }, label, value, theme);
        top += 26.0f;
    }

    const float vramTotal = std::max(static_cast<float>(model.store.value(MetricId::VramTotal)), 0.001f);
    const float used = static_cast<float>(model.store.value(MetricId::VramUsed) / vramTotal);

    renderer.text(L"Видеопамять", Rect{ inner.left, inner.bottom - 34.0f, inner.left + 120.0f, inner.bottom - 16.0f },
        TextRole::Small, theme.textFaint);
    renderer.text(wideMetric(MetricId::VramUsed, model.store.value(MetricId::VramUsed)) + L" / "
            + wideMetric(MetricId::VramTotal, vramTotal),
        Rect{ inner.left + 120.0f, inner.bottom - 34.0f, inner.right, inner.bottom - 16.0f }, TextRole::Small,
        theme.text, TextAlign::Right);
    drawBar(renderer, Rect{ inner.left, inner.bottom - 12.0f, inner.right, inner.bottom - 6.0f }, used, theme.gpu,
        theme.line);
}

void drawMemoryCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);

    renderer.text(L"Память", Rect{ inner.left, inner.top, inner.right, inner.top + 22.0f }, TextRole::Heading,
        theme.text);
    const double ramTotal = model.store.value(MetricId::RamUsed) + model.store.value(MetricId::RamFree);
    const std::wstring subtitle = ramTotal > 0.0
        ? L"занято " + wideNumber(model.store.value(MetricId::RamUsed), MetricUnit::Gigabytes, 1) + L" из "
            + wideNumber(ramTotal, MetricUnit::Gigabytes, 1)
        : std::wstring(L"нет данных");
    renderer.text(subtitle, Rect{ inner.left, inner.top + 20.0f, inner.right, inner.top + 38.0f }, TextRole::Tiny,
        theme.textFaint);

    const std::wstring used = wideMetric(MetricId::RamUsed, model.store.value(MetricId::RamUsed));
    renderer.text(used, Rect{ inner.left, inner.top + 42.0f, inner.left + 140.0f, inner.top + 78.0f },
        TextRole::Value, theme.text);
    renderer.text(L"занято", Rect{ inner.left + 90.0f, inner.top + 48.0f, inner.left + 160.0f, inner.top + 70.0f },
        TextRole::Small, theme.textFaint);

    const double total = model.store.value(MetricId::RamUsed) + model.store.value(MetricId::RamFree);
    const float fraction = total > 0.0 ? static_cast<float>(model.store.value(MetricId::RamUsed) / total) : 0.0f;

    drawBar(renderer, Rect{ inner.left, inner.top + 86.0f, inner.right, inner.top + 94.0f }, fraction, theme.ram,
        theme.line);

    const std::pair<std::wstring, std::wstring> rows[] = {
        { L"Кэш", wideMetric(MetricId::RamCache, model.store.value(MetricId::RamCache)) },
        { L"Свободно", wideMetric(MetricId::RamFree, model.store.value(MetricId::RamFree)) },
        { L"Своп-файл", wideMetric(MetricId::PageFile, model.store.value(MetricId::PageFile)) },
    };

    // Строки ужимаются под фактическую высоту карточки: при маленьком окне они
    // не вылезают за рамку и не наезжают на полосу памяти.
    const float rowsTop = inner.top + 100.0f;
    const float rowStep = std::clamp((inner.bottom - rowsTop) / 3.0f, 18.0f, 26.0f);
    float top = rowsTop;
    for (const auto& [label, value] : rows) {
        drawLabelValue(renderer, Rect{ inner.left, top, inner.right, top + 18.0f }, label, value, theme);
        top += rowStep;
    }
}

void drawPerformanceCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);

    renderer.text(L"Загрузка", Rect{ inner.left, inner.top, inner.right, inner.top + 22.0f }, TextRole::Heading,
        theme.text);

    const std::pair<std::wstring_view, Color> legend[] = {
        { L"CPU", theme.cpu },
        { L"GPU", theme.gpu },
        { L"RAM", theme.ram },
    };

    float legendLeft = inner.left + 90.0f;
    for (const auto& [label, color] : legend) {
        renderer.fillRoundRect(Rect{ legendLeft, inner.top + 6.0f, legendLeft + 7.0f, inner.top + 13.0f }, 2.0f, color);
        renderer.text(label, Rect{ legendLeft + 12.0f, inner.top + 2.0f, legendLeft + 54.0f, inner.top + 18.0f },
            TextRole::Tiny, theme.textDim);
        legendLeft += 56.0f;
    }

    const Rect chart = Rect{ inner.left, inner.top + 28.0f, inner.right, inner.bottom };
    for (int step = 1; step < 4; ++step) {
        const float y = chart.top + chart.height() * static_cast<float>(step) / 4.0f;
        renderer.drawLine(chart.left, y, chart.right, y, theme.line.withAlpha(0.6f), 1.0f);
    }

    const MetricId series[] = { MetricId::CpuLoad, MetricId::GpuLoad, MetricId::RamUsed };
    const Color colors[] = { theme.cpu, theme.gpu, theme.ram };

    for (std::size_t index = 0; index < 3; ++index) {
        const std::size_t count = model.store.copyHistory(series[index],
            model.historyScratch.data(), kHistoryCapacity);
        if (count < 2) {
            continue;
        }

        if (series[index] == MetricId::RamUsed) {
            // Память в гигабайтах: к общей шкале процентов приводим по занятой доле.
            const double total = model.store.value(MetricId::RamUsed) + model.store.value(MetricId::RamFree);
            if (total > 0.0) {
                for (std::size_t point = 0; point < count; ++point) {
                    model.historyScratch[point] = static_cast<float>(model.historyScratch[point] * 100.0 / total);
                }
            }
        }

        renderer.drawSeries(ChartSeries{ model.historyScratch.data(), count, colors[index], false }, chart, 0.0f, 100.0f,
            1.5f);
    }
}

void drawIoCard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& card)
{
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);

    renderer.text(L"Диски и сеть", Rect{ inner.left, inner.top, inner.right, inner.top + 22.0f }, TextRole::Heading,
        theme.text);

    const MetricId series[] = { MetricId::DiskRead, MetricId::DiskWrite, MetricId::NetDown, MetricId::NetUp };
    float top = inner.top + 34.0f;
    const float rowHeight = (inner.bottom - top) / 4.0f;

    for (MetricId id : series) {
        const std::wstring label = wideText(metricInfo(id).label);
        renderer.text(label, Rect{ inner.left, top, inner.centerX(), top + 16.0f }, TextRole::Tiny, theme.textFaint);
        renderer.text(wideMetric(id, model.store.value(id)),
            Rect{ inner.centerX(), top, inner.right, top + 16.0f }, TextRole::Small, theme.text, TextAlign::Right);

        const std::size_t count = model.store.copyHistory(id, model.historyScratch.data(),
            kHistoryCapacity);
        float maximum = 0.1f;
        for (std::size_t index = 0; index < count; ++index) {
            maximum = std::max(maximum, model.historyScratch[index]);
        }

        renderer.drawSeries(ChartSeries{ model.historyScratch.data(), count, theme.disk.withAlpha(0.85f), false },
            Rect{ inner.left, top + 18.0f, inner.right, top + rowHeight - 8.0f }, 0.0f, maximum * 1.15f, 1.2f);
        top += rowHeight;
    }
}

} // namespace

void drawDashboard(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits)
{
    const float leftWidth = std::max(area.width() * 0.63f, 420.0f);
    const Rect leftColumn{ area.left, area.top, area.left + leftWidth - kGap, area.bottom };
    const Rect rightColumn{ leftColumn.right + kGap, area.top, area.right, area.bottom };

    const float topRow = std::clamp(area.height() * 0.34f, 190.0f, 228.0f);
    const float middleRow = std::clamp(area.height() * 0.28f, 176.0f, 200.0f);

    drawFpsCard(renderer, model, theme, Rect{ leftColumn.left, leftColumn.top, leftColumn.right,
                                                leftColumn.top + topRow });
    drawSessionCard(renderer, model, theme, Rect{ rightColumn.left, rightColumn.top, rightColumn.right,
                                                    rightColumn.top + topRow });

    const float cardsTop = area.top + topRow + kGap;
    const float cardWidth = (area.width() - kGap * 2.0f) / 3.0f;
    const Rect cpuCard{ area.left, cardsTop, area.left + cardWidth, cardsTop + middleRow };
    const Rect gpuCard{ cpuCard.right + kGap, cardsTop, cpuCard.right + kGap + cardWidth, cardsTop + middleRow };
    const Rect ramCard{ gpuCard.right + kGap, cardsTop, area.right, cardsTop + middleRow };

    drawProcessorCard(renderer, model, theme, cpuCard);
    drawGraphicsCard(renderer, model, theme, gpuCard, hits);
    drawMemoryCard(renderer, model, theme, ramCard);

    const float chartsTop = cardsTop + middleRow + kGap;
    const float chartsHeight = std::max(area.bottom - chartsTop, 120.0f);

    drawPerformanceCard(renderer, model, theme,
        Rect{ leftColumn.left, chartsTop, leftColumn.right, chartsTop + chartsHeight });
    drawIoCard(renderer, model, theme, Rect{ rightColumn.left, chartsTop, rightColumn.right, chartsTop + chartsHeight });
}

} // namespace yawa

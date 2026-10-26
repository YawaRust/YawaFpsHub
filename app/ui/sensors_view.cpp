#include "ui/sensors_view.h"
#include "ui/layout.h"

#include "metrics/overlay_metric.h"
#include "metrics/units.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <string>
#include <vector>

namespace yawa {
namespace {

struct Group {
    std::wstring_view title;
    std::vector<MetricId> ids;
};

constexpr std::array<std::wstring_view, 7> kFilterNames{ L"Все", L"Кадры", L"Процессор", L"Видеокарта", L"Память",
    L"Диски и сеть", L"Система" };

constexpr float kToolbarHeight = 36.0f;
// Две строки панели, как в демо: поиск с фильтрами, под ним «только в оверлее».
constexpr float kToolbarSecondRow = 26.0f;
constexpr float kToolbarGap = 8.0f;
// Компактные строки: на экран помещается больше датчиков, и таблицу не приходится
// листать так часто. Выше плотности демо, но читается без напряжения.
constexpr float kRowHeight = 38.0f;
constexpr float kGroupHeight = 30.0f;

std::vector<Group> makeGroups(std::uint32_t cores)
{
    std::vector<MetricId> cpu { MetricId::CpuLoad };
    // Ядра добавляются только реально существующие: пустых строк «Ядро 17» быть не должно.
    for (std::uint32_t index = 0; index < std::min<std::uint32_t>(cores, 16); ++index) {
        cpu.push_back(static_cast<MetricId>(static_cast<std::size_t>(MetricId::Core0Load) + index));
    }

    cpu.insert(cpu.end(),
        { MetricId::CpuTemp, MetricId::CpuClock, MetricId::CpuPower, MetricId::GameThreadLoad });

    return {
        { L"Кадры", { MetricId::Fps, MetricId::FrameTime, MetricId::Low1Fps, MetricId::Low01Fps, MetricId::AvgFps,
                        MetricId::DroppedFrames } },
        { L"Процессор", std::move(cpu) },
        { L"Видеокарта", { MetricId::GpuLoad, MetricId::GpuTemp, MetricId::GpuHotspot, MetricId::GpuClock,
                             MetricId::GpuMemClock, MetricId::GpuPower, MetricId::GpuFan, MetricId::GpuEncode,
                             MetricId::GpuDecode, MetricId::PcieLoad, MetricId::VramUsed } },
        { L"Память", { MetricId::RamUsed, MetricId::RamLoad, MetricId::RamCache, MetricId::PageFile,
                         MetricId::PageFileLoad } },
        { L"Диски и сеть", { MetricId::DiskRead, MetricId::DiskWrite, MetricId::DiskTemp, MetricId::NetDown,
                               MetricId::NetUp } },
        { L"Система", { MetricId::ChipsetTemp, MetricId::SelfCpu, MetricId::SelfRam, MetricId::SelfThreads } },
    };
}

bool valueKnown(MetricId id, const AppModel& model)
{
    if (!model.store.hasValue(id)) {
        return false;
    }

    // Медиа-движки и PCI-e есть только там, где их реально отдают счётчики/драйвер.
    if (id == MetricId::GpuEncode || id == MetricId::GpuDecode) {
        return model.availability.gpuMedia;
    }
    if (id == MetricId::PcieLoad) {
        return model.availability.pcie;
    }
    if (id == MetricId::PageFileLoad || id == MetricId::PageFileTotal) {
        // Ноль процентов/гигабайт — валидное значение: подкачка может быть пустой.
        return model.availability.pageFile;
    }

    // Ноль у этих датчиков означает «источника нет», а не реальный ноль.
    switch (id) {
    case MetricId::CpuTemp:
        return model.availability.cpuTemperature && model.store.value(id) > 0.0;
    case MetricId::CpuClock:
        return model.availability.cpuClock && model.store.value(id) > 0.0;
    case MetricId::CpuPower:
    case MetricId::CpuPeakLoad:
    case MetricId::GameThreadLoad:
    case MetricId::GpuHotspot:
    case MetricId::GpuPower:
    case MetricId::GpuFan:
    case MetricId::VramTotal:
    case MetricId::DiskTemp:
    case MetricId::ChipsetTemp:
        return model.store.value(id) > 0.0;
    case MetricId::GpuTemp:
        return model.availability.gpuTemperature && model.store.value(id) > 0.0;
    case MetricId::GpuLoad:
        return model.availability.gpuLoad;
    default:
        return true;
    }
}

// towupper в локали «C» не переводит кириллицу в верхний регистр, поэтому заголовки
// групп переводим сами: это единственный способ получить капс без setlocale.
wchar_t upperSymbol(wchar_t symbol)
{
    if (symbol >= L'а' && symbol <= L'я') {
        return static_cast<wchar_t>(symbol - 0x20);
    }
    if (symbol == L'ё') {
        return L'Ё';
    }
    return static_cast<wchar_t>(std::towupper(symbol));
}

std::wstring upperCopy(std::wstring_view text)
{
    std::wstring result(text);
    for (wchar_t& symbol : result) {
        symbol = upperSymbol(symbol);
    }
    return result;
}

std::wstring lowerCopy(std::wstring_view text)
{
    std::wstring result(text);
    for (wchar_t& symbol : result) {
        symbol = static_cast<wchar_t>(std::towlower(symbol));
    }
    return result;
}

bool matchesQuery(MetricId id, std::wstring_view group, const std::wstring& query)
{
    if (query.empty()) {
        return true;
    }
    const std::wstring label = lowerCopy(wideText(metricInfo(id).label));
    return label.find(query) != std::wstring::npos || lowerCopy(group).find(query) != std::wstring::npos;
}

// Цвет пипа как в демо: у каждой группы свой оттенок.
Color pipColor(std::size_t groupIndex, const Theme& theme)
{
    switch (groupIndex) {
    case 0:
        return theme.accent;
    case 1:
        return theme.cpu;
    case 2:
        return theme.gpu;
    case 3:
        return theme.ram;
    case 4:
        return theme.disk;
    case 5:
        return theme.warn;
    default:
        return theme.textDim;
    }
}

void drawSearchBox(Renderer& renderer, const AppModel& model, const Theme& theme, const Rect& rect,
    std::vector<UiHit>& hits)
{
    const bool active = model.sensorSearchActive;
    const bool hover = isHovered(model, rect);
    renderer.fillRoundRect(rect, 10.0f, active || hover ? theme.panelSoft : theme.panel);
    renderer.strokeRoundRect(rect, 10.0f, active ? theme.accentGlow : theme.line, 1.0f);

    const float iconX = rect.left + 16.0f;
    const float iconY = rect.centerY();
    renderer.strokeCircle(iconX, iconY - 1.0f, 5.0f, active ? theme.accent : theme.textFaint, 1.4f);
    renderer.drawLine(iconX + 4.0f, iconY + 3.0f, iconX + 8.0f, iconY + 7.0f,
        active ? theme.accent : theme.textFaint, 1.4f);

    const std::wstring query = wideText(model.sensorQuery);
    const Rect textRect{ rect.left + 32.0f, rect.top, rect.right - 12.0f, rect.bottom };
    if (query.empty() && !active) {
        renderer.text(L"Найти датчик: температура, частота, VRAM…", textRect, TextRole::Small, theme.textFaint);
    } else {
        renderer.text(active ? query + L"|" : query, textRect, TextRole::Small, active ? theme.text : theme.textDim);
    }

    hits.push_back(UiHit{ rect, ActionKind::FocusSensorSearch, 0 });
}

} // namespace

void drawSensors(Renderer& renderer, AppModel& model, const Theme& theme, const Rect& area,
    std::vector<UiHit>& hits)
{
    // --- Панель инструментов: поиск, фильтры, «только в оверлее».
    const float searchWidth = 288.0f;
    const Rect search{ area.left, area.top, area.left + searchWidth, area.top + kToolbarHeight };
    drawSearchBox(renderer, model, theme, search, hits);

    // «Только в оверлее» — вторая строка панели под поиском, как в демо.
    const float secondRowTop = area.top + kToolbarHeight + kToolbarGap;
    const Rect overlaySwitch{ area.left, secondRowTop, area.left + 210.0f, secondRowTop + kToolbarSecondRow };
    const bool overlayOn = model.overlayOnly;
    const bool switchHover = isHovered(model, overlaySwitch);
    // Подпись слева, переключатель сразу за ней: так строка читается как одна пара.
    renderer.text(L"Только в оверлее",
        Rect{ overlaySwitch.left, overlaySwitch.top, overlaySwitch.left + 152.0f, overlaySwitch.bottom },
        TextRole::Small, overlayOn || switchHover ? theme.text : theme.textDim);
    const Rect track{ overlaySwitch.left + 160.0f, overlaySwitch.centerY() - 9.0f, overlaySwitch.left + 196.0f,
        overlaySwitch.centerY() + 9.0f };
    renderer.fillRoundRect(track, 9.0f, overlayOn ? theme.accentSoft : theme.panelSoft);
    if (overlayOn) {
        renderer.strokeRoundRect(track, 9.0f, theme.accentGlow, 1.0f);
    }
    renderer.fillCircle(overlayOn ? track.right - 9.0f : track.left + 9.0f, track.centerY(), 6.0f,
        overlayOn ? theme.accent : theme.textFaint);
    hits.push_back(UiHit{ overlaySwitch, ActionKind::ToggleOverlayOnly, 0 });

    // Фильтры подстраиваются под ширину окна: на 1040 px последний чип раньше обрезался.
    float filterLeft = search.right + 10.0f;
    const float filterRight = area.right;
    float gap = 6.0f;
    float padding = 12.0f;
    float textWidth = 0.0f;
    for (std::wstring_view name : kFilterNames) {
        textWidth += renderer.measureText(name, TextRole::Small);
    }
    const float available = filterRight - filterLeft;
    if (textWidth + padding * 2.0f * static_cast<float>(kFilterNames.size())
            + gap * static_cast<float>(kFilterNames.size() - 1)
        > available) {
        gap = 4.0f;
        padding = std::max(6.0f,
            (available - textWidth - gap * static_cast<float>(kFilterNames.size() - 1))
                / (2.0f * static_cast<float>(kFilterNames.size())));
    }

    for (std::size_t index = 0; index < kFilterNames.size(); ++index) {
        const float width = renderer.measureText(kFilterNames[index], TextRole::Small) + padding * 2.0f;
        if (filterLeft + width > filterRight) {
            break;
        }
        const Rect chip{ filterLeft, area.top, filterLeft + width, area.top + kToolbarHeight - 2.0f };
        const bool active = index == 0 ? model.sensorFilter < 0 : model.sensorFilter == static_cast<int>(index) - 1;
        drawChip(renderer, chip, kFilterNames[index], active, theme, TextRole::Small, isHovered(model, chip));
        hits.push_back(UiHit{ chip, ActionKind::SetSensorFilter, index == 0 ? -1 : static_cast<int>(index) - 1 });
        filterLeft = chip.right + gap;
    }

    // --- Таблица: одна колонка на весь экран, как в демо.
    const Rect card{ area.left, secondRowTop + kToolbarSecondRow + 12.0f, area.right, area.bottom };
    drawCard(renderer, card, theme);
    const Rect inner = card.inset(kCardPadding);

    // Сетка колонок как в демо: имя, значение, мин, среднее, макс, график, «глаз».
    const float width = inner.width();
    const float nameX = inner.left + 14.0f;
    const float valueRight = inner.left + width * 0.50f;
    const float minRight = inner.left + width * 0.62f;
    const float avgRight = inner.left + width * 0.73f;
    const float maxRight = inner.left + width * 0.84f;
    const float sparkLeft = inner.left + width * 0.85f;
    // Справа жёлоб под полосу прокрутки: строки и подсветка заканчиваются до неё,
    // поэтому полоса больше не ложится поверх текста и плашки наведения.
    constexpr float kScrollGutter = 14.0f;
    const float rowRight = inner.right - kScrollGutter;
    const float sparkRight = rowRight - 36.0f;

    renderer.text(L"ДАТЧИК", Rect{ nameX, inner.top, inner.left + width * 0.3f, inner.top + 14.0f }, TextRole::Tiny,
        theme.textFaint);
    const std::array<std::pair<std::wstring_view, float>, 4> headers{
        std::pair<std::wstring_view, float>{ L"ЗНАЧЕНИЕ", valueRight }, { L"МИН", minRight },
        { L"СРЕДНЕЕ", avgRight }, { L"МАКС", maxRight } };
    for (const auto& [title, right] : headers) {
        renderer.text(title, Rect{ right - 100.0f, inner.top, right, inner.top + 14.0f }, TextRole::Tiny,
            theme.textFaint, TextAlign::Right);
    }
    renderer.text(L"60 С", Rect{ sparkLeft, inner.top, sparkRight, inner.top + 14.0f }, TextRole::Tiny,
        theme.textFaint, TextAlign::Right);
    renderer.drawLine(inner.left, inner.top + 20.0f, rowRight, inner.top + 20.0f, theme.line, 1.0f);

    const std::vector<Group> groups = makeGroups(model.cpuCores);
    const std::wstring query = lowerCopy(wideText(model.sensorQuery));
    const int historySamples = std::max(60000 / std::max(model.settings.capture.intervalMs, 100), 2);

    struct Entry {
        std::size_t group;
        MetricId id;
    };
    std::vector<Entry> entries;
    std::vector<std::size_t> groupStarts;
    std::vector<std::size_t> groupIndexes;

    for (std::size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        const Group& group = groups[groupIndex];
        if (model.sensorFilter >= 0 && static_cast<std::size_t>(model.sensorFilter) != groupIndex) {
            continue;
        }
        bool started = false;
        for (MetricId id : group.ids) {
            if (!matchesQuery(id, group.title, query)) {
                continue;
            }
            bool pinned = model.settings.overlay.metrics[static_cast<std::size_t>(id)];
            for (MetricId detail : kPairedMetricDetails) {
                if (displayMetric(detail) == id) {
                    pinned |= model.settings.overlay.metrics[static_cast<std::size_t>(detail)];
                }
            }
            if (model.overlayOnly && !pinned) {
                continue;
            }
            if (!started) {
                started = true;
                groupStarts.push_back(entries.size());
                groupIndexes.push_back(groupIndex);
            }
            entries.push_back(Entry{ groupIndex, id });
        }
    }

    // Высота содержимого: заголовок группы на каждую непустую группу.
    // Между группами нужен воздух: иначе подсветка строки липнет к следующему заголовку.
    const float groupGap = 12.0f;
    const float contentHeight = static_cast<float>(entries.size()) * kRowHeight
        + static_cast<float>(groupStarts.size()) * (kGroupHeight + groupGap);

    // Низ таблицы освобождает строку под сноску: раньше она висела под карточкой.
    const Rect viewport{ inner.left, inner.top + 26.0f, inner.right, inner.bottom - 24.0f };
    const float maxScroll = std::max(0.0f, contentHeight - viewport.height());
    const float offset = std::clamp(static_cast<float>(model.sensorScroll), 0.0f, maxScroll);

    renderer.pushClip(viewport);
    float top = viewport.top - offset;

    if (entries.empty()) {
        renderer.text(query.empty() ? L"Датчиков нет: проверьте фильтр" : L"Ничего не найдено. Измените запрос.",
            viewport, TextRole::Small, theme.textFaint, TextAlign::Center);
    }

    std::size_t nextGroupStart = 0;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (nextGroupStart < groupStarts.size() && groupStarts[nextGroupStart] == index) {
            // Воздух ставим перед заголовком: иначе подсветка последней строки липла к нему.
            if (nextGroupStart > 0) {
                top += groupGap;
            }
            const Rect headerRect{ inner.left, top, rowRight, top + kGroupHeight - 4.0f };
            if (top + kGroupHeight > viewport.top) {
                renderer.text(upperCopy(groups[groupIndexes[nextGroupStart]].title), headerRect, TextRole::Tiny,
                    theme.accent);
            }
            top += kGroupHeight;
            ++nextGroupStart;
        }

        const MetricId id = entries[index].id;
        const float rowTop = top;

        // Рисуем только целиком видимые строки: раньше внизу оставалась обрезанная
        // пополам строка, и таблица выглядела сломанной.
        if (rowTop + kRowHeight > viewport.top && rowTop + kRowHeight <= viewport.bottom + 0.5f) {
            const Rect row{ inner.left, rowTop, rowRight, rowTop + kRowHeight };
            const bool hover = isHovered(model, row);
            // Тонкая линия под строкой: таблица читается построчно, как в демо.
            if (!hover) {
                renderer.drawLine(inner.left + 10.0f, rowTop + kRowHeight - 0.5f, rowRight - 10.0f,
                    rowTop + kRowHeight - 0.5f, theme.line.withAlpha(0.45f), 1.0f);
            }
            if (hover) {
                // Подсветка строки была почти не виднее фона карточки — теперь это заметная плашка.
                renderer.fillRoundRect(row, 6.0f, theme.accentSoft);
                renderer.fillRoundRect(Rect{ inner.left + 2.0f, rowTop + 4.0f, inner.left + 4.0f,
                                               rowTop + kRowHeight - 4.0f },
                    1.0f, theme.accent);
            }

            const MetricInfo& info = metricInfo(id);
            const MetricSlot& slot = model.store.slot(id);
            const bool known = valueKnown(id, model);

            // Пип группы и название.
            renderer.fillCircle(nameX + 3.0f, rowTop + kRowHeight * 0.5f, 3.0f, pipColor(entries[index].group, theme));
            renderer.text(id == MetricId::VramUsed ? L"Видеопамять занято / всего"
                    : id == MetricId::RamUsed      ? L"ОЗУ занято / всего"
                    : id == MetricId::PageFile     ? L"Файл подкачки занято / всего"
                    : id == MetricId::CpuLoad      ? L"CPU общий / пик ядра"
                                                   : wideText(info.label),
                Rect{ nameX + 14.0f, rowTop, valueRight - 120.0f,
                                                     rowTop + kRowHeight },
                TextRole::Small, known ? theme.textDim : theme.textFaint);

            // Значение крупнее остальных, единица — мелким шрифтом через пробел.
            // Недоступный датчик пишет причину словами: прочерк читался как сломанный интерфейс.
            const bool combined = id == MetricId::VramUsed || id == MetricId::RamUsed
                || id == MetricId::PageFile || id == MetricId::CpuLoad;
            const std::wstring value = known
                ? wideText(combined ? displayMetricValue(id, model.store)
                                    : formatValue(slot.stats.current, info.decimals))
                                             : L"нет данных";
            const std::wstring unit = known && !combined ? wideText(unitSuffix(info.unit)) : std::wstring();
            const float unitWidth = unit.empty() ? 0.0f : renderer.measureText(unit, TextRole::Tiny) + 4.0f;
            // Значение — главное в строке: крупнее подписи, как в демо.
            renderer.text(value, Rect{ valueRight - 130.0f - unitWidth, rowTop, valueRight - unitWidth,
                                   rowTop + kRowHeight },
                known ? TextRole::Number : TextRole::Tiny, known ? theme.text : theme.textFaint, TextAlign::Right);
            if (!unit.empty()) {
                renderer.text(unit, Rect{ valueRight - unitWidth, rowTop, valueRight, rowTop + kRowHeight },
                    TextRole::Tiny, theme.textFaint, TextAlign::Right);
            }

            const std::array<double, 3> values{ slot.stats.minimum, slot.stats.average, slot.stats.maximum };
            const std::array<float, 3> rights{ minRight, avgRight, maxRight };
            for (std::size_t stat = 0; stat < values.size(); ++stat) {
                renderer.text(known ? wideText(formatValue(values[stat], info.decimals)) : L"—",
                    Rect{ rights[stat] - 80.0f, rowTop, rights[stat], rowTop + kRowHeight }, TextRole::Tiny,
                    theme.textFaint, TextAlign::Right);
            }

            const Rect spark{ sparkLeft, rowTop + 6.0f, sparkRight, rowTop + kRowHeight - 6.0f };
            std::array<float, kHistoryCapacity> history{};
            const std::size_t count = model.store.copyHistory(id, history.data(), history.size());
            if (known && count >= 2 && spark.width() > 16.0f) {
                const std::size_t visible = std::min(count, static_cast<std::size_t>(historySamples));
                float maximum = 0.01f;
                for (std::size_t sample = count - visible; sample < count; ++sample) {
                    maximum = std::max(maximum, history[sample]);
                }
                renderer.drawSeries(ChartSeries{ history.data() + (count - visible), visible,
                                        pipColor(entries[index].group, theme), true },
                    spark, 0.0f, maximum, 1.4f);
            }

            // «Глаз» есть у каждой строки, как в демо: серый — датчик не в оверлее,
            // зелёный — выбран именно этот показатель.
            const auto metricIndex = static_cast<std::size_t>(id);
            // Кликабельная область глаза — вся правая колонка строки: попасть в иконку
            // 26×30 мышью было почти невозможно.
            const Rect eyeHit{ rowRight - 40.0f, rowTop, rowRight - 2.0f, rowTop + kRowHeight };
            const Rect eye{ rowRight - 34.0f, rowTop + 4.0f, rowRight - 6.0f, rowTop + kRowHeight - 4.0f };
            bool pinned = model.settings.overlay.metrics[metricIndex];
            for (MetricId detail : kPairedMetricDetails) {
                if (displayMetric(detail) == id) {
                    pinned |= model.settings.overlay.metrics[static_cast<std::size_t>(detail)];
                }
            }
            const bool eyeHover = isHovered(model, eyeHit);
            // Как в демо: выключенный «глаз» — серый перечёркнутый контур, включённый — зелёный чип.
            const Color eyeColor = pinned ? theme.accent : (eyeHover ? theme.textDim : theme.textFaint);
            if (pinned || eyeHover) {
                renderer.fillRoundRect(eye, 8.0f, pinned ? theme.accentSoft : theme.panelSoft);
            }
            if (pinned) {
                renderer.strokeRoundRect(eye, 8.0f, theme.accentGlow, 1.0f);
            }
            // Настоящий «глаз»: веко-эллипс и зрачок; выключенный перечёркнут косой чертой.
            const float centerX = eye.centerX();
            const float centerY = eye.centerY();
            renderer.strokeEllipse(centerX, centerY, 7.0f, 4.4f, eyeColor, 1.3f);
            renderer.fillCircle(centerX, centerY, pinned ? 2.2f : 1.7f, eyeColor);
            if (!pinned) {
                renderer.drawLine(centerX - 6.6f, centerY + 4.4f, centerX + 6.6f, centerY - 4.4f, eyeColor, 1.3f);
            }

            // Строка попадает в список хитов раньше глаза: обработчик берёт последний
            // подходящий хит, поэтому раньше клик по глазу гасился пустым действием строки.
            hits.push_back(UiHit{ row, ActionKind::None, static_cast<int>(index) });
            hits.push_back(UiHit { eyeHit, ActionKind::ToggleSensor, static_cast<int>(id) });
        }

        top += kRowHeight;
    }
    renderer.popClip();

    // Полоса прокрутки: бегунок тянется мышью, клик по дорожке листает на страницу.
    model.sensorScrollMax = maxScroll;
    model.sensorViewportHeight = viewport.height();
    model.sensorScrollTrack = Rect{ inner.right - 9.0f, viewport.top, inner.right - 3.0f, viewport.bottom };
    if (contentHeight > viewport.height() && viewport.height() > 0.0f && maxScroll > 0.0f) {
        const float thumbHeight = std::max(24.0f, viewport.height() * (viewport.height() / contentHeight));
        const float thumbTop = viewport.top + (viewport.height() - thumbHeight) * (offset / maxScroll);
        const Rect scrollTrack = model.sensorScrollTrack;
        const Rect thumb{ scrollTrack.left, thumbTop, scrollTrack.right, thumbTop + thumbHeight };
        const bool hovered = isHovered(model, scrollTrack);
        renderer.fillRoundRect(scrollTrack, 3.0f, theme.line.withAlpha(hovered ? 0.55f : 0.3f));
        renderer.fillRoundRect(thumb, 3.0f, theme.textFaint.withAlpha(hovered ? 1.0f : 0.8f));
        hits.push_back(UiHit{ thumb, ActionKind::ScrollSensorsThumb, 0 });
        hits.push_back(UiHit{ scrollTrack, ActionKind::ScrollSensorsTrack, 0 });
    }

    renderer.text(L"ETW — кадры, PDH — счётчики диспетчера задач, NVML — NVIDIA, ACPI-термозона — процессор."
                  L" Драйверов не требуется; «нет данных» — прошивка не отдаёт этот датчик.",
        Rect{ inner.left, inner.bottom - 18.0f, inner.right - 14.0f, inner.bottom }, TextRole::Tiny,
        theme.textFaint);
}

} // namespace yawa

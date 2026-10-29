#include "metrics/overlay_metric.h"
#include "metrics/overlay_rows.h"
#include "metrics/sensor_status.h"
#include "metrics/smbios_memory.h"
#include "ui/canvas_transform.h"
#include "ui/rtss_exchange.h"
#include "ui/settings.h"

#include "test_support.h"
#include <algorithm>
#include <charconv>
#include <memory>

#include <string>
#include <string_view>
#include <vector>

namespace yawa::test {

namespace {

    void testIndependentOverlaySensors()
    {
        Settings settings;
        const auto before = settings.overlay.metrics;
        const auto clock = static_cast<std::size_t>(MetricId::CpuClock);
        settings.overlay.metrics[clock] = true;
        settings.overlay.metrics[static_cast<std::size_t>(MetricId::CpuTemp)] = false;
        CHECK(settings.overlay.metrics[static_cast<std::size_t>(MetricId::CpuLoad)]
            == before[static_cast<std::size_t>(MetricId::CpuLoad)]);
        CHECK(settings.overlay.metrics[static_cast<std::size_t>(MetricId::GpuTemp)]
            == before[static_cast<std::size_t>(MetricId::GpuTemp)]);
        Settings parsed;
        std::string error;
        CHECK(parseSettings(serializeSettings(settings), parsed, error));
        CHECK(parsed.overlay.metrics == settings.overlay.metrics);
        CHECK(parsed.overlay.metrics[clock]);
        CHECK(!parsed.overlay.metrics[static_cast<std::size_t>(MetricId::CpuTemp)]);
    }

    void testEverySelectedSensorReachesOverlay()
    {
        OverlaySettings settings;
        MetricStore store;
        settings.metrics.fill(true);
        for (const MetricInfo& metric : kMetricTable) {
            store.set(metric.id, 42.0);
        }
        auto rows = overlayMetricRows(settings, store);
        CHECK(rows.size() == kMetricCount - kPairedMetricDetails.size() - 1);
        for (const MetricInfo& metric : kMetricTable) {
            CHECK(std::count_if(rows.begin(), rows.end(), [&](const auto& row) {
                return row.id == displayMetric(metric.id);
            }) == 1);
        }
        settings.metrics[static_cast<std::size_t>(MetricId::CpuTemp)] = false;
        rows = overlayMetricRows(settings, store);
        CHECK(rows.size() == kMetricCount - kPairedMetricDetails.size() - 2);
        CHECK(std::none_of(
            rows.begin(), rows.end(), [](const auto& row) { return row.id == MetricId::CpuTemp; }));
        CHECK(std::any_of(
            rows.begin(), rows.end(), [](const auto& row) { return row.id == MetricId::CpuLoad; }));
        settings.metrics.fill(false);
        CHECK(overlayMetricRows(settings, store).empty());
        for (const MetricInfo& metric : kMetricTable) {
            settings.metrics[static_cast<std::size_t>(metric.id)] = true;
            rows = overlayMetricRows(settings, store);
            if (metric.id == MetricId::CpuPeakLoad) {
                CHECK(rows.empty()); // сохранённый старый ключ не рисуется
            } else {
                CHECK(rows.size() == 1);
                if (!rows.empty()) {
                    CHECK(rows[0].id == displayMetric(metric.id));
                    CHECK(!rows[0].value.empty());
                }
            }
            settings.metrics[static_cast<std::size_t>(metric.id)] = false;
        }
    }

    void testUnavailableAndZeroOverlaySensors()
    {
        OverlaySettings settings;
        settings.metrics.fill(false);
        settings.metrics[static_cast<std::size_t>(MetricId::PageFileLoad)] = true;
        settings.metrics[static_cast<std::size_t>(MetricId::DiskTemp)] = true;
        MetricStore store;
        store.set(MetricId::PageFileLoad, 0.0);
        const auto rows = overlayMetricRows(settings, store);
        CHECK(rows.size() == 2);
        CHECK(rows[0].value != "нет данных");
        CHECK(rows[1].value == "нет данных");
        settings.metrics.fill(false);
        settings.metrics[static_cast<std::size_t>(MetricId::CpuLoad)] = true;
        settings.labels[1].clear();
        CHECK(overlayMetricRows(settings, store).size() == 1);
        CHECK(overlayMetricRows(settings, store)[0].label.empty());
    }

    void testOverlayDecimalPoint()
    {
        OverlaySettings settings;
        settings.metrics.fill(false);
        settings.fpsDecimals = 1;
        MetricStore store;
        const auto select = [&](MetricId id, double value) {
            settings.metrics[static_cast<std::size_t>(id)] = true;
            store.set(id, value);
        };
        select(MetricId::Fps, 123.4);
        select(MetricId::FrameTime, 12.34);
        select(MetricId::NetDown, 2.44);
        select(MetricId::RamUsed, 4.5);
        select(MetricId::RamFree, 1.2);
        settings.customLabelSet[static_cast<std::size_t>(MetricId::FrameTime)] = true;
        settings.customLabels[static_cast<std::size_t>(MetricId::FrameTime)] = "Время, кадра";
        const auto rows = overlayMetricRows(settings, store);
        const auto hasRow = [&](MetricId id, std::string_view value, std::string_view label = { }) {
            return std::any_of(rows.begin(), rows.end(), [&](const auto& row) {
                return row.id == id && row.value == value && (label.empty() || row.label == label);
            });
        };
        CHECK(hasRow(MetricId::Fps, "123.4"));
        CHECK(hasRow(MetricId::FrameTime, "12.34 мс", "Время, кадра"));
        CHECK(hasRow(MetricId::NetDown, "2.4 Мбит/с"));
        CHECK(hasRow(MetricId::RamUsed, "4.5 / 5.7 ГБ"));
    }

    void testLegacyOverlaySelection()
    {
        Settings settings;
        std::string error;
        CHECK(parseSettings("[overlay]\nshow_cpu = 0\nshow_fps = 0\n", settings, error));
        CHECK(!settings.overlay.metrics[static_cast<std::size_t>(MetricId::CpuLoad)]);
        CHECK(!settings.overlay.metrics[static_cast<std::size_t>(MetricId::CpuTemp)]);
        CHECK(!settings.overlay.metrics[static_cast<std::size_t>(MetricId::Fps)]);
        CHECK(!settings.overlay.metrics[static_cast<std::size_t>(MetricId::Low1Fps)]);
        CHECK(settings.overlay.metrics[static_cast<std::size_t>(MetricId::GpuLoad)]);
        CHECK(parseSettings("[overlay_metrics]\ncpu_temp = 1\n[overlay]\nshow_cpu = 0\n", settings, error));
        CHECK(settings.overlay.metrics[static_cast<std::size_t>(MetricId::CpuTemp)]);
        CHECK(settings.overlay.metrics[static_cast<std::size_t>(MetricId::CpuLoad)]);
        settings.overlay.metrics.fill(false);
        Settings parsed;
        CHECK(parseSettings(serializeSettings(settings), parsed, error));
        CHECK(parsed.overlay.metrics == settings.overlay.metrics);
    }

    void testPairedMetricsAndOrder()
    {
        OverlaySettings settings;
        MetricStore store;
        settings.metrics.fill(false);
        store.set(MetricId::VramUsed, 2.0);
        store.set(MetricId::VramTotal, 8.0);
        store.set(MetricId::RamUsed, 6.0);
        store.set(MetricId::RamFree, 10.0);
        store.set(MetricId::PageFile, 3.0);
        store.set(MetricId::PageFileTotal, 12.0);
        store.set(MetricId::CpuLoad, 45.0);
        store.set(MetricId::CpuPeakLoad, 89.0);
        for (MetricId detail : kPairedMetricDetails) {
            settings.metrics[static_cast<std::size_t>(detail)] = true;
        }
        settings.metrics[static_cast<std::size_t>(MetricId::CpuLoad)] = true;
        settings.metrics[static_cast<std::size_t>(MetricId::CpuPeakLoad)] = true; // старые настройки
        auto rows = overlayMetricRows(settings, store);
        CHECK(rows.size() == 4);
        CHECK(rows[0].value.find("45") != std::string::npos);
        CHECK(rows[0].value.find("89") == std::string::npos);
        CHECK(std::none_of(
            rows.begin(), rows.end(), [](const auto& row) { return row.id == MetricId::CpuPeakLoad; }));
        const auto vram = std::find_if(
            rows.begin(), rows.end(), [](const auto& row) { return row.id == MetricId::VramUsed; });
        CHECK(vram != rows.end());
        if (vram != rows.end()) {
            CHECK(vram->value.find("2.0 / 8.0") != std::string::npos);
        }
        moveOverlayMetric(settings, MetricId::VramTotal, MetricId::CpuLoad);
        rows = overlayMetricRows(settings, store);
        CHECK(rows.front().id == MetricId::VramUsed);
        Settings saved;
        saved.overlay = settings;
        moveOverlayMetric(saved.overlay, MetricId::CpuLoad, MetricId::VramUsed);
        CHECK(!saved.overlay.pixelLayout);
        Settings reordered;
        std::string reorderError;
        CHECK(parseSettings(serializeSettings(saved), reordered, reorderError));
        CHECK(!reordered.overlay.pixelLayout);
        CHECK(overlayMetricRows(reordered.overlay, store)[0].id == MetricId::CpuLoad);
        enableOverlayPixelLayout(saved.overlay);
        saved.overlay.rowPositions[static_cast<std::size_t>(MetricId::VramUsed)] = { 30, 320, 17 };
        Settings parsed;
        std::string error;
        CHECK(parseSettings(serializeSettings(saved), parsed, error));
        CHECK(parsed.overlay.order == saved.overlay.order);
        CHECK(parsed.overlay.pixelLayout);
        CHECK(parsed.overlay.rowPositions[static_cast<std::size_t>(MetricId::VramUsed)].valueX == 320);
        CHECK(parsed.overlay.rowPositions[static_cast<std::size_t>(MetricId::VramUsed)].y == 17);
        CHECK(parseSettings("[overlay_order]\nrows = fps,fps\n", parsed, error));
        CHECK(parsed.overlay.order == defaultOverlayOrder());
        std::string extra = "[overlay_order]\nrows = ";
        for (const MetricInfo& metric : kMetricTable) {
            if (extra.back() != ' ') {
                extra += ',';
            }
            extra += metric.key;
        }
        extra += ",fps\n";
        CHECK(parseSettings(extra, parsed, error));
        CHECK(parsed.overlay.order == defaultOverlayOrder());
        extra.erase(extra.size() - 4); // после последней запятой — пустое поле
        extra += "\n";
        CHECK(parseSettings(extra, parsed, error));
        CHECK(parsed.overlay.order == defaultOverlayOrder());
        CHECK(parseSettings("[overlay_positions]\nvram_used = -50,999,17\n", parsed, error));
        CHECK(parsed.overlay.rowPositions[static_cast<std::size_t>(MetricId::VramUsed)].y == -1);
    }

    void testEditorMetricSettings()
    {
        MetricStore cpu;
        cpu.set(MetricId::CpuLoad, 40.0);
        cpu.set(MetricId::CpuPeakLoad, 53.0);
        CHECK(displayMetricValue(MetricId::CpuLoad, cpu).find("пик") == std::string::npos);
        Settings settings;
        settings.overlay.metrics.fill(false);
        settings.overlay.metrics[static_cast<std::size_t>(MetricId::VramUsed)] = true;
        CHECK(settings.overlay.metricColors[static_cast<std::size_t>(MetricId::VramUsed)] == -1);
        setOverlayMetricLabel(settings.overlay, MetricId::VramUsed, "Память ГП");
        settings.overlay.metricColors[static_cast<std::size_t>(MetricId::VramUsed)] = 3;
        MetricStore store;
        store.set(MetricId::VramUsed, 2.0);
        CHECK(overlayMetricRows(settings.overlay, store)[0].label == "Память ГП");
        Settings parsed;
        std::string error;
        CHECK(parseSettings(serializeSettings(settings), parsed, error));
        CHECK(parsed.overlay.metricColors[static_cast<std::size_t>(MetricId::VramUsed)] == 3);
        CHECK(overlayMetricRows(parsed.overlay, store)[0].label == "Память ГП");
        setOverlayMetricLabel(parsed.overlay, MetricId::VramUsed, "");
        CHECK(overlayMetricRows(parsed.overlay, store)[0].label.empty());
        CHECK(parseSettings(serializeSettings(parsed), settings, error));
        CHECK(settings.overlay.customLabelSet[static_cast<std::size_t>(MetricId::VramUsed)]);
        CHECK(overlayMetricRows(settings.overlay, store)[0].label.empty());
        settings.overlay.customLabelSet[static_cast<std::size_t>(MetricId::VramUsed)] = false;
        CHECK(overlayMetricRows(settings.overlay, store)[0].label == settings.overlay.labels[4]);
        CHECK(parseSettings("[overlay_metric_colors]\nvram_used = 1000\n", parsed, error));
        CHECK(parsed.overlay.metricColors[static_cast<std::size_t>(MetricId::VramUsed)] == 5);
        std::uint32_t rgba = 0;
        CHECK(parseOverlayRgba("#A03F00C0", rgba));
        CHECK(rgba == 0xA03F00C0u);
        CHECK(overlayRgbaHex(rgba) == "#A03F00C0");
        CHECK(!parseOverlayRgba("#FFFFFGFF", rgba));
        CHECK(!parseOverlayRgba("#112233", rgba));
        settings.overlay.valueRgba = 0xFFFFFFFFu;
        settings.overlay.labelRgba = 0x11223300u;
        settings.overlay.accentRgba = 0xABCDEF80u;
        settings.overlay.rowRgba[1] = 0x10102040u;
        settings.overlay.metricRgba[static_cast<std::size_t>(MetricId::VramUsed)] = 0xA03F00C0u;
        CHECK(parseSettings(serializeSettings(settings), parsed, error));
        CHECK(parsed.overlay.valueRgba == settings.overlay.valueRgba);
        CHECK(parsed.overlay.labelRgba == settings.overlay.labelRgba);
        CHECK(parsed.overlay.accentRgba == settings.overlay.accentRgba);
        CHECK(parsed.overlay.rowRgba == settings.overlay.rowRgba);
        CHECK(parsed.overlay.metricRgba == settings.overlay.metricRgba);
        CHECK(parseSettings("[overlay_metric_rgba]\nvram_used = #BADHEXFF\n", parsed, error));
        CHECK(!parsed.overlay.metricRgba[static_cast<std::size_t>(MetricId::VramUsed)]);
        applyOverlayRecipe(settings.overlay, OverlayRecipe::Hdr);
        CHECK(!settings.overlay.valueRgba
            && !settings.overlay.metricRgba[static_cast<std::size_t>(MetricId::VramUsed)]);
    }

    void testSmbiosDecodesRealType17Offsets()
    {
        std::vector<std::uint8_t> table(8);
        table[1] = 3;
        table[2] = 5;
        const auto append
            = [&table](const std::vector<std::uint8_t>& record, const std::string& strings = { }) {
                  table.insert(table.end(), record.begin(), record.end());
                  table.insert(table.end(), strings.begin(), strings.end());
                  table.push_back(0);
                  table.push_back(0);
              };
        std::vector<std::uint8_t> array(0x0F);
        array[0] = 16;
        array[1] = 0x0F;
        array[2] = 0x34;
        array[3] = 0x12;
        array[5] = 3; // System Memory
        array[0x0D] = 4;
        append(array);

        std::vector<std::uint8_t> dimm(0x28);
        dimm[0] = 17;
        dimm[1] = 0x28;
        dimm[4] = 0x34;
        dimm[5] = 0x12;
        dimm[0x0C] = 0;
        dimm[0x0D] = 0x40; // 16 384 МиБ
        dimm[0x10] = 1; // Device Locator
        dimm[0x12] = 0x22; // DDR5
        dimm[0x15] = 0xE0;
        dimm[0x16] = 0x15; // 5600 MT/s rated
        dimm[0x17] = 2;
        dimm[0x1A] = 3;
        dimm[0x20] = 0x50;
        dimm[0x21] = 0x14; // 5200 MT/s configured
        dimm[0x26] = 0x4C;
        dimm[0x27] = 0x04; // 1.1 V
        const auto module = [&append, &dimm](const char* slot) {
            std::string strings(slot);
            strings.push_back('\0');
            strings += "Vendor";
            strings.push_back('\0');
            strings += "Part";
            append(dimm, strings);
        };
        module("DIMM_A1");
        module("DIMM_B1");
        const auto length = static_cast<std::uint32_t>(table.size() - 8);
        for (int byte = 0; byte < 4; ++byte) {
            table[4 + byte] = static_cast<std::uint8_t>(length >> (byte * 8));
        }
        const MemoryIdentity memory = parseSmbiosMemory(table, 31.5);
        CHECK(memory.slots == 4);
        CHECK(memory.modules.size() == 2);
        CHECK(memory.modules[0].sizeGb == 16.0);
        CHECK(memory.modules[0].slot == "DIMM_A1");
        CHECK(memory.modules[1].slot == "DIMM_B1");
        CHECK(memory.modules[0].vendor == "Vendor");
        CHECK(memory.modules[0].part == "Part");
        CHECK(memory.modules[0].type == "DDR5");
        CHECK(memory.modules[0].ratedMt == 5600.0);
        CHECK(memory.modules[0].configuredMt == 5200.0);
        CHECK_NEAR(memory.modules[0].voltage, 1.1, 0.001);
        CHECK(configuredMemorySpeed(memory.modules) == 5200.0);

        std::vector<MemoryModule> conflicting = memory.modules;
        conflicting[1].configuredMt = 4800.0;
        CHECK(configuredMemorySpeed(conflicting) == 0.0);
        conflicting[1].configuredMt = 0.0;
        CHECK(configuredMemorySpeed(conflicting) == 0.0);
        table[4] = 0xFF; // заголовок обещает больше данных, чем помещается в буфере
        CHECK(parseSmbiosMemory(table, 31.5).modules.empty());
    }

    void testSmbiosModuleSizes()
    {
        CHECK(smbiosModuleSizeGb(0x7fff, 32768) == 32.0);
        CHECK(smbiosModuleSizeGb(0x7fff, 0) == 0.0);
        CHECK(smbiosModuleSizeGb(0x7fff, 1024) == 1.0);
        CHECK(smbiosModuleSizeGb(0x8200, 0) == 0.0);
        CHECK(smbiosModuleSizeGb(0xffff, 0) == 0.0);
        CHECK(smbiosModuleSizeGb(0, 0) == 0.0);
        CHECK(smbiosModuleSizeGb(0x8001, 0) == 0.0);
        CHECK(smbiosSizesPlausible(32.0, 31.5));
        CHECK(!smbiosSizesPlausible(2.0, 16.0));
        CHECK(!smbiosSizesPlausible(64.0, 16.0));
    }

    void testDefaultsRoundTrip()
    {
        const Settings settings = defaultSettings();
        std::string error;
        Settings parsed;
        CHECK(parseSettings(serializeSettings(settings), parsed, error));
        CHECK(error.empty());

        CHECK(parsed.capture.intervalMs == 1000);
        CHECK(parsed.capture.targetHz == 60);
        CHECK(parsed.overlay.style == OverlayStyle::Frosted);
        CHECK(parsed.overlay.position == OverlayPosition::TopLeft);
        CHECK(parsed.overlay.anchorMode == OverlayAnchorMode::Corner);
        CHECK(labelValue(parsed.overlay, LabelField::Gpu) == "GPU");
        // Сворачивание в трей по умолчанию выключено: закрытие просто закрывает окно.
        CHECK(!parsed.ui.closeToTray);
        CHECK(!defaultSettings().ui.closeToTray);
    }

    void testUnknownSensorFailureStaysUnknown()
    {
        CHECK(cpuSensorFailure(false, CpuDeviceFailure::Other) == SensorFailure::Unsupported);
        CHECK(cpuSensorFailure(true, CpuDeviceFailure::Missing) == SensorFailure::NeedsAccess);
        CHECK(cpuSensorFailure(true, CpuDeviceFailure::AccessDenied) == SensorFailure::NeedsAccess);
        CHECK(cpuSensorFailure(true, CpuDeviceFailure::Other) == SensorFailure::Unknown);
        CHECK(
            sensorMissingReason(MetricId::CpuTemp, SensorFailure::NeedsAccess) == SensorFailure::NeedsAccess);
        CHECK(sensorMissingReason(MetricId::CpuPower, SensorFailure::Unsupported)
            == SensorFailure::Unsupported);
        CHECK(sensorMissingReason(MetricId::CpuTemp, SensorFailure::Unknown) == SensorFailure::Unknown);
        CHECK(sensorMissingReason(MetricId::GpuTemp, SensorFailure::Unsupported) == SensorFailure::Unknown);
        CHECK(sensorMissingReason(MetricId::DiskTemp, SensorFailure::NeedsAccess) == SensorFailure::Unknown);
    }

    void testCanvasCoordinatesSurviveZoom()
    {
        const CanvasAxis horizontal { 20.0f, 35.0f, 0.5f };
        CHECK(horizontal.toCanvas(1920.0f) == 1015.0f);
        CHECK(horizontal.toScreen(1015.0f) == 1920.0f);
        const float focus = 210.0f;
        const float screenPosition = horizontal.toScreen(focus);
        const CanvasAxis zoomed { horizontal.origin, horizontal.panAtZoom(focus, 0.75f), 0.75f };
        CHECK(zoomed.toScreen(focus) == screenPosition);
        CHECK(zoomed.toCanvas(0.0f) == zoomed.origin + zoomed.pan);
        // Строка у нижнего края рабочего стола должна стать читаемой в центре холста,
        // при этом обратное преобразование обязано сохранить координаты экрана.
        const ScreenBox line { 100, 980, 550, 1020 };
        const CanvasFit fitted = fitCanvasContent(line, 356.0f, 200.0f, 1920.0f);
        CHECK(fitted.zoom > 3.0f && fitted.zoom < 6.0f);
        const CanvasAxis fittedX { 20.0f, fitted.panX, fitted.zoom * 356.0f / 1920.0f };
        const CanvasAxis fittedY { 320.0f, fitted.panY, fitted.zoom * 356.0f / 1920.0f };
        CHECK(std::abs(fittedX.toCanvas(325.0f) - 198.0f) < 0.01f);
        CHECK(std::abs(fittedY.toCanvas(1000.0f) - 420.0f) < 0.01f);
        CHECK(std::abs(fittedX.toScreen(fittedX.toCanvas(100.0f)) - 100.0f) < 0.01f);
        CHECK(fittedX.toCanvas(550.0f) <= 20.0f + 356.0f - 14.0f + 0.01f);
        const CanvasFit workspace = fitCanvasContent(line, 1115.0f, 580.0f, 1920.0f);
        const CanvasAxis workspaceX { 184.0f, workspace.panX, workspace.zoom * 1115.0f / 1920.0f };
        CHECK(workspaceX.toCanvas(550.0f) - workspaceX.toCanvas(100.0f) > 1000.0f);
        CHECK(std::abs(workspaceX.toScreen(workspaceX.toCanvas(325.0f)) - 325.0f) < 0.01f);
        CHECK(resizeRtssExtent(120, 0, 2.0f) == 120);
        CHECK(resizeRtssExtent(120, 18, 2.0f) == 129);
        CHECK(resizeRtssExtent(120, -400, 2.0f) == 16);
        CHECK(resizeRtssExtent(2048, 10, 1.0f) == 2048);
        CHECK(resizeRtssExtent(4096, 0, 1.0f) == 4096);
        CHECK(alignCanvasEdge(10.0f, 90.0f, 40.0f, 70.0f, 0) == -30.0f);
        CHECK(alignCanvasEdge(10.0f, 90.0f, 40.0f, 70.0f, 1) == -5.0f);
        CHECK(alignCanvasEdge(10.0f, 90.0f, 40.0f, 70.0f, 2) == 20.0f);
        CHECK(clampDraggedRow(150, 150, 20, 80) == 150);
        CHECK(clampDraggedRow(149, 150, 20, 80) == 150);
        CHECK(clampDraggedRow(152, 150, 20, 80) == 152);
        CHECK(clampDraggedRow(150, 150, -50, -10) == 150);
        CHECK(clampDraggedRow(200, 150, -50, -10) == 150);
        // RTSS со смещёнными абсолютными координатами: вписываем слои, не пустоту от (0,0).
        const CanvasFit rtss = fitCanvasContent({ 1420, 680, 1720, 880 }, 356.0f, 200.0f, 1920.0f);
        const CanvasAxis rtssX { 20.0f, rtss.panX, rtss.zoom * 356.0f / 1920.0f };
        CHECK(std::abs(rtssX.toCanvas(1570.0f) - 198.0f) < 0.01f);
        CHECK(fitCanvasContent({ 0, 0, 4096, 100 }, 356.0f, 200.0f, 1920.0f).zoom < 0.75f);
        CHECK(fitCanvasContent({ }, 356.0f, 200.0f, 1920.0f).zoom == 1.0f);
        // Виртуальный экран начинается на x=-1920: начало сохранённого монитора — x=1920.
        CHECK(originOnMonitor(2040, 1920) == 120);
        CHECK(originOnMonitor(110, 0) == 110);
        CHECK(originOnMonitor(10, 1920) == 0);
        const std::array<int, 5> guides { 0, 96, 960, 1824, 1920 };
        CHECK(snapGrid(13) == 16 && snapGrid(-13) == -16);
        CHECK(snapToGuides(98, 0, guides, 6).value == 96);
        CHECK(snapToGuides(185, 100, guides, 6).guide == -1);
        CHECK(snapToGuides(910, 100, guides, 6).guide == 960); // центр плашки
        CHECK(snapToGuides(910, 100, guides, 6).value == 910);
        CHECK(snapToGuides(120, 0, guides, 6).guide == -1);
        CHECK(snapToGuides(120, 0, guides, 6).value == 120);
        const std::vector<int> baselines { 12, 40, 68 };
        CHECK(snapToGuides(38, 0, baselines, 4).value == 40);
        CHECK(originFromMonitor(120, 0, -1920) == 2040);
        CHECK(originFromMonitor(120, -1920, -1920) == 120);
        CHECK(originFromMonitor(120, -1920, 0) == 0);
        ScreenBox target;
        const ScreenBox virtualScreen { -1920, 0, 1920, 1080 };
        CHECK(monitorOnCanvas(virtualScreen, ScreenBox { 0, 0, 1920, 1080 }, target));
        CHECK(target.left == 1920 && target.right == 3840);
        CHECK(target.top == 0 && target.bottom == 1080);
        CHECK(monitorOnCanvas(virtualScreen, ScreenBox { -1920, -100, 0, 1080 }, target));
        CHECK(target.left == 0 && target.top == 0 && target.right == 1920);
        const ScreenBox previous = target;
        CHECK(!monitorOnCanvas(virtualScreen, ScreenBox { 4000, 0, 5000, 1080 }, target));
        CHECK(target.left == previous.left && target.right == previous.right);
    }

    void testDraggedRowKeepsColumnsAndClamps()
    {
        const OverlayRowPosition initial { 12, 252, 40 };
        const OverlayRowPosition moved = moveOverlayRow(initial, 13, -7);
        CHECK(moved.labelX == 25 && moved.valueX == 265 && moved.y == 33);
        const OverlayRowPosition left = moveOverlayRow(initial, -999, -999);
        CHECK(left.labelX == 0 && left.valueX == 240 && left.y == 0);
        const OverlayRowPosition right = moveOverlayRow(initial, 999, 9999);
        CHECK(right.labelX == 230 && right.valueX == 470 && right.y == 1490);
        CHECK(moveOverlayRow({ 400, 100, 10 }, 999, 0).labelX == 470);
    }

    void testStyledRowOffsetKeepsPresetAndRoundTrips()
    {
        Settings settings;
        CHECK(!settings.overlay.pixelLayout);
        auto& offset = settings.overlay.styleRowOffsets[static_cast<std::size_t>(MetricId::CpuLoad)];
        offset = moveOverlayRowOffset(offset, -18, 42);
        CHECK(offset.x == -18 && offset.y == 42);
        CHECK(!settings.overlay.pixelLayout);
        Settings loaded;
        std::string error;
        CHECK(parseSettings(serializeSettings(settings), loaded, error));
        const auto& restored = loaded.overlay.styleRowOffsets[static_cast<std::size_t>(MetricId::CpuLoad)];
        CHECK(restored.x == -18 && restored.y == 42);
        CHECK(!loaded.overlay.pixelLayout);
        CHECK(moveOverlayRowOffset(restored, 5000, -5000).x == 2048);
        CHECK(moveOverlayRowOffset(restored, 5000, -5000).y == -2048);
        CHECK(parseSettings("[overlay_style_offsets]\ncpu_load = 3000,0\n", loaded, error));
        CHECK(loaded.overlay.styleRowOffsets[static_cast<std::size_t>(MetricId::CpuLoad)].x == 0);
    }

    void testOverlayAnchorMigration()
    {
        Settings settings;
        std::string error;
        CHECK(parseSettings("[overlay]\nposition = br\noffset_x = -43\noffset_y = 25\n", settings, error));
        CHECK(settings.overlay.anchorMode == OverlayAnchorMode::Corner);
        CHECK(settings.overlay.position == OverlayPosition::BottomRight);
        CHECK(settings.overlay.offsetX == -43);
        CHECK(settings.overlay.offsetY == 25);
        CHECK(!settings.overlay.followGameMonitor);
        CHECK(settings.overlay.savedMonitorId.empty());
        Settings migrated;
        CHECK(parseSettings(serializeSettings(settings), migrated, error));
        CHECK(migrated.overlay.anchorMode == OverlayAnchorMode::Corner);
        CHECK(migrated.overlay.position == OverlayPosition::BottomRight);
        CHECK(migrated.overlay.offsetX == -43);
        CHECK(migrated.overlay.offsetY == 25);

        CHECK(parseSettings(
            "[overlay]\nanchor_mode = free\norigin_x = 540\norigin_y = 230\n", settings, error));
        CHECK(settings.overlay.anchorMode == OverlayAnchorMode::Free);
        CHECK(settings.overlay.originX == 540);
        CHECK(settings.overlay.originY == 230);
        Settings roundTrip;
        CHECK(parseSettings(serializeSettings(settings), roundTrip, error));
        CHECK(roundTrip.overlay.anchorMode == OverlayAnchorMode::Free);
        CHECK(roundTrip.overlay.originX == 540);
        CHECK(roundTrip.overlay.originY == 230);
        CHECK(!roundTrip.overlay.followGameMonitor);
        CHECK(roundTrip.overlay.savedMonitorId.empty());
        settings.overlay.savedMonitorId = "interface:DISPLAY#ABC#{GUID}";
        settings.overlay.monitorOriginX = 120;
        settings.overlay.monitorOriginY = 40;
        CHECK(parseSettings(serializeSettings(settings), roundTrip, error));
        CHECK(roundTrip.overlay.savedMonitorId == settings.overlay.savedMonitorId);
        CHECK(roundTrip.overlay.monitorOriginX == 120 && roundTrip.overlay.monitorOriginY == 40);
        CHECK(parseSettings("[overlay]\nsaved_monitor_id = interface:DISPLAY#ABC\n"
                            "monitor_origin_x = -5\nmonitor_origin_y = 99999\n",
            roundTrip, error));
        CHECK(roundTrip.overlay.monitorOriginX == 0 && roundTrip.overlay.monitorOriginY == 32768);
        CHECK(parseSettings("[overlay]\nsaved_monitor_id = \n", roundTrip, error));
        CHECK(roundTrip.overlay.savedMonitorId.empty());
        CHECK(parseSettings("[overlay]\nfollow_game_monitor = 1\n", settings, error));
        CHECK(settings.overlay.followGameMonitor);
        CHECK(parseSettings(serializeSettings(settings), roundTrip, error));
        CHECK(roundTrip.overlay.followGameMonitor);
        CHECK(parseSettings("[overlay]\nfollow_game_monitor = no\n", roundTrip, error));
        CHECK(!roundTrip.overlay.followGameMonitor);
        CHECK(!roundTrip.overlay.showSparkline);
        CHECK(parseSettings("[overlay]\nfps_sparkline = 1\n", settings, error));
        CHECK(settings.overlay.showSparkline);
        CHECK(parseSettings(serializeSettings(settings), roundTrip, error));
        CHECK(roundTrip.overlay.showSparkline);
        CHECK(parseSettings("[overlay]\nfps_sparkline = 0\n", roundTrip, error));
        CHECK(!roundTrip.overlay.showSparkline);
        CHECK(roundTrip.overlay.sparklineMetric == MetricId::Fps);
        CHECK(roundTrip.overlay.menuOpacityPercent == 0);
        settings.overlay.menuOpacityPercent = 38;
        CHECK(parseSettings(serializeSettings(settings), roundTrip, error));
        CHECK(roundTrip.overlay.menuOpacityPercent == 38);
        CHECK(parseSettings("[overlay]\nmenu_opacity = 900\n", roundTrip, error));
        CHECK(roundTrip.overlay.menuOpacityPercent == 100);
        for (MetricId metric : { MetricId::CpuLoad, MetricId::GpuLoad, MetricId::FrameTime }) {
            settings.overlay.sparklineMetric = metric;
            CHECK(parseSettings(serializeSettings(settings), roundTrip, error));
            CHECK(roundTrip.overlay.showSparkline);
            CHECK(roundTrip.overlay.sparklineMetric == metric);
        }
        CHECK(parseSettings("[overlay]\nfps_sparkline = 1\nsparkline_metric = cpu_temp\n", roundTrip, error));
        CHECK(roundTrip.overlay.sparklineMetric == MetricId::Fps);
        CHECK(parseSettings(
            "[overlay]\nanchor_mode = other\norigin_x = -20\norigin_y = 99999\n", roundTrip, error));
        CHECK(roundTrip.overlay.anchorMode == OverlayAnchorMode::Corner);
        CHECK(roundTrip.overlay.originX == 0);
        CHECK(roundTrip.overlay.originY == 32768);
    }

    void testOverlayCustomization()
    {
        const std::string text = "[overlay]\n"
                                 "accent_color = 3\n"
                                 "row_color_fps = 1\n"
                                 "row_color_gpu = 5\n"
                                 "border_width = 2\n"
                                 "shadow_strength = 40\n"
                                 "letter_spacing = 150\n"
                                 "uppercase = 1\n"
                                 "separators = 0\n"
                                 "fps_decimals = 2\n"
                                 "background_width = 180\n"
                                 "background_height = 90\n"
                                 "[overlay_metric_text_scale]\n"
                                 "cpu_load = 150\n";

        Settings settings;
        std::string error;
        CHECK(parseSettings(text, settings, error));
        CHECK(settings.overlay.accentColor == 3);
        CHECK(settings.overlay.rowColors[0] == 1);
        CHECK(settings.overlay.rowColors[2] == 5);
        // Незаданные строки остаются «как в стиле».
        CHECK(settings.overlay.rowColors[1] == -1);
        CHECK(settings.overlay.borderWidth == 2);
        CHECK(settings.overlay.shadowStrength == 40);
        CHECK(settings.overlay.letterSpacing == 150);
        CHECK(settings.overlay.uppercaseMode == 1);
        CHECK(settings.overlay.separatorsMode == 0);
        CHECK(settings.overlay.fpsDecimals == 2);
        CHECK(settings.overlay.backgroundWidthPx == 180);
        CHECK(settings.overlay.backgroundHeightPx == 90);
        CHECK(settings.overlay.metricTextScalePercent[static_cast<std::size_t>(MetricId::CpuLoad)] == 150);
        CHECK(settings.overlay.metricTextScalePercent[static_cast<std::size_t>(MetricId::GpuLoad)] == 100);

        Settings copy;
        CHECK(parseSettings(serializeSettings(settings), copy, error));
        CHECK(copy.overlay.accentColor == 3);
        CHECK(copy.overlay.rowColors[0] == 1);
        CHECK(copy.overlay.rowColors[2] == 5);
        CHECK(copy.overlay.borderWidth == 2);
        CHECK(copy.overlay.fpsDecimals == 2);
        CHECK(copy.overlay.backgroundWidthPx == 180);
        CHECK(copy.overlay.backgroundHeightPx == 90);
        CHECK(copy.overlay.metricTextScalePercent[static_cast<std::size_t>(MetricId::CpuLoad)] == 150);

        // Смена пресета возвращает ручные подстройки к значениям стиля.
        applyStyleDefaults(copy.overlay, OverlayStyle::Rtss);
        CHECK(copy.overlay.accentColor == -1);
        CHECK(copy.overlay.rowColors[0] == -1);
        CHECK(copy.overlay.borderWidth == -1);
        CHECK(copy.overlay.uppercaseMode == -1);
    }

    void testOverlayPreviewScale()
    {
        // Обратное преобразование: низ предпросмотра соответствует низу виртуального экрана.
        CHECK(scaledOverlayOffset(1080, 1080.0, 216.0) == 216);
        CHECK(scaledOverlayOffset(-216, 216.0, 1080.0) == -1080);
        CHECK(scaledOverlayOffset(81, 360.0, 1920.0) == 432);
        CHECK(scaledOverlayOffset(10000, 1.0, 1000.0) == 8192);
        CHECK(scaledOverlayOffset(32, 0.0, 200.0) == 32);
    }

    void testOverlayCustomizationClamps()
    {
        Settings settings;
        std::string error;
        CHECK(parseSettings(
            "[overlay]\nborder_width = 9\nshadow_strength = 500\nletter_spacing = -40\nfps_decimals = 7\n",
            settings, error));
        CHECK(settings.overlay.borderWidth == 3);
        CHECK(settings.overlay.shadowStrength == 100);
        CHECK(settings.overlay.letterSpacing == 0);
        CHECK(settings.overlay.fpsDecimals == 2);
        CHECK(parseSettings("[overlay]\nbackground_width = 79\nbackground_height = 5000\n"
                            "[overlay_metric_text_scale]\ncpu_load = 201\n",
            settings, error));
        CHECK(settings.overlay.backgroundWidthPx == -1);
        CHECK(settings.overlay.backgroundHeightPx == -1);
        CHECK(settings.overlay.metricTextScalePercent[static_cast<std::size_t>(MetricId::CpuLoad)] == 100);
    }

    void testParsesValues()
    {
        const std::string text = "[capture]\n"
                                 "interval_ms = 500\n"
                                 "target_hz = 144\n"
                                 "etw_enabled = 0\n"
                                 "\n"
                                 "[overlay]\n"
                                 "position = br\n"
                                 "layout = line\n"
                                 "style = terminal\n"
                                 "scale = 130\n"
                                 "opacity = 40\n"
                                 "\n"
                                 "[labels]\n"
                                 "cpu = ЦП\n"
                                 "gpu = ВИДЕО\n"
                                 "\n"
                                 "[gpu]\n"
                                 "luid_low = 4321\n"
                                 "luid_high = 0\n"
                                 "\n"
                                 "[ui]\n"
                                 "accent = violet\n"
                                 "density = compact\n";

        Settings settings;
        std::string error;
        CHECK(parseSettings(text, settings, error));
        CHECK(settings.capture.intervalMs == 500);
        CHECK(settings.capture.targetHz == 144);
        CHECK(!settings.capture.etwEnabled);
        CHECK(settings.overlay.position == OverlayPosition::BottomRight);
        CHECK(settings.overlay.layout == OverlayLayout::Line);
        CHECK(settings.overlay.style == OverlayStyle::Terminal);
        CHECK(settings.overlay.scalePercent == 130);
        CHECK(settings.overlay.opacityPercent == 40);
        CHECK(labelValue(settings.overlay, LabelField::Cpu) == "ЦП");
        CHECK(labelValue(settings.overlay, LabelField::Gpu) == "ВИДЕО");
        CHECK(settings.gpu.hasSelection);
        CHECK(settings.gpu.luidLow == 4321);
        CHECK(settings.ui.accent == Accent::Violet);
        CHECK(settings.ui.compactDensity);
    }

    void testClampsOutOfRange()
    {
        const std::string text = "[capture]\n"
                                 "interval_ms = 5\n"
                                 "[overlay]\n"
                                 "scale = 500\n"
                                 "opacity = -20\n";

        Settings settings;
        std::string error;
        CHECK(parseSettings(text, settings, error));
        CHECK(settings.capture.intervalMs == 250);
        CHECK(settings.overlay.scalePercent == 160);
        CHECK(settings.overlay.opacityPercent == 0);
    }

    void testUnknownAndBrokenLines()
    {
        Settings settings;
        std::string error;
        CHECK(parseSettings("[unknown]\nkey = value\n[overlay]\nstyle = neon\n", settings, error));
        CHECK(settings.overlay.style == OverlayStyle::Neon);

        Settings broken;
        CHECK(!parseSettings("[overlay\nstyle = neon\n", broken, error));
        CHECK(!error.empty());

        CHECK(!parseSettings("[overlay]\nstyle neon\n", broken, error));
        CHECK(error.find("строка 2") != std::string::npos);
    }

    void testLabelLengthIsLimited()
    {
        OverlaySettings overlay;
        setLabel(overlay, LabelField::Gpu, "ОЧЕНЬДЛИННАЯПОДПИСЬ");
        // Лимит — 10 символов, а не байт: иначе кириллица обрезалась бы вдвое раньше.
        CHECK(labelValue(overlay, LabelField::Gpu) == "ОЧЕНЬДЛИНН");

        setLabel(overlay, LabelField::Gpu, "КАДРЫ");
        CHECK(labelValue(overlay, LabelField::Gpu) == "КАДРЫ");

        // Пустая подпись допустима: пользователь стирает её, чтобы ввести новую.
        setLabel(overlay, LabelField::Cpu, "");
        CHECK(labelValue(overlay, LabelField::Cpu).empty());

        setLabel(overlay, LabelField::Ram, "   ");
        CHECK(labelValue(overlay, LabelField::Ram).empty());
    }

    void testKeysAreStable()
    {
        CHECK(overlayStyleKey(OverlayStyle::Mono) == "mono");
        CHECK(overlayStyleKey(OverlayStyle::Rtss) == "rtss");
        CHECK(overlayPositionKey(OverlayPosition::TopRight) == "tr");
        CHECK(overlayLayoutKey(OverlayLayout::Plates) == "plates");
        CHECK(accentKey(Accent::Coral) == "coral");

        OverlayStyle style = OverlayStyle::Frosted;
        // Старые ключи из файлов прошлых версий читаются и переводятся в новые пресеты.
        CHECK(parseOverlayStyle("capsule", style));
        CHECK(style == OverlayStyle::Esport);
        CHECK(parseOverlayStyle("glass", style));
        CHECK(style == OverlayStyle::Frosted);
        CHECK(parseOverlayStyle("minimal", style));
        CHECK(style == OverlayStyle::Mono);
        CHECK(parseOverlayStyle("retro", style));
        CHECK(style == OverlayStyle::Retro);
        CHECK(!parseOverlayStyle("nope", style));
    }

    void testRecipeMigrationAndAxes()
    {
        CHECK(overlayColorGroup(MetricId::Low1Fps) == std::size_t { 0 });
        CHECK(overlayColorGroup(MetricId::Core15Load) == std::size_t { 1 });
        CHECK(overlayColorGroup(MetricId::GpuEncode) == std::size_t { 2 });
        CHECK(overlayColorGroup(MetricId::RamUsed) == std::size_t { 3 });
        CHECK(overlayColorGroup(MetricId::VramUsed) == std::size_t { 4 });
        CHECK(!overlayColorGroup(MetricId::DiskRead));
        const auto& recipes = overlayRecipes();
        CHECK(recipes.size() == 16);
        for (std::size_t i = 0; i < recipes.size(); ++i) {
            OverlayRecipe parsed = OverlayRecipe::Frosted;
            CHECK(parseOverlayRecipe(recipes[i].key, parsed));
            CHECK(overlayRecipeKey(parsed) == recipes[i].key);
            CHECK(static_cast<std::size_t>(parsed) == i);
        }
        OverlayRecipe invalid = OverlayRecipe::Frosted;
        CHECK(!parseOverlayRecipe("missing", invalid));
        Settings legacy;
        std::string error;
        CHECK(parseSettings("[overlay]\nstyle = rtss\nlayout = line\nfont = 3\n"
                            "background = 1\nborder = 1\n",
            legacy, error));
        CHECK(legacy.overlay.recipe == OverlayRecipe::Rtss);
        CHECK(!legacy.overlay.surfaceActive);
        CHECK(legacy.overlay.layout == OverlayLayout::Line);
        CHECK(legacy.overlay.fontFamily == 3);
        CHECK(legacy.overlay.showBackground && legacy.overlay.showBorder);
        Settings roundTrip;
        CHECK(parseSettings(serializeSettings(legacy), roundTrip, error));
        CHECK(roundTrip.overlay.recipe == legacy.overlay.recipe);
        CHECK(!roundTrip.overlay.surfaceActive);
        CHECK(roundTrip.overlay.layout == legacy.overlay.layout);
        CHECK(roundTrip.overlay.fontFamily == 3);
        CHECK(roundTrip.overlay.showBackground && roundTrip.overlay.showBorder);

        CHECK(parseSettings("[overlay]\nborder = 0\nrecipe = focus\nsurface = outline\n"
                            "layout = gauge\nfont = 1\n",
            roundTrip, error));
        CHECK(roundTrip.overlay.recipe == OverlayRecipe::Focus);
        CHECK(roundTrip.overlay.layout == OverlayLayout::Gauge);
        CHECK(roundTrip.overlay.surfaceActive && roundTrip.overlay.surface == OverlaySurface::Outline);
        CHECK(!roundTrip.overlay.showBackground && !roundTrip.overlay.showBorder);
        CHECK(roundTrip.overlay.fontFamily == 1);
        CHECK(parseSettings(serializeSettings(roundTrip), legacy, error));
        CHECK(legacy.overlay.recipe == OverlayRecipe::Focus);
        CHECK(legacy.overlay.layout == OverlayLayout::Gauge);
        CHECK(legacy.overlay.surface == OverlaySurface::Outline && !legacy.overlay.showBorder);
        legacy.overlay.surfaceActive = false;
        CHECK(parseSettings(serializeSettings(legacy), roundTrip, error));
        CHECK(!roundTrip.overlay.surfaceActive);
        CHECK(!roundTrip.overlay.showBackground && !roundTrip.overlay.showBorder);
        applyOverlayRecipe(legacy.overlay, OverlayRecipe::FpsLarge);
        CHECK(legacy.overlay.metrics[static_cast<std::size_t>(MetricId::Fps)]);
        CHECK(!legacy.overlay.metrics[static_cast<std::size_t>(MetricId::CpuLoad)]);
        CHECK(legacy.overlay.scalePercent == 130);
        CHECK(legacy.overlay.layout == OverlayLayout::Line);
        CHECK(legacy.overlay.surface == OverlaySurface::None);
        CHECK(!legacy.overlay.showBackground && !legacy.overlay.showBorder);
        CHECK(parseSettings(serializeSettings(legacy), roundTrip, error));
        CHECK(roundTrip.overlay.recipe == OverlayRecipe::FpsLarge);
        CHECK(!roundTrip.overlay.metrics[static_cast<std::size_t>(MetricId::CpuLoad)]);
        roundTrip.overlay.savedMonitorId = "PRIVATE_MONITOR";
        roundTrip.overlay.customLabels[0] = "PRIVATE_LABEL";
        roundTrip.overlay.metricRgba[0] = 0x12345678u;
        const std::string exported = serializeOverlayRecipeJson(roundTrip.overlay);
        CHECK(exported.find("\"schema\": 1") != std::string::npos);
        CHECK(exported.find("\"recipe\": \"fps_large\"") != std::string::npos);
        CHECK(exported.find("PRIVATE_MONITOR") == std::string::npos);
        CHECK(exported.find("PRIVATE_LABEL") == std::string::npos);
        CHECK(exported.find("12345678") == std::string::npos);
        CHECK(exported.find("\"colors\"") == std::string::npos);
        roundTrip.overlay.valueRgba = 0x10203040u;
        roundTrip.overlay.rowRgba[2] = 0xAABBCCDDu;
        const std::string colored = serializeOverlayRecipeJson(roundTrip.overlay);
        CHECK(colored.find("\"colors\": {") != std::string::npos);
        CHECK(colored.find("\"value\": \"#10203040\"") != std::string::npos);
        CHECK(colored.find("\"gpu\": \"#AABBCCDD\"") != std::string::npos);
        CHECK(colored.find("12345678") == std::string::npos);
        CHECK(colored.find("PRIVATE_LABEL") == std::string::npos);

        OverlaySettings imported;
        imported.metrics.fill(false);
        imported.metricRgba[0] = 0x12345678u;
        imported.metricColors[1] = 3;
        imported.customLabels[0] = "PRIVATE_LABEL";
        imported.savedMonitorId = "PRIVATE_MONITOR";
        imported.originX = 123;
        CHECK(importOverlayRecipeJson(colored, imported, error));
        CHECK(imported.recipe == OverlayRecipe::FpsLarge);
        CHECK(imported.style == roundTrip.overlay.style);
        CHECK(imported.layout == roundTrip.overlay.layout);
        CHECK(imported.scalePercent == roundTrip.overlay.scalePercent);
        CHECK(imported.valueRgba == 0x10203040u);
        CHECK(imported.rowRgba[2] == 0xAABBCCDDu);
        CHECK(imported.metricRgba[0] == 0x12345678u);
        CHECK(imported.metricColors[1] == 3);
        CHECK(!imported.metrics[static_cast<std::size_t>(MetricId::Fps)]);
        CHECK(imported.customLabels[0] == "PRIVATE_LABEL");
        CHECK(imported.savedMonitorId == "PRIVATE_MONITOR" && imported.originX == 123);
        CHECK(importOverlayRecipeJson(exported, imported, error));
        CHECK(!imported.valueRgba && !imported.rowRgba[2]);
        CHECK(imported.metricRgba[0] == 0x12345678u);

        const std::string recipe
            = "{\"schema\":1,\"recipe\":\"clean\",\"palette\":\"mono\","
              "\"shape\":\"gauge\",\"surface\":\"custom\",\"typeface\":\"mono\",\"scale\":100}";
        imported.showBackground = false;
        imported.showBorder = true;
        CHECK(importOverlayRecipeJson(recipe, imported, error));
        CHECK(!imported.surfaceActive && !imported.showBackground && imported.showBorder);
        const auto rejects = [&](std::string_view json) {
            Settings snapshot;
            snapshot.overlay = imported;
            const std::string before = serializeSettings(snapshot);
            CHECK(!importOverlayRecipeJson(json, imported, error));
            snapshot.overlay = imported;
            CHECK(serializeSettings(snapshot) == before);
            CHECK(!error.empty());
        };
        rejects("{}");
        rejects("{\"schema\":2,\"recipe\":\"clean\"}");
        rejects(recipe + " trailing");
        rejects("{\"schema\":1,\"recipe\":\"clean\",\"recipe\":\"neon\"}");
        rejects("{\"schema\":1,\"colors\":{\"fps\":\"#1234567Z\"}}");
        rejects("{\"schema\":1,\"colors\":{\"fps\":\"#12345678\",\"fps\":\"#FFFFFFFF\"}}");
        rejects("{\"schema\":1,\"savedMonitorId\":\"PRIVATE_MONITOR\"}");
        rejects("{\"schema\":1,\"colors\":{\"metric_fps\":\"#12345678\"}}");
        rejects(recipe.substr(0, recipe.size() - 1) + ",\"monitor\":\"PRIVATE_MONITOR\"}");
        rejects(recipe.substr(0, recipe.size() - 1)
            + ",\"colors\":{\"value\":\"#00000000\","
              "\"value\":\"#FFFFFFFF\"}}");
        rejects(std::string(16385, 'x'));
    }

    void testRtssImportRecognizesOnlyKnownSources()
    {
        OverlaySettings settings;
        settings.originX = 200;
        settings.savedMonitorId = "display:PRIMARY";
        const std::string before = "[Master]\r\nImplementation=2\r\n[General]\r\nSources=1\r\nLayers=3\r\n"
                                   "[Source0]\r\nName=GPU1 usage\r\nProvider=HAL\r\nID=GPU1 usage\r\n"
                                   "[Layer0]\r\nText=<FR>\r\n[Layer1]\r\nText=%GPU1 usage%\r\n"
                                   "[Layer2]\r\nText=%External source%\r\n";
        std::string error;
        CHECK(importRtssOverlay(before, settings, error));
        CHECK(settings.style == OverlayStyle::Rtss);
        CHECK(settings.metrics[static_cast<std::size_t>(MetricId::Fps)]);
        CHECK(settings.metrics[static_cast<std::size_t>(MetricId::GpuLoad)]);
        CHECK(!settings.metrics[static_cast<std::size_t>(MetricId::CpuLoad)]);
        CHECK(settings.savedMonitorId == "display:PRIMARY" && settings.originX == 200);
        CHECK((settings.rtssTemplate && *settings.rtssTemplate == before) && settings.rtssOmittedLayers == 1);
        Settings persisted;
        persisted.overlay = settings;
        Settings restored;
        CHECK(parseSettings(serializeSettings(persisted), restored, error));
        CHECK((restored.overlay.rtssTemplate && *restored.overlay.rtssTemplate == before)
            && restored.overlay.rtssOmittedLayers == 1);
        const OverlaySettings snapshot = settings;
        CHECK(!importRtssOverlay("[Master]\nImplementation=3\n[General]\nLayers=1\n"
                                 "[Layer0]\nText=<FR>\n",
            settings, error));
        CHECK(settings.metrics == snapshot.metrics && settings.style == snapshot.style);
        CHECK(!importRtssOverlay("[Master]\nImplementation=2\n[General]\nLayers=1\n"
                                 "[Layer7]\nText=<FR>\n",
            settings, error));
        CHECK(!importRtssOverlay(std::string(1048577, 'x'), settings, error));
    }

    void testRtssHypertextDoesNotExpandPreview()
    {
        const std::string original = "<C=FFFFFF><S=180>CPU: %CPU temperature% °C\\n"
                                     "<C=FF0000><A=1><FR> FPS";
        const std::string visible = rtssVisibleTemplate(original);
        CHECK(visible == "CPU: %CPU temperature% °C\n<FR> FPS");
        CHECK(rtssVisibleTemplate("Первая\\nВторая") == "Первая\nВторая");
        CHECK(rtssVisibleTemplate("<G=Frametime,0,0,1,33,0,80>") == "<G=Frametime,0,0,1,33,0,80>");
        CHECK(rtssVisibleTemplate("<I=0,0,0,0,225,544>") == "<I=0,0,0,0,225,544>");
        CHECK(original.find("<C=") == 0); // исходные слои при экспорте не переписываются.
    }

    void testRtssVisualLayersPersistWithoutExternalProvider()
    {
        const std::string source = "[Master]\nImplementation=2\nFontFace=Unispace\nFontHeight=-7\n"
                                   "FontWeight=700\nZoomRatio=2\n[General]\nLayers=3\nSources=1\n"
                                   "[Source0]\nName=GPU1 usage\nProvider=Other\nID=GPU1 usage\n"
                                   "[Layer0]\nText=GPU\nPositionX=-2\nPositionY=-4\n"
                                   "ExtentX=-16\nExtentY=-6\nBgndColor=1000000,,0,80000000,0,(Framerate,1)\n"
                                   "[Layer1]\nText=%GPU1 usage%\nPositionX=-12\nTextColor=FFFF8000\n"
                                   "[Layer2]\nText=<FR> FPS\\nREADY\nExtentOrigin=2\nSize=200\n";
        OverlaySettings imported;
        std::string error;
        CHECK(importRtssOverlay(source, imported, error));
        CHECK(imported.rtssVisual && imported.rtssVisual->layers.size() == 3);
        CHECK(imported.rtssVisual->fontFace == "Unispace" && imported.rtssVisual->zoom == 2);
        CHECK(imported.rtssVisual->layers[0].x == -2 && imported.rtssVisual->layers[0].y == -4);
        CHECK(imported.rtssVisual->layers[0].backgroundColor == 0x80000000u);
        CHECK(imported.rtssVisual->layers[1].textColor == 0xFFFF8000u);
        CHECK(imported.rtssVisual->layers[2].size == 200 && imported.rtssVisual->layers[2].origin == 2);
        CHECK(rtssVisibleTemplate(imported.rtssVisual->layers[2].text) == "<FR> FPS\nREADY");
        CHECK(imported.rtssVisual->availableTags.size() == 1
            && imported.rtssVisual->availableTags[0].first == "<FR>");
        Settings saved;
        saved.overlay = imported;
        saved.overlay.rtssLayerEdits[1].x = 23;
        saved.overlay.rtssLayerEdits[1].y = -7;
        saved.overlay.rtssLayerEdits[1].visible = false;
        saved.overlay.rtssLayerEdits[1].text = "GPU <FR> / тест";
        saved.overlay.rtssLayerEdits[1].textColor = 0x80773322u;
        saved.overlay.rtssLayerEdits[0].backgroundColor = 0x00000000u;
        saved.overlay.rtssLayerEdits[1].backgroundColor = 0x804488CCu;
        saved.overlay.rtssLayerEdits[1].size = 175;
        saved.overlay.rtssLayerEdits[1].width = 320;
        saved.overlay.rtssLayerEdits[1].height = 48;
        saved.overlay.rtssLayerEdits[2].text = ""; // пустая строка — тоже намеренная правка.
        Settings restored;
        CHECK(parseSettings(serializeSettings(saved), restored, error));
        CHECK(restored.overlay.rtssLayerEdits[1].x == 23 && restored.overlay.rtssLayerEdits[1].y == -7
            && !restored.overlay.rtssLayerEdits[1].visible);
        CHECK(restored.overlay.rtssTemplate && *restored.overlay.rtssTemplate == source);
        CHECK(restored.overlay.rtssVisual && restored.overlay.rtssVisual->layers[0].x == -2);
        CHECK(restored.overlay.rtssVisual->availableTags.size() == 1);
        CHECK(restored.overlay.rtssLayerEdits[1].text == saved.overlay.rtssLayerEdits[1].text);
        CHECK(restored.overlay.rtssLayerEdits[1].textColor == 0x80773322u);
        CHECK(restored.overlay.rtssLayerEdits[0].backgroundColor == 0x00000000u);
        CHECK(restored.overlay.rtssLayerEdits[1].backgroundColor == 0x804488CCu);
        CHECK(restored.overlay.rtssLayerEdits[2].backgroundColor == std::nullopt);
        CHECK(restored.overlay.rtssLayerEdits[1].size == 175);
        CHECK(restored.overlay.rtssLayerEdits[1].width == 320
            && restored.overlay.rtssLayerEdits[1].height == 48);
        CHECK(restored.overlay.rtssLayerEdits[2].text && restored.overlay.rtssLayerEdits[2].text->empty());
        const std::string graph10 = "[Master]\nImplementation=2\n[General]\nLayers=2\nSources=1\n"
                                    "[Source0]\nName=CPU usage\nProvider=HAL\nID=CPU usage\n"
                                    "[Layer0]\nText=<G=Frametime,0,0,1,0,40,10>\n"
                                    "[Layer1]\nText=<G=CPU usage,0,0,1,0,100,10>\n";
        CHECK(importRtssOverlay(graph10, imported, error));
        CHECK(imported.rtssOmittedLayers == 0);
        CHECK(imported.rtssVisual && imported.rtssVisual->layers[1].text.starts_with("<G=CPU usage,"));
        const std::size_t provider = graph10.find("Provider=HAL");
        std::string externalGraph = graph10;
        externalGraph.replace(provider, 12, "Provider=Other");
        CHECK(importRtssOverlay(externalGraph, imported, error));
        CHECK(imported.rtssOmittedLayers == 1);
        Settings invalidEdits;
        CHECK(
            parseSettings("[rtss_layer_edits]\n1 = 2049,0,0\n2 = 0,0,3\n256 = 1,1,1\n", invalidEdits, error));
        CHECK(parseSettings("[rtss_layer_text]\n1 = XY\n256 = 4142\n"
                            "[rtss_layer_color]\n1 = GGGGGGGG\n"
                            "[rtss_layer_background]\n1 = FFFFFFGG\n256 = 00000000\n"
                            "[rtss_layer_size]\n1 = 401\n"
                            "[rtss_layer_extent]\n1 = 3000,40\n2 = 80,-4\n",
            invalidEdits, error));
        CHECK(invalidEdits.overlay.rtssLayerEdits[1].width == 0
            && invalidEdits.overlay.rtssLayerEdits[2].height == 0);
        CHECK(!invalidEdits.overlay.rtssLayerEdits[1].text
            && !invalidEdits.overlay.rtssLayerEdits[1].textColor
            && !invalidEdits.overlay.rtssLayerEdits[1].backgroundColor
            && invalidEdits.overlay.rtssLayerEdits[1].size == 0);
        CHECK(invalidEdits.overlay.rtssLayerEdits[1].visible && invalidEdits.overlay.rtssLayerEdits[1].x == 0
            && invalidEdits.overlay.rtssLayerEdits[2].visible);
        CHECK(importRtssOverlay(source, restored.overlay, error));
        CHECK(restored.overlay.rtssLayerEdits[1].visible && restored.overlay.rtssLayerEdits[1].x == 0
            && !restored.overlay.rtssLayerEdits[1].backgroundColor);
    }

    void testRtssGraphUsesLocalHistoryWithoutFabricatedSource()
    {
        const std::string source = "[Master]\nImplementation=2\n[General]\nLayers=1\n"
                                   "[Layer0]\nName=Frametime graph\nText=<G=Frametime,0,0,1,33,0,80>\n"
                                   "ExtentX=-32\nExtentY=-4\n";
        OverlaySettings imported;
        std::string error;
        CHECK(importRtssOverlay(source, imported, error));
        CHECK(imported.rtssVisual && imported.rtssVisual->layers.size() == 1);
        CHECK(imported.rtssVisual->layers[0].text == "<G=Frametime,0,0,1,33,0,80>");
        CHECK(imported.rtssOmittedLayers == 0);
        CHECK(!imported.metrics[static_cast<std::size_t>(MetricId::FrameTime)]);
        CHECK(imported.rtssTemplate && *imported.rtssTemplate == source);
    }

    void testRtssExternalProviderCannotImpersonateHal()
    {
        const std::string source = "[Master]\nImplementation=2\n[General]\nSources=2\nLayers=3\n"
                                   "[Source0]\nName=CPU temperature\nProvider=HwInfo\nID=CPU temperature\n"
                                   "[Source1]\nName=GPU1 usage\nProvider=HAL\nID=GPU1 usage\n"
                                   "[Layer0]\nText=%CPU temperature%\nPositionX=-5\n"
                                   "[Layer1]\nText=%GPU1 usage%\n"
                                   "[Layer2]\nImage=custom.png\n";
        OverlaySettings settings;
        std::string error;
        CHECK(importRtssOverlay(source, settings, error));
        CHECK(settings.metrics[static_cast<std::size_t>(MetricId::GpuLoad)]);
        CHECK(!settings.metrics[static_cast<std::size_t>(MetricId::CpuTemp)]);
        CHECK(settings.rtssOmittedLayers == 2 && (settings.rtssTemplate && *settings.rtssTemplate == source));
        settings.scalePercent = 140;
        Settings stored;
        stored.overlay = settings;
        Settings restored;
        CHECK(parseSettings(serializeSettings(stored), restored, error));
        CHECK((restored.overlay.rtssTemplate && *restored.overlay.rtssTemplate == source));
        CHECK(restored.overlay.scalePercent == 140);

        const std::string imageOnly = "[Master]\nImplementation=2\n[General]\nLayers=1\n"
                                      "[Layer0]\nImage=picture.png\n";
        CHECK(importRtssOverlay(imageOnly, settings, error));
        CHECK((settings.rtssTemplate && *settings.rtssTemplate == imageOnly)
            && settings.rtssOmittedLayers == 1);
        CHECK(std::none_of(
            settings.metrics.begin(), settings.metrics.end(), [](bool chosen) { return chosen; }));
        const std::string ambiguous = "[Master]\nImplementation=2\n[General]\nSources=2\nLayers=1\n"
                                      "[Source0]\nName=GPU1 usage\nProvider=HAL\nID=GPU1 usage\n"
                                      "[Source1]\nName=GPU1 usage\nProvider=AIDA\nID=GPU1 usage\n"
                                      "[Layer0]\nText=%GPU1 usage%\n";
        CHECK(importRtssOverlay(ambiguous, settings, error));
        CHECK(!settings.metrics[static_cast<std::size_t>(MetricId::GpuLoad)]);
        CHECK((settings.rtssTemplate && *settings.rtssTemplate == ambiguous)
            && settings.rtssOmittedLayers == 1);
        CHECK(!importRtssOverlay("[Master]\nImplementation=3\n[General]\nLayers=1\n"
                                 "[Layer0]\nText=<FR>\n",
            settings, error));
        CHECK((settings.rtssTemplate && *settings.rtssTemplate == ambiguous));
        const std::string embedded = "[Master]\nImplementation=2\n[Settings]\nEmbeddedImage="
            + std::string(70000, 'A') + "\n[General]\nLayers=1\n[Layer0]\nText=<FR>\n";
        CHECK(importRtssOverlay(embedded, settings, error));
        stored.overlay = settings;
        CHECK(parseSettings(serializeSettings(stored), restored, error));
        CHECK(restored.overlay.rtssTemplate && *restored.overlay.rtssTemplate == embedded);
        const std::string imageLayout = "[Master]\nImplementation=2\n[Settings]\n"
                                        "EmbeddedImage=atlas.png\n[General]\nLayers=1\n"
                                        "[Layer0]\nText=<I=0,0,0,0,2,2>\nExtentX=2\nExtentY=2\n";
        CHECK(importRtssOverlay(imageLayout, settings, error));
        CHECK(settings.rtssVisual && settings.rtssVisual->embeddedImage == "atlas.png");
        auto visual = std::make_shared<RtssVisualLayout>(*settings.rtssVisual);
        const std::string image = "\x89PNG\r\n\x1A\n" + std::string(24, 'x');
        visual->imageBytes = std::make_shared<const std::string>(image);
        settings.rtssVisual = std::move(visual);
        stored.overlay = settings;
        CHECK(parseSettings(serializeSettings(stored), restored, error));
        CHECK(restored.overlay.rtssVisual && restored.overlay.rtssVisual->imageBytes
            && *restored.overlay.rtssVisual->imageBytes == image);
        CHECK(restored.overlay.rtssTemplate && *restored.overlay.rtssTemplate == imageLayout);
        const std::string unsafe = "[Master]\nImplementation=2\n[Settings]\n"
                                   "EmbeddedImage=../atlas.png\n[General]\nLayers=1\n"
                                   "[Layer0]\nText=<I=0,0,0,0,2,2>\n";
        CHECK(importRtssOverlay(unsafe, settings, error));
        CHECK(settings.rtssVisual && settings.rtssVisual->embeddedImage.empty());
    }

    void testRtssExchangeKeepsOriginalAndRejectsDamage()
    {
        const std::string_view hex
            = "3043445592000000a7000000000000000000000030575a4c006104d132451200000004000000e00c000000b6000000"
              "604b93"
              "3073e89491d3a58182246de0b029d3a68c1b3a61e8a479e3a6878c060ab61c29e3a68c9c306cba3450c0244c9e3272"
              "e6f490"
              "d140c1162661f2949103a34b030554cae0a1d3838711293e1a28d8c2244c9e3272627469a0804a193c747ab07973e6"
              "4d0305"
              "65f0846903874d19176fecb001f0000000000600000090088a93230d14b07973e68d0b386ece000808";
        std::string package;
        for (std::size_t at = 0; at < hex.size(); at += 2) {
            unsigned int byte = 0;
            const auto parsed = std::from_chars(hex.data() + at, hex.data() + at + 2, byte, 16);
            CHECK(parsed.ec == std::errc { });
            package.push_back(static_cast<char>(byte));
        }
        OverlaySettings imported;
        imported.savedMonitorId = "MY_MONITOR";
        std::string error;
        CHECK(importRtssExchange(package, imported, error));
        RtssExchange unpacked;
        CHECK(unpackRtssExchange(package, unpacked, error));
        CHECK(unpacked.images.size() == 1 && unpacked.images[0].first == "logo.png"
            && unpacked.images[0].second == "\x89PNG\r\n");
        CHECK(imported.rtssTemplate && imported.rtssTemplate->find("Text=<FR>") != std::string::npos);
        CHECK(imported.rtssExchange && *imported.rtssExchange == package);
        CHECK(imported.rtssResources == 1 && imported.rtssOmittedLayers == 1);
        CHECK(imported.metrics[static_cast<std::size_t>(MetricId::Fps)]);
        CHECK(imported.savedMonitorId == "MY_MONITOR");
        Settings saved;
        saved.overlay = imported;
        Settings restored;
        CHECK(parseSettings(serializeSettings(saved), restored, error));
        CHECK(restored.overlay.rtssExchange && *restored.overlay.rtssExchange == package);
        CHECK(restored.overlay.rtssTemplate && *restored.overlay.rtssTemplate == *imported.rtssTemplate);
        CHECK(restored.overlay.rtssResources == 1);
        OverlaySettings snapshot = imported;
        package[0] = 'X';
        CHECK(!importRtssExchange(package, imported, error));
        CHECK(imported.rtssExchange == snapshot.rtssExchange);
        package[0] = '0';
        package.back() = static_cast<char>(static_cast<unsigned char>(package.back()) | 0x80u);
        CHECK(!importRtssExchange(package, imported, error));
        CHECK(imported.rtssExchange == snapshot.rtssExchange);
        package[4] = 0;
        CHECK(!importRtssExchange(package, imported, error));
        CHECK(imported.rtssExchange == snapshot.rtssExchange);
        CHECK(!importRtssExchange(package.substr(0, package.size() - 2), imported, error));
        CHECK(imported.rtssExchange == snapshot.rtssExchange);
    }

    void testRtssExportKeepsSupportedOrderAndColor()
    {
        OverlaySettings source;
        source.metrics.fill(false);
        source.metrics[static_cast<std::size_t>(MetricId::Fps)] = true;
        source.metrics[static_cast<std::size_t>(MetricId::CpuLoad)] = true;
        source.metrics[static_cast<std::size_t>(MetricId::GpuLoad)] = true;
        source.metrics[static_cast<std::size_t>(MetricId::CpuPeakLoad)] = true; // нет RTSS HAL-аналога
        source.metricRgba[static_cast<std::size_t>(MetricId::GpuLoad)] = 0xED1C24AAu;
        auto cpu = std::find(source.order.begin(), source.order.end(), MetricId::CpuLoad);
        auto gpu = std::find(source.order.begin(), source.order.end(), MetricId::GpuLoad);
        std::iter_swap(cpu, gpu);
        const std::string text = serializeRtssOverlay(source);
        CHECK(text.find("Implementation=2\r\n") != std::string::npos);
        CHECK(text.find("Sources=2\r\n") != std::string::npos);
        CHECK(text.find("Layers=3\r\n") != std::string::npos);
        CHECK(text.find("TextColor=AAED1C24\r\n") != std::string::npos);
        CHECK(text.find("Text=%GPU1 usage%") < text.find("Text=%CPU usage%"));
        CHECK(text.find("CPU usage max") == std::string::npos);
        OverlaySettings imported;
        std::string error;
        CHECK(importRtssOverlay(text, imported, error));
        CHECK(imported.metrics[static_cast<std::size_t>(MetricId::Fps)]);
        CHECK(imported.metrics[static_cast<std::size_t>(MetricId::GpuLoad)]);
        CHECK(imported.metrics[static_cast<std::size_t>(MetricId::CpuLoad)]);
        CHECK(imported.metricRgba[static_cast<std::size_t>(MetricId::GpuLoad)] == 0xED1C24AAu);
        CHECK(imported.order[0] == MetricId::Fps && imported.order[1] == MetricId::GpuLoad
            && imported.order[2] == MetricId::CpuLoad);
        CHECK(!imported.metrics[static_cast<std::size_t>(MetricId::CpuPeakLoad)]);
        source.metrics.fill(false);
        source.metrics[static_cast<std::size_t>(MetricId::CpuPeakLoad)] = true;
        CHECK(serializeRtssOverlay(source).empty());
    }

    void testGameProfileKeepsPersonalPlacement()
    {
        OverlaySettings base;
        base.savedMonitorId = "display:PRIMARY";
        base.originX = 200;
        base.followGameMonitor = true;
        base.clickThrough = true;
        base.scalePercent = 110;
        OverlaySettings profile;
        profile.scalePercent = 145;
        profile.metrics.fill(false);
        profile.metrics[static_cast<std::size_t>(MetricId::Fps)] = true;
        const OverlaySettings applied = overlayForGame(base, profile);
        CHECK(applied.scalePercent == 145);
        CHECK(applied.metrics[static_cast<std::size_t>(MetricId::Fps)]);
        CHECK(!applied.metrics[static_cast<std::size_t>(MetricId::GpuLoad)]);
        CHECK(applied.savedMonitorId == base.savedMonitorId);
        CHECK(applied.originX == 200 && applied.followGameMonitor && applied.clickThrough);
        Settings saved = defaultSettings();
        saved.overlay = applied;
        copyOverlayPlacement(saved.overlay, defaultSettings().overlay);
        CHECK(serializeSettings(saved).find("display:PRIMARY") == std::string::npos);
        CHECK(!saved.overlay.followGameMonitor && !saved.overlay.clickThrough);
        CHECK(gameProfileKey(L"Game.EXE") == "game.exe");
        CHECK(gameProfileKey(L"The Witcher 3.exe") == "the witcher 3.exe");
        CHECK(gameProfileKey(L"Игра.exe") == "%000418%000433%000440%000430.exe");
        CHECK(gameProfileKey(L"../game.exe").empty());
        CHECK(gameProfileKey(L"game:bad.exe").empty());
        CHECK(gameProfileKey(L"game.txt").empty());
    }

    void testCrlfFile()
    {
        Settings settings;
        std::string error;
        CHECK(parseSettings("[overlay]\r\nstyle = neon\r\n", settings, error));
        CHECK(settings.overlay.style == OverlayStyle::Neon);
    }

} // namespace

void runSettingsTests()
{
    testIndependentOverlaySensors();
    testEverySelectedSensorReachesOverlay();
    testUnavailableAndZeroOverlaySensors();
    testOverlayDecimalPoint();
    testLegacyOverlaySelection();
    testPairedMetricsAndOrder();
    testSmbiosModuleSizes();
    testSmbiosDecodesRealType17Offsets();
    testEditorMetricSettings();
    testOverlayPreviewScale();
    testDefaultsRoundTrip();
    testUnknownSensorFailureStaysUnknown();
    testCanvasCoordinatesSurviveZoom();
    testDraggedRowKeepsColumnsAndClamps();
    testStyledRowOffsetKeepsPresetAndRoundTrips();
    testOverlayAnchorMigration();
    testOverlayCustomization();
    testOverlayCustomizationClamps();
    testParsesValues();
    testClampsOutOfRange();
    testUnknownAndBrokenLines();
    testLabelLengthIsLimited();
    testKeysAreStable();
    testRecipeMigrationAndAxes();
    testRtssImportRecognizesOnlyKnownSources();
    testRtssHypertextDoesNotExpandPreview();
    testRtssVisualLayersPersistWithoutExternalProvider();
    testRtssGraphUsesLocalHistoryWithoutFabricatedSource();
    testRtssExternalProviderCannotImpersonateHal();
    testRtssExchangeKeepsOriginalAndRejectsDamage();
    testRtssExportKeepsSupportedOrderAndColor();
    testGameProfileKeepsPersonalPlacement();
    testCrlfFile();
}

} // namespace yawa::test

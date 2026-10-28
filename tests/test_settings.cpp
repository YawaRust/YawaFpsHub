#include "metrics/overlay_metric.h"
#include "metrics/overlay_rows.h"
#include "metrics/sensor_status.h"
#include "metrics/smbios_memory.h"
#include "ui/canvas_transform.h"
#include "ui/settings.h"

#include "test_support.h"
#include <algorithm>

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
                                 "fps_decimals = 2\n";

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

        Settings copy;
        CHECK(parseSettings(serializeSettings(settings), copy, error));
        CHECK(copy.overlay.accentColor == 3);
        CHECK(copy.overlay.rowColors[0] == 1);
        CHECK(copy.overlay.rowColors[2] == 5);
        CHECK(copy.overlay.borderWidth == 2);
        CHECK(copy.overlay.fpsDecimals == 2);

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
        const std::string before = "[Master]\nImplementation=2\n[General]\nLayers=3\n"
                                   "[Layer0]\nText=<FR>\n[Layer1]\nText=%GPU1 usage%\n"
                                   "[Layer2]\nText=%External source%\n";
        std::string error;
        CHECK(importRtssOverlay(before, settings, error));
        CHECK(settings.style == OverlayStyle::Rtss);
        CHECK(settings.metrics[static_cast<std::size_t>(MetricId::Fps)]);
        CHECK(settings.metrics[static_cast<std::size_t>(MetricId::GpuLoad)]);
        CHECK(!settings.metrics[static_cast<std::size_t>(MetricId::CpuLoad)]);
        CHECK(settings.savedMonitorId == "display:PRIMARY" && settings.originX == 200);
        const OverlaySettings snapshot = settings;
        CHECK(!importRtssOverlay("[Master]\nImplementation=3\n[General]\nLayers=1\n"
                                 "[Layer0]\nText=<FR>\n",
            settings, error));
        CHECK(settings.metrics == snapshot.metrics && settings.style == snapshot.style);
        CHECK(!importRtssOverlay("[Master]\nImplementation=2\n[General]\nLayers=1\n"
                                 "[Layer7]\nText=<FR>\n",
            settings, error));
        CHECK(!importRtssOverlay(std::string(65537, 'x'), settings, error));
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
    testRtssExportKeepsSupportedOrderAndColor();
    testGameProfileKeepsPersonalPlacement();
    testCrlfFile();
}

} // namespace yawa::test

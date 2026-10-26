#include "metrics/overlay_metric.h"
#include "metrics/overlay_rows.h"
#include "metrics/smbios_memory.h"
#include "ui/settings.h"

#include "test_support.h"
#include <algorithm>

#include <string>

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
        CHECK(rows.size() == kMetricCount - kPairedMetricDetails.size());
        for (const MetricInfo& metric : kMetricTable) {
            CHECK(std::count_if(rows.begin(), rows.end(), [&](const auto& row) {
                return row.id == displayMetric(metric.id);
            }) == 1);
        }
        settings.metrics[static_cast<std::size_t>(MetricId::CpuTemp)] = false;
        rows = overlayMetricRows(settings, store);
        CHECK(rows.size() == kMetricCount - kPairedMetricDetails.size() - 1);
        CHECK(std::none_of(
            rows.begin(), rows.end(), [](const auto& row) { return row.id == MetricId::CpuTemp; }));
        CHECK(std::any_of(
            rows.begin(), rows.end(), [](const auto& row) { return row.id == MetricId::CpuLoad; }));
        settings.metrics.fill(false);
        CHECK(overlayMetricRows(settings, store).empty());
        for (const MetricInfo& metric : kMetricTable) {
            settings.metrics[static_cast<std::size_t>(metric.id)] = true;
            rows = overlayMetricRows(settings, store);
            CHECK(rows.size() == 1);
            CHECK(rows[0].id == displayMetric(metric.id));
            CHECK(!rows[0].value.empty());
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
    auto rows = overlayMetricRows(settings, store);
    CHECK(rows.size() == 4);
    CHECK(rows[0].value.find("45") != std::string::npos);
    CHECK(rows[0].value.find("89") != std::string::npos);
    const auto vram = std::find_if(
        rows.begin(), rows.end(), [](const auto& row) { return row.id == MetricId::VramUsed; });
    CHECK(vram != rows.end());
    if (vram != rows.end()) {
        CHECK(vram->value.find("2,0 / 8,0") != std::string::npos);
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
        if (extra.back() != ' ') { extra += ','; }
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
    CHECK(labelValue(parsed.overlay, LabelField::Gpu) == "GPU");
    // Сворачивание в трей по умолчанию выключено: закрытие просто закрывает окно.
    CHECK(!parsed.ui.closeToTray);
    CHECK(!defaultSettings().ui.closeToTray);
}

void testOverlayCustomization()
{
    const std::string text =
        "[overlay]\n"
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

void testOverlayCustomizationClamps()
{
    Settings settings;
    std::string error;
    CHECK(parseSettings("[overlay]\nborder_width = 9\nshadow_strength = 500\nletter_spacing = -40\nfps_decimals = 7\n",
        settings, error));
    CHECK(settings.overlay.borderWidth == 3);
    CHECK(settings.overlay.shadowStrength == 100);
    CHECK(settings.overlay.letterSpacing == 0);
    CHECK(settings.overlay.fpsDecimals == 2);
}

void testParsesValues()
{
    const std::string text =
        "[capture]\n"
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
    const std::string text =
        "[capture]\n"
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
    testLegacyOverlaySelection();
    testPairedMetricsAndOrder();
    testSmbiosModuleSizes();
    testDefaultsRoundTrip();
    testOverlayCustomization();
    testOverlayCustomizationClamps();
    testParsesValues();
    testClampsOutOfRange();
    testUnknownAndBrokenLines();
    testLabelLengthIsLimited();
    testKeysAreStable();
    testCrlfFile();
}

} // namespace yawa::test

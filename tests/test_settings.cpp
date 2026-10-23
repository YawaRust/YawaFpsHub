#include "ui/settings.h"
#include "test_support.h"

#include <string>

namespace yawa::test {

namespace {

void testDefaultsRoundTrip()
{
    const Settings settings = defaultSettings();
    std::string error;
    Settings parsed;
    CHECK(parseSettings(serializeSettings(settings), parsed, error));
    CHECK(error.empty());

    CHECK(parsed.capture.intervalMs == 1000);
    CHECK(parsed.capture.targetHz == 120);
    CHECK(parsed.overlay.style == OverlayStyle::Glass);
    CHECK(parsed.overlay.position == OverlayPosition::TopLeft);
    CHECK(labelValue(parsed.overlay, LabelField::Gpu) == "GPU");
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
    CHECK(labelValue(overlay, LabelField::Gpu).size() <= 10);

    const std::string before = labelValue(overlay, LabelField::Cpu);
    setLabel(overlay, LabelField::Cpu, "   ");
    CHECK(labelValue(overlay, LabelField::Cpu) == before);
}

void testKeysAreStable()
{
    CHECK(overlayStyleKey(OverlayStyle::Minimal) == "minimal");
    CHECK(overlayPositionKey(OverlayPosition::TopRight) == "tr");
    CHECK(overlayLayoutKey(OverlayLayout::Plates) == "plates");
    CHECK(accentKey(Accent::Coral) == "coral");

    OverlayStyle style = OverlayStyle::Glass;
    CHECK(parseOverlayStyle("capsule", style));
    CHECK(style == OverlayStyle::Capsule);
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
    testDefaultsRoundTrip();
    testParsesValues();
    testClampsOutOfRange();
    testUnknownAndBrokenLines();
    testLabelLengthIsLimited();
    testKeysAreStable();
    testCrlfFile();
}

} // namespace yawa::test

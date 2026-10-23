#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace yawa {

enum class OverlayStyle : std::uint8_t { Glass, Neon, Capsule, Terminal, Minimal };
enum class OverlayPosition : std::uint8_t { TopLeft, TopRight, BottomLeft, BottomRight };
enum class OverlayLayout : std::uint8_t { Plates, Line };
enum class Accent : std::uint8_t { Mint, Ice, Violet, Amber, Coral };

enum class LabelField : std::uint8_t { Fps, Cpu, Gpu, Ram, Vram };
constexpr std::size_t kLabelCount = 5;

struct CaptureSettings {
    int intervalMs = 1000;
    int smoothingPercent = 40;
    bool etwEnabled = true;
    bool pauseWhenIdle = true;
    bool includeDiskNet = true;
    int targetHz = 120;
};

struct OverlaySettings {
    OverlayPosition position = OverlayPosition::TopLeft;
    OverlayLayout layout = OverlayLayout::Plates;
    OverlayStyle style = OverlayStyle::Glass;
    int scalePercent = 100;
    int opacityPercent = 72;
    std::array<std::string, kLabelCount> labels{ "FPS", "CPU", "GPU", "RAM", "VRAM" };
};

struct UiSettings {
    Accent accent = Accent::Mint;
    bool compactDensity = false;
    bool animate = true;
    bool closeToTray = true;
};

struct GpuSelection {
    bool hasSelection = false;
    std::uint32_t luidLow = 0;
    std::int32_t luidHigh = 0;
};

struct Settings {
    CaptureSettings capture;
    OverlaySettings overlay;
    UiSettings ui;
    GpuSelection gpu;
};

Settings defaultSettings();

// Формат — docs/settings.md. Неизвестные ключи пропускаются, отсутствующие остаются
// значениями по умолчанию: файл мог остаться от прошлой версии.
bool parseSettings(std::string_view text, Settings& target, std::string& error);
std::string serializeSettings(const Settings& settings);

std::string_view labelKey(LabelField field);
const std::string& labelValue(const OverlaySettings& overlay, LabelField field);
void setLabel(OverlaySettings& overlay, LabelField field, std::string value);

std::string_view overlayStyleKey(OverlayStyle style);
std::string_view overlayPositionKey(OverlayPosition position);
std::string_view overlayLayoutKey(OverlayLayout layout);
std::string_view accentKey(Accent accent);

bool parseOverlayStyle(std::string_view text, OverlayStyle& style);
bool parseOverlayPosition(std::string_view text, OverlayPosition& position);
bool parseOverlayLayout(std::string_view text, OverlayLayout& layout);
bool parseAccent(std::string_view text, Accent& accent);

} // namespace yawa

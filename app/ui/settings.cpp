#include "ui/settings.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <utility>

namespace yawa {
namespace {

constexpr int kMinIntervalMs = 250;
constexpr int kMaxIntervalMs = 5000;
constexpr int kMinScale = 70;
constexpr int kMaxScale = 160;
constexpr std::size_t kMaxLabelLength = 10;

struct StyleKey {
    OverlayStyle style;
    std::string_view key;
};

constexpr StyleKey kStyles[] = {
    { OverlayStyle::Neon, "neon" },
    { OverlayStyle::Rtss, "rtss" },
    { OverlayStyle::Frosted, "frosted" },
    { OverlayStyle::Terminal, "terminal" },
    { OverlayStyle::Cyber, "cyber" },
    { OverlayStyle::Mono, "mono" },
    { OverlayStyle::Esport, "esport" },
    { OverlayStyle::Retro, "retro" },
};

// Ключи прошлых версий остаются читаемыми: файл настроек лежит рядом с exe и не должен
// ломать выбор стиля после обновления.
constexpr StyleKey kLegacyStyles[] = {
    { OverlayStyle::Frosted, "glass" },
    { OverlayStyle::Mono, "minimal" },
    { OverlayStyle::Esport, "capsule" },
};

std::string_view trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

int parseInt(std::string_view text, int fallback)
{
    int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} ? value : fallback;
}

bool parseBool(std::string_view text, bool fallback)
{
    if (text == "1" || text == "true" || text == "on") {
        return true;
    }
    if (text == "0" || text == "false" || text == "off") {
        return false;
    }
    return fallback;
}

std::uint32_t parseUint32(std::string_view text, std::uint32_t fallback)
{
    std::uint32_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} ? value : fallback;
}

std::int32_t parseInt32(std::string_view text, std::int32_t fallback)
{
    std::int32_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} ? value : fallback;
}

std::string sanitizeLabel(std::string_view text)
{
    std::string result(trim(text));
    // Лимит считается по символам UTF-8, иначе кириллица обрезалась бы вдвое раньше:
    // «КАДРЫ» — это 10 байт, но всего 5 символов.
    std::size_t characters = 0;
    std::size_t cut = 0;
    while (cut < result.size() && characters < kMaxLabelLength) {
        const auto byte = static_cast<unsigned char>(result[cut]);
        cut += (byte & 0xC0) == 0x80 ? 1 : (byte >= 0xF0 ? 4 : (byte >= 0xE0 ? 3 : (byte >= 0xC0 ? 2 : 1)));
        ++characters;
    }
    result.resize(std::min(cut, result.size()));
    return result;
}

} // namespace

Settings defaultSettings()
{
    return Settings{};
}

std::string_view labelKey(LabelField field)
{
    switch (field) {
    case LabelField::Fps:
        return "fps";
    case LabelField::Cpu:
        return "cpu";
    case LabelField::Gpu:
        return "gpu";
    case LabelField::Ram:
        return "ram";
    case LabelField::Vram:
        return "vram";
    }
    return "fps";
}

const std::string& labelValue(const OverlaySettings& overlay, LabelField field)
{
    return overlay.labels[static_cast<std::size_t>(field)];
}

void setLabel(OverlaySettings& overlay, LabelField field, std::string value)
{
    // Пустая строка допустима: так пользователь стирает подпись целиком и вводит новую.
    overlay.labels[static_cast<std::size_t>(field)] = sanitizeLabel(value);
}

void applyStyleDefaults(OverlaySettings& overlay, OverlayStyle style)
{
    overlay.style = style;
    switch (style) {
    case OverlayStyle::Rtss:
        overlay.showBackground = false;
        overlay.showBorder = false;
        overlay.showShadow = false;
        break;
    case OverlayStyle::Mono:
        overlay.showBackground = true;
        overlay.showBorder = false;
        overlay.showShadow = false;
        break;
    case OverlayStyle::Neon:
        overlay.showBackground = true;
        overlay.showBorder = true;
        overlay.showShadow = true;
        break;
    default:
        overlay.showBackground = true;
        overlay.showBorder = true;
        overlay.showShadow = true;
        break;
    }
    overlay.cornerRadius = -1;
    // Ручные подстройки оформления относятся к конкретному стилю: при смене пресета
    // возвращаем их к значениям стиля, иначе «свой» цвет тянулся бы во все пресеты.
    overlay.backgroundOpacityPercent = -1;
    overlay.paddingPx = -1;
    overlay.rowGapPx = -1;
    overlay.valueColor = -1;
    overlay.labelColor = -1;
    overlay.accentColor = -1;
    overlay.rowColors = { -1, -1, -1, -1, -1 };
    overlay.borderWidth = -1;
    overlay.shadowStrength = 100;
    overlay.letterSpacing = 100;
    overlay.uppercaseMode = -1;
    overlay.separatorsMode = -1;
    overlay.fpsDecimals = -1;
    overlay.textScalePercent = 100;
}

std::string_view overlayStyleKey(OverlayStyle style)
{
    for (const StyleKey& item : kStyles) {
        if (item.style == style) {
            return item.key;
        }
    }
    return "glass";
}

std::string_view overlayPositionKey(OverlayPosition position)
{
    switch (position) {
    case OverlayPosition::TopLeft:
        return "tl";
    case OverlayPosition::TopRight:
        return "tr";
    case OverlayPosition::BottomLeft:
        return "bl";
    case OverlayPosition::BottomRight:
        return "br";
    }
    return "tl";
}

std::string_view overlayLayoutKey(OverlayLayout layout)
{
    return layout == OverlayLayout::Line ? "line" : "plates";
}

std::string_view accentKey(Accent accent)
{
    switch (accent) {
    case Accent::Mint:
        return "mint";
    case Accent::Ice:
        return "ice";
    case Accent::Violet:
        return "violet";
    case Accent::Amber:
        return "amber";
    case Accent::Coral:
        return "coral";
    }
    return "mint";
}

bool parseOverlayStyle(std::string_view text, OverlayStyle& style)
{
    for (const StyleKey& item : kStyles) {
        if (item.key == text) {
            style = item.style;
            return true;
        }
    }
    for (const StyleKey& item : kLegacyStyles) {
        if (item.key == text) {
            style = item.style;
            return true;
        }
    }
    return false;
}

bool parseOverlayPosition(std::string_view text, OverlayPosition& position)
{
    const std::pair<std::string_view, OverlayPosition> items[] = {
        { "tl", OverlayPosition::TopLeft },
        { "tr", OverlayPosition::TopRight },
        { "bl", OverlayPosition::BottomLeft },
        { "br", OverlayPosition::BottomRight },
    };

    for (const auto& [key, value] : items) {
        if (key == text) {
            position = value;
            return true;
        }
    }
    return false;
}

bool parseOverlayLayout(std::string_view text, OverlayLayout& layout)
{
    if (text == "line") {
        layout = OverlayLayout::Line;
        return true;
    }
    if (text == "plates") {
        layout = OverlayLayout::Plates;
        return true;
    }
    return false;
}

bool parseAccent(std::string_view text, Accent& accent)
{
    const std::pair<std::string_view, Accent> items[] = {
        { "mint", Accent::Mint },
        { "ice", Accent::Ice },
        { "violet", Accent::Violet },
        { "amber", Accent::Amber },
        { "coral", Accent::Coral },
    };

    for (const auto& [key, value] : items) {
        if (key == text) {
            accent = value;
            return true;
        }
    }
    return false;
}

bool parseSettings(std::string_view text, Settings& target, std::string& error)
{
    target = defaultSettings();
    error.clear();

    std::string section;
    std::size_t lineNumber = 0;

    while (!text.empty()) {
        const std::size_t lineEnd = text.find('\n');
        std::string_view line = lineEnd == std::string_view::npos ? text : text.substr(0, lineEnd);
        text = lineEnd == std::string_view::npos ? std::string_view{} : text.substr(lineEnd + 1);
        ++lineNumber;

        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        line = trim(line);
        if (line.empty() || line.front() == ';' || line.front() == '#') {
            continue;
        }

        if (line.front() == '[') {
            const std::size_t close = line.find(']');
            if (close == std::string_view::npos) {
                error = "строка " + std::to_string(lineNumber) + ": незакрытая секция";
                return false;
            }
            section = std::string(trim(line.substr(1, close - 1)));
            continue;
        }

        const std::size_t separator = line.find('=');
        if (separator == std::string_view::npos) {
            error = "строка " + std::to_string(lineNumber) + ": нет знака =";
            return false;
        }

        const std::string key(trim(line.substr(0, separator)));
        const std::string_view value = trim(line.substr(separator + 1));

        if (section == "capture") {
            if (key == "interval_ms") {
                target.capture.intervalMs = std::clamp(parseInt(value, target.capture.intervalMs),
                    kMinIntervalMs, kMaxIntervalMs);
            } else if (key == "smoothing") {
                target.capture.smoothingPercent = std::clamp(parseInt(value, target.capture.smoothingPercent), 0, 90);
            } else if (key == "etw_enabled") {
                target.capture.etwEnabled = parseBool(value, target.capture.etwEnabled);
            } else if (key == "pause_when_idle") {
                target.capture.pauseWhenIdle = parseBool(value, target.capture.pauseWhenIdle);
            } else if (key == "include_disk_net") {
                target.capture.includeDiskNet = parseBool(value, target.capture.includeDiskNet);
            } else if (key == "target_hz") {
                target.capture.targetHz = std::clamp(parseInt(value, target.capture.targetHz), 30, 480);
            } else if (key == "target_auto") {
                target.capture.targetAuto = parseBool(value, target.capture.targetAuto);
            }
        } else if (section == "overlay") {
            if (key == "position") {
                parseOverlayPosition(value, target.overlay.position);
            } else if (key == "layout") {
                parseOverlayLayout(value, target.overlay.layout);
            } else if (key == "style") {
                parseOverlayStyle(value, target.overlay.style);
            } else if (key == "scale") {
                target.overlay.scalePercent = std::clamp(parseInt(value, target.overlay.scalePercent),
                    kMinScale, kMaxScale);
            } else if (key == "opacity") {
                target.overlay.opacityPercent = std::clamp(parseInt(value, target.overlay.opacityPercent), 0, 100);
            } else if (key == "offset_x") {
                target.overlay.offsetX = std::clamp(parseInt(value, target.overlay.offsetX), -2000, 2000);
            } else if (key == "offset_y") {
                target.overlay.offsetY = std::clamp(parseInt(value, target.overlay.offsetY), -2000, 2000);
            } else if (key == "background") {
                target.overlay.showBackground = parseBool(value, target.overlay.showBackground);
            } else if (key == "border") {
                target.overlay.showBorder = parseBool(value, target.overlay.showBorder);
            } else if (key == "shadow") {
                target.overlay.showShadow = parseBool(value, target.overlay.showShadow);
            } else if (key == "font") {
                target.overlay.fontFamily = std::clamp(parseInt(value, target.overlay.fontFamily), 0, 3);
            } else if (key == "bold") {
                target.overlay.fontBold = parseInt(value, 0) != 0;
            } else if (key == "italic") {
                target.overlay.fontItalic = parseInt(value, 0) != 0;
            } else if (key == "text_outline") {
                target.overlay.textOutline = std::clamp(parseInt(value, target.overlay.textOutline), 0, 3);
            } else if (key == "corner") {
                target.overlay.cornerRadius = std::clamp(parseInt(value, target.overlay.cornerRadius), -1, 24);
            } else if (key == "text_scale") {
                target.overlay.textScalePercent = std::clamp(
                    parseInt(value, target.overlay.textScalePercent), 85, 130);
            } else if (key == "bg_opacity") {
                target.overlay.backgroundOpacityPercent = std::clamp(
                    parseInt(value, target.overlay.backgroundOpacityPercent), -1, 100);
            } else if (key == "padding") {
                target.overlay.paddingPx = std::clamp(parseInt(value, target.overlay.paddingPx), -1, 28);
            } else if (key == "row_gap") {
                target.overlay.rowGapPx = std::clamp(parseInt(value, target.overlay.rowGapPx), -1, 14);
            } else if (key == "value_color") {
                target.overlay.valueColor = std::clamp(parseInt(value, target.overlay.valueColor), -1, 5);
            } else if (key == "label_color") {
                target.overlay.labelColor = std::clamp(parseInt(value, target.overlay.labelColor), -1, 5);
            } else if (key == "accent_color") {
                target.overlay.accentColor = std::clamp(parseInt(value, target.overlay.accentColor), -1, 5);
            } else if (key == "border_width") {
                target.overlay.borderWidth = std::clamp(parseInt(value, target.overlay.borderWidth), -1, 3);
            } else if (key == "shadow_strength") {
                target.overlay.shadowStrength = std::clamp(parseInt(value, target.overlay.shadowStrength), 0, 100);
            } else if (key == "letter_spacing") {
                target.overlay.letterSpacing = std::clamp(parseInt(value, target.overlay.letterSpacing), 0, 200);
            } else if (key == "uppercase") {
                target.overlay.uppercaseMode = std::clamp(parseInt(value, target.overlay.uppercaseMode), -1, 1);
            } else if (key == "separators") {
                target.overlay.separatorsMode = std::clamp(parseInt(value, target.overlay.separatorsMode), -1, 1);
            } else if (key == "fps_decimals") {
                target.overlay.fpsDecimals = std::clamp(parseInt(value, target.overlay.fpsDecimals), -1, 2);
            } else if (key.size() > 10 && key.substr(0, 10) == "row_color_") {
                for (std::size_t index = 0; index < kLabelCount; ++index) {
                    const auto field = static_cast<LabelField>(index);
                    if (key.substr(10) == labelKey(field)) {
                        target.overlay.rowColors[index] = std::clamp(
                            parseInt(value, target.overlay.rowColors[index]), -1, 5);
                    }
                }
            } else if (key == "click_through") {
                target.overlay.clickThrough = parseBool(value, target.overlay.clickThrough);
            } else if (key.size() > 5 && key.substr(0, 5) == "show_") {
                for (std::size_t index = 0; index < kLabelCount; ++index) {
                    const auto field = static_cast<LabelField>(index);
                    if (key.substr(5) == labelKey(field)) {
                        target.overlay.visible[index] = parseBool(value, target.overlay.visible[index]);
                    }
                }
            }
        } else if (section == "hotkeys") {
            if (key == "overlay") {
                target.hotkeys.overlay = static_cast<unsigned int>(std::clamp(parseInt(value, 0), 0, 254));
            } else if (key == "position") {
                target.hotkeys.position = static_cast<unsigned int>(std::clamp(parseInt(value, 0), 0, 254));
            } else if (key == "window") {
                target.hotkeys.window = static_cast<unsigned int>(std::clamp(parseInt(value, 0), 0, 254));
            } else if (key == "click_through") {
                target.hotkeys.clickThrough = static_cast<unsigned int>(std::clamp(parseInt(value, 0), 0, 254));
            }
        } else if (section == "labels") {
            for (std::size_t index = 0; index < kLabelCount; ++index) {
                const auto field = static_cast<LabelField>(index);
                if (key == labelKey(field)) {
                    setLabel(target.overlay, field, std::string(value));
                }
            }
        } else if (section == "gpu") {
            if (key == "luid_low") {
                target.gpu.luidLow = parseUint32(value, target.gpu.luidLow);
                target.gpu.hasSelection = true;
            } else if (key == "luid_high") {
                target.gpu.luidHigh = parseInt32(value, target.gpu.luidHigh);
                target.gpu.hasSelection = true;
            }
        } else if (section == "ui") {
            if (key == "accent") {
                parseAccent(value, target.ui.accent);
            } else if (key == "density") {
                target.ui.compactDensity = value == "compact";
            } else if (key == "animate") {
                target.ui.animate = parseBool(value, target.ui.animate);
            } else if (key == "close_to_tray") {
                target.ui.closeToTray = parseBool(value, target.ui.closeToTray);
            }
        }
    }

    return true;
}

std::string serializeSettings(const Settings& settings)
{
    std::string text;
    text += "; YawaFpsHub — портативные настройки. Лежат рядом с exe, реестр не используется.\n\n";

    text += "[capture]\n";
    text += "interval_ms = " + std::to_string(settings.capture.intervalMs) + "\n";
    text += "smoothing = " + std::to_string(settings.capture.smoothingPercent) + "\n";
    text += std::string("etw_enabled = ") + (settings.capture.etwEnabled ? "1" : "0") + "\n";
    text += std::string("pause_when_idle = ") + (settings.capture.pauseWhenIdle ? "1" : "0") + "\n";
    text += std::string("include_disk_net = ") + (settings.capture.includeDiskNet ? "1" : "0") + "\n";
    text += "target_hz = " + std::to_string(settings.capture.targetHz) + "\n";
    text += std::string("target_auto = ") + (settings.capture.targetAuto ? "1" : "0") + "\n\n";

    text += "[gpu]\n";
    text += "luid_low = " + std::to_string(settings.gpu.luidLow) + "\n";
    text += "luid_high = " + std::to_string(settings.gpu.luidHigh) + "\n\n";

    text += "[overlay]\n";
    text += "position = " + std::string(overlayPositionKey(settings.overlay.position)) + "\n";
    text += "layout = " + std::string(overlayLayoutKey(settings.overlay.layout)) + "\n";
    text += "style = " + std::string(overlayStyleKey(settings.overlay.style)) + "\n";
    text += "scale = " + std::to_string(settings.overlay.scalePercent) + "\n";
    text += "opacity = " + std::to_string(settings.overlay.opacityPercent) + "\n";
    text += "offset_x = " + std::to_string(settings.overlay.offsetX) + "\n";
    text += "offset_y = " + std::to_string(settings.overlay.offsetY) + "\n";
    text += std::string("background = ") + (settings.overlay.showBackground ? "1" : "0") + "\n";
    text += std::string("border = ") + (settings.overlay.showBorder ? "1" : "0") + "\n";
    text += std::string("shadow = ") + (settings.overlay.showShadow ? "1" : "0") + "\n";
    text += "corner = " + std::to_string(settings.overlay.cornerRadius) + "\n";
    text += "font = " + std::to_string(settings.overlay.fontFamily) + "\n";
    text += std::string("bold = ") + (settings.overlay.fontBold ? "1" : "0") + "\n";
    text += std::string("italic = ") + (settings.overlay.fontItalic ? "1" : "0") + "\n";
    text += "text_outline = " + std::to_string(settings.overlay.textOutline) + "\n";
    text += "text_scale = " + std::to_string(settings.overlay.textScalePercent) + "\n";
    text += "bg_opacity = " + std::to_string(settings.overlay.backgroundOpacityPercent) + "\n";
    text += "padding = " + std::to_string(settings.overlay.paddingPx) + "\n";
    text += "row_gap = " + std::to_string(settings.overlay.rowGapPx) + "\n";
    text += "value_color = " + std::to_string(settings.overlay.valueColor) + "\n";
    text += "label_color = " + std::to_string(settings.overlay.labelColor) + "\n";
    text += "accent_color = " + std::to_string(settings.overlay.accentColor) + "\n";
    text += "border_width = " + std::to_string(settings.overlay.borderWidth) + "\n";
    text += "shadow_strength = " + std::to_string(settings.overlay.shadowStrength) + "\n";
    text += "letter_spacing = " + std::to_string(settings.overlay.letterSpacing) + "\n";
    text += "uppercase = " + std::to_string(settings.overlay.uppercaseMode) + "\n";
    text += "separators = " + std::to_string(settings.overlay.separatorsMode) + "\n";
    text += "fps_decimals = " + std::to_string(settings.overlay.fpsDecimals) + "\n";
    for (std::size_t index = 0; index < kLabelCount; ++index) {
        const auto field = static_cast<LabelField>(index);
        text += std::string("row_color_") + std::string(labelKey(field)) + " = "
            + std::to_string(settings.overlay.rowColors[index]) + "\n";
    }
    text += std::string("click_through = ") + (settings.overlay.clickThrough ? "1" : "0") + "\n";
    for (std::size_t index = 0; index < kLabelCount; ++index) {
        const auto field = static_cast<LabelField>(index);
        text += std::string("show_") + std::string(labelKey(field)) + " = "
            + (settings.overlay.visible[index] ? "1" : "0") + "\n";
    }
    text += "\n";

    text += "[labels]\n";
    for (std::size_t index = 0; index < kLabelCount; ++index) {
        const auto field = static_cast<LabelField>(index);
        text += std::string(labelKey(field)) + " = " + labelValue(settings.overlay, field) + "\n";
    }
    text += "\n";

    text += "[hotkeys]\n";
    text += "overlay = " + std::to_string(settings.hotkeys.overlay) + "\n";
    text += "position = " + std::to_string(settings.hotkeys.position) + "\n";
    text += "window = " + std::to_string(settings.hotkeys.window) + "\n";
    text += "click_through = " + std::to_string(settings.hotkeys.clickThrough) + "\n\n";

    text += "[ui]\n";
    text += "accent = " + std::string(accentKey(settings.ui.accent)) + "\n";
    text += std::string("density = ") + (settings.ui.compactDensity ? "compact" : "normal") + "\n";
    text += std::string("animate = ") + (settings.ui.animate ? "1" : "0") + "\n";
    text += std::string("close_to_tray = ") + (settings.ui.closeToTray ? "1" : "0") + "\n";
    return text;
}

} // namespace yawa

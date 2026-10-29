#include "ui/settings.h"
#include "metrics/overlay_metric.h"
#include "ui/rtss_exchange.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <utility>
#include <vector>

namespace yawa {
namespace {

    constexpr int kMinIntervalMs = 250;
    constexpr int kMaxIntervalMs = 5000;
    constexpr int kMinScale = 70;
    constexpr int kMaxScale = 160;
    constexpr std::size_t kMaxLabelLength = 10;
    constexpr std::size_t kMaxRtssBytes = 1024 * 1024;
    constexpr std::size_t kMaxRtssExchangeBytes = 8 * 1024 * 1024;

    struct StyleKey
    {
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

    constexpr std::array<OverlayRecipeSpec, kOverlayRecipeCount> kRecipes { {
        { "neon", OverlayStyle::Neon, OverlayLayout::Plates, OverlaySurface::Glass, 0, 100, false },
        { "rtss", OverlayStyle::Rtss, OverlayLayout::Plates, OverlaySurface::None, 0, 100, false },
        { "frosted", OverlayStyle::Frosted, OverlayLayout::Plates, OverlaySurface::Glass, 0, 100, false },
        { "terminal", OverlayStyle::Terminal, OverlayLayout::Plates, OverlaySurface::Plate, 0, 100, false },
        { "cyber", OverlayStyle::Cyber, OverlayLayout::Plates, OverlaySurface::Glass, 0, 100, false },
        { "mono", OverlayStyle::Mono, OverlayLayout::Plates, OverlaySurface::Plate, 0, 100, false },
        { "esport", OverlayStyle::Esport, OverlayLayout::Plates, OverlaySurface::Plate, 0, 100, false },
        { "retro", OverlayStyle::Retro, OverlayLayout::Plates, OverlaySurface::Plate, 0, 100, false },
        { "fps_large", OverlayStyle::Neon, OverlayLayout::Line, OverlaySurface::None, 0, 130, true },
        { "stream", OverlayStyle::Frosted, OverlayLayout::Line, OverlaySurface::Plate, 0, 110, false },
        { "laptop", OverlayStyle::Mono, OverlayLayout::Line, OverlaySurface::None, 2, 85, false },
        { "hdr", OverlayStyle::Esport, OverlayLayout::Line, OverlaySurface::Outline, 0, 120, false },
        { "recording", OverlayStyle::Terminal, OverlayLayout::Plates, OverlaySurface::Plate, 1, 105, false },
        { "quiet", OverlayStyle::Frosted, OverlayLayout::Plates, OverlaySurface::Glass, 0, 85, false },
        { "focus", OverlayStyle::Cyber, OverlayLayout::Gauge, OverlaySurface::Outline, 2, 110, false },
        { "clean", OverlayStyle::Mono, OverlayLayout::Gauge, OverlaySurface::Plate, 1, 100, false },
    } };

    struct RtssMetric
    {
        MetricId id;
        std::string_view tag;
        std::string_view source;
        std::string_view unit;
    };

    constexpr std::array<RtssMetric, 13> kRtssMetrics { {
        { MetricId::Fps, "<FR>", "", "FPS" },
        { MetricId::Fps, "%Framerate%", "Framerate", "FPS" },
        { MetricId::FrameTime, "%Frametime%", "Frametime", "ms" },
        { MetricId::CpuLoad, "%CPU usage%", "CPU usage", "%" },
        { MetricId::CpuTemp, "%CPU temperature%", "CPU temperature", "C" },
        { MetricId::CpuClock, "%CPU clock%", "CPU clock", "MHz" },
        { MetricId::CpuPower, "%CPU power%", "CPU power", "W" },
        { MetricId::GpuLoad, "%GPU1 usage%", "GPU1 usage", "%" },
        { MetricId::GpuTemp, "%GPU1 temperature%", "GPU1 temperature", "C" },
        { MetricId::GpuClock, "%GPU1 clock%", "GPU1 clock", "MHz" },
        { MetricId::GpuPower, "%GPU1 power%", "GPU1 power", "W" },
        { MetricId::VramUsed, "%GPU1 memory usage%", "GPU1 memory usage", "MB" },
        { MetricId::RamUsed, "%Total RAM%", "Total RAM", "MB" },
    } };

    void attachRtssImage(OverlaySettings& overlay, const RtssExchange& exchange)
    {
        if (!overlay.rtssVisual || overlay.rtssVisual->embeddedImage.empty()) {
            return;
        }
        const std::string& wanted = overlay.rtssVisual->embeddedImage;
        const auto image = std::find_if(
            exchange.images.begin(), exchange.images.end(), [&wanted](const auto& file) {
                return file.first.size() == wanted.size()
                    && std::equal(file.first.begin(), file.first.end(), wanted.begin(),
                        [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
            });
        if (image != exchange.images.end()) {
            auto visual = std::make_shared<RtssVisualLayout>(*overlay.rtssVisual);
            visual->imageBytes = std::make_shared<const std::string>(image->second);
            overlay.rtssVisual = std::move(visual);
        }
    }

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

    std::string hexEncode(std::string_view bytes)
    {
        constexpr char digits[] = "0123456789ABCDEF";
        std::string encoded;
        encoded.reserve(bytes.size() * 2);
        for (unsigned char byte : bytes) {
            encoded.push_back(digits[byte >> 4]);
            encoded.push_back(digits[byte & 15]);
        }
        return encoded;
    }

    std::string hexDecode(std::string_view encoded, std::size_t maxBytes)
    {
        if (encoded.size() > 2 * maxBytes || encoded.size() % 2 != 0) {
            return { };
        }
        std::string bytes;
        bytes.reserve(encoded.size() / 2);
        for (std::size_t index = 0; index < encoded.size(); index += 2) {
            unsigned int byte = 0;
            const auto parsed = std::from_chars(encoded.data() + index, encoded.data() + index + 2, byte, 16);
            if (parsed.ec != std::errc { } || parsed.ptr != encoded.data() + index + 2) {
                return { };
            }
            bytes.push_back(static_cast<char>(byte));
        }
        return bytes;
    }

    int parseInt(std::string_view text, int fallback)
    {
        int value = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        return result.ec == std::errc { } ? value : fallback;
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
        return result.ec == std::errc { } ? value : fallback;
    }

    std::int32_t parseInt32(std::string_view text, std::int32_t fallback)
    {
        std::int32_t value = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        return result.ec == std::errc { } ? value : fallback;
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
            cut += (byte & 0xC0) == 0x80 ? 1
                                         : (byte >= 0xF0 ? 4 : (byte >= 0xE0 ? 3 : (byte >= 0xC0 ? 2 : 1)));
            ++characters;
        }
        result.resize(std::min(cut, result.size()));
        return result;
    }

} // namespace

std::array<bool, kMetricCount> defaultOverlayMetrics()
{
    std::array<bool, kMetricCount> selected { };
    for (MetricId id : { MetricId::Fps, MetricId::Low1Fps, MetricId::CpuLoad, MetricId::CpuTemp,
             MetricId::GpuLoad, MetricId::GpuTemp, MetricId::RamUsed, MetricId::VramUsed }) {
        selected[static_cast<std::size_t>(id)] = true;
    }
    return selected;
}

std::array<MetricId, kMetricCount> defaultOverlayOrder()
{
    std::array<MetricId, kMetricCount> order { };
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = static_cast<MetricId>(i);
    }
    return order;
}

std::array<int, kMetricCount> defaultOverlayMetricColors()
{
    std::array<int, kMetricCount> colors { };
    colors.fill(-1);
    return colors;
}

std::optional<std::size_t> overlayColorGroup(MetricId id)
{
    if (id <= MetricId::DroppedFrames)
        return 0;
    if ((id >= MetricId::CpuLoad && id <= MetricId::CpuPeakLoad)
        || (id >= MetricId::Core0Load && id <= MetricId::Core15Load))
        return 1;
    if (id >= MetricId::GpuLoad && id <= MetricId::PcieLoad)
        return 2;
    if (id >= MetricId::VramUsed && id <= MetricId::VramTotal)
        return 4;
    if (id >= MetricId::RamUsed && id <= MetricId::PageFileLoad)
        return 3;
    return std::nullopt;
}

void setOverlayMetricLabel(OverlaySettings& settings, MetricId id, std::string label)
{
    const std::size_t index = static_cast<std::size_t>(id);
    if (index >= kMetricCount) {
        return;
    }
    // Не обрезаем UTF-8 посередине символа; переводы строк не должны ломать INI.
    std::string clean;
    std::size_t characters = 0;
    for (std::size_t i = 0; i < label.size() && characters < 32;) {
        const unsigned char byte = static_cast<unsigned char>(label[i]);
        const std::size_t length = byte < 0x80 ? 1 : byte < 0xe0 ? 2 : byte < 0xf0 ? 3 : 4;
        if (i + length > label.size()) {
            break;
        }
        if (byte != '\n' && byte != '\r' && byte != '=' && byte != '[' && byte != ']') {
            clean.append(label, i, length);
            ++characters;
        }
        i += length;
    }
    settings.customLabels[index] = std::move(clean);
    settings.customLabelSet[index] = true;
}

void moveOverlayMetric(OverlaySettings& settings, MetricId moved, MetricId before)
{
    const auto source = std::find(settings.order.begin(), settings.order.end(), displayMetric(moved));
    const auto target = std::find(settings.order.begin(), settings.order.end(), displayMetric(before));
    if (source == settings.order.end() || target == settings.order.end() || source == target) {
        return;
    }
    const MetricId value = *source;
    if (source < target) {
        std::move(source + 1, target + 1, source);
        *target = value;
    } else {
        std::move_backward(target, source, source + 1);
        *target = value;
    }
}

void enableOverlayPixelLayout(OverlaySettings& settings)
{
    int row = 0;
    for (MetricId id : settings.order) {
        if (static_cast<std::size_t>(id) >= kMetricCount || displayMetric(id) != id) {
            continue;
        }
        bool selected = settings.metrics[static_cast<std::size_t>(id)];
        for (MetricId detail : kPairedMetricDetails) {
            if (displayMetric(detail) == id) {
                selected |= settings.metrics[static_cast<std::size_t>(detail)];
            }
        }
        if (!selected) {
            continue;
        }
        if (settings.rowPositions[static_cast<std::size_t>(id)].y < 0) {
            settings.rowPositions[static_cast<std::size_t>(id)].y = std::min(12 + row * 28, 1490);
        }
        ++row;
    }
    settings.pixelLayout = true;
}

OverlayRowPosition moveOverlayRow(OverlayRowPosition start, int deltaX, int deltaY)
{
    const int dx = std::clamp(
        deltaX, -std::min(start.labelX, start.valueX), 470 - std::max(start.labelX, start.valueX));
    return { start.labelX + dx, start.valueX + dx, std::clamp(start.y + deltaY, 0, 1490) };
}

OverlayRowOffset moveOverlayRowOffset(OverlayRowOffset start, int deltaX, int deltaY)
{
    return { std::clamp(start.x + deltaX, -2048, 2048), std::clamp(start.y + deltaY, -2048, 2048) };
}

int scaledOverlayOffset(int offset, double sourceExtent, double targetExtent)
{
    if (sourceExtent <= 0.0 || targetExtent <= 0.0) {
        return offset;
    }
    return std::clamp(
        static_cast<int>(std::lround(static_cast<double>(offset) * targetExtent / sourceExtent)), -8192,
        8192);
}

bool parseOverlayRgba(std::string_view text, std::uint32_t& rgba)
{
    if (text.size() != 9 || text.front() != '#') {
        return false;
    }
    std::uint32_t parsed = 0;
    const auto result = std::from_chars(text.data() + 1, text.data() + text.size(), parsed, 16);
    if (result.ec != std::errc { } || result.ptr != text.data() + text.size()) {
        return false;
    }
    rgba = parsed;
    return true;
}

std::string overlayRgbaHex(std::uint32_t rgba)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result = "#00000000";
    for (int index = 8; index >= 1; --index) {
        result[static_cast<std::size_t>(index)] = digits[rgba & 0xF];
        rgba >>= 4;
    }
    return result;
}

Settings defaultSettings()
{
    return Settings { };
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
    overlay.valueRgba.reset();
    overlay.labelRgba.reset();
    overlay.accentRgba.reset();
    overlay.rowRgba.fill(std::nullopt);
    overlay.metricRgba.fill(std::nullopt);
    overlay.rowColors = { -1, -1, -1, -1, -1 };
    overlay.borderWidth = -1;
    overlay.shadowStrength = 100;
    overlay.letterSpacing = 100;
    overlay.uppercaseMode = -1;
    overlay.separatorsMode = -1;
    overlay.fpsDecimals = -1;
    overlay.textScalePercent = 100;
}

const std::array<OverlayRecipeSpec, kOverlayRecipeCount>& overlayRecipes()
{
    return kRecipes;
}

std::string_view overlayRecipeKey(OverlayRecipe recipe)
{
    const auto index = static_cast<std::size_t>(recipe);
    return index < kRecipes.size() ? kRecipes[index].key : kRecipes[2].key;
}

bool parseOverlayRecipe(std::string_view key, OverlayRecipe& recipe)
{
    for (std::size_t index = 0; index < kRecipes.size(); ++index) {
        if (key == kRecipes[index].key) {
            recipe = static_cast<OverlayRecipe>(index);
            return true;
        }
    }
    return false;
}

std::string_view overlaySurfaceKey(OverlaySurface surface)
{
    switch (surface) {
    case OverlaySurface::Glass:
        return "glass";
    case OverlaySurface::Plate:
        return "plate";
    case OverlaySurface::Outline:
        return "outline";
    case OverlaySurface::None:
        return "none";
    }
    return "glass";
}

bool parseOverlaySurface(std::string_view key, OverlaySurface& surface)
{
    for (OverlaySurface candidate :
        { OverlaySurface::Glass, OverlaySurface::Plate, OverlaySurface::Outline, OverlaySurface::None }) {
        if (key == overlaySurfaceKey(candidate)) {
            surface = candidate;
            return true;
        }
    }
    return false;
}

void setOverlaySurface(OverlaySettings& overlay, OverlaySurface surface)
{
    overlay.surface = surface;
    overlay.surfaceActive = true;
    overlay.showBackground = surface == OverlaySurface::Glass || surface == OverlaySurface::Plate;
    overlay.showBorder = surface == OverlaySurface::Glass || surface == OverlaySurface::Outline;
    overlay.showShadow = surface == OverlaySurface::Glass;
}

void applyOverlayRecipe(OverlaySettings& overlay, OverlayRecipe recipe)
{
    const auto index = static_cast<std::size_t>(recipe);
    if (index >= kRecipes.size()) {
        return;
    }
    const OverlayRecipeSpec& spec = kRecipes[index];
    applyStyleDefaults(overlay, spec.palette);
    overlay.recipe = recipe;
    overlay.layout = spec.shape;
    overlay.fontFamily = spec.fontFamily;
    overlay.scalePercent = spec.scalePercent;
    if (index >= 8) {
        setOverlaySurface(overlay, spec.surface);
    } else {
        overlay.surface = spec.surface;
        overlay.surfaceActive = false; // старые восемь рецептов сохраняют прежнюю палитру и подложку
    }
    // Рецепт осознанно сбрасывает состав, но не порядок/глазики вне применения рецепта.
    overlay.metrics = defaultOverlayMetrics();
    if (spec.onlyFps) {
        overlay.metrics.fill(false);
        overlay.metrics[static_cast<std::size_t>(MetricId::Fps)] = true;
    }
}

std::string serializeOverlayRecipeJson(const OverlaySettings& overlay)
{
    const std::string_view font = overlay.fontFamily == 1 ? "mono"
        : overlay.fontFamily == 2                         ? "condensed"
        : overlay.fontFamily == 3                         ? "dense"
                                                          : "sans";
    std::string json = "{\n  \"schema\": 1,\n  \"recipe\": \"" + std::string(overlayRecipeKey(overlay.recipe))
        + "\",\n  \"palette\": \"" + std::string(overlayStyleKey(overlay.style)) + "\",\n  \"shape\": \""
        + std::string(overlayLayoutKey(overlay.layout)) + "\",\n  \"surface\": ";
    json += overlay.surfaceActive ? "\"" + std::string(overlaySurfaceKey(overlay.surface)) + "\"" : "null";
    json += ",\n  \"typeface\": \"" + std::string(font)
        + "\",\n  \"scale\": " + std::to_string(std::clamp(overlay.scalePercent, 70, 160));
    std::string colors;
    const auto addColor = [&colors](std::string_view key, std::optional<std::uint32_t> rgba) {
        if (rgba) {
            if (!colors.empty())
                colors += ",\n";
            colors += "    \"" + std::string(key) + "\": \"" + overlayRgbaHex(*rgba) + "\"";
        }
    };
    addColor("value", overlay.valueRgba);
    addColor("label", overlay.labelRgba);
    addColor("accent", overlay.accentRgba);
    for (std::size_t index = 0; index < kLabelCount; ++index) {
        addColor(labelKey(static_cast<LabelField>(index)), overlay.rowRgba[index]);
    }
    if (!colors.empty())
        json += ",\n  \"colors\": {\n" + colors + "\n  }";
    return json + "\n}\n";
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
    switch (layout) {
    case OverlayLayout::Plates:
        return "plates";
    case OverlayLayout::Line:
        return "line";
    case OverlayLayout::Gauge:
        return "gauge";
    }
    return "plates";
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
    if (text == "gauge") {
        layout = OverlayLayout::Gauge;
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

    // Рецепт применяется до любых явных полей независимо от порядка строк INI:
    // старые ключи layout/font/background и глазики должны иметь приоритет.
    std::shared_ptr<const std::string> standaloneImage;
    bool hasRecipe = false;
    bool hasSurface = false;
    bool customSurface = false;
    OverlayRecipe chosenRecipe = target.overlay.recipe;
    OverlaySurface chosenSurface = target.overlay.surface;
    std::string_view scan = text;
    std::string_view scanSection;
    while (!scan.empty()) {
        const std::size_t end = scan.find('\n');
        std::string_view line = trim(end == std::string_view::npos ? scan : scan.substr(0, end));
        scan = end == std::string_view::npos ? std::string_view { } : scan.substr(end + 1);
        if (!line.empty() && line.front() == '[') {
            const std::size_t close = line.find(']');
            scanSection
                = close == std::string_view::npos ? std::string_view { } : trim(line.substr(1, close - 1));
        } else if (scanSection == "overlay") {
            const std::size_t equals = line.find('=');
            if (equals != std::string_view::npos && trim(line.substr(0, equals)) == "recipe") {
                hasRecipe = parseOverlayRecipe(trim(line.substr(equals + 1)), chosenRecipe);
            } else if (equals != std::string_view::npos && trim(line.substr(0, equals)) == "surface") {
                const std::string_view requested = trim(line.substr(equals + 1));
                customSurface = requested == "custom";
                hasSurface = !customSurface && parseOverlaySurface(requested, chosenSurface);
            }
        }
    }
    if (hasRecipe) {
        applyOverlayRecipe(target.overlay, chosenRecipe);
    }
    if (hasSurface) {
        setOverlaySurface(target.overlay, chosenSurface);
    } else if (customSurface) {
        target.overlay.surfaceActive = false;
    }

    std::string section;
    std::size_t lineNumber = 0;
    bool hasMetricSelection = false;

    while (!text.empty()) {
        const std::size_t lineEnd = text.find('\n');
        std::string_view line = lineEnd == std::string_view::npos ? text : text.substr(0, lineEnd);
        text = lineEnd == std::string_view::npos ? std::string_view { } : text.substr(lineEnd + 1);
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
            if (section == "overlay_metrics") {
                hasMetricSelection = true;
            }
            continue;
        }

        const std::size_t separator = line.find('=');
        if (separator == std::string_view::npos) {
            error = "строка " + std::to_string(lineNumber) + ": нет знака =";
            return false;
        }

        const std::string key(trim(line.substr(0, separator)));
        std::string_view value = trim(line.substr(separator + 1));

        if (section == "capture") {
            if (key == "interval_ms") {
                target.capture.intervalMs
                    = std::clamp(parseInt(value, target.capture.intervalMs), kMinIntervalMs, kMaxIntervalMs);
            } else if (key == "smoothing") {
                target.capture.smoothingPercent
                    = std::clamp(parseInt(value, target.capture.smoothingPercent), 0, 90);
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
            } else if (key == "anchor_mode") {
                if (value == "corner") {
                    target.overlay.anchorMode = OverlayAnchorMode::Corner;
                } else if (value == "free") {
                    target.overlay.anchorMode = OverlayAnchorMode::Free;
                }
            } else if (key == "origin_x") {
                target.overlay.originX = std::clamp(parseInt(value, target.overlay.originX), 0, 32768);
            } else if (key == "origin_y") {
                target.overlay.originY = std::clamp(parseInt(value, target.overlay.originY), 0, 32768);
            } else if (key == "saved_monitor_id") {
                if (value.size() <= 256 && value.find_first_of("\r\n") == std::string_view::npos) {
                    target.overlay.savedMonitorId = std::string(value);
                }
            } else if (key == "monitor_origin_x") {
                target.overlay.monitorOriginX
                    = std::clamp(parseInt(value, target.overlay.monitorOriginX), 0, 32768);
            } else if (key == "monitor_origin_y") {
                target.overlay.monitorOriginY
                    = std::clamp(parseInt(value, target.overlay.monitorOriginY), 0, 32768);
            } else if (key == "follow_game_monitor") {
                target.overlay.followGameMonitor = parseBool(value, target.overlay.followGameMonitor);
            } else if (key == "fps_sparkline") {
                target.overlay.showSparkline = parseBool(value, target.overlay.showSparkline);
            } else if (key == "sparkline_metric") {
                const MetricInfo* metric = findMetricByKey(value);
                if (metric != nullptr
                    && (metric->id == MetricId::Fps || metric->id == MetricId::FrameTime
                        || metric->id == MetricId::CpuLoad || metric->id == MetricId::GpuLoad)) {
                    target.overlay.sparklineMetric = metric->id;
                }
            } else if (key == "recipe") {
                // Уже применён до чтения всех ручных параметров.
            } else if (key == "layout") {
                parseOverlayLayout(value, target.overlay.layout);
            } else if (key == "style") {
                parseOverlayStyle(value, target.overlay.style);
            } else if (key == "surface") {
                // Уже применено до явных переключателей background/border/shadow.
            } else if (key == "scale") {
                target.overlay.scalePercent
                    = std::clamp(parseInt(value, target.overlay.scalePercent), kMinScale, kMaxScale);
            } else if (key == "opacity") {
                target.overlay.opacityPercent
                    = std::clamp(parseInt(value, target.overlay.opacityPercent), 0, 100);
            } else if (key == "menu_opacity") {
                target.overlay.menuOpacityPercent
                    = std::clamp(parseInt(value, target.overlay.menuOpacityPercent), 0, 100);
            } else if (key == "offset_x") {
                target.overlay.offsetX = std::clamp(parseInt(value, target.overlay.offsetX), -8192, 8192);
            } else if (key == "pixel_layout") {
                target.overlay.pixelLayout = parseBool(value, target.overlay.pixelLayout);
            } else if (key == "offset_y") {
                target.overlay.offsetY = std::clamp(parseInt(value, target.overlay.offsetY), -8192, 8192);
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
                target.overlay.cornerRadius
                    = std::clamp(parseInt(value, target.overlay.cornerRadius), -1, 24);
            } else if (key == "text_scale") {
                target.overlay.textScalePercent
                    = std::clamp(parseInt(value, target.overlay.textScalePercent), 85, 130);
            } else if (key == "bg_opacity") {
                target.overlay.backgroundOpacityPercent
                    = std::clamp(parseInt(value, target.overlay.backgroundOpacityPercent), -1, 100);
            } else if (key == "padding") {
                target.overlay.paddingPx = std::clamp(parseInt(value, target.overlay.paddingPx), -1, 28);
            } else if (key == "row_gap") {
                target.overlay.rowGapPx = std::clamp(parseInt(value, target.overlay.rowGapPx), -1, 14);
            } else if (key == "background_width") {
                const int width = parseInt(value, -1);
                if (width == -1 || (width >= 80 && width <= 2000))
                    target.overlay.backgroundWidthPx = width;
            } else if (key == "background_height") {
                const int height = parseInt(value, -1);
                if (height == -1 || (height >= 40 && height <= 1600))
                    target.overlay.backgroundHeightPx = height;
            } else if (key == "value_color") {
                target.overlay.valueColor = std::clamp(parseInt(value, target.overlay.valueColor), -1, 5);
            } else if (key == "label_color") {
                target.overlay.labelColor = std::clamp(parseInt(value, target.overlay.labelColor), -1, 5);
            } else if (key == "accent_color") {
                target.overlay.accentColor = std::clamp(parseInt(value, target.overlay.accentColor), -1, 5);
            } else if (key == "value_rgba") {
                std::uint32_t rgba = 0;
                if (parseOverlayRgba(value, rgba))
                    target.overlay.valueRgba = rgba;
            } else if (key == "label_rgba") {
                std::uint32_t rgba = 0;
                if (parseOverlayRgba(value, rgba))
                    target.overlay.labelRgba = rgba;
            } else if (key == "accent_rgba") {
                std::uint32_t rgba = 0;
                if (parseOverlayRgba(value, rgba))
                    target.overlay.accentRgba = rgba;
            } else if (key.size() > 9 && key.substr(0, 9) == "row_rgba_") {
                for (std::size_t index = 0; index < kLabelCount; ++index) {
                    if (key.substr(9) == labelKey(static_cast<LabelField>(index))) {
                        std::uint32_t rgba = 0;
                        if (parseOverlayRgba(value, rgba))
                            target.overlay.rowRgba[index] = rgba;
                    }
                }
            } else if (key == "border_width") {
                target.overlay.borderWidth = std::clamp(parseInt(value, target.overlay.borderWidth), -1, 3);
            } else if (key == "shadow_strength") {
                target.overlay.shadowStrength
                    = std::clamp(parseInt(value, target.overlay.shadowStrength), 0, 100);
            } else if (key == "letter_spacing") {
                target.overlay.letterSpacing
                    = std::clamp(parseInt(value, target.overlay.letterSpacing), 0, 200);
            } else if (key == "uppercase") {
                target.overlay.uppercaseMode
                    = std::clamp(parseInt(value, target.overlay.uppercaseMode), -1, 1);
            } else if (key == "separators") {
                target.overlay.separatorsMode
                    = std::clamp(parseInt(value, target.overlay.separatorsMode), -1, 1);
            } else if (key == "fps_decimals") {
                target.overlay.fpsDecimals = std::clamp(parseInt(value, target.overlay.fpsDecimals), -1, 2);
            } else if (key.size() > 10 && key.substr(0, 10) == "row_color_") {
                for (std::size_t index = 0; index < kLabelCount; ++index) {
                    const auto field = static_cast<LabelField>(index);
                    if (key.substr(10) == labelKey(field)) {
                        target.overlay.rowColors[index]
                            = std::clamp(parseInt(value, target.overlay.rowColors[index]), -1, 5);
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
        } else if (section == "overlay_metrics") {
            for (const MetricInfo& metric : kMetricTable) {
                if (key == metric.key) {
                    const auto index = static_cast<std::size_t>(metric.id);
                    target.overlay.metrics[index] = parseBool(value, target.overlay.metrics[index]);
                }
            }
        } else if (section == "overlay_order") {
            if (key == "rows") {
                std::array<MetricId, kMetricCount> parsed { };
                std::array<bool, kMetricCount> seen { };
                bool valid = true;
                std::size_t index = 0;
                while (!value.empty() && index < kMetricCount) {
                    const std::size_t comma = value.find(',');
                    const auto token = trim(value.substr(0, comma));
                    const auto entry = std::find_if(kMetricTable.begin(), kMetricTable.end(),
                        [token](const MetricInfo& info) { return info.key == token; });
                    if (entry == kMetricTable.end() || seen[static_cast<std::size_t>(entry->id)]) {
                        valid = false;
                        break;
                    }
                    parsed[index++] = entry->id;
                    seen[static_cast<std::size_t>(entry->id)] = true;
                    if (comma == std::string_view::npos) {
                        break;
                    }
                    if (index == kMetricCount || comma + 1 == value.size()) {
                        valid = false;
                        break;
                    }
                    value.remove_prefix(comma + 1);
                }
                if (valid && index == kMetricCount
                    && std::all_of(seen.begin(), seen.end(), [](bool included) { return included; })) {
                    target.overlay.order = parsed;
                }
            }
        } else if (section == "overlay_positions") {
            const auto entry = std::find_if(kMetricTable.begin(), kMetricTable.end(),
                [&key](const MetricInfo& info) { return info.key == key; });
            if (entry != kMetricTable.end()) {
                const std::size_t first = value.find(',');
                const std::size_t second
                    = first == std::string_view::npos ? first : value.find(',', first + 1);
                if (first != std::string_view::npos && second != std::string_view::npos
                    && value.find(',', second + 1) == std::string_view::npos) {
                    int label = parseInt(trim(value.substr(0, first)), -1);
                    int number = parseInt(trim(value.substr(first + 1, second - first - 1)), -1);
                    int y = parseInt(trim(value.substr(second + 1)), -1);
                    if (label >= 0 && label <= 470 && number >= 0 && number <= 470 && y >= 0 && y <= 1490) {
                        target.overlay.rowPositions[static_cast<std::size_t>(entry->id)]
                            = { label, number, y };
                    }
                }
            }
        } else if (section == "overlay_style_offsets") {
            const auto entry = std::find_if(kMetricTable.begin(), kMetricTable.end(),
                [&key](const MetricInfo& info) { return info.key == key; });
            const std::size_t comma = value.find(',');
            if (entry != kMetricTable.end() && comma != std::string_view::npos
                && value.find(',', comma + 1) == std::string_view::npos) {
                const int x = parseInt(trim(value.substr(0, comma)), 2049);
                const int y = parseInt(trim(value.substr(comma + 1)), 2049);
                if (x >= -2048 && x <= 2048 && y >= -2048 && y <= 2048) {
                    target.overlay.styleRowOffsets[static_cast<std::size_t>(entry->id)] = { x, y };
                }
            }
        } else if (section == "overlay_metric_text_scale") {
            const auto entry = std::find_if(kMetricTable.begin(), kMetricTable.end(),
                [&key](const MetricInfo& info) { return info.key == key; });
            if (entry != kMetricTable.end()) {
                const int size = parseInt(value, 0);
                if (size >= 70 && size <= 200)
                    target.overlay.metricTextScalePercent[static_cast<std::size_t>(entry->id)] = size;
            }
        } else if (section == "overlay_metric_colors" || section == "overlay_metric_rgba") {
            const auto entry = std::find_if(kMetricTable.begin(), kMetricTable.end(),
                [&key](const MetricInfo& info) { return info.key == key; });
            if (entry != kMetricTable.end()) {
                const auto index = static_cast<std::size_t>(entry->id);
                if (section == "overlay_metric_colors") {
                    target.overlay.metricColors[index] = std::clamp(parseInt(value, -1), -1, 5);
                } else {
                    std::uint32_t rgba = 0;
                    if (parseOverlayRgba(value, rgba))
                        target.overlay.metricRgba[index] = rgba;
                }
            }
        } else if (section == "overlay_labels") {
            const auto entry = std::find_if(kMetricTable.begin(), kMetricTable.end(),
                [&key](const MetricInfo& info) { return info.key == key; });
            if (entry != kMetricTable.end()) {
                setOverlayMetricLabel(target.overlay, entry->id, std::string(value));
            }
        } else if (section == "rtss_original") {
            if (key == "template_hex" || key == "exchange_hex") {
                const std::size_t limit = key == "exchange_hex" ? kMaxRtssExchangeBytes : kMaxRtssBytes;
                std::string decoded = hexDecode(value, limit);
                if (!decoded.empty()) {
                    auto original = std::make_shared<const std::string>(std::move(decoded));
                    if (key == "exchange_hex") {
                        target.overlay.rtssExchange = std::move(original);
                    } else {
                        target.overlay.rtssTemplate = std::move(original);
                    }
                }
            } else if (key == "image_hex") {
                std::string decoded = hexDecode(value, 4 * 1024 * 1024);
                if (!decoded.empty()) {
                    standaloneImage = std::make_shared<const std::string>(std::move(decoded));
                }
            } else if (key == "omitted_layers") {
                target.overlay.rtssOmittedLayers = std::clamp(parseInt(value, 0), 0, 256);
            } else if (key == "resources") {
                target.overlay.rtssResources = std::clamp(parseInt(value, 0), 0, 63);
            }
        } else if (section == "rtss_layer_edits") {
            const int index = parseInt(key, -1);
            const std::size_t first = value.find(',');
            const std::size_t second = first == std::string_view::npos ? first : value.find(',', first + 1);
            if (index >= 0 && index < 256 && std::to_string(index) == key
                && second != std::string_view::npos && value.find(',', second + 1) == std::string_view::npos) {
                const int x = parseInt(trim(value.substr(0, first)), 2049);
                const int y = parseInt(trim(value.substr(first + 1, second - first - 1)), 2049);
                const int visible = parseInt(trim(value.substr(second + 1)), -1);
                if (x >= -2048 && x <= 2048 && y >= -2048 && y <= 2048
                    && (visible == 0 || visible == 1)) {
                    auto& edit = target.overlay.rtssLayerEdits[static_cast<std::size_t>(index)];
                    edit.x = x;
                    edit.y = y;
                    edit.visible = visible == 1;
                }
            }
        } else if (section == "rtss_layer_text" || section == "rtss_layer_color"
            || section == "rtss_layer_size" || section == "rtss_layer_extent") {
            const int index = parseInt(key, -1);
            if (index >= 0 && index < 256 && std::to_string(index) == key) {
                auto& edit = target.overlay.rtssLayerEdits[static_cast<std::size_t>(index)];
                if (section == "rtss_layer_text" && value.size() <= 2 * 8192 && value.size() % 2 == 0) {
                    const std::string decoded = hexDecode(value, 8192);
                    if (value.empty() || !decoded.empty()) {
                        edit.text = decoded;
                    }
                } else if (section == "rtss_layer_color" && value.size() == 8) {
                    std::uint32_t color = 0;
                    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), color, 16);
                    if (parsed.ec == std::errc { } && parsed.ptr == value.data() + value.size()) {
                        edit.textColor = color;
                    }
                } else if (section == "rtss_layer_size") {
                    const int size = parseInt(value, 0);
                    if (size >= 10 && size <= 400) {
                        edit.size = size;
                    }
                } else if (section == "rtss_layer_extent") {
                    const auto comma = value.find(',');
                    if (comma != std::string_view::npos
                        && value.find(',', comma + 1) == std::string_view::npos) {
                        const auto valid = [](int size) { return size == 0 || (size >= 16 && size <= 2048); };
                        const int width = parseInt(value.substr(0, comma), -1);
                        const int height = parseInt(value.substr(comma + 1), -1);
                        if (valid(width) && valid(height)) {
                            edit.width = width;
                            edit.height = height;
                        }
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
                target.hotkeys.clickThrough
                    = static_cast<unsigned int>(std::clamp(parseInt(value, 0), 0, 254));
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

    if (target.overlay.rtssTemplate) {
        OverlaySettings visualSettings;
        std::string visualError;
        if (importRtssOverlay(*target.overlay.rtssTemplate, visualSettings, visualError)) {
            target.overlay.rtssVisual = std::move(visualSettings.rtssVisual);
            if (standaloneImage && !target.overlay.rtssExchange
                && !target.overlay.rtssVisual->embeddedImage.empty()) {
                auto visual = std::make_shared<RtssVisualLayout>(*target.overlay.rtssVisual);
                visual->imageBytes = std::move(standaloneImage);
                target.overlay.rtssVisual = std::move(visual);
            }
        } else {
            target.overlay.rtssTemplate.reset();
        }
    }
    if (!target.overlay.rtssTemplate) {
        target.overlay.rtssOmittedLayers = 0;
        target.overlay.rtssExchange.reset();
        target.overlay.rtssVisual.reset();
    }
    if (target.overlay.rtssExchange) {
        RtssExchange package;
        std::string packageError;
        if (!unpackRtssExchange(*target.overlay.rtssExchange, package, packageError)
            || package.layout != *target.overlay.rtssTemplate) {
            target.overlay.rtssExchange.reset();
        } else {
            attachRtssImage(target.overlay, package);
        }
    }
    if (!target.overlay.rtssExchange) {
        target.overlay.rtssResources = 0;
    }
    if (!hasMetricSelection) {
        for (std::size_t index = 0; index < kLabelCount; ++index) {
            target.overlay.metrics[static_cast<std::size_t>(kOverlayPrimaryMetrics[index])]
                = target.overlay.visible[index];
        }
        target.overlay.metrics[static_cast<std::size_t>(MetricId::Low1Fps)] = target.overlay.visible[0];
        target.overlay.metrics[static_cast<std::size_t>(MetricId::CpuTemp)] = target.overlay.visible[1];
        target.overlay.metrics[static_cast<std::size_t>(MetricId::GpuTemp)] = target.overlay.visible[2];
    }
    if (!hasRecipe) {
        // Старая палитра становится одноимённым рецептом; явно заданные layout,
        // font, состав и chrome не трогаем — старый файл визуально не мигрирует сам.
        target.overlay.recipe = static_cast<OverlayRecipe>(target.overlay.style);
    }
    return true;
}

std::string rtssVisibleTemplate(std::string_view text)
{
    std::string visible;
    visible.reserve(std::min<std::size_t>(text.size(), 8192));
    for (std::size_t at = 0; at < text.size() && visible.size() < 8192;) {
        if (text[at] == '<') {
            const std::size_t end = text.find('>', at + 1);
            if (end != std::string_view::npos && end - at <= 256) {
                const auto token = text.substr(at, end - at + 1);
                if (token == "<FR>" || token.starts_with("<G=") || token.starts_with("<I=")) {
                    visible.append(token);
                }
                at = end + 1;
                continue;
            }
        }
        if (text[at] == '\\' && at + 1 < text.size()) {
            const char code = text[at + 1];
            visible += code == 'n' || code == 'r' ? '\n' : code == 't' || code == 'b' ? ' ' : code;
            at += 2;
        } else {
            visible += text[at++];
        }
    }
    return visible;
}

bool importRtssOverlay(std::string_view text, OverlaySettings& overlay, std::string& error)
{
    error.clear();
    if (text.size() > kMaxRtssBytes || text.find('\0') != std::string_view::npos) {
        error = "Шаблон RTSS слишком большой или содержит двоичные данные";
        return false;
    }

    struct RtssSource
    {
        std::string name;
        std::string provider;
        std::string id;
    };
    const auto original = std::make_shared<const std::string>(text);
    auto visual = std::make_shared<RtssVisualLayout>();
    visual->layers.resize(256);
    const auto coordinate = [](std::string_view value, int fallback) {
        return std::clamp(parseInt(value, fallback), -512, 2048);
    };
    const auto decodeColor = [](std::string_view encoded, std::uint32_t fallback) {
        // У RTSS условный цвет начинается с управляющих полей, затем идут
        // цветовые значения. Берём первое буквальное значение, не вычисляя условие.
        for (int index = 0; index < 12 && !encoded.empty(); ++index) {
            const auto comma = encoded.find(',');
            const std::string_view candidate = encoded.substr(0, comma);
            if (candidate.size() == 6 || candidate.size() == 8) {
                std::uint32_t parsed = 0;
                const auto result
                    = std::from_chars(candidate.data(), candidate.data() + candidate.size(), parsed, 16);
                if (result.ec == std::errc { } && result.ptr == candidate.data() + candidate.size()) {
                    return candidate.size() == 6 ? parsed | 0xFF000000u : parsed;
                }
            }
            if (comma == std::string_view::npos) {
                break;
            }
            encoded.remove_prefix(comma + 1);
        }
        return fallback;
    };
    std::array<bool, kMetricCount> selected { };
    std::array<std::string, 256> layerTexts { };
    std::array<std::string, 256> layerColors { };
    std::array<RtssSource, 256> sources { };
    std::array<std::optional<std::uint32_t>, kMetricCount> colors { };
    std::vector<MetricId> order;
    std::string section;
    bool version = false;
    int layers = -1;
    bool validLayers = false;
    int sourceCount = -1;
    int omitted = 0;
    std::array<bool, 256> layerPresent { };
    while (!text.empty()) {
        const auto end = text.find('\n');
        std::string_view line = trim(end == std::string_view::npos ? text : text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view { } : text.substr(end + 1);
        if (line.empty() || line.front() == ';' || line.front() == '#') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section.assign(line.substr(1, line.size() - 2));
            if (section.size() > 5 && section.substr(0, 5) == "Layer") {
                const std::string_view number = std::string_view(section).substr(5);
                const int index = parseInt(number, -1);
                if (index >= 0 && index < 256 && std::to_string(index) == number) {
                    layerPresent[static_cast<std::size_t>(index)] = true;
                }
            }
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const std::string_view key = trim(line.substr(0, equals));
        const std::string_view value = trim(line.substr(equals + 1));
        if (section == "Master" && key == "Implementation") {
            version = value == "2";
        } else if (section == "Master" && key == "FontFace" && value.size() <= 96) {
            visual->fontFace = value;
        } else if (section == "Master" && key == "FontHeight") {
            visual->fontHeight = std::clamp(std::abs(std::clamp(parseInt(value, -13), -64, 64)), 4, 64);
        } else if (section == "Master" && key == "FontWeight") {
            visual->fontWeight = std::clamp(parseInt(value, 400), 100, 900);
        } else if (section == "Master" && key == "ZoomRatio") {
            visual->zoom = std::clamp(parseInt(value, 1), 1, 8);
        } else if (section == "Settings" && key == "EmbeddedImage" && value.size() >= 5 && value.size() <= 256
            && value.find("..") == std::string_view::npos
            && value.find_first_of("/\\:") == std::string_view::npos) {
            std::string extension(value.substr(value.size() - 4));
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](unsigned char symbol) { return static_cast<char>(std::tolower(symbol)); });
            if (extension == ".png") {
                visual->embeddedImage = value;
            }
        } else if (section == "General" && key == "Layers") {
            layers = parseInt(value, -1);
            validLayers = layers >= 1 && layers <= 256 && std::to_string(layers) == value;
        } else if (section == "General" && key == "Sources") {
            sourceCount = parseInt(value, -1);
            if (sourceCount < 0 || sourceCount > 256 || std::to_string(sourceCount) != value) {
                sourceCount = -1;
            }
        } else if (section.size() > 6 && section.substr(0, 6) == "Source") {
            const std::string_view number = std::string_view(section).substr(6);
            const int index = parseInt(number, -1);
            if (index < 0 || index >= 256 || std::to_string(index) != number) {
                continue;
            }
            RtssSource& source = sources[static_cast<std::size_t>(index)];
            if (key == "Name") {
                source.name = value;
            } else if (key == "Provider") {
                source.provider = value;
            } else if (key == "ID") {
                source.id = value;
            }
        } else if (section.size() > 5 && section.substr(0, 5) == "Layer") {
            const std::string_view layerNumber = std::string_view(section).substr(5);
            const int index = parseInt(layerNumber, -1);
            if (index < 0 || index >= 256 || std::to_string(index) != layerNumber) {
                continue;
            }
            RtssVisualLayer& layer = visual->layers[static_cast<std::size_t>(index)];
            if (key == "Name" && value.size() <= 256) {
                layer.name = value;
            } else if (key == "Text") {
                layerTexts[static_cast<std::size_t>(index)] = value;
                if (value.size() <= 8192) {
                    layer.text = value;
                }
            } else if (key == "TextColor") {
                layerColors[static_cast<std::size_t>(index)] = value;
                layer.textColor = decodeColor(value, 0xFFFFFFFFu);
            } else if (key == "BgndColor") {
                layer.backgroundColor = decodeColor(value, 0);
                layer.hasBackground = true;
            } else if (key == "VisibilitySource" && value.size() <= 256) {
                layer.visibilitySource = value;
            } else if (key == "PositionX") {
                layer.x = coordinate(value, 0);
            } else if (key == "PositionY") {
                layer.y = coordinate(value, 0);
            } else if (key == "ExtentX") {
                layer.width = coordinate(value, 0);
            } else if (key == "ExtentY") {
                layer.height = coordinate(value, 0);
            } else if (key == "ExtentOrigin") {
                layer.origin = std::clamp(parseInt(value, 0), 0, 8);
            } else if (key == "Size") {
                layer.size = std::clamp(parseInt(value, 100), 10, 400);
            }
        }
    }
    if (!version || !validLayers
        || std::none_of(
            layerPresent.begin(), layerPresent.begin() + layers, [](bool present) { return present; })) {
        error = "Неподдерживаемый шаблон RTSS OverlayEditor";
        return false;
    }
    visual->layers.resize(static_cast<std::size_t>(layers));
    const auto availableSource = [&](const RtssMetric& metric) {
        if (metric.source.empty()) {
            return true; // <FR> выдаёт сам RTSS, не [SourceN].
        }
        bool found = false;
        const int count = std::max(0, sourceCount);
        for (int index = 0; index < count; ++index) {
            const RtssSource& source = sources[static_cast<std::size_t>(index)];
            if (source.name != metric.source) {
                continue;
            }
            if (source.provider != "HAL" || source.id != metric.source) {
                return false; // Дубликат у внешнего провайдера делает тег неоднозначным.
            }
            found = true;
        }
        return found;
    };
    for (const RtssMetric& metric : kRtssMetrics) {
        if (availableSource(metric)) {
            visual->availableTags.emplace_back(metric.tag, metric.id);
        }
    }
    for (int index = 0; index < layers; ++index) {
        std::string lowered = layerTexts[static_cast<std::size_t>(index)];
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
            [](unsigned char letter) { return static_cast<char>(std::tolower(letter)); });
        const auto& layer = visual->layers[static_cast<std::size_t>(index)];
        const bool graphMode
            = layer.text.ends_with(",10>") || layer.text.ends_with(",80>") || layer.text.ends_with(",81>");
        bool recognized = graphMode
            && (layer.text.starts_with("<G=Frametime,") || layer.text.starts_with("<G=Framerate 1% Low,"));
        if (graphMode) {
            for (const RtssMetric& metric : kRtssMetrics) {
                if ((layer.text.starts_with("<G=CPU usage,") && metric.tag == "%CPU usage%")
                    || (layer.text.starts_with("<G=GPU1 usage,") && metric.tag == "%GPU1 usage%")) {
                    recognized |= availableSource(metric);
                }
            }
        }
        for (const RtssMetric& source : kRtssMetrics) {
            std::string tag(source.tag);
            std::transform(tag.begin(), tag.end(), tag.begin(),
                [](unsigned char letter) { return static_cast<char>(std::tolower(letter)); });
            if (lowered.find(tag) != std::string::npos && availableSource(source)) {
                recognized = true;
                const auto slot = static_cast<std::size_t>(source.id);
                if (!selected[slot]) {
                    order.push_back(source.id);
                    const std::string& encoded = layerColors[static_cast<std::size_t>(index)];
                    if (encoded.size() == 6 || encoded.size() == 8) {
                        std::uint32_t color = 0;
                        const auto parsed
                            = std::from_chars(encoded.data(), encoded.data() + encoded.size(), color, 16);
                        if (parsed.ec == std::errc { } && parsed.ptr == encoded.data() + encoded.size()) {
                            const std::uint32_t alpha = encoded.size() == 8 ? color >> 24 : 0xFFu;
                            colors[slot] = ((color & 0xFFFFFFu) << 8) | alpha;
                        }
                    }
                }
                selected[slot] = true;
            }
        }
        omitted += layerPresent[static_cast<std::size_t>(index)] && !recognized;
    }
    OverlaySettings imported = overlay;
    applyOverlayRecipe(imported, OverlayRecipe::Rtss);
    imported.metrics = selected;
    imported.metricRgba = colors;
    imported.rtssTemplate = original;
    imported.rtssExchange.reset();
    imported.rtssVisual = std::move(visual);
    imported.rtssLayerEdits = {};
    imported.rtssOmittedLayers = omitted;
    imported.rtssResources = 0;
    std::array<bool, kMetricCount> placed { };
    std::size_t output = 0;
    for (MetricId id : order) {
        imported.order[output++] = id;
        placed[static_cast<std::size_t>(id)] = true;
    }
    for (MetricId id : defaultOverlayOrder()) {
        if (!placed[static_cast<std::size_t>(id)]) {
            imported.order[output++] = id;
        }
    }
    overlay = std::move(imported);
    return true;
}

bool importRtssExchange(std::string_view bytes, OverlaySettings& overlay, std::string& error)
{
    RtssExchange package;
    if (!unpackRtssExchange(bytes, package, error)) {
        return false;
    }
    OverlaySettings imported = overlay;
    if (!importRtssOverlay(package.layout, imported, error)) {
        return false;
    }
    imported.rtssExchange = std::make_shared<const std::string>(bytes);
    imported.rtssResources = package.resources;
    attachRtssImage(imported, package);
    overlay = std::move(imported);
    return true;
}

std::string serializeRtssOverlay(const OverlaySettings& overlay)
{
    std::vector<const RtssMetric*> rows;
    std::array<bool, kMetricCount> seen { };
    for (MetricId id : overlay.order) {
        const std::size_t slot = static_cast<std::size_t>(id);
        if (slot >= kMetricCount || seen[slot] || !overlay.metrics[slot]) {
            continue;
        }
        seen[slot] = true;
        const auto source = std::find_if(kRtssMetrics.begin(), kRtssMetrics.end(),
            [id](const RtssMetric& metric) { return metric.id == id; });
        if (source != kRtssMetrics.end()) {
            rows.push_back(&*source);
        }
    }
    if (rows.empty()) {
        return { };
    }
    const std::size_t sources = static_cast<std::size_t>(std::count_if(
        rows.begin(), rows.end(), [](const RtssMetric* metric) { return !metric->source.empty(); }));
    std::string text = "[Master]\r\nImplementation=2\r\nFontFace=Consolas\r\nFontHeight=-9\r\n"
                       "FontWeight=400\r\nZoomRatio=3\r\n[Settings]\r\nRefreshPeriod=1000\r\n"
                       "[General]\r\nSources="
        + std::to_string(sources) + "\r\nTables=0\r\nLayers=" + std::to_string(rows.size()) + "\r\n";
    std::size_t sourceIndex = 0;
    for (const RtssMetric* metric : rows) {
        if (metric->source.empty()) {
            continue;
        }
        text += "[Source" + std::to_string(sourceIndex++) + "]\r\nName=" + std::string(metric->source)
            + "\r\nUnits=" + std::string(metric->unit)
            + "\r\nFormat=\r\nFormula=\r\nProvider=HAL\r\nID=" + std::string(metric->source) + "\r\n";
    }
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const RtssMetric& metric = *rows[index];
        text += "[Layer" + std::to_string(index)
            + "]\r\nName=" + std::string(metric.source.empty() ? "Framerate" : metric.source)
            + "\r\nText=" + std::string(metric.tag)
            + "\r\nPositionX=4\r\nPositionY=" + std::to_string(5 + static_cast<int>(index) * 14)
            + "\r\nExtentX=0\r\nExtentY=0\r\nExtentOrigin=0\r\nFixedAlignment=1\r\nTextColor=";
        const auto color = overlay.metricRgba[static_cast<std::size_t>(metric.id)];
        if (color) {
            const std::string rgba = overlayRgbaHex(*color);
            text += rgba.substr(7, 2) + rgba.substr(1, 6);
        } else {
            text += "FFFFFFFF";
        }
        text += "\r\n";
    }
    return text;
}

void copyOverlayPlacement(OverlaySettings& target, const OverlaySettings& source)
{
    target.position = source.position;
    target.anchorMode = source.anchorMode;
    target.originX = source.originX;
    target.originY = source.originY;
    target.savedMonitorId = source.savedMonitorId;
    target.monitorOriginX = source.monitorOriginX;
    target.monitorOriginY = source.monitorOriginY;
    target.followGameMonitor = source.followGameMonitor;
    target.offsetX = source.offsetX;
    target.offsetY = source.offsetY;
    target.clickThrough = source.clickThrough;
}

OverlaySettings overlayForGame(const OverlaySettings& base, const OverlaySettings& profile)
{
    OverlaySettings result = profile;
    copyOverlayPlacement(result, base);
    return result;
}

std::string gameProfileKey(std::wstring_view processName)
{
    if (processName.size() < 5 || processName.size() > 80) {
        return { };
    }
    std::string key;
    constexpr char hex[] = "0123456789ABCDEF";
    for (wchar_t letter : processName) {
        if (letter > 127) {
            key.push_back('%');
            const auto code = static_cast<std::uint32_t>(letter);
            for (int shift = 20; shift >= 0; shift -= 4) {
                key.push_back(hex[(code >> shift) & 0xFu]);
            }
        } else if ((letter >= L'a' && letter <= L'z') || (letter >= L'A' && letter <= L'Z')
            || (letter >= L'0' && letter <= L'9') || letter == L'_' || letter == L'-' || letter == L'.'
            || letter == L' ') {
            key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(letter))));
        } else {
            return { };
        }
        if (key.size() > 120) {
            return { };
        }
    }
    return key.ends_with(".exe") ? key : std::string { };
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
    text += std::string("anchor_mode = ")
        + (settings.overlay.anchorMode == OverlayAnchorMode::Free ? "free\n" : "corner\n");
    text += "origin_x = " + std::to_string(settings.overlay.originX) + "\n";
    text += "origin_y = " + std::to_string(settings.overlay.originY) + "\n";
    if (!settings.overlay.savedMonitorId.empty() && settings.overlay.savedMonitorId.size() <= 256
        && settings.overlay.savedMonitorId.find_first_of("\r\n") == std::string::npos) {
        text += "saved_monitor_id = " + settings.overlay.savedMonitorId + "\n";
        text += "monitor_origin_x = " + std::to_string(settings.overlay.monitorOriginX) + "\n";
        text += "monitor_origin_y = " + std::to_string(settings.overlay.monitorOriginY) + "\n";
    }
    text += std::string("follow_game_monitor = ") + (settings.overlay.followGameMonitor ? "1\n" : "0\n");
    text += std::string("fps_sparkline = ") + (settings.overlay.showSparkline ? "1\n" : "0\n");
    const MetricId sparkline = settings.overlay.sparklineMetric;
    const std::string_view sparklineKey
        = sparkline == MetricId::FrameTime || sparkline == MetricId::CpuLoad || sparkline == MetricId::GpuLoad
        ? metricInfo(sparkline).key
        : "fps";
    text += "sparkline_metric = " + std::string(sparklineKey) + "\n";
    text += "recipe = " + std::string(overlayRecipeKey(settings.overlay.recipe)) + "\n";
    text += "layout = " + std::string(overlayLayoutKey(settings.overlay.layout)) + "\n";
    text += "style = " + std::string(overlayStyleKey(settings.overlay.style)) + "\n";
    if (settings.overlay.surfaceActive) {
        text += "surface = " + std::string(overlaySurfaceKey(settings.overlay.surface)) + "\n";
    } else if (static_cast<std::size_t>(settings.overlay.recipe) >= 8) {
        text += "surface = custom\n";
    }
    text += "scale = " + std::to_string(settings.overlay.scalePercent) + "\n";
    text += "opacity = " + std::to_string(settings.overlay.opacityPercent) + "\n";
    text += "menu_opacity = " + std::to_string(settings.overlay.menuOpacityPercent) + "\n";
    text += "offset_x = " + std::to_string(settings.overlay.offsetX) + "\n";
    text += "offset_y = " + std::to_string(settings.overlay.offsetY) + "\n";
    text += std::string("pixel_layout = ") + (settings.overlay.pixelLayout ? "1" : "0") + "\n";
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
    text += "background_width = " + std::to_string(settings.overlay.backgroundWidthPx) + "\n";
    text += "background_height = " + std::to_string(settings.overlay.backgroundHeightPx) + "\n";
    text += "value_color = " + std::to_string(settings.overlay.valueColor) + "\n";
    text += "label_color = " + std::to_string(settings.overlay.labelColor) + "\n";
    text += "accent_color = " + std::to_string(settings.overlay.accentColor) + "\n";
    if (settings.overlay.valueRgba)
        text += "value_rgba = " + overlayRgbaHex(*settings.overlay.valueRgba) + "\n";
    if (settings.overlay.labelRgba)
        text += "label_rgba = " + overlayRgbaHex(*settings.overlay.labelRgba) + "\n";
    if (settings.overlay.accentRgba)
        text += "accent_rgba = " + overlayRgbaHex(*settings.overlay.accentRgba) + "\n";
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
        if (settings.overlay.rowRgba[index]) {
            text += "row_rgba_" + std::string(labelKey(field)) + " = "
                + overlayRgbaHex(*settings.overlay.rowRgba[index]) + "\n";
        }
    }
    text += std::string("click_through = ") + (settings.overlay.clickThrough ? "1" : "0") + "\n";
    for (std::size_t index = 0; index < kLabelCount; ++index) {
        const auto field = static_cast<LabelField>(index);
        text += std::string("show_") + std::string(labelKey(field)) + " = "
            + (settings.overlay.metrics[static_cast<std::size_t>(kOverlayPrimaryMetrics[index])] ? "1" : "0")
            + "\n";
    }
    text += "\n";

    text += "[overlay_metrics]\n";
    for (const MetricInfo& metric : kMetricTable) {
        text += std::string(metric.key) + " = "
            + (settings.overlay.metrics[static_cast<std::size_t>(metric.id)] ? "1" : "0") + "\n";
    }
    text += "\n[overlay_order]\nrows = ";
    for (std::size_t i = 0; i < kMetricCount; ++i) {
        if (i != 0) {
            text += ",";
        }
        text += metricInfo(settings.overlay.order[i]).key;
    }
    text += "\n[overlay_positions]\n";
    for (const MetricInfo& metric : kMetricTable) {
        const auto& pos = settings.overlay.rowPositions[static_cast<std::size_t>(metric.id)];
        if (pos.y >= 0) {
            text += std::string(metric.key) + " = " + std::to_string(pos.labelX) + ","
                + std::to_string(pos.valueX) + "," + std::to_string(pos.y) + "\n";
        }
    }
    text += "\n[overlay_style_offsets]\n";
    for (const MetricInfo& metric : kMetricTable) {
        const auto& offset = settings.overlay.styleRowOffsets[static_cast<std::size_t>(metric.id)];
        if (offset.x != 0 || offset.y != 0) {
            text += std::string(metric.key) + " = " + std::to_string(offset.x) + ","
                + std::to_string(offset.y) + "\n";
        }
    }
    text += "\n[overlay_metric_text_scale]\n";
    for (const MetricInfo& metric : kMetricTable) {
        const int size = settings.overlay.metricTextScalePercent[static_cast<std::size_t>(metric.id)];
        if (size != 100)
            text += std::string(metric.key) + " = " + std::to_string(size) + "\n";
    }
    text += "\n[overlay_metric_colors]\n";
    for (const MetricInfo& metric : kMetricTable) {
        const int color = settings.overlay.metricColors[static_cast<std::size_t>(metric.id)];
        if (color >= 0) {
            text += std::string(metric.key) + " = " + std::to_string(color) + "\n";
        }
    }
    text += "\n[overlay_metric_rgba]\n";
    for (const MetricInfo& metric : kMetricTable) {
        const auto& rgba = settings.overlay.metricRgba[static_cast<std::size_t>(metric.id)];
        if (rgba)
            text += std::string(metric.key) + " = " + overlayRgbaHex(*rgba) + "\n";
    }
    text += "\n[overlay_labels]\n";
    for (const MetricInfo& metric : kMetricTable) {
        const std::size_t index = static_cast<std::size_t>(metric.id);
        if (settings.overlay.customLabelSet[index]) {
            text += std::string(metric.key) + " = " + settings.overlay.customLabels[index] + "\n";
        }
    }
    text += "\n[labels]\n";
    for (std::size_t index = 0; index < kLabelCount; ++index) {
        const auto field = static_cast<LabelField>(index);
        text += std::string(labelKey(field)) + " = " + labelValue(settings.overlay, field) + "\n";
    }
    text += "\n";
    if (settings.overlay.rtssTemplate && settings.overlay.rtssTemplate->size() <= kMaxRtssBytes) {
        text += "[rtss_original]\n";
        text += "template_hex = " + hexEncode(*settings.overlay.rtssTemplate) + "\n";
        text += "omitted_layers = " + std::to_string(settings.overlay.rtssOmittedLayers) + "\n";
        if (!settings.overlay.rtssExchange && settings.overlay.rtssVisual
            && settings.overlay.rtssVisual->imageBytes
            && settings.overlay.rtssVisual->imageBytes->size() <= 4 * 1024 * 1024) {
            text += "image_hex = " + hexEncode(*settings.overlay.rtssVisual->imageBytes) + "\n";
        }
        if (settings.overlay.rtssExchange && settings.overlay.rtssExchange->size() <= kMaxRtssExchangeBytes) {
            text += "exchange_hex = " + hexEncode(*settings.overlay.rtssExchange) + "\n";
            text += "resources = " + std::to_string(settings.overlay.rtssResources) + "\n";
        }
        text += "\n";
    }

    bool hasRtssEdits = false;
    for (const RtssLayerEdit& edit : settings.overlay.rtssLayerEdits) {
        hasRtssEdits |= edit.x != 0 || edit.y != 0 || !edit.visible;
    }
    if (hasRtssEdits) {
        text += "[rtss_layer_edits]\n";
        for (std::size_t index = 0; index < settings.overlay.rtssLayerEdits.size(); ++index) {
            const RtssLayerEdit& edit = settings.overlay.rtssLayerEdits[index];
            if (edit.x != 0 || edit.y != 0 || !edit.visible) {
                text += std::to_string(index) + " = " + std::to_string(edit.x) + ","
                    + std::to_string(edit.y) + "," + (edit.visible ? "1" : "0") + "\n";
            }
        }
        text += "\n";
    }
    for (std::string_view section :
        { "rtss_layer_text", "rtss_layer_color", "rtss_layer_size", "rtss_layer_extent" }) {
        bool header = false;
        for (std::size_t index = 0; index < settings.overlay.rtssLayerEdits.size(); ++index) {
            const auto& edit = settings.overlay.rtssLayerEdits[index];
            std::string value;
            bool hasValue = false;
            if (section == "rtss_layer_text" && edit.text && edit.text->size() <= 8192) {
                value = hexEncode(*edit.text);
                hasValue = true;
            } else if (section == "rtss_layer_color" && edit.textColor) {
                constexpr char digits[] = "0123456789ABCDEF";
                for (int shift = 28; shift >= 0; shift -= 4) {
                    value += digits[(*edit.textColor >> shift) & 15];
                }
                hasValue = true;
            } else if (section == "rtss_layer_size" && edit.size >= 10 && edit.size <= 400) {
                value = std::to_string(edit.size);
                hasValue = true;
            } else if (section == "rtss_layer_extent" && (edit.width > 0 || edit.height > 0)) {
                value = std::to_string(edit.width) + "," + std::to_string(edit.height);
                hasValue = true;
            }
            if (!hasValue) {
                continue;
            }
            if (!header) {
                text += "[" + std::string(section) + "]\n";
                header = true;
            }
            text += std::to_string(index) + " = " + value + "\n";
        }
        if (header) {
            text += "\n";
        }
    }
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
